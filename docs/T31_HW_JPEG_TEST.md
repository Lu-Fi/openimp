# T31 hardware JPEG: test guide for a local Claude Code session

> **Status (2026-11-14):** Hardware JPEG is now the default, `OPENIMP_T31_HW_JPEG=0` selects software JPEG (`src/t40/openimp_p2_encoder.c:681`, `src/t40/codec-t40.c:11262`); the companion stage is always off while hardware JPEG is on (`src/t40/codec-t40.c:11794-11809`). Sections that treat HW JPEG as opt-in (`OPENIMP_T31_HW_JPEG=1`) are historical. The `claude/t31-*` branches named here were merged into `next` and deleted (CHANGELOG.md line 9); read them as history.

This guide is meant for a Claude Code session that runs on a machine in the
same network as a **T31 test camera** and can reach it over SSH. It
assumes no knowledge from earlier sessions.

Background and the register map are in [`T31_HW_JPEG_RE.md`](T31_HW_JPEG_RE.md); open points
and a code review are in [`T31_HW_JPEG_RE_FOLLOWUP.md`](T31_HW_JPEG_RE_FOLLOWUP.md). The
short version:

- OpenIMP's software encoder costs roughly 0.3 s of CPU per 1080p picture and blocks
  H.264 submits during that time.
- The original `libimp` uses a separate hardware core in the AVPU (core 1) for this. It
  needs ~8 ms and practically does not load the CPU.
- OpenIMP rebuilds the hardware path. It was checked on a T31X (valid pictures, timpsd
  18.5 % instead of 66.9 % CPU) and has been the **default** since. `OPENIMP_T31_HW_JPEG=0` switches
  back to the software encoder (opt-out). This test checks it on more cameras.

---

## 0. Rules

The rules are binding. If unsure, ask the human.

1. **One test device only.** While poking at registers the AVPU can hang. Then only a
   reboot helps; with a watchdog loop possibly a power cycle.
2. **Change nothing permanently.**
   - No flashing, no writing to `/etc` or to the overlay.
   - New files only to `/tmp`.
   - Mount the new `libimp.so` only with a bind mount; a reboot undoes everything.
3. **Git:** In the repo `Lu-Fi/openimp`, write only to branches you create yourself
   (e.g. `claude/t31-hw-jpeg-results`). Never push to `main`, `claude/t31-re`, `claude/t31-hw-jpeg` or
   other existing branches, never force-push.
4. **Before every step that stops the streamer,** tell the human briefly: The camera
   then delivers no picture.
5. **Report results completely,** including failures. A cleanly documented "does not
   work because …" is just as valuable.

---

## 1. Check the prerequisites

On the local machine:

```sh
git clone https://github.com/Lu-Fi/openimp && cd openimp
git switch claude/t31-re
git log --oneline -15
```

The branch contains (among others):

| Commit | Content |
|---|---|
| `t31: allow disabling the AVPU companion stage …` | Switch `OPENIMP_T31_COMPANION_STAGE=0` |
| `t31: opt-in hardware JPEG …` | Hardware path; now the default, `OPENIMP_T31_HW_JPEG=0` turns it off |
| `t31: log hardware JPEG state …` | `HWJPEG:` messages in syslog |
| `tools: add t31_hwjpeg_probe …` | standalone test tool |

On the camera (over SSH):

```sh
uname -a; cat /etc/os-release 2>/dev/null | head -5
grep -i -m1 'soc\|system type' /proc/cpuinfo         # must be T31
ls -l /dev/avpu                                       # must exist
lsmod | grep -i -E 'avpu|tx.isp'                      # which driver?
ls -l /usr/lib/libimp.so; strings /usr/lib/libimp.so | grep -i -m3 openimp
ls /etc/init.d/ | grep -i -E 'timps|prudynt|raptor|rvd|streamer'
df -h /tmp                                            # room for ~2 MB
```

Note down:

- **Userspace:** Does OpenIMP run (`strings` finds "openimp") or the original `libimp`?
- **Streamer:** which one, and with which init script?
- **Command line of the streamer:** Read the init script (`cat /etc/init.d/S95…`). Stage 3
  starts the streamer by hand with additional environment variables.

The camera must have the open-stack variant (thingino branch `aperto`, open-tx-isp +
OpenIMP). For stage 1, any T31 with `/dev/avpu` is enough.

---

## 2. Stage 1: test tool `t31_hwjpeg_probe` (without streamer)

The tool encodes **one** picture directly through the hardware core and checks the
core assumptions of the reconstruction:

- IRQ slot 4
- register block `0x8400–0x8428`
- table format (EP1)
- whether the hardware writes the JFIF headers itself

### Build

Static, so that it runs independently of the camera's libc. Either with the
thingino toolchain (`<thingino>/output/<branch>/<target>/host/bin/mipsel-linux-gcc`) or
with a Debian/Ubuntu cross compiler (`gcc-mipsel-linux-gnu`):

```sh
mipsel-linux-gnu-gcc -O2 -march=mips32r2 -static -Wall -Wextra \
    -o t31_hwjpeg_probe tools/t31_hwjpeg_probe.c
scp t31_hwjpeg_probe root@<camera>:/tmp/
```

(Older Dropbear versions need `scp -O`.)

### Run

The AVPU driver module serves only **one** client, so the streamer must be stopped:

```sh
/etc/init.d/<S95streamer> stop
sleep 1
/tmp/t31_hwjpeg_probe -v -e /tmp/ep1.bin 2>&1 | tee /tmp/probe-1080p.txt
/tmp/t31_hwjpeg_probe -W 640 -H 360 -o /tmp/hwjpeg-360.jpg 2>&1 | tee /tmp/probe-360p.txt
dmesg | tail -40 > /tmp/probe-dmesg.txt
/etc/init.d/<S95streamer> start
```

Without `-i` the tool encodes a built-in test picture. At the top are horizontal
grey levels, in the middle a 32 px checkerboard, at the bottom vertical grey levels, in the lower
colour area eight colour bars. The output goes to `/tmp/hwjpeg.jpg`.

### Analysis

Fetch the files (`scp root@<camera>:/tmp/{hwjpeg.jpg,hwjpeg-360.jpg,probe-*.txt,ep1.bin} .`)
and check:

```sh
file hwjpeg.jpg
ffprobe -v error -show_entries stream=width,height,pix_fmt hwjpeg.jpg
python3 -c "from PIL import Image; im=Image.open('hwjpeg.jpg'); im.load(); print(im.size, im.mode)"
```

Then look at the picture. Are grey levels, checkerboard and colour bars right?

| Observation | Meaning | Next step |
|---|---|---|
| `SOI present`, `EOI present`, picture correct | Reconstruction is right, the hardware writes the headers | continue with stage 2 |
| `no IRQ slot 4 within 2000 ms` | Core does not start or the IRQ does not arrive | Report the register values from `-v` and `dmesg`; repeat with `-n` (without core-0 init) |
| IRQ arrives, `len` = 0 or implausible | Core runs but writes nothing | Report `0x8430/0x8438`; fetch `/tmp/hwjpeg-stream.raw` |
| `ERROR/overflow bit` | Buffer too small or tables invalid | Repeat with `-q 30`; report the output |
| `no SOI`, but data present | Hardware delivers only entropy data | Report the first 64 bytes of `hwjpeg.jpg` as a hex dump (`xxd hwjpeg.jpg \| head -4`) |
| Picture decodes, but block artefacts or wrong brightness | Quant tables or reciprocals in the wrong byte order | Provide `ep1.bin`; see "EP1 against stock" below |
| Colours swapped (blue/red) | U/V order | Report |
| Camera hangs | AVPU blocks | Reboot; note exactly after which call |

Additional probe runs, once the base test works:

```sh
/tmp/t31_hwjpeg_probe -s 65536 -o /tmp/overflow.jpg      # expected: overflow bit (0x8438 bit 1)
/tmp/t31_hwjpeg_probe -c 0x31 -o /tmp/bit8off.jpg         # bit 8 off: what changes?
/tmp/t31_hwjpeg_probe -c 0x00080131 -o /tmp/dri8.jpg      # restart interval 8 in [31:16]: RST markers in the JPEG?
```

Report the output, the `0x85f8` values and `xxd <file> | head -4` each time. Whether `dri8.jpg`
contains restart markers: `python3 -c "d=open('dri8.jpg','rb').read(); print([hex(d[i+1]) for i in range(len(d)-1) if d[i]==0xff and 0xd0<=d[i+1]<=0xd7][:5], b'\xff\xdd' in d)"`.

With a real camera picture (optional, OpenIMP streamer runs beforehand):

```sh
# have one source picture saved per channel
OPENIMP_T31_DUMP_SOURCE_DIR=/tmp <streamer-command-line> &   # see stage 3
ls -l /tmp/openimp-source-ch*.nv12
# stop the streamer, then:
/tmp/t31_hwjpeg_probe -i /tmp/openimp-source-ch0-1920x1080.nv12 -W 1920 -H 1080 -o /tmp/real.jpg
```

The tool accepts NV12 packed (`W*H*3/2` bytes) or with the luma plane padded to 16 rows.
If the dump file has a different size, it is read as packed. The colours
are then shifted: please report, with the file size.

---

## 3. Stage 2: build `libimp.so` from this branch

`libimp.so` must be linked against the camera's libc (uClibc or musl). This
only works with the thingino toolchain of the matching camera profile. If there is no
thingino build yet, ask the human. After an `aperto` build (`make` for the profile of the
test camera) the toolchain is in `<thingino>/output/<branch>/<target>/host/bin/`.

```sh
cd openimp
TC=<thingino>/output/<branch>/<target>/host/bin/mipsel-linux
THINGINO_DIR=<thingino> TOOLCHAIN_PREFIX=$TC T31_OUTPUT_DIR=$PWD/build/t31 sh build-t31.sh
file build/t31/libimp.so
scp build/t31/libimp.so root@<camera>:/tmp/libimp-hwjpeg.so
```

(`build-t31.sh` calls `$TOOLCHAIN_PREFIX-gcc` and `$TOOLCHAIN_PREFIX-strip`.)

Mount it on the camera, only until the next reboot:

```sh
cp /usr/lib/libimp.so /tmp/libimp-orig.so           # for comparison
mount --bind /tmp/libimp-hwjpeg.so /usr/lib/libimp.so
```

Undo: `umount /usr/lib/libimp.so` or reboot.

---

## 4. Stage 3: tests in the running streamer

Start the streamer **by hand** for every pass, so that the environment variables
reliably arrive. Take the command line from the init script:

```sh
/etc/init.d/<S95streamer> stop
<VAR>=<value> <streamer-command-line> > /tmp/run-<name>.log 2>&1 &
sleep 20
logread 2>/dev/null | grep -E 'HWJPEG|companion stage' | tail -20   # or: grep … /tmp/run-<name>.log
```

Check that the variable is really set:
`tr '\0' '\n' < /proc/$(pidof <streamer>)/environ | grep OPENIMP`

### 4a. Companion stage A/B (H.264 regression)

Background: With `OPENIMP_T31_HW_JPEG=0`, OpenIMP additionally starts the JPEG core after **every** H.264 frame. The
settings for this were copied from a vendor trace in which a
JPEG channel ran in parallel. According to the vendor code this does not belong to H.264. It presumably writes into
an H.264 stream buffer that may still be with the streamer.

Hardware JPEG (default) always turns the companion stage off. For this comparison,
set `OPENIMP_T31_HW_JPEG=0` in both runs:

| Run | Variables |
|---|---|
| A | `OPENIMP_T31_HW_JPEG=0` (companion stage on) |
| B | `OPENIMP_T31_HW_JPEG=0 OPENIMP_T31_COMPANION_STAGE=0` |

Capture 5 minutes of RTSP per run:

```sh
ffmpeg -rtsp_transport tcp -i rtsp://<camera>/<path> -t 300 -c copy run-A.mp4
ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames run-A.mp4
ffmpeg -v error -i run-A.mp4 -f null - 2>&1 | head -20      # decode errors?
```

Also search `logread` or the log for `timeout`, `stall`, `error`, and look at the picture
briefly.

**Expectation:** B is at least as good as A. If B is worse (fewer frames, timeouts,
artefacts), that is an important finding: H.264 then depends on it after all.

### 4b. Hardware JPEG

| Run | Variables |
|---|---|
| C | `OPENIMP_T31_HW_JPEG=0` (software JPEG) |
| D | none (hardware JPEG, default) |

Create load, from another machine or on the camera: one snapshot per second
or keep an MJPEG stream open. Take the endpoint from the streamer's documentation.

```sh
while true; do curl -s -o /dev/null -w '%{http_code} %{size_download} %{time_total}\n' \
    'http://<camera>:<port>/<snapshot-path>'; sleep 1; done
```

Measure:

- **CPU:** `top -b -H -n 6 -d 5 > /tmp/top-<run>.txt` during the load
  (per thread, streamer threads and `idle`).
- **Log D:**
  - `HWJPEG: T31 hardware JPEG enabled (OPENIMP_T31_HW_JPEG=0 selects the software encoder)`
  - `HWJPEG: ready, ep1=…`
  - `HWJPEG: 1920x1080 q75 -> … bytes`
  - or, logged as an error, `HWJPEG: disabled for this process, all JPEG frames now use the software encoder: <reason>`
- **Log C:** `HWJPEG: T31 hardware JPEG off (OPENIMP_T31_HW_JPEG=0), software encoder`
- **Pictures from D:** save a few snapshots and look at them. The picture must be correct.
- **H.264 in D:** check briefly as in 4a that the stream stays clean.

**Expectation:** In D the CPU load of the streamer drops clearly during the snapshots, the
JPEGs are correct, and H.264 stays stable.

**If `disabled: …` appears:**

| Reason | Meaning |
|---|---|
| `repeated completion timeouts` | IRQ 4 did not arrive three times in a row. Provide the preceding lines `HWJPEG: no IRQ 4 within 200 ms (status …, 0x85f8 …)`. |
| `hardware did not emit a JFIF stream` | no `FF D8` at the start or no `FF D9` in the last 64 bytes. See stage 1, header |
| `implausible length` | see stage 1 |
| `DMA allocation failed` / `EP1 … aligned` | Memory. The log line `HWJPEG: ready …` is missing, please report. |

Other messages that do **not** turn it off:

- `HWJPEG: no AVC IRQ waiter yet, software until one runs`: The shared IRQ thread
  starts with the first H.264 channel. Until then the path encodes in software. If
  no `HWJPEG: … bytes` ever appears afterwards, no H.264 is running; please report.
- `HWJPEG: AVC core not initialised yet, software until it is`: The AVPU core is initialised with the
  first H.264 frame (and shut down again after the last H.264 channel).
  Until then the path encodes in software, without counting that as a timeout.
- `HWJPEG: stream overflow …`: This one picture goes to software, and for the next one the
  stream buffer is doubled.
- `HWJPEG: cannot grow the stream buffer to … bytes, keeping …`: The doubling got
  no memory. The old buffer stays, and only pictures that overflow it again go to
  software. This size is not tried again.

**If the pictures from D look broken** (stripes, old picture content, block garbage) but the
probe from stage 1 delivers clean pictures: Repeat pass D with additionally
`OPENIMP_T31_HW_JPEG_SRC_COHERENT=1`. Then the source picture also lies in
coherent instead of cached memory. If it becomes clean with that, the cache flush of the
cached rmem on T31 is the cause. That is an important finding.

Evaluate stage 3a (companion stage) before 3b: Hardware JPEG (default) always turns off
the companion stage as well; otherwise H.264 anomalies cannot be attributed.

---

## 5. Result report

Write a report `T31_HW_JPEG_RESULTS.md`. Either hand it to the human or
push it to a **new branch of your own**, `claude/t31-hw-jpeg-results`, in the fork (rule 3).

Content:

1. **Device:**
   - camera model, SoC variant (`/proc/cpuinfo`)
   - thingino revision, kernel
   - streamer
   - libimp: OpenIMP commit or original
2. **Stage 1:**
   - complete outputs of `t31_hwjpeg_probe` (1080p and 360p)
   - assessment of the pictures
   - hex dump of the first 64 bytes
   - `dmesg` excerpt
3. **Stage 3a:** frames, errors and anomalies, A against B.
4. **Stage 3b:**
   - CPU values C against D (table)
   - `HWJPEG:` log lines
   - JPEG sizes and picture quality
   - H.264 stability
5. **Anything unexpected,** with the exact time and the command before it.

Attach (or commit along) the files (JPEGs, `ep1.bin`, logs, `top` outputs), no videos.

---

## Appendix: comparing EP1 against stock (if tables are suspect)

If the picture decodes but looks wrong, the error is usually in the table buffer:

- byte order of the quant bytes
- reciprocals
- header words at `0x180`

The safest comparison is an EP1 dump of the original `libimp` while a JPEG channel is running.

1. The address is in register `0x8418` (read with `devmem` or `tools/avpu_peek.c`
   while the original `libimp` produces JPEG).
2. Save the 0x790 bytes from this physical address (`devmem` word by word or
   `tools/t31_phys_dump.c`).
3. Pass it to `t31_hwjpeg_probe` with `-E stock-ep1.bin`. If the picture is correct with that, report the differences to our own generator with
   `cmp -l ep1.bin stock-ep1.bin`.
