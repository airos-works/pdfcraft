//! BFilePanel requests run asynchronously; the UI thread routes replies to picker workers.
use crate::host;
use pdfcraft_ui_egui::native_services::{self, clipboard::ImageData, file_dialog::AsyncFileDialog};
use std::path::PathBuf;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{Mutex, mpsc};

pub const SIGNATURE: &str = "application/x-vnd.storytold-PdfCraft";
static SAVED: Mutex<Vec<(PathBuf, std::time::Instant)>> = Mutex::new(Vec::new());
static NEXT: AtomicI32 = AtomicI32::new(1);
static PENDING: Mutex<Option<(i32, mpsc::Sender<Vec<PathBuf>>)>> = Mutex::new(None);

pub fn claim_document_types() {
    host::claim_types(SIGNATURE, &["application/pdf"]);
}
pub fn answer(id: i32, picked: bool, text: &str) {
    if let Ok(mut pending) = PENDING.lock()
        && pending.as_ref().is_some_and(|(n, _)| *n == id)
        && let Some((_, sender)) = pending.take()
    {
        let paths = if picked { text.lines().filter(|s| !s.is_empty()).map(PathBuf::from).collect() } else { Vec::new() };
        let _ = sender.send(paths);
    }
}

fn pick(dialog: AsyncFileDialog) -> Vec<PathBuf> {
    let (tx, rx) = mpsc::channel();
    let id = NEXT.fetch_add(1, Ordering::Relaxed);
    if let Ok(mut pending) = PENDING.lock() {
        if pending.is_some() {
            return Vec::new();
        }
        *pending = Some((id, tx));
    } else {
        return Vec::new();
    }
    let extensions: Vec<&str> = dialog.extensions.iter().map(String::as_str).collect();
    let title = if dialog.title.is_empty() {
        match dialog.mode {
            1 => "Save As",
            2 => "Choose Folder",
            _ => "Open",
        }
    } else {
        &dialog.title
    };
    host::file_panel(id, dialog.mode, &dialog.name, &dialog.directory, &extensions, &format!("PdfCraft: {title}"));
    let paths = rx.recv().unwrap_or_default();
    if dialog.mode == 1
        && let Ok(mut saved) = SAVED.lock()
    {
        saved.extend(paths.iter().cloned().map(|p| (p, std::time::Instant::now())));
    }
    paths
}
pub fn clipboard_text() -> Option<String> {
    host::clipboard_get("text/plain").map(|v| String::from_utf8_lossy(&v).trim_end_matches('\0').to_string())
}
fn clipboard_image() -> Result<ImageData<'static>, String> {
    let bytes = host::clipboard_get("image/png").ok_or("no PNG image on the clipboard")?;
    let image = image::load_from_memory_with_format(&bytes, image::ImageFormat::Png).map_err(|e| e.to_string())?.to_rgba8();
    Ok(ImageData { width: image.width() as usize, height: image.height() as usize, bytes: image.into_raw().into() })
}
fn set_image(data: ImageData<'_>) -> Result<(), String> {
    let w = u32::try_from(data.width).map_err(|e| e.to_string())?;
    let h = u32::try_from(data.height).map_err(|e| e.to_string())?;
    let image = image::RgbaImage::from_raw(w, h, data.bytes.into_owned()).ok_or("invalid clipboard image dimensions")?;
    let mut bytes = std::io::Cursor::new(Vec::new());
    image.write_to(&mut bytes, image::ImageFormat::Png).map_err(|e| e.to_string())?;
    if host::clipboard_set_picture("image/png", bytes.get_ref()) { Ok(()) } else { Err("could not set clipboard image".into()) }
}
pub fn install() -> Result<(), &'static str> {
    native_services::install(native_services::Services {
        pick,
        clipboard_text: || clipboard_text().ok_or_else(|| "no text on the clipboard".into()),
        clipboard_image,
        set_image,
        set_files: |paths| host::clipboard_files(paths).then_some(()).ok_or_else(|| "could not copy files".into()),
    })
}

/// Save continuations may write on a later frame. Register their MIME types once present.
pub fn type_saved() -> bool {
    let Ok(mut saved) = SAVED.lock() else { return false };
    saved.retain(|(path, started)| {
        if path.is_file() {
            host::type_file(path);
            false
        } else {
            started.elapsed() < std::time::Duration::from_secs(10)
        }
    });
    !saved.is_empty()
}
