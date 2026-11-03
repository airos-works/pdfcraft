//! The one way to the operating system's clipboard.
//!
//! The system clipboard is a single process-wide resource: on Windows, two threads that
//! open it at once can corrupt the heap. Every access goes through [`with`], which holds a
//! lock across the whole open, use and close, so parallel callers (test threads, workers)
//! take turns instead of racing.

use std::sync::{Mutex, PoisonError};

static LOCK: Mutex<()> = Mutex::new(());

/// What a clipboard call fails with: arboard's error, or the air/OS host's message.
#[cfg(not(target_os = "haiku"))]
pub(crate) type Error = arboard::Error;
#[cfg(target_os = "haiku")]
pub(crate) type Error = String;

/// Opens the system clipboard, runs `f` on it and closes it, with no other thread inside.
/// `crate::clipboard` is arboard, or the air/OS host's `BClipboard` services.
pub(crate) fn with<T>(f: impl FnOnce(&mut crate::clipboard::Clipboard) -> Result<T, Error>) -> Result<T, Error> {
    let _turn = LOCK.lock().unwrap_or_else(PoisonError::into_inner);
    // Declared after the guard, so it's closed before the lock is released.
    let mut clipboard = crate::clipboard::Clipboard::new()?;
    f(&mut clipboard)
}

/// Nonempty text supersedes the app's cached image, even if another clipboard format also exists.
pub(crate) fn has_text() -> bool {
    with(|cb| cb.get_text()).is_ok_and(|text| !text.is_empty())
}

/// The system clipboard as image paste sees it; text takes precedence over a cached image.
pub(crate) fn for_image_paste() -> Option<crate::create_ui::Clip> {
    if has_text() { Some(crate::create_ui::Clip::Text(String::new())) } else { crate::create_ui::read_clipboard() }
}
