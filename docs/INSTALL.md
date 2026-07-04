# Installing fsr2dlss (Lies of P)

Replaces the game's AMD FSR 3.1 upscaling + frame generation with **NVIDIA DLSS + DLSS‑G**.
Tested on **Lies of P** on an **RTX 5090**. Other FSR 3.1 (FfxApi) games may work but are untested.

> ⚠️ This is an unofficial mod that replaces a game DLL. Back up first, use at your own risk, and
> expect to keep it updated as the game/driver changes.

---

## 1. Requirements

- **NVIDIA RTX GPU** with DLSS Frame Generation support (RTX 40‑series or 50‑series). 3×/4× MFG
  needs RTX 50‑series.
- Recent NVIDIA driver.
- **Hardware‑accelerated GPU Scheduling (HAGS) = On** (Windows Settings → System → Display →
  Graphics → *Default graphics settings*). Reboot if you change it.
- Windows 10/11, DirectX 12.
- A copy of **Lies of P**.

---

## 2. Get the files

### a) The mod DLL
Download the latest **`fsr2dlss`** artifact from the repo's GitHub Actions **Build** run (it contains
`amd_fidelityfx_dx12.dll` + a sample `fsr2dlss.ini`). Or build it yourself — see
[ARCHITECTURE.md §8](ARCHITECTURE.md#8-build).

### b) The Streamline 2.7.1 runtime (NVIDIA binaries — not distributable, source from a game you own)
You need a **matched Streamline 2.7.1 set + nvngx 310.1**. The known‑good source is **Cyberpunk 2077**
(`…\Cyberpunk 2077\bin\x64\`). Copy these files:

```
sl.interposer.dll   sl.common.dll   sl.dlss.dll   sl.dlss_g.dll   sl.pcl.dll   sl.reflex.dll
nvngx_dlss.dll      nvngx_dlssg.dll
```

Verify their versions: `sl.*` should be **2.7.1**, `nvngx_*` should be **310.1**. They must all match.

---

## 3. Install

Let `WIN64 = …\steamapps\common\Lies of P\LiesofP\Binaries\Win64` (where `LOP-Win64-Shipping.exe` is).

1. **Back up** `WIN64\amd_fidelityfx_dx12.dll` (the real one, ~6.6 MB).
2. **Rename** it to `amd_fidelityfx_dx12.amd.dll` (the proxy forwards to this).
   *(Or just run `src/install.ps1`, which does steps 1–2 automatically and drops the proxy in.)*
3. **Copy the proxy** `amd_fidelityfx_dx12.dll` (from the artifact) into `WIN64\`.
4. **Create `WIN64\streamline\`** and put the 8 Streamline/nvngx files from step 2b into it.
5. **Copy `fsr2dlss.ini`** into `WIN64\` (see §5 to configure).

Final layout:
```
WIN64\
  amd_fidelityfx_dx12.dll        <- the mod (proxy)
  amd_fidelityfx_dx12.amd.dll    <- the original AMD DLL (backup the proxy forwards to)
  fsr2dlss.ini
  streamline\
    sl.interposer.dll  sl.common.dll  sl.dlss.dll  sl.dlss_g.dll
    sl.pcl.dll         sl.reflex.dll  nvngx_dlss.dll  nvngx_dlssg.dll
```

---

## 4. In‑game settings

Launch the game and in the video/graphics options:

- **Upscaling / Super Resolution = FSR** (any quality preset — the mod swaps it to DLSS and
  auto‑selects the matching DLSS mode; a lower FSR preset = DLSS upscaling for more FPS, native = DLAA).
- **Frame Generation = ON**.

The game still *thinks* it's running FSR; DLSS runs underneath. (Steam's overlay may label it "FSR" —
that's cosmetic; it reads the menu setting, not what's actually running.)

---

## 5. Configure `fsr2dlss.ini`

Place it next to `amd_fidelityfx_dx12.dll`. Key settings (full docs in the file):

| Setting | Values | Notes |
|---|---|---|
| `Multiplier` | `2` / `3` / `4` | Frame‑gen multiplier. 3×/4× need RTX 50; clamped to card max. |
| `DLSS` | `true` / `false` | DLSS upscaling on, or pass through to FSR. |
| `FlipMetering` | `software` / `hardware` / `auto` | Leave `software` (hardware doesn't insert frames under injection yet). |
| `HostVersion` | `2.7.1` | Must match the `streamline\` plugin versions. |
| `HudlessTags` | `false` | Experimental; leave off for baked‑UI games. |

---

## 6. Verify it's working

- **In‑game feel:** framerate goes up with Frame Generation on; DLSS looks sharper/cleaner than FSR.
- **NVIDIA on‑screen indicators (definitive):** run `src/fg_overlay.ps1` **as administrator** once —
  it enables NVIDIA's DLSS + DLSS‑G indicators. Relaunch: you should see NVIDIA's "DLSS Frame
  Generation" indicator (top) and DLSS super‑resolution indicator. Remove later with
  `fg_overlay.ps1 -Off`.
- **Log:** `WIN64\ffx_bridge.log` should show `[SR] DLSS upscale ACTIVE`, `[FGC] … presented=2`
  (or 3/4 for MFG), and `[CFG] fsr2dlss.ini: …`. `WIN64\streamline\sl.log` shows `sl.dlss` +
  `sl.dlss_g` plugins loaded.

---

## 7. Troubleshooting

- **Fatal error / crash on launch:** usually a Streamline version mismatch. Ensure all `sl.*` are
  2.7.1 and `HostVersion = 2.7.1`. Ensure the original DLL was renamed to `amd_fidelityfx_dx12.amd.dll`.
- **Image is "fizzly"/looks like FSR:** DLSS‑SR isn't loading — check `sl.dlss.dll` is present in
  `streamline\` and `[SR] DLSS upscale ACTIVE` appears in the log. (Missing `sl.dlss.dll` is the #1 cause.)
- **Red/wrong colors:** Streamline/model mismatch — use the matched 2.7.1 + 310.1 set.
- **Frame gen not doubling:** confirm HAGS is on; check `presented=2` in the log; make sure in‑game
  Frame Generation is ON.
- **1 fps crawl:** you're on a Streamline version that doesn't engage under injection — use 2.7.1.

---

## 8. Uninstall

Run `src/uninstall.ps1`, or manually: delete `amd_fidelityfx_dx12.dll`, rename
`amd_fidelityfx_dx12.amd.dll` back to `amd_fidelityfx_dx12.dll`, and remove `streamline\`,
`fsr2dlss.ini`, and the `*.log` files.
