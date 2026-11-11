#!/usr/bin/env python3
"""Split OUT/streams.bin (qemu-ioctl-trace.sh STREAMS_OUT=1) by the sample's
"T32J <n> <bytes> ..." lines into OUT/stream-<n>.<ext>, check each JPEG with
ffmpeg (decode errors) and print size + mean Y/U/V of 8 vertical bands of the
middle rows (the QEMU colour bars).  Pictures stay local.

usage: t32_streams.py OUT [ext]   (Pillow + ffmpeg)

ext h264 (#293) / h265 (#294): the streams are one H.264 / H.265 elementary
stream, OUT/stream.<ext>; ffmpeg decodes it (errors, frame count, picture types) and every frame's band
means are printed from OUT/frame-<n>.png (pictures stay local).
"""
import io
import os
import re
import subprocess
import sys

out = sys.argv[1]
ext = sys.argv[2] if len(sys.argv) > 2 else "jpg"


def bands_of(im):
    im = im.convert("YCbCr")
    w, h = im.size
    px = im.load()
    bands = []
    for b in range(8):
        x0, x1 = w * b // 8 + w // 32, w * (b + 1) // 8 - w // 32
        acc, k = [0, 0, 0], 0
        for y in range(h // 4, h * 3 // 4, 16):
            for x in range(x0, x1, 8):
                p = px[x, y]
                acc = [acc[i] + p[i] for i in range(3)]
                k += 1
        bands.append("%d,%d,%d" % tuple(a // k for a in acc))
    return "%dx%d bands(Y,U,V): %s" % (w, h, " ".join(bands))


if ext in ("h264", "h265"):
    import glob
    from PIL import Image
    data = open(os.path.join(out, "streams.bin"), "rb").read()
    path = os.path.join(out, "stream." + ext)
    fmt = ["-f", "hevc" if ext == "h265" else "h264"]
    open(path, "wb").write(data)
    for f in glob.glob(os.path.join(out, "frame-*.png")):
        os.remove(f)
    err = subprocess.run(["ffmpeg", "-v", "error"] + fmt + ["-i", path, os.path.join(out, "frame-%03d.png")],
                         capture_output=True, text=True).stderr.strip()
    types = subprocess.run(["ffprobe", "-v", "error"] + fmt + ["-show_entries", "frame=pict_type,pkt_size",
                            "-of", "csv=p=0", path], capture_output=True, text=True).stdout.split()
    frames = sorted(glob.glob(os.path.join(out, "frame-*.png")))
    for n, f in enumerate(frames):
        print("frame %d %s: %s" % (n, types[n] if n < len(types) else "?", bands_of(Image.open(f))))
    print("%s: %d bytes, %d frames, ffmpeg=%s" % (ext, len(data), len(frames), "ok" if not err else "ERR " + err[:300]))
    sys.exit(1 if err or not frames else 0)
fmt = []
data = open(os.path.join(out, "streams.bin"), "rb").read()
sizes = [int(m.group(2)) for m in re.finditer(r"^T32J (\d+) (\d+)", open(os.path.join(out, "sample.txt"), errors="replace").read(), re.M)]
pos, bad = 0, 0
for n, size in enumerate(sizes):
    chunk = data[pos:pos + size]
    pos += size
    path = os.path.join(out, "stream-%d.%s" % (n, ext))
    open(path, "wb").write(chunk)
    err = subprocess.run(["ffmpeg", "-v", "error"] + fmt + ["-i", path, "-f", "null", "-"],
                         capture_output=True, text=True).stderr.strip()
    line = "stream %d: %d bytes soi=%s eoi=%s ffmpeg=%s" % (
        n, size, chunk[:2] == b"\xff\xd8", chunk[-2:] == b"\xff\xd9", "ok" if not err else "ERR " + err[:120])
    if err or chunk[:2] != b"\xff\xd8":
        bad += 1
    if ext == "jpg" and not err:
        from PIL import Image
        im = Image.open(io.BytesIO(chunk)).convert("YCbCr")
        w, h = im.size
        px = im.load()
        bands = []
        for b in range(8):
            x0, x1 = w * b // 8 + w // 32, w * (b + 1) // 8 - w // 32
            acc, k = [0, 0, 0], 0
            for y in range(h // 4, h * 3 // 4, 16):
                for x in range(x0, x1, 8):
                    p = px[x, y]
                    acc = [acc[i] + p[i] for i in range(3)]
                    k += 1
            bands.append("%d,%d,%d" % tuple(a // k for a in acc))
        line += " %dx%d bands(Y,U,V): %s" % (w, h, " ".join(bands))
    print(line)
print("streams %d, bad %d, bytes left %d" % (len(sizes), bad, len(data) - pos))
sys.exit(1 if bad or not sizes else 0)
