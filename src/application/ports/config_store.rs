//! Load, patch and save sedici.toml.
//!
//! The application never touches the filesystem directly; it asks a [`ConfigStore`]. Everything the
//! port exchanges is a plain value (paths, strings, small structs), so tests can drive a memory
//! store and the adapter owns every Windows call.
use std::path::{Path, PathBuf};

/// The state of a file, used to detect an outside edit between an edit session and a save.
///
/// The digest and the file identity are both load-bearing: a same-size edit with a restored write
/// timestamp still changes the digest, and replacing a file with an identical copy still changes
/// the identity (K43).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FileStamp {
    /// False means the filesystem state could not be determined; never treat a query error as
    /// proof that a file is absent.
    pub valid: bool,
    pub exists: bool,
    pub last_write: u64,
    pub size: u64,
    pub file_id: u64,
    pub volume_serial: u32,
    pub sha256: [u8; 32],
}

/// Equivalence of two stamps. An unknown state never compares equal to anything.
pub fn same_stamp(a: &FileStamp, b: &FileStamp) -> bool {
    if !a.valid || !b.valid || a.exists != b.exists {
        return false;
    }
    if !a.exists {
        return true;
    }
    a.last_write == b.last_write
        && a.size == b.size
        && a.file_id == b.file_id
        && a.volume_serial == b.volume_serial
        && a.sha256 == b.sha256
}

/// The subset of live values a save writes back to the file.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ConfigValues {
    /// `[fg] target_fps`: a number, or the string "auto".
    pub write_target_fps: bool,
    pub target_fps: f64,
    pub target_auto: bool,

    pub write_log: bool,
    pub log_level: i32,

    pub write_ui_mode: bool,
    pub ui_mode: String,

    pub write_ui_scale: bool,
    pub ui_scale_auto: bool,
    pub ui_scale: f32,
}

/// The result of one patch-and-replace. A refused save is a value, never an error the game sees.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SaveOutcome {
    pub written: bool,
    /// The file changed on disk since the caller's baseline; nothing was written (or an outside
    /// edit was put back).
    pub conflict: bool,
    /// User-facing reason when `written` is false.
    pub error: String,
    /// The state of the file that was written.
    pub written_stamp: FileStamp,
}

/// The file operations settings need. Comments, unrelated keys, line endings and formatting are
/// preserved by `save`; missing keys are appended to their section and missing sections appended to
/// the file.
pub trait ConfigStore {
    /// `sedici.toml` beside the DLL, or `None` when the DLL directory could not be resolved.
    fn default_path(&self) -> Option<PathBuf>;

    /// True when `path` exists and is a regular file.
    fn is_file(&self, path: &Path) -> bool;

    /// The file's current state; an unknown state has `valid == false`.
    fn stamp(&self, path: &Path) -> FileStamp;

    /// The file's text. A failure carries the already-formatted problem line for the log.
    fn load_text(&self, path: &Path) -> Result<String, String>;

    /// Patch `values` into the file atomically. When `expected` is given, the file is rechecked
    /// before replacement and an outside edit refuses the save (K43).
    fn save(&self, path: &Path, values: &ConfigValues, expected: Option<&FileStamp>)
        -> SaveOutcome;
}

#[cfg(test)]
pub(crate) mod memory_store {
    use super::*;
    use std::sync::Mutex;

    struct Model {
        text: String,
        stamp: FileStamp,
        next_id: u64,
        saves_before_failure: Option<u32>,
        edit_after_write: bool,
    }

    /// An in-memory [`ConfigStore`] for application tests. It models an outside edit as a plain
    /// text write (same identity, new digest) and a replacement as a new identity.
    pub struct MemoryStore {
        path: PathBuf,
        model: Mutex<Model>,
        patches: Mutex<Vec<String>>,
    }

    impl MemoryStore {
        pub fn new(path: impl Into<PathBuf>, text: impl Into<String>) -> Self {
            let text = text.into();
            Self {
                path: path.into(),
                model: Mutex::new(Model {
                    stamp: FileStamp {
                        valid: true,
                        exists: true,
                        size: text.len() as u64,
                        last_write: 1,
                        file_id: 1,
                        volume_serial: 7,
                        sha256: digest(text.as_bytes()),
                    },
                    text,
                    next_id: 2,
                    saves_before_failure: None,
                    edit_after_write: false,
                }),
                patches: Mutex::new(Vec::new()),
            }
        }

        pub fn text(&self) -> String {
            self.model.lock().unwrap().text.clone()
        }

        /// An outside edit that keeps the file's identity.
        pub fn write_text(&self, text: impl Into<String>) {
            let text = text.into();
            let mut model = self.model.lock().unwrap();
            model.stamp.size = text.len() as u64;
            model.stamp.last_write += 1;
            model.stamp.sha256 = digest(text.as_bytes());
            model.text = text;
        }

        /// A replacement with a new identity (a different inode), even with identical content.
        pub fn replace_text(&self, text: impl Into<String>) {
            let text = text.into();
            let mut model = self.model.lock().unwrap();
            model.stamp.size = text.len() as u64;
            model.stamp.last_write += 1;
            model.stamp.file_id = model.next_id;
            model.next_id += 1;
            model.stamp.sha256 = digest(text.as_bytes());
            model.text = text;
        }

        pub fn fail_next_saves(&self, count: u32) {
            self.model.lock().unwrap().saves_before_failure = Some(count);
        }

        /// The next save's replacement is followed immediately by an outside edit, as an editor
        /// can do between the swap and the caller's verification (K43).
        pub fn edit_immediately_after_write(&self) {
            self.model.lock().unwrap().edit_after_write = true;
        }
    }

    fn digest(bytes: &[u8]) -> [u8; 32] {
        use sha2::{Digest, Sha256};
        let mut out = [0u8; 32];
        out.copy_from_slice(&Sha256::digest(bytes));
        out
    }

    impl ConfigStore for MemoryStore {
        fn default_path(&self) -> Option<PathBuf> {
            Some(self.path.clone())
        }

        fn is_file(&self, _path: &Path) -> bool {
            true
        }

        fn stamp(&self, _path: &Path) -> FileStamp {
            self.model.lock().unwrap().stamp
        }

        fn load_text(&self, _path: &Path) -> Result<String, String> {
            Ok(self.model.lock().unwrap().text.clone())
        }

        fn save(
            &self,
            _path: &Path,
            values: &ConfigValues,
            expected: Option<&FileStamp>,
        ) -> SaveOutcome {
            let mut model = self.model.lock().unwrap();
            if let Some(failures) = model.saves_before_failure.as_mut() {
                if *failures > 0 {
                    *failures -= 1;
                    return SaveOutcome {
                        written: false,
                        conflict: false,
                        error: "could not create a unique temporary file".to_owned(),
                        written_stamp: FileStamp::default(),
                    };
                }
            }
            if let Some(expected) = expected {
                if !same_stamp(&model.stamp, expected) {
                    return SaveOutcome {
                        written: false,
                        conflict: true,
                        error: "sedici.toml changed before save".to_owned(),
                        written_stamp: FileStamp::default(),
                    };
                }
            }
            let text = format!("{}\n# patched {:?}\n", model.text.trim_end(), values);
            self.patches.lock().unwrap().push(text.clone());
            model.stamp.size = text.len() as u64;
            model.stamp.last_write += 1;
            model.stamp.sha256 = digest(text.as_bytes());
            model.text = text;
            // What the caller will verify against, captured before any outside edit lands.
            let written = model.stamp;
            if model.edit_after_write {
                model.edit_after_write = false;
                model.stamp.last_write += 1;
                model.stamp.sha256 = digest(b"[fg]\ntarget_fps = 13\n");
                model.text = "[fg]\ntarget_fps = 13\n".to_owned();
            }
            SaveOutcome {
                written: true,
                conflict: false,
                error: String::new(),
                written_stamp: written,
            }
        }
    }
}
