# T32 rate control on HERA (#295, planned)

T32 support is planned (QEMU only, no T32 camera yet); this is not a release gate.

## What runs

- The T32 API (vendor 2.x, PRJ007) has the T23-family rate-control attributes plus CVBR, AVBR and QVBR
  (`include/imp/imp_encoder.h`, `IMPEncoderAttrRcMode`, 64 bytes). FIXQP keeps the fixed I/P QPs.
- H.264: `src/t32/openimp_t32_rc.c` puts the attributes into an i264e parameter block and runs the port of the
  vendor's T33 revision (`src/eprc/eprc_t33_h264.c`) behind `src/t32/openimp_t32_rc33.c`, a reimplementation of the
  vendor i264e side (`i264e_param_default`, `i264e_ratecontrol_init` / `_start` / `_end` / `_is_reenc`) on the
  controller's address arena (`src/eprc/eprc_t33_mem.h`). H.265 (its T33 revision is not ported yet) and QVBR map
  onto `src/eprc` (the T23 revision of the vendor "eprc" controller, bit-exact with the T23 OEM code,
  docs/T23_EPRC.md). Per picture FRAME_START gives the QP, which goes into the HERA command list (EFE_FRM_QP, the
  CABAC tables and the QP-banded analysis defaults follow it) and the slice header; after the picture the coded
  slice size and the engine statistics go to FRAME_END.
- Statistics: vendor 2.2.0 reads 48 registers after each H.264 picture; the first 25 are the Helix registers the
  T23 controller reads (0x13200000 + 0x80120..0x8014c, 0x500e8.., 0x80080, 0x40094..0x400a0, 0x800e0..0x800ec,
  0x80168: HERA keeps them at the Helix offsets), and the vendor `i264e_ratecontrol_end` folds them the same way
  (QP sums `lo16 + hi16`, SSE pairs masked to 27 bits). H.265 has no such registers in its read list: its
  controller gets the coded size only.
- H.264 attribute placement (T33), checked against the vendor 2.2.0 IMP layer (below): channel set-up as
  `IMP_Encoder_CreateChn` -> `channel_i264e_encoder_init` -> `i264e_validate_parameters`. The QP bytes
  (maxQp/minQp/blkQpEn/initialQp/maxIQp/minIQp) -> +0xf0/+0xec/byte +0xf4/+0xf8/+0xfc/+0x100, bit rate +0x110,
  iBiasLvl +0x11c, changePos +0x134 (not CBR), staticTime +0x130 (not CVBR), qualityLvl +0x138 (not CBR),
  maxIprop/minIprop +0x150/+0x154, maxI/PPictureSize +0x158/+0x15c, CBR flucLvl +0x13c and IP/PPfrmQPDelta
  +0x120/+0x124, SMART/AVBR minStillRate/maxStillQp +0x160/+0x164 and the super-SMART fields byte +0x168, +0x16c..
  +0x174, CVBR long max/min +0x178/+0x17c, short/long stat time +0x180/+0x184, extraBitRate +0x188, FIXQP I/P/B QP
  +0xe0/+0xe4/+0xe8. The vendor has no "0 = default" rule: a field goes in as given and only the
  `i264e_validate_parameters` clamps change it (QPs 0..51, min QP <= max QP, bit rate >= 128, static / stat times
  >= 1, change position 50..100, quality 0..7, flucLvl 0..8, QP steps 0..15 (0 stays 0), I share 0..100 and min <=
  max, still rate 1..100, still QP 0..51, super-SMART level 0..10 / rate level 0..3 / max rate 1..100, extra rate
  0..1000). The one default: maxIPictureSize 0 -> 1.5 x the rate. Further: frame rate reduced (0 -> 25/1), GOP
  rounded up to a multiple of the frame-rate numerator (0 -> the frame rate; OpenIMP's H.264 IDR interval follows
  it), VBV size +0x1a0 = (stream buffer - 8320) x 8 bits clamped to 3/50 .. 12/3 (12/6 above 800 pixels wide) bits per
  pixel, +0x1a4 = that / 1.4, the bitstream-full settings +0x344/+0x348 = on / 250. The stream buffer is
  `CreateChn`'s: bufSize 0 -> 1.5 bytes per pixel + 8320, else at least 0.75 / 2 (0.75 / 3 above 800x600) bytes per
  pixel + 8320 and 16640, 4-aligned. The per-picture motion mode is 2 (`i264e_update_fenc`).
- `IMP_Encoder_SetDefaultParam` fills the vendor's full mode defaults (QP 15..45 for P and I, IP/PP QP step 8/8,
  static time 2, CBR flucLvl 2 / quality 3, others quality 6 / change position 80, I share 1..100, max I/P picture
  1.2 / 0.72 x the rate, SMART still rate 25 / still QP 35 / super-SMART 5, 2, 20, CVBR long max/min 0.8 / 0.6 x the
  rate, stat times 2 / 60, extra 5; FIXQP I = P = the initial QP, B 0) and, for H.264, profile index 2 (High) as the
  vendor does.
- `IMP_Encoder_SetChnAttrRcMode` (H.264) is taken at the next IDR the vendor way: through
  `i264e_reconfig_rc_set` into the encoder's reconfiguration copy of the parameters, which `i264e_idr_reconfig`
  copies back before the clamps and `i264e_ratecontrol_init`. Fields the new mode does not carry keep their last
  value; maxIPictureSize 0 stays 0, the CBR qualityLvl is taken, blkQpEn becomes 0/1, and the SMART
  supSmtStillRateLvl is taken from maxIprop clamped to 0..3 (vendor code, kept for equal behaviour).
- H.265 / QVBR mode mapping (T23 revision): CBR -> CBR (outBitRate, iBiasLvl, staticTime, qualityLvl); VBR -> VBR
  (maxBitRate, changePos, staticTime, qualityLvl); SMART -> SMART; CVBR -> VBR on maxBitRate and shortStatTime (the
  long-term bounds are not modelled); AVBR -> SMART; QVBR (PRJ009/PRJ010 only in the vendor header) -> VBR at
  targetBitrate. QPs clamped to 0..51, VBR rates floored at 128 kbit/s, frame / GOP QP steps 3 / 15. maxIQp /
  minIQp bound the IDR QP.
- H.265 and QVBR: a rate-control change (`IMP_Encoder_SetChnAttrRcMode`) restarts the controller at the next IDR.
  QVBR: vendor 2.2.0 refuses it on PRJ007 (SetDefaultParam / CreateChn / SetChnAttrRcMode take modes 0..5 only);
  OpenIMP keeps running it as VBR (beyond vendor, unchanged).
- Not done: re-encoding of oversized pictures (FRAME_REPEATE_JUDGE skipped), the per-row slice sizes the vendor reads
  from HERA after an IDR (zero for now; the controller's IDR slice targets fall back to its split), the slice block
  and MB QP map the T33 revision fills for the engine (computed, not yet put into the command list), macroblock QP
  (HERA QP generator stays off, P pictures get the window 1..51 as before), the H.265 T33 revision, the other
  run-time setters (SetChnBitRate, SetChnQpBounds, ...) on the reconfiguration copy, IVDC channels.

## Vendor reference (libimp 2.2.0, eprc T33 revision)

The vendor controller is a new revision (`eprc_default_set_H264_T33`, `JZ_VPU_RC_{VIDEO_CFG,FRAME_START,FRAME_END}_H264_T33`
and helpers `get_I_frame_qp`, `get_P_frame_qp`, `estimate_qp`, `init_inst_param`, `get_bu_target_bits`,
`VPI_set_sliceinfo`, ...; H.265 the same set without `_H264`, about 70 KiB of code each), not the T23 one.
`tools/eprc_t33_oracle.py` runs it under unicorn through the vendor `i264e_param_default` /
`i264e_ratecontrol_init` / `_start` / `_is_reenc` on a synthetic size model; `tests/t32/vendor-2.2.0-rc-h264.txt`
holds its QP / size sequences (CBR, VBR, SMART, CVBR, AVBR at 1000 / 2000 / 4000 kbit/s, 1920x1088, GOP 50,
25 fps, QP 15..48, 250 pictures each), `vendor-2.2.0-rc33-h264.txt` 40 random scenarios (scene cuts, still periods,
sizes up to 2560x1440, 10..30 fps), both with the per-picture motion mode the harness left at 0, and
`vendor-2.2.0-rc33-motion2-h264.txt` the same scenarios with the device value 2 (`--motion 2`). `make -C tests/t32
check` runs OpenIMP on the same model: `rc33_test` drives the port through the i264e glue with the harness's
parameters, `rc_test` through the IMP attribute mapping; both require the vendor's QP of every picture, and get it
for every mode (CBR, VBR, SMART, CVBR, AVBR) and every run. The port itself is checked per call against the vendor
functions under unicorn (`tools/t32/eprc33`, README there: 208 random scenarios, memory equal byte for byte).

vendor i264e defaults (2.2.0 `i264e_param_default`, offsets in the i264e parameter block): mode +0xdc 1 (CBR), FIXQP
I/P +0xe0/+0xe4 35/35, QP +0xec/+0xf0 15..45, initial QP +0xf8 35, I QP +0x100/+0xfc 15..45, bit rate +0x110 2000
kbit/s, change pos +0x134 80, static time +0x130 2, still rate / still QP +0x160/+0x164 25/35, CVBR long max/min
+0x178/+0x17c 1600/1200 kbit/s, short/long stat time +0x180/+0x184 2/60, max I/P picture +0x158/+0x15c 2400/1440 kbit.

## IMP layer reference (libimp 2.2.0)

`tools/eprc_t33_imp_oracle.py` drives the vendor IMP layer under unicorn: `IMP_Encoder_SetDefaultParam` per mode
(`--defaults`, `tests/t32/vendor-2.2.0-rc-imp-defaults.txt`: 72 calls), and random channel attributes through
`IMP_Encoder_CreateChn` up to `i264e_init` plus `i264e_validate_parameters`, then a random
`IMP_Encoder_SetChnAttrRcMode` through `i264e_reconfig_rc_set` and `i264e_idr_reconfig` up to
`i264e_ratecontrol_init` (`--reconfig`, `vendor-2.2.0-rc-imp.txt.gz`: 1500 channels; every rate-control field
kept / 0 / in range / raw at random, sizes 352x288 .. 2560x1440, 5..30 fps, GOP 1..120, bufSize 0 or random).
`tests/t32/rc_imp_test.c` requires every i264e word the controller reads to be equal: 72/72 defaults,
1500/1500 set-ups, 1500/1500 run-time changes. `tools/eprc_t33_oracle.py --imp` runs the controller on the block
the vendor IMP layer makes (`vendor-2.2.0-rc-imp-h264.txt`, rc_test: the QP of every picture equal for every
mode and rate). Finding on the way: the i264e default at +0x30c is 0x1001003f (the glue had 0x1000ff3f).

## QEMU (`-M ingenic-t32`, no RC statistics / motion search in the HERA model)

Runs `run-oi-cbr264` / `run-oi-cbr265` (OpenIMP, CBR 1000 kbit/s, 1920x1080, GOP 15, 30 pictures): the
picture QP moves with the coded size (36 38 37 36 34 ... 15), EFE_FRM_QP of every command list equals the slice
QP, both streams decode with ffmpeg without errors and show the colour bars. Device proof pending (no T32 camera).

T33 revision (H.264, 2026-11-12): the same CBR run gives the picture QPs 34 31 30 29 ... 18, then 15 from the second
GOP on (sizes ~200 bytes, far below the budget), the sequence the vendor controller gives under unicorn for the same
sizes; H.264 and H.265 decode with ffmpeg without errors. (That run predates the IMP-layer check: SetDefaultParam
then filled only the basic fields and a 0 QP step kept the i264e default; both now follow the vendor, not re-run in
QEMU yet.)
