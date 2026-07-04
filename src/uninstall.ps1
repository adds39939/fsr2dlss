# Reverts everything install.ps1 did: restores the original AMD DLL and the LukeFZ mod files.
$ErrorActionPreference = "Stop"
$win64 = "E:\Games\Steam\steamapps\common\Lies of P\LiesofP\Binaries\Win64"
$real   = Join-Path $win64 "amd_fidelityfx_dx12.dll"
$backup = Join-Path $win64 "amd_fidelityfx_dx12.amd.dll"

# Remove proxy, restore original AMD DLL
if (Test-Path $backup) {
    if (Test-Path $real) { Remove-Item $real -Force }
    Rename-Item $backup $real
    Write-Host "[+] restored original amd_fidelityfx_dx12.dll"
} else {
    Write-Host "[=] no backup found; nothing to restore for the AMD DLL"
}

# Re-enable LukeFZ files (optional; they are still broken on FSR3.1, but restore to original state)
foreach ($f in @("winmm.dll","FSR2Streamline.asi")) {
    $p = Join-Path $win64 $f
    if (Test-Path "$p.off") { if (Test-Path $p) { Remove-Item $p -Force }; Rename-Item "$p.off" $p; Write-Host "[+] re-enabled $f" }
}

# Best-effort: remove the NVIDIA DLSS-G debug overlay registry values (needs admin = HKLM).
try {
    $ngx = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NGXCore'
    Remove-ItemProperty -Path $ngx -Name 'DLSSG_IndicatorText' -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $ngx -Name 'ShowDlssIndicator'   -ErrorAction SilentlyContinue
    Write-Host "[+] removed NVIDIA FG overlay registry values"
} catch {
    Write-Host "[=] couldn't remove NVIDIA overlay values (need admin). Run:  powershell -ExecutionPolicy Bypass -File `"$PSScriptRoot\fg_overlay.ps1`" -Off"
}
Write-Host "DONE. Game restored to pre-bridge state."
