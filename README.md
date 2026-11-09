# asahi-m4-displayport

Research code: **USB-C DisplayPort alt-mode external display on the Apple M4 (T8132), MacBook Air "J713"**, under an Asahi-based Linux
(tested on [Omarchy-M4-Native](https://github.com/Kb2uka/Omarchy-M4-Native)'s kernel and boot chain).

> **Read this first.**
> This is bring-up scaffolding written to find out what the hardware needs. It pokes power-management and DART registers, loads
> modules that can never be unloaded, and can hang or panic the machine. It has been run on **one** laptop with **one** monitor.
> The installers (`install-dcpext-v2.py`, `install-auxshm.py`) write `/boot/efi/omarchy/boot.bin`. Do not run any of it
> unless you understand what it does and can recover your boot chain. No warranty.
> It is **not** a mergeable driver (see [FINDINGS.md](FINDINGS.md#what-this-is-and-is-not)); the findings are the useful part.

## Status

| | |
|---|---|
| Works | One Dell U2723QE on USB-C port 2 as a normal Hyprland monitor: 2560x1440@60, RGB 8 bpc, HBR2 x2 lanes, from a fresh boot with the monitor already plugged in |
| Does not work | 4K (needs HBR3 + DSC), hotplug/replug, suspend/resume, EDID (modes come from the display firmware's timing list), HDCP, audio, other ports/monitors |
| Limits | The external-DCP firmware allows **one connect per boot**; the probe module cannot be removed; `j713_extdrm` can be reloaded |
| Boot autostart | `j713-extdisplay.service` + `install-extdisplay.sh` exist but are untested at boot |

## How it works (short)

1. m1n1 reserves the external DCP's firmware (iBoot preloads it but never starts it on a laptop): `m1n1/m1n1-dcpext-handoff.patch`,
   `m1n1/m1n1-auxshm.patch`.
2. `j713_dcpext_probe2` (dcpext2/) starts the coprocessor and talks to iBoot's display service over AFK.
3. Before connecting: `j713_dpxbar` selects crossbar state 2, `j713_dpaux` programs the DP AUX/PHY for HBR2, `j713_pmcon` powers the
   PMGR `PMC` device on, `j713_dispfb` maps a scanout buffer into the display DARTs.
4. `j713_extdrm` (extdrm/) is a small DRM/KMS driver on top: shadow plane, damage blit into the scanout buffer, modes from the
   firmware's timing list. Hyprland sees it as another monitor.
5. `extdisplay-up.sh` runs the whole sequence on a fresh boot.

## Layout

| Path | What |
|---|---|
| `dcpext2/` | external-DCP client + AFK transport (iBoot display ops: power, mode, surface), exports used by the DRM driver |
| `extdrm/` | the DRM/KMS driver |
| `dispfb/` | runtime DT overlay + scanout buffer mapped at the same IOVA in `dart-dispext0` and `dart-dcpext0` |
| `dpaux/`, `dpxbar/` | DP AUX/PHY (HBR2) programming and the T8132 DP crossbar |
| `pmcon/`, `pmgrdump/`, `pmcpeek/`, `dartwalk/` | PMGR power-on, PMGR/DART inspection tools |
| `retarget/` | retargets dcpext0 to atc0 at runtime |
| `tipd-livepatch/` | livepatch for a tipd oops when reading partner identity |
| `m1n1/` | m1n1 patches (firmware handoff, AUX SHM tunable) |
| `omarchy/` | workaround for a Hyprland crash on screen-off/wake with the external display (lock-screen blank skips the external output) |
| `tools/` | ADT / firmware-log decoders |
| `test-*.sh`, `extdisplay-up.sh`, ... | the bring-up experiments, in the order they were run; the `test-*.sh` scripts are historical |

## Build

Out-of-tree modules; copy `KMAKE.example` to `KMAKE`, point `-C` at a configured build tree of the kernel you boot, then in each module
directory: `make $(cat ../KMAKE) M=$PWD modules` (`dcpext2` also needs `KBUILD_EXTRA_SYMBOLS` from `dpaux`; see the scripts).

## Licence and attribution

Unless a file says otherwise: **GPL-2.0-only** (`COPYING`). Third-party and derived code keeps its own notices:

- `dcpext2/` derives from the M4 port's DCP backlight module ([Omarchy-M4-Native](https://github.com/Kb2uka/Omarchy-M4-Native), KB2UKA), whose AFK transport derives from
  Asahi Linux's `drivers/gpu/drm/apple/afk.c` (Copyright 2022 Sven Peter, GPL-2.0-only OR MIT). See `dcpext2/NOTICE` and `dcpext2/LICENSES/`.
- `dpxbar/` derives from Asahi Linux's display crossbar mux driver (Sven Peter / Asahi Linux Contributors, GPL-2.0-only OR MIT).
- `dpaux/`'s HBR2 register map is adapted from Asahi's T8122 DisplayPort work (AsahiLinux/linux PR #662).
- `tipd-livepatch/` is based on the kernel's `drivers/usb/typec/tipd` (Intel, Purism; GPL-2.0+).

Not affiliated with Apple, Asahi Linux or Omarchy. **No Apple firmware, kernelcache, device-tree dump or macOS material is included.**

## AI assistance

This code was developed with substantial AI assistance (Anthropic's Claude, via Claude Code). Treat it as unreviewed research code.
