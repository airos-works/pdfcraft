# PdfCraft for air/OS

Status: initial x86_64 port built and installed on the air/OS laptop, 2026-10-18.

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

The host uses `BApplication`, `BWindow`, `BView`, `BBitmap`, `BFilePanel`,
`BClipboard`, `BCursor`, `BLocaleRoster` and `BMimeType`. The interface is drawn by
the siblings' CPU rasterizer with dirty bands and air/OS display density support.
Open, multiple-file, folder and save pickers use native asynchronous panels. Both
Control and Command map to editing shortcuts. Tracker launches and drops open PDFs.
The engine retains its own software PDF renderer and font handling.

Settings and logs live under `~/config/settings/PdfCraft`, with recovery in its
`Recovery` subfolder. Recovery locks use Haiku's `flock` through rustix, as in
WordCraft. The engine and UI forbid unsafe Rust; the host's documented FFI wrappers
are the only exception. C++ is limited to the native Be API boundary.

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
it, and `artifacts/haiku/pdfcraft-0.8.0~airos1-2-x86_64.hpkg`. Install with
`pkgman install <package>`. The application appears in the Deskbar Applications
menu and registers for PDFs without replacing an existing preferred PDF viewer.

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
- Persist and restore the dark theme. With isolated recovery directories, verify
  a second instance ignores live snapshots, then force-stop the test instance and
  recover/save its unsaved changes on relaunch.

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

- CPU rendering; complex PDFs and large HiDPI windows can be expensive.
- Direct native printing is not wired up yet; print-ready PDF export remains available.
- IME composition, native accessibility nodes and persistent window geometry are not implemented.
- Clipboard images currently use PNG, rather than archived native BBitmap data.
- Optional OCR models and craft-fonts are not bundled. Upstream errors describe missing models.
- Upstream's alpha PDF format and editing limitations still apply.

The fork removes restricted ArtCraft trademark graphics and screenshots containing
them. Its native folded-page icon is contributor-original, generated by
`tools/haiku/make-icon.py` and listed in `ATTRIBUTION.toml`.

## Revision history

- 2026-10-18: initial native host and cross-build integration.
