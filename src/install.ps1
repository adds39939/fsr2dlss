# Installs the FfxApi proxy (currently the Phase-2 logging shim) into the game,
# backing up the original AMD DLL and disabling the broken LukeFZ ASI mod.
# Fully reversible via uninstall.ps1.
$ErrorActionPreference = "Stop"
$win64 = "E:\Games\Steam\steamapps\common\Lies of P\LiesofP\Binaries\Win64"
$proxy = "C:\Users\Adam\fsrb\src\amd_fidelityfx_dx12.dll"

$real   = Join-Path $win64 "amd_fidelityfx_dx12.dll"
$backup = Join-Path $win64 "amd_fidelityfx_dx12.amd.dll"   # original AMD DLL (proxy loads this)

if (-not (Test-Path $proxy)) { throw "proxy not built: $proxy" }

# 1) Back up the original AMD DLL (only once; never clobber an existing backup)
if (Test-Path $backup) {
    Write-Host "[=] backup already exists: $backup (leaving as-is)"
} else {
    $info = Get-Item $real
    if ($info.Length -lt 1MB) { throw "current amd_fidelityfx_dx12.dll is only $($info.Length) bytes - not the AMD original; aborting to avoid clobbering" }
    Rename-Item $real $backup
    Write-Host "[+] backed up original AMD DLL -> amd_fidelityfx_dx12.amd.dll ($($info.Length) bytes)"
}

# 2) Install proxy as amd_fidelityfx_dx12.dll
Copy-Item $proxy $real -Force
Write-Host "[+] installed proxy -> amd_fidelityfx_dx12.dll ($((Get-Item $real).Length) bytes)"

# 3) Disable the broken LukeFZ ASI loader (winmm.dll) + its .asi so it can't fatal-error
foreach ($f in @("winmm.dll","FSR2Streamline.asi")) {
    $p = Join-Path $win64 $f
    if (Test-Path $p) { Rename-Item $p "$p.off" -Force; Write-Host "[+] disabled $f -> $f.off" }
    elseif (Test-Path "$p.off") { Write-Host "[=] $f already disabled" }
}

# 4) Clear any stale log
$log = Join-Path $win64 "ffx_bridge.log"
if (Test-Path $log) { Remove-Item $log -Force; Write-Host "[+] cleared old ffx_bridge.log" }

# 5) Best-effort: enable NVIDIA's DLSS-G on-screen overlay for debugging (needs admin = HKLM).
#    Non-fatal: if not elevated, just point at the standalone toggle.
try {
    $ngx = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NGXCore'
    if (-not (Test-Path $ngx)) { New-Item -Path $ngx -Force | Out-Null }
    New-ItemProperty -Path $ngx -Name 'DLSSG_IndicatorText' -PropertyType DWord -Value 2     -Force -ErrorAction Stop | Out-Null
    New-ItemProperty -Path $ngx -Name 'ShowDlssIndicator'   -PropertyType DWord -Value 0x400 -Force -ErrorAction Stop | Out-Null
    Write-Host "[+] NVIDIA FG overlay enabled (DLSSG_IndicatorText=2 top, ShowDlssIndicator=0x400 bottom-left)"
} catch {
    Write-Host "[=] couldn't set NVIDIA overlay (need admin). Run:  powershell -ExecutionPolicy Bypass -File `"$PSScriptRoot\fg_overlay.ps1`""
}

Write-Host "`nDONE. Launch the game, set Upscaling = FSR (and enable Frame Generation), play ~30s, quit."
Write-Host "Log will be at: $log"
