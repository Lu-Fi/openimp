# Reverse engineering on the camera: recording traces of the original libimp

> **Status (2026-11-14):** The `claude/t31-*` branches named here were merged into `next` and deleted (CHANGELOG.md line 9); read them as history.

Guide for a **local Claude Code session** that accesses a T31 test camera over SSH.
Goal: record ground truth from the vendor's original `libimp`, so that OpenIMP
reconstructions are backed by evidence instead of guesses.

The branch `claude/t31-re` in the fork `Lu-Fi/openimp` contains all tools:

| Tool | Purpose | Guide |
|---|---|---|
| `tools/oem_trace/oem_trace.c` | `LD_PRELOAD` tracer: records the ioctls of libimp and saves DMA data | this document |
| `tools/t31_hwjpeg_probe.c` | Drive the hardware JPEG core directly | [`T31_HW_JPEG_TEST.md`](T31_HW_JPEG_TEST.md) |
| `tools/t31_ipu_osd_probe.c` + `tools/ipu_osd_analyze.py` | Drive the IPU OSD directly and analyse it | [`T31_OSD_IPU_TEST.md`](T31_OSD_IPU_TEST.md) |
| `tools/ep1_match.py` | Compare an EP1 capture against the OpenIMP generator | below |

Background: [`T31_HW_JPEG_RE.md`](T31_HW_JPEG_RE.md),
[`T31_HW_JPEG_RE_FOLLOWUP.md`](T31_HW_JPEG_RE_FOLLOWUP.md), [`T31_OSD_RE.md`](T31_OSD_RE.md).

---

## 0. Rules

1. **One test device only.** Do not flash anything. Do not change anything under `/etc` or in
   the overlay. Write files only to `/tmp`; a reboot restores everything.
2. **Git:** In `Lu-Fi/openimp`, write only to branches you create yourself (e.g.
   `claude/oem-traces-<date>`). Never push to `main` or to existing branches. Never
   force-push.
3. **Privacy:** The tracer saves JPEG images from the camera. Before recording, ask the
   human what the camera is looking at (neutral scene, or cover the lens). Commit images
   only with their consent.
4. Tell the human before you stop the streamer.
5. Report everything, including failures.

---

## 1. What the traces should answer

| Question | Evidence |
|---|---|
| Does the hardware write the JPEG headers (SOI…EOI) itself? | `jpeg-*.bin` starts with `ff d8` |
| Is our EP1 table buffer byte-identical to the original? | `ep1-*.bin` + `tools/ep1_match.py` |
| Which values does the original write to `0x8400` (bit 8, restart) and `0x8408`, and what is in `0x8430`/`0x85F8`? | `jpeg start:` lines and `avpu R 0x8430` in the log |
| Which `para`, `fmt` and `bak_argb` values does the IPU use for text, logo and cover? | `ipu  chN …` lines |
| In which format are the OSD bitmaps? | `osd-*.bin` |
| How often does the original libimp flush the cache for OSD (cost)? | `ipu FLUSH_CACHE` lines per frame |
| What do the ISP tuning functions send to the kernel? | `isp TUNING` lines |
| Where does OpenIMP differ from the original? | the same trace once with OpenIMP (section 5) |

---

## 2. Prerequisites

- **T31 camera with the proprietary stack:** original `libimp.so` and original kernel modules.
  - This is thingino `master` or `aperto` with `BR2_PACKAGE_THINGINO_ISP_PROPRIETARY=y`.
  - Check: `strings /usr/lib/libimp.so | grep -i -m1 openimp` must be **empty**.
- **A streamer that uses JPEG and OSD,** i.e. prudynt or timps with OSD and
  snapshot/MJPEG.
- **Root and `/dev/mem`:** `ls -l /dev/mem`. Without `/dev/mem` you only get the log, no dumps.
- **The thingino toolchain of the camera profile:** The tracer is a shared library and must
  be built against the camera's libc (uClibc or musl). A glibc cross compiler is **not**
  enough. If there is no thingino build yet, ask the human.

---

## 3. Build

```sh
git clone https://github.com/Lu-Fi/openimp && cd openimp
git switch claude/t31-re
TC=<thingino>/output/<branch>/<target>/host/bin/mipsel-linux
$TC-gcc -O2 -fPIC -shared -Wall -Wextra -o liboem_trace.so tools/oem_trace/oem_trace.c -ldl
file liboem_trace.so                                   # MIPS32, dynamically linked
scp liboem_trace.so root@<camera>:/tmp/

# for the analysis on the PC
gcc -O2 -o probe-host tools/t31_hwjpeg_probe.c         # only for -G (generate EP1)
```

---

## 4. Recording (original libimp)

1. Find the streamer's command line in the init script:

   ```sh
   ls /etc/init.d/ | grep -i -E 'prudynt|timps|raptor|rvd'
   cat /etc/init.d/<S95streamer>
   ```

2. Stop the streamer and start it by hand with the tracer:

   ```sh
   /etc/init.d/<S95streamer> stop; sleep 1
   rm -rf /tmp/oemtrace
   OEM_TRACE_DIR=/tmp/oemtrace OEM_TRACE_DUMPS=4 LD_PRELOAD=/tmp/liboem_trace.so \
       <streamer-command-line> > /tmp/oemtrace-stdout.log 2>&1 &
   ```

3. Let it run for 20–30 seconds. During that time:
   - open an RTSP stream (H.264 runs),
   - fetch 3–5 snapshots or a short MJPEG stream (JPEG jobs),
   - make sure the OSD is visible (time, logo, possibly a privacy zone).

4. Stop and collect:

   ```sh
   kill $(pidof <streamer>); sleep 2
   du -sh /tmp/oemtrace; ls -la /tmp/oemtrace
   tar czf /tmp/oemtrace-stock.tgz -C /tmp oemtrace
   /etc/init.d/<S95streamer> start
   ```

The log grows by a few register lines with every H.264 frame, so a few KB per second.
`/tmp` is in RAM, so do not let it run for minutes.

If `LD_PRELOAD` does not work (no `trace.log`), check:
- Does the streamer link `libimp.so` dynamically? `cat /proc/<pid>/maps | grep libimp`
- Does the init script reset the environment?

---

## 5. Recording with OpenIMP (comparison)

If the human also has a camera or an image with the open stack (OpenIMP), repeat the same
procedure there, with the same scene and the same actions. Result: `oemtrace-openimp.tgz`.
The tracer works unchanged; it just sees a different `libimp.so`.

---

## 6. Analysis (on the PC)

```sh
mkdir stock && tar xzf oemtrace-stock.tgz -C stock
L=stock/oemtrace/trace.log

# JPEG: header, register values, status
xxd stock/oemtrace/jpeg-000.bin | head -4                  # starts with ffd8ffe0?
python3 -c "from PIL import Image; im=Image.open('stock/oemtrace/jpeg-000.bin'); im.load(); print(im.size)"
grep 'jpeg start:' $L | head -3
grep -E 'avpu R 0x(8430|8434|8438|85f8)' $L | head -12
grep -E 'avpu W 0x85f[04]' $L | head -6

# EP1 against our generator
python3 tools/ep1_match.py ./probe-host stock/oemtrace/ep1-*.bin

# IPU/OSD
grep -c 'ipu START' $L                                     # runs in the capture
grep 'ipu  ch' $L | awk '{print $4,$5,$6}' | sort | uniq -c | sort -rn | head
grep -c 'ipu FLUSH_CACHE' $L
ls stock/oemtrace/osd-*.bin

# ISP-Tuning
grep 'isp TUNING' $L | awk '{print $5}' | sort | uniq -c | sort -rn | head -20
```

With an OpenIMP trace (section 5), also compare the JPEG start lines and `ipu` lines
side by side:

```sh
diff <(grep 'jpeg start:' stock/oemtrace/trace.log | head -1 | cut -d' ' -f3-) \
     <(grep 'jpeg start:' openimp/oemtrace/trace.log | head -1 | cut -d' ' -f3-)
```

Addresses differ, of course. Words 0–3 (`cmd …`) and the sizes are the interesting part.

---

## 7. Reporting the result

Write a report `RE_TRACE_RESULTS.md` with:

1. **Device:**
   - model, SoC variant
   - thingino revision
   - streamer and version
   - stack (proprietary/OpenIMP)
2. **The answers to the questions in section 1,** each with the log lines that prove it.
3. **The output of `ep1_match.py`.**
4. **IPU:** the most common channel configurations (`fmt`, `para`, `bak_argb`) and the number
   of flushes per frame.
5. **Anything unusual.**

Push the report and the trace archives (without camera images, unless the human agrees) to a
**new branch of your own**, `claude/oem-traces-<date>`, in the fork, e.g. under `traces/`.
Or hand both directly to the human.
