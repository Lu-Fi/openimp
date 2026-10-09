# Encoder ROI per SoC (branch `claude/roi-all`, not in the first release)

| SoC | API | state |
|---|---|---|
| T10/T20 | `IMP_Encoder_SetChnROI` (8 regions, pixel corners) | works, see `T1X_ROI_CHROMA.md` |
| T21 | the same | works by default since this branch (beyond vendor), device-tested |
| T23 | the same, passed to the vendor i264e (param 2) | code unchanged, not tested (below) |
| T40/T41 | `IMP_Encoder_Set/GetChnRoiAttr` (10 windows, delta QP) | implemented, **experimental, off by default**; the missing enable bit is now set (below), not yet device-tested |
| T31 | none in the vendor library; OpenIMP: `IMP_Encoder_Set/GetChnRoiAttr` (T41 API) | works by default (beyond vendor), device-tested; QP clamped to valid H.264 (below) |

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

**Probable cause, found with the T31 (code only on T41):** the AVPU reads the
table only when the command enables it. In the vendor T41 1.2.6 `encode1`
copies `AL_OPT_USE_QP_TABLE` (picture option bit 0) to SliceParam+0x63 and
the "relative" flag to SliceParam+0x62 (offsets from the SliceParam base
pic+0x248, matched against the neighbouring fields), and
`SliceParamToCmdRegsEnc1` (0xe9d68) packs them into **cmd[152] bit 0 and
bit 3**. OpenIMP's T41 builder writes cmd[152] = 0xf6 (both clear). With
`OPENIMP_T41_ROI=1` and a window set, OpenIMP now ORs 0x9 into cmd[152].
Not tested on a T41 yet; the header-word hang above is unrelated (do not
write the EP2 header). On the vendor side the 32-bit entries with byte 3 =
0x20 suggest that the vendor sets the bit for every picture.

## T31 (device-tested, beyond vendor)

The vendor T31 libimp 1.1.6 has no ROI API, but its Allegro core shows how
the AVPU takes a macroblock QP table (HLIL in `docs/re/libimp.so_hlil.txt`):

- `AL_Common_Encoder_Process` sets picture option bit 0
  (`AL_OPT_USE_QP_TABLE`) when a QP buffer comes with the frame and uses that
  buffer as EP2; otherwise EP2 is `AL_IntermMngr_GetEp2Addr` (interm buffer
  + EP1 + WPP), which is OpenIMP's `cmd[0x23]`.
- `encode1` copies the picture option bit 0 to SliceParam+0x6c and channel
  option bit 0 to SliceParam+0x6b; `SliceParamToCmdRegsEnc1` packs them into
  **cmd[9] bit 25 (use the table) and bit 24 (table relative)**. The vendor
  watermark path (`embed_watermark`) sets both. Without bit 25 the AVPU
  ignores EP2's table; OpenIMP's command template (cmd[9] = 0xfc010000 /
  0xfc000000) had both clear.
- table layout (`AL_GetAllocSizeEP2`, `AL_RoiMngr_FillBuff`): the 0x40-byte
  EP2 header (auto-QP seed and QP range, written by `avpu_t40_init_ep2`;
  zeroing it gives QP 0 pictures, 33 Mbit/s), then **one byte per 16x16
  macroblock**, raster order: bits 5:0 QP (6-bit two's complement when
  relative), bit 6 force intra, bit 7 force skip.

Measured on the garage camera (sc4336p, 2560x1440): bit 25 alone = absolute
table (an entry 0 is QP 0), bit 24 alone = no effect, both = relative table.
OpenIMP always uses the relative table. An absolute window (`mode =
IMP_ROI_QPMODE_FIXED_QP`, QP 0..51) is written as the difference to the
picture QP of the last command and rewritten when that QP changes: exact under
FixQP, one picture late under CBR/VBR. Delta windows take -26..25 (the vendor T41 range; `SetChnRoiAttr` returns -1
outside, it used to take -32..31). A window
covers every macroblock it touches; with overlapping windows the higher index
wins. The table is rewritten (and flushed) only when the windows or, with an
absolute window, the picture QP change; with no window the command and EP2
are as before. `OPENIMP_T31_ROI=0` refuses `SetChnRoiAttr`. H.264 only.

Results (`tools/roitest`, night/IR, region 768x512 at (896,464), 10 fps):

| run | IDR bytes | P bytes | kbit/s |
|---|---|---|---|
| FixQP 30, no ROI | 176 K | 2.5 K | 1124 |
| FixQP 30, delta +20 | 157 K | 2.2 K | 1410 (3 IDRs) |
| FixQP 30, absolute 51 | 156 K | 2.2 K | 1403 (3 IDRs) |
| FixQP 30, absolute 10 / delta -20 | 247 K | 44 K | 4550 |
| FixQP 30, 2 windows (1280x720 delta +25, 1280x720 absolute 51) | 98 K | 1.2 K | 843 |
| CBR 1 Mbit/s, no ROI / absolute 45 / delta +20 | 119 / 92 / 94 K | | 1336 / 911 / 1138 |

The decoded pictures show the region visibly blurred/blocky at +20 / 51 and
the rest unchanged (collages local only). A change on P pictures without IDR
works (P frames 2.7 K to 2.5 K). (Measured before the clamp below: whole-picture
offsets were not limited to the channel QP range, CBR +31: 82 kbit/s, -32:
9.9 Mbit/s; afterwards CBR needed a few seconds to recover.) Stability: 8 live changes in one stream,
streamer restarted, 0 oops.

## Valid H.264: the QP clamp (all SoCs with ROI)

H.264 allows `mb_qp_delta` only in -26..+25, and the encoder writes it as
QP(macroblock) - QP(previous macroblock in coding order); skipped macroblocks
keep the previous QP. Anything outside is **invalid H.264**: software ffmpeg
hides it, hardware decoders (Intel VA-API, VLC, browsers) show broken blocks.
Measured on T10/T20: relative -26 and an absolute QP more than 25 from the
macroblock QP (absolute 15 at QP 42) both break the stream. A relative -26
fails on the way *out* of the window (+26 back to the neighbours).

`src/avpu_roi.h` (T31, experimental T41) and `Helix_H264_RoiSanitize` in
`src/t30/helix_roi.h` (T10/T20/T21, applied when each command list is built
with the slice QP of that picture) therefore guarantee:

- every window delta is in **-25..+25** (absolute QPs are turned into a delta
  against the picture QP first);
- the spread of all deltas, uncovered macroblocks counting as 0, is **at
  most 25**; if it is larger the higher QPs are lowered (quality wins);
- picture QP + delta stays in **0..51** and in the rate control's
  **min_qp..max_qp** (T31/T41-style table; positive deltas above max_qp were
  only capped by the hardware before, now the table says what happens);
- Helix absolute QPs lie in `[max_qp - 25, min_qp + 25]` of the picture
  (T21/T30/T10 command lists: slice QP -12/+13, which is also the saturation
  seen on the T21; T20: the `max_qp_cap`), so every macroblock QP is within
  25 of the window QP.

Requests beyond that are clamped, a one-time warning is logged
(`ROI: requested QP delta ... clamped`), `SetChnRoiAttr` (T31, T41) returns -1
for a delta outside -26..25 or an absolute QP outside 0..51. The T31/T41
table is rewritten when the picture QP changes and the clamp depends on it
(absolute window, or a delta that touched the QP range). Under CBR/VBR the
picture QP is the one of the last command, so the clamp can be one picture
late. Host test: `tests/t31/roi_clamp_test.c` (fuzz of 20000 random window
sets, every table is walked like the encoder and checked against -26..25).
T23 passes the regions to the vendor i264e unchanged (not tested).

## T23 (code only)

Untested; on the Jooan: `roitest_t23` (tools/roitest) with a region, FixQP
and CBR, check the stream with a decoder, 8 regions, live change, region at the
picture edge, H.265.
