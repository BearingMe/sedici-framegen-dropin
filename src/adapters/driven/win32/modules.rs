//! Safe enumeration of the process's loaded modules (ports the module scan in `ui/ui_input.cpp`).
//!
//! The IAT adapter asks for the modules to patch: the executable plus every module that is not
//! part of Windows and not Sedici itself (K132: the game's engine DLL may be the one that imports
//! `SetCursorPos`/`ClipCursor`).

use windows::Win32::Foundation::{HMODULE, MAX_PATH};
use windows::Win32::System::ProcessStatus::K32EnumProcessModules;
use windows::Win32::System::SystemInformation::{GetSystemDirectoryW, GetWindowsDirectoryW};
use windows::Win32::System::Threading::GetCurrentProcess;

/// One loaded module, as an opaque image base.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Module(pub HMODULE);

impl std::hash::Hash for Module {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        (self.0 .0 as isize).hash(state);
    }
}

/// The main executable's module.
fn executable_module() -> Option<Module> {
    // SAFETY: a null module name requests the executable; the result is a borrowed handle with no
    // reference to release.
    unsafe { windows::Win32::System::LibraryLoader::GetModuleHandleW(None) }.ok().map(Module)
}

/// The process's modules in load order. On a failure the executable alone is returned, so a scan
/// still covers the common case (the C++ does the same).
pub fn loaded_modules() -> Vec<Module> {
    let executable = executable_module();
    // SAFETY: `GetCurrentProcess` takes no arguments and returns a pseudo-handle that must not be
    // closed. The buffer is local and its length is passed in bytes.
    let process = unsafe { GetCurrentProcess() };
    let mut buffer: Vec<HMODULE> = vec![HMODULE::default(); 256];
    let mut needed = 0_u32;
    // SAFETY: the pointer and byte length describe `buffer`; `needed` is a local out-parameter.
    let ok = unsafe {
        K32EnumProcessModules(process, buffer.as_mut_ptr(), (buffer.len() * std::mem::size_of::<HMODULE>()) as u32, &mut needed)
            .as_bool()
    };
    if !ok {
        return executable.map(|module| vec![module]).unwrap_or_default();
    }
    let count = needed as usize / std::mem::size_of::<HMODULE>();
    if count > buffer.len() {
        buffer.resize(count + 16, HMODULE::default());
        // SAFETY: the resized buffer is still valid and its new byte length is passed.
        let retried = unsafe {
            K32EnumProcessModules(process, buffer.as_mut_ptr(), (buffer.len() * std::mem::size_of::<HMODULE>()) as u32, &mut needed)
                .as_bool()
        };
        if !retried {
            return executable.map(|module| vec![module]).unwrap_or_default();
        }
    }
    buffer.truncate(needed as usize / std::mem::size_of::<HMODULE>());
    buffer.into_iter().map(Module).collect()
}

/// The main executable's module.
pub fn executable() -> Option<Module> {
    executable_module()
}

/// True when this code is running from a DLL loaded into some other process image, rather than
/// from a test binary that linked the library statically.
///
/// The composition root runs only in the first case: a test binary that calls the five exports
/// directly must keep seeing the scaffold error answers (the same behaviour as before the router
/// existed), and its own entry point is not the DLL.
#[cfg(windows)]
pub fn running_as_dll() -> bool {
    // The main executable module (the game, or a test binary).
    // SAFETY: a null module name asks for the process's main executable module; the result is a
    // borrowed handle that must not be released.
    let Some(main) = (unsafe { windows::Win32::System::LibraryLoader::GetModuleHandleW(None) }).ok() else {
        return false;
    };
    // The module that contains this function: the DLL when loaded as one, the test executable when
    // statically linked. Comparing the two handles is exact: a DLL's own module is never the
    // process's main module.
    let Some(own) = own_module() else { return false };
    own != main
}

/// The module this code was loaded from, or `None` if the loader cannot say.
#[cfg(windows)]
fn own_module() -> Option<HMODULE> {
    use windows::Win32::System::LibraryLoader::{
        GetModuleHandleExW, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    };
    let mut module = HMODULE::default();
    // SAFETY: the address is a function in this module, so the UNCHANGED_REFCOUNT query only reads
    // the loader's module table and never owns a reference we would have to release.
    unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            windows::core::PCWSTR(own_module as *const u16),
            &mut module,
        )
    }
    .ok()?;
    Some(module)
}

#[cfg(not(windows))]
pub fn running_as_dll() -> bool {
    false
}

/// True when `module` lives under the Windows or System directory. An unknown path counts as
/// system: the scan leaves it alone rather than risk patching a protected module.
pub fn is_system_module(module: Module) -> bool {
    let Some(path) = module_path(module) else { return true };
    let path = path.to_ascii_lowercase();
    let under = |root: Option<std::path::PathBuf>| {
        root.map(|root| path.starts_with(root.to_string_lossy().to_ascii_lowercase().as_str())).unwrap_or(false)
    };
    under(system_directory()) || under(windows_directory())
}

fn utf16_to_string(bytes: &[u16]) -> String {
    let end = bytes.iter().position(|&c| c == 0).unwrap_or(bytes.len());
    String::from_utf16_lossy(&bytes[..end])
}

/// The full path of a loaded module, or `None` when Windows cannot report it.
pub fn module_path(module: Module) -> Option<String> {
    let mut buffer = vec![0_u16; MAX_PATH as usize];
    // SAFETY: the buffer and its length are consistent; the returned count is the number of UTF-16
    // units written (excluding the terminator) or the required size on truncation.
    let written =
        unsafe { windows::Win32::System::LibraryLoader::GetModuleFileNameW(Some(module.0), &mut buffer) };
    if written == 0 {
        return None;
    }
    let written = (written as usize).min(buffer.len());
    Some(utf16_to_string(&buffer[..written]))
}

fn directory(getter: unsafe fn(Option<&mut [u16]>) -> u32) -> Option<std::path::PathBuf> {
    let mut buffer = vec![0_u16; MAX_PATH as usize];
    // SAFETY: the buffer and its length are consistent; the getter writes into it.
    let written = unsafe { getter(Some(&mut buffer)) };
    // A return of 0 is failure; a return longer than the buffer is truncation.
    if written == 0 || written as usize >= buffer.len() {
        return None;
    }
    Some(std::path::PathBuf::from(utf16_to_string(&buffer[..written as usize])))
}

pub fn system_directory() -> Option<std::path::PathBuf> {
    directory(GetSystemDirectoryW)
}

pub fn windows_directory() -> Option<std::path::PathBuf> {
    directory(GetWindowsDirectoryW)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enumerates_the_executable_first_and_it_is_not_system() {
        let modules = loaded_modules();
        assert!(!modules.is_empty(), "the process has at least its own module");
        let exe = executable().expect("the test executable is a loaded module");
        assert!(modules.contains(&exe), "the executable appears in the enumeration");
        assert!(!is_system_module(exe), "the test executable is not under the Windows directory");
        let path = module_path(exe).expect("the executable path is readable");
        assert!(path.to_ascii_lowercase().ends_with(".exe"), "module_path returns a full path: {path}");
    }

    #[test]
    fn kernel32_is_a_system_module() {
        // SAFETY: a borrowed module handle for a string name; no reference is taken.
        let kernel32 = unsafe { windows::Win32::System::LibraryLoader::GetModuleHandleW(windows::core::w!("kernel32.dll")) };
        let kernel32 = kernel32.expect("kernel32 is loaded in every process");
        assert!(is_system_module(Module(kernel32)));
    }
}
