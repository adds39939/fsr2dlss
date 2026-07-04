$ErrorActionPreference = "Stop"
$root = "C:\Users\Adam\fsrb"
$gpp  = "$root\mingw64\bin\g++.exe"
$src  = "$root\src"
$out  = "$src\amd_fidelityfx_dx12.dll"
& $gpp -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ `
    -I"$root\sdk\sl2.2" `
    -o $out `
    "$src\proxy.cpp" "$src\slbridge.cpp" "$src\exports.def" `
    -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { Write-Error "build failed ($LASTEXITCODE)"; exit 1 }
"BUILD OK -> $out  ($((Get-Item $out).Length) bytes)"
