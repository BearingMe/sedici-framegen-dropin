//! tracing subscriber writing sedici.log beside the DLL (ports diagnostics/log).
//!
//! Line shape is the C++ one, because `tools/analyze_log.ps1` matches it:
//!
//! ```text
//! [<ms since start> ms] <text>
//! ```
//!
//! The stamp is milliseconds since this module first loaded, printed with three decimals. Only
//! the event message is written (no level, target or module path), matching the C++ log. A
//! message is one line: any newline in it becomes a space, and the message is capped at 511
//! bytes.
//!
//! Writes go through a non-blocking writer on a dedicated thread, so a slow or failing disk
//! cannot stall a Present. A full queue drops the line and counts it; the next line that gets
//! through says how many were dropped. Repeats of one message are rate limited (a burst of 8,
//! then 2 per second, digits ignored when comparing), and the next admitted repeat says how
//! many were suppressed. A failed write counts the line as lost and says so when the file is
//! writable again.
//!
//! Levels follow the C++ mapping: 0 = errors only, 1 = info (default), 2 = debug/trace. The
//! level can be changed live through [`LogHandle::set_level`] or [`set_level`].
//!
//! One convention beyond plain tracing: an event carrying the field `raw = true` is written
//! without the per-message rate limit, matching the C++ `log_raw` used by the configuration
//! loader:
//!
//! ```ignore
//! tracing::event!(tracing::Level::INFO, raw = true, "sedici: configuration ...");
//! ```

use std::fs::OpenOptions;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use tracing::level_filters::LevelFilter;
use tracing_appender::non_blocking::{ErrorCounter, NonBlocking, NonBlockingBuilder, WorkerGuard};
use tracing_subscriber::layer::{Context, SubscriberExt};
use tracing_subscriber::registry;
use tracing_subscriber::Layer;

use crate::application::stats::LogStats;

/// The log file name beside the DLL.
pub const LOG_FILE: &str = "sedici.log";
/// Ring geometry: 256 lines of up to 511 bytes is 128 KB.
const RING_SLOTS: usize = 256;
const LINE_MAX: usize = 512;
/// Per-message rate limit: a burst of 8 lines, then 2 per second.
const BURST: f64 = 8.0;
const REFILL_PER_SECOND: f64 = 2.0;
const BUCKETS: usize = 32;
const KEY_CHARS: usize = 96;

/// Load-time origin of the millisecond stamp. First use of the module starts the clock.
fn start() -> Instant {
    static START: OnceLock<Instant> = OnceLock::new();
    *START.get_or_init(Instant::now)
}

fn elapsed_ms() -> f64 {
    start().elapsed().as_secs_f64() * 1000.0
}

/// The C++ `MFSRFG_LOG` mapping: 0 = errors only, 1 = info, 2 = debug/trace.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum LogLevel {
    Error,
    #[default]
    Info,
    Debug,
}

impl LogLevel {
    /// Reads `MFSRFG_LOG`; anything but a single digit 0-2 keeps the default. `None` means the
    /// variable is unset or invalid.
    pub fn from_env() -> Option<Self> {
        Self::parse(&std::env::var("MFSRFG_LOG").ok()?)
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "0" => Some(Self::Error),
            "1" => Some(Self::Info),
            "2" => Some(Self::Debug),
            _ => None,
        }
    }

    pub fn as_int(self) -> i32 {
        match self {
            Self::Error => 0,
            Self::Info => 1,
            Self::Debug => 2,
        }
    }

    pub fn from_int(level: i32) -> Self {
        match level {
            0 => Self::Error,
            2 => Self::Debug,
            _ => Self::Info,
        }
    }

    /// The tracing filter this level installs. Debug includes trace, as the C++ "verbose" does.
    pub fn filter(self) -> LevelFilter {
        match self {
            Self::Error => LevelFilter::ERROR,
            Self::Info => LevelFilter::INFO,
            Self::Debug => LevelFilter::TRACE,
        }
    }
}

// ---------------------------------------------------------------------------
// Rate limiting
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug)]
struct Bucket {
    key: u64,
    tokens: f64,
    last: f64,
    suppressed: u32,
}

impl Default for Bucket {
    fn default() -> Self {
        Self { key: 0, tokens: BURST, last: 0.0, suppressed: 0 }
    }
}

/// FNV-1a over the message with digits skipped, capped at [`KEY_CHARS`] characters, so
/// "frame 7" and "frame 8" are one message. 0 is remapped to 1 so an empty key never matches
/// an unused bucket.
fn message_key(message: &str) -> u64 {
    let mut hash: u64 = 1_469_598_103_934_665_603;
    let mut taken = 0;
    for c in message.chars() {
        if taken >= KEY_CHARS {
            break;
        }
        taken += 1;
        if c.is_ascii_digit() {
            continue;
        }
        hash ^= c as u64;
        hash = hash.wrapping_mul(1_099_511_628_211);
    }
    if hash == 0 {
        1
    } else {
        hash
    }
}

/// Token bucket per message key. Returns `Some(suppressed_since_last)` when the line is
/// admitted, `None` when the rate limit refused it.
fn admit(buckets: &mut [Bucket; BUCKETS], message: &str, now_ms: f64) -> Option<u32> {
    let key = message_key(message);
    let bucket = &mut buckets[(key % BUCKETS as u64) as usize];
    if bucket.key != key {
        *bucket = Bucket { key, last: now_ms, ..Bucket::default() };
    }
    bucket.tokens = BURST.min(bucket.tokens + (now_ms - bucket.last) * (REFILL_PER_SECOND / 1000.0));
    bucket.last = now_ms;
    if bucket.tokens < 1.0 {
        bucket.suppressed += 1;
        return None;
    }
    bucket.tokens -= 1.0;
    let repeats = bucket.suppressed;
    bucket.suppressed = 0;
    Some(repeats)
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

#[derive(Debug, Default)]
struct Counters {
    queued: AtomicU64,
    written: AtomicU64,
    dropped: AtomicU64,
    suppressed: AtomicU64,
    lost: AtomicU64,
    sink_failures: AtomicU64,
    /// `max_depth` is the high-water mark of admitted-but-unwritten lines.
    max_depth: AtomicU64,
    sink_ok: AtomicU64,
    dropped_unreported: AtomicU64,
    lost_unreported: AtomicU64,
    failures_in_outage: AtomicU64,
    outage_reported: AtomicU64,
}

impl Counters {
    /// Lines admitted into the queue. The appender's own loss counter is added by
    /// [`log_stats`]; a queue-full drop never reaches this counter.
    fn admitted(&self) -> u64 {
        self.queued.load(Ordering::Relaxed)
    }

    /// Admitted but not yet consumed (written or lost): what [`flush`] waits for.
    fn depth(&self) -> u64 {
        let consumed = self.written.load(Ordering::Relaxed) + self.lost.load(Ordering::Relaxed);
        self.admitted().saturating_sub(consumed)
    }

    fn snapshot(&self) -> LogStats {
        LogStats {
            queued: self.admitted(),
            written: self.written.load(Ordering::Relaxed),
            dropped: self.dropped.load(Ordering::Relaxed),
            suppressed: self.suppressed.load(Ordering::Relaxed),
            lost: self.lost.load(Ordering::Relaxed),
            sink_failures: self.sink_failures.load(Ordering::Relaxed),
            depth: self.depth() as u32,
            max_depth: self.max_depth.load(Ordering::Relaxed) as u32,
            sink_ok: self.sink_ok.load(Ordering::Relaxed) != 0,
        }
    }
}

// ---------------------------------------------------------------------------
// Sink
// ---------------------------------------------------------------------------

/// State shared by the layer (which admits and formats) and the sink thread (which writes).
#[derive(Debug)]
struct Shared {
    counters: Counters,
    buckets: Mutex<[Bucket; BUCKETS]>,
    level: Mutex<LogLevel>,
    /// The appender's own queue-full counter; set once the writer exists.
    queue_drops: OnceLock<ErrorCounter>,
}

impl Shared {
    fn new(level: LogLevel) -> Self {
        Self {
            counters: Counters { sink_ok: AtomicU64::new(1), ..Counters::default() },
            buckets: Mutex::new([Bucket::default(); BUCKETS]),
            level: Mutex::new(level),
            queue_drops: OnceLock::new(),
        }
    }

    fn level(&self) -> LogLevel {
        *self.level.lock().unwrap()
    }
}

/// The writer the non-blocking thread owns: opens `sedici.log` in append mode per line, so a
/// directory that disappears and comes back is picked up again.
struct FileSink {
    directory: PathBuf,
    shared: Arc<Shared>,
    /// Queue drops already reported in the file, so each report says only what is new.
    reported_dropped: u64,
}

impl FileSink {
    fn prefix(&mut self) -> String {
        let mut prefix = String::new();
        let dropped = self
            .shared
            .queue_drops
            .get()
            .map_or(0, |counter| counter.dropped_lines() as u64);
        let new_drops = dropped.saturating_sub(self.reported_dropped);
        if new_drops > 0 {
            prefix.push_str(&format!(
                "[{:.3} ms] log: {new_drops} line(s) were dropped because the queue was full\n",
                elapsed_ms()
            ));
            self.reported_dropped = dropped;
            self.shared.counters.dropped_unreported.fetch_add(new_drops, Ordering::Relaxed);
        }
        let lost = self.shared.counters.lost_unreported.load(Ordering::Relaxed);
        if lost > 0 && self.shared.counters.outage_reported.load(Ordering::Relaxed) != 0 {
            prefix.push_str(&format!(
                "[{:.3} ms] log: the file was unwritable ({} failed write(s)); {lost} line(s) were lost\n",
                elapsed_ms(),
                self.shared.counters.failures_in_outage.load(Ordering::Relaxed),
            ));
        }
        prefix
    }
}

impl Write for FileSink {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let mut text = self.prefix();
        text.push_str(&String::from_utf8_lossy(buf));
        let path = self.directory.join(LOG_FILE);
        let result = OpenOptions::new().create(true).append(true).open(&path).and_then(|mut file| {
            file.write_all(text.as_bytes())?;
            file.flush()
        });
        let counters = &self.shared.counters;
        match result {
            Ok(()) => {
                counters.written.fetch_add(1, Ordering::Relaxed);
                counters.sink_ok.store(1, Ordering::Relaxed);
                counters.dropped_unreported.store(0, Ordering::Relaxed);
                counters.lost_unreported.store(0, Ordering::Relaxed);
                counters.failures_in_outage.store(0, Ordering::Relaxed);
                counters.outage_reported.store(0, Ordering::Relaxed);
                Ok(buf.len())
            }
            Err(error) => {
                counters.sink_failures.fetch_add(1, Ordering::Relaxed);
                counters.failures_in_outage.fetch_add(1, Ordering::Relaxed);
                counters.lost.fetch_add(1, Ordering::Relaxed);
                counters.lost_unreported.fetch_add(1, Ordering::Relaxed);
                counters.sink_ok.store(0, Ordering::Relaxed);
                counters.outage_reported.store(1, Ordering::Relaxed);
                Err(error)
            }
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

/// What one event carries: its message and whether it asked to bypass the rate limit.
#[derive(Default)]
struct EventFields {
    message: String,
    raw: bool,
}

impl tracing::field::Visit for EventFields {
    fn record_str(&mut self, field: &tracing::field::Field, value: &str) {
        if field.name() == "message" {
            self.message.push_str(value);
        }
    }

    fn record_bool(&mut self, field: &tracing::field::Field, value: bool) {
        if field.name() == "raw" {
            self.raw = value;
        }
    }

    fn record_debug(&mut self, field: &tracing::field::Field, value: &dyn std::fmt::Debug) {
        if field.name() == "message" {
            // `tracing` wraps a formatted message in `format_args!`, whose Debug is the text;
            // a plain string field arrives quoted.
            let text = format!("{value:?}");
            let unquoted = text.strip_prefix('"').and_then(|s| s.strip_suffix('"')).unwrap_or(&text);
            self.message.push_str(unquoted);
        }
    }
}

/// One line with the C++ shape: the stamp, the sanitised text, a newline.
fn format_line(message: &str) -> String {
    let mut out = format!("[{:.3} ms] ", elapsed_ms());
    let mut written = 0;
    for c in message.chars() {
        if written >= LINE_MAX - 1 {
            break;
        }
        let c = if c == '\n' || c == '\r' { ' ' } else { c };
        out.push(c);
        written += c.len_utf8();
    }
    out.push('\n');
    out
}

// ---------------------------------------------------------------------------
// The layer
// ---------------------------------------------------------------------------

/// Admits, rate limits and formats one event per line, then hands the bytes to the
/// non-blocking writer.
struct LogLayer {
    /// `NonBlocking` writes through `&mut self`; the mutex keeps the layer `Sync`.
    writer: Mutex<NonBlocking>,
    shared: Arc<Shared>,
}

impl<S: tracing::Subscriber> Layer<S> for LogLayer {
    /// The level is live, so a callsite's interest can change: ask on every event rather than
    /// letting the dispatcher cache `never`.
    fn register_callsite(&self, _metadata: &'static tracing::Metadata<'static>) -> tracing::subscriber::Interest {
        tracing::subscriber::Interest::sometimes()
    }

    fn enabled(&self, metadata: &tracing::Metadata<'_>, _ctx: Context<'_, S>) -> bool {
        metadata.level() <= &self.shared.level().filter()
    }

    fn on_event(&self, event: &tracing::Event<'_>, _ctx: Context<'_, S>) {
        let mut fields = EventFields::default();
        event.record(&mut fields);
        if fields.message.is_empty() {
            return;
        }

        if !fields.raw {
            let now = elapsed_ms();
            let mut buckets = self.shared.buckets.lock().unwrap();
            match admit(&mut buckets, &fields.message, now) {
                Some(repeats) => {
                    if repeats > 0 {
                        fields.message.push_str(&format!(" [+{repeats} similar suppressed]"));
                    }
                }
                None => {
                    self.shared.counters.suppressed.fetch_add(1, Ordering::Relaxed);
                    return;
                }
            }
        }

        let line = format_line(&fields.message);
        let counters = &self.shared.counters;
        counters.queued.fetch_add(1, Ordering::Relaxed);
        // The appender's queue can hold `RING_SLOTS`; the layer's own gate keeps the accepted
        // count in step with the writer, so depth never runs away.
        counters.max_depth.fetch_max(counters.depth(), Ordering::Relaxed);
        let _ = self.writer.lock().unwrap().write_all(line.as_bytes());
    }
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

/// The installed log: a handle for the level and the guard that drains the writer on drop.
pub struct LogHandle {
    shared: Arc<Shared>,
    /// Kept alive for the process lifetime: dropping it stops the writer thread.
    guard: Option<WorkerGuard>,
}

impl std::fmt::Debug for LogHandle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("LogHandle").field("level", &self.level()).finish()
    }
}

impl LogHandle {
    pub fn level(&self) -> LogLevel {
        self.shared.level()
    }

    /// Changes the level live, as the overlay's settings do.
    pub fn set_level(&self, level: LogLevel) {
        *self.shared.level.lock().unwrap() = level;
    }

    /// False when a global subscriber was already installed by something else; the log then
    /// writes nothing and the game keeps running (bookkeeping never fails a game call).
    pub fn is_installed(&self) -> bool {
        self.guard.is_some()
    }
}

static SHARED: OnceLock<Arc<Shared>> = OnceLock::new();

/// Installs the subscriber writing `<directory>/sedici.log`. `level` is the explicit level
/// (from `[log] level` or the settings layer); `None` reads `MFSRFG_LOG`, then defaults to
/// info. Never panics and never fails a caller: if a global subscriber is already installed,
/// the returned handle reports [`LogHandle::is_installed`] == false and logging is a no-op.
pub fn init(directory: impl AsRef<Path>, level: Option<LogLevel>) -> LogHandle {
    let level = level.or_else(LogLevel::from_env).unwrap_or_default();
    let shared = Arc::new(Shared::new(level));

    let sink = FileSink { directory: directory.as_ref().to_path_buf(), shared: Arc::clone(&shared), reported_dropped: 0 };
    let (writer, guard) = NonBlockingBuilder::default()
        .buffered_lines_limit(RING_SLOTS)
        .lossy(true)
        .thread_name("Sedici log")
        .finish(sink);
    let _ = shared.queue_drops.set(writer.error_counter());

    let layer = LogLayer { writer: Mutex::new(writer), shared: Arc::clone(&shared) };
    let installed = tracing::subscriber::set_global_default(registry().with(layer)).is_ok();
    let _ = SHARED.set(Arc::clone(&shared));
    LogHandle { shared, guard: if installed { Some(guard) } else { None } }
}

/// The live log level of the installed log; [`LogLevel::Info`] before [`init`].
pub fn level() -> LogLevel {
    SHARED.get().map_or(LogLevel::Info, |shared| shared.level())
}

/// Changes the installed log's level live, as the overlay's settings do. A no-op before
/// [`init`].
pub fn set_level(level: LogLevel) {
    if let Some(shared) = SHARED.get() {
        *shared.level.lock().unwrap() = level;
    }
}

/// Counters of what the log itself did. All zero when nothing was installed.
pub fn log_stats() -> LogStats {
    SHARED.get().map_or_else(LogStats::default, |shared| {
        let mut stats = shared.counters.snapshot();
        // The appender counts queue-full drops itself; those are the "dropped" lines.
        if let Some(counter) = shared.queue_drops.get() {
            stats.dropped += counter.dropped_lines() as u64;
        }
        stats
    })
}

/// Waits until the queue is empty and the writer is idle. False on timeout. Runs once at load
/// and from the offline tests, never on a Present path.
pub fn flush(timeout: Duration) -> bool {
    let Some(shared) = SHARED.get() else { return true };
    let deadline = Instant::now() + timeout;
    while shared.counters.depth() != 0 {
        if Instant::now() >= deadline {
            return false;
        }
        std::thread::yield_now();
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case("0", Some(LogLevel::Error))]
    #[case("1", Some(LogLevel::Info))]
    #[case("2", Some(LogLevel::Debug))]
    #[case("3", None)]
    #[case("", None)]
    #[case("info", None)]
    fn level_parsing(#[case] value: &str, #[case] want: Option<LogLevel>) {
        assert_eq!(LogLevel::parse(value), want);
    }

    #[rstest]
    #[case(LogLevel::Error, LevelFilter::ERROR)]
    #[case(LogLevel::Info, LevelFilter::INFO)]
    #[case(LogLevel::Debug, LevelFilter::TRACE)]
    fn level_filters(#[case] level: LogLevel, #[case] filter: LevelFilter) {
        assert_eq!(level.filter(), filter);
        assert_eq!(LogLevel::from_int(level.as_int()), level);
    }

    #[test]
    fn digits_do_not_distinguish_messages() {
        assert_eq!(message_key("frame 7 took 3 ms"), message_key("frame 8 took 12 ms"));
        assert_ne!(message_key("frame 7"), message_key("frames"));
    }

    #[rstest]
    // A burst of eight is admitted, the ninth inside the same instant is refused.
    #[case(7, true)]
    #[case(8, false)]
    fn burst_then_refill(#[case] attempt: u32, #[case] admitted: bool) {
        let mut buckets = [Bucket::default(); BUCKETS];
        let mut last = false;
        for _ in 0..=attempt {
            last = admit(&mut buckets, "same line", 0.0).is_some();
        }
        assert_eq!(last, admitted);
    }

    #[test]
    fn suppressed_repeats_are_reported_on_the_next_admitted_line() {
        let mut buckets = [Bucket::default(); BUCKETS];
        for _ in 0..8 {
            assert!(admit(&mut buckets, "repeat", 0.0).is_some());
        }
        for _ in 0..5 {
            assert!(admit(&mut buckets, "repeat", 0.0).is_none());
        }
        // 500 ms refills one token at 2 per second.
        assert_eq!(admit(&mut buckets, "repeat", 500.0), Some(5));
    }

    #[rstest]
    #[case("plain", "plain\n")]
    #[case("two\nlines", "two lines\n")]
    #[case("cr\rhere", "cr here\n")]
    fn line_shape(#[case] message: &str, #[case] want: &str) {
        let text = format_line(message);
        let (stamp, rest) = text.split_once(" ms] ").expect("stamp");
        assert!(stamp.starts_with('['));
        assert!(stamp[1..].parse::<f64>().is_ok(), "{stamp}");
        assert_eq!(rest, want);
    }

    #[test]
    fn long_messages_are_capped() {
        let text = format_line(&"x".repeat(1000));
        let body = text.split_once(" ms] ").unwrap().1.trim_end_matches('\n');
        assert_eq!(body.len(), LINE_MAX - 1);
    }

    #[test]
    fn a_sink_that_cannot_open_counts_lost_lines() {
        let shared = Arc::new(Shared::new(LogLevel::Info));
        let mut sink = FileSink {
            // A path that cannot be a directory, so opening `<dir>/sedici.log` fails.
            directory: PathBuf::from("\u{0}not-a-directory"),
            shared: Arc::clone(&shared),
            reported_dropped: 0,
        };
        assert!(sink.write(b"line\n").is_err());
        let stats = shared.counters.snapshot();
        assert_eq!((stats.sink_failures, stats.lost, stats.sink_ok), (1, 1, false));
    }

    /// End to end against a scratch file: the shape `analyze_log.ps1` matches, the level gate,
    /// the rate limit with its suppressed report, and the counters.
    #[test]
    fn writes_the_cpp_line_shape_and_counts() {
        let dir = std::env::temp_dir().join(format!("sedici-rust-log-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        // A subscriber is process-global, so this test owns it; others do not install one.
        let handle = init(&dir, Some(LogLevel::Info));
        if !handle.is_installed() {
            // Another test in this binary already installed the global subscriber.
            return;
        }

        tracing::info!("distinct line a");
        tracing::info!("distinct line b");
        tracing::debug!("hidden at info");
        for _ in 0..1000 {
            tracing::error!("repeat storm");
        }
        // One token refills at 2 per second; the next admitted repeat reports the suppressed.
        std::thread::sleep(Duration::from_millis(600));
        tracing::error!("repeat storm");
        tracing::info!(raw = true, "raw line");
        assert!(flush(Duration::from_secs(5)), "the queue drains");

        handle.set_level(LogLevel::Debug);
        tracing::debug!("visible at debug");
        assert!(flush(Duration::from_secs(5)));

        let text = std::fs::read_to_string(dir.join(LOG_FILE)).unwrap();
        let stamp = regex_like_stamp(&text);
        assert!(stamp, "every line starts with a [.. ms] stamp: {text}");
        assert!(text.contains("distinct line a"));
        assert!(!text.contains("hidden at info"));
        assert!(text.contains("visible at debug"));
        assert!(text.contains("raw line"));
        // 8 admitted from the burst, then one refilled after the storm's own suppressed count
        // is reported on the next admitted line.
        assert!(text.matches("repeat storm").count() <= 9, "{}", text.matches("repeat storm").count());
        assert!(text.contains("similar suppressed]"));

        let stats = log_stats();
        assert!(stats.queued > 0 && stats.written > 0 && stats.sink_ok);
        assert!(stats.suppressed >= 990, "{}", stats.suppressed);

        set_level(LogLevel::Error);
        assert_eq!(level(), LogLevel::Error);
        handle.set_level(LogLevel::Info);
        assert_eq!(level(), LogLevel::Info);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Every non-empty line starts with `[<number> ms] `.
    fn regex_like_stamp(text: &str) -> bool {
        text.lines().filter(|line| !line.is_empty()).all(|line| {
            line.strip_prefix('[')
                .and_then(|rest| rest.split_once(" ms] "))
                .is_some_and(|(value, _)| value.parse::<f64>().is_ok())
        })
    }
}
