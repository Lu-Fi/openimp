#!/bin/sh
# #882 guards: (1) the H.264 source rows between the picture height and the 16-aligned height repeat the last picture
# row before the HERA list is built (vendor 2.2.0 on the same driver; without it every P picture codes the last
# macroblock row again, 1.7-2.5x the vendor's P size at the same QP); (2) FrameSource REQBUFS asks for nrVBs + max
# delay as vendor 2.2.0 IMP_FrameSource_EnableChn (0x1452d0).
set -e
e=${1:-../../src/t32/openimp_t32_encoder.c}
f=${2:-../../src/t32/openimp_t32_fs.c}
grep -q 'memcpy(y + r \* stride, y + (h - 1u) \* stride, stride);' "$e"
grep -q 'memcpy(uv + r \* stride, uv + (h / 2u - 1u) \* stride, stride);' "$e"
grep -A1 'if (e->h264 && !e->yuv)' "$e" | grep -q 'enc_pad_rows(f, \*stride);'
grep -q 'rq.count = (uint32_t)c->attr.nrVBs + (c->maxdelay > 0 ? (uint32_t)c->maxdelay : 0u);' "$f"
echo "check_enc_pad: ok"
