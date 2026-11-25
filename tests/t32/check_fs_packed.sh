#!/bin/sh
# #855 guard: the T32 GetFrame depth copy hands out packed NV12 (UV at width * height) when the capture buffer is padded,
# as the vendor 2.2.0 library does on the same driver (device A/B, 640x360); T33 keeps the aligned layout.
set -e
f=${1:-../../src/t32/openimp_t32_fs.c}
grep -q 'if (!c->i2d_on && (c->stride != c->width || ALIGN_UP(c->height, 16) != c->height))' "$f"
grep -A1 'if (!c->i2d_on && (c->stride' "$f" | grep -q 'fs_pack_nv12(c, b, (uint8_t \*)(uintptr_t)slot->dma.virt_addr)'
awk '/^#ifndef PLATFORM_T33/{o=NR} /fs_pack_nv12\(c, b, \(uint8_t/{if (o && NR-o<8) ok=1} END{exit ok?0:1}' "$f"
echo "check_fs_packed: ok"
