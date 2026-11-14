# T31 test results, round 2: OpenIMP `claude/t31-all` (`dfbd90f`) and open-tx-isp `claude/t31-isp-all`

Test run on 2026-09-30 after the hand-over from the cloud session. Run by a local
Claude Code session over SSH. Camera anonymised (`cam-A`, `192.0.2.x`). Round 1 (original
stack, OpenIMP `3f501a2`) is on the branch `claude/t31-test-results-20260930`.

## Header

| | |
|---|---|
| Camera | WUUK Y0510, profile `wuuk_y0510_t31x_sc4336p_ssv6158`, sensor SC4336P (2560×1440) |
| SoC | T31X, `isa: mips32r1`, `ASEs implemented:` empty (no MXU2) |
| Memory | `mem=78M@0x0 rmem=50M@0x4e00000` |
| thingino | `ciao` `cf463ffc3` with `BR2_PACKAGE_THINGINO_ISP_OPEN=y`, plus the three T31 patches `0004`–`0006` from open-tx-isp from `aperto`; flashed by a complete OTA (`make ota`), with the human's approval |
| Kernel | `3.10.14__isvp_swan_1.0__` |
| ISP module flashed ("old") | open-tx-isp `e92166b` + thingino patches `0001`–`0006` |
| ISP module test ("new") | open-tx-isp `claude/t31-isp-all` `97cccfa2` + thingino patches `0001`–`0006`, md5 `03b40e861c0724309e098a1c7cbfcf96` |
| libimp flashed | OpenIMP `3f501a2` (round 1) |
| libimp test | OpenIMP `claude/t31-all` `dfbd90f`, md5 `8b48664e0c0252c99625d18340f9a274`, via bind mount |
| Streamer | timps v1.9.28, for the tests started by hand with a copy of the configuration in `/tmp` |
| Main/Sub | H.264 1920×1080 / 640×360, 15 fps |

Rules as in the test plan: only this camera, files only in `/tmp`, bind mount, no camera pictures
in the repo (only looked at locally, with consent).

## Overview

| Area | Result |
|---|---|
| Phase 1 C (FrameSource cycles, rmem fix) | **passed**; No-Reuse reproduces the old error |
| Phase 1 S1 (78 min, 15 fps) | **passed** |
| HW JPEG as default (`claude/t31-hwjpeg-default`) | **passed**; ~17 % instead of ~70 % CPU at 1 snapshot/s, no jitter disadvantage on open-tx-isp |
| FrameSource stop (`claude/t31-fs-stop-order`) | **passed**; fixes a deadlock (unconditional `pthread_cancel`) that occurred with the new module after 6 cycles |
| G OSD via IPU (also in HW JPEG) | **passed** |
| I motion with client | **passed**; without client with `dfbd90f` no result → **fixed** in `claude/t31-ivs-idle` |
| Stop takes 4 s + `hard exit` with an active client | Cause audio `NOBLOCK` → **fixed** in `claude/t31-ai-noblock` (stop 1.1 s, AAC from the start) |
| Phase 2 T1–T7 (new module) | **passed** (T2 with drop 0 and with drop 6) |
| Phase 2 latency | new module: max. 660 µs at day/night (old 1313 µs), 0 seconds > 1 ms |
| Phase 2 tuning | passed except 5.6 (not conclusive); `it_max` and `image.ae_it_max_us` now work; resetting to 0 is missing in timps |
| Phase 2 rmmod (L1) | old and `97cccfa2`: **oops** → **fixed** in open-tx-isp `claude/t31-isp-fixes` (3 load cycles clean) |
| C + S with both new parts | **passed** (with the stop fix); final run 60 min @ 25 fps with all corrections **passed** |
| Untested (branches exist) | AU check/H.264 double picture, quiet logs + fast idle stop, timestamps on the stock driver, `fw_ota.sh` fix (thingino, local) |

### Branches from this round

OpenIMP (`Lu-Fi/openimp`, all on `dfbd90f`):
`claude/t31-hwjpeg-default` (5087c02), `claude/t31-fs-stop-order` (6ce3114), `claude/t31-ivs-idle` (7c5ba1b), `claude/t31-ai-noblock` (0a2331a), `claude/t31-all-fixes` (4653f0a, merge of the four), `claude/t31-quiet-logs` (on all-fixes, untested), `claude/t31-fs-timestamps` (on all-fixes, untested), `claude/t31-avc-au-check` (untested).

open-tx-isp (`Lu-Fi/open-tx-isp`, on `claude/t31-isp-all` 97cccfa2): `claude/t31-isp-rmmod-fix` (c7c2e7d2), `claude/t31-isp-csi-mutex` (0e1c5773), `claude/t31-isp-fixes` (6caeb75e, merge, device-tested).

---

# Phase 1: new libimp, flashed open-tx-isp

## C. Regression with FrameSource cycles – **passed**

Files: [`p1-c/`](p1-c/).

### 25 FrameSource cycles

As in round 1: start timps without variables, wait 45 s, then 25 times fetch main for 8 s via RTSP
and disconnect, each time followed by 2 s sub, 5 s pause
([`p1-c/cycles-25.txt`](p1-c/cycles-25.txt), log [`p1-c/run-cycles-25.log`](p1-c/run-cycles-25.log)).

| | Result |
|---|---|
| FrameSource enables main / sub | **26 / 25** (in cycles 19–23 a WebRTC client of the web UI held main open for 140 s, so there are no cycles there) |
| `allocation failed` / `rmem out of memory` | 0 / 0 |
| ffmpeg errors main | 0 in all cycles |
| timps PID | the same throughout |
| Picture check | 1st and 16th frame after each reconnect looked at locally: complete pictures, no block garbage. The numerical deviations in cycles 2–4 are the exposure settling after the start, from cycle 19 a real scene change (PTZ position and open door). |

**Finding, old module:** Once the main stream froze after a day/night switch and
recovered only through the timps recovery:

```
13:46:08.423 [INF] HAL_ING      control image.running_mode=1
13:46:16.383 [INF] HAL_ING      control image.running_mode=1        (re-assert, stream running)
13:46:17.003 [WRN] HAL_ING      chn0: PollingStream idle (rc=-1, miss#1) - encoder emits no frames
13:46:21.507 [ERR] HAL_ING      chn0: encoder dead after 10 consecutive misses - forcing a framesource disable/enable cycle to recover (recovery attempt 1/5)
```

After that the stream ran again (recovery attempt 1 was enough). This is the freeze after a
day/night switch that `claude/t31-isp-all` is meant to fix; comparison in phase 2 (T2, L5).

### Control test `OPENIMP_RMEM_NO_REUSE=1` – **passed**

[`p1-c/cycles-noreuse.txt`](p1-c/cycles-noreuse.txt), [`p1-c/run-noreuse.log`](p1-c/run-noreuse.log):
The old error comes back, after 6 enables:

```
[DMA] rmem out of memory: requested 6266880, used 49426432 of 52428800, largest free block 3002368, 20 allocations
13:57:05.622 [ERR] HAL_ING      framesource 0: EnableChn failed (attempt 1)
…
13:57:31.313 [INF] MAIN         teardown complete - exiting
```

timps ends cleanly (no zombie as in round 1 on the stock driver).

Incidentally: The camera was unreachable from my machine for ~40 min afterwards, but kept
running (uptime unchanged, DHCP renewals in syslog). This was a network problem between
machine and camera, not a camera fault.

### 10-minute run

Main+Sub decoded live via RTSP, snapshot every 10 s, `top`
([`p1-c/c3-summary.txt`](p1-c/c3-summary.txt), [`p1-c/run-c3.log`](p1-c/run-c3.log)):

| | Result |
|---|---|
| Main / Sub | 8891 / 8890 frames in 600 s (15 fps), no interval < 140 except the truncated end |
| Decode errors | 0 |
| Snapshots | 56/56, avg. 358 KB, avg. 0.53 s |
| S2 measurement (30 s frame timestamps main) | median 67 ms, p99 73 ms, **max 80 ms**, 0 intervals > 100 ms |
| `[ERR]`/`[WRN]` | 1× `jpeg chn3: PollingStream idle (miss#1)`, without consequences |
| timpsd CPU | 13–32 % (software JPEG snapshots in the measurement window) |

The frame intervals on open-tx-isp are clearly more even than in round 1 on the
stock driver (there max. 101 ms without snapshot load).

## Code review: HW JPEG as default? (review by a second Claude instance, read-only, base `dfbd90f`)

Reason: With software JPEG, timpsd under snapshot load is clearly above the original libimp
(round 1, E: 66.9 % versus 19.4 %), with `OPENIMP_T31_HW_JPEG=1` on par (18.5 %).

Recommendation: **default with conditions.** Beforehand:

1. *(medium)* `src/t40/codec-t40.c:7859-7869`: When the stream buffer is grown after an
   overflow, the old coherent buffer is freed before the new allocation. If the
   ~2.2 MB allocation from kernel lowmem fails (fragmentation, cf. A1 in round 1),
   `disable("DMA allocation failed")` (`:7961`) switches the path off permanently. Fix: allocate the new one first,
   keep the old buffer on failure; better stream/EP1 from rmem like the AVC path.
2. *(medium)* Start race: HW JPEG only checks whether the IRQ waiter is running (`:7935`), the global
   AVC init happens only in the first AVC process (`:9335-9341`). An early JPEG job can collect three
   timeouts and switch the path off permanently (`:8035`). Fix: before the AVC init, encode the frame
   in software, do not switch off.
3. *(test condition)* HW JPEG always turns off the companion stage (`:8114-8122`). Before switching over,
   run test C with cycles and the 10-minute run with `OPENIMP_T31_HW_JPEG=1`.

Minimal change: switch `codec-t40.c:7784-7788` and `openimp_p2_encoder.c:231-235` identically to
"on, except `OPENIMP_T31_HW_JPEG=0`"; docs from opt-in to opt-out.

Further points: All other reasons for switching off are permanent, but without flapping; `disable` should
be logged as an error. The larger timestamp jitter with HW JPEG comes from every
JPEG job (~10–15 ms incl. 3 MB copy) holding the shared core lock; uncritical for 1 snapshot/s,
noticeable for MJPEG. EP1 and stream buffer come from `/dev/avpu` (kernel lowmem), not from rmem.

**Zombie hang from round 1:** no relation to HW JPEG. `IMP_FrameSource_DisableChn` does
STREAMOFF before the join of the tick thread (`src/framesource/framesource_tseries.c:1993-2005`); the
inner loop calls DQBUF again without a `running` check (`:1019-1046`). In the stock tx-isp
this DQBUF spins in the kernel after STREAMOFF. Commit `70c42bc` (close fd before pool release)
does not fix this and was already contained in the tested state. Proposal: on T31, first stop and join
the thread cooperatively, then STREAMOFF. The hang did not occur on open-tx-isp.

## S1. H.264 long run – **passed** (78 min, without hang)

`dfbd90f` on the flashed open-tx-isp, no variables, 14:57–16:15. Main and sub permanently decoded live via
RTSP, snapshot every 10 s. Shortened because of a break by the human (departure), otherwise
2 h. The raw log was filtered and emptied every minute on the camera (the per-frame debug lines
of OpenIMP otherwise produce ~25 MB/h in `/tmp`). Files: [`p1-s/`](p1-s/).

| | Main | Sub |
|---|---|---|
| Frames in 78 min | 69397 (15.0 fps) | 69393 (15.0 fps) |
| Connection drops / 10 s intervals with 0 frames | 0 / 0 | 0 / 0 |
| 10 s intervals < 140 frames | 2 (135, 139) | 0 |
| Decode errors | 0 | 0 |

- Snapshots **444/444** HTTP 200, avg. 0.49 s, max. 0.89 s.
- Counters (syslog and filtered log): `completion timeout`, `without size writeback`,
  `completion found without IRQ`, `keeps returning EINTR`, `size written back but core still
  running`, `picture larger than the stream buffer`, `[ERR]`, `[WRN]`: **all 0**.
- Memory (`top`, used incl. cache): 36.2 MB after the start, afterwards constant 39.8–40.3 MB.
  `/tmp` constant at 3 %. timpsd CPU from the 10-minute samples: 0–33 % depending on a snapshot in the window.
- No hang, so the recovery path was not triggered.
- After the run, stop in 0.3 s, bind mount released, `S95timps` started normally.

## New corrections from this round (local branches, created by agents, checked on the device)

All based on `dfbd90f`, each on its own branch, each change as its own commit.

| Branch | Content | Device test |
|---|---|---|
| `claude/t31-hwjpeg-default` (`5087c02`) | HW JPEG: stream buffers grow without permanent switch-off; no JPEG job before the AVC init; switch-off logged as an error; **HW JPEG on by default**, `OPENIMP_T31_HW_JPEG=0` switches back | passed (below) |
| `claude/t31-fs-stop-order` (`6ce3114`, contains the HW JPEG commits) | FrameSource stop: STREAMOFF → join worker, `pthread_cancel` only after 1 s as a last resort; STREAMOFF of all channels in the destructor/fatal handler; single log line saying which ISP driver is running. Also contains the withdrawn intermediate states `1f07faa`/`b77e0cf`/`2ead380`/`1deebd7` (diagnosis) | passed (below) |
| `claude/t31-ivs-idle` (`7c5ba1b`) | IVS without stream client: frames not picked up go back to the driver after 1 s | passed (below) |
| `claude/t31-ai-noblock` (`0a2331a`) | Audio: `IMP_AI_GetFrame(NOBLOCK)`/`PollingFrame(timeout)` as in the original libimp (record thread + FIFO) | passed (below) |

### HW JPEG as default (`5087c02`)

Code review by a second instance (section above), implemented as 4 commits.

| Test (open-tx-isp, flashed module) | Result |
|---|---|
| 12 FrameSource cycles + snapshot every 5 s | 29 enables, 0 decode errors, 41/41 snapshots, `HWJPEG: ready` |
| 10-minute run (snapshot every 10 s) | Main 8890 frames (15 fps), 57/57 snapshots avg. 0.23 s, timpsd ~9.6 % CPU, main frame intervals max. 84 ms. Once at start `chn1: encoder dead` with recovery (sub got 2× `503`, then 10 min clean); did not occur again in 11 further targeted starts (6 individually, 5 main+sub in parallel) |
| CPU/jitter at 1 snapshot/s, 3 min | HW: timpsd ~17 %, snapshot avg. 0.21 s, frame interval max. 92 ms, 0 > 100 ms. `OPENIMP_T31_HW_JPEG=0`: ~70 %, 0.42 s, max. 104 ms. On open-tx-isp, HW JPEG no longer increases the jitter (unlike in round 1 on the stock driver). |
| Opt-out | `HWJPEG: T31 hardware JPEG off (OPENIMP_T31_HW_JPEG=0), software encoder` |

Files: [`hwjpeg/`](hwjpeg/), [`fs-stop/hwj*-cycles.txt`](fs-stop/).

### FrameSource stop order

Starting point: the zombie hang from round 1 (stock driver). The first approach (`1f07faa`: stop the worker before STREAMOFF) ran into the 1 s timeout on open-tx-isp for ~25 % of the shutdowns; the diagnosis (`1deebd7`) showed `worker in step dqbuf (dq ioctl) for 1353–3075 ms … exited after STREAMOFF`: **both drivers ignore `O_NONBLOCK` in DQBUF**, STREAMOFF is the only waker. `b77e0cf` (select before every DQBUF) did not help and coincided with a new H.264 warning on stream entry, and was withdrawn.

Final design (`92d00db`, `6ce3114`): STREAMOFF → join, cancel only as a last resort after 1 s; at process end STREAMOFF on all channels. Rationale from the HLIL of the stock module: its DQBUF spins only in an `-ERESTARTSYS` loop when a signal (also `pthread_cancel`/SIGKILL) hits a thread while the stream is **still running**.

| Test | Result |
|---|---|
| 12 cycles + snapshots (flashed module) | 0 timeouts in 38 shutdowns, 0 decode errors, 41/41 snapshots, log `T31 ISP driver: open tx-isp` |
| 8 stops with running stream, 3× `kill -9` | no zombie, restart afterwards normal |
| **20 cycles on the new module** | 48 enables, 0 errors, 69/69 snapshots (see deadlock finding below) |

I can no longer check this on the stock driver (origin of the zombie finding), because the camera now has open-tx-isp.

**Deadlock finding with `dfbd90f` on the new module:** In the C cycle test with the new module and `dfbd90f`, timps stopped completely after 6 cycles (`/control` does not answer, RTSP connections stay in the listen queue, all threads sleep, 0 % CPU). Last output `[KernelIF] Stream stopped` of channel 1, then nothing ([`fs-stop/run-p2-c2-deadlock.log`](fs-stop/run-p2-c2-deadlock.log)). This fits the unconditional `pthread_cancel` in `dfbd90f`, which aborts the worker in the VBM mutex. With `claude/t31-fs-stop-order`, 20 cycles then ran without errors. `kill -9` cleared the hang, no reboot needed.

### G (OSD) with HW JPEG – passed

`OPENIMP_T31_OSD=1` with `5087c02`: OSD (date/time, name, uptime, logo) also visible in the hardware JPEG snapshot (looked at locally), 15/15 snapshots avg. 0.23 s, timpsd ~9.6 % CPU with OSD + HW JPEG + snapshot every 10 s, 0 errors. [`p1-g/`](p1-g/).

### I (motion) on open-tx-isp

- **Stage 2 with client – passed** as in round 1: 120 s quiet scene 0 false alarms, every PTZ pan detected, sensitivity taken over live, stop/start ok, CPU with/without detection ~10 %/~6 %, IVS statistics 149 frames/10 s, 0 dropped. PTZ back to the starting position. [`p1-i/`](p1-i/).
- **Without client – with `dfbd90f` still no result** (also on open-tx-isp): 2 frames delivered, then `no IVS move results … cycling`, `stalled:1`. Cause (analysis): the filled VBM ready queue is emptied only by the encoder (`PollingStream`); without an encoder consumer the two buffers never return to the driver.
- **With `claude/t31-ivs-idle` – passed:** without client `[IVS] chn0: 149 frames, 0 dropped busy, 25 results`, 0 stall messages, `stalled:0`, PTZ pan detected; with main+sub client unchanged (890 frames/60 s, IVS statistics the same, 0 errors). [`p2-ivs-idle/`](p2-ivs-idle/).

### Finding: timps needs 4 s to exit when a client has just connected – fixed with `claude/t31-ai-noblock`

On all OpenIMP states (also `3f501a2`/`dfbd90f`) every stop with a running client ended after ~4 s with `timpsd: shutdown alarm fired - hard exit` (20/20 in T1). Timestamped capture via `/dev/kmsg` ([`p2/stopdiag-dmesg.txt`](p2/stopdiag-dmesg.txt)): after `framesource 0 disabled` 3.9 s of silence, the codec teardown is never reached; with the new module no `MSCA channel still busy`, so no kernel timeout. Cause (analysis, confirmed on the device): on the idle→active transition timps empties the audio buffer with `IMP_AI_GetFrame(NOBLOCK)` up to 512 frames; OpenIMP ignored `NOBLOCK` and blocked 40 ms per frame → ~20 s (log `audio resume: flushed 512 stale AI frame(s)` exactly ~20 s after the connect), audio is missing during this time (`no AAC within warmup -> video-only mp4`), and a stop in this window waits for the audio thread.

With `claude/t31-ai-noblock`: 4/4 stops (5 s and 12 s after client connect) in 1.08–1.11 s with `teardown complete`, no `hard exit`, `flushed 6 stale AI frame(s)`, no `no AAC` warning, the fMP4 contains **H.264 and AAC** from the beginning. Additional recommendation for timps: limit the drain loop in time (robust against both libimp semantics).

---

# Phase 2: new open-tx-isp module (`claude/t31-isp-all` `97cccfa2` + thingino patches)

## Module swap via insmod – not possible, therefore flashed

As in the guide (stop streamer, `rmmod sensor_sc4336p_t31`, `rmmod tx_isp_t31`): The **flashed old module** (opensensor `e92166b` + patches) crashes on `rmmod` – `Unable to handle kernel paging request at virtual address 00000000, epc 8037cbf0 (__mutex_lock_slowpath)`, module stays `Unloading` ([`p2/rmmod-old-oops.txt`](p2/rmmod-old-oops.txt)). A first attempt with `panic_on_oops=1` led to a reboot without traces; the second without it delivered the oops. With the human's approval, an image with the new module as the installed module was therefore built and flashed by a complete `make ota` (md5 of the module in the image `bbbfbbdd581448c667e7e61e47b80ad4`). After the flash the SSH host key changed (new data partition), replaced with approval.

libimp for all phase 2 tests: `dfbd90f` via bind mount (unless stated otherwise). timps with configuration copy `/tmp/timps-test.conf`, stream fetch locally via `https://127.0.0.1:8880/stream.mp4?chn=0` (fMP4). Note: The INFO markers of the guide (`stopped channel`, `ISP core quiet after`, `already open …`, `Day/night mode updated`) do not appear in dmesg with `print_level=1` and thingino patch 0006; evaluation was done via byte counts, registers and FAIL markers.

## Life cycle T1–T7

| Test | Result |
|---|---|
| T1 start/stop ×20 | **passed**: each round 222–342 KB in 5 s, afterwards `0x13309804`/`0x13309808` = 0, no FAIL hits. Each stop ~5 s with `hard exit` → audio finding above (fixed with `claude/t31-ai-noblock`) |
| T2 day/night ×20 with running stream | **passed**: all short fetches with data, permanent client ran through (52.8 MB), no `encoder dead`. (With `isp_day_night_switch_drop_frame_num=0`; the parameter can only be set at `insmod`.) For comparison phase 1 with the old module: once 4.5 s freeze after a switch |
| T3 `kill -9` ×10 | **passed**: `0x13309804`=0 after every kill, no zombie, restart delivers 378–660 KB/5 s |
| T4 30 snapshots in parallel to the stream | **passed**: 30/30 `ffd8`, avg. 195 KB, stream client 120 s continuously (24.9 MB) |
| T5 second `open()` ×5 | **passed**: data rate with/without the same (4.36/4.24 MB in 30 s), no gap |
| T6 20 day/night kicks without client | **passed**: data immediately afterwards (300 KB/5 s), no FAIL hits |
| T7 memory | slab 5196 → 5472 KB over all tests, MemFree unchanged; no steady growth |

Files: [`p2/T*.txt`](p2/).

## Latency old versus new (same libimp `dfbd90f`, main stream active)

| | old module (flashed, `e92166b`+patches) | new module (`97cccfa2`+patches) |
|---|---|---|
| L2 base load 60 s, max | 463 µs | 697 µs |
| L2 24 day/night switches in 120 s, max | **1313 µs** | **660 µs** |
| Seconds with max > 1 ms | 2 | **0** |
| Wakeups 500–999 µs | 194 | 198 |
| L4 `isp-m0` / `isp-w02` per s | 115.9 / 29.6 | 121.1 / 29.6 |
| L4 `ctxt` per s | 2736 | **2618** (−118) |

The day/night peaks above 1 ms disappear with the new module. Files: [`p2/lat-*.txt`](p2/), [`p2/L4-*.txt`](p2/).

## Tuning tests (T31_ISP_TUNING_TEST.md, section 5)

Baseline values with the old module: [`p2/tune-baseline-alt.txt`](p2/tune-baseline-alt.txt) (there `expr` still reported `it_max=0`). Tests with the new module, timps running: [`p2/tuning-neu*.txt`](p2/).

| Test | Result |
|---|---|
| 5.1 Logging | passed (no `Set control` lines) |
| 5.2 B7/B15 zones | passed: AF zones only `00`, AE zones real, changing values, no `a5` pattern, 0x8000045 20× `00` |
| 5.3 B2/B3 it_max | **passed**: `it_max=2246` (old: 0), cap 1123 takes effect immediately (`it` → 1123), 0 → `EINVAL`, reset ok. SetAeMin only read |
| 5.4 B14 / 5.5 B12 | passed (`EPERM` or 2× `EINVAL`) |
| 5.6 Anti-flicker by FPS | not conclusive: the expected `T31_DEFLICK` line does not appear with `print_level=1` |
| 5.7 B9 ModuleControl | passed (registers unchanged) |
| 5.8 B4 sharpness | **passed** (with 1 s pause after each set): 128/200/200/128/60/60/128 → identical registers for identical values, back to the starting value |
| 5.9 B5 sinter | **passed**: 200 takes effect immediately, idempotent, back to the starting value |
| 5.10 B6 defog | **passed**: 200 → 200, 200+128 → 128 |
| 5.11 B8/B17 | passed: weights written back unchanged, weight 9 → `EPERM`, histogram filled, invalid pointer → `EFAULT`, no oops. *Test tool error:* `parse_list` copies the list into `char tmp[1024]` and truncated 225 values to 205; enlarged locally to 8192 |
| 5.12 B11 gamma | **passed**: same LUT → registers unchanged, `lut[1]`+0x10 → immediately `0x00090000` → `0x000A0000`, back ok |
| 5.13 B10 CCM | passed (register = the matrix set, also after 3 s; attribute byte 0 = 1; back to auto). The current matrix was set, not a different one |
| 5.14 B13 AeAttr | **passed**: manual IT 1123 taken over and held, again 2×, back to auto |
| timps `image.ae_it_max_us` | **now takes effect**: `image.ae_it_max_us=10000 -> capped AE at 454 lines; GetExpr now reports max=454 lines (9988us)`; `expr` confirms `it_max=454`. Resetting to 0 did not lift the cap (stayed 454) – noted for timps/libimp |

## rmmod of the new module (L1) – not passed, fix in progress

- **`97cccfa2` (claude/t31-isp-all):** `rmmod tx_isp_t31` crashes exactly like the old module (`__mutex_lock_slowpath`, NULL). addr2line: `tx_isp_exit` → `tx_isp_vic_remove` → `tx_isp_vic_stop` ([`p2/rmmod-new-oops.txt`](p2/rmmod-new-oops.txt)). The commit "cancel the day/night work on module exit" does not fix this crash. Reboot needed.
- **Fix branch open-tx-isp `claude/t31-isp-rmmod-fix` (by an agent, 2 commits):** Causes according to the analysis: `vic_frame_end_lock` never `mutex_init`ed, `kfree(vic_dev)` before its remove (use-after-free + double free), `ourISPdev` freed in the core remove. In addition `claude/t31-isp-csi-mutex` (3 commits: CSI mutex lay over OEM fields and was never initialised, VIC frame-end completions and custom-AE completion never initialised; `BUILD_BUG_ON` layout check).
- **Device test of the flashed state `claude/t31-isp-csi-mutex` `0e1c5773`** (module md5 `8c9b353f5e460e16896f362fe348eb7a`): The module runs normally (boot, stream). `rmmod sensor` ok, `rmmod tx_isp_t31` leads to a **reboot**. Live capture via `/proc/kmsg` ([`p2/kmsg-rmmod.txt`](p2/kmsg-rmmod.txt)): `Trying to free nonexistent resource <13310000-1331ffff>` and `<133e0000-133effff>`, directly afterwards an oops in a foreign path (`kworker` → `call_usermodehelper` → `do_execve` → `kernel_read` → `__kmalloc`, BadVA 0x40) – damaged slab free list, so yet another wrong/double `kfree` in the unload path. Feedback to the agent has been given.
- Consequence: L1 and T2/L5 with `isp_day_night_switch_drop_frame_num=6` could not be tested (the parameter can only be set at `insmod`).
- Incidentally while flashing: An OTA attempt failed with `scp: /sbin/sysupgrade-stage2: No such file or directory`, because `fw_ota.sh` deletes `/overlay/usr` during operation and overlayfs afterwards cannot create files under `/usr` (here `/overlay/usr` existed); after a reboot the OTA worked. Noted for `thingino-firmware/scripts/fw_ota.sh`.

## Test C + S with both new parts

New module (`97cccfa2`) + libimp:

| libimp | Test | Result |
|---|---|---|
| `dfbd90f` | C, 12 cycles | **Deadlock after 6 cycles** (see finding above, `pthread_cancel` in the VBM mutex) |
| `claude/t31-fs-stop-order` | C, 20 cycles + snapshots | **passed**: 48 enables, 0 errors, 69/69 snapshots |
| `claude/t31-fs-stop-order` | S, 30 min, 15 fps | **passed**: main/sub 26669 frames each, 0 decode errors, 175/175 snapshots avg. 0.23 s (HW JPEG), all AVC counters 0, 0 ERR/WRN. [`p2-s/`](p2-s/) |


### Addendum: rmmod fix passed (open-tx-isp `claude/t31-isp-fixes` `6caeb75e`)

Second attempt by the agent (`c7c2e7d2`): The slab damage came from a **double `kfree` of the pad arrays** in `tx_isp_subdev_deinit` (the named fields `outpads`/`inpads` overlap the OEM raw slots +0xcc/+0xd0 with swapped roles; for VIN and CSI the same array was freed twice). In addition: unregistration order (core last, otherwise `__release_resource` unhooks the child regions → "nonexistent resource"), missing `misc_deregister` of the four `/dev/framechanN` (use-after-free), double `cdev_del`. Integration branch `claude/t31-isp-fixes` = rmmod fix + CSI mutex fix, flashed as an image (module md5 `4d5d49785f289b325b2518d88648d62d`).

| Test | Result |
|---|---|
| L1: 3× `rmmod sensor` + `rmmod tx_isp_t31` + `insmod` + `modprobe` | **passed**: all rc=0, no oops (kernel messages captured live via `/proc/kmsg`), afterwards no `isp-m0`/`isp-w02` in `/proc/interrupts` and no ISP regions in `/proc/iomem`, reload ok every time, MemFree 43132 → 43084 KB |
| T2/L5 with `isp_day_night_switch_drop_frame_num=6` (via insmod), 20 switches with running stream | **passed**: every short fetch with data, permanent client continuous (16.5 MB), only isolated `PollingStream idle (miss#1)`, no `encoder dead` |

Files: [`p2/kmsg-L1-fixes.txt`](p2/kmsg-L1-fixes.txt), [`p2/L5-drop6.txt`](p2/L5-drop6.txt). The first two attempts (`97cccfa2` and `claude/t31-isp-csi-mutex` with the first rmmod fix) are above.

Addendum OTA: The error in `fw_ota.sh` does not occur on every flash. On the last flash the first attempt worked despite an existing `/overlay/usr`. `/overlay/usr` comes from the data partition of the image (sound files under `/usr/share/sounds`). A fix is in the thingino repo on the local branch `claude/fw-ota-overlay-fix` (helper to `/tmp` instead of `/sbin`, no deleting in the overlay), still untested.

## Final run: all OpenIMP corrections, new module, 25 fps – passed

libimp `claude/t31-all-fixes` (`4653f0a`, md5 `b2ab4fee6923f8da59ae87cbd739b9bc`) = HW JPEG default + FrameSource stop + IVS without client + audio NOBLOCK; module `97cccfa2`+patches; configuration copy with `video0.fps = video1.fps = sensor.fps = 25` (`/etc` unchanged, deliberately 15 fps there). 60 min, main+sub live, snapshot every 10 s. Files: [`final-25fps/`](final-25fps/).

| | Result |
|---|---|
| Frames main / sub | 88800 / 88824 (≈ 25 fps), no 10 s interval below 233, 0 connection drops |
| Decode errors | 0 / 0 |
| Snapshots | 351/351 (HW JPEG), avg. 0.22 s |
| Counters | `completion timeout` 0, `without size writeback` 0, `EINTR` 0, `[ERR]` 0, `[WRN]` 1, stop timeout 0 |
| Start | `audio resume: flushed 6 stale AI frame(s)`, `HWJPEG: ready`, `T31 ISP driver: open tx-isp` |
| Memory | used 33.7 → 37.4–38.0 MB (cache), stable |

## Follow-up tests overnight (new module `claude/t31-isp-fixes`, flashed)

### `claude/t31-avc-au-check` – passed (merged into `claude/t31-all-fixes`)

Not testable on its own (base `dfbd90f`): With the new module, timps hung at the first stop of the sub stream in the known `pthread_cancel` deadlock (fixed in `claude/t31-fs-stop-order`) and had to be ended with `kill -9`. Therefore a new branch **`claude/t31-all-fixes-au`** (`5b8b451` = `claude/t31-all-fixes` + AU check; conflict in `src/t40/openimp_p2_encoder.c` resolved by an agent, both sides kept; libimp md5 `938e213dafc5e09a3d8262cd305b8555`).

Command: `cycle2.sh allau-c /tmp/libimp-allau.so 12`, then three times `ffmpeg -loglevel verbose -rtsp_transport tcp -t 3 -i rtsp://…/ch0 -map 0:v -c copy -bsf:v trace_headers -f null -`.

| | Result |
|---|---|
| 12 connection cycles main/sub | 12/12, 2 JPEGs main each, sub 30 frames/2 s, 0 decode errors, no `allocation failed`/`rmem out of memory`, same PID |
| VCL NALs per AU (3 × 45 AUs) | always exactly 1 |
| `exceeds max`, errors in the trace | 0 |
| `idr_pic_id` | alternates (1, then 0) |
| `malformed AU` in the timps log | 0 (gate discarded nothing) |

Files: [`au-check/`](au-check/). The earlier ffmpeg message `(0+2) exceeds max (1)` did not occur in this run; the diagnosis stays in the code to classify it if it reappears.

### `claude/t31-fs-timestamps` (`5e3b064`) – passed

libimp md5 `d7b29289631300ce35b314a7760e71ce`, 5 min main+sub live, snapshots. Log: `[KernelIF] frame timestamps use CLOCK_MONOTONIC_RAW`. PTS intervals sub (443 frames): median 0.067 s, p99 0.072 s, max 0.100 s, no duplicate or backward-running PTS. Main/sub 4446/4445 frames, 0 decode errors, 27/27 snapshots. Files: [`timestamps/`](timestamps/).

### Night run: `claude/t31-quiet-logs`, 25 fps, 6 h 40 min – passed

libimp `claude/t31-quiet-logs` (`6431d97`, md5 `be8d27034b00dda14b7dc1bcf5f5b465` = `claude/t31-all-fixes` + quieter logs + fast tuning stop), module `claude/t31-isp-fixes`, configuration copy with 25 fps. 00:15–06:56, main+sub decoded live, snapshot every 10 s (until 06:57), `top`/`free` every 5 min. Files: [`night-25fps/`](night-25fps/).

| | Result |
|---|---|
| Frames main / sub | 592053 / 592111 (≈ 24.7 fps), no 10 s interval below 227, 1 connection per stream over 24000 s, 0 decode errors |
| Snapshots | 2350/2350 (HTTP 200, JPEG), avg. 0.22 s; first snapshot 1.5 s, afterwards with live clients connected max 0.34 s; without live client (06:56–06:57, cold start of the FrameSource): 1.7 s |
| timps | same PID the whole time, `[ERR]` 0, `[WRN]` 9 (all `jpeg chn3: PollingStream idle (miss#1)`, see below) |
| Log size | 25 KB in 6.7 h (measured every 10 min; the trimming at > 1 MB never kicked in) |
| Memory (`free` used) | after the first hour 19.2–19.8 MB, no trend; RSS timpsd 4.1–4.7 MB |
| CPU timpsd | median 7.6 % (5-minute samples) |
| Stop afterwards | 0.2 s (`stopped (waited 2x100ms)`) |

Side finding: The last snapshots (06:56–06:57) were taken after the live clients ended. timps switches the FrameSource on for each of them and off again after ~3.6 s; the snapshot then takes 1.7 s instead of 0.2 s, and timps reports `jpeg chn3: PollingStream idle (miss#1)` once at every cold start, because the FrameSource has been running for only 0.5 s. This is cosmetic and concerns timps, not the libimp; timps could suppress `miss#1` directly after switching on.

## Collective branch `claude/t31-integration` – passed

`aedfced` = `claude/t31-all-fixes-au` + `claude/t31-quiet-logs` + `claude/t31-fs-timestamps` + two small corrections (timestamp sequence restarts when the P0 time base jumps back; summary of synthetic timestamps only for new cases). Merges by an agent, without conflicts; libimp md5 `d991d14150e555bd7827e86b14ea8da5`; module `claude/t31-isp-fixes` (flashed).

| Test | Result |
|---|---|
| `cycle2.sh integ-c /tmp/libimp-integ.so 12` | 12/12, 0 decode errors, no `allocation failed`/`rmem out of memory`, same PID |
| `trace_headers` (3 × 3 s main) | exactly 1 VCL NAL per AU, `idr_pic_id` alternating 1/0, 0 errors |
| Timestamps | `[KernelIF] frame timestamps use CLOCK_MONOTONIC_RAW` |
| `phase.sh integ-s 1200 10` (25 fps, main+sub live, snapshots) | main/sub 29597/29609 frames, no 10 s interval below 234, 0 errors; 116/116 snapshots avg. 0.22 s; sub PTS median 0.040 s, max 0.047 s; `[ERR]` 0; log 10 KB; stop 0.9 s |

Files: [`integration/`](integration/). Branch pushed.

## open-tx-isp: DQBUF with O_NONBLOCK and poll (`claude/t31-isp-dqbuf-nonblock`) – passed

Tested with consent via `insmod` from `/tmp` (not flashed), then reboot onto the flashed module. Test program `fc_nonblock_test` (by an agent, linked statically against the libimp objects of the image; drives `/dev/framechan0` directly). Outputs: [`isp-nonblock/fc_nonblock_test.txt`](isp-nonblock/fc_nonblock_test.txt).

- **Finding in the previous module (`6caeb75e`):** DQBUF ignores O_NONBLOCK (blocks until the next frame), and **poll/select always report POLLIN**, even without a frame and after STREAMOFF. Cause: `tx_isp_core.c` had its own `static frame_channel_fops` without `.poll`; the `extern` at the registration of the misc devices bound to this copy, the complete fops in `tx_isp_module.c` was dead code. The libimp worker therefore never really waited in `select`, but in the blocking DQBUF.
- **`85fbd1fd` (only O_NONBLOCK):** (a) EAGAIN in 4 µs, but poll still always "ready" → with timps the FS worker spins (FS(0) tick 2–10 % CPU instead of ~0 %). Cannot be used like this.
- **`5fe6bbb2` (+ poll really wired up):** 6/6 PASS (EAGAIN 3 µs, poll without frame 0, POLLIN after 62 ms, POLLERR after STREAMOFF, EINTR without buffer loss). timps with `claude/t31-integration`, 5 min 25 fps: 0 errors, FS worker 0–0.3 % CPU; `kill -9` 3× without zombie, restart clean.
- **rmmod oops after `kill -9`** (with `5fe6bbb2`): `rmmod tx_isp_t31` → `Unable to handle kernel paging request`, module stuck in "Unloading", reboot needed ([`isp-nonblock/oops.txt`](isp-nonblock/oops.txt)). Cause (agent, via symbol resolution): use-after-free of the sensor module, **already present in `6caeb75e`**. Only `IMP_ISP_DelSensor` unregistered the sensor from the ISP; after `kill -9` of timps this is missing, `rmmod` of the sensor frees its memory, and `tx_isp_exit()` then calls `reset` through the stale pointer. Earlier rmmod tests were clean because DelSensor ran beforehand.
- **Fix `d9b68caa`:** `tx_isp_subdev_deinit()` detaches a still registered sensor from the ISP (list, slot, `ourISPdev->sensor`, saved ops). Test ([`isp-nonblock/rmmod-cycles-d9b68caa.txt`](isp-nonblock/rmmod-cycles-d9b68caa.txt)): 3 cycles insmod → `fc_nonblock_test` (6/6) → timps 40 s → end by `kill -9` / clean stop / `kill -9` → rmmod sensor + tx-isp: all clean, no oops. Afterwards reboot onto the flashed module.
- Residual risk according to the agent: after `kill -9`, ISP/AE threads keep running; a sensor access that coincides exactly with the unloading of the sensor is still possible (very small window). A full solution would be to stop ISP/sensor at the last `close` of `/dev/tx-isp`.
- Branch `claude/t31-isp-dqbuf-nonblock` (`85fbd1fd` → `5fe6bbb2` → `d9b68caa`) pushed. For use in the image, `OPEN_TX_ISP_VERSION` would have to be changed and the image flashed again (not done).

## OSD on by default (`claude/t31-osd-default`) – passed

Until now OpenIMP blended the OSD on T31 only with `OPENIMP_T31_OSD=1` (timps creates the regions, nothing appeared in the picture). `18127d2` (on `claude/t31-integration`) switches the IPU backend on by default, `OPENIMP_T31_OSD=0` switches it off. libimp md5 `4569960ed6d62804dcd77d367c73c50f`. Test via bind mount, 15 fps (`/etc` configuration as a copy): `phase.sh osd-s 300 10` → main/sub 4446/4445 frames, 0 errors, 27/27 snapshots avg. 0.23 s, timpsd 8.5–12.6 % CPU, `[ERR]` 0; OSD (date/time, name, uptime, logo) visible in the main snapshot and in the sub stream (looked at locally, not checked in); `cycle2.sh osd-c … 12` → 12/12, same PID.

## New image flashed (complete OTA, with consent)

thingino `ciao` (build as before) with open-tx-isp `claude/t31-isp-dqbuf-nonblock` `d9b68caa` (module md5 `da108912…`) and OpenIMP `claude/t31-osd-default` `18127d2` (libimp md5 `4569960e…`, identical to the tested build). First only the module (08:10, with flash libimp `3f501a2`: 3 min main+sub 0 errors, FS thread 0.2 % CPU, two restarts via `S95timps` in 1–2 s), then both (08:35). After the flash: timps runs via `S95timps`, log `T31 IPU OSD backend enabled`, main and sub decodable, OSD in the picture, no oops.

## Final state of the camera (2026-10-01, 08:37)

New image (see above): module `d9b68caa`, libimp `18127d2` with OSD, no bind mount, timps via `S95timps` with `/etc/timps.conf` (15 fps), main and sub decodable. `/etc` and overlay unchanged; test files were only in `/tmp`.
