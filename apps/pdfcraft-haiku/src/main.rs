//! PdfCraft for air/OS (and Haiku).
//!
//! The same interface as the desktop app (`pdfcraft-ui-egui`), hosted natively: a
//! `BApplication` and `BWindow` from the Be API (`native/host.cpp`) take the input and show the
//! frames, which egui's output becomes with Vulkan (`gpu`, shown by `present`) or, without a
//! hardware Vulkan adapter, through a software rasteriser (`raster`), with the system's file
//! panels, clipboard, cursors, file types and locale. eframe and winit don't run on Haiku;
//! nothing below the interface changes.
//!
//! Usage: `PdfCraft [--control FILE] [FILE…]`, as the desktop app.
#![deny(clippy::unwrap_used, clippy::expect_used, clippy::panic, clippy::unimplemented, clippy::todo, clippy::unreachable)]
// Elsewhere only the platform-neutral parts build (and are tested).
#![cfg_attr(not(target_os = "haiku"), allow(dead_code))]

mod damage;
mod keys;
mod raster;

#[cfg(target_os = "haiku")]
mod gpu;
#[cfg(target_os = "haiku")]
mod host;
#[cfg(target_os = "haiku")]
#[path = "../../pdfcraft/src/logging.rs"]
mod logging;
#[cfg(target_os = "haiku")]
mod present;
#[cfg(target_os = "haiku")]
mod scanout;
#[cfg(target_os = "haiku")]
mod services;
#[cfg(target_os = "haiku")]
mod shell;

#[cfg(not(target_os = "haiku"))]
fn main() {
    let _ = std::io::Write::write_all(&mut std::io::stderr(), b"PdfCraft for air/OS runs on Haiku; elsewhere run `pdfcraft`.\n");
    std::process::exit(1);
}

#[cfg(target_os = "haiku")]
use haiku::*;

#[cfg(target_os = "haiku")]
fn main() {
    haiku::main()
}

#[cfg(target_os = "haiku")]
mod haiku {
    use std::path::PathBuf;

    use pdfcraft_ui_egui::PdfCraftApp;

    use crate::{host, logging, services, shell, write_control_file};

    const USAGE: &str = "\
PdfCraft — the PdfCraft PDF editor for air/OS

USAGE:
  PdfCraft [--control FILE] [--page N] [--zoom PERCENT] [FILE…]

OPTIONS:
  --control FILE      opt-in authenticated loopback control; writes private endpoint JSON
  --version           print the version
  -h, --help          print this help
";

    /// The default window: content size in points, and the smallest (the desktop app's).
    const DEFAULT_SIZE: [f32; 2] = [1440.0, 900.0];
    const MIN_SIZE: [f32; 2] = [640.0, 420.0];

    /// The settings folder: `~/config/settings/PdfCraft`, beside other applications'. A copy
    /// started without `HOME` (by a server, say) uses Haiku's one home folder.
    fn config_dir() -> Option<PathBuf> {
        let home = std::env::var_os("HOME").filter(|h| !h.is_empty()).map_or_else(|| PathBuf::from("/boot/home"), PathBuf::from);
        Some(home.join("config/settings/PdfCraft"))
    }

    /// Runs without preferences (`PDFCRAFT_NO_PREFS`, agents' test runs) neither read nor
    /// write them, and keep no log file.
    fn prefs_enabled() -> bool {
        std::env::var_os("PDFCRAFT_NO_PREFS").is_none()
    }

    fn log_dir() -> Option<PathBuf> {
        Some(config_dir()?.join("logs"))
    }

    /// Persist the upstream application settings atomically.
    pub fn save_prefs(app: &PdfCraftApp) {
        if prefs_enabled()
            && let Some(dir) = config_dir()
            && let Err(e) = std::fs::create_dir_all(&dir).and_then(|()| {
                pdfcraft_platform::staging::write_atomically(
                    &dir.join("ui.json"),
                    app.persist().as_bytes(),
                    pdfcraft_platform::staging::StagingName::TagThenSuffix,
                )
            })
        {
            log::warn!("saving preferences: {e}");
        }
    }

    fn build_sha() -> &'static str {
        option_env!("PDFCRAFT_BUILD_SHA").map(|s| s.get(..8).unwrap_or(s)).unwrap_or("dev")
    }

    struct Args {
        files: Vec<String>,
        options: Vec<(String, String)>,
        control: Option<String>,
    }
    fn args() -> Args {
        let mut out = Args { files: Vec::new(), options: Vec::new(), control: None };
        let mut args = std::env::args().skip(1);
        while let Some(arg) = args.next() {
            match arg.as_str() {
                "--version" | "-V" => {
                    println!("PdfCraft {} ({}) for air/OS", env!("CARGO_PKG_VERSION"), build_sha());
                    std::process::exit(0);
                }
                "--help" | "-h" => {
                    println!("{USAGE}");
                    std::process::exit(0);
                }
                "--control" => {
                    out.control = args.next();
                    if out.control.is_none() {
                        let _ = std::io::Write::write_all(&mut std::io::stderr(), b"--control requires a file path\n");
                        std::process::exit(2);
                    }
                }
                "--new-window" => {}
                option if option.starts_with("--") => {
                    if let Some(value) = args.next() {
                        out.options.push((option.trim_start_matches("--").into(), value));
                    }
                }
                _ => out.files.push(arg),
            }
        }
        out
    }

    fn interface(args: Args, scale: f32) {
        let ctx = egui::Context::default();
        ctx.set_embed_viewports(true);
        let mut app = PdfCraftApp::new();
        // First start (or settings from before Units existed): the region's unit, as the desktop app.
        app.units = pdfcraft_ui_egui::units::Unit::system();
        if prefs_enabled()
            && let Some(dir) = config_dir()
            && let Ok(json) = std::fs::read_to_string(dir.join("ui.json"))
        {
            app.restore(&json);
        }
        app.integrated_titlebar = false;
        app.motion.reduced = true;
        if let Err(e) = services::install() {
            log::error!("{e}");
            return;
        }
        if let Some(file) = args.control {
            let client = app.attach_control(&ctx);
            match pdfcraft_ui_egui::control::serve(client).and_then(|endpoint| write_control_file(&file, endpoint.port, &endpoint.token)) {
                Ok(()) => {}
                Err(e) => {
                    host::alert("PdfCraft", &format!("Could not start control channel: {e}"));
                    return;
                }
            }
        }
        if std::env::var_os("PDFCRAFT_NO_RECOVERY").is_none()
            && let Some(dir) = pdfcraft_ui_egui::RecoveryStore::default_dir()
        {
            app.enable_recovery(pdfcraft_ui_egui::RecoveryStore::new(dir));
        }
        // With Preferences ▸ Reopen last session on, last session's files come back first; files
        // named on the command line (Tracker's) open after them, in front, as on the desktop.
        app.reopen_last_files(&args.files);
        for f in &args.files {
            app.open_path(f);
        }
        for (key, value) in args.options {
            if let Err(e) = app.set_option(&key, &value) {
                log::warn!("--{key}: {e}");
            }
        }
        let mut shell = match shell::Shell::new(app, ctx, scale) {
            Ok(shell) => shell,
            Err(e) => {
                log::error!("{e}");
                host::alert("PdfCraft", &e);
                return;
            }
        };
        shell.run();
        shell.finish();
    }

    pub fn main() {
        // Before any thread starts: the locale variables come from Haiku's Locale preferences.
        host::export_locale();
        let logger = logging::install();
        std::panic::set_hook(Box::new(|info| {
            let trace = std::backtrace::Backtrace::capture();
            let report = if trace.status() == std::backtrace::BacktraceStatus::Captured {
                format!("internal error: {info}\n{trace}")
            } else {
                format!("internal error: {info}")
            };
            if log::log_enabled!(log::Level::Error) {
                log::error!("{report}");
            } else {
                let _ = std::io::Write::write_fmt(&mut std::io::stderr(), format_args!("PdfCraft: {report}\n"));
            }
        }));
        let args = args();
        if let Some(logger) = logger
            && let Some(dir) = log_dir().filter(|_| prefs_enabled())
        {
            match logger.attach_dir(&dir) {
                Ok(path) => log::info!("PdfCraft {} ({}) for air/OS, log file {}", env!("CARGO_PKG_VERSION"), build_sha(), path.display()),
                Err(e) => log::warn!("no log file: {e}"),
            }
        }
        if let Err(e) = host::init(services::SIGNATURE) {
            log::error!("{e}");
            std::process::exit(1);
        }
        if prefs_enabled() {
            services::claim_document_types();
        }
        // The rasteriser's workers: up to 8 (more cost more in waking than they save on a
        // frame), named for the system's process lists.
        let workers = std::thread::available_parallelism().map_or(4, |n| n.get().clamp(1, 8));
        if let Err(e) = rayon::ThreadPoolBuilder::new().num_threads(workers).thread_name(|i| format!("PdfCraft raster {i}")).build_global() {
            log::warn!("rasteriser threads: {e}");
        }
        let scale = host::ui_scale();
        let px = |v: f32| v * scale;
        host::open_window(&host::WindowSpec {
            title: "PdfCraft".into(),
            position: None,
            size: DEFAULT_SIZE.map(px),
            min_size: MIN_SIZE.map(px),
            zoomed: false,
        });
        let ui = std::thread::Builder::new().name("PdfCraft interface".into()).stack_size(16 << 20).spawn(move || {
            let ran = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| interface(args, scale)));
            if ran.is_err() {
                log::error!("the interface stopped after an internal error");
                host::alert("PdfCraft", "PdfCraft ran into an internal error and has to close. The log is in ~/config/settings/PdfCraft/logs.");
            }
            host::quit();
        });
        match ui {
            Ok(handle) => {
                host::run();
                let _ = handle.join();
            }
            Err(e) => log::error!("couldn't start the interface: {e}"),
        }
    }
}

#[cfg(target_os = "haiku")]
fn write_control_file(path: &str, port: u16, token: &str) -> std::io::Result<()> {
    use std::io::{ErrorKind, Write};
    let json = serde_json::json!({ "port": port, "token": token, "pid": std::process::id() }).to_string();
    let path = std::path::Path::new(path);
    let name = path.file_name().ok_or_else(|| std::io::Error::new(ErrorKind::InvalidInput, "not a file name"))?;
    // Unpredictable suffixes, so the name can't be planted in advance. `RandomState` is keyed from
    // the operating system's random source.
    let random = std::hash::RandomState::new();
    for i in 0..16u32 {
        let mut staged = std::ffi::OsString::from(".");
        staged.push(name);
        staged.push(format!(".{:016x}.tmp", std::hash::BuildHasher::hash_one(&random, i)));
        let staged = path.with_file_name(staged);
        let mut opts = std::fs::OpenOptions::new();
        opts.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            opts.mode(0o600);
        }
        let file = match opts.open(&staged) {
            Ok(f) => f,
            Err(e) if e.kind() == ErrorKind::AlreadyExists => continue,
            // Windows refuses `create_new` on a folder with "access denied"; the name is taken all
            // the same. A folder we can't write to, with nothing at the name, still fails here.
            Err(e) if e.kind() == ErrorKind::PermissionDenied && staged.symlink_metadata().is_ok() => continue,
            Err(e) => return Err(e),
        };
        // The block closes the file before it is renamed.
        let written = {
            let mut file = file;
            file.write_all(json.as_bytes())
        };
        let done = written.and_then(|()| std::fs::rename(&staged, path));
        if done.is_err() {
            let _ = std::fs::remove_file(&staged);
        }
        return done;
    }
    Err(std::io::Error::new(ErrorKind::AlreadyExists, "no free name for the control file's temporary copy"))
}
