# Open stack - feature guide (the user's view)

**Audience:** an AI assistant answering "how do I ...?", "does X work on my
camera?" and "should I use the open or the vendor stack?". State: 2026-11-08,
RC3 `v2026.11.07-rc3` plus `next`. The per-SoC state changes after every merge
round: for the exact cell, quote the live matrix
https://lu-fi.github.io/openimp/feature-matrix.html (legend: ✅ works like the
vendor, ✅+ better than the vendor, ⚠️ known defect or deviation, 🔧 in
progress, ❌ missing, — hardware has no such function, ? not documented).

Rules for answers (see [AGENTS.md](../../AGENTS.md)):

- Find out the SoC and which stack runs
  ([troubleshooting.md](troubleshooting.md#1-which-stack-is-running)).
- Each feature below says for **both stacks**: *works*, *works only with the
  open stack*, *not on this SoC*, or *check the live matrix*.
- Settings are made in the firmware's streamer or web UI; this file names the
  IMP calls, environment variables and module parameters behind them so you
  can tell what is possible. Say "the firmware's streamer" for the
  application.
- Say plainly when the vendor stack is better ([section 4](#4-where-the-vendor-stack-is-still-better-or-the-open-stack-is-not-finished)).

## 1. Feature areas

### Picture controls: brightness, contrast, saturation, sharpness, hue

- **What:** `IMP_ISP_Tuning_SetBrightness` / `SetContrast` / `SetSaturation` /
  `SetSharpness` / `SetBcshHue`, set from the image settings of the streamer.
- **SoCs:** open stack ✅ on T10, T20, T23, T30, T31, T40, T41; ✅+ on T21.
  Vendor stack: works, except that `SetBrightness` is a no-op on the vendor
  T21 (the vendor kernel only stores the value).
- **Beyond vendor:** T21 brightness acts on the open stack (scales the AE
  target by value/128; 128 = the vendor picture). T10/T20 get an AE ceiling,
  AE luma, backlight compensation and hue that the vendor T10/T20 library
  does not have.
- **Verify:** take snapshots at 0 / 128 / 255 and compare; values persist
  across day/night switches (T20, T21).

### AE compensation, max gain, exposure limits

- **What:** `SetAeComp`, `SetBacklightComp`, `SetHighlightDepress`,
  `SetMaxAgain` / `SetMaxDgain`, AE integration-time limits, `SetSensorFPS`.
- **SoCs:** open ✅ on all gate SoCs (T21 ✅+). Vendor: works.
- **Limits:** T31: the AE has about 6 s dead time and needs 25-30 s to
  settle after an AeComp change. T40: AeComp 40..200 gives Y 94..137 against
  vendor 92..142; at comp 200 the open analog gain stops at 4095 (vendor
  5247). T20: `SetMaxAgain(0)` did not cap the gain (#238), fixed in
  open-tx-isp after RC3 (needs a newer image than RC3).
- **Beyond vendor:** T21 `SetIntegrationTime` with a RANGE (exposure ceiling) acts
  (the vendor T21 ignores it); T23 has an extra ISP digital-gain AE stage.
- **Verify:** T10/T20: `/proc/jz/isp/isp-m0` ("MAX SENSOR analog gain") shows
  the reachable gain cap; otherwise compare snapshot brightness.

### White balance

- **What:** auto AWB, presets, manual gains (`SetWB`).
- **SoCs:** open ✅ on T10, T20, T23, T31, T40, T41; ✅+ T21 (hysteresis 10 %
  and IR night freeze, module parameters `awb_lux_hyst`, `awb_ir_freeze`; 0 =
  vendor behaviour). **T30 ⚠️: presets have no effect on the open stack**
  (auto and manual work; #202). Vendor: works on all, including T30 presets.
- **Limits:** T23: the open AWB runs on every 4th ISP interrupt, the vendor on
  every one (#233, no light-box proof yet).
- **Verify:** snapshot of a grey card under a known lamp; wait until
  day/night and IR cut are stable first.

### Day/night and IR cut

- **What:** the ISP side (`SetISPRunningMode`, night bank, mono) is in libimp
  and the driver; the **decision** and the IR-cut / IR-LED GPIOs belong to the
  firmware (streamer or day/night script), not to libimp.
- **SoCs:** ISP side open ✅ on all gate SoCs (T23 ✅+). IR cut open ✅ on
  T10, T20, T21, T23, T31, T40, T41; **T30: ? (check the live matrix)**.
  Vendor: works.
- **Beyond vendor:** T41 day/night works without a stream consumer and
  returns to day by itself (vendor stayed in night for 120 s in an A/B).
  T23: a block whose parameters fail to load is bypassed, user bypass bits
  survive the switch.
- **Verify:** `/proc/jz/isp/isp-m0` shows the running mode on most SoCs (not
  readable on T41); check that the IR-cut filter moves.

### Anti-flicker (50/60 Hz)

- **What:** `IMP_ISP_Tuning_SetAntiFlickerAttr`; the AE keeps the exposure in
  whole half mains periods.
- **SoCs:** open ✅ on T10, T20, T21, T23, T31, T40, T41; **T30 🔧** (round
  trip works, banding test not run). Vendor: works.
- **Verify:** integration time in `isp-m0` changes with off / 50 / 60 Hz; no
  rolling bands under mains-powered lamps.

### Flip / mirror

- **What:** `SetHVFLIP` / ISP mirror.
- **SoCs:** open ✅ on all gate SoCs. Vendor: works.
- **Notes:** T31: the MSCA flip takes effect at the next channel start (like
  the vendor). T40 sc830ai: sensor flip modes are stored and ignored (like the
  vendor). Front crop is applied before the flip: a streamer that mirrors must
  mirror the crop window too. Light box T20 (2026-11-08): the vendor `hflip`
  did not mirror the picture, the open stack did.

### Rotation 90° / 270°

| SoC | Vendor stack | Open stack |
|---|---|---|
| T10, T20, T21 | **not available** (the vendor libimp has no `IMP_FrameSource_SetChnRotate`) | **only with the open stack**, ✅+: software rotation of sub streams, up to 704x576, sides multiples of 16; device-tested 2026-11-08 on T10, T20X and T21N (15 fps, correct direction, main stream unchanged) |
| T23 | **not available** (no rotation call in the vendor library) | **only with the open stack**, ✅: software rotation up to 1280x720 (C kernel, about 15 % CPU at 720p15); device-tested on two T23N |
| T30 | **not available** (no rotation call in the vendor library) | **only with the open stack**, ✅+: software rotation (MXU2) up to 1280x720; main 720p plus sub at 25 fps cost 34.5 % process CPU |
| T31 | works (software rotation) | ✅ software rotation, 704x1280 at 9 ms per frame |
| T40, T41 | works (hardware I2D) | ✅ hardware I2D (90/180/270, flip, mirror); frame width/height multiples of 8, 90/270 width a multiple of 16 |

- **How:** the streamer's rotation setting; underneath
  `IMP_FrameSource_SetChnRotate` (software path) or the I2D attribute
  (T40/T41). An application can probe `OpenIMP_Cap_FsRotate` and
  `OpenIMP_FS_RotateLimits()` for the envelope. A channel larger than the
  envelope is refused with -1 and stays unrotated.
- **Options:** `OPENIMP_FS_ROTATE_SPARES=1|2` (no copy-back, one consumer per
  channel); `OPENIMP_MXU=0` forces the C kernel.

### Front crop / digital zoom, privacy mask, scene modes, defog, DPC, denoise

- **Front crop** (`SetFrontCrop`): open ✅ on T10..T31, T41, ✅+ T40, **T30
  🔧**. T31 refuses a window smaller than a running channel's output (the MSCA
  cannot upscale more than `fcrop_upscale_pct`, default 100 = 2.0x).
  `/sys/module/tx_isp_t23|t31/parameters/fcrop_upscale_pct` exists only in
  drivers with the safe crop path. T10/T20/T21 have no vendor `SetFrontCrop`.
- **Privacy mask** (ISP block): open ✅ on T10..T23, T31, T40, T41; T30 ?.
- **Scene mode / colour effects:** open ✅+ T10 and T21 (vendor T21 ignores
  them; sepia works on the open T21), ✅ T20, T23, T30, T31; — on T40/T41 (no
  such call in the vendor headers).
- **Defog:** open ✅+ T20, T21, T23, T41 (T41 strength call beyond the vendor
  SDK), ✅ T10, T30, T31; — on T40.
- **DPC:** open ✅+ T10, T20, T21 (strength acts, vendor no-op), ✅ T23, T31,
  T41; T30 🔧; — T40.
- **Denoise (Sinter/Temper):** open ✅+ T10, T20, T21 (strength acts, vendor
  no-op), ✅ T23, T31, T40, T41; **T30 ⚠️** (Temper: since 2026-11-07 on like
  the vendor in `next`, remaining gap tracked in #236). Light box T20: higher
  temporal noise than the vendor at matched exposure (section 4).
- **WDR / ADR / DRC:** open ✅+ T20, T21, T23 (DRC strength acts), ✅ T10, T31,
  T40, T41; **T30 ⚠️** (RawDRC unlimit strength reads above the vendor).
  T23: no real WDR (the tested sensors have no WDR mode).

### Encoder: H.264, H.265, sub stream, JPEG snapshots

| | T10 | T20 | T21 | T23 | T30 | T31 | T40 | T41 |
|---|---|---|---|---|---|---|---|---|
| H.264 (open) | ✅ | ✅ | ✅ | ✅+ | ✅+ | ✅ | ✅ | ✅ |
| H.265 (open) | — | — | — | — | ✅ | ✅ | ✅ | ✅ |
| H.265 (vendor) | — | — | — | — | works | works | works | works |
| JPEG (open) | ✅ | ✅ | ✅+ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Sub stream (open) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

- **H.265 on T10/T20/T21/T23: not on this SoC** (no HEVC hardware, either
  stack). The open stack refuses `PT_H265` with -1 and a clear log line; the
  vendor library returns 0 and creates a channel that never encodes.
- **H.265 on T30:** works on **both** stacks. Open: Radix encoder,
  vendor-equal CBR (2035 vs 2034 kbit/s at 1080p25), live rate-control
  changes since RC3 (#228); at 2048x1536 the vendor delivers no frames, the
  open stack encodes (beyond vendor at that size). Not covered on the device:
  run-time fps/GOP changes (host-tested), H.265 ROI and user data.
  `OPENIMP_T30_H265=0` switches it off; `OpenIMP_Cap_H265` tells an
  application.
- **H.265 on T40:** about equal to the vendor in CBR; **measured with the
  vendor ISP driver, not re-run on the open ISP driver.**
- **H.264 extras:** T23 native encoder without the vendor helper; T30 calm
  first stream window (start QP from bits per pixel, black start up to 0.5 s,
  start QP floor; `OPENIMP_T30_START_QP=vendor`, `OPENIMP_T30_BLACK_START=0`,
  `OPENIMP_T30_START_FLOOR=0` restore the vendor start).
- **JPEG:** T21 hardware JPEG without the vendor helper (snapshots 0.05-0.18
  s). T20 snapshot latency 0.20 s vs vendor 0.45 s (night A/B).
- **Limits:** T40 4K IDR about 2.5x the vendor size; T41 I pictures of
  detailed scenes about 35 % larger at the same QP. T41 channel 1 is limited
  to `t41_msca_ch1_max_w` x `_h` (default 1920x1080).
- **Verify:** play the stream and look at the picture (a clean decode is not
  proof); the encoder logs an "effective rate control" line per channel.

### Encoder: rate control, GOP, ROI

- **Modes** (CBR / VBR / FixQP / Capped* / SMART): open ✅ on T20, T21, T23,
  T31, T40, T41, ✅+ T10; **T30 🔧** (FixQP and SMART not run on the device).
  RC parameters (QP steps, staticTime, changePos, qualityLvl, I bias): ✅
  except **T30 🔧**; **on T10/T20/T21 they are accepted but ignored** (mode,
  bitrate, QP range, GOP and fps act).
- **T31 deviations:** CBR writes no filler NAL (static scenes stay below the
  target); CappedQuality behaves like CappedVBR (less than the vendor).
- **Beyond vendor:** T10 VBR super-frame fix (re-encodes 800 -> 0, CPU 8.3 ->
  5.5 %); T20 I-aware CBR budget; T31 Capped modes are real closed loops; T41
  recovers within about 1 s after a light step (vendor holds max QP for about
  2.5 s); QP / fps out of range are clamped with a warning.
- **ROI** (region QP): open ✅+ T21 (vendor never programs it), T31 (no ROI
  API in the vendor T31 library), T41 (also under FixQP); ✅ T10, T20, T23,
  T40; **T30 🔧**. Switches: `OPENIMP_T21_ROI`, `OPENIMP_T23_ROI`,
  `OPENIMP_T31_ROI`, `OPENIMP_T41_ROI` (all default on, `0` = off).

### OSD

- **What:** text, bitmap, rectangle, line, cover (`IMP_OSD_*`), and on some
  SoCs ISP-drawn OSD.
- **SoCs:** open ✅ on T10, T20, T21, T23, T31, T41; ✅+ T40 (ISP draw window
  and line work); **T30 🔧** (not yet checked in the picture). Vendor: works.
- **Limits:** T41 ISP-drawn OSD draws into the main stream only (as the
  vendor). T23: odd OSD widths are refused (they stop the ISP).

### Motion detection (IVS)

- **SoCs:** open ✅ on T10, T21, T23, T40, T41; ✅+ T20 (6/6 events, 0 false
  alarms) and T31 (about 85 % less IVS CPU than the vendor library); **T30
  🔧** (device test pending). Vendor: works.
- **Beyond vendor:** motion v2 (bounding boxes, strength, false-alarm
  suppression), on by default; `OPENIMP_MOTION_V2=vendor` restores the vendor
  algorithm. `OPENIMP_IVS_MOVE_INTERVAL` saves CPU.
- **Verify:** walk through the picture; `OPENIMP_MOTION_V2_LOG=1` (debug)
  prints each decision change.

### Audio: input, output, echo cancellation

| | T10 | T20 | T21 | T23 | T30 | T31 | T40 | T41 |
|---|---|---|---|---|---|---|---|---|
| Input (open) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — (no audio hardware on the test board) |
| Output (open) | ✅ | ✅ | ✅ | ✅ | 🔧 | ✅ | 🔧 | — |
| AEC (open) | ✅ ERLE 25 dB | ✅ 24 dB | ✅ 20 dB (vendor 16 dB) | ✅ 19 dB | 🔧 | ✅+ (ERLE 44 dB in loopback) | 🔧 | — |

- Vendor stack: works. On **T30 and T40 speaker output and AEC are not
  finished on the open stack** - recommend the vendor stack if the user needs
  two-way audio there today. T41 audio is untested (no hardware on the test
  board); say so.
- `IMP_DMIC_Enable` returns -1 on T40/T41 when the audio module has
  `dmic_enable=0` (the vendor library oopses the kernel there).

### Test pattern

- Synthetic pictures fed into the pipeline, **only with the open stack**, all
  SoCs, default off. Modes: 1 ramp / checkerboard / colour bars / logo, 2
  mode 1 + moving square + counter, 3 motion test objects, 4 the sensor's own
  pattern, 5 pattern inside the ISP (T23 only).
- **How:** `OPENIMP_TEST_PATTERN=<1..3>` at start, or
  `OPENIMP_TEST_PATTERN_CTL=<socket>` and `tpctl set <mode> ...` at run time;
  API `OpenIMP_TestPattern_Set`. Some firmware streamers offer it in their UI.
- Per-SoC state: see the wiki page Test-Pattern (mode 1 ✅ on all but T30 🔧).

### Memory (rmem / ispmem)

- `rmem` (video memory for OpenIMP) and `ispmem` (driver) are set on the
  kernel command line by the boot loader. OpenIMP logs `rmem peak X KB of Y
  KB` and, on failure, how much to add (`raise rmem by at least M KB (suggest
  rmem=<n>M)`).
- Open stack savings: T21 free rmem with main + sub + MJPEG 2.76 MB vs about
  1.2 MB on the vendor; T21/T23 reference sharing saves about 1.5 MB at
  1080p (`OPENIMP_REF_SHARE`); T23 JPEG shares the bitstream area (-1.44 MB);
  libimp 0.33-0.73 MB vs vendor 1.0-1.36 MB.
- **Verify:** read the `rmem peak` line with the real stream set; leave
  headroom. Details: [troubleshooting.md](troubleshooting.md#33-memory-rmem--ispmem-too-small-oom).

### Reset diagnostics

- After an unplanned reboot: `dmesg | grep "RESET ERROR"`,
  `/proc/diag_lastpanic`, `resetcause` lines in the syslog. T31 keeps a short
  panic record in the RTC; T23 encodes it in the reset PC ("sled"). Needs the
  diagnostic options of the firmware image. Details:
  [troubleshooting.md](troubleshooting.md#310-unexpected-reboots---reset-diagnostics).

## 2. Only with the open stack (beyond vendor)

Built from `docs/OPENIMP_BEYOND_VENDOR.md` and the ✅+ matrix cells. "Since"
= the first release that contains it.

| Feature | SoC | Default | How to use / switch | Limits | Since |
|---|---|---|---|---|---|
| Rotation 90/270 where the vendor has no call | T23 (✅), T30 (✅+), T10/T20/T21 (🔧) | on when the application asks | `IMP_FrameSource_SetChnRotate`; probe `OpenIMP_FS_RotateLimits` | 704x576 (T10/T20/T21), 1280x720 (T23/T30); CPU cost | T23/T30: RC3; T10/T20/T21 code since 2026-10-05, device test pending |
| H.265 at 2048x1536 | T30 | on | `PT_H265`; `OPENIMP_T30_H265=0` off | vendor delivers no frames at that size; H.265 itself also works on the vendor at 1080p | RC3 |
| Calm first stream window | T30 H.264 | on | `OPENIMP_T30_START_QP`, `_BLACK_START`, `_START_FLOOR` (`vendor` / `0` = vendor start) | start QP map fitted on the SC4236 only | RC3 |
| Fallback frame "NO SENSOR SIGNAL" | T40, T41 | on | `OPENIMP_FALLBACK_FRAME=0` off, `OPENIMP_FALLBACK_TIMEOUT_MS` | - | RC3 |
| Test pattern with run-time control | all | off | `OPENIMP_TEST_PATTERN`, `tpctl` | mode 5 T23 only | see CHANGELOG |
| Motion v2 (boxes, strength, false-alarm suppression) | IVS SoCs | on | `OPENIMP_MOTION_V2` | static appearing objects are ignored by design | RC1 (default on since 2026-10-04) |
| Brightness acts | T21 | on (128 = vendor) | `SetBrightness` | exposure-based, a few seconds to settle | RC1 |
| Sepia, scene / colour effects act | T21 (also T10) | off until set | `SetColorfxMode`, `SetSceneMode` | - | RC1 |
| Sinter/Temper/DPC strength act | T10, T20, T21 | 128 = vendor picture | tuning strength calls | - | RC1 |
| AE ceiling / backlight / hue calls | T10, T20 (AE ceiling, backlight also T21) | - | `SetAe_IT_MAX`, `GetAeLuma`, backlight, hue | - | see CHANGELOG |
| Encoder ROI where the vendor has none or ignores it | T21, T31 (H.264), T41 under FixQP | on | `IMP_Encoder_Set*Roi*`; `OPENIMP_<SOC>_ROI=0` off | T31: H.264 only | T21/T31 RC1, T41 RC2 |
| Real Capped VBR / Capped Quality | T31 | on | rc mode | CappedQuality = CappedVBR | RC1 |
| Defog strength | T41 | - | `Set/GetDefog_Strength` | - | 2026-10-25 (see CHANGELOG) |
| Custom AWB algorithm library actually runs; AE library released when the client dies | T41 | on | `IMP_ISP_SetAwbAlgoFunc`, `SetAeAlgoFunc`; `OPENIMP_ISP_ALGO_FUNC=0` off | - | RC3 |
| Day/night without stream consumer | T41 | on | - | - | RC2 (2026-10-24) |
| ISP bypass graph (RAW) | T23, T30, T31, T41 | off | `OPENIMP_<SOC>_BYPASS_GRAPH=1` (+ `isp_bypass_graph=1` on T23/T31/T41) | bring-up feature, not for production | RC3 |
| Lower CPU / memory / size | most SoCs | - | nothing to do | measured per SoC (matrix rows 34-38) | - |
| Removes `libalog` / `libsysutils` / helper daemons | T10..T41 | - | nothing to do | - | RC1 (T40: RC2) |
| Clear refusal of H.265 on SoCs without HEVC | T10, T20, T21, T23 | on | check the `CreateChn` return | - | see CHANGELOG |

## 3. Quick answer table: "Can I ... on my SoC?"

| Want | T10 / T20 / T21 | T23 | T30 | T31 | T40 / T41 |
|---|---|---|---|---|---|
| Rotate 90/270 | only open stack (🔧, small sub streams) | only open stack | only open stack | both stacks | both stacks (hardware I2D) |
| H.265 | not on this SoC | not on this SoC | both stacks (open also 2048x1536) | both | both (T40 open: measured on the vendor ISP driver only) |
| AWB presets | both | both | vendor only works today (open ⚠️) | both | both |
| Speaker / two-way audio with AEC | both | both | vendor recommended (open 🔧) | both | T40: vendor recommended (open 🔧); T41: untested |
| Motion detection | both | both | open 🔧, vendor works | both | both |
| OSD | both | both | open 🔧 in the picture, vendor works | both | both |
| Anti-flicker | both | both | open 🔧, vendor works | both | both |
| Test pattern | only open stack | only open stack | only open stack | only open stack | only open stack |
| Fallback picture when the sensor stops | no | no | no | no | only open stack |
| Anything not listed | check the live matrix | | | | |

## 4. Where the vendor stack is still better or the open stack is not finished

From the matrix (🔧 / ⚠️ / ? cells), the light-box protocols
(`docs/lightbox/`, website) and the release notes. Recommend the vendor stack
when one of these matters to the user.

| SoC | Item | Evidence |
|---|---|---|
| all | **Release 1 is not reached**: the 14-day run without an unplanned reset on the RC3 image is still open; the open stack is a release candidate | release gates |
| T30 | ⚠️ WDR / RawDRC (unlimit strength above the vendor); auto exposure about 0.3 EV off the vendor in the dark (#202); 🔧 anti-flicker, DPC, front crop / CSC presets, OSD in the picture, IVS, rate-control modes (FixQP/SMART not run), RC parameters, ROI, audio output, AEC; `?` for 13 cells (for example IR cut, CCM/LSC, privacy mask); fixed on 2026-11-07/08: AWB presets, Temper / P-picture size (#236), module reload; one lab camera only | matrix T30 column, RC3 known limitations |
| T20 | temporal noise 15-22 % higher than the vendor at matched exposure, day AE about 7 % darker (open: closer to the chart colours, more saturated) | light box 2026-11-08 |
| T10 | night picture brighter than the vendor (Y 87 vs 76, longer exposure, less noise; accepted); tone curve differs at matched exposure | light box 2026-11-07 |
| T23 | open AWB every 4th interrupt (#233); rare single Helix encode error; no real WDR | RC3 note, matrix |
| T31 | CappedQuality = CappedVBR (vendor lowers QP further in static scenes, about 1-2 QP finer); CBR without filler | OPENIMP_BEYOND_VENDOR.md |
| T40 | only the sc830ai board device-tested; speaker output and AEC 🔧; analog gain stops at 4095 at comp 200 (vendor 5247); 4K IDR about 2.5x vendor size; H.265 not re-run on the open ISP driver | matrix, RC3 note |
| T41 | fair light-box comparison not done (#230); audio untested (no hardware on the test board); AF statistics only with `t41_af=1`; `IMP_ISP_SetVicDoneCbFunc`, `IMP_ISP_RAW_RwControl`, `IMP_Decoder_*` return -1 (ENOTSUP); I pictures about 35 % larger at the same QP | matrix, OPENIMP_BEYOND_VENDOR.md |
| T40, T41 | `IMP_FrameSource_SetYuvAlign` refuses alignments that need padding; `IMP_EmuFrameSource_*` not implemented | OPENIMP_BEYOND_VENDOR.md |
| T32, T33 | planned only; use the vendor stack | - |

## 5. Example questions and answers

**Q: How do I rotate the image?**
A: It depends on the SoC. On T31, T40 and T41 both stacks rotate (T40/T41 in
hardware). On T23 and T30 rotation works **only with the open stack**
(software rotation up to 1280x720 with some CPU cost). On T10, T20 and T21 the
vendor library cannot rotate; the open stack has it for sub streams up to
704x576, but the device test is still pending (🔧). Use the rotation setting
of the firmware's streamer; if it is refused, the channel is larger than the
envelope.

**Q: Does H.265 work on my T30?**
A: Yes, on both stacks. With the open stack the Radix encoder gives
vendor-equal CBR (measured 1080p25: 2035 vs 2034 kbit/s), live bitrate
changes work since RC3, and 2048x1536 works where the vendor delivers no
frames. Not yet tested on the device: run-time fps/GOP changes.

**Q: Can I use H.265 on my T23 (or T10/T20/T21)?**
A: No. These SoCs have no HEVC hardware on either stack. The open stack
refuses it with a clear log line; the vendor stack silently creates a channel
that never sends frames. Use H.264.

**Q: Why is my picture magenta / pink in daylight?**
A: The documented case was the IR-cut filter staying open in day mode (the
filter is driven by the firmware through a GPIO, not by the library). Check
the day/night and IR-cut state first. Then compare with the vendor image on
the same camera; if only the open stack shows it, report it with SoC, sensor,
`dmesg` and `logread`.

**Q: I need two-way audio with echo cancellation on my T40. Open or vendor?**
A: Today the vendor stack. On the open stack speaker output and AEC on T40 are
still in progress (🔧, sound tests pending). Audio input works on both.

**Q: My T30 white-balance presets ("incandescent", "daylight") do nothing.**
A: Known deviation of the open stack on T30 (#202): presets have no effect,
auto and manual white balance work. If you need the presets, use the vendor
stack until the matrix cell turns ✅.

**Q: Is the open stack better than the vendor stack?**
A: Only where measured. Examples: lower CPU (T20 application 10.5 % vs 25.2 %,
T40 streamer 0.34 % vs 2.26 %), smaller libimp, more free video memory on
T21, features the vendor lacks (rotation on T23/T30, test pattern, fallback
frame). Elsewhere it is behind (see section 4), and Release 1 is not reached
yet. Check the live matrix and the light-box protocols for your SoC.

**Q: The camera rebooted by itself. What do I send?**
A: `IMAGE_ID` and `BUILD_ID` from `/etc/os-release`, which stack runs, SoC
and sensor, `dmesg | grep "RESET ERROR"`, `/proc/diag_lastpanic` if present,
`logread`, and whether there was a power cut. Open an issue at
https://github.com/Lu-Fi/openimp/issues.

**Q: How do I know if my camera runs the open stack?**
A: `strings /usr/lib/libimp.so | grep openimp` prints `libimp.so <SOC>
openimp` with OpenIMP. The driver name is the same in both stacks; an
open-only parameter such as `/sys/module/tx_isp_t31/parameters/qbuf_guard`
shows the open driver.
