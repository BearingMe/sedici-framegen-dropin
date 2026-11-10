//! Asynchronous settings file I/O (ports `settings/settings_io.cpp`).
//!
//! At most one job and one unconsumed completion exist. Start functions perform no file I/O
//! themselves; a worker owns the job and the host owns the thread. A false return means no job was
//! started. The UI must poll before starting another job.
//!
//! K83: a worker blocked in the filesystem cannot be cancelled safely, but it must not leave the
//! menu waiting forever. After [`SettingsIo::set_stall_ms`] the poll reports the job as stalled
//! (once), asks the host to cancel its synchronous I/O, and discards the late result. The worker
//! count stays one: no new job starts until the stalled worker exits.
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Instant;

use crate::application::settings::tx::{SaveResult, SaveTicket};

/// The default stall limit in milliseconds.
pub const DEFAULT_STALL_MS: u64 = 5_000;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum IoKind {
    #[default]
    BeginEdit,
    Save,
}

/// What one finished (or stalled) job reports back to the menu.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct IoCompletion {
    pub kind: IoKind,
    pub view_epoch: u64,
    pub worker_thread_id: u32,
    pub begin_ok: bool,
    pub save: SaveResult,
    /// The worker did not finish within the stall limit; `save.message` says so (K83).
    pub stalled: bool,
}

/// The file operations the worker performs. Implemented by the transaction, driven by the worker.
pub trait SettingsIoPort: Send + Sync + 'static {
    fn begin_edit_for(&self, generation: u64) -> bool;
    fn save_for(&self, ticket: SaveTicket) -> SaveResult;
}

/// Thread creation and cancellation, supplied by an adapter so the application owns no OS calls.
pub trait WorkerHost: Send + Sync + 'static {
    /// Run `job` on one worker thread. False when no thread could be started.
    fn spawn(&self, job: Box<dyn FnOnce() + Send>) -> bool;
    /// Current thread id, from inside the job.
    fn thread_id(&self) -> u32;
    /// Best-effort cancellation of the running job's synchronous I/O (K83).
    fn cancel(&self);
}

/// State the worker needs after it finishes, shared with the scheduler.
#[derive(Default)]
struct Shared {
    /// The one unconsumed completion.
    result: Mutex<Option<IoCompletion>>,
    /// A job that failed to build a result; the UI must still be told it ended.
    lost: Mutex<Option<(IoKind, u64)>>,
    /// Serializes publishing against the stall report.
    publish: Mutex<()>,
    busy: AtomicBool,
    stall_reported: AtomicBool,
}

/// The one-job settings I/O scheduler.
pub struct SettingsIo {
    port: Arc<dyn SettingsIoPort>,
    host: Arc<dyn WorkerHost>,
    shared: Arc<Shared>,
    stall_count: AtomicU64,
    stall_ms: AtomicU64,
    job_started: Mutex<Option<Instant>>,
    active_kind: Mutex<IoKind>,
    active_epoch: AtomicU64,
}

impl SettingsIo {
    pub fn new(port: Arc<dyn SettingsIoPort>, host: Arc<dyn WorkerHost>) -> Self {
        Self {
            port,
            host,
            shared: Arc::new(Shared::default()),
            stall_count: AtomicU64::new(0),
            stall_ms: AtomicU64::new(DEFAULT_STALL_MS),
            job_started: Mutex::new(None),
            active_kind: Mutex::new(IoKind::BeginEdit),
            active_epoch: AtomicU64::new(0),
        }
    }

    pub fn start_begin(&self, generation: u64, view_epoch: u64) -> bool {
        if generation == 0 {
            return false;
        }
        self.start(
            IoKind::BeginEdit,
            generation,
            SaveTicket::default(),
            view_epoch,
        )
    }

    pub fn start_save(&self, ticket: SaveTicket, view_epoch: u64) -> bool {
        if !ticket.valid {
            return false;
        }
        self.start(IoKind::Save, 0, ticket, view_epoch)
    }

    fn start(&self, kind: IoKind, generation: u64, ticket: SaveTicket, view_epoch: u64) -> bool {
        if self
            .shared
            .busy
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return false;
        }
        {
            // A completion the caller has not consumed blocks a new job, as does a lost one.
            let publish = lock(&self.shared.publish);
            let blocked = lock(&self.shared.result).is_some() || lock(&self.shared.lost).is_some();
            drop(publish);
            if blocked {
                self.shared.busy.store(false, Ordering::Release);
                return false;
            }
        }

        *lock(&self.job_started) = Some(Instant::now());
        self.shared.stall_reported.store(false, Ordering::Release);
        *lock(&self.active_kind) = kind;
        self.active_epoch.store(view_epoch, Ordering::Relaxed);

        let port = Arc::clone(&self.port);
        let host = Arc::clone(&self.host);
        let shared = Arc::clone(&self.shared);
        let job = Box::new(move || {
            let mut result = IoCompletion {
                kind,
                view_epoch,
                worker_thread_id: host.thread_id(),
                ..IoCompletion::default()
            };
            let completed = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match kind {
                IoKind::BeginEdit => {
                    result.begin_ok = port.begin_edit_for(generation);
                    if result.begin_ok {
                        tracing::info!("sedici: settings edit baseline ready on worker");
                    }
                }
                IoKind::Save => result.save = port.save_for(ticket),
            }));
            // The publish lock is the same one the stall report takes: a job reported as stalled
            // has had its completion delivered, and its late result must not reach a menu that has
            // moved on.
            let publish = lock(&shared.publish);
            if completed.is_err() {
                *lock(&shared.lost) = Some((kind, view_epoch));
            } else if !shared.stall_reported.load(Ordering::Acquire) {
                *lock(&shared.result) = Some(result);
            }
            drop(publish);
            shared.busy.store(false, Ordering::Release);
        });

        if !self.host.spawn(job) {
            self.shared.busy.store(false, Ordering::Release);
            return false;
        }
        true
    }

    /// Takes the one unconsumed completion, or reports a stall.
    pub fn poll(&self) -> Option<IoCompletion> {
        let publish = lock(&self.shared.publish);
        // A completion that is ready is never reported as a stall, even if the worker has not yet
        // cleared its busy flag: the stall limit bounds the wait, not a finished job's bookkeeping.
        let ready = lock(&self.shared.result).is_some();
        if !ready && self.should_report_stall() {
            self.shared.stall_reported.store(true, Ordering::Release);
            self.stall_count.fetch_add(1, Ordering::Relaxed);
            drop(publish);
            self.host.cancel();
            tracing::error!(
                "sedici: settings file I/O has not completed; the request is reported as \
                 unconfirmed and no new settings job starts until the worker exits"
            );
            return Some(IoCompletion {
                kind: *lock(&self.active_kind),
                view_epoch: self.active_epoch.load(Ordering::Relaxed),
                stalled: true,
                save: SaveResult {
                    message: "Settings file is not responding; the change was not confirmed"
                        .to_owned(),
                    ..SaveResult::default()
                },
                ..IoCompletion::default()
            });
        }
        if let Some(result) = lock(&self.shared.result).take() {
            return Some(result);
        }
        let lost = lock(&self.shared.lost).take();
        drop(publish);
        lost.map(|(kind, view_epoch)| IoCompletion {
            kind,
            view_epoch,
            ..IoCompletion::default()
        })
    }

    fn should_report_stall(&self) -> bool {
        if !self.shared.busy.load(Ordering::Acquire)
            || self.shared.stall_reported.load(Ordering::Acquire)
            || lock(&self.shared.result).is_some()
        {
            return false;
        }
        let started = *lock(&self.job_started);
        started.is_some_and(|started| {
            started.elapsed().as_millis() as u64 >= self.stall_ms.load(Ordering::Relaxed)
        })
    }

    pub fn busy(&self) -> bool {
        self.shared.busy.load(Ordering::Acquire)
    }

    /// A reported-stalled worker is still running.
    pub fn stalled(&self) -> bool {
        self.busy() && self.shared.stall_reported.load(Ordering::Acquire)
    }

    /// Jobs reported stalled so far.
    pub fn stall_count(&self) -> u64 {
        self.stall_count.load(Ordering::Relaxed)
    }

    /// Offline test seam: shorten the stall limit.
    pub fn set_stall_ms(&self, ms: u64) {
        self.stall_ms.store(ms, Ordering::Relaxed);
    }

    pub fn stall_ms(&self) -> u64 {
        self.stall_ms.load(Ordering::Relaxed)
    }
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// Test helpers: a port that blocks until released, and a host that runs jobs on real threads.
#[cfg(test)]
pub(crate) mod test_support {
    use super::*;
    use std::sync::Condvar;

    pub struct FakePort {
        gate: Arc<(Mutex<bool>, Condvar)>,
        begin_ok: AtomicBool,
    }

    impl FakePort {
        pub fn new() -> Arc<Self> {
            Arc::new(Self {
                gate: Arc::new((Mutex::new(false), Condvar::new())),
                begin_ok: AtomicBool::new(true),
            })
        }
        pub fn release(&self) {
            *lock(&self.gate.0) = true;
            self.gate.1.notify_all();
        }
        fn wait(&self) {
            let mut open = lock(&self.gate.0);
            while !*open {
                open = self.gate.1.wait(open).unwrap_or_else(|p| p.into_inner());
            }
        }
    }

    impl SettingsIoPort for FakePort {
        fn begin_edit_for(&self, _generation: u64) -> bool {
            self.wait();
            self.begin_ok.load(Ordering::Relaxed)
        }
        fn save_for(&self, _ticket: SaveTicket) -> SaveResult {
            self.wait();
            SaveResult {
                ok: true,
                conflict: false,
                message: "Saved".to_owned(),
            }
        }
    }

    pub struct ThreadHost {
        next: AtomicU64,
    }

    impl ThreadHost {
        pub fn new() -> Arc<Self> {
            Arc::new(Self {
                next: AtomicU64::new(1000),
            })
        }
    }

    impl WorkerHost for ThreadHost {
        fn spawn(&self, job: Box<dyn FnOnce() + Send>) -> bool {
            std::thread::spawn(job);
            true
        }
        fn thread_id(&self) -> u32 {
            self.next.fetch_add(1, Ordering::Relaxed) as u32
        }
        fn cancel(&self) {}
    }
}

#[cfg(test)]
mod tests {
    use super::test_support::{FakePort, ThreadHost};
    use super::*;
    use std::time::Duration;

    fn wait_for(predicate: impl FnMut() -> bool, timeout: Duration) -> bool {
        let mut predicate = predicate;
        let start = Instant::now();
        while start.elapsed() < timeout {
            if predicate() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        predicate()
    }

    #[test]
    fn a_job_runs_on_another_thread_and_reports_its_epoch() {
        let port = FakePort::new();
        port.release();
        let io = SettingsIo::new(port, ThreadHost::new());
        assert!(io.start_begin(1, 102));
        assert!(io.busy());
        let mut completion = None;
        assert!(wait_for(
            || {
                completion = io.poll();
                completion.is_some()
            },
            Duration::from_secs(5)
        ));
        let completion = completion.expect("completion");
        assert_eq!(completion.kind, IoKind::BeginEdit);
        assert_eq!(completion.view_epoch, 102);
        assert!(completion.begin_ok);
        assert_ne!(completion.worker_thread_id, 0);
        assert!(!io.busy());
    }

    #[test]
    fn only_one_job_runs_and_a_pending_completion_blocks_the_next() {
        let port = FakePort::new();
        port.release();
        let io = SettingsIo::new(port, ThreadHost::new());
        assert!(io.start_begin(1, 1));
        assert!(wait_for(|| !io.busy(), Duration::from_secs(5)));
        // The completion is still unconsumed, so a new job is refused.
        assert!(!io.start_begin(2, 2));
        assert!(io.poll().is_some());
        assert!(io.start_begin(3, 3));
    }

    #[test]
    fn a_zero_generation_or_invalid_ticket_starts_nothing() {
        let port = FakePort::new();
        let io = SettingsIo::new(port, ThreadHost::new());
        assert!(!io.start_begin(0, 1));
        assert!(!io.start_save(SaveTicket::default(), 1));
        assert!(!io.busy());
    }

    #[test]
    fn a_stalled_job_is_reported_once_with_its_epoch_and_no_second_worker_starts() {
        let port = FakePort::new(); // never released until the end
        let io = SettingsIo::new(Arc::clone(&port) as _, ThreadHost::new());
        io.set_stall_ms(100);
        assert!(io.start_begin(1, 41));
        assert!(io.busy());

        let start = Instant::now();
        let mut reported = None;
        while reported.is_none() && start.elapsed() < Duration::from_secs(3) {
            reported = io.poll();
            if reported.is_none() {
                std::thread::sleep(Duration::from_millis(5));
            }
        }
        let reported = reported.expect("stalled completion");
        assert!(reported.stalled);
        assert_eq!(reported.view_epoch, 41);
        assert_eq!(reported.kind, IoKind::BeginEdit);
        assert!(!reported.save.message.is_empty());
        assert!(
            start.elapsed() >= Duration::from_millis(90),
            "reported before the limit"
        );
        assert!(io.busy() && io.stalled());
        assert!(!io.start_begin(2, 42));
        assert!(io.poll().is_none(), "the stall is reported once");
        assert_eq!(io.stall_count(), 1);

        // The filesystem recovers: the worker ends, the late result is discarded.
        port.release();
        assert!(wait_for(|| !io.busy(), Duration::from_secs(5)));
        assert!(!io.stalled());
        assert!(io.poll().is_none());
        assert!(io.start_begin(3, 43));
        assert!(wait_for(|| io.poll().is_some(), Duration::from_secs(5)));
    }
}
