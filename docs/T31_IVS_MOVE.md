# T31: IVS motion detection ("move" and "base move") in OpenIMP

> **Status (2026-11-14):** The `claude/t31-*` branches named here were merged into `next` and deleted (CHANGELOG.md line 9); read them as history.

Status: branch `claude/t31-ivs-move`. Built and tested under qemu, **has not yet run on a
camera.** Section 6 describes the device test for a local Claude session.

Until now the `IMP_IVS_*` functions for T31 were only stubs: `PollingResult` only
slept, `GetResult` returned empty results. Now both
vendor algorithms really compute, with the behaviour of T31 libimp 1.1.6.

---

## 1. Files

| File | Content |
|---|---|
| `src/t31/openimp_t31_ivs_abi.h` | Structures in the vendor layout, with `_Static_assert` (0x450 `IMP_IVS_MoveParam`, 0x40 `IMP_IVS_BaseMoveParam`, 0x34 `IMPIVSInterface`, 0x30 `IMPFrameInfo`) |
| `src/t31/openimp_t31_ivs_move.c/.h` | the two algorithms, plain C without dependencies |
| `src/t31/openimp_t31_ivs.c/.h` | IVS framework (`IMP_IVS_*`), interfaces, frame hook |
| `src/kernel_interface.c` | calls the frame hook per capture frame (T31 only) |
| `src/t31/openimp_t31_services.c` | old stubs only remain for T21/T23/T30 |
| `tools/t31_ivs_compare.c` | Comparison with a stock libimp on synthetic frames |

The generic headers `include/imp/imp_ivs*.h` do **not** match the T31 ABI
(`IMPFrameInfo` has only 8 bytes there, `IMPRect` is x/y/w/h). They are not used for T31.

---

## 2. Behaviour

### 2.1 move (one yes/no per ROI)

- The luma is decimated 2:1: the top-left pixel of each 2×2 block.
- A ring of 4 half-size pictures. The input frame t is always compared with frame t−3.
- Binary difference `|A−B| > 20`.
- 3×3 erosion: a pixel stays only if all 9 neighbours are set. At the picture edge it is
  replicated.
- Per ROI the remaining pixels are counted. `retRoi[i] = count > T[sense]`.
- Thresholds T (pixels at half resolution):

  | sense | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
  |---|---|---|---|---|---|---|---|---|---|
  | T | 1365 | 455 | 151 | 50 | 16 | 8 | 4 | 2 | 1 |

- On creation, sense is clamped to 0…4. Via `IMP_IVS_SetParam`, 0…8 are allowed.
- The first 3 frames deliver results with all zeros.
- After that a result comes every `skipFrameCnt + 1` frames. The first at frame
  `skipFrameCnt + 3`.
- ROI coordinates: first clamp to the picture, then halve. The corners are inclusive.
- Vendor quirk, reproduced: If the bounding box of all ROIs does not start in row 0, the
  erosion is shifted down by one row. Output row y is then centred on picture row y+1.
  With a full grid (timps without privacy mask) this does not occur.
- `SetParam` takes over only `sense[]`, `roiRect[]` and `roiRectCnt`. `skipFrameCnt` and
  `frameInfo` stay. The ring and the frame counter are kept.

### 2.2 base move (one byte per 8×8 block)

- Full resolution. Frame n is compared with frame n − `referenceNum`, every
  `skipFrameCnt + 1` frames.
- `T = |A−B|` if `|A−B| ≥ thr`, else 0. thr by sense: 30, 20, 15, 10 for 0…3, otherwise 0.
- 3×3 minimum (edge replicated), then sum per 8×8 block, **truncated to 8 bits**.
- Only widths and heights that are multiples of 8 deliver data.
- A result is published for **every frame**. `ret = 1` only on detection frames.
  With `ret = 0`, `data` contains old content. Consumers must check `ret`.
- `sadMode` must be 0, `skipFrameCnt ≥ 0`, `referenceNum ≥ 1`. Otherwise
  `IMP_IVS_CreateChn` fails.
- `SetParam`: only `sense` has an effect.

### 2.3 Framework

- Only IVS group 0, channels 0…63.
- Per channel one thread `ivs_chnN` and three semaphores (start = 0, end = 1, result = 0).
- Every frame of the FrameSource channel that is bound to group 0 reaches every
  receiving channel. If a channel is still busy with the previous frame, the new frame
  is **dropped** for it.
- `IMP_IVS_SetParam` copies to `interface->param`. The change takes effect at the **next
  accepted frame**. Until then `IMP_IVS_GetParam` still returns the old state.
- Results are in a ring with 6 entries. `GetResult` returns a pointer into it and
  does not check whether a result is there. `ReleaseResult` does nothing. Whoever is more than 6 results
  behind reads overwritten data (as in the vendor).
- `PollingResult(chn, 0)` only checks. Negative timeout (`IMP_IVS_DEFAULT_TIMEOUTMS`) = 10 s.
- `StopRecvPic` waits up to 1 s for a running frame, then `flushFrame`.
- `DestroyChn` stops, unregisters, waits for waiting callers and ends the thread.

### 2.4 Frame hook

- In OpenIMP, `IMP_System_Bind` only enters into a table. There are no observers for
  `DEV_ID_IVS`, and the encoder fetches frames from the FrameSource ready queue.
- Therefore `VBMKernelDequeue` calls `openimp_t31_ivs_capture()` for **every** capture frame,
  **before** the frame goes into the ready queue.
- Advantages:
  - This matches the vendor location (`ivs_update` in the FrameSource context, every frame).
  - At this moment the buffer belongs only to the capture thread. The encoder can neither
    release it nor blend OSD into it.
  - No encoder lock is involved.
- Only the copy happens there:
  - move: the 2:1 decimation, and only for frames that a later comparison reads.
    Exception since `claude/ivs-opt`: If the new frame is not needed as a reference later
    (skipFrameCnt 1 or ≥ 3), detection runs right here directly on the
    capture buffer (see 2.6).
  - base move: the luma of the capture frames.
- Before the copy the luma area is invalidated in the cache (`DMA_RmemFlushCache(virt, size, 2)`),
  because the ISP writes by DMA into cached rmem.
- The computation runs in the channel thread.
- The source of the group is searched backwards with `IMP_System_GetBindbyDest` (normally
  FS → IVS directly).

### 2.5 Cost (qemu, meaningful only as a ratio)

Measured against a simple reference implementation of the same behaviour, same inputs:

| Case | OpenIMP | Reference | Factor |
|---|---|---|---|
| move 640×360, skip 5, 52 ROIs, per frame | 51 µs | 237 µs | 4.6 |
| move 1920×1080, skip 5, 52 ROIs, per frame | 346 µs | 1.87 ms | 5.4 |
| move 1920×1080, one detection | 1.45 ms | 8.4 ms | 5.8 |
| move 1920×1080, one decimation | 0.41 ms | 0.97 ms | 2.4 |
| base move 640×360, detection every frame | 1.3 ms | 9.7 ms | 7.5 |
| base move 1920×1080, detection every frame | 9–14 ms | 77–83 ms | 6–9 |

- For move, the vendor copies the full luma in **every** frame (2 MB at 1080p). With skip 5, OpenIMP
  decimates only 2 of 6 frames.
- The device test measures the real times on the camera (`OPENIMP_T31_IVS_STATS=1`).

### 2.6 Speed-up (branch `claude/ivs-opt`)

Reason: T20 (Wyze, 1080p main stream) showed timps with motion detection at ~8 % CPU,
without it at ~0.4 %. The results stay bit-identical.

- **Erosion on demand:** Output row y is the AND of the eroded rows y+sh−1, y+sh,
  y+sh+1. So an empty row clears three output rows. The rows are computed only when
  needed, the bottom one first. In a quiet scene only every third row is
  differenced.
- **Detection directly on the capture buffer:** If no later comparison reads the new
  frame (skipFrameCnt 1 or ≥ 3), `feed()` no longer decimates it completely into the ring.
  It computes the detection immediately against the reference picture and decimates only the rows
  it reads. `run()` then only compares with the thresholds. Consequence for the
  statistics: `copy` (capture thread) contains the detection for these frames, `process` is
  almost zero.
- **Difference test in byte lanes:** |a − b| > 20 for four pixels per word via floor and
  ceiling mean of a and 255 − b (≥ 138 or ≤ 117). Before, two 16-bit lanes were used.
- **Optional, only on request:** `OPENIMP_IVS_MOVE_INTERVAL=N` (2…1000). A move channel
  sees only every N-th capture frame; `skipFrameCnt` then counts these frames. Without the
  variable (or with 1) everything stays as in the vendor. base move and foreign interfaces are
  not affected.
- **Check:** `make -C tests/t30 check` runs `ivs_move_bench_t20 equiv`. This
  compares the new code frame by frame with a frozen copy of the old one
  (`tests/t23/ivs_move_ref.c`): 10 sizes up to 1920×1080 (also odd ones), skip 0…9,
  grid and random ROIs, live SetParam with sense 0…8, five scenes. `tests/t23` also checks
  the fixed digest `ca3d1021f2a30c82` of `ivs_move_digest`.
- **Host measurement:** `make -C tests/t30 ivs-bench` (x86-64, gcc -O2, 4×4 ROI grid,
  ns per input frame for feed + run, scenes: quiet with ±3 noise, night ±24, moving
  rectangle):

| Case | before | after | Factor |
|---|---|---|---|
| 1080p quiet, skip 5 | 175 µs | 82 µs | 2.1 |
| 1080p quiet, skip 0 | 744 µs | 417 µs | 1.8 |
| 1080p motion, skip 5 | 184 µs | 118 µs | 1.6 |
| 1080p night, skip 5 | 175 µs | 165 µs | 1.06 |
| 360p quiet, skip 5 | 14.4 µs | 6.6 µs | 2.2 |
| 360p quiet, skip 0 | 68 µs | 32 µs | 2.1 |
| 360p motion, skip 5 | 14.9 µs | 9.6 µs | 1.6 |
| 360p night, skip 5 | 14.5 µs | 13.8 µs | 1.05 |

- Not done: MXU SIMD (no `-mmxu`/MXU code in the project, the toolchain is built without
  it). Skipping the work entirely when nobody polls: timps polls all the time, so no
  gain, but a behaviour risk.

---

## 3. Deviations from the vendor

1. **Row behind the buffer (move).** If the ROI box starts below row 0 **and** reaches
   the bottom row, the vendor reads one row behind its buffer (heap garbage).
   OpenIMP repeats the last row instead.
   - Affects only ROIs in the bottom row, only in this layout.
   - timps without privacy mask is not affected (box starts in row 0).
   - In the qemu comparison: 49 of 2880 frames of this layout differ, all in a lower ROI.
2. **Invalid sense in SetParam (move).** The vendor aborts midway and leaves a
   freed filter behind (use-after-free). OpenIMP checks beforehand and keeps the old
   state.
3. **Frame size.** The vendor copies `width × height` of the frame into a buffer sized by
   `frameInfo`. With a larger frame the heap overflows. OpenIMP drops frames whose
   size does not match `frameInfo` and reports this once in the log.
4. **Stride.** Luma is read row by row with the stride. In OpenIMP T31 it equals the
   width (as in the JPEG and OSD path). The vendor silently assumes this.
5. **Error in `processAsync`.** The vendor then ends the channel thread; results never
   come again. OpenIMP logs once and carries on.
6. **Unnecessary work is skipped.** Frames that no comparison reads are not decimated.
   The result is identical.
7. **No debug ballast.** base move writes no `total_count` to stdout and appends no
   frames to `/tmp/mountdir/ivsbasemovesnap.nv12`.
8. **IVS sees frames without OSD.** The hook is before the encoder and thus before OSD blending.
   Whether the vendor feeds IVS with or without OSD has not been checked. Without OSD there are no
   false alarms from the time overlay.
9. **Minor points:**
   - Channel 64 is rejected. The vendor accepts it but never serves it.
   - `DestroyChn` stops and unregisters a channel itself instead of leaving it dangling.
   - `PollingResult` waits on the monotonic clock. A jump of the system time (NTP at boot)
     does not lengthen or shorten the timeout.
   - Odd picture sizes: ROI coordinates are additionally clamped to the half-size picture.
   - Interfaces built by streamers themselves get `preProcessSync` on the capture frame
     and `processAsync` on a private NV12 copy. OpenIMP cannot hold a
     capture buffer for them. `IMP_IVS_ReleaseData` therefore does nothing.

---

## 4. Tests under qemu (done)

- **Algorithms against the reference**, bit-exact: 82,880 move frames (42,328 results) and
  70,560 base-move frames (22,365 detections), 0 deviations.
  - Sizes 640×360, 320×240, 64×48, 66×50, 96×34, 128×72.
  - skipFrameCnt −1, 0, 1, 2, 3, 5, 9; referenceNum 1, 3, 5, 7; sense −1…4 (move via
    SetParam up to 8).
  - Moving squares, noise, brightness jumps, flicker, static scene.
  - ROI special cases: single pixels, edges, outside the picture, empty and overlapping ROIs,
    52 ROIs, box from row > 0.
  - Stride larger than the width, also not divisible by 4.
- **Against the stock libimp (scalar path)** with `tools/t31_ivs_compare.c`: 8591 of 8591
  move frames and 11,520 of 11,520 base-move frames identical, plus the 49 frames from
  deviation 1.
- **Framework** (semaphores, drop when busy, SetParam at the next frame, wrong
  frame size, foreign FS channel, foreign interfaces, timeouts, DestroyChn while
  PollingResult waits): all passed, under qemu and natively with ThreadSanitizer and
  AddressSanitizer (Create/Start/Stop/Destroy in a loop while frames are flowing).

---

## 5. Open questions that only the camera can answer

1. **SIMD = scalar in the vendor?** On the camera the stock libimp computes with MXU2. Do
   its results match the scalar path (and thus OpenIMP)? → Stage 1.
2. **Cache:** After invalidation, does the CPU see the current ISP frame? Signs of a
   problem: hardly any hits on motion, or hits without motion.
3. **Stride:** Is the NV12 stride of the T31 FrameSource equal to the width for all
   resolutions used?
4. **Cost:** Copy time in the capture thread and compute time in the IVS thread at 1080p and 2K. Does
   the copy disturb the frame rate of the main stream?
5. **"Feel":** Does the detection react at sense 0…4 like with the stock libimp?

---

## 6. Device test (local Claude session with SSH to the T31 test camera)

### 6.0 Rules

1. Only the one test camera.
2. Change nothing permanently:
   - do not flash;
   - change nothing under `/etc` or in the overlay, also not via `POST /control` against the
     normal configuration (timps then saves to `/etc/timps.conf`);
   - new files only to `/tmp`;
   - mount the new `libimp.so` only with a bind mount. A reboot restores everything.
3. Tell the human briefly before every stop of the streamer.
4. **Camera pictures** (snapshots, recordings) may be passed on only with the human's consent.
   Stage 1 needs no camera pictures.
5. Report everything, including failures, with the exact command before it.

### 6.1 Build (on the machine)

```sh
cd openimp && git switch claude/t31-ivs-move
TC=<thingino>/output/<branch>/<target>/host/bin/mipsel-linux
THINGINO_DIR=<thingino> TOOLCHAIN_PREFIX=$TC T31_OUTPUT_DIR=$PWD/build/t31 sh build-t31.sh
# comparison tool; dynamic and with the same (uclibc) toolchain, because it loads the
# stock libimp via dlopen:
$TC-gcc -O2 -march=mips32r2 -rdynamic -Isrc/t31 -o build/t31/t31_ivs_compare \
    tools/t31_ivs_compare.c src/t31/openimp_t31_ivs_move.c -ldl
scp build/t31/libimp.so root@<camera>:/tmp/libimp-ivs.so
scp build/t31/t31_ivs_compare root@<camera>:/tmp/
```

The stock libimp for the comparison: T31 SDK 1.1.6, uclibc
(`ingenic-lib/T31/lib/1.1.6/uclibc/4.7.2/libimp.so`, md5 `6ba99769…a818`). Copy it to
`/tmp/libimp-stock.so`. If it is not at hand, ask the human or skip stage 1.

### 6.2 Stage 1: algorithms against the stock libimp (without stopping the streamer)

The tool computes only on synthetic frames. It needs neither ISP nor a camera picture and runs
next to the streamer.

```sh
grep -i 'ASEs' /proc/cpuinfo            # does it list mxu_v2?
rm -f /tmp/closesimd
/tmp/t31_ivs_compare /tmp/libimp-stock.so 2>&1 | tee /tmp/ivs-compare-simd.txt
touch /tmp/closesimd                    # forces the vendor scalar path
/tmp/t31_ivs_compare /tmp/libimp-stock.so 2>&1 | tee /tmp/ivs-compare-scalar.txt
rm -f /tmp/closesimd                    # important: otherwise a stock streamer also computes scalar
```

- The first line names the path (`MXU2 SIMD` or `scalar`).
- **Success:** in both runs `differ=0` for move and base. `oob-differ` is allowed
  (deviation 1).
- Deviations in the SIMD run are an important result, not a reason to abort. Report the first
  `MOVE DIFF`/`BASE DIFF` lines.
- If `dlopen` fails (missing symbols, wrong libc), return the message.

### 6.3 Stage 2: timps with OpenIMP and motion detection

```sh
cp /usr/lib/libimp.so /tmp/libimp-orig.so
mount --bind /tmp/libimp-ivs.so /usr/lib/libimp.so
cp /etc/timps.conf /tmp/timps-ivs.conf
```

Set in `/tmp/timps-ivs.conf` (only in the copy):

```
motion.enabled = 1
motion.monitor_stream = 0
motion.sensitivity = 128
motion.skip_frames = 5
```

Start the streamer by hand, with statistics:

```sh
/etc/init.d/S95timps stop
OPENIMP_T31_IVS_STATS=1 /usr/bin/timpsd -c /tmp/timps-ivs.conf > /tmp/run-ivs.log 2>&1 &
sleep 30
grep -E '\[IVS\]|motion' /tmp/run-ivs.log | tail -20
logread 2>/dev/null | grep -E 'IVS|motion' | tail -20
```

Check that the variable arrives:
`tr '\0' '\n' < /proc/$(pidof timpsd)/environ | grep OPENIMP`

Measurements:

1. **Start:** Does timps report `motion detection started (…)`? No `CreateChn failed`, no
   `does not match the configured` from `[IVS]`.
2. **Statistics** (one line per channel every 10 s):
   `[IVS] chn0: <n> frames, <d> dropped busy, <r> results; copy avg … max …; process avg … max …`
   - `frames` ≈ frame rate × 10.
   - `results` ≈ `frames / (skip_frames + 1)` minus drops.
   - `copy` = time in the capture thread, `process` = compute time in the IVS thread.
3. **Quiet scene** (nobody in the picture, 2 minutes):
   `curl -s http://127.0.0.1:8880/control` (token as needed), in the `motion` object: `active[]`
   stays 0, `last_ms` does not change or rarely. No `stalled: 1`.
4. **Walk test:** With the human's consent, walk through the picture (or hold a hand in front of the
   lens). `active[]` shows the cells, `last_ms` becomes current. Follow live:
   `curl -N "http://127.0.0.1:8880/events?stream=motion"`.
5. **Sensitivity live:** Because timps runs with `-c /tmp/timps-ivs.conf`, `POST /control`
   saves only into the copy:
   `curl -s -X POST http://127.0.0.1:8880/control -d '{"motion":{"sensitivity":255}}'`,
   then `0`. In the log: `motion sensitivity updated live to …`. Expectation: at 255 (sense 4)
   clearly more hits than at 0 (sense 0).
6. **Load and frame rate:** `top -b -n 3 | head -20` with `motion.enabled = 1` and with `0`
   (second run). Frame rate of the main stream from `/control` (`stats`) in both runs. Connect an
   RTSP client for 2 minutes, watch for stutter.
7. **Night/IR** (optional): in the dark with IR, quiet scene for 2 minutes. Report many false alarms.
8. **Stop/start:** `curl … -d '{"motion":{"enabled":0}}'`, then `1`. No crash, then results again
   (statistics line).

**Abort criterion:** Crash, frozen stream or constant `stalled: 1`. Then
save `logread`, `dmesg` and `/tmp/run-ivs.log` and report.

prudynt instead of timps: the same steps. Copy the configuration to `/tmp`, switch on
motion detection there and pass the config path at start (check the option with
`prudynt --help`). Results are in the log there.

### 6.4 Stage 3 (optional): comparison with the stock stack

Only with a second T31 camera with the original stack (stock kernel modules and stock libimp),
same scene, same settings. The stock libimp does not run on the open stack.

- Walk test as in 6.3, item 4, once without and once with `touch /tmp/closesimd` before the
  streamer start (afterwards `rm -f /tmp/closesimd`).
- Compare: Which cells trigger, how fast, how many false alarms in a quiet scene.
- Expectation: same behaviour, because the algorithms are bit-identical. Differences then come
  from the input picture, frame rate or OSD.

### 6.5 Clean-up

```sh
kill $(pidof timpsd); umount /usr/lib/libimp.so; /etc/init.d/S95timps start
rm -f /tmp/closesimd
```

Or reboot the camera. Then check that the streamer runs normally.

### 6.6 What to report back

- Header: model, SoC (`/proc/cpuinfo`, also the line `ASEs implemented`), thingino revision,
  kernel, streamer and version, commit (`git rev-parse --short HEAD`).
- Stage 1: both outputs of `t31_ivs_compare` in full.
- Stage 2:
  - the `[IVS]` statistics lines (at least 1 minute) for the 1080p or 2K main stream;
  - `top` values with and without motion detection, frame rate of both runs;
  - result of quiet scene, walk test, sensitivity, stop/start;
  - log excerpts on errors.
- Stage 3, if done: observations in comparison.
- No camera pictures without consent.

## 7. T23 (T23 SDK 1.3.0)

Since branch `claude/t23-stub-fixes`, `build-t23.sh` builds the same files
(`openimp_t31_ivs.c`, `openimp_t31_ivs_move.c`) with `PLATFORM_T23`; the IVS stubs in
`openimp_t31_services.c` now apply only to T21/T30.

- **Vendor comparison (disassembly of `libimp.a` 1.3.0 against T31 1.1.6):** `ivs.c`,
  `move_ivs.c`, `base_move_ivs.c`, `ivs_move.c`, `ivs_base_move.c`, `filter.c`, `sad.c` are
  identical except for structure offsets. Where T31 has an MXU2 branch (`resize`,
  `MorphRowFilter`, `MorphColumnFilter`, `move_detect`, `Proceed`, `MergeBaseMove`), T23
  has only the scalar branch and an `assert` instead of the SIMD code; constants
  (thresholds 1365/455/151/50/16, 20, 30/20/15/10) and tables are the same. OpenIMP reproduces
  the scalar T31 path bit-exactly, and so also T23.
- **ABI difference:** only `IMPFrameInfo` (0x38 instead of 0x30 bytes: `direct_phyAddr` at 0x20,
  `timeStamp` at 0x28, `timeStamp_ivdc` at 0x30). From this follows `IMP_IVS_MoveParam` 0x458
  (roiRect 0x110, roiRectCnt 0x450), `IMP_IVS_BaseMoveParam` 0x48 and the public
  `IMP_IVS_BaseMoveOutput.timeStamp` at 0x10. `openimp_t31_ivs_abi.h` checks this with
  `_Static_assert`.
- **Frame hook and idle drain:** as T31 (`VBMKernelDequeue` → `openimp_t31_ivs_capture`,
  returning the buffers to the driver as long as nobody calls `GetFrame`, and
  `VBMRecycleIdleFrames` after an empty `select`), now also for `PLATFORM_T23`.
- **Host tests:** `make -C tests/t23 check` compares the algorithms with T31 and T23 layout
  (same digest, 3708 move results, 5120 base-move detections) and runs the
  framework with T23 capture records from group/chn/bind through to poll/get/release.
- **Device comparison:** build `tools/t31_ivs_compare.c` with `-DPLATFORM_T23` and run it against
  `/opt/openimp-t23/libimp.so` (the stock libimp of the Helix worker).
