# T32 rate control on HERA (#295, planned)

T32 support is planned (QEMU only, no T32 camera yet); this is not a release gate.

## What runs

- The T32 API (vendor 2.x, PRJ007) has the T23-family rate-control attributes plus CVBR, AVBR and QVBR
  (`include/imp/imp_encoder.h`, `IMPEncoderAttrRcMode`, 64 bytes). FIXQP keeps the fixed I/P QPs.
- `src/t32/openimp_t32_rc.c` maps every other mode onto `src/eprc` (the T23 revision of the vendor "eprc"
  controller, bit-exact with the T23 OEM code, docs/T23_EPRC.md) and runs it per picture:
  FRAME_START gives the picture QP, which goes into the HERA command list (EFE_FRM_QP, the CABAC tables and the
  QP-banded analysis defaults follow it) and the slice header; after the picture the coded slice size and the
  engine statistics go to FRAME_END.
- Statistics: vendor 2.2.0 reads 48 registers after each H.264 picture; the first 25 are the Helix registers the
  T23 controller reads (0x13200000 + 0x80120..0x8014c, 0x500e8.., 0x80080, 0x40094..0x400a0, 0x800e0..0x800ec,
  0x80168: HERA keeps them at the Helix offsets), and the vendor `i264e_ratecontrol_end` folds them the same way
  (QP sums `lo16 + hi16`, SSE pairs masked to 27 bits). H.265 has no such registers in its read list: its
  controller gets the coded size only.
- Mode mapping: CBR -> CBR (outBitRate, iBiasLvl, staticTime, qualityLvl); VBR -> VBR (maxBitRate, changePos,
  staticTime, qualityLvl); SMART -> SMART; CVBR -> VBR on maxBitRate and shortStatTime (the long-term bounds are not
  modelled); AVBR -> SMART; QVBR (PRJ009/PRJ010 only in the vendor header) -> VBR at targetBitrate. QPs clamped to
  0..51, VBR rates floored at 128 kbit/s, frame / GOP QP steps 3 / 15. maxIQp / minIQp bound the IDR QP.
- A rate-control change (`IMP_Encoder_SetChnAttrRcMode`) restarts the controller at the next IDR, as i264e does.
- Not done: re-encoding of oversized pictures (FRAME_REPEATE_JUDGE skipped), macroblock QP (HERA QP generator stays
  off, P pictures get the window 1..51 as before), the T33 revision of the controller (below).

## Vendor reference (libimp 2.2.0, eprc T33 revision)

The vendor controller is a new revision (`eprc_default_set_H264_T33`, `JZ_VPU_RC_{VIDEO_CFG,FRAME_START,FRAME_END}_H264_T33`
and helpers `get_I_frame_qp`, `get_P_frame_qp`, `estimate_qp`, `init_inst_param`, `get_bu_target_bits`,
`VPI_set_sliceinfo`, ...; H.265 the same set without `_H264`, about 70 KiB of code each), not the T23 one.
`tools/eprc_t33_oracle.py` runs it under unicorn through the vendor `i264e_param_default` /
`i264e_ratecontrol_init` / `_start` / `_is_reenc` on a synthetic size model; `tests/t32/vendor-2.2.0-rc-h264.txt`
holds its QP / size sequences (CBR, VBR, SMART, CVBR, AVBR at 1000 / 2000 / 4000 kbit/s, 1920x1088, GOP 50,
25 fps, QP 15..48, 250 pictures each). `make -C tests/t32 check` (`rc_test`) runs OpenIMP's controller on the same
model and compares:

| mode | 2000 kbit/s: vendor / OpenIMP | 4000 kbit/s | mean QP diff (2000 / 4000) |
|---|---|---|---|
| CBR | 2031 / 2095 | 4070 / 4105 | 1.79 / 2.26 |
| VBR | 1849 / 1978 | 3474 / 3792 | 2.00 / 2.78 |
| CVBR | 2034 / 1978 | 3993 / 3792 | 1.24 / 1.85 |
| SMART | 2239 / 1978 | 3356 / 3792 | 1.69 / 3.55 |
| AVBR | 1594 / 1978 | 2542 / 3792 | 2.91 / 5.81 |

CBR, VBR and CVBR are checked (rate within 12 % of the vendor, mean QP difference <= 3). SMART and AVBR are
reported only: the T33 revision has still-scene rules the T23 one lacks (SMART codes every IDR at a fixed QP, 35 =
i264e default +0x164; AVBR lowers the rate towards minStillRate). Next step: port the T33 revision (H.264 first)
bit-exact against the oracle, then the H.265 one and the HERA per-row statistics the vendor reads directly through
`/dev/mem` (`i264e_ratecontrol_init` maps 0x13240000 / 0x13220000).

vendor i264e defaults (2.2.0 `i264e_param_default`, offsets in the i264e parameter block): mode +0xdc 1 (CBR), FIXQP
I/P +0xe0/+0xe4 35/35, QP +0xec/+0xf0 15..45, initial QP +0xf8 35, I QP +0x100/+0xfc 15..45, bit rate +0x110 2000
kbit/s, change pos +0x134 80, static time +0x130 2, still rate / still QP +0x160/+0x164 25/35, CVBR long max/min
+0x178/+0x17c 1600/1200 kbit/s, short/long stat time +0x180/+0x184 2/60, max I/P picture +0x158/+0x15c 2400/1440 kbit.

## QEMU (`-M ingenic-t32`, no RC statistics / motion search in the HERA model)

Runs `run-oi-cbr264` / `run-oi-cbr265` (OpenIMP, CBR 1000 kbit/s, 1920x1080, GOP 15, 30 pictures): the
picture QP moves with the coded size (36 38 37 36 34 ... 15), EFE_FRM_QP of every command list equals the slice
QP, both streams decode with ffmpeg without errors and show the colour bars. Device proof pending (no T32 camera).
