#!/usr/bin/env python3
"""NV12 frame (Y stride = ALIGN16(w), UV after ALIGN16(h) rows) -> PNG.

usage: nv12png.py IN.nv12 WIDTH HEIGHT OUT.png   (needs Pillow; pictures stay local)
"""
import sys
from PIL import Image

src, w, h, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
stride, rows = (w + 15) & ~15, (h + 15) & ~15
data = open(src, "rb").read()
y = Image.frombytes("L", (stride, rows), data[:stride * rows]).crop((0, 0, w, h))
uv = data[stride * rows:stride * rows + stride * rows // 2]
u = Image.frombytes("L", (stride // 2, rows // 2), bytes(uv[0::2])).crop((0, 0, w // 2, h // 2))
v = Image.frombytes("L", (stride // 2, rows // 2), bytes(uv[1::2])).crop((0, 0, w // 2, h // 2))
u, v = u.resize((w, h)), v.resize((w, h))
Image.merge("YCbCr", (y, u, v)).convert("RGB").save(dst)
print(dst)
