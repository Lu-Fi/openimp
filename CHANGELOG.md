# Changelog (OpenIMP)

**Where to read what**

- **This file**: release-oriented summary per area (what changed against the vendor stack and against the original upstream line, [opensensor/openimp](https://github.com/opensensor/openimp)), followed by the condensed OpenIMP-only log by date.
- **Full campaign log**: [docs/OPEN_STACK_CHANGELOG.md](docs/OPEN_STACK_CHANGELOG.md): every change of OpenIMP, open-tx-isp, timps and the thingino integration since 2026-09-30, per session, with device-test evidence and numbers. The same text is on the wiki page *Changelog*.
- **Current state per feature and SoC**: [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md) (HTML view: [docs/feature-matrix.html](docs/feature-matrix.html)); what goes beyond the vendor: [docs/OPENIMP_BEYOND_VENDOR.md](docs/OPENIMP_BEYOND_VENDOR.md).

Release plan: `next` goes to `aperto` (fast-forward only) after the long soak of the release candidate and is tagged `vYYYY.MM.DD`; until then dates are the reference. The first release covers T10, T20, T21, T23 and T31; T10 has not been re-tested on the open stack today ("not re-tested"); T41 is not part of the first release. Branch names (`claude/...`) are historic: the topic branches were merged into `next` and deleted. Cameras are anonymised: cam-A (T31), cam-B (T23), cam-C (T20), cam-D (T21), cam-E (T10), cam-F (T41); cam-G and cam-H are further T23 cameras, cam-I a second T20, cam-J a second T21.

## Release candidate agg-34 (state of 2026-10-09 evening)

OpenIMP `agg-34` (838f8147) = agg-32 (8980cae) + encoder ROI clamp (fec3e69, 8545fb9) + reviewed DeepSeek patches (11abe42 and 17 patches); open-tx-isp `agg-34` (da9baf1e) = agg-32 (3cf9bdac) + front-crop fixes for T31, T23, T20 and T10 (d940ba20 to 10bac52f, 164225e5) + T20 `isp-m0` gain cap (da9baf1e). The first release still covers T10, T20, T21, T23 and T31; T41 is experimental and not part of it. The combination agg-34 has not run on the cameras as a whole; the individual fixes below were device-tested on the SoC named.

- **agg-32 soak (2026-10-07 16:07 to 2026-10-09 11:31, 43.4 h):** 0 unplanned reboots, 0 oopses, 0 guard trips, 0 snapshot failures and no memory trend on the seven timps cameras without user interaction; the only reboots were a planned OTA and two crashes from a front crop on T31 (fixed below). The picture check found the T20 day/night regression, a T10 black frame in the dark and the T41 channel-1 problem (known issues).
- **Encoder ROI:**
  - **T31** gets an ROI that the vendor library does not have (`IMP_Encoder_Set/GetChnRoiAttr` through the AVPU QP table, H.264, `OPENIMP_T31_ROI=0` refuses it): device-tested, relative -10 gave 2.7x the bit rate, 0 decoder errors in 20 VA-API checks.
  - **T10/T20/T21** apply `SetChnROI` and the chroma QP offset on the Helix/NVPU (T21 by default, beyond vendor, `OPENIMP_T21_ROI=0` switches it off): device-tested.
  - **Valid H.264 on every path:** window deltas -25..+25, spread at most 25, picture QP plus delta inside 0..51 and the rate control's min/max QP; a relative -26 used to give `mb_qp_delta` +26 at the window edge, which Intel VA-API, VLC, Firefox and Edge show as broken blocks (software ffmpeg hides it). Requests beyond that are clamped with a one-time warning.
  - **T23:** regions go to the vendor encoder, no visible effect (no-op). **T41:** code only, off unless `OPENIMP_T41_ROI=1`.
- **Front crop (open-tx-isp, device-tested):**
  - **T31:** a window smaller than a channel's scaler output stalled every MSCA output until reboot; the kernel now refuses it (-EINVAL), drops a crop that a larger main stream cannot fit, and writes the scaler steps so a valid window zooms. The picture effect on T31 is experimental, not proven.
  - **T23:** crop off unlocks the window, a window that does not fit is dropped instead of hanging, the lock is released with the ISP session (the 2026-10-06 crop hang is fixed).
  - **T20/T10:** a window is kept while no downscaled channel is open and applied at stream on, and released with the tuning session.
  - **T31/T23:** module parameter `fcrop_upscale_pct` (default 0) lets the geometry check tolerate a measured upscale (T23: up to 2.0).
- **T20 day/night:** `isp-m0` reported an analog gain cap of 158 while the AE stops at 128, so the streamer never saw the cap reached; it now reports the smaller value (host-tested; the streamer needs its matching change, timps 5bc4eff).
- **DeepSeek patches:** vendor ABI struct sizes (T40/T41), encoder group bounds (6; 8 kept on T23/T40), T31 IVS channel 64, `OSD_SetPoolSize(0)` rejected, idempotent T31 IVS group create/destroy, `PollingStream` guards, `GetDefaultBinPath`. Host-tested and cross-built; device checks pending.
- Details: [docs/OPEN_STACK_CHANGELOG.md](docs/OPEN_STACK_CHANGELOG.md), section "2026-10-07 to 2026-10-09: agg-32 soak and release candidate agg-34"; per-function state: [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md); ROI rules: [docs/ROI.md](docs/ROI.md); beyond-vendor items: [docs/OPENIMP_BEYOND_VENDOR.md](docs/OPENIMP_BEYOND_VENDOR.md).

### Known issues (agg-34)

- **T41 sub-channel (channel 1):** green or flat frames at each day/night switch and the exposure of channel 1 does not follow channel 0; T41 is not part of the first release.
- **T20 day/night** needs the driver fix (da9baf1e) and the streamer change (timps 5bc4eff); with agg-32 alone both T20 cameras stayed in Day mode in the dark.
- **T31 front crop:** the crash is fixed, the picture effect is not proven.
- **T10:** occasional switch to Day in the dark with a black frame (3 events in 44 h).
- The older candidate's known issues below apply unless stated here; the T23 crop hang is fixed.

## Summary by area (state of 2026-10-06 evening)

All test cameras run the open stack (open-tx-isp kernel driver + OpenIMP + timps) from full OTA images built from thingino `aperto`; no Ingenic or neo helper libraries remain, and T23 runs without helixd and without vendor libimp.

### Release candidate (OpenIMP `agg-27`, open-tx-isp `agg-28`)

| SoC | In the first release | apitest of 2026-10-06 | Notes |
|---|---|---|---|
| T10 | yes, **not re-tested** | not run | userspace is the T20 build, cells mirror T20 |
| T20 | yes | 212 PASS / 0 FAIL on two cameras | `GetAeZone` is a vendor no-op (N/A); encoder ROI and chroma QP offset |
| T21 | yes | 228 PASS / 0 FAIL on two cameras | `SetBrightness` acts and sepia works (beyond vendor); antifog/scene are vendor no-ops |
| T23 | yes | 304 PASS / 4 FAIL (AF getters, fixed by `source_af=1`) | stock-like release defaults, MSCA scratch buffer; H.265 has no hardware; **crop hang open** |
| T31 | yes | 274 PASS / 1 FAIL | `SetFrameDrop` with stock semantics; H.264 stall after JPEG teardown fixed (agg-29) |
| T41 | **no** (experimental) | 255 PASS / 12 FAIL on the experimental branch | channel 1 scaling registers staged, output-restart hang, 38 tuning IDs missing |

- **Frame source and encoder (all SoCs):** `GetFrame`/`SnapFrame` wait up to 2 s like the vendor library and the T20 capture no longer stalls (apps that set a frame depth got no frames before); `Encoder_GetFd` returns a pollable pipe; a JPEG channel feeds itself from the frame source when its video channel is not polled (beyond vendor); two IVS groups as every vendor libimp; `InsertUserData` SEI; `PollingModuleStream` honours the channel bitmap.
- **T20/T10/T21:** encoder ROI and H.264 chroma QP offset on the Helix/NVPU (T20 device-tested; T21 stores ROI/QPG/SuperFrame like the vendor and applies them after the release).
- **T21:** `SetBrightness` acts through the AE target and SEPIA works (both beyond vendor); `SetDPStrength`/`SetAntiFogAttr` exported like the vendor.
- **T23:** pool rmem parked as on T21, driver defaults are the stock-like set that ran 7 h overnight, STREAMOFF drain wait and QBUF cache invalidate (no cold-start snapshot 503), MSCA scratch buffer for stopped channels (beyond stock), AF statistics on by default, 19 ISP getters routed, `GetSensorRegister` offset fixed.
- **T31:** stack overruns of `GetAwbZone`/`GetAWBCt` fixed (a bus error in apitest), `SetFrameDrop` with the stock lsize/fmark semantics, `EnableDefog` takes its flag, vendor Movestate logic (host-tested).
- Vendor no-ops are matched, not "fixed": T21 antifog/scene, T20/T10 `GetAeZone`, T23 `GetBlcAttr`. T23 H.265 stays refused (no hardware).
- Details: [docs/OPEN_STACK_CHANGELOG.md](docs/OPEN_STACK_CHANGELOG.md), section "2026-10-06 afternoon/evening: release candidate agg-27/agg-28"; per-function state: [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md); beyond-vendor items: [docs/OPENIMP_BEYOND_VENDOR.md](docs/OPENIMP_BEYOND_VENDOR.md).

### OpenIMP (userspace libimp)

- **Against the vendor library**
  - Functions that were stubs or silent no-ops in the vendor stack now act: T21 controls and noise reduction, T10 noise-reduction strength, `ae_it_max_us` on T21, T23 brightness/contrast/saturation/hue, getters that return what was set.
  - Smaller: libimp about 0.5-0.6 MB instead of 1.0-1.3 MB (`gc-sections`, tables generated instead of copied); more free video memory on T21 (2.76 MB instead of about 1.2 MB); reference-frame sharing on T21/T23 (about 1.5 MB less video memory, on by default).
  - More robust: checked inputs, rate-limited error logs, bounded waits, clean stop/reload (0 oops in 10 cycles per SoC); clear errors instead of silent `-1`/0 (for example H.265 on SoCs without HEVC fails at once so streamers fall back to H.264).
  - Beyond the vendor API (documented for streamer authors in OPENIMP_BEYOND_VENDOR.md): motion detection v2 with bounding boxes and strength (`OPENIMP_MOTION_V2=0` restores the vendor algorithm), rmem high-water logging and shortfall hints, effective rate control logged per channel.
- **Against the original opensensor/openimp line**
  - Encoders: real HEVC and hardware JPEG on T31; native Helix H.264 on T23 (no vendor helper); hardware JPEG on T20/T21/T23 without the vendor library; H.265 on T41 (AVPU).
  - Rate control: vendor Allegro core on T31, OEM controllers on T10/T20 (default), eprc on T21/T23 (0 oracle deviations), capped modes mapped properly instead of silent CBR.
  - Image and audio: OSD (text, bitmap, rectangle, line) on T20/T21/T30, real frame-diff motion detection on T20/T21/T30, software rotation on T31, real AECM echo cancellation on T31 (-18 dB echo), volume/mute and whole-fragment audio writes.
  - SoC coverage: T41 stream buffers, flip, BCSH and unload fixes; T30 builds against a real kernel (no device in the campaign).
  - Hardening from audits and an independent review: NULL crashes, buffer overflows (T31 AF/AE getters, module-chain dump, OSD size, ABI struct sizes), EINTR and `O_CLOEXEC` handling, T23 reconfigure race.

### open-tx-isp (kernel driver)

- **Against the vendor driver**: lifecycle hardening on all SoCs (locking, use-after-free, STREAMOFF races, last-close races, bounded tuning access, checked user copies); module reload clean on every SoC including T41; modules smaller than the vendor ones on T21 (452 vs 616 KB), T23 (622 vs 857 KB) and T31 (711 vs 829 KB); sensor module pinned while the ISP is open; sensor registry under `/proc/jz/sensor`; optional 8 MB MMAP pool on T10/T20; unknown control IDs are rejected instead of silently succeeding.
- **Against the original line**: exposure readback and vendor-format `isp-m0` on all T-series; T21 stock AE, ADR and control dispatchers lifted instruction by instruction (night flicker gone); T23 about 45 control IDs wired and the vendor AE lifted (default on); T20 simple AE/AWB with limits; T31 AF/AE statistics, SensorAttr and WaitFrame through the driver; T41 gc5603 tuning fix and the channel-restart hang fix.

### timps and thingino

- timps changes are made by the timps maintainers; OpenIMP only provides what the streamer calls (for example AE IT max reset to 0).
- thingino: packages `openimp` and `open-tx-isp` are in upstream branch `aperto` ([#1756](https://github.com/themactep/thingino-firmware/pull/1756)), pinned to the Lu-Fi forks; kernel VPU/rmem stability patches (#1748, #1752), optional boot guard (#1749) and SC2336 flip fixes (#1750, #1751) are merged there.

### Known issues (release candidate)

- **T23 crop hang:** the pipeline stops after a FrameSource crop change; under investigation, release blocker.
- **T31 H.264 stalls after the JPEG channel is torn down:** fixed: two threads polled the same encoder channel at once (`AL_Codec_Encode_Process` concurrently); `IMP_Encoder_PollingStream` now serialises per channel (openimp `claude/release-fix27` 5c2ccef), apitest on T31 FAIL 0 with the fix; goes into the next candidate (agg-29).
- **T41 is not part of the first release:** channel 1 scaling registers are staged, an output restart hangs the SoC, 38 tuning IDs are missing (branches `claude/release-t41` and open-tx-isp `claude/t41-ch1-fix`, not merged).
- T21: the OSD blend takes effect only about 2 s after a wake from idle (the first snapshot waits for it); cause not found.
- The colour image functions must be re-tested in daylight (the candidate's imgfx run was made in the dark); T10 was not re-tested.

### Known open items (older)

T23 sporadic single Helix encode error (errno 5; the frequent frame drops are fixed) and T23 real WDR; T41 flip, night column noise, short IVS gaps, day/night and AE/AWB quality; T21 a 4th module reload in one boot crashed once; AEC device tests on T23/T21/T20 (no speaker tests on shared cameras); first release tag after the 24 h soak. Details: the "Still open" section of the full log and the feature matrix.

## OpenIMP changes by date (condensed)

Only OpenIMP (userspace libimp) changes, newest first. Everything listed was device-tested on the SoC named unless marked otherwise.

## 2026-10-09

- Release candidate `agg-34` (838f8147): agg-32 plus the ROI clamp and the reviewed DeepSeek patches (see the section above).
- ROI: every window delta is limited to -25..+25 (spread at most 25, picture QP plus delta inside the rate control's min/max QP) on T31 and on the T10/T20/T21 Helix command lists, so hardware decoders no longer show broken blocks; under FixQP the T31 table is not clamped to the RC range. T31 measured: relative -20 became -19, 0 decoder errors in 20 VA-API checks.
- DeepSeek review: vendor ABI layouts, encoder group bounds (6; 8 on T23/T40), T31 IVS channel 64, `OSD_SetPoolSize(0)`, idempotent T31 IVS group create/destroy, `PollingStream` guards (host-tested; device checks pending).
- open-tx-isp (not OpenIMP): front-crop guards on T31/T23/T20/T10 and the T20 `isp-m0` gain cap, see the section above.

## 2026-10-07 to 2026-10-08 (agg-30 to agg-32)

- T10/T20/T21: encoder ROI and chroma QP offset on the Helix/NVPU; T21 `SetChnROI` effective by default (beyond vendor, `OPENIMP_T21_ROI=0` switches it off); `roitest` tool.
- T31: encoder ROI through the AVPU QP table (`IMP_Encoder_Set/GetChnRoiAttr`, beyond vendor, `OPENIMP_T31_ROI=0` refuses it); T41: experimental ROI table, off by default (`OPENIMP_T41_ROI=1`).
- Soak of agg-32 for 43.4 h on ten cameras (see the section above).

## 2026-10-06

- Release candidate `agg-27` (merge of the release branches); the apitest/imgfx FAILs of the day were fixed or classified (T20 212 PASS / 0 FAIL, T21 228 / 0, T23 304 / 4 with the AF getters fixed afterwards, T31 274 / 1). See the summary above and the full log.
- FrameSource T20 (`claude/t20-fs-frames`): `REQBUFS` count = pool size (+ max delay) and `SET_BANKS` as the vendor library; before, apps that set a frame depth got `nrVBs` frames and then nothing. `SetFrameDepth(chn, 0)` succeeds.
- FrameSource T20/T21/T23/T31: `GetFrame`/`SnapFrame` wait up to 2 s like the vendor library; the T20 capture thread no longer sleeps in `DQBUF` without a queued buffer (`claude/release-fs-enc-ivs`, `claude/release-quickwins`).
- Encoder: `IMP_Encoder_GetFd` (pipe plus pump thread); a JPEG channel reads the frame source itself when its video channel is not polled; `InsertUserData` emits a `user_data_unregistered` SEI on T20/T21/T23; `PollingModuleStream` honours the input bitmap; T31 `SetMaxStreamCnt` refused on a created channel.
- IVS: two groups as every vendor libimp (group 1 move/base-move device-tested on T20/T21).
- T31: `GetAwbZone` passed a stack struct of pointers where the driver copies 675 bytes (bus error); `GetAWBCt` always returned -1; `EnableDefog` always wrote 1 (`claude/release-t23-getters`, `claude/release-deepseek-defog`).
- T23: `GetSensorRegister` bus-type offset, `FrameSource_GetPool`, monotonic audio frame stamps (`claude/release-t23-getters`); the pool rmem is parked at `DisableChn` as on T21 (`claude/release-t23-vbm`, 40 streamer restarts, every snapshot OK, 0 oopses; `OPENIMP_VBM_PARK=0` frees at once).
- T21: `SetDPStrength` and `SetAntiFogAttr` exported like the vendor T21 libimp (DP strength reaches the DPC ratio, antifog goes to control 0x8000163 which the OEM kernel accepts without effect) (`claude/release-t21-image`). `SetBrightness` acts through the AE target (beyond vendor, driver change in open-tx-isp `claude/release-t21-brightness`; imgfx Y 28.8/117.7/193.0 for 30/128/225, timps `image.brightness` live 44.5/130.8/223.9). SEPIA works (open-tx-isp `claude/release-t21-image`).
- Core: `IMP_Log_Set_Option` exported; `DisableSensor` passes -1 to `DESTROY_LINKS` like stock (patches from an automated review, verified against the vendor disassembly).
- Release quick wins (host-tested only): T21 `SetModuleControl` cache fallback, `AI_SetHpfCoFrequency(0)` accepted on T40/T41, P3 capture restart guard, T21 OSD INFO lines trace-only.
- Pending (host-tested and cross-built only): `IMP_AI_SetHpfCoFrequency` takes effect on T23/T31 (float biquad in libaudioProcess-neo, beyond the original library); T31 Enable/DisableMovestate run the vendor logic; T21 `QpgMode`/`H265TransCfg` as the vendor; `SetbufshareChn` documented as store-only.
- imgfx: the green/magenta stripe in the top rows of the saved ch1 pictures was a tool error (chroma plane read at the wrong offset); streams and snapshots were never affected.
- Known issues of the candidate: T23 crop hang (under investigation); T31 H.264 stall after JPEG teardown fixed for agg-29, T41 not part of the release.

## 2026-10-05

- T10/T20/T21: front crop and CSC presets (beyond vendor, device-tested: crop, rejection of invalid values, 10 cycles), software rotation 90/270 on sub streams (T21 with OSD, colours correct), CSC mode 4 sign fix (green picture on T10/T20), encoder ROI and chroma QP offset (T20 device-tested; QP51 region coarse, QP15 fine, absolute QP bypasses CBR).
- T20/T21/T23/T31/T41: many vendor functions connected that were stubs, cache-only or missing (audit of every vendor IMP/SU function per SoC, see the feature matrix): AE/AWB zone and statistics getters, WaitFrame, ModuleControl, delay FIFO (`SetMaxDelay`/`SetDelay`/`GetTimedFrame`), `FlushStream`, AENC/ADEC on all SoCs, `IMP_AO_CacheSwitch`, T41 `SnapFrame`.
- T31: stack overruns fixed and verified with guard arenas: `GetAfHist` wrote 88 bytes into the 24-byte vendor struct, `Get/SetAeAttr` overran the caller by 80 bytes; AF statistics chain rebuilt (focus value, zones, weights, histogram).
- T23: `SetGamma` takes effect at once (steep/linear curves visible, falling curve rejected, restore ok).
- T21: the first snapshot after a start or an idle wake had no OSD (the IPU blend has no effect for about 2 s after a wake); OpenIMP now verifies the blend on a glyph pixel and withholds JPEG frames without a confirmed overlay (beyond vendor: the vendor stack shows the same missing OSD).
- T41: restarting one channel while the other streams hung the SoC; a driver fix (flip update only on a real change, output kept like the vendor on stream-off) passed the targeted repro 35/35 and a 40 min soak; the hang returns once the MSCA output geometry is latched correctly (see the known issues above, T41 is not part of the first release).

## 2026-10-04

- thingino: `openimp` and `open-tx-isp` are part of upstream `aperto` ([#1756](https://github.com/themactep/thingino-firmware/pull/1756)), pinned to the Lu-Fi forks; the T23 OEM Helix helper option and hybrid install are gone. All test cameras run `aperto` images (30/30 snapshots, 0 oops, 0 VPU errors).
- T41: stream buffers sized like the vendor, rmem arena split so an idle 1080p stream can always restart, no software H.264 stub (it produced a corrupt stream); AE compensation, gain/exposure caps and noise reduction reach the ISP.
- Pending (on a branch, not yet in `next`): the audio capture read size that always covers whole driver fragments (ported from an upstream patch, author credited), after the 24 h soak.
- T20/T10: rmem high-water logging and shortfall messages with a concrete suggestion when reserved memory is too small instead of silent degradation.
- Hardening: repeated driver errors rate-limited, silent `-1` returns logged, four NULL crashes, a buffer overflow in the module-chain dump, lost items on EINTR and an OSD size overflow fixed.
- T31: `SetChnQpIPDelta` now updates the value `GetChnAttrRcMode` returns, as the vendor does.
- T23: OSD stride fix (clean date/time text on main and sub stream); sub-stream reference fix; the OEM Helix helper option and the hybrid install are removed, T23 runs fully open. The frequent Helix frame drops are fixed (residual interrupt 0x100, kernel patch merged upstream); still open: a rare single Helix encode error (errno 5).
- T21/T23: sub-stream (640x360) corruption from reference sharing on the small channel fixed.
- T41: H.265 on the AVPU path (like T31); a stuck AVPU job times out after 2 s and resets the core. CBR overshoot fix, unload/flip/BCSH fixes in `next`.
- Review fixes: complete `O_CLOEXEC`, eprc `FRAME_END` for dropped pictures, T31 Allegro RC lock, forced IDR after a YUV error, T20 MB-RC table bounds.
- Size and CPU: T20 MB-RC table 590 to 58 KiB, no per-frame malloc; libimp T31 -34 KB, T41 -20 KB, T21 -17.5 KiB text; JPEG Huffman parsing by table, OSD cache invalidation, T31 EBSP copy (timps CPU T31 8.7 to 7.9 %, T21 17.5 to 15.5 %).
- Motion detection v2 (on by default; `OPENIMP_MOTION_V2=0` restores the vendor algorithm, bit-identical): background model per grid cell, suppression after IR/exposure switches, blob grouping, bounding boxes and strength through the versioned `OpenIMP_IVS_MoveGetResultEx` API; false alarms down in overnight runs.
- T30 readiness without hardware: builds for a real T30 kernel; fixed an `IMPEncoderCHNAttr` ABI size bug (4-byte overrun).
- `next` branch created; it tracks the tested aggregate (fast-forward only).

## 2026-10-03

- T10/T20/T21/T23 H.265: `IMP_Encoder_CreateChn(PT_H265)` fails with -1 and one log line on SoCs without HEVC hardware, so streamers fall back to H.264 at once (the vendor returns 0 and creates an empty channel).
- Rate control, T31: vendor Allegro core ported (VBR, CappedVBR, CappedQuality, CBR), state-identical against traces; closed-loop VBR; no filler NAL in CBR (deviation).
- Rate control, T21/T23 (eprc): vendor controller ported with 0 oracle deviations, default on T21; FIXQP, scene-cut IDR, runtime RC/fps/GOP/HSkip, `SetChnHSkip`; opt-in macroblock-level RC (`OPENIMP_EPRC_MBRC=1`).
- Rate control, T20/T10: vendor controllers as default (T20 I-aware P budget, T10 super-frame fix: re-encodes 800 to 0, CPU 8.3 to 5.5 %).
- T41: bitrate setting had no effect (negative bucket level discarded) fixed; OpenIMP runs against the vendor T41 driver with video, JPEG, OSD and motion detection.
- T10: picture drifting diagonally fixed (reference border added twice); noise-reduction strength acts (the vendor does nothing).
- T20: snapshot debounce no longer polls the JPEG encoder (14.4/15.0 fps instead of 11.2/14.3 with 1 snapshot/s); 270-line sub stream rounded to 272 instead of a scaler hang; bottom-row chroma fix.
- T23: brightness/contrast/saturation/hue act; JPEG shares the H.264 bitstream area (-1.44 MB); sub-stream rotation 90/270 on the native encoder; `IMP_Encoder_YuvSetCrop`; daylight green cast fixed by keeping white balance across stream restarts.
- T21: reference-buffer sharing on by default (~1.4 MB less video memory; `OPENIMP_REF_SHARE=0` disables); user contrast sent instead of default.
- Helix: encoder re-created after 3 failed pictures, channel stops after 2 fruitless re-creates; JPEG bitstream buffer 1 MiB (-328 KiB on a T10).
- Vendor logging functions built into libimp; `libalog`/`libsysutils` no longer needed on images.
- Size: `gc-sections` (T23 libimp 774 to 726 KB, T20 694 to 594 KB).
- Encoder diagnostics: effective rate control logged per channel, out-of-range QP/fps clamped with a warning.

## 2026-10-02

- T31: hardware JPEG default; real HEVC on the AVPU; software rotation 90/270; lambda tables generated from a formula; real AECM echo cancellation (-18 dB echo on a speech loopback).
- Helix (T20/T21/T23): hardware JPEG without the vendor library (~95 % less CPU for snapshots); dedicated JPEG/MJPEG channel gets frames; OSD lines, rectangles and bitmaps drawn on T31/T20/T21/T30.
- T23: native Helix H.264 encoder is the default, no vendor helper or vendor libimp; RC parameters reach the encoder; bitstream overflow handling (frame dropped and QP raised instead of a stuck channel).
- T21: shared bitstream buffer sized like the vendor (main/sub switching works), EMC scratch sized like the vendor (free rmem 1.56 to 2.76 MB).
- Robustness audits: AEC reference queue heap overflow, audio-effect switch use-after-free, double stop, DQBUF/EPIPE races at channel stop.
- Independent review: T23 reconfigure divide-by-zero race fixed; top-level `NOTICE` added.

## 2026-09-30 to 2026-10-01 (campaign start)

- ISP tuning: vendor defaults for contrast/sharpness; SDK control IDs and pointer semantics on T20/T21; T23 `GetSensorAttr` buffer overrun; T31 AF IDs and driver-backed SensorAttr/WaitFrame/ModuleControl.
- Day/night on T20/T21 re-sends image controls like the vendor.
- Encoder: CappedVBR/CappedQuality/SMART mapped to VBR with a log line instead of silent CBR; JPEG quality applied; channel-stat struct layout fixed.
- Motion detection: real frame-diff IVS on T20/T21/T30 (was always "no motion").
- Audio out: volume/mute applied, whole-fragment writes.
- T20/T21 framesource: pool parking and reuse instead of freeing on idle teardown.
- Tools: `t23tune` for T23 tuning on device.
