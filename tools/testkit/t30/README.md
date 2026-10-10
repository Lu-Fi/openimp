# OpenIMP T30 test kit

Help wanted: we have no T30 camera. This kit lets you run OpenIMP on a T30/T30X
camera that runs Thingino with the **vendor stack** (vendor `libimp.so`, vendor
`tx-isp-t30` kernel driver) and send us the results. It takes about 10 minutes,
needs no flashing and leaves the camera as it was.

## What it does

`run.sh` runs on the camera (busybox `sh`) and:

1. Detects SoC, sensor name, I2C address, sensor size and the vendor libimp.
2. Stops the running streamer (prudynt, timps or raptor) through its init
   script and remembers it. It **always restarts it at the end** (shell trap,
   also on Ctrl-C or errors).
3. Runs `apitest_t30` (calls every non-image vendor IMP API function and prints
   PASS/FAIL/N/A per function) twice:
   - baseline with the **vendor** `libimp.so`,
   - with the **OpenIMP** `libimp.so`, loaded via `LD_LIBRARY_PATH` from the kit
     directory.
4. Encodes 8 seconds of H.264 (sensor size, 15 fps, CBR) through OpenIMP into a
   file in `/tmp`, and records only counts and the stream header.
5. Collects `dmesg` (full, plus a grep for oops/ISP/VPU lines), the stage logs,
   `/proc/jz/isp/isp-m0` before and after, system info, into one
   `/tmp/t30-testkit-result-<date>.tar.gz`.
6. Every stage has a timeout watchdog (default 600 s). If a stage hangs it says
   so, skips the stages that depend on it, still restarts the streamer and packs
   what it has. If a driver call is stuck in the kernel (process not killable),
   the camera needs a power cycle; the report says so.

## What it does NOT do

- No flashing, no `rmmod`/`insmod`, no write to `/usr/lib` or any system path.
  OpenIMP is only used from the kit directory.
- No change of any configuration file, no settings, no network changes.
- No audio playback (the speaker is never used; the audio and sysutils areas of
  apitest are off by default).
- It does not read, print or pack `/etc/*.conf`, Wi-Fi/wpa files, `shadow`,
  `fw_printenv`, process command lines (only process names are listed), or any
  picture.

## Requirements

- T30 / T30X camera with Thingino, **vendor stack** (the normal Thingino setup;
  not an OpenIMP/open-tx-isp setup).
- About 5 MB free in `/tmp` (RAM). A scene that is lit and static gives the most
  useful results. Do not point the camera at people.

## Copy and run

On your PC, unpack the kit and copy it to the camera (replace the address):

```sh
tar xzf openimp-t30-testkit-*.tar.gz
scp -O -r t30kit root@CAMERA_IP:/tmp/
```

If `scp` is not available, serve the tarball from your PC
(`python3 -m http.server 8000` in the directory with the tarball) and on the
camera:

```sh
cd /tmp && wget http://PC_IP:8000/openimp-t30-testkit-XXXX.tar.gz
tar xzf openimp-t30-testkit-*.tar.gz
```

Then on the camera:

```sh
cd /tmp/t30kit
sha256sum -c SHA256SUMS      # optional integrity check
sh run.sh
```

If detection fails it tells you; then pass the values, for example
`SENSOR=sc4236 I2C=0x30 W=1920 H=1080 sh run.sh`.

At the end it prints the archive name and its sha256, for example
`/tmp/t30-testkit-result-20261011-120000.tar.gz`.

## Review and send

Look at the archive before you send it:

```sh
tar tzf /tmp/t30-testkit-result-*.tar.gz
tar xzOf /tmp/t30-testkit-result-*.tar.gz result/summary.txt
```

It contains only text files: stage logs, dmesg (MAC and IPv4 addresses are
replaced by `xx:...` / `x.x.x.x`), `/proc/jz` status, versions and module list.
No pictures, no credentials, no configuration files. The encoded H.264 file is
**not** packed (it would show your scene); it is deleted from `/tmp`.

Copy it to your PC (`scp -O root@CAMERA_IP:/tmp/t30-testkit-result-*.tar.gz .`)
and send it to us: attach it to a comment on the T30 issue/PR in
`Lu-Fi/openimp`, or send it to the maintainer by the channel you already use.
Please add: camera model, sensor, Thingino build/version, and whether the
streamer came back by itself.

Afterwards remove the kit: `rm -rf /tmp/t30kit /tmp/t30-testkit-result-* /tmp/t30kit-run-*`
(everything is in `/tmp`; a reboot also clears it).

## Troubleshooting

- Streamer did not come back: run the init script it names, for example
  `/etc/init.d/S95prudynt start`, or reboot.
- Camera frozen during a stage: power-cycle it; send the archive if it was
  created, otherwise tell us the last line `run.sh` printed.
- `Exec format error` or a missing library: the kit is built for MIPS32r2
  uclibc (Thingino T30); other libcs are not supported.

## For maintainers

`build-dist.sh` assembles the tarball from a Thingino uclibc toolchain, the T30
SDK 1.0.5 headers and an `apitest_t30` built from `tools/apitest`
(branch `claude/imgfx-tool`, `./build.sh T30`). Binaries are never committed.
