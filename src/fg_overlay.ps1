# Toggles NVIDIA's built-in DLSS / DLSS-G (Frame Generation) on-screen debug overlay.
# This is a DRIVER-side overlay read by nvngx from the registry - it works on the
# CURRENT proxy build with no rebuild, and shows whether DLSS-G is actually
# generating/presenting frames (top of screen) plus DLSS-SR state (bottom-left).
#
#   .\fg_overlay.ps1          # turn the overlays ON
#   .\fg_overlay.ps1 -Off     # turn them OFF
#
# Writes HKLM, so it self-elevates to admin. Reversible (-Off removes the values).
param([switch]$Off)

$key  = 'HKLM:\SOFTWARE\NVIDIA Corporation\Global\NGXCore'
# DLSSG_IndicatorText: 0=off, 1=minimal, 2=detailed (the FG frame counter, top of screen)
# ShowDlssIndicator:   0x400 shows the DLSS-SR indicator for both prod + dev DLLs (bottom-left)

# --- self-elevate if not admin ---
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    if ($Off) { $a += '-Off' }
    Write-Host "[*] elevating to admin to write HKLM ..."
    Start-Process powershell.exe -Verb RunAs -ArgumentList $a
    return
}

if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }

if ($Off) {
    Remove-ItemProperty -Path $key -Name 'DLSSG_IndicatorText' -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $key -Name 'ShowDlssIndicator'   -ErrorAction SilentlyContinue
    Write-Host "[+] NVIDIA DLSS / FG overlay DISABLED (registry values removed)."
} else {
    New-ItemProperty -Path $key -Name 'DLSSG_IndicatorText' -PropertyType DWord -Value 2     -Force | Out-Null
    New-ItemProperty -Path $key -Name 'ShowDlssIndicator'   -PropertyType DWord -Value 0x400 -Force | Out-Null
    Write-Host "[+] NVIDIA FG overlay ENABLED  (DLSSG_IndicatorText=2, top of screen)."
    Write-Host "[+] NVIDIA DLSS-SR indicator ENABLED (ShowDlssIndicator=0x400, bottom-left)."
    Write-Host "    Relaunch the game to see them. If FG is doubling, the top overlay shows it generating."
}
Write-Host "[i] key: $key"
if ($Off) { } else { Write-Host "[i] turn back off with:  .\fg_overlay.ps1 -Off" }
Read-Host "`nPress Enter to close"
