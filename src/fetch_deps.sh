#!/usr/bin/env bash
# Fetch the third-party pieces the build needs into deps/ (gitignored):
#   deps/minhook           MinHook sources (MIT)                         pinned tag
#   deps/imgui             Dear ImGui sources (MIT)                      pinned tag
#   deps/streamline        Streamline SDK zip + the runtime DLLs we ship pinned by third_party/streamline/SDK_VERSION
# Idempotent: existing checkouts/files are reused. Run from anywhere; paths resolve from the repo root.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/deps"
mkdir -p "$DEPS"

MINHOOK_TAG="${MINHOOK_TAG:-v1.3.4}"
IMGUI_TAG="${IMGUI_TAG:-v1.93.0}"          # third_party/imgui_backends is patched from this tag; keep in step

SL_VERSION="$(sed -n 1p "$ROOT/third_party/streamline/SDK_VERSION" | tr -d '[:space:]')"
SL_SHA256="$(sed -n 2p "$ROOT/third_party/streamline/SDK_VERSION" | tr -d '[:space:]')"
SL_ZIP="$DEPS/streamline-sdk-v$SL_VERSION.zip"
SL_URL="https://github.com/NVIDIA-RTX/Streamline/releases/download/v$SL_VERSION/streamline-sdk-v$SL_VERSION.zip"

# The plugins the mod loads plus the DLSS models they drive. Everything else in the SDK is unused.
SL_DLLS=(sl.interposer.dll sl.common.dll sl.dlss.dll sl.dlss_g.dll sl.pcl.dll sl.reflex.dll nvngx_dlss.dll nvngx_dlssg.dll)
SL_LICENSES=(nvngx_dlss.license.txt reflex.license.txt)

clone_tag() { # repo url, tag, dir
    if [ -d "$3/.git" ]; then echo "[deps] $3 present"; return; fi
    echo "[deps] cloning $1 @ $2"
    GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --branch "$2" "$1" "$3"
}
clone_tag https://github.com/TsudaKageyu/minhook.git "$MINHOOK_TAG" "$DEPS/minhook"
clone_tag https://github.com/ocornut/imgui.git      "$IMGUI_TAG"   "$DEPS/imgui"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

if [ ! -f "$SL_ZIP" ] || [ "$(sha256_of "$SL_ZIP")" != "$SL_SHA256" ]; then
    echo "[deps] downloading Streamline SDK v$SL_VERSION"
    curl -fL --retry 4 --retry-delay 3 -o "$SL_ZIP.part" "$SL_URL"
    mv "$SL_ZIP.part" "$SL_ZIP"
fi
GOT="$(sha256_of "$SL_ZIP")"
if [ "$GOT" != "$SL_SHA256" ]; then
    echo "[deps] ERROR: $SL_ZIP sha256 $GOT != expected $SL_SHA256 (update third_party/streamline/SDK_VERSION if the pin moved)" >&2
    exit 1
fi
echo "[deps] Streamline SDK v$SL_VERSION verified"

OUT="$DEPS/streamline/bin"
rm -rf "$OUT" && mkdir -p "$OUT"
for f in "${SL_DLLS[@]}" "${SL_LICENSES[@]}"; do
    unzip -q -o -j "$SL_ZIP" "bin/x64/$f" -d "$OUT"
done
unzip -q -o -j "$SL_ZIP" "license.txt" -d "$DEPS/streamline"
mv -f "$DEPS/streamline/license.txt" "$DEPS/streamline/streamline.license.txt"
for f in "${SL_DLLS[@]}"; do [ -s "$OUT/$f" ] || { echo "[deps] ERROR: $f missing from SDK zip" >&2; exit 1; }; done
echo "[deps] runtime DLLs ready in $OUT:"; ls -la "$OUT"
