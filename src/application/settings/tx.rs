//! Transactional access to the live settings (P2.6, ports `settings/settings_tx.cpp`).
//!
//! Every change to a live value goes through one complete candidate: it is validated as a whole,
//! published as a coherent snapshot, and given a new revision. A request names the revision it was
//! built from; if another change landed first the request is a [`ApplyStatus::Conflict`] and
//! nothing is applied, so two writers can never silently overwrite each other.
//!
//! `[ui] mode` is menu state, not a setting, and lives on [`Live`].
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard};

use crate::application::ports::config_store::{
    same_stamp, ConfigStore, ConfigValues, FileStamp, SaveOutcome,
};
use crate::application::settings::live::{Live, LiveValues};
use crate::domain::config::{Settings, Source, UiScaleMode};

/// The outcome of one apply.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ApplyStatus {
    /// Immediately published fields changed; `revision` advanced.
    Applied,
    /// The candidate equals the current values; no revision, no side effects.
    Unchanged,
    /// A value is out of range or invalid; nothing was applied.
    #[default]
    Rejected,
    /// Built from a stale revision; nothing was applied.
    Conflict,
}

/// The published desired values and the revision they belong to.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SettingsSnapshot {
    pub revision: u64,
    pub values: LiveValues,
}

/// The outcome of one apply. `reason` is set for Rejected and Conflict.
#[derive(Clone, Debug, PartialEq)]
pub struct ApplyResult {
    pub status: ApplyStatus,
    pub revision: u64,
    pub reason: String,
}

/// A save the caller captured before the file work started. A worker must revalidate it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SaveTicket {
    pub generation: u64,
    pub revision: u64,
    pub valid: bool,
}

/// A save's user-facing result. A refused save is a value, never an exception.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SaveResult {
    pub ok: bool,
    /// The file changed on disk since the edit began; not written.
    pub conflict: bool,
    /// "Saved" or the reason it was not.
    pub message: String,
}

#[derive(Clone, Copy, Debug, Default)]
struct Baseline {
    values: LiveValues,
    stamp: FileStamp,
}

#[derive(Debug, Default)]
struct TxState {
    revision: u64,
    generation: u64,
    saving: bool,
    baseline: Option<Baseline>,
    edit_revision: u64,
    persistence_uncertain: bool,
}

/// In-memory settings state. The `Arc` is shared between the menu, the limiter and the I/O worker.
pub struct SettingsTx {
    live: Arc<Live>,
    config: Settings,
    store: Arc<dyn ConfigStore + Send + Sync>,
    path_override: Mutex<Option<PathBuf>>,
    state: Mutex<TxState>,
    /// Serializes persistence and path changes; never held by an apply.
    io: Mutex<()>,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    // A poisoning panic can only come from a bug in this module; the state is still readable and
    // the game must never see a panic across an ffx export.
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

impl SettingsTx {
    pub fn new(
        live: Arc<Live>,
        config: Settings,
        store: Arc<dyn ConfigStore + Send + Sync>,
    ) -> Self {
        Self {
            live,
            config,
            store,
            path_override: Mutex::new(None),
            state: Mutex::new(TxState {
                revision: 1,
                ..TxState::default()
            }),
            io: Mutex::new(()),
        }
    }

    pub fn live(&self) -> &Arc<Live> {
        &self.live
    }

    pub fn config(&self) -> &Settings {
        &self.config
    }

    /// The file this transaction reads and writes: the override, else `sedici.toml` beside the DLL.
    pub fn config_path(&self) -> Option<PathBuf> {
        let override_path = lock(&self.path_override).clone();
        override_path.or_else(|| self.store.default_path())
    }

    pub fn snapshot(&self) -> SettingsSnapshot {
        // K59: a lock failure must not escape. The values are still readable.
        let state = lock(&self.state);
        SettingsSnapshot {
            revision: state.revision,
            values: self.live.snapshot(),
        }
    }

    /// Validates `candidate`, then applies it only if `base_revision` is still current.
    pub fn apply(&self, base_revision: u64, candidate: LiveValues) -> ApplyResult {
        let mut state = lock(&self.state);
        self.apply_locked(&mut state, base_revision, candidate)
    }

    /// A single-control change: read the snapshot, mutate a copy, apply against that revision. A
    /// concurrent change makes it a Conflict; the caller keeps showing the acknowledged values.
    pub fn change(&self, mutate: impl FnOnce(&mut LiveValues)) -> ApplyResult {
        let base = self.snapshot();
        let mut next = base.values;
        mutate(&mut next);
        self.apply(base.revision, next)
    }

    fn apply_locked(
        &self,
        state: &mut TxState,
        base_revision: u64,
        candidate: LiveValues,
    ) -> ApplyResult {
        let revision = state.revision;
        if let Err(reason) = validate(&candidate) {
            return ApplyResult {
                status: ApplyStatus::Rejected,
                revision,
                reason,
            };
        }
        if base_revision != revision {
            return ApplyResult {
                status: ApplyStatus::Conflict,
                revision,
                reason: "settings changed since this request was built".to_owned(),
            };
        }

        let before = self.live.snapshot();
        let target_env = self.config.fg.target_fps_source == Source::Env
            && (candidate.target_fps != before.target_fps
                || candidate.target_auto != before.target_auto);
        let log_env =
            self.config.log.level_source == Source::Env && candidate.log_level != before.log_level;
        if target_env || log_env {
            return ApplyResult {
                status: ApplyStatus::Rejected,
                revision,
                reason: "an environment override controls this setting".to_owned(),
            };
        }
        if before == candidate {
            return ApplyResult {
                status: ApplyStatus::Unchanged,
                revision,
                reason: String::new(),
            };
        }

        // Publish the candidate as one coherent snapshot.
        self.live.apply(candidate);
        state.revision += 1;
        if before.target_fps != candidate.target_fps || before.target_auto != candidate.target_auto
        {
            self.live.notify_cap_changed();
        }
        ApplyResult {
            status: ApplyStatus::Applied,
            revision: state.revision,
            reason: String::new(),
        }
    }

    /// Points the transaction at a different file (tests and `settings_use_file`).
    pub fn use_file(&self, path: impl Into<PathBuf>) {
        let _io = lock(&self.io);
        let mut state = lock(&self.state);
        *lock(&self.path_override) = Some(path.into());
        state.baseline = None;
        bump_generation(&mut state.generation);
    }

    /// Reserves an edit epoch without disk I/O. Returns 0 only if the generation cannot advance.
    pub fn start_edit(&self) -> u64 {
        let mut state = lock(&self.state);
        state.baseline = None;
        bump_generation(&mut state.generation);
        state.generation
    }

    pub fn invalidate_edit(&self) {
        let _ = self.start_edit();
    }

    /// Captures the baseline for `generation`, if it is still current. Performs file inspection;
    /// the caller runs this on the I/O worker.
    pub fn begin_edit_for(&self, generation: u64) -> bool {
        if generation == 0 {
            return false;
        }
        let _io = lock(&self.io);
        let (values, revision, path) = {
            let mut state = lock(&self.state);
            if generation != state.generation {
                return false;
            }
            state.baseline = None;
            (self.live.snapshot(), state.revision, self.config_path())
        };
        let Some(path) = path else { return false };
        let stamp = self.store.stamp(&path);

        let mut state = lock(&self.state);
        if !stamp.valid || generation != state.generation || revision != state.revision {
            return false;
        }
        state.baseline = Some(Baseline { values, stamp });
        state.edit_revision = revision;
        state.persistence_uncertain = false;
        true
    }

    pub fn begin_edit(&self) -> bool {
        self.begin_edit_for(self.start_edit())
    }

    /// Applies a menu edit only if no outside writer changed the edit session.
    pub fn apply_edit(&self, base_revision: u64, candidate: LiveValues) -> ApplyResult {
        let mut state = lock(&self.state);
        if state.saving {
            return conflict_result(state.revision, "save is in progress");
        }
        if state.persistence_uncertain {
            return conflict_result(state.revision, "file state is uncertain; reopen the menu");
        }
        if state.baseline.is_none() || state.edit_revision != state.revision {
            return conflict_result(state.revision, "settings changed outside this edit");
        }
        let result = self.apply_locked(&mut state, base_revision, candidate);
        if matches!(result.status, ApplyStatus::Applied | ApplyStatus::Unchanged) {
            state.edit_revision = result.revision;
        }
        result
    }

    pub fn change_edit(&self, mutate: impl FnOnce(&mut LiveValues)) -> ApplyResult {
        let base = self.snapshot();
        let mut next = base.values;
        mutate(&mut next);
        self.apply_edit(base.revision, next)
    }

    pub fn cancel_edit(&self) -> ApplyResult {
        let mut state = lock(&self.state);
        if state.saving {
            return conflict_result(state.revision, "save is in progress");
        }
        if state.persistence_uncertain {
            return ApplyResult {
                status: ApplyStatus::Rejected,
                revision: state.revision,
                reason: "file state is uncertain; reopen the menu".to_owned(),
            };
        }
        let Some(baseline) = state.baseline else {
            return ApplyResult {
                status: ApplyStatus::Rejected,
                revision: state.revision,
                reason: "edit baseline is unavailable".to_owned(),
            };
        };
        if state.edit_revision != state.revision {
            return conflict_result(state.revision, "settings changed outside this edit");
        }
        let revision = state.revision;
        let result = self.apply_locked(&mut state, revision, baseline.values);
        if matches!(result.status, ApplyStatus::Applied | ApplyStatus::Unchanged) {
            state.edit_revision = result.revision;
        }
        result
    }

    /// Captures only in-memory state; a worker must revalidate the ticket before I/O.
    pub fn capture_save_ticket(&self) -> SaveTicket {
        let state = lock(&self.state);
        SaveTicket {
            generation: state.generation,
            revision: state.revision,
            valid: state.baseline.is_some()
                && !state.saving
                && state.edit_revision == state.revision
                && !state.persistence_uncertain,
        }
    }

    pub fn save(&self) -> SaveResult {
        self.save_impl(None)
    }

    pub fn save_for(&self, ticket: SaveTicket) -> SaveResult {
        self.save_impl(Some(ticket))
    }

    fn save_impl(&self, ticket: Option<SaveTicket>) -> SaveResult {
        let mut out = SaveResult::default();
        // Allocate this before any replacement so even an allocation failure afterwards can report
        // that the file may already have changed.
        const WROTE_UNVERIFIED: &str = "Save was written, but verification failed; reopen the menu";
        const SAVED: &str = "Saved";
        const NEWER: &str = "Saved an earlier revision; newer settings remain unsaved";
        const POST_WRITE_CONFLICT: &str =
            "Save was written, but the file changed immediately afterward; reopen the menu";

        // Persistence operations are serialized separately from live settings. Neither file
        // inspection nor writing holds the settings state lock.
        let _io = lock(&self.io);
        let (path, baseline_stamp, values, saved_revision, generation, file) = {
            let mut state = lock(&self.state);
            let Some(baseline) = state.baseline else {
                out.message = "Save failed: file state is unavailable".to_owned();
                return out;
            };
            if state.persistence_uncertain {
                out.message = "Save failed: file state is unavailable".to_owned();
                return out;
            }
            if let Some(ticket) = ticket {
                let stale = !ticket.valid
                    || ticket.generation != state.generation
                    || ticket.revision != state.revision;
                if stale {
                    out.conflict = true;
                    out.message = "settings changed since Save was requested".to_owned();
                    return out;
                }
            }
            if state.edit_revision != state.revision {
                out.conflict = true;
                out.message = "settings changed outside this edit; not saved".to_owned();
                return out;
            }
            let Some(path) = self.config_path() else {
                out.message = "Save failed: file state is unavailable".to_owned();
                return out;
            };
            let values = self.live.snapshot();
            let mut file = ConfigValues::default();
            if self.config.fg.target_fps_source != Source::Env {
                file.write_target_fps = true;
                file.target_fps = values.target_fps;
                file.target_auto = values.target_auto;
            }
            if self.config.log.level_source != Source::Env {
                file.write_log = true;
                file.log_level = values.log_level;
            }
            // UI mode is a startup preference, not this edit session's view state.
            file.write_ui_scale = true;
            file.ui_scale_auto = values.scale_mode == UiScaleMode::Auto;
            file.ui_scale = values.ui_scale;
            state.saving = true;
            (
                path,
                baseline.stamp,
                values,
                state.revision,
                state.generation,
                file,
            )
        };
        let _clear = SavingFlag { tx: self };

        let current = self.store.stamp(&path);
        if !same_stamp(&current, &baseline_stamp) {
            out.conflict = true;
            out.message = "sedici.toml changed or is unavailable; not saved".to_owned();
            tracing::error!("sedici: overlay save refused: file changed or unavailable");
            return out;
        }

        let outcome: SaveOutcome = self.store.save(&path, &file, Some(&baseline_stamp));
        if !outcome.written {
            out.conflict = outcome.conflict;
            out.message = format!("Save failed: {}", outcome.error);
            tracing::error!("{}", out.message);
            return out;
        }

        // From here the file is ours, so any failure must report that it may already have changed.
        match self.verify_save(
            &path,
            outcome.written_stamp,
            values,
            saved_revision,
            generation,
        ) {
            Ok(unchanged) => {
                out.ok = true;
                out.message = if unchanged { SAVED } else { NEWER }.to_owned();
            }
            Err(post_write_conflict) => {
                out.conflict = true;
                out.message = if post_write_conflict {
                    POST_WRITE_CONFLICT
                } else {
                    // K59/K43: an unverifiable write leaves the file state uncertain, so a later
                    // Save must not assume the baseline still matches the disk.
                    lock(&self.state).persistence_uncertain = true;
                    WROTE_UNVERIFIED
                }
                .to_owned();
                return out;
            }
        }

        tracing::info!("sedici: overlay saved a settings snapshot");
        out
    }

    /// Verifies the file after the replacement and moves the baseline. `Ok(true)` means the saved
    /// revision was still current; `Err(true)` means the file was written but an outside edit landed
    /// immediately after it; `Err(false)` means the write could not be verified.
    fn verify_save(
        &self,
        path: &std::path::Path,
        written_stamp: FileStamp,
        values: LiveValues,
        saved_revision: u64,
        generation: u64,
    ) -> Result<bool, bool> {
        if !written_stamp.valid {
            return Err(false);
        }
        let saved_stamp = self.store.stamp(path);
        let matches = same_stamp(&saved_stamp, &written_stamp);
        let mut state = lock(&self.state);
        let mut unchanged = false;
        // The I/O lock prevents session/path replacement while saving.
        if generation == state.generation {
            state.baseline = matches.then_some(Baseline {
                values,
                stamp: saved_stamp,
            });
            if matches {
                unchanged = state.revision == saved_revision;
            }
        }
        if !matches {
            return Err(true);
        }
        Ok(unchanged)
    }
}

struct SavingFlag<'a> {
    tx: &'a SettingsTx,
}

impl Drop for SavingFlag<'_> {
    fn drop(&mut self) {
        lock(&self.tx.state).saving = false;
    }
}

/// The transaction is the file operations the I/O worker drives.
impl crate::application::settings::io::SettingsIoPort for SettingsTx {
    fn begin_edit_for(&self, generation: u64) -> bool {
        SettingsTx::begin_edit_for(self, generation)
    }

    fn save_for(&self, ticket: SaveTicket) -> SaveResult {
        self.save_for(ticket)
    }
}

fn bump_generation(generation: &mut u64) {
    *generation = generation.wrapping_add(1);
    if *generation == 0 {
        *generation = 1;
    }
}

fn conflict_result(revision: u64, reason: &str) -> ApplyResult {
    ApplyResult {
        status: ApplyStatus::Conflict,
        revision,
        reason: reason.to_owned(),
    }
}

/// Returns the first problem, or `Ok(())` when the whole candidate is valid.
fn validate(values: &LiveValues) -> Result<(), String> {
    if !values.target_fps.is_finite() || !(0.0..=1000.0).contains(&values.target_fps) {
        return Err("fg.target_fps must be between 0 and 1000".to_owned());
    }
    if !(0..=2).contains(&values.log_level) {
        return Err("log.level must be 0, 1 or 2".to_owned());
    }
    if !matches!(values.scale_mode, UiScaleMode::Auto | UiScaleMode::Fixed) {
        return Err("ui.scale_mode is not valid".to_owned());
    }
    if !values.ui_scale.is_finite() || !(0.5..=2.0).contains(&values.ui_scale) {
        return Err("ui.scale must be between 0.5 and 2.0".to_owned());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application::ports::config_store::memory_store::MemoryStore;
    use rstest::rstest;
    use std::sync::Arc;

    fn tx(text: &str) -> (SettingsTx, Arc<MemoryStore>) {
        let live = Arc::new(Live::new());
        live.set_target_fps(37.0);
        let store = Arc::new(MemoryStore::new("C:/game/sedici.toml", text));
        (
            SettingsTx::new(live, Settings::default(), store.clone()),
            store,
        )
    }

    #[test]
    fn a_valid_candidate_is_applied_with_a_new_revision() {
        let (tx, _) = tx("");
        let base = tx.snapshot();
        let mut next = base.values;
        next.target_fps = 40.0;
        let applied = tx.apply(base.revision, next);
        assert_eq!(applied.status, ApplyStatus::Applied);
        assert_eq!(applied.revision, base.revision + 1);
        assert_eq!(tx.live().limiter_fps(), 40.0);
    }

    #[test]
    fn identical_candidate_is_unchanged_and_stale_one_conflicts() {
        let (tx, _) = tx("");
        let base = tx.snapshot();
        let mut next = base.values;
        next.target_fps = 40.0;
        let applied = tx.apply(base.revision, next);
        assert_eq!(
            tx.apply(applied.revision, next).status,
            ApplyStatus::Unchanged
        );

        let mut stale = next;
        stale.target_fps = 50.0;
        let conflict = tx.apply(base.revision, stale);
        assert_eq!(conflict.status, ApplyStatus::Conflict);
        assert_eq!(conflict.revision, applied.revision);
        assert_eq!(tx.live().limiter_fps(), 40.0);
    }

    #[rstest]
    #[case(LiveValues { target_fps: 2000.0, ..LiveValues::default() })]
    #[case(LiveValues { log_level: 9, ..LiveValues::default() })]
    #[case(LiveValues { ui_scale: 0.1, ..LiveValues::default() })]
    fn out_of_range_values_are_rejected_without_touching_the_live_value(
        #[case] candidate: LiveValues,
    ) {
        let (tx, _) = tx("");
        let before = tx.snapshot();
        let rejected = tx.apply(before.revision, candidate);
        assert_eq!(rejected.status, ApplyStatus::Rejected);
        assert!(!rejected.reason.is_empty());
        assert_eq!(tx.live().limiter_fps(), 37.0);
        assert_eq!(tx.snapshot().revision, before.revision);
    }

    #[test]
    fn one_invalid_field_rejects_the_whole_candidate() {
        let (tx, _) = tx("");
        let base = tx.snapshot();
        let candidate = LiveValues {
            target_fps: 41.0,
            log_level: 9,
            ..base.values
        };
        assert_eq!(
            tx.apply(base.revision, candidate).status,
            ApplyStatus::Rejected
        );
        assert_eq!(tx.live().limiter_fps(), 37.0);
    }

    #[test]
    fn an_environment_controlled_field_rejects_live_overrides() {
        let live = Arc::new(Live::new());
        let store = Arc::new(MemoryStore::new("C:/game/sedici.toml", ""));
        let mut config = Settings::default();
        config.fg.target_fps = 45.0;
        config.fg.target_fps_source = Source::Env;
        config.log.level = 2;
        config.log.level_source = Source::Env;
        let tx = SettingsTx::new(Arc::clone(&live), config, store);
        live.initialize(tx.config());
        let base = tx.snapshot();

        let target = LiveValues {
            target_fps: 30.0,
            ..base.values
        };
        assert_eq!(
            tx.apply(base.revision, target).status,
            ApplyStatus::Rejected
        );
        let log = LiveValues {
            log_level: 1,
            ..base.values
        };
        assert_eq!(tx.apply(base.revision, log).status, ApplyStatus::Rejected);
        assert_eq!(tx.snapshot().revision, base.revision);
    }

    #[test]
    fn an_invalidated_edit_generation_cannot_publish_a_baseline() {
        let (tx, _) = tx("[fg]\ntarget_fps = 12\n");
        let superseded = tx.start_edit();
        tx.invalidate_edit();
        assert!(!tx.begin_edit_for(superseded));
        assert!(tx.begin_edit());
    }

    #[test]
    fn cancel_restores_the_baseline_as_one_change() {
        let (tx, _) = tx("[fg]\ntarget_fps = 12\n");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        let applied = tx.change_edit(|values| {
            values.target_fps = 50.0;
            values.log_level = 2;
        });
        assert_eq!(applied.status, ApplyStatus::Applied);
        assert_eq!(tx.live().limiter_fps(), 50.0);

        assert_eq!(tx.cancel_edit().status, ApplyStatus::Applied);
        assert_eq!(tx.live().limiter_fps(), 37.0);
        assert_eq!(tx.live().log_level(), 1);
    }

    #[test]
    fn save_persists_the_acknowledged_value_and_moves_the_baseline() {
        let (tx, store) = tx("[limiter]\n# keep this comment\nfps = 0\n");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        assert_eq!(
            tx.change_edit(|v| v.target_fps = 45.0).status,
            ApplyStatus::Applied
        );
        let saved = tx.save();
        assert!(saved.ok, "{}", saved.message);
        assert!(store.text().contains("37.0") || store.text().contains("45"));
        assert!(store.text().contains("# keep this comment"));

        // A later cancel returns to the saved value, not the pre-save one.
        assert_eq!(
            tx.change_edit(|v| v.target_fps = 60.0).status,
            ApplyStatus::Applied
        );
        let _ = tx.cancel_edit();
        assert_eq!(tx.live().limiter_fps(), 45.0);
    }

    #[test]
    fn a_stale_save_ticket_cannot_write_the_file() {
        let (tx, store) = tx("[fg]\ntarget_fps = 12\n");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        let ticket = tx.capture_save_ticket();
        assert!(ticket.valid);
        assert_eq!(
            tx.change_edit(|v| v.target_fps = 38.0).status,
            ApplyStatus::Applied
        );
        let before = store.text();
        let stale = tx.save_for(ticket);
        assert!(!stale.ok && stale.conflict);
        assert_eq!(store.text(), before);
    }

    #[test]
    fn an_outside_edit_between_begin_and_save_is_refused_and_kept() {
        let (tx, store) = tx("[fg]\ntarget_fps = 12\n");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        store.write_text("[fg]\ntarget_fps = 13\n");
        assert_eq!(
            tx.change_edit(|v| v.target_fps = 33.0).status,
            ApplyStatus::Applied
        );
        let refused = tx.save();
        assert!(!refused.ok && refused.conflict);
        assert_eq!(store.text(), "[fg]\ntarget_fps = 13\n");
        assert_eq!(tx.live().limiter_fps(), 33.0);
    }

    #[test]
    fn a_replacement_with_identical_bytes_invalidates_the_baseline() {
        let (tx, store) = tx("[fg]\ntarget_fps = 12\n");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        store.replace_text("[fg]\ntarget_fps = 12\n");
        let saved = tx.save();
        assert!(!saved.ok && saved.conflict, "{}", saved.message);
    }

    #[test]
    fn a_post_write_external_edit_is_not_accepted_as_the_saved_baseline() {
        let (tx, store) = tx("[fg]\ntarget_fps = 12\n");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        assert_eq!(
            tx.change_edit(|v| v.target_fps = 34.0).status,
            ApplyStatus::Applied
        );
        store.edit_immediately_after_write();
        let saved = tx.save();
        assert!(!saved.ok, "{}", saved.message);
        assert!(saved.message.contains("was written"), "{}", saved.message);
        assert_eq!(tx.cancel_edit().status, ApplyStatus::Rejected);
    }

    #[test]
    fn a_failed_write_is_a_result_and_leaves_live_values_alone() {
        let (tx, store) = tx("");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        store.fail_next_saves(1);
        let failed = tx.save();
        assert!(!failed.ok && !failed.conflict);
        assert!(
            failed.message.starts_with("Save failed"),
            "{}",
            failed.message
        );
        assert_eq!(tx.live().limiter_fps(), 37.0);
        // The baseline is still usable after a failed (never-written) save.
        assert!(tx.save().ok);
    }

    #[test]
    fn cancel_and_save_refuse_an_unavailable_baseline() {
        let (tx, _) = tx("[fg]\ntarget_fps = 12\n");
        live_initialize(&tx);
        // Never began an edit.
        assert_eq!(tx.cancel_edit().status, ApplyStatus::Rejected);
        assert!(!tx.save().ok);
    }

    #[test]
    fn an_outside_change_makes_cancel_and_save_refuse() {
        let (tx, _) = tx("[fg]\ntarget_fps = 12\n");
        live_initialize(&tx);
        assert!(tx.begin_edit());
        assert_eq!(
            tx.change_edit(|v| v.target_fps = 32.0).status,
            ApplyStatus::Applied
        );
        assert_eq!(
            tx.change(|v| v.target_fps = 33.0).status,
            ApplyStatus::Applied
        );
        assert_eq!(tx.cancel_edit().status, ApplyStatus::Conflict);
        assert!(tx.save().conflict);
    }

    #[test]
    fn concurrent_changes_never_lose_or_double_count_a_revision() {
        let (tx, _) = tx("");
        let tx = Arc::new(tx);
        let start = tx.snapshot().revision;
        let mut handles = Vec::new();
        for worker in 0..4 {
            let tx = Arc::clone(&tx);
            handles.push(std::thread::spawn(move || {
                let mut applied = 0u64;
                for i in 0..200 {
                    let fps = 20.0 + f64::from(worker * 10 + i % 5);
                    if tx.change(|v| v.target_fps = fps).status == ApplyStatus::Applied {
                        applied += 1;
                    }
                }
                applied
            }));
        }
        let applied: u64 = handles.into_iter().map(|h| h.join().expect("writer")).sum();
        assert!(applied > 0);
        assert_eq!(tx.snapshot().revision, start + applied);
    }

    fn live_initialize(tx: &SettingsTx) {
        tx.live().initialize(tx.config());
        tx.live().set_target_fps(37.0);
    }
}
