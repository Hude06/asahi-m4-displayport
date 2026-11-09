# Findings

Everything here was observed on one MacBook Air M4 (J713, T8132), a Dell U2723QE connected directly to USB-C port 2 (atc0), running a
7.1.9-based Asahi kernel. "Observed" means seen in kernel/firmware logs or by a working/failing experiment; nothing here comes from
Apple source code. Treat each item as a lead to verify, not a specification.

## What this is and is not

- It drives the display through **iBoot's display service** on the external DCP (AFK endpoint 0x23, `disp0-service`), the same minimal
  interface m1n1 uses (`SET_SURFACE`, `SET_POWER`, `GET_HPD`, `GET_TIMING_MODES`, `GET_COLOR_MODES`, `SET_MODE`, ...).
- It does **not** use Asahi's DCP driver. In an Asahi tree snapshot from 2026-05-30 that driver handles firmware compat 12.3 and 13.5;
  this machine's external-DCP firmware reports 18000.161.10. A mergeable M4 display path would extend the DCP driver, not ship these modules.
- The modules are experiments (no unload, one connect per boot, direct register pokes). The value is in the findings below.

## Findings

1. **DP crossbar state.** For dcpext0 the crossbar must be in state 2 (`dispext1,0` -> atc). The default state 0 makes the firmware's DPCD
   read fail with `0xe00002ed`. macOS logs show the same selection (`mux_select=2`).
2. **PHY rate/drive.** `phy_configure(set_rate)` in the kernel's T8132 atcphy returns -110 (`ACIOPHY_CMN_SHM_STS_REG0_CMD_READY` not set), so
   HBR3 cannot be requested; `set_voltages` is not applied (returns -EINVAL). HBR2 x2 works when the PLL/lane registers are programmed by
   hand (`dpaux/`); link training then passes CR/EQ and the sink reports 49 timing modes.
3. **PMGR `PMC` must be powered on.** The PMGR device `PMC` (id 0x14) has no consumer in the device tree, so genpd powers it off at boot.
   With it off, the display firmware takes an SError at MMIO 0x3_828aa140 (seen in the firmware crash log) on `SET_POWER 0` / `SET_MODE`.
   Powering `PMC` on before the connect makes the whole power-off/mode-set path complete without a fault (`pmcon/`).
4. **Write-only iBoot ops take no RX buffer.** `SET_POWER`/`SET_MODE`/`SET_SURFACE` sent with an RX buffer trip a firmware assertion
   (`AFKEPCommandContext.cpp:204`).
5. **Scanout mapping.** `dart-dispext0` (0x2_86300000) is locked by iBoot with a valid, empty L1 for stream 0, so Linux's `apple-dart` can drive
   it from a runtime DT overlay (no boot-chain change). Mapping the framebuffer at the same IOVA (bit 40 set) in `dart-dispext0` and
   `dart-dcpext0` gives a picture. Format: `x2r10g10b10` (iBoot format 9), pitch aligned to 256 bytes.
6. **Auto power management.** The `DISPEXT0_CPU` domain rests at actual state 4 (clock-gated) with target 0xf and auto-PM enabled, waking for
   about 0.3 ms at vsync cadence. Register access from the AP is safe in that state; it is *off with auto-PM disabled* (like `PMC`) that faults.
7. **tipd oops.** Reading a partner's identity through the tipd sysfs files oopses on this machine; `tipd-livepatch/` works around it.
8. **Userspace: Aquamarine/Hyprland.**
   - A display-only DRM device with no render node needs a software GBM path in Aquamarine's secondary-GPU code. The downstream patched
     Aquamarine only enables it for a driver named exactly `simpledrm`, hence the `drm_name=simpledrm` module parameter.
   - **Crash:** when every output goes DPMS-off (the Omarchy lock screen blanks all monitors 5 s after locking), Aquamarine logs
     `Deinitializing secondary renderer ... no enabled outputs`; on wake it re-creates the renderer and Mesa segfaults in `eglCreateContext`
     (Hyprland 0.56.2), taking the whole session down. Workaround: blank every monitor except the external display (`omarchy/`). The
     real fix is in Aquamarine/Mesa; the Mesa fault was not symbolised. With the workaround, a real lock and a real lid close both leave
     the Dell lit and Hyprland alive.
9. **Boot autostart needs two stages.** The DRM driver must be loaded after the user's Hyprland has done its first modeset, so that
   Aquamarine hot-adds the device (before that, the display manager's `AQ_DRM_DEVICES` pin hides it, and the greeter's own Hyprland would
   take it). A `.path` trigger on the user's runtime dir never fired, because `/run/user/<uid>` is a tmpfs mounted after the path unit
   started watching; a service `WantedBy=user@<uid>.service` works. With a 1 s settle after the first modeset, Hyprland adopted the output
   0.5 s after the module loaded. The link bring-up's fixed 10 s wait after `connect` is what gates the result when the user logs in fast
   (the Dell lit about 14 s after login on the measured boot).
10. **Replug at firmware level.** With the Dell up: `set_hpd(0)` (DPTX group 8 cmd 8) makes the firmware run a full, clean unplug (HPD removed,
    display power off, link shut down, PMGR domains off; no fault once `PMC` is on) and `GET_HPD` then reports no sink. `set_hpd(1)` brings the
    sink and its 49 timings back, but the firmware only logs `VideoInterface::Plug callback: AP not ready yet, mark hot-plug pending` and
    leaves the link down (the same message appears after the boot-time connect). Replaying `SET_POWER 1`, `SET_MODE` (same timing and colour
    as before) and `SET_SURFACE` (same buffer) restored the picture without a reboot or any compositor involvement; the link-training
    calls (`GET_MAX_LINK_RATE`, `SET_LINK_RATE`, `SET_ACTIVE_LANE_COUNT`, drive settings) started 0.2 s after `SET_MODE`, 3 s after
    `SET_POWER`, so retraining is triggered by `SET_MODE`. Not done: reacting to a typec partner disconnect, telling userspace (the DRM
    driver has no connector polling or hotplug event), and whether the PHY/crossbar survive a real cable pull. Note that making Hyprland
    drop and re-add the output may hit the renderer re-creation crash from item 8 (untested).
11. **Suspend is not safe on this setup.** An `echo mem > /sys/power/state` with `pm_test=devices` and the Dell attached hard-hung the machine
    (no kernel message, journal ends before the call). The same laptop has no qualified suspend path, so this says nothing specific about
    the display driver; it is recorded so nobody repeats it (`test-pm-devices.sh` now refuses to run).

## Known gaps

4K60 (needs HBR3 and DSC), TX drive settings and lane 1 PHY programming, hotplug as a driver feature (item 10), suspend/resume (item 11),
EDID/monitor identity, HDCP (firmware reports `protectLinkGated 0xe00002c7`), audio, a persistent `PMC` fix (a device-tree consumer for
the `pmc` power domain; today `pmcon/` powers it on at every boot), and the one-connect-per-boot limit.
