# Build fsr2dlss (amd_fidelityfx_dx12.dll). Requires MinGW-w64 g++ and MinHook.
# Local dev uses the toolchain under $root\mingw64; CI uses MSYS2 (see .github/workflows/build.yml).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot           # repo root
$gpp  = "$root\mingw64\bin\g++.exe"
$gcc  = "$root\mingw64\bin\gcc.exe"
$src  = "$root\src"
$sl   = "$root\third_party\sl2.2"                  # committed Streamline 2.2 headers
$mh   = "$root\minhook_src"                        # git clone https://github.com/TsudaKageyu/minhook
$mhobj= "$root\mh_obj"
$out  = "$src\amd_fidelityfx_dx12.dll"

if (-not (Test-Path $mh)) { git clone --depth 1 https://github.com/TsudaKageyu/minhook.git $mh }

# Compile MinHook C sources once
New-Item -ItemType Directory -Force -Path $mhobj | Out-Null
foreach ($f in @("buffer","hook","trampoline")) {
    & $gcc -c -O2 -I "$mh\include" -I "$mh\src" "$mh\src\$f.c" -o "$mhobj\$f.o"
}
foreach ($f in @("hde32","hde64")) {
    & $gcc -c -O2 -I "$mh\include" -I "$mh\src" "$mh\src\hde\$f.c" -o "$mhobj\$f.o"
}

# Build the proxy DLL
& $gpp -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ `
    -I "$sl" -I "$mh\include" `
    -o $out `
    "$src\proxy.cpp" "$src\slbridge.cpp" "$src\nvhook.cpp" `
    "$mhobj\buffer.o" "$mhobj\hook.o" "$mhobj\trampoline.o" "$mhobj\hde32.o" "$mhobj\hde64.o" `
    "$src\exports.def" -lkernel32 -luser32 -ladvapi32
if ($LASTEXITCODE -ne 0) { Write-Error "build failed ($LASTEXITCODE)"; exit 1 }
"BUILD OK -> $out  ($((Get-Item $out).Length) bytes)"
