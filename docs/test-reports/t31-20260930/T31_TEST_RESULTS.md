# T31 test results, branch `claude/t31-all`

Test run on 2026-09-30 according to [`docs/T31_TEST_PLAN.md`](../../T31_TEST_PLAN.md), run by
a local Claude Code session over SSH. The camera is anonymised in the report (`cam-A`,
`192.0.2.x`), and so are the files under `test-results/`.

## Header

| | |
|---|---|
| Camera | WUUK Y0510, profile `wuuk_y0510_t31x_sc4336p_ssv6158`, sensor SC4336P (2560×1440) |
| SoC | T31X (`/proc/cpuinfo`: `system type: Swan`, `cpu model: Ingenic Xburst V0.0 FPU V0.0`, `isa: mips32r1`, **`ASEs implemented:` empty**, so no MXU2) |
| Memory | `mem=78M@0x0 rmem=50M@0x4e00000` |
| thingino | branch `ciao` (`BUILD_ID="ciao+64ae16c, 2026-09-28 14:03:37 UTC"`), Buildroot 2026.08, uClibc, GCC 16 |
| Kernel | `3.10.14__isvp_swan_1.0__ #2 PREEMPT Mon Sep 28 15:58:36 CEST 2026` |
| ISP/AVPU | **original stack**: `tx-isp-t31.ko` (vendor author string), `avpu.ko` (Allegro), `sensor_sc4336p_t31.ko` |
| libimp in flash | **original libimp** T31 SDK 1.1.6, uclibc/5.4.0 (md5 `97ca48de99f9039bd9550ba40c0ae7eb`) |
| Streamer | timps v1.9.28 (init script `S95timps`, `/usr/bin/timpsd -c /etc/timps.conf`) |
| Main/Sub | H.264 1920×1080 and 640×360, 15 fps, 1200/384 kbit/s, snapshot from JPEG channel on group 0 |
| Tested commit | `claude/t31-all` = `3f501a2` (with rmem fix; first C run with `014edbc`, comparison in C additionally `main` = `9cab219`) |
| Toolchain | thingino toolchain of the camera profile (`mipsel-linux-gcc` 16.1.0) |
| Scene | dark interior, night mode with IR, static |

### Deviation from the test plan: original stack instead of open stack

The test plan assumes a camera with thingino `aperto` and the open stack
(open-tx-isp + OpenIMP). The test camera runs the **original stack** (stock tx-isp, original
libimp). After consultation with the human, testing was done anyway:

- A1 and A2 are independent of this (they need only `/dev/avpu` and `/dev/ipu`).
- From B on, OpenIMP runs via bind mount on the **stock kernel modules**. According to the README,
  OpenIMP supports these drivers. The reference in C is therefore the **original libimp** (instead of
  original OpenIMP) and additionally OpenIMP `main`.
- All manual timps starts run with a copy of the configuration under `/tmp/timps.conf`,
  because timps writes settings back (e.g. `image.running_mode` at the day/night switch).
  Nothing was written to `/etc`.
- A thingino image with the open stack for this profile was built locally (see appendix),
  but not flashed.

## Overview

| Test | Status | Key statement |
|---|---|---|
| A1 HW JPEG tool | passed | valid JPEGs with hardware headers, 7.85 ms for 1080p, DRI/overflow as expected |
| A2 IPU OSD tool | passed | all layers correct; cover colour: the IPU takes Y without +16 |
| B build/mount | passed | all 126 symbols needed by timps present |
| C regression | passed with `3f501a2` | `014edbc` (and `main`) failed on the rmem leak; the fix holds 25 cycles, no-reuse reproduces the old error |
| S1 long run 2 h | passed | 108002 frames per stream, 690/690 snapshots, all AVC counters 0; no hang, so recovery was not triggered |
| S2 snapshot load | passed with remark | no gaps; OpenIMP timestamps less even than the original, most with HW JPEG |
| I stage 1 | passed | move and base `differ=0` (scalar path; CPU without MXU2); comparison tool had a memory leak (fixed) |
| I stage 2 | passed with finding | with client: 0 false alarms, PTZ movement detected, live sensitivity, stop/start ok; **without client the detection delivers nothing (stalled)** |
| G OSD | passed | OSD visible in RTSP and snapshot; timpsd 7.9 % instead of 13.0 % (original) |
| E/H JPEG | passed (H partly) | HW JPEG: 18.5 % instead of 66.9 % CPU, 0.28 s per snapshot, pictures correct |
| D companion stage | passed | no disadvantage without the companion stage |
| F, S3, S4 | skipped | no T20; time |
| open-tx-isp via insmod | not possible | `rmmod tx_isp_t31` (stock) → kernel oops |
| **Finding on exit** | open | 5 of ~40 stops: `FS(n)-tick` spins in the kernel, timpsd stays a zombie, only a reboot helps |

---

## A1. Hardware JPEG test tool – **passed**

Built with `mipsel-linux-gcc -O2 -march=mips32r2 -static -Wall -Wextra` (thingino toolchain),
timps stopped for it. Files: [`a1/`](a1/).

```sh
/tmp/t31_hwjpeg_probe -v -e /tmp/ep1.bin 2>&1 | tee /tmp/probe-1080p.txt
/tmp/t31_hwjpeg_probe -W 640 -H 360 -o /tmp/hwjpeg-360.jpg 2>&1 | tee /tmp/probe-360p.txt
```

| Run | Status `0x8430` | Length | `0x8438` | HW time | Result |
|---|---|---|---|---|---|
| 1920×1080 | `0x0004039e` | 52012 | 0 | 7.85 ms | SOI + EOI, JFIF header from the hardware |
| 640×360 | `0x000075a8` | 7703 | 0 | 0.91 ms | SOI + EOI |
| `-c 0x31` (bit 8 off), 1080p and 360p | as above | 52012 / 7703 | 0 | 7.85 / 0.91 ms | **bit-identical** to the run with bit 8 |
| `-W 640 -H 360 -c 0x00080131` (DRI 8) | `0x000075a8` | 8203 | 0 | 0.92 ms | `FF DD` present, **114 RST markers** `D0…D7` (920 MCUs / 8 = 115 intervals, fits) |
| `-W 640 -H 360 -s 4096` | `0x000075a8` | 7703 | **`0x2`** | 0.91 ms | overflow bit as expected; tool reports `implausible length 7703` |
| `-s 65536` (1080p) | as 1080p | 52012 | 0 | 7.86 ms | **no** overflow: the test picture is only 52 KB, 64 KB is enough |

- `0x85f8` is always `0x00000000` before and after the start.
- First bytes: `ff d8 ff e0 00 10 4a 46 49 46 00 01 01 00 00 01` (hex dump in
  [`a1/probe-1080p.txt`](a1/probe-1080p.txt)).
- Picture check: grey levels, checkerboard and the eight colour bars (grey, yellow, cyan, green,
  magenta, red, blue, grey) match the generator, no U/V swap.
  Luma against the target pattern ([`a1/luma-check.txt`](a1/luma-check.txt), decoded directly as YCbCr):
  54.7 dB (1080p), 55.6 dB (360p), checkerboard rows lossless.
- `ep1.bin` is attached ([`a1/ep1.bin`](a1/ep1.bin), 0x790 bytes).

**Finding: DMA memory via `/dev/avpu`.** After the first 1080p run, further
1080p runs failed:

```
/tmp/t31_hwjpeg_probe -v -s 32768 -o /tmp/overflow-32768.jpg
GET_DMA_MMAP(3133440) failed: Cannot allocate memory
```

dmesg: `avpu avpu.0: Can't alloc DMA buffer`. `/dev/avpu` gets the buffer by
`dma_alloc_coherent` from the 78 MB kernel RAM (not from rmem). A 3 MB buffer needs a
free 4 MB block (order 10); `/proc/buddyinfo` showed none any more, also not after
`drop_caches`. Therefore the additional tests ran with 640×360. For the hardware JPEG path in
OpenIMP this means: source and stream buffers should not come from the AVPU coherent fallback.

## A2. IPU OSD test tool – **passed** (with a note on the cover colour)

```sh
/tmp/t31_ipu_osd_probe -v -n 200 2>&1 | tee /tmp/ipu-probe.txt
python3 tools/ipu_osd_analyze.py test-results/a2
```

Files: [`a2/`](a2/) (`ipu-probe.txt`, `ipu-dmesg.txt`, `ipu_osd_analyze.txt`, both PNGs,
raw data).

- `IOCTL_IPU_START ok, 379 us`, no IPU errors in dmesg.
- All four layers in the right place, **0 pixels changed outside the boxes**.
- Bitmap: bands in the order red, green, blue, white, black; alpha fades in from left
  to right; channel 1 half as opaque as channel 0. Red opaque, blue semi-transparent.
- Luma model: best `kernel-analog+16` with `+128>>8` (mean error 0.10); global alpha
  128: mean error 0.05.
- Times (`-n 200`):
  - 4 layers in one run: **352.2 µs**
  - one run per layer: **375.1 µs** per frame (4 runs)
  - `IOCTL_IPU_BUF_FLUSH_CACHE` over 356352 bytes: **80.1 µs** (the tool flushes 348 KB, not 1 MB)

**Note on the cover colour:** The hardware takes Y from the colour word **unchanged**:

| Area | Colour word (`bak`) | measured | analyze expects (stock) |
|---|---|---|---|
| red, a=255 | `0xff425af0` (Y=0x42=66) | Y 66.0, U 90, V 240 | Y=82 |
| blue, a=128 | `0x8019f06e` (Y=0x19=25) | Y 66.5 (background 108.5), U 184, V 119 | Y=41 |

Blue fits exactly `(25+108.5)/2`, `(240+128)/2`, `(110+128)/2`. The tool writes Y without
+16 (66 instead of 82). If the cover colour is to match the stock output, the colour word must
carry Y including +16; the IPU adds nothing.

## B. Build and mount `libimp.so` – **passed**

```sh
TC=<thingino>/output/ciao/wuuk_y0510_t31x_sc4336p_ssv6158-3.10.14-uclibc-192.0.2.x/host/bin/mipsel-linux
THINGINO_DIR=<thingino> TOOLCHAIN_PREFIX=$TC T31_OUTPUT_DIR=$PWD/build/t31 sh build-t31.sh
# camera:
cp /usr/lib/libimp.so /tmp/libimp-orig.so
mount --bind /tmp/libimp-t31all.so /usr/lib/libimp.so
```

- Build without errors (only warnings, [`b/build-t31all.log`](b/build-t31all.log)),
  `libimp.so` 485228 bytes, md5 on the camera `b8035149d22f40270ee51b452eb0ec31`.
- All **126** libimp symbols that timpsd imports are exported by OpenIMP (`nm -D` against the
  original libimp).
- timps starts with OpenIMP on the stock kernel: ISP, framesource, H.264, JPEG, OSD calls
  start up (`/proc/<pid>/maps` shows the mounted file).

## C. Regression without switch – first run **failed** (rmem leak), with rmem fix `3f501a2` **passed**

The first run with `014edbc` failed on the rmem leak (below). The fix
(`a511238 dma: free reserved-arena (rmem) allocations instead of leaking them`, merged as
`3f501a2`) was tested afterwards, see [C with rmem fix](#c-with-rmem-fix-3f501a2). All tests
from S on run with `3f501a2`.

### Procedure (first run, `014edbc`)

```sh
/etc/init.d/S95timps stop
mount --bind /tmp/libimp-t31all.so /usr/lib/libimp.so
/usr/bin/timpsd -c /tmp/timps.conf > /tmp/run-c.log 2>&1 &
# PC: ffmpeg -rtsp_transport tcp -i rtsp://…/ch0 or ch1 -t 600 -c copy …, snapshot every 10 s
```

### Reference: original libimp (5 min, same procedure)

| | Main | Sub |
|---|---|---|
| Frames / fps | 4442 / 14.81 | 4445 / 14.81 |
| Gaps > 0.5 s | 0 | 0 |
| Decode errors | 0 | 0 |

Snapshots 30/30 HTTP 200, avg. 230 KB, avg. 0.37 s. timpsd 11–15 % CPU, system ~78 % idle
([`c/top-stock.txt`](c/top-stock.txt), [`c/snaps-stock.txt`](c/snaps-stock.txt)).

### With `claude/t31-all`

- Starts, H.264 main/sub and JPEG start up (`chn0: first encoded frame … key=1`), the first
  snapshots come, OSD calls without error message (`OSD stream 0: 4 overlay(s)`).
- After the first teardown of the framesource (the main client left), it cannot be
  activated again:

```
08:07:29.667 [INF] HAL_ING      framesource 0 disabled (idle)
[VBM] CreatePool: chn=0, 1920x1080 … 2 frames, size=3133440
[VBM] CreatePool: allocation failed
08:07:32.653 [ERR] HAL_ING      framesource 0: EnableChn failed (attempt 1)
08:07:37.654 [ERR] HAL_ING      jpeg chn3: encoder dead after 10 consecutive misses …
08:08:30 … jpeg chn3: 5 consecutive forced-recovery cycles never produced a frame - giving up …
```

- Snapshots afterwards HTTP 503 (result: 1× 200, 7× 503, 3× timeout; [`c/snaps-c.txt`](c/snaps-c.txt)).
- Log: [`c/run-c-t31all.log`](c/run-c-t31all.log) (per-frame lines `GetFrame`/`DQBUF` removed),
  dmesg: [`c/dmesg-c-t31all.txt`](c/dmesg-c-t31all.txt).

### Cause: rmem is never released

On this camera OpenIMP creates all DMA buffers via `/dev/rmem` with a bump allocator
(`src/dma_alloc.c`, `dma_free_buffer()`: *"RMEM bump allocations are not individually freed
(no-op)"*). Every off/on cycle of the framesource creates the two 3 MB frame buffers anew. The
physical addresses only rise:

```
0x55f1000 0x58ee000 | 0x5beb000 0x5ee8000 | 0x61e5000 0x64e2000 | 0x67df000 0x6adc000 |
0x6dd9000 0x70d6000 | 0x73d3000 0x76d0000 | (sub) 0x7d7c000 0x7dd2400 | afterwards: allocation failed
```

rmem ends at `0x8000000`. timps tears the framesource down as soon as the last client leaves, and
switches it on briefly several times at start for the day/night switch (4 cycles in the
first 30 s). After 5–7 cycles rmem is full.

### Control test with `main` (`9cab219`)

Same procedure for both libraries: start timps, wait 45 s, then 6× fetch main for 10 s
and disconnect ([`c/cyc-main-summary.txt`](c/cyc-main-summary.txt),
[`c/cyc-t31all-summary.txt`](c/cyc-t31all-summary.txt)).

| | `main` | `claude/t31-all` |
|---|---|---|
| Framesource enables until the first error | 5 | 5 |
| `allocation failed` already at the 1st client | 14 | 14 |
| End | `chn0: 5 consecutive forced-recovery cycles never produced a frame … exiting` | identical |

Both behave **identically**. The place in `dma_alloc.c` is the same on `main`,
`claude/t31-re` and `claude/t31-all`. This is a pre-existing OpenIMP error, not a
regression of the branch. It hits every streamer that tears down and re-creates channels at run time.

**Proposal:** Give the rmem arena allocator a release (free list or buddy), or
keep VBM pools per channel after the first creation and reuse them on
`DisableChn`/`EnableChn`.

### Interim workaround (old state only)

Before the fix, S1 ran briefly (08:30–08:45) with the old state and permanent RTSP clients, so that
the framesource is not torn down: 86/86 snapshots, all AVC counters 0, sub constant 15 fps,
main 7 of 90 10 s intervals with only 77–139 instead of ~150 frames. This run was aborted for the fix
and does not count as an S result.

### C with rmem fix (`3f501a2`)

Library rebuilt (md5 on the camera `d1d9d43ca6e65be727c3b31c6594f50f`), mounted as in B,
timps started without variables and **without** a permanent client.

**FrameSource cycles** ([`c/cycles-c2.txt`](c/cycles-c2.txt), log [`c/run-c2.log`](c/run-c2.log)):
after a 45 s start (4 day/night cycles), 25 times fetch main for 8 s via RTSP and disconnect, each
followed by fetching sub for 2 s, 5 s pause (timps then tears the framesource down).

| | Result |
|---|---|
| Framesource enables main / sub | **30 / 25** |
| `allocation failed` | 0 |
| `rmem out of memory` | 0 |
| `[ERR]`/`[WRN]` in the log | 0 |
| ffmpeg errors main (without dts hints) | 0 in all 25 cycles |
| Sub frames in 2 s | 31–32 in every cycle |
| timps PID | the same throughout |

**Picture errors after a cycle:** From each cycle, the 1st and 16th frame after
reconnecting were saved (local only, camera picture) and looked at: all 25 first frames complete,
without block garbage or old picture content. Numerically: largest mean deviation of a
16×16 block from the median of all 50 frames **7.7** grey levels, i.e. noise level
([`c/blockcheck-c2.txt`](c/blockcheck-c2.txt)). No sign of hardware writing into released
buffers.

**Control test `OPENIMP_RMEM_NO_REUSE=1`** ([`c/cycles-c2-noreuse.txt`](c/cycles-c2-noreuse.txt),
[`c/tail-c2-noreuse.txt`](c/tail-c2-noreuse.txt)): The old error comes back as expected,
after 6 enables:

```
[DMA] rmem out of memory: requested 706560, used 51888128 of 52428800, largest free block 540672, 26 allocations
… chn1: 5 consecutive forced-recovery cycles never produced a frame … exiting
```

The new message now names the cause directly.

**Open:** In the first cycle of the no-reuse run (and in the first C run), ffmpeg reported
`number of reference frames (0+2) exceeds max (1; probably corrupt input)` on entering the
main stream ([`c/ffmpeg-noreuse-cycle1.txt`](c/ffmpeg-noreuse-cycle1.txt)). In the 25 cycles
with reuse it did not occur. A connection to rmem is unlikely; more likely the
entry in the middle of a GOP or the double-IDR spot.

**10-minute run** (RTSP main+sub decoded live with ffmpeg, snapshot every 10 s, no
permanent client beforehand; [`c/rtsp-summary-c3.txt`](c/rtsp-summary-c3.txt),
[`c/snaps-c3.txt`](c/snaps-c3.txt), [`c/top-c3.txt`](c/top-c3.txt), [`c/run-c3.log`](c/run-c3.log)):

| | Original libimp (5 min) | OpenIMP `3f501a2` (10 min) |
|---|---|---|
| Main / sub fps | 14.81 / 14.81 | 15.0 / 15.0 (9001 frames each) |
| 10 s intervals with < 140 frames | – | 0 (except the truncated last one) |
| Decode errors | 0 | 0 (only `non monotonically increasing dts`, see jitter) |
| Snapshots | 30/30, avg. 230 KB, 0.37 s | 58/58, avg. 208 KB, 0.44 s |
| timpsd CPU (`top -d 5`) | 11–15 % | 5–7 % without snapshot in the window, 45 % with (software JPEG) |
| `[ERR]`/`[WRN]` | – | 0 |
| OSD | visible | invisible (expected), OSD calls without error |

So C with the rmem fix is **passed**.

### Further observations in C

- The first main recording (`ffmpeg … -c copy run.mkv`) aborted after 5 s:
  `number of reference frames (0+2) exceeds max (1; probably corrupt input), discarding one`,
  then `Can't write packet with unknown timestamp` ([`c/ffmpeg-rec-main-t31all.txt`](c/ffmpeg-rec-main-t31all.txt)).
  With the original libimp this did not occur. Whether this is the known double-IDR spot
  (`idr_pic_id = 0`) could not be clarified.
- Timestamp jitter: The frame intervals in the sub stream are more irregular with OpenIMP
  (besides 67/68 ms also 34, 55, 88 ms; occasionally duplicate timestamps), with the original libimp
  almost only 67/68 ms. ffmpeg therefore regularly reports `non monotonically increasing dts`.
- With OpenIMP, the stock tx-isp writes at every teardown of the framesource
  `Streaming off, will not wait for buffers,chan index 0` with a stack dump in dmesg (the calling
  thread is each time OpenIMP's `FS(0)-tick`). Apparently the thread is still waiting in DQBUF when
  STREAMOFF comes. This had no consequences.

## S. H.264 stability

### S1. Long run main + sub – **passed** (2 h, without hang)

`claude/t31-all` `3f501a2`, no variables, 09:17–11:17. Main and sub permanently via RTSP
(ffmpeg decodes live to `null`, progress every 10 s), plus a snapshot every 10 s;
night picture, 15 fps. Files: [`s/`](s/).

| | Main | Sub |
|---|---|---|
| Frames in 2 h | 108002 (15.0 fps) | 108002 (15.0 fps) |
| Connection drops | 0 | 0 |
| 10 s intervals with 0 frames | 0 | 0 |
| 10 s intervals < 140 frames | 3 (133, 136, 132) + truncated end | 3 (136, 138, 133) + end |
| ffmpeg decode errors | 0 | 0 |
| `non monotonically increasing dts` (jitter, see C) | 3673 | 3117 |

The three dips were in the same interval for main and sub (e.g. ~09:42:45), without a
log message from timps or libimp; rather WLAN/network than encoder.

- Snapshots: **690/690** HTTP 200, avg. 0.44 s, max. 0.82 s ([`s/s1-snaps.txt`](s/s1-snaps.txt)).
- Counters (syslog, [`s/s1-syslog.txt`](s/s1-syslog.txt)): `completion timeout` 0,
  `without size writeback` 0, `completion found without IRQ` 0, `keeps returning EINTR` 0,
  `size written back but core still running` 0, `picture larger than the stream buffer` 0,
  `[ERR]`/`[WRN]` 0.
- timpsd CPU from `top` every 10 min ([`s/s1-mon.txt`](s/s1-mon.txt)): 0–13 % without snapshot at the
  moment of measurement, 36–80 % with (software JPEG). Free RAM dropped from ~6 to ~3.6 MB, cache rose
  accordingly; `/tmp` constant.
- Since no hang occurred, the recovery path (core reset, IDR after ≤ 2 s) was not
  triggered and is therefore not evidenced.
- Comparison run with the old library: The short run with `014edbc` + workaround (08:30–08:45,
  [`c/`](c/) section "Interim workaround") showed on main 7 of 90 intervals
  with 77–139 frames; with `3f501a2` over 2 h only the 3 common network dips.

## I. IVS motion detection

### Stage 1: algorithms against the original libimp – **passed** (after fixing the comparison tool)

Stock libimp: T31 SDK 1.1.6 uclibc/4.7.2 (md5 `6ba997693eb23c0d5e6a5fc14b56a818`) as
`/tmp/libimp-stock.so`. The CPU reports **no** `mxu_v2` (`ASEs implemented:` empty), so the
original libimp computes scalar in both runs.

```sh
rm -f /tmp/closesimd; /tmp/t31_ivs_compare /tmp/libimp-stock.so 2>&1 | tee /tmp/ivs-compare-simd.txt
touch /tmp/closesimd;  /tmp/t31_ivs_compare /tmp/libimp-stock.so 2>&1 | tee /tmp/ivs-compare-scalar.txt
rm -f /tmp/closesimd
```

Result, both runs identical ([`i/ivs-compare-simd.txt`](i/ivs-compare-simd.txt),
[`i/ivs-compare-scalar.txt`](i/ivs-compare-scalar.txt)):

```
stock path: scalar
move: frames same=8591 differ=0 oob-differ=49 (results 3708)
base: frames same=11520 differ=0 (detections 5120)
rc=0
```

The MXU2 SIMD path of the original libimp could not be checked on this CPU.

**Finding: The comparison tool never released the original handles.** The first run
(11:19, next to OpenIMP timps with ~3.6 MB free RAM) ended with a **reboot of the camera**
(boot message `CPU0 RESET ERROR PC:801A4008` = `arch_local_irq_restore`, so probably
watchdog after starvation). The repetition next to original timps showed the cause: The
process grew linearly by ~1.5 MB/s up to ~50 MB RSS, then the OOM killer ended it
(`Killed process … (t31_ivs_compare) total-vm:52868kB, anon-rss:50828kB`) after the move part
([`i/ivs-compare-unpatched-memory.txt`](i/ivs-compare-unpatched-memory.txt),
[`i/ivs-compare-unpatched-oom.txt`](i/ivs-compare-unpatched-oom.txt)). `compare_move()` and
`compare_base()` create one handle per configuration with `imp_alloc_move`/`imp_alloc_base_move`
and never release it (comment in the code: "its destructor is internal"). The original libimp
does export `imp_free_move` and `imp_free_base_move` (one argument, the handle; checked by
disassembly). Fix as a separate commit on this results branch:
`tools: t31_ivs_compare frees the stock move/base-move handles`. Afterwards the process stays at
~1–1.5 MB RSS.

### Stage 2: timps with motion detection – **passed with finding** (no results without client)

Configuration: copy `/tmp/timps-ivs.conf` with `motion.enabled = 1`, `monitor_stream = 0`,
`sensitivity = 128`, `skip_frames = 5`. **`motion.on_motion` set to `""` in the copy**,
because the camera's hook sends pictures to external services. `/etc/timps.conf` unchanged.
Start: `OI_CONF=/tmp/timps-ivs.conf … OPENIMP_T31_IVS_STATS=1 timpsd -c /tmp/timps-ivs.conf`.
Motion by PTZ instead of a walk test: `motors -d g -x 150 -y 0`, 4 s, back with
`motors -d h -x 2630 -y 0` (end position checked: `2630,0`). Procedure: [`i/ivs2-steps.txt`](i/ivs2-steps.txt).

**Finding, first run without RTSP client:** The detection delivered nothing:

```
[MOTION] motion detection started (1920x1080 grid 5x5=25 cells, sense=2/4, max 52, hold=800ms)
[ERR] MOTION  no IVS move results for over 10000ms - cycling the move channel to recover   (every 10 s)
/control: "stalled":1
```

Not a single `[IVS]` statistics line. Framesource 0 (pinned by timps for the detection)
delivered exactly **2 frames** (`DQBUF … OK` twice, `nrVBs=2`), after that DQBUF blocked.
`GetFrame` 0 times. Without an encoder consumer, the IVS path apparently does not return the two buffers to
the driver. Motion detection without simultaneous streaming (the normal case for
recording "only on motion") thus does not work. Log:
[`i/run-ivs-stalled.log`](i/run-ivs-stalled.log).

**Second run, main+sub connected via RTSP** ([`i/run-ivs2.log`](i/run-ivs2.log),
[`i/ivs-stats.txt`](i/ivs-stats.txt)):

| Item | Result |
|---|---|
| Start | `motion detection started (…)`, no `CreateChn failed`, no `does not match the configured` |
| Statistics (43 lines of 10 s each) | frames 151 (median, 125–152), results 25 (23–27) = frames/6, **0 dropped busy**; copy avg. 1.9 ms (max 22 ms), process avg. 1.3 ms (max 22 ms) |
| Quiet scene, 120 s, query every 0.5 s | **0** samples with an active cell, `stalled` 0, no false alarms (night/IR) |
| PTZ pan, sensitivity 128 | detected (`motion detected` ~1 s after start of travel), 3 of 30 samples active |
| sensitivity 255 (`motion sensitivity updated live to 4/4`) | quiet 30 s: 0; pan: detected, 2/30 active |
| sensitivity 0 (`… 0/4`) | quiet 30 s: 0; pan: detected, 3/30 active |
| Stop/start (`enabled` 0 → 1) | `motion detection stopped` / `started`, no crash, afterwards statistics and detection on pan again |
| Frame rate main/sub during the detection | 149–152 frames per 10 s, 0 gaps |
| timpsd CPU (4 samples of 5 s) | with detection avg. 10.8 %, without avg. 7.7 % (system idle 80 % and 85 %) |

- A PTZ pan changes the whole picture, so it triggers at every sensitivity. The
  expected difference between 255 and 0 could not be shown this way. That would need a
  small, weak movement (walk test).
- Active cells only shortly after the start of travel (2–3 samples of 0.5 s at `hold_ms=800`), not during the
  rest of the travel; whether this corresponds to the original (comparison stage 3) is open.
- When this run was stopped, the zombie hang (see finding below) occurred with `FS(1)-tick`.

## G. OSD in the streamer (`OPENIMP_T31_OSD=1`) – **passed**

`3f501a2`, timps with `OPENIMP_T31_OSD=1`, OSD of the streamer configuration switched on
(4 elements per stream: date/time, camera name, system uptime `T:HH:MM`, thingino logo as
bitmap; no privacy zone configured). Files: [`g/`](g/).

- Log: `[OSD] T31 IPU OSD backend requested (OPENIMP_T31_OSD=1)`, `OSD stream 0: 4 overlay(s)`,
  `OSD stream 1: 4 overlay(s)`; **no** `backend disabled`, no IPU message in dmesg.
- Visual check (camera pictures only looked at locally, not committed):
  - Snapshot (JPEG, 1920×1080): all four elements at the expected place, text clean,
    logo with coloured "ing" (colour and alpha correct).
  - RTSP main: time keeps counting (two frames 2 s apart show `11:35:32` and
    `11:35:34`), no flicker, no old time.
  - RTSP sub (640×360): OSD present, scaled smaller, logo visible.
  - Uptime element jumps every minute (`0:00:15` → `0:00:16`), as intended by the format.
- 5 min with snapshot every 10 s: 27/27 snapshots, avg. 0.51 s, main/sub 15 fps, 0 decode errors.
- **CPU** (2.5 min each, main+sub via RTSP, **without** snapshots, `top -d 5`, 11 samples):

| | timpsd avg. | system idle avg. |
|---|---|---|
| Original libimp (OSD via original) | **13.0 %** | 79.3 % |
| OpenIMP without variable (OSD invisible) | 7.8 % | 83.0 % |
| OpenIMP `OPENIMP_T31_OSD=1` | **7.9 %** | 84.8 % |

  The IPU OSD measurably costs nothing and is clearly below the original libimp.

## Finding: timpsd stays on exit as a zombie with a spinning kernel thread

**5 of well over 40 stops** of an OpenIMP timps did not end completely (11:40 after G, 11:48 after
a short run without variable, 12:30 after the HW JPEG run, 12:37 after the first IVS run,
12:47 after the second IVS run with streaming clients). Each time:

- The main thread is `Z` (zombie, parent process PID 1). An OpenIMP thread **`FS(0)-tick`**
  or **`FS(1)-tick`** keeps running, 100 % in the kernel (`utime` ≈ 0, `stime` +~95 ticks/s,
  system 100 % sys). `SIGKILL` has no effect.
- Consequence: `/run/timps.lock` counts as taken (`another timpsd instance already holds
  /run/timps.lock`), the next start stops after `audio in`, the bind mount cannot be
  released (`Device or resource busy`). **Only a reboot helps.**
- The dmesg messages `Streaming off, will not wait for buffers,chan index 0` of the stock tx-isp
  always come from `Comm: FS(0)-tick`; call trace `SyS_ioctl` → `vfs_ioctl` → tx-isp
  (`0xc0233070`, `0xc023f2e4`). So the tick thread calls DQBUF on a channel that is
  stopped or is just being stopped. At process end the stock driver spins in this
  ioctl.
- Evidence: [`g/fs-tick-spin-evidence.txt`](g/fs-tick-spin-evidence.txt),
  [`g/fs-tick-spin-evidence-2.txt`](g/fs-tick-spin-evidence-2.txt),
  [`g/fs-tick-spin-evidence-3.txt`](g/fs-tick-spin-evidence-3.txt),
  [`i/stuck-after-ivs.txt`](i/stuck-after-ivs.txt).
- Targeted reproduction with short runs (20–40 s) **failed 28 times**
  ([`g/stop-cycles.txt`](g/stop-cycles.txt)): stop while streaming, when idle,
  0.5–3 s after the last client disconnected, with output via a pipe, with double
  SIGTERM. The hits came after runs of 1–9 min, with and without clients, with a tick thread that had been
  running for a long time (TID from the start phase) and with a newly created one.
  No simple pattern is recognisable.
- With the original libimp this did not occur in any stop.
- Proposal: Stop and join the tick thread before `STREAMOFF`/`DisableChn` and before the process end;
  no DQBUF on a channel that is not streaming. Whether open-tx-isp shows the same
  is open (see below, open-tx-isp could not be loaded).

## E/H. JPEG in the streamer – **passed** (E run C/D; H only partly)

`3f501a2` contains the changes from `claude/t31-perf` (faster software JPEG, HW JPEG reads
directly from rmem). E and H therefore coincide. 3 min each, main+sub via RTSP, on the camera
a snapshot loop (`curl`, 1 s pause between the fetches), `top -d 5`. Files: [`e/`](e/).

| | Original libimp | OpenIMP software JPEG (run C) | OpenIMP `OPENIMP_T31_HW_JPEG=1` (run D) |
|---|---|---|---|
| Snapshots | 129/129 | 112/112 | 128/128 |
| Snapshot time avg. / max | 0.29 / 0.55 s | 0.46 / 1.00 s | **0.28 / 0.54 s** |
| JPEG size avg. | 290 KB | 283 KB | 289 KB |
| timpsd CPU avg. | 19.4 % | **66.9 %** | **18.5 %** |
| System idle avg. | 64.7 % | 17.6 % | 65.0 % |
| Main/sub fps, decode errors | 15 / 15, 0 | 15 / 15, 0 | 15 / 15, 0 |

**Log run D** ([`e/hwjpeg-log-lines.txt`](e/hwjpeg-log-lines.txt)):

```
[Codec] HWJPEG: T31 hardware JPEG requested (OPENIMP_T31_HW_JPEG=1)
[Codec] AVPU: T31 companion stage disabled
[Codec] HWJPEG: no AVC IRQ waiter yet, software until one runs
[Codec] HWJPEG: ready, ep1=0x00df0000
[Codec] HWJPEG: 1920x1080 q75 -> 294175 bytes [#1]
…
[Codec] HWJPEG: 1920x1080 q75 -> 295808 bytes [#800]
```

No `disabled`, no `no IRQ 4`, no `stream overflow`. Over 800 hardware JPEGs in 3 min.
The A1 finding on the AVPU coherent memory did not apply here (`ep1` is in rmem).

**Picture quality run D:** Snapshot looked at locally (camera picture, not committed): complete,
no stripes, no block garbage, no old picture content, brightness and sharpness as with
software JPEG. `OPENIMP_T31_HW_JPEG_SRC_COHERENT=1` was not needed.

**H, point 1 (bit-identical software JPEG compared with `claude/t31-re`):** Not checked. With
live pictures this cannot be compared bit-exactly; a comparison on a fixed source picture
would be necessary.

## S2. Snapshot load (F6) – **passed with remark**

`ffprobe -show_frames … -read_intervals %+30` on the main stream during the runs from E, plus a
run without snapshots:

| | Frames in 30 s | Interval median | p99 | max | Intervals > 100 ms |
|---|---|---|---|---|---|
| OpenIMP without snapshots | 449 | 67 ms | 89 ms | 101 ms | 4 |
| OpenIMP software JPEG, 1 snapshot/s | 449 | 67 ms | 102 ms | 149 ms | 33 |
| OpenIMP HW JPEG, 1 snapshot/s | 447 | 67 ms | 135 ms | 164 ms | 93 |
| Original libimp, 1 snapshot/s | 442 | 67 ms | 69 ms | 71 ms | 0 |

- No gaps: Under software JPEG load the largest gap stays at 149 ms, frames are not
  lost. F6 (software JPEG does not block H.264) is thus fulfilled.
- The timestamps are in general less even with OpenIMP than with the original libimp (see C,
  jitter), more so under load. Surprising: With **HW JPEG** the jitter is larger than with
  software JPEG (93 instead of 33 intervals > 100 ms). Presumably H.264 and JPEG wait for the same
  AVPU IRQ waiter or core lock; not investigated further.

## D. Companion stage A/B – **passed** (B at least as good as A)

| Run | Variable | Duration | Main/sub | Decode errors | Snapshots |
|---|---|---|---|---|---|
| A (= C with rmem fix) | none (`[Codec] AVPU: T31 companion stage enabled`) | 10 min | 9001 frames each, 15 fps | 0 | 58/58 |
| B | `OPENIMP_T31_COMPANION_STAGE=0` (`… companion stage disabled`) | 5 min | 4501 frames each, 15 fps | 0 | 27/27, avg. 0.44 s |

In B no timeouts, no `stall` messages, no intervals below 149 frames/10 s (except
the truncated end). S2 measurement in B (snapshot every 10 s): median 67 ms, max. 138 ms.
So H.264 does not depend on the companion stage. Files: [`d/`](d/). Recordings as a
video file were not saved (decoded live), the picture in B was not looked at separately.

## F. `SetIntegrationTime` on T20 – **skipped**

No T20 test camera with OpenIMP in the assignment.

## S3/S4 – **skipped**

- S3 (re-creating the sub stream at run time): timps re-creates encoder channels only via `restart video`,
  which restarts all channels. Not done.
- S4 (`OPENIMP_T31_AVC_LEGACY=1`, 1 h): not done for time reasons. Since S1 had not a single
  hang, the A/B comparison for F1/F2 would be of little value anyway.

## Addendum: open-tx-isp via insmod – **not possible**

At the human's request, open-tx-isp was to be tested without flashing: stop timps,
unload the stock `sensor_sc4336p_t31` and `tx_isp_t31`, load the modules from the open-stack build
(appendix) from `/tmp`. `avpu.ko` is bit-identical in both builds and stays loaded, the
kernel is configured the same (vermagic identical).

```sh
rmmod sensor_sc4336p_t31   # rc=0
rmmod tx_isp_t31           # rc=139, Segmentation fault
```

Kernel oops in `class_unregister` (`epc 801dbea4`), called from the exit routine of the
stock `tx_isp_t31` (`ra c0247b60`). The module stays as `Unloading`, another module
of the same name cannot be loaded. Then reboot. Evidence:
[`open-tx-isp/rmmod-oops.txt`](open-tx-isp/rmmod-oops.txt).

So the stock ISP driver cannot be unloaded. Testing open-tx-isp on this camera
needs a permanent change (not loading the modules at boot: `/etc/modules.d`, overlay or
U-Boot env) or flashing the open-stack image. The rules of the test exclude both.

## Appendix: open-stack image for this profile (built, not flashed)

In a separate worktree of the thingino checkout (branch `ciao`, `cf463ffc3`, with the same
local `ingenic-sdk` change as the running image), `BR2_PACKAGE_THINGINO_ISP_OPEN=y` was set via a
fragment. Build 11:45 min, successful.

- open-tx-isp `e92166b985606613f2395831bac65413c7542877` (opensensor/open-tx-isp, `main`),
  `tx-isp-t31.ko` reports the upstream author string and `description=TX-ISP Camera Driver`.
- OpenIMP in the image: opensensor/openimp `main` `9cab2192` (**not** this branch).
- Compared with the stock image, only `openimp-tuningd`, `S30openimp-tuning` and
  `openimp-tuning.conf` are added. The tuning daemon starts only with the raptor-v4l2 backend,
  so not with timps.

## Miscellaneous (all deviations and incidents)

- 07:51: `timpsd --version` to determine the version briefly started a second instance; it
  ended immediately because of `/run/timps.lock`, the running streamer was untouched.
- A1: `dmesg -c` emptied and `echo 3 > /proc/sys/vm/drop_caches` executed (volatile).
- 11:20: Camera reboot caused by the comparison tool (see I, stage 1).
- 11:47, 11:52, 12:31, 12:38, 12:49: Reboots because of the zombie hang (see finding).
- 12:56: Reboot after the oops at `rmmod tx_isp_t31`.
- My test harness at times started timpsd via a pipe (log filter). timps' stdout is
  buffered then, and the file log is incomplete; in these cases syslog (`logread`) was
  evaluated. The pipe was not the cause of the hang (4 control tests clean).
- The first two `ffmpeg` recordings in C with `-c copy` to Matroska aborted on
  missing timestamps; afterwards decoding was done live to `null`.
- Camera pictures were only looked at locally and not committed.
- Nothing changed under `/etc`, in the overlay or in the flash; files only under `/tmp`.
