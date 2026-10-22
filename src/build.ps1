# Build fsr2dlss (amd_fidelityfx_dx12.dll) on Windows with MinGW-w64, and assemble the package in dist\.
#
#   .\src\build.ps1                 # uses g++ from $root\mingw64\bin if present, else from PATH
#
# Fetches MinHook and Dear ImGui (pinned tags) and the pinned Streamline SDK zip into deps\ on first run.
# CI uses src\fetch_deps.sh + src\build.sh under MSYS2 (see .github\workflows\build.yml); this script does
# the same thing with PowerShell so a local Windows build needs nothing but git and a MinGW-w64 toolchain.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot           # repo root
$deps = "$root\deps"
$obj  = "$root\build\obj"
$dist = "$root\dist"
$bin  = if (Test-Path "$root\mingw64\bin\g++.exe") { "$root\mingw64\bin\" } else { "" }
$gpp  = "${bin}g++.exe"
$gcc  = "${bin}gcc.exe"

$minhookTag = "v1.3.4"
$imguiTag   = "v1.93.0"                             # third_party\imgui_backends is patched from this tag
$slPin      = Get-Content "$root\third_party\streamline\SDK_VERSION"
$slVersion  = $slPin[0].Trim()
$slSha      = $slPin[1].Trim().ToLower()
$slZip      = "$deps\streamline-sdk-v$slVersion.zip"
$slUrl      = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$slVersion/streamline-sdk-v$slVersion.zip"
$slDlls     = @("sl.interposer.dll","sl.common.dll","sl.dlss.dll","sl.dlss_g.dll","sl.pcl.dll","sl.reflex.dll","nvngx_dlss.dll","nvngx_dlssg.dll")
$slLic      = @("nvngx_dlss.license.txt","reflex.license.txt")

New-Item -ItemType Directory -Force -Path $deps, $obj | Out-Null

# ---- dependencies -------------------------------------------------------------------------------
if (-not (Test-Path "$deps\minhook\.git")) { git clone -q --depth 1 --branch $minhookTag https://github.com/TsudaKageyu/minhook.git "$deps\minhook" }
if (-not (Test-Path "$deps\imgui\.git"))   { git clone -q --depth 1 --branch $imguiTag   https://github.com/ocornut/imgui.git      "$deps\imgui" }

$needZip = -not (Test-Path $slZip)
if (-not $needZip) { $needZip = (Get-FileHash $slZip -Algorithm SHA256).Hash.ToLower() -ne $slSha }
if ($needZip) {
    Write-Host "[deps] downloading Streamline SDK v$slVersion"
    Invoke-WebRequest -Uri $slUrl -OutFile "$slZip.part"
    Move-Item -Force "$slZip.part" $slZip
}
$got = (Get-FileHash $slZip -Algorithm SHA256).Hash.ToLower()
if ($got -ne $slSha) { Write-Error "Streamline SDK zip hash $got != pinned $slSha"; exit 1 }

$slBin = "$deps\streamline\bin"
if (Test-Path $slBin) { Remove-Item -Recurse -Force $slBin }
New-Item -ItemType Directory -Force -Path $slBin | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($slZip)
try {
    foreach ($name in ($slDlls + $slLic)) {
        $e = $zip.GetEntry("bin/x64/$name"); if (-not $e) { Write-Error "$name missing from SDK zip"; exit 1 }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$slBin\$name", $true)
    }
    $e = $zip.GetEntry("license.txt"); [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$deps\streamline\streamline.license.txt", $true)
} finally { $zip.Dispose() }

# ---- compile ------------------------------------------------------------------------------------
$cflags   = @("-O2","-DNDEBUG")
$cxxflags = @("-O2","-std=c++17","-DNDEBUG","-DNOMINMAX","-Wno-deprecated-declarations",
              "-DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD","-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS","-DIMGUI_DISABLE_DEMO_WINDOWS",
              "-I","$root\third_party\streamline\include","-I","$deps\minhook\include",
              "-I","$root\third_party\imgui_backends","-I","$deps\imgui","-I","$deps\imgui\backends")
function Run($exe, $args) { & $exe @args; if ($LASTEXITCODE -ne 0) { Write-Error "$exe failed ($LASTEXITCODE)"; exit 1 } }

Write-Host "[build] MinHook"
foreach ($f in @("buffer","hook","trampoline")) { Run $gcc ($cflags + @("-c","-I","$deps\minhook\include","-I","$deps\minhook\src","$deps\minhook\src\$f.c","-o","$obj\mh_$f.o")) }
foreach ($f in @("hde32","hde64"))              { Run $gcc ($cflags + @("-c","-I","$deps\minhook\include","-I","$deps\minhook\src","$deps\minhook\src\hde\$f.c","-o","$obj\mh_$f.o")) }

Write-Host "[build] Dear ImGui"
foreach ($f in @("imgui","imgui_draw","imgui_tables","imgui_widgets")) { Run $gpp ($cxxflags + @("-c","$deps\imgui\$f.cpp","-o","$obj\$f.o")) }
Run $gpp ($cxxflags + @("-c","$deps\imgui\backends\imgui_impl_win32.cpp","-o","$obj\imgui_impl_win32.o"))
Run $gpp ($cxxflags + @("-c","$root\third_party\imgui_backends\imgui_impl_dx12.cpp","-o","$obj\imgui_impl_dx12.o"))

Write-Host "[build] fsr2dlss"
foreach ($f in @("proxy","slbridge","nvhook","config","overlay","dxgihook")) { Run $gpp ($cxxflags + @("-Wall","-Wno-unused-function","-c","$root\src\$f.cpp","-o","$obj\$f.o")) }

Write-Host "[build] link"
$out = "$root\build\amd_fidelityfx_dx12.dll"
$objs = @("proxy","slbridge","nvhook","config","overlay","dxgihook","imgui","imgui_draw","imgui_tables","imgui_widgets","imgui_impl_win32","imgui_impl_dx12","mh_buffer","mh_hook","mh_trampoline","mh_hde32","mh_hde64") | ForEach-Object { "$obj\$_.o" }
Run $gpp (@("-shared","-static","-static-libgcc","-static-libstdc++","-s","-Wl,--gc-sections","-o",$out) + $objs + @("$root\src\exports.def","-lkernel32","-luser32","-lgdi32","-limm32","-ldwmapi","-ld3dcompiler","-ld3d12","-ldxgi"))
"BUILD OK -> $out  ($((Get-Item $out).Length) bytes)"

# ---- package ------------------------------------------------------------------------------------
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path "$dist\streamline", "$dist\licenses" | Out-Null
Copy-Item $out "$dist\"
Copy-Item "$root\fsr2dlss.ini.example" "$dist\fsr2dlss.ini"
Copy-Item "$root\docs\INSTALL.md" "$dist\"
Copy-Item "$slBin\*.dll" "$dist\streamline\"
Copy-Item "$slBin\*.license.txt" "$dist\licenses\"
Copy-Item "$deps\streamline\streamline.license.txt" "$dist\licenses\"
Copy-Item "$deps\imgui\LICENSE.txt" "$dist\licenses\imgui.license.txt"
Copy-Item "$deps\minhook\LICENSE.txt" "$dist\licenses\minhook.license.txt"
$versions = @("fsr2dlss package", "streamline_sdk=$slVersion")
foreach ($f in Get-ChildItem "$dist\streamline\*.dll") { $versions += "$($f.Name)=$($f.VersionInfo.FileVersion)" }
$versions | Set-Content "$dist\VERSIONS.txt"
Get-Content "$dist\VERSIONS.txt"
"PACKAGE OK -> $dist"
