use std::ffi::c_void;
use crate::utils::hash::dbj2_hash;

#[repr(C)]
pub struct UNICODE_STRING {
    pub Length: u16,
    pub MaximumLength: u16,
    pub Buffer: *mut u16,
}

#[repr(C)]
pub struct LDR_DATA_TABLE_ENTRY {
    pub InLoadOrderLinks: [usize; 2],
    pub InMemoryOrderLinks: [usize; 2],
    pub InInitializationOrderLinks: [usize; 2],
    pub DllBase: *mut c_void,
    pub EntryPoint: *mut c_void,
    pub SizeOfImage: u32,
    pub FullDllName: UNICODE_STRING,
    pub BaseDllName: UNICODE_STRING,
}

#[repr(C)]
pub struct PEB_LDR_DATA {
    pub Length: u32,
    pub Initialized: u8,
    pub SsHandle: *mut c_void,
    pub InLoadOrderModuleList: [usize; 2],
}

#[repr(C)]
pub struct PEB {
    pub Reserved1: [u8; 2],
    pub BeingDebugged: u8,
    pub Reserved2: [u8; 21],
    pub Ldr: *mut PEB_LDR_DATA,
}

pub unsafe fn get_module_base_by_hash(module_hash: u32) -> Option<*mut c_void> {
    let peb: *mut PEB;
    std::arch::asm!("mov {}, gs:[0x60]", out(reg) peb);

    let ldr = (*peb).Ldr;
    let head = &(*ldr).InLoadOrderModuleList as *const [usize; 2] as *const usize;
    let mut current = (*head) as *const LDR_DATA_TABLE_ENTRY;

    while (current as usize) != (head as usize) {
        if !(*current).BaseDllName.Buffer.is_null() {
            let name_slice = std::slice::from_raw_parts(
                (*current).BaseDllName.Buffer,
                ((*current).BaseDllName.Length / 2) as usize
            );
            let name = String::from_utf16_lossy(name_slice);
            if dbj2_hash(&name.to_lowercase()) == module_hash {
                return Some((*current).DllBase);
            }
        }
        current = (*current).InLoadOrderLinks[0] as *const LDR_DATA_TABLE_ENTRY;
    }
    None
}

#[repr(C)]
struct IMAGE_EXPORT_DIRECTORY {
    Characteristics: u32,
    TimeDateStamp: u32,
    MajorVersion: u16,
    MinorVersion: u16,
    Name: u32,
    Base: u32,
    NumberOfFunctions: u32,
    NumberOfNames: u32,
    AddressOfFunctions: u32,
    AddressOfNames: u32,
    AddressOfNameOrdinals: u32,
}

pub unsafe fn get_export_address_by_hash(module_base: *mut c_void, func_hash: u32) -> Option<*mut c_void> {
    let base = module_base as usize;
    let nt_headers = base + *((base + 0x3c) as *const u32) as usize;
    let export_dir_rva = *((nt_headers + 0x88) as *const u32) as usize;
    if export_dir_rva == 0 { return None; }

    let export_dir = (base + export_dir_rva) as *const IMAGE_EXPORT_DIRECTORY;
    let names_rva = (*export_dir).AddressOfNames as usize;
    let ordinals_rva = (*export_dir).AddressOfNameOrdinals as usize;
    let functions_rva = (*export_dir).AddressOfFunctions as usize;

    for i in 0..(*export_dir).NumberOfNames {
        let name_rva = *((base + names_rva + (i as usize * 4)) as *const u32) as usize;
        let name_ptr = (base + name_rva) as *const i8;
        let name = std::ffi::CStr::from_ptr(name_ptr).to_str().ok()?;

        if dbj2_hash(name) == func_hash {
            let ordinal = *((base + ordinals_rva + (i as usize * 2)) as *const u16) as usize;
            let func_rva = *((base + functions_rva + (ordinal * 4)) as *const u32) as usize;
            return Some((base + func_rva) as *mut c_void);
        }
    }
    None
}
