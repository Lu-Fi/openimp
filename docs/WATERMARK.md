# SetFisheyeEnableStatus: the vendor luma watermark (issues #357, #397)

`IMP_Encoder_SetFisheyeEnableStatus` does no lens correction in any vendor
library.  With the flag set before `IMP_Encoder_CreateChn`, the vendor encoder
marks every I picture with an invisible watermark in the top-left 64x16 luma
pixels of the source frame.  On T10/T20/T23 the vendor i264e also codes a QP
region (docs/T20_FISHEYE.md, docs/T21_FISHEYE.md, `src/fisheye`).  OpenIMP does
both, vendor-equal, on every SoC whose vendor library has the call.

## Where the vendor applies it

| SoC (vendor libimp) | Encoder path | Plane handed to `embed_watermark` |
|---|---|---|
| T10/T20 3.12.0 | `hwicodec_pf_h264e_t10_enc`, `_t20_enc` | source luma, stride `mb_width*16`, height `mb_height*16` |
| T21 1.0.33, T23 1.3.0 | `hwicodec_pf_h264e_t21_enc` | same |
| T30 1.0.5 | `hwicodec_pf_h264e_t30_enc`, `hwicodec_pf_h265e_t30_enc` | same (H.265: aligned width/height) |
| T31 1.1.6, T40 1.3.1 | Allegro `encode1` (I slice) | source luma, `ALIGN16(width)`, `ALIGN16(height)` |
| T41 1.2.6 | none: neither the call nor the routine | - |

The context comes from `watermark_init()` at channel creation (H.264
`hwicodec_*_init`, Allegro `AL_Common_Encoder_CreateChannel`).  The routine runs
on every hardware run of an I slice (Helix: a re-encoded I picture is marked
again) and rewrites the source frame in place, before the hardware reads it.

T31/T40 `encode1` also writes the QP table (EP2 + 0x40, the same buffer as the
ROI table) while the flag is set, on every picture (#402):

- entries 0..12 cleared;
- an I picture, after `embed_watermark` succeeded: H.264 entries 0..3 (the four
  top-left 16x16 macroblocks) = 0x21; H.265 entries 0..4 and 8..12 (the first
  two 32x32 CTBs of the 8-byte-per-CTB layout, `AL_GetAllocSizeEP2(HEVC)` =
  align128(8 * CTBs) + 0x40) = 0x21.  0x21 is relative -31 (6-bit two's
  complement): the marked blocks are coded at the lowest QP the hardware gives;
- the picture's "use QP table" and "relative" options (cmd[9] bits 25, 24).
  When `embed_watermark` fails the I picture keeps the cleared table without
  the two options.

On a textured scene the -31 blocks make the hardware raise its LCU overflow
warning: status word +0x104 bit 31, bits 29:0 the entropy byte count (vendor
`EntropyStatusRegsToSliceStatus`).  The vendor prints "Warning some LCU exceed
the maximum allowed bits" (`AL_WARN_LCU_OVERFLOW`) and keeps the picture.
OpenIMP did not mask the size word, so the first marked IDR looked larger than
the stream buffer and was dropped; it now masks it and logs the same warning.
`OPENIMP_WM_PARTS` (default 3: bit 0 the luma mark, bit 1 the QP entries)
switches a part off.

## The routine (`src/watermark`)

- The 64x16 area is cut into 16 8x8 blocks (8 across, 2 down).
- Each block gets a single-precision 8x8 DCT with the vendor's six-digit cosine
  table.  A block whose DC magnitude is below 80 (mean luma below 10) is
  replaced by a fixed coefficient block.
- The coefficients with the 31st, 32nd and 33rd largest magnitude are set to the
  32nd magnitude (0.5 when that is 0.5 or less), each keeping its sign.
- Coefficients 33 and 12 (raster order) become `(1 + 0.1 * mark) * level` with
  their own sign; `mark` is the block's pair of a fixed 32-value pattern.
- The inverse DCT goes back to pixels: + 0.5, truncated to 8 bits **without
  clamping** (a result of 256 is stored as 0), like the vendor.

The vendor library also has `check_watermark()`, a detector that estimates the
mark of each block and correlates it with the pattern (0.42 or more = marked).
`tools/watermark_check.py` runs it on the decoded I pictures of a stream.

### Rounding: fused or not

The routine is the same code in all vendor libraries, but the gcc 4.7.2 builds
(T10/T20 3.12.0, T21 1.0.33, T30 1.0.5) and the gcc 7.2 T40 build compile the
DCT sums and the mark gain to `madd.s`, which the Ingenic FPU computes with one
rounding (fused).  The gcc 5.4 builds (T23 1.3.0, T31 1.1.6) use separate
multiply and add.  Thingino ships 4.7.2 builds for T10/T20/T21/T30, 5.4 builds
for T23/T31 and 7.2 for T40, so `WATERMARK_FUSED_MADD` is 1 for T20 (T10),
T21, T30 and T40 builds and 0 for T23 and T31.  On the camera the fused form is
the `madd.s` instruction itself.

## Evidence

- `tools/watermark_oracle.py`: the vendor `embed_watermark` under unicorn (which
  rounds `madd.s` twice) on random planes; all eight vendor builds give the same
  output; `tests/watermark/watermark_vectors.txt` (unfused) is checked byte for
  byte by `make -C tests/watermark check`.  6000 further random cases matched.
- `tools/watermark_devcheck.c`: on the camera, the vendor routine (dlopen of the
  vendor libimp from /tmp) and `watermark_embed()` on the same 4000 random
  planes: 0 mismatches on T20, T21, T23, T31 and T40 (one camera each).  The
  T20 run showed the fused rounding (the unfused form differed in 962 of 1000
  planes there); `tests/watermark/watermark_vectors_fused.txt` are vendor results
  recorded on the T20 camera.
- Streams (apitest case `fisheye`, 640x360, FixQP 30): on T20 the vendor
  detector finds the mark in every I picture of the vendor library and of
  OpenIMP with the flag (correlation 0.92..0.97) and in none without (<= 0.10).
  On the T31 camera's textured scene the detector stays below its threshold for
  both libraries (vendor 0.05..0.40, OpenIMP 0.04..0.35, flag off up to 0.29);
  there the CPU check above is the proof and the trace shows the routine on
  every IDR.

## QP entries on the cameras (#402, 2026-11-12)

- T31 (.26, apitest case `fisheye`, 640x360 FixQP 30, vendor 1.1.6 and OpenIMP
  back to back): first IDR macroblock QPs (tools/fisheye_mbqp.py), row 0 x 0..7
  / row 1: vendor 3 2 1 0 27 27 27 27 / 33 33 29 30 29 27 27 27, OpenIMP the
  same, without the entries 33 31 30 29 ...; both print the LCU warning once per
  marked IDR; every OpenIMP IDR (0, 10, 20) codes and decodes.
- T31 H.265 (CBR; FixQP H.265 on T31 is broken without the flag too, #403):
  every IDR codes and decodes with the entries; there is no H.265 per-block QP
  reader, so the QPs are not compared.
- T40 (.184, OpenIMP only, FixQP 30): IDR row 0 = 0 0 0 0 29 29 29 27, the
  IDRs decode; H.265 (CBR) codes and decodes.  A vendor T40 run needs the
  vendor stack and was not done.
- Host: tests/t31 stream_layout_test (size mask and warning bit).

## Data note

The constant tables of the mark (DCT magnitude ranking, coefficient levels, QP region entries) are taken over from the vendor library as plain data (user decision 2026-11-12); the code around them (`src/watermark`) is hand-written C, equal to the vendor routine on the host oracle and on the devices listed in the changelog. This is vendor-equal behaviour, not a beyond-vendor item.
