# T31 OSD via the IPU: test guide (stage 1, test tool)

> **Status (2026-11-14):** OSD on T31 is now drawn by the IPU backend, on by default, `OPENIMP_T31_OSD=0` turns it off (`src/t31/openimp_t31_services.c:122,146`). Stage 2 text about `OPENIMP_T31_OSD=1` as opt-in is historical. The `claude/t31-*` branches named here were merged into `next` and deleted (CHANGELOG.md line 9); read them as history.

For a local Claude Code session with SSH access to a **T31 test camera**. The same rules apply
as in [`T31_HW_JPEG_TEST.md`](T31_HW_JPEG_TEST.md), section 0:
- one test device only
- change nothing permanently, write files only to `/tmp`
- push only to new branches you created yourself
- tell the human before the streamer is stopped

Background: [`T31_OSD_RE.md`](T31_OSD_RE.md). In short:
- The original `libimp` blends text and logos (BGRA bitmaps) and cover areas directly into the
  NV12 picture with the IPU block (`/dev/ipu`).
- OpenIMP has drawn nothing at all on T31 so far.
- This tool checks the reconstructed IPU control and measures the cost. The results are used to
  build the OSD backend in OpenIMP.

## Build

```sh
git clone https://github.com/Lu-Fi/openimp && cd openimp
git switch claude/t31-re
mipsel-linux-gnu-gcc -O2 -march=mips32r2 -static -Wall -Wextra \
    -o t31_ipu_osd_probe tools/t31_ipu_osd_probe.c        # or the thingino toolchain
scp t31_ipu_osd_probe root@<camera>:/tmp/
```

## Run

The tool gets DMA memory through `/dev/avpu`, so the streamer must be stopped:

```sh
ls -l /dev/ipu                                   # must exist (CONFIG_JZ_IPU)
/etc/init.d/<S95streamer> stop; sleep 1
/tmp/t31_ipu_osd_probe -v -n 200 2>&1 | tee /tmp/ipu-probe.txt
dmesg | tail -30 > /tmp/ipu-dmesg.txt
/etc/init.d/<S95streamer> start
```

The tool blends four layers into a 640×368 test picture (grey-scale gradient):

| Channel | Content | Position |
|---|---|---|
| 0 | BGRA bitmap: 5 colour bands, alpha ramp 0…255 from left to right | (32, 32) |
| 1 | same bitmap, pixel alpha × global alpha 128 | (32, 128) |
| 2 | cover area red, opaque | (224, 32) |
| 3 | cover area blue, alpha 128 | (224, 128) |

With `-n 200` it also measures:
- one IPU run with 4 layers
- 4 runs with 1 layer each
- the 1 MB cache flush that the original `libimp` does per region and frame

If the combined run fails, test each layer on its own: `-l 1`, `-l 2`, `-l 4`, `-l 8`
(each with `-d /tmp/l<n>` into its own directory).

## Analysis (on the PC)

```sh
scp 'root@<camera>:/tmp/ipu-*' ./ipu/
python3 tools/ipu_osd_analyze.py ./ipu            # needs numpy, Pillow for PNGs
```

The script shows:
- where the picture changed (each layer in the right place, nothing outside)
- which colour conversion and alpha rounding best explain the measured values
- the colour values of the cover areas

It also creates `ipu-before.png` and `ipu-after.png`. Please look at them:
- Are the colours in band order red, green, blue, white, black?
- Does the alpha fade in from left to right?
- Is channel 1 half as opaque as channel 0?
- Is red opaque and blue semi-transparent?

## Feedback

- `ipu-probe.txt` and `ipu-dmesg.txt`
- the output of `ipu_osd_analyze.py`
- the two PNGs and your assessment (what looks wrong?)
- the three times from `-n` (µs per run or flush)

| Observation | Meaning |
|---|---|
| `IOCTL_IPU_START` error, `dmesg` "ipu: error …" | A parameter word is wrong. Find the layer with `-l`. |
| Bitmap colours swapped (red ↔ blue) | Byte order in the `para` word, bits 14–17 |
| Bitmap opaque instead of alpha ramp | Alpha mode, bits 1–2 |
| Cover area wrong colour / too dark | Colour word (Y without +16?) or mask bit 23 |
| Picture changed outside the boxes | Position or stride problem |

---

# Stage 2: OSD backend in OpenIMP (branch `claude/t31-perf`)

Do this only when stage 1 (test tool) has produced plausible pictures. On `claude/t31-perf`
OpenIMP draws the OSD regions with the IPU as soon as `OPENIMP_T31_OSD=1` is set. Without the
variable nothing changes. Since `claude/t31-osd-default` the backend is on by default;
`OPENIMP_T31_OSD=0` turns it off.

What the backend does differently from the original `libimp`:
- up to 4 regions per IPU run (original: one run per region), sorted by `layer`
- bitmaps are copied to rmem only when they change; there is no 1 MB cache flush per frame any more
- after 10 IPU errors in a row it turns itself off (log line `T31 IPU OSD backend disabled`),
  the stream keeps running

## Procedure

1. Build `libimp.so` from `claude/t31-perf` and mount it with a bind mount as in the test plan,
   step B (`git switch claude/t31-perf` instead of `claude/t31-re`).
2. Leave the OSD switched on in the streamer (time, possibly logo and a privacy zone).
3. Start the streamer by hand, once without and once with the variable:

   ```sh
   /etc/init.d/<S95streamer> stop; sleep 1
   OPENIMP_T31_OSD=1 <streamer-command-line> > /tmp/osd-run.log 2>&1 &
   ```

4. Open an RTSP stream and let it run for 5 minutes. During that time:
   - `top -b -n 5 -d 2 | grep -E '<streamer>|CPU:'` once without, once with the variable
   - `grep -E 'OSD' /tmp/osd-run.log`
   - `dmesg | grep -i ipu | tail`
5. Look at the picture (only the human, or with their consent): Is the time in the right
   place, does it count up every second, are colours and transparency right, is the
   privacy zone opaque?
6. If the streamer offers snapshots/MJPEG: fetch a snapshot. The OSD must be visible there too
   (JPEG reads the picture after blending).

## Feedback

| Observation | Meaning |
|---|---|
| `backend requested`, no `disabled`, OSD visible | Success. Note CPU load with/without. |
| `disabled: cannot open /dev/ipu` | Kernel without IPU driver (`ls -l /dev/ipu`) |
| `disabled: repeated IPU errors` | Parameter word wrong, send `dmesg` |
| OSD visible in RTSP, not in snapshot | Cache invalidation after blending is not enough |
| Text flickers or shows old time | Check the bitmap update (double buffer / write-back) |
| Colours swapped, wrong opacity | as in the table of stage 1 |
| Stream stutters or FPS drops | IPU time per frame too high, compare with `-n 200` from stage 1 |
