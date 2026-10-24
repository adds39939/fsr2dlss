#!/usr/bin/env bash
# Build amd_fidelityfx_dx12.dll (the fsr2dlss proxy) with MinGW-w64 and assemble the release package
# in dist/. Used by CI (MSYS2 MINGW64) and works from any bash with a MinGW-w64 g++ on PATH.
#
#   src/fetch_deps.sh && src/build.sh
#
# Env overrides: CXX / CC (default g++ / gcc), e.g. CXX=x86_64-w64-mingw32-g++ for cross-compiling.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/deps"
OBJ="$ROOT/build/obj"
DIST="$ROOT/dist"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
[ -d "$DEPS/minhook" ] && [ -d "$DEPS/imgui" ] && [ -d "$DEPS/streamline/bin" ] || { echo "run src/fetch_deps.sh first" >&2; exit 1; }

mkdir -p "$OBJ"
cd "$ROOT"

CFLAGS=(-O2 -DNDEBUG)
CXXFLAGS=(-O2 -std=c++17 -DNDEBUG -DNOMINMAX -Wno-deprecated-declarations
          -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS
          -I third_party/streamline/include -I "$DEPS/minhook/include"
          -I third_party/imgui_backends -I "$DEPS/imgui" -I "$DEPS/imgui/backends")

echo "[build] MinHook"
for f in buffer hook trampoline; do
    "$CC" "${CFLAGS[@]}" -c -I "$DEPS/minhook/include" -I "$DEPS/minhook/src" "$DEPS/minhook/src/$f.c" -o "$OBJ/mh_$f.o"
done
for f in hde32 hde64; do
    "$CC" "${CFLAGS[@]}" -c -I "$DEPS/minhook/include" -I "$DEPS/minhook/src" "$DEPS/minhook/src/hde/$f.c" -o "$OBJ/mh_$f.o"
done

echo "[build] Dear ImGui"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    "$CXX" "${CXXFLAGS[@]}" -c "$DEPS/imgui/$f.cpp" -o "$OBJ/$f.o"
done
"$CXX" "${CXXFLAGS[@]}" -c "$DEPS/imgui/backends/imgui_impl_win32.cpp" -o "$OBJ/imgui_impl_win32.o"
"$CXX" "${CXXFLAGS[@]}" -c third_party/imgui_backends/imgui_impl_dx12.cpp -o "$OBJ/imgui_impl_dx12.o"

echo "[build] fsr2dlss"
"$CC" "${CFLAGS[@]}" -c src/mingw_compat.c -o "$OBJ/mingw_compat.o"
for f in proxy slbridge nvhook config overlay dxgihook; do
    "$CXX" "${CXXFLAGS[@]}" -Wall -Wno-unused-function -c "src/$f.cpp" -o "$OBJ/$f.o"
done

echo "[build] link"
"$CXX" -shared -static -static-libgcc -static-libstdc++ -s -Wl,--gc-sections \
    -o "$ROOT/build/amd_fidelityfx_dx12.dll" \
    "$OBJ"/proxy.o "$OBJ"/slbridge.o "$OBJ"/nvhook.o "$OBJ"/config.o "$OBJ"/overlay.o "$OBJ"/dxgihook.o "$OBJ"/mingw_compat.o \
    "$OBJ"/imgui.o "$OBJ"/imgui_draw.o "$OBJ"/imgui_tables.o "$OBJ"/imgui_widgets.o "$OBJ"/imgui_impl_win32.o "$OBJ"/imgui_impl_dx12.o \
    "$OBJ"/mh_buffer.o "$OBJ"/mh_hook.o "$OBJ"/mh_trampoline.o "$OBJ"/mh_hde32.o "$OBJ"/mh_hde64.o \
    src/exports.def -lkernel32 -luser32 -lgdi32 -limm32 -ldwmapi -ld3dcompiler -ld3d12 -ldxgi
ls -la "$ROOT/build/amd_fidelityfx_dx12.dll"

echo "[build] package -> $DIST"
rm -rf "$DIST" && mkdir -p "$DIST/streamline" "$DIST/licenses"
cp "$ROOT/build/amd_fidelityfx_dx12.dll" "$DIST/"
cp fsr2dlss.ini.example "$DIST/fsr2dlss.ini"
cp docs/INSTALL.md "$DIST/"
cp "$DEPS/streamline/bin/"*.dll "$DIST/streamline/"
cp "$DEPS/streamline/bin/"*.license.txt "$DIST/licenses/"
cp "$DEPS/streamline/streamline.license.txt" "$DIST/licenses/"
cp "$DEPS/imgui/LICENSE.txt" "$DIST/licenses/imgui.license.txt"
cp "$DEPS/minhook/LICENSE.txt" "$DIST/licenses/minhook.license.txt"

# Record what went into the package (FileVersion from each DLL's version resource).
SL_VERSION="$(sed -n 1p third_party/streamline/SDK_VERSION | tr -d '[:space:]')"
dll_version() { strings -el "$1" 2>/dev/null | grep -A1 -x FileVersion | sed -n 2p | tr ',' '.' | tr -d '[:space:]'; }
{
    echo "fsr2dlss package"
    echo "streamline_sdk=$SL_VERSION"
    for f in "$DIST/streamline/"*.dll; do
        v="$(dll_version "$f")"
        echo "$(basename "$f")=${v:-unknown}"
    done
} > "$DIST/VERSIONS.txt"
cat "$DIST/VERSIONS.txt"
ls -R "$DIST"
