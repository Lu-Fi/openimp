# OpenIMP environment variables

**Applies to OpenIMP `next` of 2026-11-08 (RC3 `v2026.11.07-rc3` plus later
commits).** This list is complete: it holds every `OPENIMP_*` name that
`src/` reads with `getenv()` (directly, through a wrapper, or built from a
name prefix), **173 variables**: 54 user, 59 debug only, 60 internal.

How they work:

- They are read by `libimp.so` **inside the application process** (the
  firmware's streamer or any application using the IMP API). Set them in the
  environment of that process (its init script), not in a login shell.
- Most are read once (at library start, channel or interface creation), so
  the application must be restarted after a change.
- They exist only in OpenIMP. With the vendor libimp they do nothing.
- Class: **user** = safe to set in production; **debug only** = diagnostics
  or A/B rollback, do not set in production; **internal** = bring-up or trace
  switch, undocumented on purpose, never recommend it to a user.
- SoC "Helix SoCs" = T20, T21, T23, T30 (and T10 through the T20 build);
  "p2 encoder" = the shared encoder front end.
- The defaults are those of the source; the source wins over this file.

Capability symbols (not variables) that an application can probe with a weak
`extern`: `OpenIMP_Cap_H265` (T30: 1 = H.265 on the Radix encoder, 0 when
`OPENIMP_T30_H265=0`), `OpenIMP_Cap_T23HelixRoi` (T23 encoder ROI),
`OpenIMP_Cap_FsRotate` and `OpenIMP_FS_RotateLimits()` (software rotation
envelope; T10/T20/T21 704x576, T23/T30 1280x720, T31 4096x4096),
`OpenIMP_Cap_TestPattern` (test pattern ABI).

Build-time switch (not an environment variable): `OPENIMP_DEBUG=1` compiles
the debug-level log messages in.

## Logging and diagnostics

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_DEBUG_TRACE` | set = on | Very verbose tracing; forces the debug log level. | all | debug only |
| `OPENIMP_LOGGER_DEV` | device path; default `/dev/log_main` | Logger device of the vendor-style log sink (host tests). Ignored in a set-id process. | all | debug only |
| `OPENIMP_LOG_FILE` | path; default `timp.log` in the working directory | File for the vendor-style log file sink. Ignored in a set-id process. | all | debug only |
| `OPENIMP_LOG_LEVEL` | `0`..`7` (syslog priorities, one digit); default `5` | Log level of libimp. `OPENIMP_DEBUG_TRACE` forces debug. Debug-level messages exist only in a build with `OPENIMP_DEBUG=1`. | all | user |
| `OPENIMP_LOG_SYSLOG` | `1` = on; default off | Also send libimp log lines to syslog (otherwise stderr of the application). | all | user |
| `OPENIMP_PROFILE` | set = on | Periodic profile report. | all | debug only |
| `OPENIMP_PROFILE_INTERVAL` | number of completed frames | Interval of the profile report. | all | debug only |
| `OPENIMP_STARTUP_TRACE` | set = on | Verbose start-up tracing. | all | debug only |

## Memory (rmem, reference sharing, buffers)

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_DISABLE_RMEM` | `1` = skip `/dev/rmem` | Do not use `/dev/rmem` (old T31 workaround). | T31 | debug only |
| `OPENIMP_QBUF_FORCE_PHYS` | set | Queue frame buffers by physical address (the default). | all | internal |
| `OPENIMP_QBUF_USE_VIRT` | non-zero | Queue frame buffers by virtual address (experiment only). | all | internal |
| `OPENIMP_REF_SHARE` | `0` = off; default on | Reference-frame sharing ring of the Helix encoder (saves about 1.5 MB rmem). | T21, T23 | user |
| `OPENIMP_REF_SHARE_B0` | hex | Ring byte-0 override. | T21, T23 | internal |
| `OPENIMP_REF_SHARE_DEBUG` | `1` | Debug output of the reference ring. | T21, T23 | debug only |
| `OPENIMP_REF_SHARE_FLAGS` | hex | Ring flags override. | T21, T23 | internal |
| `OPENIMP_REF_SHARE_GAP` | number | Tuning of the reference ring. | T21, T23 | debug only |
| `OPENIMP_RMEM_BASE` | address; both BASE and SIZE required, page aligned | Override the rmem window taken from the kernel command line; an invalid pair is rejected with a log line. | all | debug only |
| `OPENIMP_RMEM_MAP` | set = on | Also log every live rmem allocation. | all | debug only |
| `OPENIMP_RMEM_NO_REUSE` | `1` | Old allocator behaviour without block reuse. | all | debug only |
| `OPENIMP_RMEM_SIZE` | size; see `OPENIMP_RMEM_BASE` | Size of the overridden rmem window. | all | debug only |
| `OPENIMP_RMEM_START_OFFSET` | offset into rmem | Start the T40/T41 allocator at an offset inside the rmem window (bring-up). | T40, T41 | internal |
| `OPENIMP_RMEM_TRACE` | set = on | rmem allocation tracing. | T23 | debug only |
| `OPENIMP_VBM_PARK` | `0` = free at once; default on | Park the frame-source pool of a disabled channel for the next pool instead of freeing it (T23 default, as on T21). | T21, T23 | debug only |

## Frame source, fallback frame, rotation, MXU

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_3A_FEED` | `0` off, `hi` highest enabled channel, anything else / unset lowest channel | Feed the idle 3A channel so AE/AWB/day-night run while no client pulls frames. | T40 | user |
| `OPENIMP_3A_FEED_DIV` | 1..30; default 1 | Feed only every n-th frame interval. | T40 | user |
| `OPENIMP_FALLBACK_FRAME` | `0` = off; default on | When the sensor delivers no frame for the dwell time while a consumer waits, send a synthetic picture ("NO SENSOR SIGNAL" band, frame counter, blinking square) so streams stay up; switches back when real frames return. `OpenIMP_FrameSource_SetFallback` changes it at run time. | T40, T41 | user |
| `OPENIMP_FALLBACK_TIMEOUT_MS` | 200..60000; default 2000 | Dwell time before the fallback frame starts. | T40, T41 | user |
| `OPENIMP_FS_ROTATE_MAX_PIXELS` | pixels (up to 4096x4096); default 704x576 on T10/T20/T21, 1280x720 on T23/T30 | Overrides the size cap of the frame-source software rotation (source comment: tests only); larger channels are refused and stay unrotated. | T10, T20, T21, T23, T30 | debug only |
| `OPENIMP_FS_ROTATE_SPARES` | `1`/`2`; default off | Rotate into spare buffers instead of copying back (one consumer per channel; `VBMLockFrame` does not see spare-backed records). | software-rotation SoCs (T10, T20, T21, T23, T30, T31) | user |
| `OPENIMP_FS_ROTATE_STATS` | non-zero | Print rotation statistics. | software-rotation SoCs | debug only |
| `OPENIMP_MXU` | `0`/`off` = C kernels only; default on | MXU2 SIMD kernels (today: channel rotation). Each kernel runs a bit-exact selftest once and is used only when it passes. Never probed on T23. | T10, T20, T31, T30 (MXU2 SoCs) | user |

## Test pattern

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_TEST_PATTERN` | start mode `1`..`3` | Start with a synthetic test picture (see Test pattern). | all | user |
| `OPENIMP_TEST_PATTERN_CTL` | socket path | Control socket for `tpctl` to switch the test pattern at run time. | all | user |
| `OPENIMP_TEST_PATTERN_EVENTS` | path | Event script of mode 3 (motion test). | all | user |
| `OPENIMP_TEST_PATTERN_NOISE` | number | Noise of the motion-test pattern. | all | user |
| `OPENIMP_TEST_PATTERN_STEP` | number | Grid step of the pattern. | all | user |
| `OPENIMP_TP_LOGO` | `0`/`1` | Logo in the pattern. | all | user |

## ISP

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_ISP_ALGO_FUNC` | `0` = off; default on | Serve the custom AE/AWB algorithm libraries registered with `IMP_ISP_SetAeAlgoFunc` / `SetAwbAlgoFunc`. | T40, T41 | debug only |
| `OPENIMP_SOURCE_STATS` | set | Periodic source statistics output. | T31, T40, T41 | debug only |
| `OPENIMP_T23_BYPASS_GRAPH` | `1` = on; default off | `IMP_ISP_Tuning_SetISPBypass(DISABLE)` runs the vendor link rebuild with a RAW channel; needs the driver parameter `isp_bypass_graph=1`. Without it DISABLE is refused (-1). | T23 | user |
| `OPENIMP_T30_BYPASS_GRAPH` | `1` = on; default off | As above for T30 (no driver parameter needed). RAW frames are 12-bit Bayer, MSB-aligned in 16 bits. | T30 | user |
| `OPENIMP_T31_BYPASS_GRAPH` | `1` = on; default off | As above for T31; needs `isp_bypass_graph=1` (raw frames, RG12). | T31 | user |
| `OPENIMP_T41_BYPASS_GRAPH` | `1` = on; default off | Optional ISP bypass RAW16 channel; needs `isp_bypass_graph=1`. | T41 | user |

## Motion detection (IVS)

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_IVS_MOVE_INTERVAL` | 2..1000; default 1 | Analyse only every n-th frame for motion (saves CPU). | T31 IVS path | user |
| `OPENIMP_IVS_VENDOR_ORDER` | `1` | Vendor ordering of IVS results (A/B). | T31 IVS path | internal |
| `OPENIMP_MOTION_V2` | unset / `1` / `on` / `all` = all features (default); `0` / `off` / `vendor` = vendor algorithm; `shadow` = all but override; a number = feature mask | Motion v2: bounding boxes, strength, false-alarm suppression on top of the vendor motion detection. | IVS SoCs (T31 path; see the matrix row 29) | user |
| `OPENIMP_MOTION_V2_BG` | `0`/`1` | Motion v2 feature bit: background model. | as `OPENIMP_MOTION_V2` | user |
| `OPENIMP_MOTION_V2_BLOBS` | `0`/`1` | Feature bit: blob / bounding boxes. | as above | user |
| `OPENIMP_MOTION_V2_DUMPFD` | file descriptor number | Dump motion v2 data to an open descriptor. | as above | internal |
| `OPENIMP_MOTION_V2_JUMP_PCT` | percent | Global-jump threshold (light change). | as above | user |
| `OPENIMP_MOTION_V2_K` | integer | Threshold factor. | as above | user |
| `OPENIMP_MOTION_V2_LEARN` | integer | Learning shift of the background model. | as above | user |
| `OPENIMP_MOTION_V2_LOG` | `1` | One stderr line per change of the vendor or v2 motion decision. | as above | debug only |
| `OPENIMP_MOTION_V2_MIN_CELLS` | integer | Minimum cells of a blob. | as above | user |
| `OPENIMP_MOTION_V2_MIN_DELTA` | integer | Minimum per-cell change. | as above | user |
| `OPENIMP_MOTION_V2_MIN_FRAMES` | integer | Minimum frames before a blob counts. | as above | user |
| `OPENIMP_MOTION_V2_MIN_MOVE` | integer | Minimum movement of a blob. | as above | user |
| `OPENIMP_MOTION_V2_OVERRIDE` | `0`/`1` | Feature bit: v2 overrides the vendor decision. | as above | user |
| `OPENIMP_MOTION_V2_SUPPRESS` | `0`/`1` | Feature bit: false-alarm suppression. | as above | user |
| `OPENIMP_MOTION_V2_SUPPRESS_MS` | ms | Suppression time after a global change. | as above | user |
| `OPENIMP_T31_IVS_STATS` | set | Periodic IVS statistics. | T31 IVS path | debug only |

## OSD

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_OSD_RETRY` | `0`/`1`; on by default on T21, off elsewhere | Retry an OSD update that failed. | T21, T31 | debug only |
| `OPENIMP_OSD_RETRY_IDLE_MS` | ms | Idle time of the OSD retry. | T21, T31 | debug only |
| `OPENIMP_OSD_RETRY_WINDOW_MS` | ms | Window of the OSD retry. | T21, T31 | debug only |
| `OPENIMP_T23_OSD` | `0` = off; default on | T23 OSD backend. | T23 | debug only |
| `OPENIMP_T31_OSD` | `0` = off; default on | IPU OSD backend on T31. | T31 | user |

## Audio and system

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_AEC_STATS` | `1` (other non-`0` values select variants) | Periodic audio echo-cancellation (AEC) statistics. | T31 audio code (also used by T10, T20, T21) | debug only |
| `OPENIMP_AI_STALE` | `0` = off; default on | Drop the cleared fragments the OSS2 capture driver hands out a second time after a false overrun (0.95 s of silence, #266); `0` passes them on as the vendor library does. | T10, T20, T21, T30, T31 (`/dev/dsp`) | debug only |
| `OPENIMP_AI_STALE_LOG` | `1` = stderr, a path = append to that file; default off | One line per such run (fragments dropped, or passed on with `OPENIMP_AI_STALE=0`). | T10, T20, T21, T30, T31 (`/dev/dsp`) | debug only |
| `OPENIMP_AO_CACHE` | `1` = on; default off | Audio-output cache (opt-in). | audio SoCs | internal |
| `OPENIMP_SU_SHUTDOWN` | unset = SIGTERM to init (reboot on busybox); `vendor` = SIGCHLD; `poweroff` = SIGUSR2 | What `SU_Base_Shutdown` does. | all | user |

## Encoder: rate control

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_EPRC_MBRC` | `1` = on; default off | Macroblock-level RC inside eprc. | T21, T23 | user |
| `OPENIMP_EPRC_MBRC_LOG` | number | Log of the macroblock RC. | T21, T23 | debug only |
| `OPENIMP_EPRC_QP_DOWN1` | `1` = CBR only, `2` = all modes; default off | Limit how fast QP may fall (device test pending). | T21, T23 | user |
| `OPENIMP_T10_RC` | `0` = old GOP controller; default on | OEM-style rate controller for CBR/VBR on T10. | T10 | user |
| `OPENIMP_T10_RC_SUPERFRM` | `0` = vendor-exact VBR; default on | T10 VBR super-frame fix (inside the OEM controller). | T10 | user |
| `OPENIMP_T20_MBRC` | `1` = on, any other value = off; unset = the channel's macroblock-RC setting (on by default since RC3) | Macroblock rate control inside the T20 OEM controller. | T20 | user |
| `OPENIMP_T20_RC` | `0` = old GOP controller; default on | T20 OEM rate controller (CBR/VBR; needs the kernel patch for the NVPU statistics registers). | T20 | user |
| `OPENIMP_T20_RC_IAWARE` | `0` vendor P budget; `1` also for VBR/SMART; default on for CBR | I-frame-aware P budget inside the T20 OEM controller. | T20 | user |
| `OPENIMP_T20_RC_STATS` | set | Rate-controller statistics. | T20 | debug only |
| `OPENIMP_T21_EPRC` | `0` = old GOP controller; default on | Vendor-identical eprc rate controller. | T21 | user |
| `OPENIMP_T23_EPRC` | unset = eprc for SMART only; `1` also CBR/VBR; `0` never | eprc rate controller selection on T23. | T23 | user |
| `OPENIMP_T23_RC_STATS` | set | Rate-controller statistics. | T23 | debug only |
| `OPENIMP_T23_SMART_IDR_GOPS` | number | GOPs between IDRs in SMART mode (override). | T23 | internal |
| `OPENIMP_T30_BLACK_START` | `0` = live from picture 0; default on | Black pictures until the open AE settled (at most 8 pictures / 0.5 s), then a live IDR. | T30 | user |
| `OPENIMP_T30_EPRC` | `0` = old GOP controller; default on | OEM picture rate controller (CBR/VBR). | T30 | debug only |
| `OPENIMP_T30_EPRC_STATS` | seconds; default 0 (none) | Interval of rate-controller statistics lines. | T30 | debug only |
| `OPENIMP_T30_START_FLOOR` | `0` = off; default on | Controller min QP = start QP for the first GOP after a controller start. | T30 | user |
| `OPENIMP_T30_START_QP` | unset/`auto` = from bits per pixel; `10`..`51` fixed; `vendor` = 26 | QP of the first IDR of an H.264 stream (calm first window, beyond vendor). | T30 | user |
| `OPENIMP_T31_RC_CORE` | `legacy` = pre-Allegro core; default `allegro` | Rate-control core on T31. | T31 | user |
| `OPENIMP_T31_VBR_LOOP` | `0` = open-loop VBR | Debug A/B of the closed-loop VBR. | T31 | debug only |
| `OPENIMP_T40_FIXQP_LDA` | knob | T40 FixQP lambda update (bring-up knob). | T40 | internal |
| `OPENIMP_T40_FIXQP_NOHWRC` | knob | T40 FixQP without hardware RC words (bring-up knob). | T40 | internal |
| `OPENIMP_T40_HWRC_TARGET` | knob | T40 hardware RC target words (bring-up knob). | T40 | internal |
| `OPENIMP_T41_FIXQP_HWRC` | `1` = keep HW RC; default off | Hardware rate control under FixQP. | T41 | debug only |
| `OPENIMP_T41_FIXQP_LDA` | `0` = integer lambda table; default on | Vendor dynamic lambda under FixQP. | T41 | debug only |
| `OPENIMP_T41_RATE_CONTROL_COUPLING` | `0` = roll back | T41 rate-control coupling (A/B rollback). | T41 | debug only |

## Encoder: region of interest

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_DEBUG_CHROMA_QP` | set | Chroma QP debug output. | p2 encoder | internal |
| `OPENIMP_DEBUG_ROI` | set | ROI debug output. | T31, T40, T41 (p2 encoder) | internal |
| `OPENIMP_T21_ROI` | `0` = off; default on | Encoder ROI programming. | T21 | user |
| `OPENIMP_T23_ROI` | `0` = off; default on | Encoder ROI on the native Helix encoder; probe `OpenIMP_Cap_T23HelixRoi`. | T23 | user |
| `OPENIMP_T31_ROI` | `0` = refuse; default on | `IMP_Encoder_SetChnRoiAttr` on T31 (beyond vendor). | T31 | user |
| `OPENIMP_T40_ROI` | `0` = off; default on | Encoder ROI. | T40 | debug only |
| `OPENIMP_T41_ROI` | `0` = refuse; default on | T41 encoder ROI (device-tested under CBR and FixQP; under FixQP the QP range is opened while a window is active). Older wiki text says default off; the source has it on. | T41 | user |

## Encoder: codec paths and internals

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_HELIX_EARLY_CREATE` | `1` | Create the Helix encoder early (bring-up). | Helix SoCs | internal |
| `OPENIMP_HELIX_LAZY_CREATE` | `1` | Create the Helix encoder lazily (bring-up). | Helix SoCs | internal |
| `OPENIMP_HELIX_SOC` | `t10` / `t20` | Override the SoC probe of the combined T10/T20 build. | T10, T20 | debug only |
| `OPENIMP_T23_HELIX_TIMEOUT_MS` | 100..20000; default 20000 (OEM) | Shorter Helix wait, for testing the failure limit. | T23 | debug only |
| `OPENIMP_T30_FSC` | hex word | Replaces the REG_MCE_FSC word of the P-picture command list (#236 debug). | T30 | internal |
| `OPENIMP_T30_H265` | `0` = off; default on | Radix H.265 path; `0` refuses `PT_H265` and sets `OpenIMP_Cap_H265` = 0. | T30 | user |
| `OPENIMP_T30_HELIX_TIMEOUT_MS` | 100..20000; default 20000 (OEM) | Shorter Helix wait, for testing the failure limit. | T30 | debug only |
| `OPENIMP_T31_AU_GATE` | `0` = publish malformed AUs (log only) | Access-unit sanity gate (A/B). | T31 | debug only |
| `OPENIMP_T31_AVC_LEGACY` | `1` | Pre-fix T31 H.264 recovery path. | T31 | debug only |
| `OPENIMP_T31_COMPANION_STAGE` | `0` = skip | AVPU JPEG companion core per H.264 frame. | T31 | debug only |
| `OPENIMP_T31_DMA_DRAIN_US` | microseconds | Late-DMA drain before the payload copy. | T31 | internal |
| `OPENIMP_T31_DUMP_SOURCE_DIR` | directory | Dump source frames. | T31 | debug only |
| `OPENIMP_T31_FULL_FRAME_STATS` | set | Periodic full-frame statistics. | T31 | debug only |
| `OPENIMP_T31_HEVC_CABAC_INIT` | `0`; default on | H.265 cabac_init_flag signalling. | T31 | internal |
| `OPENIMP_T31_HEVC_HWRC` | `0` = picture-level QP; default on | H.265 hardware RC (with PPS cu_qp_delta). | T31 | internal |
| `OPENIMP_T31_HEVC_TMVP` | `0`; default on | H.265 temporal MV prediction. | T31 | internal |
| `OPENIMP_T31_STREAM_READ_UNCACHED` | `1` | Read the stream buffer uncached. | T31 | internal |
| `OPENIMP_T40_CMD12` | knob | Encoder command word 0x12 (bring-up knob). | T40 | internal |
| `OPENIMP_T40_DRAIN_CHECK` | set | Debug comparison against the old fixed drain. | T40 | internal |
| `OPENIMP_T40_DRAIN_VERIFY` | knob | Verify the payload instead of a fixed 2 ms drain. | T40 | internal |
| `OPENIMP_T40_HEVC_CABAC_INIT` | `0`; default on | H.265 cabac_init_flag signalling. | T40 | internal |
| `OPENIMP_T40_HEVC_HWRC` | `0` = picture-level QP; default on | H.265 hardware RC. | T40 | internal |
| `OPENIMP_T40_HEVC_TMVP` | `0`; default on | H.265 temporal MV prediction. | T40 | internal |
| `OPENIMP_T40_HEVC_TOOLS` | bit mask 0..15; default 13 | PPS tool flags of the T40 H.265 core. | T40 | internal |
| `OPENIMP_T40_IGNORE_STATUS_SIZE` | set | Ignore the payload size from the status word. | T40 | internal |
| `OPENIMP_T40_POC` | knob | Picture order fields in the command list (bring-up knob). | T40 | internal |
| `OPENIMP_T40_SOURCE_INVALIDATE` | knob; `0` skips | Cache invalidate of the source frame. | T40 | internal |
| `OPENIMP_T40_STREAM_CLEAR_EXTENT` | knob | Clear only the dirty extent of the stream buffer. | T40 | internal |
| `OPENIMP_T41_DUMP_CMD` | set | Dump encoder command lists. | T41 | internal |
| `OPENIMP_T41_DUMP_FIRST_AU` | directory | Dump the first access unit. | T41 | internal |
| `OPENIMP_T41_HEVC_CABAC_INIT` | `0`; default on | H.265 cabac_init_flag signalling. | T41 | internal |
| `OPENIMP_T41_HEVC_FIXQP_CUQPD` | `0` = old PPS (such streams do not decode); default on | H.265 FixQP PPS with `cu_qp_delta_enabled_flag` 1 like the vendor. | T41 | debug only |
| `OPENIMP_T41_HEVC_HWRC` | `0` = picture-level QP; default on | H.265 hardware RC. | T41 | internal |
| `OPENIMP_T41_HEVC_TMVP` | `0`; default on | H.265 temporal MV prediction. | T41 | internal |
| `OPENIMP_T41_STREAM_COPY_MODE` | `0` = roll back | T41 stream copy mode (A/B rollback). | T41 | debug only |
| `OPENIMP_T41_UNCACHED_COMMAND_RING` | `0` = roll back | Uncached command ring (A/B rollback). | T41 | debug only |
| `OPENIMP_T41_UNCACHED_EP3_RING` | `0` = roll back | Uncached EP3 ring (A/B rollback). | T41 | debug only |

## T23 Helix internals

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_PERSIST_TRACE_FILE` | path | Persistent trace file. | T23 | internal |
| `OPENIMP_T23_ENCODER` | `worker` (with the OEM helper) | Select the OEM worker encoder instead of the native one (the OEM helper is not installed by thingino). | T23 | internal |
| `OPENIMP_T23_ENCODE_TRACE` | set | Trace of the first encoded pictures. | T23 | internal |
| `OPENIMP_T23_ENC_PARAMS` | `0` = off | Encoder parameter path. | T23 | internal |
| `OPENIMP_T23_HELIX_BSF` | `1`; needs a patched kernel | Hard bitstream limit on T23 (device test open). | T23 | debug only |
| `OPENIMP_T23_HELIX_BS_KIB` | 256..4096 KiB | Bitstream window size (testing). | T23 | internal |
| `OPENIMP_T23_HELIX_COPY` | `1` | Helper copy mode. | T23 | internal |
| `OPENIMP_T23_HELIX_HELPER` | path | Path of the OEM Helix helper. | T23 | internal |
| `OPENIMP_T23_HELIX_IDR` | `0` = off; default on | Forward an IDR request to the Helix encoder. | T23 | internal |
| `OPENIMP_T23_HELIX_RMEM` | phys:size | rmem slice of the helper. | T23 | internal |
| `OPENIMP_T23_HELIX_RMEM_KB` | KB | Size of the helper rmem slice. | T23 | internal |
| `OPENIMP_T23_HELIX_STRICT_STATUS` | `1` | Also require ENDFLAG (retry once, then drop and restart the GOP). | T23 | internal |
| `OPENIMP_T23_HELIX_WORKER` | set (inside the helper) | Marks the helper process; prevents recursion. | T23 | internal |
| `OPENIMP_T23_NATIVE_FALLBACK` | `0` = off | Fallback to the native encoder. | T23 | internal |
| `OPENIMP_T23_PACE_STATS` | seconds | Pacing statistics interval. | T23 | internal |
| `OPENIMP_T23_SHARED_BS` | set | Attach the shared bitstream area. | T23 | internal |
| `OPENIMP_T23_SKIP_HELIX` | set | Skip the Helix encoder (bring-up). | T23 | internal |

## JPEG

| Variable | Values / default | Effect | SoC | Class |
|---|---|---|---|---|
| `OPENIMP_HELIX_HW_JPEG` | `0` = software encoder | Helix hardware JPEG (only in builds that also have the software JPEG). | Helix SoCs | debug only |
| `OPENIMP_HELIX_JPEG_BS_KB` | 256..65536; default 1024 | JPEG bitstream limit in KiB. | Helix SoCs | debug only |
| `OPENIMP_HELIX_JPEG_BUSY_WAIT_MS` | ms; default 200; 0 = never wait | How long a JPEG that may be replaced by the last picture waits for the VPU before it is skipped (forces the `OpenIMP_Encoder_StreamIsReused` path). | T20, T21, T30 | debug only |
| `OPENIMP_HELIX_JPEG_DUMP` | directory | Dump JPEG pictures. | Helix SoCs | internal |
| `OPENIMP_HELIX_JPEG_MAX_BS` | `1` | Program the JPEG max-bitstream register (always on T23). | Helix SoCs | internal |
| `OPENIMP_HELIX_JPEG_PROBE_MAX_BS_KB` | 1..4096 | Probe limit (testing). | Helix SoCs | internal |
| `OPENIMP_HELIX_JPEG_RESERVE_AT_CREATE` | `0` = allocate at the first picture | Reserve the JPEG buffers at channel creation. | Helix SoCs | internal |
| `OPENIMP_HELIX_JPEG_RMEM_RESERVE_KB` | 0..65536 | rmem reserve of the Helix JPEG. | Helix SoCs | debug only |
| `OPENIMP_HELIX_JPEG_SRC_COPY` | `1` = keep the copy (A/B) | Copy the source frame for the JPEG fan-out. | Helix SoCs | debug only |
| `OPENIMP_HELIX_JPEG_STATS` | `1` | JPEG statistics. | Helix SoCs | debug only |
| `OPENIMP_HELIX_JPEG_STRIPE_ROWS` | 1..4096 | Fewer macroblock rows per JPEG job (tests the striped path). | Helix SoCs | internal |
| `OPENIMP_HELIX_JPEG_TIMEOUT_MS` | 100..20000 | Helix JPEG wait. | Helix SoCs | debug only |
| `OPENIMP_JPEG_MIN_WAIT_MS` | 0..5000 ms | Minimum wait of the JPEG fan-out of the p2 encoder. | T31, T40, T41 and Helix SoCs (p2 encoder) | debug only |
| `OPENIMP_T31_HW_JPEG` | `0` = software JPEG; default on | Hardware JPEG encoder on T31. | T31 | user |
| `OPENIMP_T31_HW_JPEG_SRC_COHERENT` | `1` | JPEG source in coherent memory. | T31 | debug only |
| `OPENIMP_T31_HW_JPEG_SRC_COPY` | `1` = keep the copy (A/B) | Copy the source frame for the JPEG fan-out. | T31 | debug only |
| `OPENIMP_T31_HW_JPEG_STREAM_COHERENT` | `1` | JPEG stream buffer in coherent memory. | T31 | internal |

