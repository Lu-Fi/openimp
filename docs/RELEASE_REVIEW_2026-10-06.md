# Release review 2026-10-06 (first release)

Code review, no camera access. Scope: openimp `claude/agg-26` (1be6497)
and open-tx-isp `claude/agg-26` (47032b68) against `origin/next`, plus the
release candidates openimp `streamer-gaps-t1x-t23-t31` (d018c75), `t1x-roi`
(4051f64), `t23-enc-rest` (aefe001, includes `t23t31-cacheonly`); open-tx-isp
`streamer-gaps-t1x-t23-t31` (933cea7d), `t23-awb-stock` (0a561b7c),
`fable-chan-restart` (4ce66147), `t23-seq-audit` (e10defa9, docs only).

Tags: **[proven]** = reproduced here (host build/tests, merge attempts) or
read directly in the code; **[inferred]** = concluded from code reading, not
run; **[user]** = device result reported by the owner.

Host test suites (`make check`) **[proven]**: openimp agg-26 all suites pass
(t23, t30, t31, t41, t40, eprc, rc_t20, rc_t10); agg-26 + streamer-gaps +
t1x-roi pass; agg-26 + streamer-gaps + t23-enc-rest pass. open-tx-isp agg-26:
67 passed, 0 failed; agg-26 + streamer-gaps + t23-awb-stock + fable-chan-restart:
70 passed, 0 failed.

## 1. Blocking

**B1. fable-chan-restart ships the knob set that hangs cam-B.** **[proven
defaults, user result]** `driver/t23/tx_isp_t23_core.c` ~10040-10075 defaults
to `msca_keep_enabled=1`, `chan_stop_keep_input=0`, `msca_fifo_rearm=1`
(README "2128-step3c", 10 min / 25 wakes). The owner's 7 h + 15 restarts
clean run used `chan_stop_keep_input=1 msca_keep_enabled=2 msca_fifo_rearm=0`,
and "all three old knobs together" (= the code defaults) hangs. Do not release
with the current defaults: set the proven triple as defaults (pending the
pairwise result) and keep the switches as 0644 escape hatches.

**B2. openimp `t1x-roi` and `t23-enc-rest` cannot both merge.** **[proven]**
Both claim `HWEncoderParams` offset 0x64 in `src/hw_encoder.h`: t1x-roi
`int32_t chroma_qp_offset`, t23-enc-rest `reserved[0] = pool id + 1`
(`t30_helix_encoder.c:2328`, `codec-t40.c` SetPool). Octopus and sequential
merges conflict. One side must move (e.g. pool id to `reserved[1]`/0x68) and
the `_Static_assert` must still hold.

**B3. T23 capture buffers are freed while the MSCA output keeps writing.**
**[inferred, mechanism documented]** With `msca_keep_enabled=2` and/or
`chan_stop_keep_input=1` (the proven set, B1) a stopped output stays enabled
with the old buffer addresses in its FIFO while the input runs
(`regtrace_t23_set_msca_stream`, comment at ~10045: "keeps writing to the
addresses left in its FIFO"). openimp T23 `IMP_FrameSource_DisableChn`
(`framesource_tseries.c:2605` -> `VBMDestroyPoolParked`) frees the pool: the
parking in `kernel_interface.c` (`vbm_parked`, `vbm_destroy_pool`) is
`#if defined(PLATFORM_T21)` only. The driver README attributes the fatal run
2227 ("unmatched MSCA completion", dead 0.6 s later) to exactly this: "openimp
frees the pool right after the close". The 7 h run most likely re-allocated
the same hole each time. Fix before release (cheap): extend the rmem parking
to `PLATFORM_T23` (DestroyChn frees it, timps on-demand uses Disable/Enable),
and/or in the driver point a kept output's FIFO at a scratch buffer on
STREAMOFF. Same class on T41 (`t41_msca_stop_disable=0`, buffers freed after
REQBUFS 0 in `openimp_p1.c` `release_capture_queue`): soak-tested, verify
with the "unmatched MSCA completion" counters.

## 2. Risky: keep out of the first release or default-off

- **`t23-enc-rest`** (openimp): rewrites the allocator path for every
  allocation (`dma_alloc.c` pool blocks, `mempool_continuous.h`), adds a
  drop-and-re-IDR check in the T23 native encoder hot path (`t23_maxpic_check`),
  T31 eval info, ISP OSD pool. Dormant for prudynt/timps (neither calls
  `IMP_System_MemPoolRequest`, `*_SetPool`, `SetChnMaxPictureSize`; prudynt's
  attr `uMaxPictureSize` is not wired on T23) **[proven by grep]**, host tests
  pass, but it conflicts with t1x-roi (B2) and has no device run I could find.
  Leave out of the first release; take later after B2 and a device run.
- **`t23-awb-stock`** (open-tx-isp): 2.5 k lines, replaces the default T23 AWB
  path; bit-identical to the stock module in the emulator golden tests
  **[proven on host]**, no lifecycle code, ~2.6 KB stack in the AWB work
  (`t23x_awb_cluster_weights`). It changes image behaviour for every T23 user:
  include only after a day/night device check, else ship streamer-gaps
  (contains `t23-awb-runtime`) without it.
- **T21 OSD first-JPEG retry** (`openimp_t31_services.c` ~300-690): default on
  for T21, retries with `usleep(10 ms)` x3 under `osd_lock` in the encoder
  thread during a 3 s window after idle, and overrides the frame phys with
  `DMA_VirtToPhys` on "MISMATCH". Device-proven on the T21 cam **[user]**; keep,
  `OPENIMP_OSD_RETRY=0` is the escape hatch. Not for other SoCs (already so).
- **T41 restart fix** (`t41_msca_stop_disable=0`, `t41_msca_flip_skip_noop=1`,
  `t41_msca_fifo_ctrl_after_live_qbuf=1`): `tisp_channel_main_stop` now returns
  before the DMA-quiescence wait **[proven]**; 30-60 min mixes clean
  **[user]**; residual = B3 class.
- **p3 audio NOBLOCK capture thread** (`openimp_p3_audio.c` `p3_capture_start`):
  if `p3_capture_stop` times out (1 s, GET_STREAM stuck) and a new start runs,
  `free(p3_audio.cap_data)` races the old thread's memcpy **[inferred]**. Edge
  case; guard by refusing a restart while `!cap_exited`.
- Default-off / app-driven, fine as is: VBM delay FIFO + `GetTimedFrame`
  (prudynt passes `maxdepth=0` **[proven]**), T21 `SetChnRotate` (D1 cap),
  video-drop monitor thread, AO cache (`OPENIMP_AO_CACHE=1`), T41 `SnapFrame`,
  T20/T21 Color2Grey/SuperFrame/ROI (t1x-roi: T21 ROI behind `OPENIMP_T21_ROI`).

## 3. Recommended composition

openimp: `agg-26` + `streamer-gaps-t1x-t23-t31` + `t1x-roi` (dormant for the
streamers, host-tested) + the B3 parking fix. `t23-enc-rest` after B2 and a
device run.

open-tx-isp: `agg-26` + `streamer-gaps-t1x-t23-t31` + `fable-chan-restart`
with defaults `chan_stop_keep_input=1 msca_keep_enabled=2 msca_fifo_rearm=0
msca_flip_skip_noop=1 msca_restart_skip=1 msca_session_release=1` (B1; only
if the pairwise test does not contradict), `crumbs=0`. `t23-seq-audit` (docs
+ emulator) can go in. `t23-awb-stock` only after a device check. CHANGELOG.md
conflicts between the three branches are trivial unions **[proven]**.

Reviewed and unremarkable (stability): VBM volume table 32x16, parking
(T21), `IMP_Free` claim-then-free, `Fifo_Deinit` under mutex, T41 GetFrame
re-check under the P1 lock, `RequestIDR` under the channel lock, IVS copy
outside `ivs_lock` with `users` refcount, T31 AeAttr 18/38-word mapping
(fixes an 80-byte overrun), T31 AF/WDR/autozoom controls, T21 open rollback,
T21 AF stubs (removed wild reads/writes), T23 `t23_printk` (no rendering in
IRQ), T23 WaitFrame without the tuning mutex, eprc `eprc_mul32` wrap.

## 4. Quick wins

1. B3: `kernel_interface.c` parking `#if defined(PLATFORM_T21) || defined(PLATFORM_T23)`.
2. B1: change the four `module_param` defaults.
3. B2: move the pool id to `reserved[1]` in `t23-enc-rest` (or stash it elsewhere).
4. T21 `IMP_ISP_Tuning_SetModuleControl` (`isp_tseries.c` ~3237) now forwards
   0x80000e2 to the driver; the open T21 driver has no handler (only T31 does)
   **[proven by grep]**, so it fails instead of caching. Fall back to the cache
   when the ioctl fails. Not called by prudynt/timps.
5. `openimp_p3_audio.c` `IMP_AI_SetHpfCoFrequency(0)` returns -1 on T40/T41
   while prudynt passes 0 = default (T31 stores it and returns 0). Pre-existing;
   align.
6. p3 capture restart guard (see above).
7. T21 OSD retry / fan-out INFO lines (24 + 40 + 16 + 16 capped) can drop to
   trace once the T21 cam is confirmed.
