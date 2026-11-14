# Test plan for the branch `claude/t31-re` (for a local Claude Code session)

> **Status (2026-11-14):** The `claude/t31-*` branches named here were merged into `next` and deleted (CHANGELOG.md line 9); read them as history. OSD on T31 is now drawn by the IPU backend, on by default, `OPENIMP_T31_OSD=0` turns it off (`src/t31/openimp_t31_services.c:122,146`). Hardware JPEG is now the default, `OPENIMP_T31_HW_JPEG=0` selects software JPEG (`src/t40/openimp_p2_encoder.c:681`, `src/t40/codec-t40.c:11262`); the companion stage is always off while hardware JPEG is on (`src/t40/codec-t40.c:11794-11809`). Statements below that say "OSD invisible" or "HW JPEG opt-in" are historical.

This document is the **entry point**. It says what is tested, in which order,
on which camera, and what is reported back. The individual steps are in the linked
guides. Everything here has so far only been built and statically checked; **nothing has run
on a camera**. This test is meant to make up for exactly that.

You work on a machine in the same network as the test camera and reach it over SSH.

**Short path:** The branch `claude/t31-all` contains everything from `claude/t31-re`, `claude/t31-perf`,
`claude/t31-avc-stability` and `claude/t31-ivs-move`. For B and all tests from C on, build this one
library; wherever another branch is named below, `claude/t31-all` applies. The new
functions can be switched by environment variable (see the respective test). If an error
shows up, repeat the test with the library of the single branch named there to narrow it down.

---

## 0. Rules (binding)

1. **One test device only.** The AVPU or IPU can hang; then only a reboot helps.
2. **Change nothing permanently.**
   - Do not flash, change nothing under `/etc` or in the overlay.
   - New files only to `/tmp`.
   - Mount the new `libimp.so` only with a bind mount; a reboot restores everything.
3. **Git:** In the fork `Lu-Fi/openimp`, write only to branches you create **new** yourself.
   Never push to `main` or to an existing branch (such as `claude/t31-re`, `claude/t31-all`),
   never force-push.
4. **Tell the human briefly before every stop of the streamer.** Ask if unsure.
5. **Report everything,** including failures, with the exact command before it.
6. **Camera pictures** (snapshots, test outputs containing a camera picture) may be passed on or
   committed only with the human's consent. The test pictures of the tools are not sensitive.

---

## 1. What is on the branch and which test covers it

| Change | File(s) | Test |
|---|---|---|
| Test tool hardware JPEG core | `tools/t31_hwjpeg_probe.c` | A1 |
| Test tool IPU OSD + analysis | `tools/t31_ipu_osd_probe.c`, `tools/ipu_osd_analyze.py` | A2 |
| OSD structures in the vendor layout (32 instead of 72 bytes), fixes a stack overwrite in `IMP_OSD_GetRgnAttr` | `src/t31/openimp_t31_osd_abi.h`, `src/t31/openimp_t31_services.c` | C |
| Switch `OPENIMP_T31_COMPANION_STAGE=0` (turn off the unnecessary JPEG core start per H.264 frame) | `src/t40/codec-t40.c` | D |
| Hardware JPEG in OpenIMP, default, opt-out `OPENIMP_T31_HW_JPEG=0` | `src/t40/codec-t40.c`, `src/hw_encoder.c` | E |
| `IMP_ISP_Tuning_SetIntegrationTime` for T20/T21/T30 | `src/isp/isp_tseries.c` | F (optional, T20 camera) |
| Tracer for the original libimp | `tools/oem_trace/` | Appendix (optional) |
| Only on `claude/t31-perf`: faster software JPEG (bit-identical output) | `src/hw_encoder.c` | H |
| Only on `claude/t31-perf`: hardware JPEG reads the frame copy directly from rmem (one 3 MB copy less) | `src/t40/openimp_p2_encoder.c`, `src/t40/codec-t40.c` | H |
| Only on `claude/t31-perf`: OSD drawing with the IPU, switch `OPENIMP_T31_OSD=1` | `src/t31/openimp_t31_services.c`, `src/t40/openimp_p2_encoder.c` | G |
| Only on `claude/t31-avc-stability` (contains `claude/t31-perf`): H.264 path recovers instead of freezing, switch `OPENIMP_T31_AVC_LEGACY=1` for A/B | `src/t40/codec-t40.c`, `src/t40/openimp_p2_encoder.c` | S |
| Only on `claude/t31-ivs-move`: IVS motion detection (move, base move) instead of stubs, comparison tool | `src/t31/openimp_t31_ivs*.{c,h}`, `src/kernel_interface.c`, `tools/t31_ivs_compare.c` | I |

---

## 2. Required hardware

- **Required:** a **T31** test camera with thingino `aperto` and the open stack
  (open-tx-isp + OpenIMP). Check:
  `strings /usr/lib/libimp.so | grep -i -m1 openimp` finds something.
- **For A1/A2** any T31 with `/dev/avpu` and `/dev/ipu` is enough, also with the original stack.
- **Optional:** a T20 camera with OpenIMP (test F); a T31 with the original stack (appendix).
- **Toolchain:** The thingino toolchain for the camera profile
  (`<thingino>/output/<branch>/<target>/host/bin/mipsel-linux-`) is needed for `libimp.so` (test B).
  The test tools (A1, A2) are static and also work with `gcc-mipsel-linux-gnu`.
  If there is no thingino build yet, ask the human.

---

## 3. Order

From low risk to high risk. Note the results after every step. At an
**abort criterion**, stop and report instead of carrying on.

### A1. Hardware JPEG test tool (without a library change)

Guide: [`T31_HW_JPEG_TEST.md`](T31_HW_JPEG_TEST.md), section 2 including the
extra runs (`-s`, `-c`).

- **Success:** `SOI present`, `EOI present`, the test picture is correct.
- **Abort criterion for E:** A1 does not deliver a valid JPEG. Then skip E, but do D anyway.

### A2. IPU OSD test tool (without a library change)

Guide: [`T31_OSD_IPU_TEST.md`](T31_OSD_IPU_TEST.md), including `-n 200`.

- **Success:**
  - All four layers appear in the right place, nothing changes outside.
  - Colours and alpha are right.
  - The three times are measured.
- These results steer the OSD implementation. There is no abort criterion for the
  other tests here.

### B. Build and mount `libimp.so` (`claude/t31-all`)

Guide: [`T31_HW_JPEG_TEST.md`](T31_HW_JPEG_TEST.md), section 3.

```sh
cd openimp && git switch claude/t31-all      # or the single branch of the test
TC=<thingino>/output/<branch>/<target>/host/bin/mipsel-linux
THINGINO_DIR=<thingino> TOOLCHAIN_PREFIX=$TC T31_OUTPUT_DIR=$PWD/build/t31 sh build-t31.sh
scp build/t31/libimp.so root@<camera>:/tmp/libimp-t31re.so
# on the camera:
cp /usr/lib/libimp.so /tmp/libimp-orig.so
mount --bind /tmp/libimp-t31re.so /usr/lib/libimp.so
```

For tests C–E you start the streamer by hand, with the variables of the respective test
(command line from the init script). See [`T31_HW_JPEG_TEST.md`](T31_HW_JPEG_TEST.md),
section 4.

### C. Regression with the new library, without a switch

- Start the streamer without additional variables and let it run for 10 minutes:
  - open an RTSP stream
  - fetch a few snapshots
  - leave the OSD switched on in the streamer configuration
- Without variables, hardware JPEG runs, and thus without the companion stage H.264 also runs
  differently from older libraries. If something deviates, repeat the run with `OPENIMP_T31_HW_JPEG=0`
  (software JPEG, companion stage on) to narrow it down.
- Comparison with the original OpenIMP (`/tmp/libimp-orig.so`), same procedure:
  - Does the streamer start?
  - Do H.264 and JPEG run?
  - Does anything crash? (`dmesg`, `logread`)
  - Does the streamer report OSD errors?
- **Expectation:**
  - Behaviour as before.
  - The OSD is still **invisible**. This is known; drawing comes only after A2.
  - The streamer's OSD calls run without errors, and nothing crashes.
- **Abort criterion:** Crash or the stream breaks although it runs with the original OpenIMP.
  Then do not carry on, but report (with `logread`/`dmesg`).
- **FrameSource cycles (rmem leak, fixed on `claude/rmem-allocator`, included in `claude/t31-all`):**
  Let the channel be switched off and on at least 20 times. With timps this works by
  the last RTSP client disconnecting and connecting again; a day/night switch also works.
  Afterwards the stream must still run, and the log must not contain
  `rmem out of memory`. For comparison: before, `EnableChn` failed after 5–7 cycles.
  `OPENIMP_RMEM_NO_REUSE=1` reproduces the old behaviour.
  Watch for picture errors directly after a cycle. They would mean that hardware is still
  writing into an already released buffer.

### D. Companion stage A/B

Guide: [`T31_HW_JPEG_TEST.md`](T31_HW_JPEG_TEST.md), section 4a. Hardware JPEG (default)
turns the companion stage off, so run both with `OPENIMP_T31_HW_JPEG=0`: run A with only that,
run B additionally with `OPENIMP_T31_COMPANION_STAGE=0`; record 5 minutes of RTSP each and compare.

### E. Hardware JPEG in the streamer

Guide: [`T31_HW_JPEG_TEST.md`](T31_HW_JPEG_TEST.md), section 4b. Run C with
`OPENIMP_T31_HW_JPEG=0` (software), run D without the variable (hardware JPEG is the default). CPU load
via `top`, `HWJPEG:` lines in the log, picture quality.

### F. Optional: `SetIntegrationTime` on T20

Only with a T20 camera with OpenIMP and timps as the streamer.

1. Build `libimp.so` for T20:
   `THINGINO_DIR=<thingino> TOOLCHAIN_PREFIX=$TC T20_OUTPUT_DIR=$PWD/build/t20 sh build-t20.sh`
2. Mount it as in B.
3. First check whether timps starts at all against the previous OpenIMP. So far it lacked
   exactly this function there, so the start should fail.
4. Start timps with the new library and set `image.ae_it_max_us` (e.g. `10000`, via
   `POST /control` or in `/etc/timps.conf` in a copy under `/tmp`).
5. Connect an RTSP client. The cap only takes effect while frames are flowing.
6. Look in the timps log for the message `image.ae_it_max_us=… -> capped AE at … lines; GetExpr now reports
   max=…`.

- **Success:** The reported maximum matches the request, no `SDK rejected the cap`.

### G. OSD in the streamer (branch `claude/t31-perf`)

Only after A2 and C. Guide: [`T31_OSD_IPU_TEST.md`](T31_OSD_IPU_TEST.md), stage 2.
For G and H, build the library from `claude/t31-perf` instead of `claude/t31-re` (otherwise as B).

- **Success:** OSD visible in RTSP and snapshot, no `backend disabled`, CPU load not higher
  than without the variable (goal: lower than with the original libimp, if a comparison is possible).

### H. JPEG optimisations (branch `claude/t31-perf`)

Like E, but with the library from `claude/t31-perf`:

1. With `OPENIMP_T31_HW_JPEG=0`: fetch snapshots, compare the CPU load for the MJPEG fetch with E (run C).
   The pictures must look identical to those with `claude/t31-re`.
2. Without variable (hardware JPEG, default): snapshots/MJPEG, `HWJPEG:` lines, compare CPU load with E
   (run D).

- **Success:** same pictures, same or lower CPU load, no new errors in the log.

### I. IVS motion detection (branch `claude/t31-ivs-move`)

Only after C. Guide: [`T31_IVS_MOVE.md`](T31_IVS_MOVE.md), section 6. Build the library from
`claude/t31-ivs-move` (otherwise as B), plus the tool `t31_ivs_compare`.

1. Stage 1: `t31_ivs_compare` against the stock libimp, once with SIMD, once with
   `/tmp/closesimd`. Runs next to the streamer, without a camera picture.
2. Stage 2: timps with `-c /tmp/timps-ivs.conf`, `motion.enabled = 1` and
   `OPENIMP_T31_IVS_STATS=1`. Quiet scene, walk test (only with consent), sensitivity
   live, load and frame rate with and without motion detection.

- **Success:**
  - Stage 1: `differ=0` for move and base in both runs (`oob-differ` is allowed).
  - Stage 2: hits on motion, none in a quiet scene, no `stalled`, frame rate of the
    main stream unchanged.
- **Abort criterion:** Crash or frozen stream with the new library. Then report.

### S. H.264 stability (branch `claude/t31-avc-stability`)

Guide: [`T31_AVC_STABILITY_TEST.md`](T31_AVC_STABILITY_TEST.md). The branch also contains
everything from `claude/t31-perf`, so G and H can run with the same library.

- **Success:** Long run main + sub without freezing; after a hang the stream keeps running with an IDR after
  at most about 2 s.

---

## 4. Result report

One file `T31_TEST_RESULTS.md` with one section per test (A1, A2, C, D, E, F, G, H, I, S):

- **Status:** passed / failed / skipped, with reason.
- **The outputs required in each individual guide:**
  - tool outputs
  - `ipu_osd_analyze.py` output
  - log lines `HWJPEG:` / `companion stage` / `[IVS]`
  - `t31_ivs_compare` outputs
  - `top` values
  - frame counts
- **A header up front with device details:**
  - model, SoC variant (`/proc/cpuinfo`)
  - thingino revision, kernel
  - streamer and version
  - commit of `claude/t31-re` (`git rev-parse --short HEAD`)

Push the report and the small files, i.e. tool outputs, PNGs from A2, `ep1.bin` and logs,
to a **new branch of your own**, `claude/t31-test-results-<date>`, in the fork, under
`test-results/`. No videos, no camera pictures without consent. Or hand everything directly to the
human.

At the end, release the bind mount (`umount /usr/lib/libimp.so`) or reboot the camera and
check that the streamer runs normally.

---

## Appendix: traces of the original libimp (optional)

If a T31 with the **original** stack is also available, a capture of the original library
answers many open questions directly: JPEG header, table buffer,
IPU parameters. Guide: [`RE_TRACE_GUIDE.md`](RE_TRACE_GUIDE.md). This is not part of the
required test.
