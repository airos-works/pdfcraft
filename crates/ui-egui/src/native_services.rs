//! Safe injection point for the air/OS host. Native calls stay in its audited FFI module.
use std::path::PathBuf;
use std::sync::OnceLock;

pub struct Services {
    pub pick: fn(file_dialog::AsyncFileDialog) -> Vec<PathBuf>,
    pub clipboard_text: fn() -> Result<String, String>,
    pub clipboard_image: fn() -> Result<clipboard::ImageData<'static>, String>,
    pub set_image: fn(clipboard::ImageData<'_>) -> Result<(), String>,
    pub set_files: fn(&[PathBuf]) -> Result<(), String>,
    /// The system's key store (air/OS: the Key Store), for digital IDs.
    pub key_store: key_store::KeyStore,
    /// Shows a file selected in its folder in the file manager (air/OS: Tracker).
    pub reveal: fn(&std::path::Path) -> Result<(), String>,
}
static SERVICES: OnceLock<Services> = OnceLock::new();
pub fn install(services: Services) -> Result<(), &'static str> {
    SERVICES.set(services).map_err(|_| "native services already installed")
}

/// The tab menu's Reveal: the file selected in its folder's file manager window.
pub fn reveal(path: &std::path::Path) -> Result<(), String> {
    (SERVICES.get().ok_or("the file manager is unavailable")?.reveal)(path)
}

pub mod file_dialog {
    use super::{PathBuf, SERVICES};
    #[derive(Default, Clone)]
    pub struct AsyncFileDialog {
        pub title: String,
        pub name: String,
        /// The filters, in order (the first shows): each one's label and extensions.
        pub filters: Vec<(String, Vec<String>)>,
        /// The folder the panel opens in; empty for the panel's own choice.
        pub directory: String,
        /// 0: file, 1: save, 2: directory, 3: multiple files.
        pub mode: i32,
    }
    pub struct FileHandle(PathBuf);
    impl FileHandle {
        pub fn path(&self) -> &std::path::Path {
            &self.0
        }
    }
    impl AsyncFileDialog {
        pub fn new() -> Self {
            Self::default()
        }
        pub fn set_title(mut self, title: impl Into<String>) -> Self {
            self.title = title.into();
            self
        }
        pub fn set_file_name(mut self, name: impl Into<String>) -> Self {
            self.name = name.into();
            self
        }
        pub fn set_directory(mut self, dir: impl AsRef<std::path::Path>) -> Self {
            self.directory = dir.as_ref().to_string_lossy().into_owned();
            self
        }
        pub fn add_filter(mut self, label: impl Into<String>, extensions: &[impl ToString]) -> Self {
            self.filters.push((label.into(), extensions.iter().map(ToString::to_string).collect()));
            self
        }
        fn pick(mut self, mode: i32) -> Vec<FileHandle> {
            self.mode = mode;
            SERVICES.get().map(|s| (s.pick)(self)).unwrap_or_default().into_iter().map(FileHandle).collect()
        }
        pub async fn pick_file(self) -> Option<FileHandle> {
            self.pick(0).into_iter().next()
        }
        pub async fn save_file(self) -> Option<FileHandle> {
            self.pick(1).into_iter().next()
        }
        pub async fn pick_folder(self) -> Option<FileHandle> {
            self.pick(2).into_iter().next()
        }
        pub async fn pick_files(self) -> Option<Vec<FileHandle>> {
            Some(self.pick(3))
        }
    }
}

pub mod clipboard {
    use super::{PathBuf, SERVICES, Services};
    pub struct ImageData<'a> {
        pub width: usize,
        pub height: usize,
        pub bytes: std::borrow::Cow<'a, [u8]>,
    }
    pub struct Clipboard;
    impl Clipboard {
        pub fn new() -> Result<Self, String> {
            SERVICES.get().ok_or("native clipboard unavailable")?;
            Ok(Self)
        }
        pub fn get_text(&mut self) -> Result<String, String> {
            (services()?.clipboard_text)()
        }
        pub fn get_image(&mut self) -> Result<ImageData<'static>, String> {
            (services()?.clipboard_image)()
        }
        pub fn set_image(&mut self, image: ImageData<'_>) -> Result<(), String> {
            (services()?.set_image)(image)
        }
        pub fn set(&mut self) -> &mut Self {
            self
        }
        pub fn file_list(&mut self, files: &[PathBuf]) -> Result<(), String> {
            (services()?.set_files)(files)
        }
    }
    fn services() -> Result<&'static Services, String> {
        SERVICES.get().ok_or_else(|| "native clipboard unavailable".into())
    }
}

/// Secrets in the system's key store: what the host keeps there for the app, each under a
/// unique identifier, with a name people see and its data.
pub mod key_store {
    use super::SERVICES;

    pub struct Stored {
        pub identifier: String,
        pub name: String,
        pub data: Vec<u8>,
    }

    pub struct KeyStore {
        pub list: fn() -> Result<Vec<Stored>, String>,
        /// Adds a secret, replacing one with the same identifier.
        pub add: fn(&Stored) -> Result<(), String>,
        pub remove: fn(&str) -> Result<(), String>,
    }

    fn store() -> Result<&'static KeyStore, String> {
        SERVICES.get().map(|s| &s.key_store).ok_or_else(|| "the key store is unavailable".into())
    }

    pub fn list() -> Result<Vec<Stored>, String> {
        (store()?.list)()
    }

    pub fn add(secret: &Stored) -> Result<(), String> {
        (store()?.add)(secret)
    }

    pub fn remove(identifier: &str) -> Result<(), String> {
        (store()?.remove)(identifier)
    }
}
