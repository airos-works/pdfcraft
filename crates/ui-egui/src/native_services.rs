//! Safe injection point for the air/OS host. Native calls stay in its audited FFI module.
use std::path::PathBuf;
use std::sync::OnceLock;

pub struct Services {
    pub pick: fn(file_dialog::AsyncFileDialog) -> Vec<PathBuf>,
    pub clipboard_text: fn() -> Result<String, String>,
    pub clipboard_image: fn() -> Result<clipboard::ImageData<'static>, String>,
    pub set_image: fn(clipboard::ImageData<'_>) -> Result<(), String>,
    pub set_files: fn(&[PathBuf]) -> Result<(), String>,
}
static SERVICES: OnceLock<Services> = OnceLock::new();
pub fn install(services: Services) -> Result<(), &'static str> {
    SERVICES.set(services).map_err(|_| "native services already installed")
}

pub mod file_dialog {
    use super::*;
    #[derive(Default, Clone)]
    pub struct AsyncFileDialog {
        pub title: String,
        pub name: String,
        pub extensions: Vec<String>,
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
        pub fn add_filter(mut self, _label: impl Into<String>, extensions: &[impl ToString]) -> Self {
            self.extensions.extend(extensions.iter().map(ToString::to_string));
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
    use super::*;
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
