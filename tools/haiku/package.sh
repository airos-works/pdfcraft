#!/usr/bin/env bash
# Cross-build and package entirely on Linux with Haiku's host tools.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. tools/haiku/env.sh
TOOLS=${HAIKU_HOST_TOOLS:-/mnt/HaikuWork/build/fw13-x86_64/objects/linux/x86_64/release/tools}
RES="$ROOT/apps/pdfcraft-haiku/resources"
OUT="$ROOT/artifacts/haiku"
VERSION=$(awk '$1 == "version" { print $2; exit }' "$RES/PdfCraft.PackageInfo")
if [[ ${1:-} != --no-build ]]; then bash tools/haiku/build.sh; fi
mkdir -p "$OUT"
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/pdfcraft-pkg-XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
DOCS="$STAGE/documentation/packages/pdfcraft"
mkdir -p "$STAGE/bin" "$STAGE/apps" "$STAGE/data/deskbar/menu/Applications" "$STAGE/boot/post-install" "$DOCS"
"$HAIKU_CROSS-strip" -o "$STAGE/apps/PdfCraft" "$CARGO_TARGET_DIR/x86_64-unknown-haiku/release/PdfCraft"
"$HAIKU_CROSS-strip" -o "$STAGE/bin/pdfcraft-cli" "$CARGO_TARGET_DIR/x86_64-unknown-haiku/release/pdfcraft-cli"
(cd "$RES" && "$TOOLS/rc/rc" -o "$STAGE/PdfCraft.rsrc" PdfCraft.rdef)
"$TOOLS/xres" -o "$STAGE/apps/PdfCraft" "$STAGE/PdfCraft.rsrc"
"$TOOLS/resattr/resattr" -O -o "$STAGE/apps/PdfCraft" "$STAGE/PdfCraft.rsrc"
rm "$STAGE/PdfCraft.rsrc"
cp "$RES/PdfCraft.PackageInfo" "$STAGE/.PackageInfo"
cp "$RES/pdfcraft-post-install.sh" "$STAGE/boot/post-install/pdfcraft.sh"
chmod 755 "$STAGE/boot/post-install/pdfcraft.sh" "$STAGE/apps/PdfCraft"
cp README.md LICENSE-MIT LICENSE-APACHE NOTICE ATTRIBUTION.md ATTRIBUTION.toml docs/haiku.md "$DOCS/"
# Keep the embedded fonts, icons and colour-profile licence texts with the binary.
while IFS= read -r license; do
    mkdir -p "$DOCS/licenses/$(dirname -- "$license")"
    cp "$license" "$DOCS/licenses/$license"
done < <(awk -F '"' '/^licence_file = / { print $2 }' ATTRIBUTION.toml | sort -u)
mkdir -p "$DOCS/licenses/epaint_default_fonts" "$DOCS/licenses/hayro-cmap"
cp "$CARGO_HOME"/registry/src/*/epaint_default_fonts-0.36.2/fonts/*.txt "$DOCS/licenses/epaint_default_fonts/"
cp "$CARGO_HOME"/registry/src/*/hayro-cmap-0.1.0/assets/LICENSE.txt "$DOCS/licenses/hayro-cmap/"
ln -s ../../../../apps/PdfCraft "$STAGE/data/deskbar/menu/Applications/PdfCraft"
"$TOOLS/package/package" create -C "$STAGE" "$OUT/pdfcraft-$VERSION-x86_64.hpkg"
