#!/usr/bin/env bash
# Cross-compiles PdfCraft for air/OS (Haiku x86_64): target/x86_64-unknown-haiku/release/PdfCraft.
# See docs/haiku.md for the toolchain (rustup stable with rust-src, the Haiku sysroot).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. tools/haiku/env.sh
PDFCRAFT_BUILD_SHA=${PDFCRAFT_BUILD_SHA:-$(git rev-parse HEAD 2>/dev/null || echo dev)} \
	cargo build --locked --release --target x86_64-unknown-haiku -Zbuild-std=std,panic_abort -j "${JOBS:-4}" -p pdfcraft-haiku -p pdfcraft-cli "$@"
