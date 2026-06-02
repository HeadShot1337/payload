// veh_demo.rs
// Educational example demonstrating Vectored Exception Handling (VEH)
// and Thread Context access in Rust using the `windows` crate.

use windows::Win32::System::Diagnostics::Debug::{
    AddVectoredExceptionHandler, CONTEXT, EXCEPTION_POINTERS,
};
use windows::Win32::Foundation::{EXCEPTION_CONTINUE_SEARCH, STATUS_SINGLE_STEP};
use std::ffi::c_void;

/// A simple Vectored Exception Handler.
/// This function is called by the Windows kernel whenever an exception occurs
/// in the process, before standard Structured Exception Handling (SEH) kicks in.
unsafe extern "system" fn educational_veh_handler(exception_info: *mut EXCEPTION_POINTERS) -> i32 {
    // Safety: exception_info is guaranteed to be valid when called by the kernel.
    let record = unsafe { &*(*exception_info).ExceptionRecord };
    let context = unsafe { &mut *(*exception_info).ContextRecord };

    // Check if the exception was a Hardware Breakpoint (Single Step)
    if record.ExceptionCode == STATUS_SINGLE_STEP {
        println!("[!] Hardware breakpoint triggered at: {:?}", record.ExceptionAddress);

        // In a security research context, the 'context' structure allows
        // inspection and modification of CPU registers (Rip, Rax, Rsp, etc.)
        // for the thread that triggered the exception.

        // Example: Accessing the instruction pointer (x64)
        // println!("Current RIP: 0x{:x}", context.Rip);
    }

    // Returning EXCEPTION_CONTINUE_SEARCH tells Windows to continue
    // looking for other handlers (like SEH blocks).
    EXCEPTION_CONTINUE_SEARCH.0
}

fn main() {
    println!("--- VEH and Context Access Demo ---");

    unsafe {
        // AddVectoredExceptionHandler:
        // First parameter: 1 means the handler is called first in the chain.
        // Second parameter: Pointer to our handler function.
        let handler = AddVectoredExceptionHandler(1, Some(educational_veh_handler));

        if handler.is_null() {
            eprintln!("[-] Failed to register VEH handler.");
            return;
        }

        println!("[+] VEH handler registered successfully at {:p}", handler);

        // At this point, if we were to set hardware breakpoints (DR0-DR3)
        // via SetThreadContext, any execution at those addresses would
        // trigger our `educational_veh_handler`.

        println!("[*] Handler is active. (Press Enter to exit)");
    }

    let mut input = String::new();
    let _ = std::io::stdin().read_line(&mut input);
}
