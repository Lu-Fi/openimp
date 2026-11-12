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
- H.264 attribute placement (T33): the common head (maxQp/minQp/initialQp/maxIQp/minIQp) -> +0xf0/+0xec/+0xf8/+0xfc/
  +0x100, bit rate +0x110, iBiasLvl +0x11c, changePos +0x134, staticTime +0x130, qualityLvl +0x138, maxIprop/minIprop
  +0x150/+0x154, maxI/PPictureSize +0x158/+0x15c, CBR flucLvl +0x13c and IP/PPfrmQPDelta +0x120/+0x124, SMART/AVBR
  minStillRate/maxStillQp +0x160/+0x164 and the super-SMART fields +0x168..+0x174, CVBR long max/min +0x178/+0x17c,
  short/long stat time +0x180/+0x184, extraBitRate +0x188. This follows the i264e defaults and the controller clamps
  the fields end in; the vendor's IMP -> i264e layer (`IMP_Encoder_SetChnAttrRcMode`, `i264e_reconfig_rc_set`,
  `i264e_idr_reconfig`) is not checked against it yet. The per-picture motion mode is 2 (`i264e_update_fenc`).
- H.265 / QVBR mode mapping (T23 revision): CBR -> CBR (outBitRate, iBiasLvl, staticTime, qualityLvl); VBR -> VBR
  (maxBitRate, changePos, staticTime, qualityLvl); SMART -> SMART; CVBR -> VBR on maxBitRate and shortStatTime (the
  long-term bounds are not modelled); AVBR -> SMART; QVBR (PRJ009/PRJ010 only in the vendor header) -> VBR at
  targetBitrate. QPs clamped to 0..51, VBR rates floored at 128 kbit/s, frame / GOP QP steps 3 / 15. maxIQp /
  minIQp bound the IDR QP.
- A rate-control change (`IMP_Encoder_SetChnAttrRcMode`) restarts the controller at the next IDR, as i264e does.
- Not done: re-encoding of oversized pictures (FRAME_REPEATE_JUDGE skipped), the per-row slice sizes the vendor reads
  from HERA after an IDR (zero for now; the controller's IDR slice targets fall back to its split), the slice block
  and MB QP map the T33 revision fills for the engine (computed, not yet put into the command list), macroblock QP
  (HERA QP generator stays off, P pictures get the window 1..51 as before), the H.265 T33 revision, the IMP ->
  i264e layer check (above).

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

## QEMU (`-M ingenic-t32`, no RC statistics / motion search in the HERA model)

Runs `run-oi-cbr264` / `run-oi-cbr265` (OpenIMP, CBR 1000 kbit/s, 1920x1080, GOP 15, 30 pictures): the
picture QP moves with the coded size (36 38 37 36 34 ... 15), EFE_FRM_QP of every command list equals the slice
QP, both streams decode with ffmpeg without errors and show the colour bars. Device proof pending (no T32 camera).

T33 revision (H.264, 2026-11-12): the same CBR run gives the picture QPs 34 31 30 29 ... 18, then 15 from the second
GOP on (sizes ~200 bytes, far below the budget), the sequence the vendor controller gives under unicorn for the same
sizes; H.264 and H.265 decode with ffmpeg without errors. IMP_Encoder_SetDefaultParam fills only the basic fields:
with the I/P and P/P QP steps at 0 the QP would stay at the IDR QP for the whole GOP, so a 0 there keeps the i264e
default (8 / 3).
