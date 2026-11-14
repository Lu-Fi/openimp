# T31 H.264 stability: test guide (branch `claude/t31-avc-stability`)

> **Status (2026-11-14):** The `claude/t31-*` branches named here were merged into `next` and deleted (CHANGELOG.md line 9); read them as history. Hardware JPEG is now the default, `OPENIMP_T31_HW_JPEG=0` selects software JPEG (`src/t40/openimp_p2_encoder.c:681`, `src/t40/codec-t40.c:11262`); the companion stage is always off while hardware JPEG is on (`src/t40/codec-t40.c:11794-11809`).

For a local Claude Code session with SSH access to a **T31 test camera** with the open
stack (open-tx-isp + OpenIMP). The rules from
[`T31_TEST_PLAN.md`](T31_TEST_PLAN.md), section 0, apply:
- one test device only
- change nothing permanently, write files only to `/tmp`, `libimp.so` only via bind mount
- push only to new branches you created yourself
- tell the human before the streamer is stopped

Background: [`T31_AVC_STABILITY_RE.md`](T31_AVC_STABILITY_RE.md) (findings F1–F14; the
line numbers there refer to `claude/t31-re`).

## What the branch changes

| Finding | Change | Visible in the log |
|---|---|---|
| F1 | After 2 s without completion: core reset, discard open commands, next picture is IDR. A lost IRQ of a finished frame is detected via the writeback from 0.5 s on. P2 holds the core lock only for a few seconds instead of up to an hour. | `AVC: completion timeout, core reset`, `AVC: completion found without IRQ` |
| F2 | An IRQ without size writeback is ignored instead of discarding the frame | `AVC: IRQ … without size writeback … ignored` |
| F3 | One IRQ waiter per device with a user counter; no 100 % CPU spin on `EINTR` | possibly `keeps returning EINTR` |
| F4 | Destroying one channel no longer disturbs the other | `AVC: … other session(s) active, core left initialized` |
| F5 | A discarded picture forces an IDR; if a picture does not fit into the stream buffer, the QP rises | `AVC: picture larger than the stream buffer` |
| F6 | Software JPEG no longer blocks H.264 | – |
| F7 | Core init only once per device | – |
| F8 | `RequestIDR` is no longer lost | – |

The switch `OPENIMP_T31_AVC_LEGACY=1` restores the old behaviour of F1 and F2 and makes the
core shut down again every time a channel is destroyed. F3 and F5–F8 and the discarding of IRQs
without an owner stay active. So the A/B comparison mainly covers F1/F2.

Another message: `AVC: size written back but core still running` means a size was already in
memory, but the core was still running. The code then waits for its own IRQ. Single hits are
harmless; many hits suggest that the hardware writes the size before the frame end.

The messages in the table always appear (stderr and syslog, so also in `logread`).
`OPENIMP_DEBUG_TRACE=1` adds very many detail lines per frame; turn it on only for
troubleshooting, not for the long run.

## Build and mount

As in the test plan, step B, but with this branch:

```sh
cd openimp && git fetch origin && git switch claude/t31-avc-stability
TC=<thingino>/output/<branch>/<target>/host/bin/mipsel-linux
THINGINO_DIR=<thingino> TOOLCHAIN_PREFIX=$TC T31_OUTPUT_DIR=$PWD/build/t31 sh build-t31.sh
scp build/t31/libimp.so root@<camera>:/tmp/libimp-stab.so
# on the camera:
mount --bind /tmp/libimp-stab.so /usr/lib/libimp.so
```

Start the streamer by hand for the tests (command line from the init script) and redirect the
output to `/tmp/stab-<run>.log`.

## Tests

### S1. Long run main + sub (required)

1. Start the streamer with main and sub stream, fetch both via RTSP, plus a snapshot every 10 s.
2. Let it run for at least 2 hours, better overnight (low frame rate, night picture).
3. On the PC, also check whether the stream has errors:
   `ffmpeg -v error -rtsp_transport tcp -i rtsp://<camera>/<path> -f null - 2>&1 | tee ffmpeg-main.txt`
4. At the end, count:

   ```sh
   L=/tmp/stab-s1.log
   grep -c 'completion timeout' $L
   grep -c 'without size writeback' $L
   grep -c 'completion found without IRQ' $L
   grep -c 'keeps returning EINTR' $L
   ```

- **Success:** both streams run through, `ffmpeg` reports at most isolated errors, all
  counters are 0 or small, and every `completion timeout` is followed by an IDR after at most
  about 2 s (the stream visibly keeps running).
- A comparison run with the previous library (`claude/t31-perf` or original OpenIMP)
  under the same conditions is very helpful.

### S2. Snapshot load (F6)

1. Run only RTSP main and measure the frame intervals:
   `ffprobe -rtsp_transport tcp -show_frames -select_streams v -of csv=p=0 -show_entries frame=pts_time -read_intervals %+30 rtsp://… > s2-without.csv`
2. The same, while snapshots are fetched in a loop in parallel
   (`while true; do curl -s -o /dev/null http://<camera>/image.jpg; done`; path depends on the
   streamer).
3. Both runs with this library and with the old one.

- **Success:** With this library the frame intervals under snapshot load show no
  clearly larger gaps than without.

### S3. Re-creating a channel (F3, F4)

Only if the streamer can reconfigure a single stream at run time (e.g. change the
resolution or bitrate of the sub stream so that the encoder channel is re-created).

1. Fetch main via RTSP.
2. Reconfigure the sub stream 10 times at 30 s intervals.
3. Meanwhile record `top -b -d 5 -n 60 | grep -E '<streamer>'`.

- **Success:** Main keeps running without gaps, no thread at 100 % CPU, the sub stream
  comes back every time.
- No `keeps returning EINTR` in the log.

### S4. A/B with `OPENIMP_T31_AVC_LEGACY=1` (optional)

Repeat S1 for 1 hour with `OPENIMP_T31_AVC_LEGACY=1`. If hangs or black streams occur there
that do not occur without the switch, that proves F1/F2.

## Known open points

- Two IDRs directly after each other both carry `idr_pic_id = 0`. This violates the
  H.264 standard; most decoders do not mind. F1/F5/F8 make it happen more often. If
  `ffmpeg -v error` reports errors exactly at such places, please note them.

## Feedback

- Per test: status, duration, the counter values, `ffmpeg` error count, anything unusual.
- For a hang: the 50 log lines before it, `dmesg | tail -50`, a `top` excerpt.
- Add the report to `T31_TEST_RESULTS.md` (section "S"), see the test plan.

| Observation | Meaning |
|---|---|
| Many `without size writeback` | Stale IRQs (harmless) or the writeback arrives late. Compare timing with `completion found without IRQ`. |
| `completion timeout` followed by more timeouts | Recovery does not work, the core stays stuck. Send the log lines before it. |
| `keeps returning EINTR` | Waiter on an unblocked fd, F3 does not take full effect |
| Stream freezes but no timeout in the log | Hang outside the AVC path (FrameSource/ISP): send `dmesg` and `logread` |
