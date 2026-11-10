//! The settings I/O worker host (ports the Win32 thread of `settings/settings_io.cpp`).
//!
//! The application only knows [`WorkerHost`]; everything Windows-shaped lives here. Threads are
//! plain `std::thread`s. The C++ also pinned the module with `GetModuleHandleEx` and released it
//! with `FreeLibraryAndExitThread` so the DLL could not be unloaded while a worker was running code
//! in it. A Rust job keeps its code and data alive through the `Arc`s it captures, and Windows does
//! not unload a DLL that has a running thread started from it, so no module reference is taken; the
//! scheduler requires the worker to exit before starting another one.
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use crate::application::settings::io::WorkerHost;

/// Runs settings jobs on real threads and can cancel their synchronous I/O.
#[derive(Default)]
pub struct NativeWorkerHost {
    /// The most recent worker's thread id, recorded from inside the job.
    last_thread: AtomicU32,
    /// Cancellation targets: the thread ids of jobs the scheduler reported as stalled.
    cancelled: Mutex<Vec<u32>>,
}

impl NativeWorkerHost {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn last_thread_id(&self) -> u32 {
        self.last_thread.load(Ordering::Relaxed)
    }

    pub fn cancelled_threads(&self) -> Vec<u32> {
        self.cancelled
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }
}

impl WorkerHost for NativeWorkerHost {
    fn spawn(&self, job: Box<dyn FnOnce() + Send>) -> bool {
        std::thread::Builder::new()
            .name("sedici-settings-io".to_owned())
            .spawn(job)
            .is_ok()
    }

    fn thread_id(&self) -> u32 {
        let id = current_thread_id();
        self.last_thread.store(id, Ordering::Relaxed);
        id
    }

    fn cancel(&self) {
        // K83: best effort. A synchronous call on a local volume is unblocked; a hung network
        // share may ignore it, which is why the report does not wait for the worker.
        let id = self.last_thread.load(Ordering::Relaxed);
        if id == 0 {
            return;
        }
        self.cancelled
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .push(id);
        cancel_thread_io(id);
    }
}

#[cfg(windows)]
fn current_thread_id() -> u32 {
    use windows::Win32::System::Threading::GetCurrentThreadId;
    // SAFETY: `GetCurrentThreadId` has no arguments, no state and cannot fail.
    unsafe { GetCurrentThreadId() }
}

#[cfg(not(windows))]
fn current_thread_id() -> u32 {
    0
}

#[cfg(windows)]
fn cancel_thread_io(thread_id: u32) {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::Threading::{OpenThread, THREAD_TERMINATE};
    use windows::Win32::System::IO::CancelSynchronousIo;

    // SAFETY: `OpenThread` returns an owned handle that is closed below; `CancelSynchronousIo`
    // only signals a pending synchronous call and is a no-op when there is none.
    unsafe {
        if let Ok(handle) = OpenThread(THREAD_TERMINATE, false, thread_id) {
            let _ = CancelSynchronousIo(handle);
            let _ = CloseHandle(handle);
        }
    }
}

#[cfg(not(windows))]
fn cancel_thread_io(_thread_id: u32) {}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;
    use std::sync::Arc;

    #[test]
    fn a_job_runs_on_another_thread() {
        let host = Arc::new(NativeWorkerHost::new());
        let (sender, receiver) = mpsc::channel();
        let worker = Arc::clone(&host);
        assert!(host.spawn(Box::new(move || {
            let _ = sender.send(worker.thread_id());
        })));
        let id = receiver
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("id");
        assert_ne!(id, 0);
        assert_ne!(id, current_thread_id());
        assert_eq!(host.last_thread_id(), id);
    }

    #[test]
    fn a_reported_stall_asks_for_cancellation_of_the_worker() {
        let host = NativeWorkerHost::new();
        host.last_thread.store(4242, Ordering::Relaxed);
        host.cancel();
        assert_eq!(host.cancelled_threads(), vec![4242]);
    }

    #[test]
    fn begin_and_save_run_on_the_worker_and_patch_the_file() {
        use crate::application::settings::io::SettingsIo;
        use crate::application::settings::live::Live;
        use crate::application::settings::tx::SettingsTx;
        use crate::domain::config::Settings;

        let dir = std::env::temp_dir().join(format!("sedici-worker-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("sedici.toml");
        std::fs::write(&path, "[fg]\ntarget_fps = 30 # keep\n").unwrap();

        let live = Arc::new(Live::new());
        let store = Arc::new(crate::adapters::driven::config_toml::TomlConfigStore::at(
            &path,
        ));
        let tx = Arc::new(SettingsTx::new(
            Arc::clone(&live),
            Settings::default(),
            store as Arc<dyn crate::application::ports::config_store::ConfigStore + Send + Sync>,
        ));
        let port: Arc<dyn crate::application::settings::io::SettingsIoPort> =
            Arc::clone(&tx) as Arc<dyn crate::application::settings::io::SettingsIoPort>;
        let io = SettingsIo::new(port, Arc::new(NativeWorkerHost::new()));

        let generation = tx.start_edit();
        assert!(io.start_begin(generation, 7));
        let begin = poll_until(&io);
        assert!(begin.begin_ok, "begin failed");
        assert_eq!(begin.view_epoch, 7);
        assert_ne!(begin.worker_thread_id, current_thread_id());

        assert_eq!(
            tx.change_edit(|values| values.target_fps = 44.0).status,
            crate::application::settings::tx::ApplyStatus::Applied
        );
        let ticket = tx.capture_save_ticket();
        assert!(io.start_save(ticket, 7));
        let saved = poll_until(&io);
        assert!(saved.save.ok, "{}", saved.save.message);
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("target_fps = 44"), "{text}");
        assert!(text.contains("# keep"));

        let _ = std::fs::remove_dir_all(&dir);
    }

    fn poll_until(
        io: &crate::application::settings::io::SettingsIo,
    ) -> crate::application::settings::io::IoCompletion {
        use std::time::{Duration, Instant};
        let start = Instant::now();
        while start.elapsed() < Duration::from_secs(5) {
            if let Some(completion) = io.poll() {
                return completion;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        panic!("no completion within the deadline");
    }
}
