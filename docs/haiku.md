# PdfCraft for air/OS

Status: x86_64 port installed on the air/OS laptop; Tracker launch fix in package revision 3, 2026-10-18.

This fork runs PdfCraft's Rust PDF engine and egui interface in a native Be API
host adapted from the existing DeckCraft, GridCraft and WordCraft ports. It
retains the reader, annotations, page organization, editing, forms, save/undo,
PDF rendering and opt-in authenticated control channel. The command-line program
is included in the package.

The initial baseline is upstream PdfCraft `0ac9b840` (0.8.0). The native shell,
input tables, rasterizer and cross-build scripts were adapted from
[jmgasper/deckcraft at e59d0d0](https://github.com/jmgasper/deckcraft/tree/e59d0d0),
which incorporates the WordCraft and GridCraft ports. PdfCraft keeps its own
application logic and authenticated control protocol. The existing eframe host
calls the same frame hooks on other platforms.

Upstream is merged into `main` (never rebased). The 2026-10-29 merge of upstream
`222a1246` (0.15.0, egui 0.37, `crates/ui-egui` split into `app/`, `canvas/`,
`platform/`, `workflows/` and other modules) kept the same seams: every picker and
clipboard call goes through `crate::file_dialog` / `crate::clipboard` (rfd and
arboard elsewhere, `native_services` on air/OS), `PdfCraftApp` has inherent
`raw_input_hook`/`logic`/`ui` methods that the eframe `App` wraps, and eframe, rfd
and arboard are not dependencies on Haiku. Pickers can now open in a folder
(`set_directory`, used by Save As for the document's own folder), passed to
`BFilePanel::SetPanelDirectory`. Like the desktop app, the host starts with the
region's measurement unit and reopens the last session when that preference is on.
Package `0.15.0~airos1-1` is the first build of this merge.

The host uses `BApplication`, `BWindow`, `BView`, `BBitmap`, `BFilePanel`,
`BClipboard`, `BCursor`, `BLocaleRoster` and `BMimeType`. The interface is drawn with
Vulkan on a hardware GPU (Rendering, below), else by the siblings' CPU rasterizer with dirty
bands; both follow the air/OS display density.
Open, multiple-file, folder and save pickers use native asynchronous panels. Both
Control and Command map to editing shortcuts. Tracker launches and drops open PDFs, as tabs
of the one running copy.
The engine retains its own software PDF renderer and font handling.

Settings and logs live under `~/config/settings/PdfCraft`, with recovery in its
`Recovery` subfolder. Recovery locks use Haiku's `flock` through rustix, as in
WordCraft: one copy runs from each executable file (One running copy, below), but copies
started from different files, such as a test build beside the package, run at the same time,
and a running copy's recovery snapshot mustn't look like a crash's. The engine and UI forbid unsafe Rust; the host's documented FFI wrappers
(`src/host.rs`) and its raw Vulkan frame buffer import (`src/scanout.rs`) are the only exceptions. C++ is limited to the native Be API boundary.

## One running copy

PdfCraft is `B_SINGLE_LAUNCH` (`PdfCraft.rdef`), as the desktop app keeps one instance
(`single_instance.rs`, macOS's open events): one copy runs from each executable file. PDFs opened
in Tracker while it runs (double-click, Open With, drops on its icon) arrive in the running copy
as `B_REFS_RECEIVED`, and a launch from Terminal (`PdfCraft report.pdf`) hands its arguments to it
(`B_ARGV_RECEIVED`: relative paths resolved against the launch's folder; options and their values,
such as `--page 3` or `--control FILE`, are ignored, since the desktop app gives a launch with
options a window of its own, which the air/OS host can't) and ends before it touches the log
file, the preferences or the recovery folder (`BApplication` reports `B_ALREADY_RUNNING`). The
documents open as tabs, as the desktop app opens what another launch or Finder hands it
(`OsEvent::Open` → `open_path`): a document already open comes to the front instead, and the
window comes forward, un-minimized. A launch without documents (the Deskbar, `B_SILENT_RELAUNCH`)
only brings the window forward. Quitting (Command+Q, the Deskbar's Quit, File › Quit) asks about
every open document with unsaved changes in turn, as upstream's close guard does for the
desktop app's window, then quits. A test copy that must run beside the installed one runs from a
file of its own (and so does `--control`: a second launch of the same file with it only hands
over its documents).

## Rendering

The window is drawn with Vulkan when the machine has a hardware adapter, as PhotoCraft's and
LightCraft's air/OS hosts do (`apps/pdfcraft-haiku/src/gpu.rs`): egui's triangles are drawn by
egui-wgpu into a BGRA texture, and the presenter (`src/present.rs`, `damage.rs`, `scanout.rs`,
shared with PhotoCraft) compares it with what app_server shows, 64 px tile by tile, and copies
back only the tiles that changed. A frame drawn from exactly the same triangles, size and
textures as the one on the screen is not drawn again (egui repaints more often than its output
changes), so an idle or hovering window costs almost nothing. The software rasteriser
(`src/raster.rs`, dirty bands) remains for machines without a hardware Vulkan adapter, and takes
over without losing the open documents if the device is lost or reports an error.

`PDFCRAFT_RENDERER=auto|vulkan|cpu` picks the renderer (auto: Vulkan on a discrete or integrated
GPU, else the rasteriser; vulkan: fail at start without one). `PDFCRAFT_PRESENT=readback|full|direct`
picks how frames reach the window (`readback`, the default; `full`, the whole frame every time;
`direct`, experimental, straight into the screen's frame buffer through air/OS's
`VkImportScanoutMemoryHAIKU`; not the default because RADV on the Framework 13 loses the device a
few seconds after the import). wgpu's Vulkan backend is enabled for Haiku by the vendored
`wgpu`, `wgpu-core` and `wgpu-core-deps-windows-linux-android` 30.0.1 (`vendor/README.md`).

## Menu bar

The menus are in a native `BMenuBar` at the top of the window, as the macOS build has them in
AppKit's menu bar (`apps/pdfcraft-haiku/src/menu.rs`, `PcWindow::SetMenu` in `native/host.cpp`),
and the in-window menus hide while it is there, as on the Mac. It is craft-menu's `NativeMenu`
with a Haiku backend, installed with `PdfCraftApp::set_native_menu`: the app's own menu tree
(`menus.rs`) laid out for the build's target (`Target::current()`, the Linux layout on Haiku:
Quit last in File, Preferences last in Edit, About last in Help). Every frame upstream's
`sync_native_menu` lays it out again and the backend sends `host.cpp` a description of it when
it changed: rebuilt when its structure changed (`craft_menu::structure_key`/`window_key`: recent
files, open documents), else labels, enabled and marked updated in place. While a menu is open
the newest description waits for it to close (`MenusEnded`). Check and radio items show as
marked items; the open documents are listed last in Window. A chosen item comes back as its
command's key (`PC_MENU`) and runs as an in-window click does (`run_menu_command`), through
craft-menu's gate (greyed while a dialog is open; Edit's Copy, Cut, Paste and Undo act on a
focused text field).

Shortcuts show the Haiku way: craft-menu's `cmd` (and `ctrl`, which folds into it off macOS) is
the Command key, Alt by default (`keys.rs` maps Command and Control to egui's command), Shift
and Option (`alt`) as they are. The keys themselves still reach egui as keys, as with AppKit's
key equivalents: `PcWindow::DispatchMessage` hands every Command key to the interface before
`BWindow` looks for a menu shortcut, except Command+Q (quits through the close guard) and
Command+Escape (opens the menu bar). Shortcuts without Command (tool keys, bare F-keys, Delete)
and keys Haiku's menus have no glyph for (F-keys, Escape, End, Page Up/Down, Insert) aren't
shown: as window shortcuts they would take keys from text fields.

The canvas starts under the menu bar, whose height follows the menu font (and so the UI scale);
the window's sizes in egui's terms are the canvas's (`InnerSize`, the minimum size, the inner
rectangle reported to egui), so the window grows by the bar's height. Full screen hides the bar.
Direct presentation offsets its origin by the bar's height in device pixels (`sCanvasTop`);
if rounding makes that disagree with the frame size it falls back to app_server.
`PDFCRAFT_MENU=window` keeps the in-window menus instead. A `ViewportCommand::Close` from the app
(craft-quit's Quit) is now a close request for the next frame, as in egui-winit, so the
unsaved-changes prompt runs; it used to quit at once.

## Scrolling

A mouse wheel scrolls in whole steps, as before (lines from a wheel, which the page view
accelerates when spun fast and single-page view turns a page per notch). An air/OS touchpad's
scroll gesture (`B_MOUSE_WHEEL_CHANGED` with `be:device_subtype` 2, `be:wheel_precise_delta_*`
and `be:wheel_phase`, air/OS since 2026-10-27) reaches the interface as a trackpad's: its
unrounded steps become points, a step as far as a wheel notch scrolls (egui's line: 100 points
at Preferences' Normal scroll speed; the Touchpad preferences set how far the fingers move for a
step), with the gesture's start and end, so the page follows the fingers without the wheel's
acceleration, single-page view turns one page per swipe (a light touch doesn't), and egui's
lists treat it as a trackpad. The end of a gesture that arrives with its last movement is
held for the next frame (egui drops the deltas of a gesture that ended). air/OS's touchpad has
no pinch gesture: Command with the wheel or a two-finger scroll zooms (egui's zoom modifier), as
do the zoom buttons.

## Input methods

The canvas view is `B_INPUT_METHOD_AWARE`: an input method (Japanese, Chinese, Korean…) composes
in the focused text field itself: a form field, a comment or sticky note being written, Find, the
command palette. Its `B_INPUT_METHOD_EVENT`s become egui's IME events: `CHANGED` is the pre-edit
(its `be:selection` the active clause), `CHANGED` with `be:confirmed` the commit, and `STOPPED`
without one drops the pre-edit. A `LOCATION_REQUEST` is answered with the caret (egui's
`IMEOutput::cursor_rect`) for each character, converted to the screen by the canvas view, so
the candidate window opens by it, below the menu bar's height included. When the interface
interrupts the composition (a click elsewhere, the field losing the keyboard) the host sends the
input method `B_INPUT_METHOD_STOPPED` and ignores what it sends afterwards until it starts again;
the pre-edit stays in the field as typed text, as in a `BTextView`. On air/OS the interface has
no face for Chinese or Japanese yet (craft-fonts aren't bundled, and the system's Noto Sans CJK
isn't among upstream's installed fallback faces), so a CJK pre-edit shows "no glyph" boxes; a
form field shows `?` for characters its PDF font lacks and keeps the real text in its value.

## Updates and community links

air/OS installs and updates PdfCraft as a package (pkgman, SoftwareUpdater): Help has no Check
for updates item (the native and the in-window menus, `HELP` in `menus.rs`), and the command
(from the command palette or the control channel) shows the version and says SoftwareUpdater
installs newer versions instead of asking upstream's releases (`platform/updates.rs`). The
start-up check stays off. The community (Discord) links stay: they open the web browser on the
same app's community, and upstream has no switch for the host to hide them (Preferences' title
bar button setting is the user's).

## File panels, clipboard and Key Store

**File panels.** Each `add_filter` of a picker is a `BRefFilter` of its own (`MakeFilters` in
`host.cpp`), the first one showing; a pop-up menu of the filters' labels sits beside the
panel's folder menu (Haiku's `BFilePanel` has no file type menu), and choosing one switches the
panel's ref filter. A filter of `*` shows every file; folders always show.

**Clipboard.** Upstream copies and pastes plain text (egui's), PNG images (Create ▸ Clipboard,
snapshots) and file lists (Copy image file); it offers no rich text anywhere, so there is
nothing to map to `text/html` or Haiku's `text/plain` + `application/x-vnd.Be-text_run_array`
(styled text from StyledEdit pastes as its `text/plain`). Files go out the ways air/OS programs
paste them: as Tracker copies them (a ref `r<device>_<node>` and its mode `m<device>_<node>` =
copy, so Paste in a Tracker window copies the file there), as `refs`, as `text/uri-list` and as
their paths in `text/plain`. Where the interface pastes text, a clipboard without `text/plain`
gives the files on it (copied in Tracker) as their paths, else its `text/uri-list` (files as
paths, links as links), as a file manager's copy pastes on Linux (`src/clip.rs`).

**Key Store.** `os_key_store_ids` is on: digital IDs can be kept in air/OS's Key Store, as the
Keychain and the Windows certificate store keep them (`crates/ui-egui/src/tools/sign_ui/key_store.rs`).
Configure New Digital ID has "Keep it in the Key Store" for both a new ID and one from a file;
such an ID is in the keyring "Digital IDs" under `keystore:<SHA-256 of the certificate>`, named
after its holder, and is listed again from the Key Store at each signing ("Key Store · ").
Remove deletes it from the Key Store. The Key Store asks the user before PdfCraft reads the
keyring the first time (Allow once / Always). Unlike the Keychain, the Key Store only keeps data
and doesn't encrypt its database yet (`Keyring::_EncryptToFlatBuffer` in keystore_server is a
TODO), so the ID goes in as its PKCS #12, still sealed with its own password, with its
certificate beside it for the list; signing asks for the password as for a file. The
automation tools (`sign_document`) take `keychain:` and `windows:` IDs but not `keystore:` ones:
the engine below the interface has no access to the host's services.

## Build

```sh
bash tools/haiku/build.sh
bash tools/haiku/package.sh --no-build
```

The cross compiler, development sysroot and host packaging tools default to this
workspace's `/mnt/HaikuWork` toolchains, as in the sibling ports. Override
`HAIKU_SYSROOT`, `HAIKU_CROSS`, `HAIKU_HOST_TOOLS`, `RUSTUP_HOME`, `CARGO_HOME` and
`CARGO_TARGET_DIR` when needed. Rust must include `rust-src`; Haiku is a tier-3
Rust target, so the script builds std with `-Zbuild-std` and `RUSTC_BOOTSTRAP=1`.
The release retains panic unwinding for upstream's document error guards.

Outputs are `target/x86_64-unknown-haiku/release/PdfCraft`, `pdfcraft-cli` beside
it, and `artifacts/haiku/pdfcraft-<version>-x86_64.hpkg` (the version in `apps/pdfcraft-haiku/resources/PdfCraft.PackageInfo`). Install with
`pkgman install <package>`. The application appears in the Deskbar Applications
menu and registers for PDFs without replacing an existing preferred PDF viewer.

The executable's resources include `BEOS:TYPE = application/x-vnd.Be-elfexecutable`.
`resattr` preserves this attribute in the package, allowing BRoster to pre-register
the process and deliver Tracker's initial `B_REFS_RECEIVED`. Without it, a Tracker
launch can show only the welcome screen even though direct document launches and
file refs sent to a running instance work. Post-install MIME sniffing cannot repair
the missing attribute on the read-only package volume.

```sh
PdfCraft /boot/home/example.pdf
PdfCraft --control /boot/home/pdfcraft-control.json /boot/home/example.pdf
pdfcraft-cli ui --control /boot/home/pdfcraft-control.json inspect
```

`PDFCRAFT_DEVICE_SCALE` overrides display density; `PDFCRAFT_NO_PREFS` and
`PDFCRAFT_NO_RECOVERY` disable settings and recovery for isolated validation.
Control and MCP remain opt-in; no port is opened during ordinary startup.

## Validation

Validated on the air/OS x86_64 laptop (Haiku R1/beta6, hrev60206+) using synthetic
PDFs and the repository's attributed form fixture. The installed package and CLI
were exercised through the opt-in control channel and native Be scripting:

- Open and render a three-page PDF at the laptop's 1.75 display density; visually
  inspect the rendered page and UI. Search, select/copy text through BClipboard,
  and create a PDF from clipboard text.
- Rotate pages, undo, save in place and Save As; inspect saved PDFs with the CLI.
  The CLI also rendered PNG, extracted text and applied page/metadata edits.
- Fill a form, save it and verify the field text from the resulting PDF.
- Open multiple PDFs via native file panels and Tracker-style file refs; cancel panels, enter
  and leave fullscreen, and cancel the unsaved-changes quit prompt.
- Verify native panel titles and placement on a second monitor, PDF MIME tagging,
  the Deskbar link, and preservation of the existing preferred PDF application.
- Revision 3: open a synthetic PDF through Tracker (`open <file>`), through
  Tracker's Open With handler, and through BRoster's initial refs message. Each
  opens the document in the editor; the same Tracker regression on revision 2
  opened only the welcome screen. Check the installed executable's `BEOS:TYPE`
  with `catattr`, rather than inferring launch behavior from a running instance.
- Persist and restore the dark theme. With isolated recovery directories, verify
  a second instance ignores live snapshots, then force-stop the test instance and
  recover/save its unsaved changes on relaunch.

`tools/haiku/input-probe.cpp` (built on air/OS: `g++ -o input-probe input-probe.cpp -lbe`)
sends a running copy the input a test machine can't make: an input method's composition (`ime`,
`ime-cancel`), a pointer position, a touchpad gesture or wheel turn; it also lists the
clipboard's types and opens files with a given copy as Tracker does (`open`). Its pointer moves
lose to the real pointer when that is over the window; place the pointer with the control
channel's `ui.move` instead. egui sends input methods nothing while the window isn't active:
activate it (`hey <team> set Active of Window 0 to true`) right before an `ime` run.

Host checks (2026-10-18):

- `cargo test -j 2 -p pdfcraft-haiku -p pdfcraft-ui-egui --lib --tests`: 1,294 passed,
  five existing private-fixture/manual-probe tests ignored, zero failures.
- `cargo test -p xtask -j 2`: 49 passed.
- Scoped Linux `cargo clippy --all-targets -- -D warnings` passes for the host, UI
  and xtask. `cargo fmt --all --check`, `cargo xtask assets` and `cargo xtask layers`
  pass (38 crates, no layering violations).
- `cargo check -p pdfcraft-ui-egui --target wasm32-unknown-unknown` passes with
  three existing dead-code warnings in editing, units and update checks.

The build and package scripts complete on Linux with the workspace toolchain.
The package includes the attribution manifest and embedded-asset licence texts.
Native clippy passes with `-D warnings -A clippy::missing_const_for_thread_local`;
the latter is the same Haiku target/toolchain false positive used by the sibling
ports (upstream thread-local initializers). It does not disable other diagnostics.

## Initial limitations

- PDF pages are still rendered by the engine on the CPU; only the interface is drawn with Vulkan.
- Direct native printing is not wired up yet; print-ready PDF export remains available.
- Native accessibility nodes and persistent window geometry are not implemented.
- Clipboard images currently use PNG, rather than archived native BBitmap data.
- Optional OCR models and craft-fonts are not bundled. Upstream errors describe missing models.
- Upstream's alpha PDF format and editing limitations still apply.

The fork removes restricted ArtCraft trademark graphics and screenshots containing
them. Its native folded-page icon is contributor-original, generated by
`tools/haiku/make-icon.py` and listed in `ATTRIBUTION.toml`.

## Revision history

- 2026-10-29: package `0.15.0~airos1-2` draws the window with Vulkan (Rendering). Framework 13
  (Radeon 760M, 175 %, speex manual, 30 samples each, against the rasteriser in the same
  build): PdfCraft CPU 1.49 s -> 0.32 s, app_server about the same (0.10 s); hover 3.5 -> 5.4 ms
  and Page Down 11.7 -> 13.6 ms per request (the two GPU round trips of the changed-tile
  readback). Full readback was slower (9.6 / 24.7 ms).
- 2026-10-18: package revision 3 includes executable MIME metadata for Tracker's
  initial document-open message. Reproduced the failure on revision 2 and verified
  Tracker and Open With launches after installing revision 3.
- 2026-10-18: initial native host and cross-build integration.

## Native application icon (2026-10-26)

The air/OS icon is an original PDF pages, format label and annotation pencil, licensed under
MIT OR Apache-2.0. Its 64-unit vector drawing follows the
[Haiku icon guidelines](https://www.haiku-os.org/development/icon-guidelines):
vertical edges stay upright in perspective, the main light comes from the top
left, the right faces are darker, a floor shadow extends to the right, and the
silhouette has a two-unit black outline (one pixel at 32px). Broad details are
intended to remain legible at 16px and 32px. No fonts or third-party artwork are used.

Rebuild both the native HVIF and its SVG preview from this checkout:

```sh
python3 tools/haiku/make-icon.py
```

`apps/pdfcraft-haiku/resources/PdfCraft.hvif` is the packaged icon;
`apps/pdfcraft-haiku/resources/PdfCraft.svg` exports the same geometry for inspection.
The existing `.rdef` imports the HVIF, and `tools/haiku/package.sh` embeds it in
the executable's resources. Tracker and Deskbar therefore use the vector icon.
The upstream icons for other platforms remain separate. Native previews can be
rendered on Haiku with `BIconUtils::GetVectorIcon`; review at 16, 32, 64 and 128px
on light and dark backgrounds before changing the drawing.

Package version `0.8.0~airos1-4` includes this icon update.
