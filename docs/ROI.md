# Encoder ROI per SoC (branch `claude/roi-all`, not in the first release)

| SoC | API | state |
|---|---|---|
| T10/T20 | `IMP_Encoder_SetChnROI` (8 regions, pixel corners) | works, see `T1X_ROI_CHROMA.md` |
| T21 | the same | works by default since this branch (beyond vendor), device-tested |
| T23 | the same, passed to the vendor i264e (param 2) | code unchanged, not tested (below) |
| T40/T41 | `IMP_Encoder_Set/GetChnRoiAttr` (10 windows, delta QP) | implemented, **experimental, off by default**: the hardware did not react |
| T31 | none in the vendor library | not done |

## T21 (device-tested)

PC420, jxf23, 1920x1080, `tools/roitest`: one 320x320 region at (240,560).
FixQP 30: absolute QP 51 blurs exactly the region, QP 15 stays clean, delta
+15/-15 work, a change on P frames (no IDR) works. Stream size 657 kbit/s
without ROI, 340 with QP 51, 504 with QP 15. CBR keeps its target; the ROI
moves the bits. Details: `T1X_ROI_CHROMA.md`. `OPENIMP_T21_ROI=0` switches
the programming off. The region corners are rounded down to macroblocks, x1/y1
are inclusive.

## T41 (family B)

Found on a T41 with the vendor libimp 1.2.6 (`IMP_Encoder_SetChnRoiAttr`
disassembly, command-list and EP2 captures with an `LD_PRELOAD` tracer):

- the vendor checks `x+w <= width`, `y+h <= height` of the channel for every
  enabled window, stores the 280-byte attribute and sets a flag; the encoding
  thread then builds a table before the next picture.
- the table is the EP2 buffer: after a 0x40-byte header one 32-bit entry per
  16x16 macroblock, raster order, row stride `ceil(width/16)`. Byte 0 is the
  relative QP (int8, -26..25), byte 3 is 0x20 in every entry (also without a
  window). Windows: corner rounded down, size to the nearest macroblock.
  Capture: windows of 40x20 and 10x6 macroblocks at 1080p changed exactly
  those 800 + 60 entries (byte 0 only).
- the AVC EP2 size of the vendor is `0x40 + 4 * macroblocks` (OpenIMP's was
  one byte per macroblock); the command words of a picture are identical with
  and without a window.

OpenIMP writes that table (`avpu_t41_roi_apply`, EP2 enlarged) and exports
`IMP_Encoder_SetChnRoiAttr/GetChnRoiAttr`. **The encoder did not react** in
FixQP or CBR (whole-frame -20/-25/+20/+25, regions up to a quarter of the
frame; bit rate and decoded pictures unchanged). Writing the EP2 header words
the vendor library has (QP limits) was tried and hung the camera. Therefore
all of it is off unless `OPENIMP_T41_ROI=1`; without it the EP2 size, the
commands and `SetChnRoiAttr` (returns -1) are as before. What is missing is a
vendor reference that works: the vendor libimp on this open-tx-isp camera
gives corrupt pictures and twice hung the kernel, so an A/B against the vendor
could not be made.

IDR pictures did not respond to the table in any variant. `SetChnMapRoi` is
not implemented.

## T31

No ROI in the vendor library (only the Allegro `AL_RoiMngr_*` inside it).
Not attempted: no working family-B reference on the AVPU (see T41).

## T23 (code only)

Untested; on the Jooan: `roitest_t23` (tools/roitest) with a region, FixQP
and CBR, check the stream with a decoder, 8 regions, live change, region at the
picture edge, H.265.
