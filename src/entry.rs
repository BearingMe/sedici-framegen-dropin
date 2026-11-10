//! Composition root: build the adapters, load the configuration once and install the router.
//!
//! The C++ loaded `sedici.toml` on the first FFX call, never in `DllMain`: the loader lock is held
//! during `DLL_PROCESS_ATTACH`, and anything that allocates, waits or loads a library there can
//! deadlock. The Rust port keeps that rule and has **no `DllMain` of its own**: a Rust `cdylib`
//! already links the CRT's entry point, and a second `DllMain` would add a sixth export (the DLL
//! must export exactly five).
//!
//! * [`ensure_started`] runs from the first `ffx*` call. It installs the log (once), loads the
//!   configuration (once) and builds the router, then installs it with
//!   [`crate::application::ffx_model::install`].
//! * It only does that when the calling process loaded this code as its DLL, not when a test
//!   binary linked the rlib. [`crate::adapters::driven::win32::modules::running_as_dll`] answers
//!   that, so `tests/abi.rs` keeps exercising the five exports directly against the scaffold
//!   answers (the same behaviour as before the router existed).
//!
//! Every step is best effort: a configuration error, a failed log file or an already-installed
//! service never fails a game call. A failure to build the router leaves the exports answering the
//! documented error codes exactly as before the composition root ran.
use std::sync::OnceLock;

use crate::application::faults::Faults;
use crate::application::ffx_model;
use crate::domain::config::LoadedSettings;

/// What the composition root built. Kept alive for the process lifetime.
pub struct Runtime {
    pub faults: std::sync::Arc<Faults>,
    pub settings: LoadedSettings,
}

static RUNTIME: OnceLock<Runtime> = OnceLock::new();
static STARTED: OnceLock<()> = OnceLock::new();

/// Runs the composition root on the first FFX call, once, when the process loaded this code as a
/// DLL. Never panics and never blocks: a failed step is logged and the game keeps running.
pub fn ensure_started() {
    // The deploy tools recognise the image by this byte string. Reading it here keeps the
    // retained static in the linked DLL under LTO; it has no other effect.
    let _ = std::hint::black_box(&crate::SEDICI_PROXY_MARKER);
    if !running_as_dll() {
        return;
    }
    STARTED.get_or_init(|| {
        // `catch_unwind`: a panic while building the adapters must not unwind into the game.
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(start_up));
    });
}

/// The configuration the DLL resolved, if the composition root has run.
pub fn settings() -> Option<&'static LoadedSettings> {
    RUNTIME.get().map(|runtime| &runtime.settings)
}

/// The fault injector the offline host uses to arm a bookkeeping failure.
pub fn faults() -> Option<std::sync::Arc<Faults>> {
    RUNTIME
        .get()
        .map(|runtime| std::sync::Arc::clone(&runtime.faults))
}

/// Whether the router has been installed.
pub fn is_installed() -> bool {
    RUNTIME.get().is_some()
}

/// Whether this code is running from the loaded DLL rather than a test binary.
#[cfg(windows)]
fn running_as_dll() -> bool {
    crate::adapters::driven::win32::modules::running_as_dll()
}

#[cfg(not(windows))]
fn running_as_dll() -> bool {
    true
}

fn start_up() {
    use crate::adapters::driven::config_toml::TomlConfigStore;
    use crate::adapters::driven::logging;

    let faults = std::sync::Arc::new(Faults::new());

    // The log is installed against the DLL's own directory. A failure to open the file never
    // stops the game: the logging adapter reports it and writes nothing.
    if let Some(directory) = module_directory() {
        let handle = logging::init(&directory, None);
        // Keep the handle alive for the process lifetime: dropping it would stop the writer.
        std::mem::forget(handle);
    }

    // Load the configuration once, on this first call, and log the report. A malformed file falls
    // back to defaults and says so (never an error to the game).
    let store = TomlConfigStore::beside_module();
    let loaded =
        crate::adapters::driven::config_toml::load_and_log(&store, |name| std::env::var(name).ok());

    // Build and install the router. The adapter that owns the backend device supplies the three
    // seams; when none is linked (the offline host installs its own router), the exports keep
    // answering their documented error codes.
    install_router(&faults);

    let _ = RUNTIME.set(Runtime {
        faults,
        settings: loaded,
    });
}

/// Installs the process router when the D3D12 adapter is present.
fn install_router(faults: &std::sync::Arc<Faults>) {
    #[cfg(windows)]
    {
        if let Some(service) =
            crate::adapters::driven::backend::build_service(std::sync::Arc::clone(faults))
        {
            if ffx_model::install(service).is_err() {
                tracing::debug!("fg: the FFX service was already installed; keeping the first");
            }
        }
    }
    #[cfg(not(windows))]
    {
        let _ = faults;
    }
}

/// The DLL's own directory, used for `sedici.log` and `sedici.toml`.
fn module_directory() -> Option<std::path::PathBuf> {
    crate::adapters::driven::config_toml::module_directory()
}
