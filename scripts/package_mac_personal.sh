#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build a self-contained personal macOS .app bundle (no Homebrew runtime dependency,
# bundles local game assets for personal use on your own Mac; see NOTICE.md — never
# distribute builds containing game assets).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

ASSETS_DIR="${RICH4_ASSETS:-$REPO_DIR/game-assets}"
OUT_APP="${1:-$REPO_DIR/artifacts/大富翁 4.app}"
BUILD_DIR="$REPO_DIR/build/mac-native"

if [[ ! -f "$ASSETS_DIR/Data.mkf" && ! -f "$ASSETS_DIR/data.mkf" ]]; then
  echo "Missing game assets in $ASSETS_DIR (Data.mkf not found)" >&2
  exit 1
fi

cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DRICH4_SANITIZERS=OFF -DRICH4_BUILD_VIEWER=OFF -DRICH4_BUILD_GAME=ON -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" --target rich4-native --parallel

mkdir -p "$(dirname "$OUT_APP")"
rm -rf "$OUT_APP"
cp -R "$BUILD_DIR/rich4-native.app" "$OUT_APP"

BIN="$OUT_APP/Contents/MacOS/rich4-native"
FW_DIR="$OUT_APP/Contents/Frameworks"
RES_DIR="$OUT_APP/Contents/Resources"
mkdir -p "$FW_DIR" "$RES_DIR/game-assets"

# Recursively bundle all non-system dylibs and rewrite load paths to @executable_path/../Frameworks.
python3 - "$BIN" "$FW_DIR" <<'PY'
import os, shutil, subprocess, sys
from pathlib import Path

main_bin = Path(sys.argv[1]).resolve()
fw_dir = Path(sys.argv[2]).resolve()

def otool_deps(path: Path):
    out = subprocess.check_output(["otool", "-L", str(path)], text=True)
    deps = []
    for line in out.splitlines()[1:]:
        dep = line.strip().split(" (", 1)[0]
        if dep:
            deps.append(dep)
    return deps

def otool_rpaths(path: Path):
    out = subprocess.check_output(["otool", "-l", str(path)], text=True)
    rpaths = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line:
            for j in range(i + 1, min(len(lines), i + 6)):
                s = lines[j].strip()
                if s.startswith("path "):
                    rpaths.append(s.split(" ", 2)[1])
                    break
    return rpaths

def is_system(dep: str) -> bool:
    return dep.startswith("/System/") or dep.startswith("/usr/lib/")

def resolve_dep(dep: str, referrer: Path) -> Path:
    if dep.startswith("@rpath/"):
        rel = dep[len("@rpath/"):]
        candidates = []
        for rp in otool_rpaths(referrer) + otool_rpaths(main_bin):
            rp_exp = rp.replace("@loader_path", str(referrer.parent)).replace("@executable_path", str(main_bin.parent))
            candidates.append(Path(rp_exp) / rel)
        candidates += [Path("/opt/homebrew/lib") / rel, Path("/usr/local/lib") / rel]
        for c in candidates:
            if c.exists():
                return c.resolve()
        raise RuntimeError(f"Cannot resolve {dep} referenced by {referrer}")
    if dep.startswith("@loader_path/"):
        return (referrer.parent / dep[len("@loader_path/"):]).resolve()
    if dep.startswith("@executable_path/"):
        return (main_bin.parent / dep[len("@executable_path/"):]).resolve()
    p = Path(dep)
    if p.exists():
        return p.resolve()
    raise RuntimeError(f"Missing dependency {dep} referenced by {referrer}")

bundled = {}  # realpath -> dest_name
queue = [(main_bin, main_bin, True)]
seen_files = set()

while queue:
    target_file, orig_file, is_main = queue.pop(0)
    if target_file in seen_files:
        continue
    seen_files.add(target_file)
    for dep in otool_deps(target_file):
        if is_system(dep):
            continue
        if not is_main and dep.endswith("/" + target_file.name) and dep.startswith("@"):
            continue
        if not is_main and Path(dep).resolve() == orig_file.resolve():
            continue
        src = resolve_dep(dep, orig_file)
        if src == orig_file.resolve():
            continue
        name = src.name
        if src not in bundled:
            dest = fw_dir / name
            if not dest.exists():
                shutil.copy2(src, dest)
                os.chmod(dest, 0o755)
                subprocess.check_call(["install_name_tool", "-id", f"@loader_path/{name}", str(dest)])
            bundled[src] = name
            queue.append((dest, src, False))
        new_ref = f"@executable_path/../Frameworks/{bundled[src]}" if is_main else f"@loader_path/{bundled[src]}"
        if dep != new_ref:
            subprocess.check_call(["install_name_tool", "-change", dep, new_ref, str(target_file)])

# Verify zero non-system absolute paths remain.
for binary in [main_bin] + sorted(fw_dir.glob("*.dylib")):
    for dep in otool_deps(binary):
        if not is_system(dep) and not dep.startswith("@executable_path/") and not dep.startswith("@loader_path/"):
            raise RuntimeError(f"Unresolved external dependency in {binary.name}: {dep}")
print(f"Bundled {len(bundled)} frameworks cleanly into {fw_dir}")
PY

# Copy required game assets (skipping unneeded placeholder .avi videos and .exe).
shopt -s nullglob nocaseglob
for f in "$ASSETS_DIR"/*.mkf "$ASSETS_DIR"/*.wav "$ASSETS_DIR"/*.mid "$ASSETS_DIR"/Midi.txt; do
  cp -f "$f" "$RES_DIR/game-assets/"
done
if [[ -d "$ASSETS_DIR/Music" ]]; then
  cp -R "$ASSETS_DIR/Music" "$RES_DIR/game-assets/"
fi
shopt -u nullglob nocaseglob

# Generate native macOS .icns icon from rich4.exe when available (pure PNG-in-ICNS, crisp pixel scaling).
if [[ -f "$ASSETS_DIR/rich4.exe" ]]; then
  python3 - "$REPO_DIR/scripts" "$ASSETS_DIR/rich4.exe" "$RES_DIR/AppIcon.icns" <<'PY'
import struct, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from make_android_icon import BACKGROUND, decode_bmp_icon, foreground, png, pe_icon
icon = decode_bmp_icon(pe_icon(Path(sys.argv[2]).read_bytes()))
chunks = []
for tag, size, scale in [(b"ic07", 128, 3), (b"ic08", 256, 6), (b"ic09", 512, 12), (b"ic10", 1024, 24)]:
    canvas = foreground(icon, size, scale, background=BACKGROUND)
    canvas = [[p[:3] + (255,) for p in row] for row in canvas]
    data = png(canvas)
    chunks.append(tag + struct.pack(">I", len(data) + 8) + data)
body = b"".join(chunks)
Path(sys.argv[3]).write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
PY
  /usr/libexec/PlistBuddy -c "Delete :CFBundleIconFile" "$OUT_APP/Contents/Info.plist" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$OUT_APP/Contents/Info.plist"
fi

/usr/libexec/PlistBuddy -c "Delete :CFBundleDisplayName" "$OUT_APP/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :CFBundleDisplayName string 大富翁 4" "$OUT_APP/Contents/Info.plist"

codesign --force --deep --sign - "$OUT_APP"
echo "Packaged personal macOS app: $OUT_APP"
du -sh "$OUT_APP"
