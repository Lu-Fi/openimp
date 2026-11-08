# Open stack - troubleshooting catalogue

**Audience:** an AI assistant helping a thingino user whose camera
misbehaves. Companion to [reference.md](reference.md) and
[feature-list.md](feature-list.md). State: 2026-11-08 (RC3
`v2026.11.07-rc3` plus `next`). Entries say when a fix is newer than RC3.

How to use this file:

1. First find out **which stack runs** (section 1) and which SoC, sensor and
   image version. Many reports are about the vendor stack, which this project
   does not change.
2. Match the symptom in section 3, or a pasted log line in section 5.
3. If the open stack is the problem and a fix is not available, say so and
   offer the vendor-stack image as a workaround (honesty rule in
   [AGENTS.md](../../AGENTS.md)).

## 1. Which stack is running

| Check (on the camera, over SSH) | Open stack | Vendor stack |
|---|---|---|
| `strings /usr/lib/libimp.so \| grep openimp` | prints `libimp.so <SOC> openimp` (the text `IMP_System_GetVersion` returns) | nothing |
| driver marker: `ls /sys/module/tx_isp_<soc>/parameters/` | open-only parameters exist: `qbuf_guard` (T10, T20, T21, T23, T31, T41), `t20_simple_ae` (T10, T20), `fcrop_upscale_pct` (T23, T31), `t40_dmsc_follow` (T40), `t41_defog` (T41) | these names are absent |
| T30 | no unique sysfs marker is documented; use the libimp check and the image build configuration | |
| module name (`lsmod`) | `tx-isp-<soc>` / `tx_isp_<soc>` - **the same name in both stacks**, so the name alone proves nothing | |

Mixed mode: `BR2_PACKAGE_THINGINO_ISP_OPEN_VENDOR_LIBIMP=y` gives the open
driver (markers present) with the vendor libimp (no `openimp` string).

Image version: `grep -E 'IMAGE_ID|BUILD_ID' /etc/os-release` on a thingino
camera gives the camera profile and the build stamp (branch, commit, build
time); the open-stack tag is
the one pinned in the firmware branch ([reference.md](reference.md#which-firmware-branch-ships-which-version-checked-2026-11-08)).

## 2. How to switch, and what to collect for a bug report

**Switch:** build a full image with the other choice
(`BR2_PACKAGE_THINGINO_ISP_OPEN=y` or `BR2_PACKAGE_THINGINO_ISP_PROPRIETARY=y`)
and flash the **complete OTA image** (kernel and rootfs together; never a
rootfs-only update, which can pair a driver with the wrong library). There is
no run-time switch. For an A/B test keep everything else (application,
configuration) the same.

**Driver or library?** Build once with
`BR2_PACKAGE_THINGINO_ISP_OPEN_VENDOR_LIBIMP=y`. If the problem stays, it is
in open-tx-isp; if it disappears, it is in OpenIMP.

**Bug report** - open an issue at https://github.com/Lu-Fi/openimp/issues
(library, encoder, integration, or when unsure) or
https://github.com/Lu-Fi/open-tx-isp/issues (kernel driver, ISP, memory).
Include:

- `IMAGE_ID` and `BUILD_ID` from `/etc/os-release`, the thingino branch, and
  **which stack** (section 1)
- SoC and sensor (`dmesg` shows the sensor driver; the camera model helps)
- `dmesg` (complete, after the problem) and `logread` (syslog; OpenIMP logs
  to stderr of the application, and also to syslog with
  `OPENIMP_LOG_SYSLOG=1`)
- the application log, the stream set (resolutions, codecs, fps, bitrate,
  snapshots, motion detection, audio)
- `cat /proc/meminfo`, `lsmod`, the `rmem=` / `ispmem=` values of the kernel
  command line (`cat /proc/cmdline`) and the `rmem peak` log line
- for a reboot: the reset-diagnostics output (section 3.10)
- does the vendor-stack image show the same? (the single most useful fact)

Never post IP addresses, credentials, location names or camera pictures
unless the user decides so.

## 3. Symptom -> cause -> fix

### 3.1 No picture / stream does not start

| Cause | How to confirm | Fix |
|---|---|---|
| The boot guard suppressed the ISP stack | log line `isp-guard: ... open ISP stack not loaded this boot` | `/etc/init.d/S10isp-guard status`; find the crash cause; `S10isp-guard clear` and reboot (guard exists on the `aperto` branch only) |
| Sensor not detected | `dmesg`: sensor I2C errors, `tx_isp_vin_init: no sensor available`, `subdev_sensor_ops_ioctl: sensor not found in subdev hostdata` (T31) | check the sensor module and cable; compare with the vendor image; sensor drivers come from the Ingenic SDK, not from this project |
| rmem too small | OpenIMP: `rmem allocation of N KB for X failed: ... raise rmem by at least M KB (suggest rmem=<n>M)` | raise `rmem` on the kernel command line, or open fewer / smaller streams (section 3.3) |
| Sensor stopped delivering frames (T40/T41) | `channel N: no sensor signal for ...`; the stream shows a synthetic "NO SENSOR SIGNAL" picture | check sensor, cable, `dmesg` (ISP interrupts, I2C). `OPENIMP_FALLBACK_FRAME=0` restores the vendor behaviour (no frames) |
| Frame channel did not start | `[KernelIF] VIDIOC_STREAM_ON failed: ...`, `[KernelIF] Failed to open /dev/...` | read `dmesg` right after it; report with both logs |
| H.265 asked on a SoC without HEVC hardware (T10/T20/T21/T23) | `CreateChn(PT_H265)` returns -1, log `H.265 not supported by the hardware on this SoC (Helix encoder is H.264/JPEG only); use H.264` | use H.264 (the vendor library returns 0 here but the channel never encodes) |
| T30 H.265 switched off | `OPENIMP_T30_H265=0` set | unset it |
| Boot loop with the open stack | camera reboots before 300 s | with the boot guard installed, break it from U-Boot with `setenv isp_open off; boot` (or `manual`), read `dmesg`; or SD recovery with `autoupdate-full.bin` |

### 3.2 Black, frozen or magenta / pink / green picture

| Cause | How to confirm | Fix |
|---|---|---|
| T30 H.264 starts black for up to 0.5 s | intended (calm first window, beyond vendor) | `OPENIMP_T30_BLACK_START=0` for a live start |
| T40: AE/AWB/day-night freeze while no client pulls frames | old library or `OPENIMP_3A_FEED=0` | library of 2026-10-26 or later; leave `OPENIMP_3A_FEED` unset |
| A snapshot after a long pause shows an old scene (T40/T41) | `dropped stale capture buffers` is the fix working | nothing to do; an old library returns the old picture |
| T40 grey / green-magenta day picture after night -> day | `t40_dn_keep_awb1` = 0 | keep the default 1 |
| T20 pink / green band at the top with vertical flip | old build (UV start from the aligned height) | update; fixed during the RC1 campaign |
| T31: one black picture of about 80 s during a day/night probe | seen once, not understood | report with logs if it recurs |
| T10: switch to Day in the dark with a black frame (3 events in 44 h, RC1 soak) | known from RC1 | report frequency |
| Magenta / pink cast in daylight | the documented case (T40, 2026-10-10) was the **IR-cut filter left open**; the IR cut is driven by the firmware through a GPIO, not by libimp | check that the IR-cut filter is closed in day mode (the firmware's day/night / IR-cut control); then compare with the vendor image on the same camera; report with a description (not a picture unless the user wants) |
| "Decodes without errors but looks wrong" | a clean decode does not prove a correct picture | check the picture itself; use the test pattern ([feature-list.md](feature-list.md#test-pattern)) |

### 3.3 Memory: rmem / ispmem too small, OOM

| Log / symptom | Meaning | Fix |
|---|---|---|
| `rmem allocation of N KB for X failed: free F KB, largest block L KB; raise rmem by at least M KB (suggest rmem=<n>M)` (OpenIMP, tag `DMA`) | the stream set does not fit rmem | raise `rmem` by at least M KB or reduce streams |
| `rmem allocation ... failed: allocation table full (N extents)` | too many allocations | fewer channels / restart the application; report |
| `rmem budget exceeded: FrameSource pool vbm_chnN ...` (warning) | pools plus fixed buffers exceed rmem | reduce streams or resolution, or raise rmem |
| `rmem peak X KB of Y KB (free Z KB)` | information: peak use | keep headroom; measure with the real stream set |
| `ispmem N KB too small for <block> at WxH (need ..., ... available) - <block> disabled; set ispmem >= N KB` (T10/T20 driver) | temporal denoise or WDR switched off | raise `ispmem` or accept |
| `MMAP buffers requested but isp_mmap_pool_kb=0; set isp_mmap_pool_kb=<n>` (T10/T20) | a V4L2 MMAP client; OpenIMP does not need it | raise the parameter only for such a client |
| `ispmem isn't enough!` (T30), `snapraw buffer needs N bytes but ispmem has M` (T23) | ispmem too small for that function | raise `ispmem` |
| `... QBUF idx=.. phys=.. outside rmem` (driver) | a buffer outside the rmem window was refused (`qbuf_guard`) | check `rmem=` of the kernel command line; `qbuf_guard=0` only as a debug step |
| OOM kill, then watchdog reset, with several streams | rmem + ispmem leave too little for Linux | reduce rmem to the measured peak plus headroom; fewer parallel streams. T41: `rmem=30M` was too much with three streams plus snapshots; about 24-26 MB worked |
| T41: U-Boot ignores the stored environment (`Bad CRC`), falls back to e.g. `rmem=30M` | environment size mismatch (64 KiB vs 32 KiB in `fw_env.config`) | use 64 KiB for the T41 environment; the fix (thingino PR 1738) was not merged upstream, apply it in your own tree; never write over a bad environment |

### 3.4 Sensor not found / AddSensor fails

- `IMP_ISP_AddSensor` fails (-1) after `IMP_ISP_SetDefaultBinPath` (T41): old
  library or driver; update both (paths up to 127 characters; a missing file
  fails `AddSensor`).
- `AddSensor` returns EBUSY after an OOM kill (T41): the sensor stays
  registered until reboot (vendor behaviour, unchanged). Reboot.
- `rmmod` of the sensor module is refused while streaming: intended (pinned
  while the ISP is open). Stop the application first.

### 3.5 EBUSY after kill, reload issues

| Symptom | Cause | Fix |
|---|---|---|
| T41: custom AE algorithm library, after `kill -9` the next `EnableSensor` fails with EBUSY | vendor module behaviour | the open driver releases the library when its client dies (no reload needed); on the vendor stack reload the module |
| IVS channel create returns EBUSY | another consumer owns the channel | do not retry blindly |
| `insmod` oopses after an earlier `rmmod` (T41) | leftover of an older module (`fs_remove` bug, fixed) | boot with `isp_open=manual`, load the fixed build once |
| Module refuses to load: unknown parameter `t20_trace` / `t10_runtime_trace` / `t20_runtime_trace` / `t20_trace_events` (T10/T20) | trace parameters are not built by default since 2026-11-06 | remove them from the load line, or build with `TX_ISP_TRACE=y` |
| Module refuses to load with a bring-up parameter (T23/T40/T41) | bring-up parameters exist only with `TX_ISP_<SOC>_BRINGUP_PARAMS=y` | remove it from the load line |
| Reload state on T30 | rmmod/insmod of the open module **not yet run on the device** (matrix row 2: 🔧) | prefer a reboot on T30 |

### 3.6 Day/night, IR cut

- The **day/night decision is made by the application** (the firmware's
  streamer or daynight script); the library and the driver report sensor
  values. Flapping at dusk (seen on T23 at RC1) is a threshold matter of the
  application.
- T10/T20: `/proc/jz/isp/isp-m0` ("MAX SENSOR analog gain") reports the
  analog gain the AE really reaches; a rule that waits for the cap needs it.
- T41: day/night works without a stream consumer and returns to day by
  itself (beyond vendor); do not force night mode in a bright room.
- Before judging colours or brightness, wait until day/night and the IR cut
  are stable.
- IR cut on T30 is `?` in the matrix (not documented); check the live matrix.

### 3.7 Colours, brightness, image controls

| Symptom | Cause | Fix |
|---|---|---|
| AWB presets have no effect (T30) | known deviation (matrix ⚠️, #202): presets read r=b=128 | use auto or manual WB, or the vendor stack if presets matter |
| Colour cast after stream restart (T23, older builds) | AWB reset on every stream start | update |
| AE compensation seems to do nothing (T31) | the AE has about 6 s dead time and needs 25-30 s to settle | wait 30 s before judging |
| T40 AE compensation far stronger than the vendor | old driver metered the NV12 picture (#232) | update to RC3 or later |
| `SetFaceAe` has no effect (T41) | `t41_ae_roi_stats` or `t41_vendor_ae` is 0 | both 1 (default); the vendor AE converges slowly |
| `SetMaxAgain(0)` does not cap the gain (T20) | bug #238 | fixed in open-tx-isp after RC3 (commit 36e7da60); update to a newer image |
| T20 picture noisier than the vendor at the same exposure | light-box measurement 2026-11-08: temporal noise 15-22 % higher, no follow-up yet | known gap; the vendor stack is quieter here |
| ISP OSD shows only blue/white parts (T41) | BGRA byte order read as ARGB | use `IMP_ISP_ARGB_TYPE_ARGB` for such a bitmap |
| `SetOSDBlock` / `SetOsdRgnAttr` returns -1 (T23) | odd width (stops the T23 ISP) | even width |
| `SetISPBypass(DISABLE)` returns -1 | the real bypass is refused unless the optional bypass graph is enabled (`OPENIMP_<SOC>_BYPASS_GRAPH=1` plus `isp_bypass_graph=1` where it exists) | keep full ISP (ENABLE; on T41 value 0) on a production camera |
| `SetISPCustomMode(DISABLE)` -1 (T31) / `GetISPCustomMode` 0xffffffff (T23) | older driver without a custom bin | update; ENABLE still needs a custom bin |

### 3.8 Encoder, bitrate

| Symptom | Cause | Fix |
|---|---|---|
| T30 H.265 live bitrate change stays at about 361 kbit/s | old library (#228) | update to RC3 or later |
| T31 CBR stream below the target in a static scene | the open stack writes no filler NAL (deviation) | expected; it saves bandwidth |
| T31 CappedQuality looks like CappedVBR | deviation (less than vendor), not implemented | expect about 1-2 QP coarser in static scenes |
| T10/T20/T21: QP steps, staticTime, changePos, qualityLvl, I/P delta, bias have no effect | accepted but ignored on these SoCs (mode, bitrate, QP range, GOP, fps act) | do not offer them as live controls |
| `IMP_Encoder_YuvEncode` returns -1 (T40/T41) | `outLen` is the capacity and the AU does not fit, or the bitrate was given in bit/s | set `outLen` to the buffer size; kbit/s |
| `Helix run failed errno=5` / `vpu error status=100` (T23) | one picture failed; frequent drops were fixed in the kernel; a rare single error stays open | harmless if rare; report frequency |
| Channel stops after repeated failures (T20/T21/T30) | after 3 failed pictures the encoder is re-created, after 2 fruitless re-creates the channel stops | restart the channel; report |
| `FrameSource_GetFrame` returns 0x80012000 after 2 s (T41) | N frames held and not released | release frames |
| `CreateChn` / `SetChnAttr`: a value was clamped with a warning | out-of-range QP or fps | use valid values; the warning names the field |

### 3.9 High CPU

- Rotation costs CPU (software rotation): T30 main 1280x720 rotated plus sub
  at 25 fps: 34.5 % process CPU; T23 about 15 % at 720p15 (C kernel). Keep
  rotated channels small.
- Motion detection: `OPENIMP_IVS_MOVE_INTERVAL` (2..1000) analyses only every
  n-th frame (T31 path).
- Measured where the open stack is lower: T20 application CPU 10.5 % vs 25.2 %
  (night, same scene); T40 streamer 0.34 % vs 2.26 %; T10 light box 2.59 % vs
  7.37 %; T20 light box 9.08 % vs 9.88 %. A higher CPU on the open stack than
  on the vendor stack is unexpected: report it with `top` output and the
  stream set.

### 3.10 Unexpected reboots -> reset diagnostics

A watchdog reset usually leaves no panic text (the kernel log is forwarded by
a user-space process, the board restarts within seconds).

1. On the camera: `dmesg | grep "RESET ERROR"` (the reset PC) and
   `/proc/diag_lastpanic` where available; in the syslog the `resetcause`
   lines (`PANIC`, `RESTART`, `SLED`, `NORECORD`).
2. T31: a short panic record survives in the RTC alarm register (proven on
   T31L and T31X). T23: the "sled" encodes reason, uptime class and task name
   in the reset PC. Both need the diagnostic image options of the firmware.
3. An SD ring log (if installed) shows the last seconds before the freeze.
4. A reset PC at `__delay` in the restart path means a planned reboot or a
   panic, not a hang.
5. Recovery of a boot-looping camera: FAT32 SD card with the full image as
   `autoupdate-full.bin` (proven on the T31L; not available on the Eufy T40).
6. Check power first: resets during power outages were power-on resets.
7. Never run MXU2 test code on a T23 (no MXU2; this caused a reset, #229).

Full write-up: [docs/RESET_DIAGNOSTICS.md](../RESET_DIAGNOSTICS.md).

## 4. What NOT to advise

- Do not suggest a rootfs-only update or mixing driver and library versions.
- Do not suggest bring-up, trace or `internal` switches as a fix
  ([env-vars.md](env-vars.md), [module-params.md](module-params.md)).
- Do not suggest `qbuf_guard=0`, `OPENIMP_DISABLE_RMEM=1` or the bypass graph
  on a production camera.
- Do not promise a fix date; releases are gates, not dates.
- Do not claim the open stack is better without a matrix cell or measurement.

## 5. Log-message dictionary

`IMP_LOG` lines carry a tag (`DMA`, `ISP`, `FrameSource`, `Encoder`, ...);
`[KernelIF]` lines go to stderr; driver lines are in `dmesg`.

| Message (exact start) | Component | Meaning / action |
|---|---|---|
| `rmem peak X KB of Y KB (free Z KB)` | OpenIMP `DMA` | peak rmem use; information |
| `rmem allocation of N KB for X failed: free F KB, largest block L KB; raise rmem by at least M KB (suggest rmem=<n>M)` | OpenIMP `DMA` | rmem too small; section 3.3 |
| `rmem allocation of N KB for X failed: allocation table full` | OpenIMP `DMA` | too many allocations; report |
| `rmem budget exceeded: FrameSource pool vbm_chnN` | OpenIMP `DMA` | pools do not fit; reduce streams |
| `rmem free: no allocation at phys=...` / `free of an address that is not a live ...` | OpenIMP `DMA` | a double or wrong free; report |
| `video drop: no frame for N s (report M)` | OpenIMP `ISP` | no frames from the ISP for N s while frames are wanted; check sensor / `dmesg` |
| `channel N: no sensor signal for ...` / `channel N: sensor signal is back` | OpenIMP `FrameSource` (T40/T41) | fallback frame started / ended |
| `channel N: fallback frames switched off` | OpenIMP `FrameSource` | `OPENIMP_FALLBACK_FRAME=0` or the API switched it off |
| `chnN: dropped stale ...` | OpenIMP (T40/T41) | normal after a pause without consumer |
| `chnN: DQBUF: stream not running (driver stopped it?)` | OpenIMP `Framesource` | the driver stopped the channel; read `dmesg` |
| `chnN: pooling thread still running N ms after STREAMOFF ...` | OpenIMP `Framesource` | teardown stall; report with `dmesg` |
| `EnableChn(N): nrVBs ... + maxdelay ... exceed ... buffers` | OpenIMP | buffer count too high for the channel |
| `[KernelIF] Failed to open <dev>: <error>` | OpenIMP | device node missing: driver not loaded or wrong module |
| `[KernelIF] VIDIOC_STREAM_ON failed` / `QBUF failed` / `DQBUF failed` / `POLL_FRAME failed` | OpenIMP | the driver refused the request; read `dmesg` |
| `[KernelIF] T20 VIDIOC_S_CROP failed` / `T20 crop ... exceeds bounds` | OpenIMP (T20) | crop outside the sensor window; on kernels without the legacy crop ioctls a no-crop start is accepted since 2026-11-08 |
| `H.265 not supported by the hardware on this SoC (Helix encoder is H.264/JPEG only); use H.264` | OpenIMP `Encoder` | T10/T20/T21/T23 have no HEVC hardware |
| `Helix failures, re-creating the encoder` / `... re-creations, ... channel stopped` | OpenIMP `Encoder` | encoder error limit (section 3.8) |
| `encoder was made for WxH: re-creating it` | OpenIMP `Encoder` | size changed; information |
| `clamped (...)` | OpenIMP `Encoder` | an out-of-range value was clamped |
| `Helix JPEG: off (OPENIMP_HELIX_HW_JPEG=0), using the software encoder` | OpenIMP | debug switch set |
| `openimp-p1: Failed to open i2d /dev/i2d` | OpenIMP (T40/T41) | hardware rotation device missing |
| `isp-guard: ... open ISP stack not loaded this boot` | thingino boot guard | the guard tripped or `isp_open=off/manual`; section 3.1 |
| `isp-guard: stable after Ns, pending mark cleared` | thingino boot guard | normal |
| `tx-isp: ispmem N KB too small for ... - ... disabled; set ispmem >= N KB` | driver (T10/T20) | section 3.3 |
| `tx-isp: MMAP buffers requested but isp_mmap_pool_kb=0; set isp_mmap_pool_kb=...` | driver (T10/T20) | section 3.3 |
| `no N KiB MMAP frame pool (dma_alloc_coherent failed); USERPTR only` | driver (T10/T20) | only with a non-zero pool set explicitly; remove that setting |
| `... QBUF idx=... phys=... len=... outside rmem` / `SET_BUF ... outside rmem` | driver | `qbuf_guard` refused a buffer; check `rmem=` |
| `tx_isp_vin_init: no sensor available` / `vin_probe: VIN probe failed` | driver (T31) | sensor not registered; section 3.4 |
| `ispmem isn't enough!` | driver (T30) | raise ispmem |
| `t23: no N-byte snapraw DMA buffer; reserve ispmem or set snapraw_phys_addr` | driver (T23) | raw snapshot needs ispmem |
| `t41_rawinject: isp_malloc_buffer(N) failed (ispmem/rmem?)` | driver (T41) | memory for RAW injection missing |
| Kernel WARN `Data will not fit into plane` (T10) | kernel | seen in the frame-source tick, no oops; report |
