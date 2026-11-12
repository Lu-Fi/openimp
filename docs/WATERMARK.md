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

T31/T40 `encode1` also writes the QP table (EP2 + 0x40) while the flag is set:
entries 0..12 cleared on every picture, entries 0..3 (the four top-left
macroblocks of an H.264 I picture) set to 0x21 (relative -31), and the picture's
"use QP table" and "relative" options.  For H.265 it writes ten entries of the
CTB layout instead.

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

## Not ported

- The T31/T40 QP table entries.  On the open T31 stack the first IDR with the
  mark and the -31 entries fails in Enc1 (no payload) and the channel stalls;
  each part alone codes fine, the vendor prints "some LCU exceed the maximum
  allowed bits" and goes on.  `OPENIMP_WM_PARTS` (bit 0 the mark, default; bit 1
  the QP entries) is there to work on it (#402).  The H.265 entries are not written.
