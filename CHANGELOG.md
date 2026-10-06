# Changelog (OpenIMP)

Condensed from the open-stack campaign changelog; only OpenIMP (userspace libimp) changes.
Newest first, grouped by date. Everything listed was device-tested on the SoC named unless
marked otherwise. Branch names refer to the `claude/*` topic branches merged into `next`.
Release tags `vYYYY.MM.DD` on the `aperto` branch are planned (the first one after the 24 h soak that started 2026-10-04); until then dates are the reference. Branch names are historic: the topic branches were merged into `next` and deleted.

## 2026-10-07 (branch `claude/roi-all`, not part of the first release)

- T21: `IMP_Encoder_SetChnROI` is effective by default (beyond vendor: the stock T21 libimp stores the regions but never programs them). With no region set the command list is unchanged; `OPENIMP_T21_ROI=0` switches it off. Device-tested on the PC420 (jxf23, 1920x1080, FixQP 30, one 320x320 region): absolute QP 51 is a visible block, QP 15 stays clean, delta +/-15 works, live changes (P frames, no IDR) work. FixQP bitrate 657 kbit/s without ROI, 340 with QP 51, 504 with QP 15; CBR keeps the target (the bits move between the regions). Test tool `tools/roitest`.
- T41: `IMP_Encoder_SetChnRoiAttr/GetChnRoiAttr` (vendor T41 API, 10 windows, delta QP) exist but are experimental and off (`OPENIMP_T41_ROI=1`): the macroblock QP table in EP2 was identified from a vendor capture and is written, but the AVPU did not react on the test camera; default behaviour unchanged (see `docs/ROI.md`).
- T31: encoder ROI (beyond vendor, branch `claude/t31-roi`): `IMP_Encoder_SetChnRoiAttr/GetChnRoiAttr` with the T41 API, 10 windows, delta QP -32..31 or absolute 0..51, live without IDR. The vendor Allegro core shows the switch: cmd[9] bit 25 = use the EP2 macroblock QP table, bit 24 = relative; the table is one byte per macroblock after the 0x40-byte EP2 header. Device-tested on cam-A (sc4336p 2560x1440): FixQP 30 with a 768x512 window, delta +20 / absolute 51 blurs exactly the window (IDR 176 to 156 KB), absolute 10 sharpens it (247 KB), two windows at once, CBR keeps the target; 0 oops. `OPENIMP_T31_ROI=0` switches it off.
- T41 (code only): the same enable bits found in the vendor T41 1.2.6 (`SliceParamToCmdRegsEnc1`: cmd[152] bit 0 = use table, bit 3 = relative); OpenIMP set neither, which explains why the T41 table had no effect. Set now under `OPENIMP_T41_ROI=1`, not yet device-tested.

## 2026-10-06

- T31: `IMP_FrameSource_GetFrame`/`SnapFrame` wait up to 2 s like the vendor libimp (T31 1.1.6 header: default timeout 2 s) instead of failing at once (branch `claude/release-quickwins`, host-tested only). The T31 encoder pulls with `VBMGetFrame` and keeps its own deadline. T41 already waits (about 1 s of DQBUF retries, `openimp_p1.c`) and is unchanged.
- Release quick wins from the 2026-10-06 review (`claude/release-quickwins`, host-tested only): T21 `IMP_ISP_Tuning_SetModuleControl` falls back to the cache when the open T21 driver rejects control 0x80000e2 (T31 still fails); `IMP_AI_SetHpfCoFrequency(0)` is accepted on T40/T41 (0 = default, as on T31); the P3 capture start is refused while the previous capture thread has not exited (no free of the buffer it still uses); the T21 OSD retry/probe and JPEG fan-out INFO lines are trace-only (`openimp_debug_trace_enabled`).
- T23: the FrameSource pool's rmem is parked at DisableChn, as on T21 (branch `claude/release-t23-vbm`). The open T23 driver keeps a stopped MSCA output enabled while the input runs, so the pool is no longer handed to other allocations while the MSCA may still write into it. The block is freed by `DestroyChn`, or at once with `OPENIMP_VBM_PARK=0`.
  - Tested on the Jooan A6M (raptor) together with open-tx-isp `claude/release-t23-driver`: 40 streamer restarts with every snapshot OK, 0 oopses.
- T21: `IMP_ISP_Tuning_SetDPStrength` and `IMP_ISP_Tuning_SetAntiFogAttr` are exported like the vendor T21 libimp (an application built against the T21 SDK did not load without them). DP strength (vendor percentage, cap 200) reaches the open driver's DPC ratio; AntiFog goes to control 0x8000163 like the vendor, which the OEM kernel accepts without effect.
- imgfx: the green/magenta stripe in the top rows of the saved ch1 pictures was a tool error (chroma plane read at width*height instead of after ALIGN16(height) lines); JPEG snapshots and streams were never affected.
- Pending (branch `claude/streamer-gaps-t1x-t23-t31`, host-tested and cross-built only, no device test yet): `IMP_AI_SetHpfCoFrequency` takes effect on T23/T31 (and, untested, T41): `IMP_AI_EnableHpf` designs the filter for the cut-off like the vendor library (illegal for a negative value or above the sample rate; 0 = default tables) and hands it to libaudioProcess-neo as a float biquad at the stream's rate (beyond the original library, which takes libimp's coefficient pointer; neo is recognised by its DRC export); `IMP_AO_EnableHpf` reaches neo the same way. T31 `IMP_ISP_Tuning_Enable/DisableMovestate` run the vendor library's logic (AE block of control 0x800002c, day/night limits from `/etc/sensor/<sensor>move.txt` loaded by EnableTuning, IDR on channel 0 on disable; the stock T31 driver ignores the control). T21 `IMP_Encoder_Get/SetQpgMode` keep any value like the vendor library (nothing reads it there), `IMP_Encoder_Get/SetH265TransCfg` check and drop like the vendor (Get returns zeros). `IMP_Encoder_Get/SetJpegeQl` on T10/T20/T21 was already applied to the running encoder (host test added). `IMP_Encoder_SetbufshareChn` on T31 is store-only in the vendor library too.
- T21 `IMP_ISP_Tuning_SetBrightness` now acts (beyond vendor; the vendor T21 kernel only stores the value): needs open-tx-isp `claude/release-t21-brightness`, no libimp change. Device-tested on the PC420 (jxf23): brightness 30/128/225 gives Y 28.8/117.7/193.0, timps `image.brightness` live the same. 128 is the vendor picture.

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
