#!/bin/sh
# Cross-compile aiquota for Kindle and assemble the deploy tarball.
# Run inside WSL. Stages fail loudly; prerequisites:
#   - koxtoolchain kindlepw2 at ~/x-tools/arm-kindlepw2-linux-gnueabi
#   - full font at kindle/fonts/NotoSansSC.ttf
#   - python venv with fonttools at ~/fontenv
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

die() {
    echo "build.sh: $1" >&2
    exit 1
}

# --- toolchain -------------------------------------------------------------
export PATH="$HOME/x-tools/arm-kindlepw2-linux-gnueabi/bin:$PATH"
command -v arm-kindlepw2-linux-gnueabi-gcc >/dev/null 2>&1 || \
    die "arm-kindlepw2-linux-gnueabi-gcc not found; run the koxtoolchain step first (./gen-tc.sh kindlepw2)"

# --- build -----------------------------------------------------------------
make -C "$ROOT/kindle"
[ -f "$ROOT/kindle/extension/bin/aiquota" ] || \
    die "make finished but kindle/extension/bin/aiquota is missing"

# --- host unit tests ---------------------------------------------------------
# The http cases need the fixture server from tests/test_main.c's header:
# 127.0.0.1:18080 serving kindle/tests (index.txt). A busy port means no
# fixture, and failing the build is the right outcome then.
PYTHON=$(command -v python3 || true)
[ -n "$PYTHON" ] || PYTHON="$HOME/fontenv/bin/python3"
[ -x "$(command -v "$PYTHON" 2>/dev/null || echo "$PYTHON")" ] || die "python3 not found"
command -v gcc >/dev/null 2>&1 || die "host gcc not found (make test needs it)"
"$PYTHON" -m http.server 18080 --bind 127.0.0.1 -d "$ROOT/kindle/tests" >/dev/null 2>&1 &
TEST_SRV=$!
trap '[ "$TEST_SRV" ] && kill "$TEST_SRV" 2>/dev/null' EXIT
sleep 1
make -C "$ROOT/kindle" test || die "host unit tests failed"
kill "$TEST_SRV" 2>/dev/null
TEST_SRV=

# --- font subset -----------------------------------------------------------
[ -f "$ROOT/kindle/fonts/NotoSansSC.ttf" ] || \
    die "kindle/fonts/NotoSansSC.ttf not found"
[ -x "$HOME/fontenv/bin/pyftsubset" ] || \
    die "$HOME/fontenv/bin/pyftsubset not found; create the fonttools venv first"

grep -hoP '[\x{0080}-\x{FFFF}]' "$ROOT"/kindle/src/*.c "$ROOT"/kindle/src/*.h | sort -u | tr -d '\n' \
    > "$ROOT/kindle/fonts/glyphs.txt"
[ -s "$ROOT/kindle/fonts/glyphs.txt" ] || \
    die "glyph extraction produced an empty file"
"$HOME/fontenv/bin/pyftsubset" "$ROOT/kindle/fonts/NotoSansSC.ttf" \
    --text-file="$ROOT/kindle/fonts/glyphs.txt" --unicodes=U+0020-007E \
    --output-file="$ROOT/kindle/fonts/NotoSansSC-subset.ttf" --no-hinting --desubroutinize
cp "$ROOT/kindle/fonts/NotoSansSC-subset.ttf" "$ROOT/kindle/extension/bin/NotoSansSC-subset.ttf"

# --- verify binary ---------------------------------------------------------
command -v file >/dev/null 2>&1 || \
    die "'file' not found; cannot verify the ARM binary"
command -v readelf >/dev/null 2>&1 || \
    die "'readelf' not found; cannot verify static libfbink linkage"
file "$ROOT/kindle/extension/bin/aiquota" | grep -q ARM || \
    die "kindle/extension/bin/aiquota is not an ARM binary"
if readelf -d "$ROOT/kindle/extension/bin/aiquota" | grep NEEDED | grep -qi fbink; then
    die "aiquota dynamically links libfbink; libfbink must be linked statically"
fi

# --- assemble deploy tree ---------------------------------------------------
DIST="$ROOT/dist/aiquota"
rm -rf "$DIST"
mkdir -p "$DIST/extensions/aiquota/bin" "$DIST/documents"
cp "$ROOT/kindle/extension/config.xml" "$DIST/extensions/aiquota/config.xml"
cp "$ROOT/kindle/extension/menu.json" "$DIST/extensions/aiquota/menu.json"
cp "$ROOT/kindle/extension/config" "$DIST/extensions/aiquota/config"
cp "$ROOT/kindle/extension/icon.png" "$DIST/extensions/aiquota/icon.png"
cp "$ROOT/kindle/extension/bin/aiquota.sh" "$DIST/extensions/aiquota/bin/aiquota.sh"
cp "$ROOT/kindle/extension/bin/watchdog.sh" "$DIST/extensions/aiquota/bin/watchdog.sh"
cp "$ROOT/kindle/extension/bin/restore.sh" "$DIST/extensions/aiquota/bin/restore.sh"
cp "$ROOT/kindle/extension/bin/aiquota" "$DIST/extensions/aiquota/bin/aiquota"
cp "$ROOT/kindle/fonts/NotoSansSC-subset.ttf" "$DIST/extensions/aiquota/bin/NotoSansSC-subset.ttf"
cp "$ROOT/kindle/scriptlet/ai-quota.sh" "$DIST/documents/ai-quota.sh"

tar -czf "$ROOT/dist/aiquota-kindle.tar.gz" -C "$DIST" .

echo "Built $ROOT/dist/aiquota-kindle.tar.gz"
echo "Deploy: copy the tarball to the Kindle USB root and extract there (tar -xzf aiquota-kindle.tar.gz)."
