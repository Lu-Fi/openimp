# T32 rate control on HERA (#295, in development)

T32 support is in development (QEMU only, no T32 camera yet); this is not a release gate.

## What runs

- The T32 API (vendor 2.x, PRJ007) has the T23-family rate-control attributes plus CVBR, AVBR and QVBR
  (`include/imp/imp_encoder.h`, `IMPEncoderAttrRcMode`, 64 bytes). FIXQP keeps the fixed I/P QPs.
- H.264: `src/t32/openimp_t32_rc.c` puts the attributes into an i264e parameter block and runs the port of the
  vendor's T33 revision (`src/eprc/eprc_t33_h264.c`) behind `src/t32/openimp_t32_rc33.c`, a reimplementation of the
  vendor i264e side (`i264e_param_default`, `i264e_ratecontrol_init` / `_start` / `_end` / `_is_reenc`) on the
  controller's address arena (`src/eprc/eprc_t33_mem.h`). H.265 runs the H.265 build of the T33 revision
  (`src/eprc/eprc_t33_h265.c`) behind the i265e side of the same file (`t32_rc65_*`: `i265e_param_default`,
  the words `channel_i265e_encoder_init` sets, the `I265e_configure` clamps, `I265eRatectrl_rcInit` / `_rcStart` /
  `_rcEnd`). QVBR maps onto `src/eprc` (the T23 revision of the vendor "eprc" controller, bit-exact with the
  T23 OEM code, docs/T23_EPRC.md). Per picture FRAME_START gives the QP, which goes into the HERA command list (EFE_FRM_QP, the
  CABAC tables and the QP-banded analysis defaults follow it) and the slice header; after the picture the coded
  slice size and the engine statistics go to FRAME_END.
- Statistics: vendor 2.2.0 reads 48 registers after each H.264 picture; the first 25 are the Helix registers the
  T23 controller reads (0x13200000 + 0x80120..0x8014c, 0x500e8.., 0x80080, 0x40094..0x400a0, 0x800e0..0x800ec,
  0x80168: HERA keeps them at the Helix offsets), and the vendor `i264e_ratecontrol_end` folds them the same way
  (QP sums `lo16 + hi16`, SSE pairs masked to 27 bits). H.265 reads its own list of 38 registers
  (`hera5_stat_regs`); `I265eRatectrl_rcEnd` folds entries 0..8, 29, 30, 32, 33 (revision 6) and, after an I
  picture, the 64-line row sizes read as for H.264.
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
- H.265 attribute placement (T33), checked against the vendor 2.2.0 IMP layer (`channel_i265e_encoder_init` +
  `I265e_configure`, `tools/eprc_t33_h265_oracle.py --params`, `tests/t32/rc65_test.c`, 400 random attributes,
  every word the rate control reads equal): mode +0x48, QP bytes maxQp/minQp/blkQpEn/initialQp/maxIQp/minIQp ->
  +0xac/+0xb4/+0x58/+0x5c/+0xb0/+0xb8 (zero-extended: an initial QP of -1 is 255, clamped to 51), bit rate +0x60,
  iBiasLvl +0xbc, CBR IP/PPfrmQPDelta +0x80/+0x84 and flucLvl +0xc4, changePos +0xcc (not CBR), staticTime +0xc0,
  qualityLvl +0xd0, maxIprop/minIprop +0xd4/+0xd8, maxI/PPictureSize +0xdc/+0xe0 (I 0 = 1.5 x rate), SMART / AVBR
  minStillRate/maxStillQp +0xe4/+0xe8, super-SMART +0xec..+0xf8, CVBR long max/min +0xfc/+0x100, short/long stat
  time +0x104/+0x108, extraBitRate +0x10c, FIXQP I/P/B QP +0x4c/+0x50/+0x54, the skip attribute +0x1ac..+0x1c4.
  `I265e_configure` lifts the keyint to the frame rate, clamps the bit rate between the frame rate and 12/6
  (12/10 above 800 pixels wide) bits per pixel and picture and caps the VBV sizes (+0x1cc..+0x1d4, 1 / 1.4 / 1.3
  of that) below any stream buffer. The vendor's `cnst_pmd_qp_thrd` .data words the H.265 controller patches
  (0x2014b4 / 0x2014b6) are region 2 of the arena, one copy per channel (vendor: one per process).
- QVBR mode mapping (T23 revision): CBR -> CBR (outBitRate, iBiasLvl, staticTime, qualityLvl); VBR -> VBR
  (maxBitRate, changePos, staticTime, qualityLvl); SMART -> SMART; CVBR -> VBR on maxBitRate and shortStatTime (the
  long-term bounds are not modelled); AVBR -> SMART; QVBR (PRJ009/PRJ010 only in the vendor header) -> VBR at
  targetBitrate. QPs clamped to 0..51, VBR rates floored at 128 kbit/s, frame / GOP QP steps 3 / 15. maxIQp /
  minIQp bound the IDR QP.
- H.265 and QVBR: a rate-control change (`IMP_Encoder_SetChnAttrRcMode`) restarts the controller at the next IDR
  (H.265: a new `I265eRatectrl_rcInit` on the new attribute; the vendor's i265e reconfiguration path is not
  ported).
  QVBR: vendor 2.2.0 refuses it on PRJ007 (SetDefaultParam / CreateChn / SetChnAttrRcMode take modes 0..5 only);
  OpenIMP keeps running it as VBR (beyond vendor, unchanged).
- After an IDR the H.264 controller gets the coded size of every 64-line row from HERA, read the vendor way
  (`i264e_ratecontrol_is_reenc`, revision 6: write 0x13240804 + 8 i to 0x13240044, read 0x13240804 + 8 i); its sum
  feeds the IDR slice targets. Checked on the model (`vendor-2.2.0-rc33-rows-h264.txt`, rc33_test -r: QPs and the
  controller's sum P+0x6bc equal); what HERA really returns there needs a device (the QEMU model has no such data).
- H.264 slice block: the T33 controller fills the vendor's slice block at every FRAME_START (`VPI_set_sliceinfo`
  into the encoder's rc memory H+0xf9c, which `hwicodec_pf_h264e_t32v_enc` copies and `H264E_T32V_SliceInit`
  packs into the HERA command list). OpenIMP's list takes from it what the vendor packs from the controller's
  fields (revision 6): EFE_FRM_QP 0x2001c (QP, MB QP window s+0x826 / s+0x825), 0x2029c (MB QP clamp s+0x76e /
  s+0x76f, map enables), the row-budget block 0x40100..0x40124 and, right after it, one 0x40900 + 4 i word per
  64-line row (s+0x568, s+0x530 rows: 17 at 1088 lines, so the lists grow to 753 / 930 pairs as the vendor's).
  The registers were found by running `H264E_T32V_SliceInit` under unicorn on the controller's block against
  a FIXQP block at the same QP (every other word equal), the bytes each one reads by flipping every byte.
  The per-MB maps the block points at (SMART map s+0x24c -> EMC 0x90028, MB QP / flag map s+0x250 -> 0x90030,
  ROI map s+0x254 -> 0x90050) are copied into the buffers the list names when the controller uses them (SMART
  still map, ROI map, intra-refresh / forced-intra flags); no OpenIMP T32 API turns those on yet, so they stay
  zero as on the vendor without them. Checked: `tools/eprc_t33_oracle.py --slice` writes, after each picture's
  'F' line, the words of the vendor `H264E_T32V_SliceInit` on the vendor controller's block
  (`vendor-2.2.0-rc33-slice-h264.txt.gz`, 40 scenarios), `rc33_test -l` requires OpenIMP's list (built from the
  port's block) to carry the same words in the same place for every picture.
- H.264 run-time setters as vendor 2.2.0: `IMP_Encoder_SetChnBitRate` (target in CBR, else the max rate),
  `_SetChnQpBounds`, `_SetChnQpBoundsPerFrame` (I bounds; at FIXQP the fixed I / P QP = max I / max P, the vendor
  takes no P bounds otherwise) and `_SetChnMaxPictureSize` check their arguments with the vendor's codes
  (0x80040001 channel, 0x80040002 argument, 0x80040008 no channel, 0x80040080 FIXQP / JPEG), update the channel
  attribute as the vendor does and go, as `i264e_set_param` 0x11..0x14 (`i264e_reconfig_*_set`), into the
  reconfiguration copy and a pending set for the next IDR. There `i264e_encode` runs `i264e_ratecontrol_start`,
  then `i264e_idr_reconfig`: the changes into the parameter block, `i264e_validate_parameters` and, as these four
  need no `i264e_ratecontrol_init`, a second `i264e_ratecontrol_start` for the IDR; OpenIMP does the same
  (`t32_rc33_start`; with a SetChnAttrRcMode at the same IDR the changes go in after its copy). H.265 has no
  setters yet (the vendor `I265eReconfig_*` path is not ported). Checked: `tools/eprc_t33_oracle.py --setters`
  calls the vendor `i264e_set_param` at random pictures (390 changes in 40 scenarios, QPs up to 60, rates from 1)
  and lets the vendor `i264e_idr_reconfig` take them; `rc33_test` on `vendor-2.2.0-rc33-setters-h264.txt.gz`:
  the QP of every picture equal (without the second start 36 of the 40 runs differ). QEMU apitest (`enc` area): SetChnBitRate reads back the new rate at once (vendor 2.2.0 still the old one until the IDR; it writes the attribute, its getter reads elsewhere), SetChnQpBoundsPerFrame 0, SetChnMaxPictureSize the same picture-size FAIL as the vendor (no re-encoding in either).
- `IMP_Encoder_SetChnMaxPictureSize` is a picture-size target, not a cap (#295). Vendor 2.2.0:
  `i264e_reconfig_maxpicsize_set` (0x717ec) stores I / P at +0x2bc4 / +0x2bc8 and sets flag 0x100000 (0x71838..0x71854);
  `i264e_idr_reconfig` takes them at the next IDR; `JZ_VPU_RC_FRAME_START_H264_T33` reads them at e+0x85c / +0x860
  (0x19b1b4 / 0x19b1bc) into the encoder's size target (output +0x18, flag +0x24) for the HERA row budgets;
  `JZ_VPU_RC_FRAME_REPEATE_JUDGE_H264_T33` (0x19d44c) returns 0, so no picture is encoded again. H.265 goes through
  `I265eReconfig_setMaxPicSize` (0x87518) the same way. Device A/B on the PRJ007 module (apitest `enc`, vendor lib
  from /tmp with `APITEST_NOPOOL=1`, same scene): with half the biggest picture as the limit the vendor's biggest
  picture after the change was 2.2x / 2.6x (H.264) and 2.8x / 2.0x (H.265) of the limit, in 3 of the 4 cases larger than the
  biggest one before it; OpenIMP 2.0x / 1.4x (H.264) and 1.2x / 1.1x (H.265). The apitest checks on T32 that both
  calls are accepted and the stream keeps flowing and reports the sizes; T33 (2.0.2.1) keeps the limit +15 % check
  (#706: vendor 5751 B for 5250 B on the T33L).
- H.265 QP check: `tools/eprc_t33_h265_oracle.py --scenarios` / `--rows` runs the vendor i265e glue and controller
  under unicorn on the synthetic size and statistics model; `rc65_test` on `vendor-2.2.0-rc65-h265.txt.gz`
  (164 scenarios, modes 1..5, 640x360 .. 2560x1440, 21740 pictures, 40 with per-row IDR sizes; the harness sets
  the per-picture motion mode 2 of `I265eFrame_recvPic`, F+0x22ec, as the vendor does, #719): the picture type
  and QP of every picture and the row-size sum (rc+0x224) equal, 0 accesses outside the arena. QEMU (640x360,
  CBR / VBR 1000 kbit/s, 30 pictures): decodes clean, colour bars intact, EFE_FRM_QP == slice QP for every
  picture and equal to the vendor controller replayed on the run's slice sizes (the QEMU HERA returns zero
  statistics, so the QP falls to the minimum 15 after the I picture at 45 on that rate).
- Not done: re-encoding of oversized pictures (FRAME_REPEATE_JUDGE skipped), the H.265 run-time setters and
  per-picture flags (ROI, still map, refresh: the i265e picture block fields stay 0), the frame-rate
  and GOP changes on the reconfiguration copy (`i264e_reconfig_fps_set` / `_gop_set`, which need
  `i264e_ratecontrol_init`), IVDC channels.

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

Run-time read-back (2026-11-12): vendor `IMP_Encoder_GetChnAttrRcMode` reads the encoder's live parameters
(`i264e_get_param` 3 = `i264e_reconfig_rc_get`, +0xdc..); `i264e_reconfig_bitrate_set` (and the QP / size /
rate-control setters) only fill the reconfiguration copy, which `i264e_idr_reconfig` takes at the next IDR. OpenIMP
shows the attribute as it was until that IDR (`rb_hold`). QEMU apitest, vendor and OpenIMP alike: `SetChnBitRate`
400 reads 512 before the next IDR and 400 after it.

Frame rate and GOP: the vendor IMP glue `encoder_get_fps_mask` (before every picture, H.264 and H.265) sends the
rate (`i264e_set_param` 5, reduced, at most the source rate) and the GOP (7) and makes the next picture an IDR
(channel +0x14c). A GOP asked for (`SetGOPSize`, and the created one before the first picture) becomes
ceil(den * gop / num) * num / den; a new rate keeps the GOP in pictures unless a GOP was asked for within the last
two pictures (then the whole seconds of the running GOP times the new rate). `i264e_idr_reconfig` takes both at the
IDR (fps +0x2a4/+0x2a8, keyint +0x2c, parameter sets) and runs `i264e_ratecontrol_init`. `GetChnFrmRate` /
`GetGOPSize` read what was sent (`i264e_reconfig_fps_get` / `_gop_get`). `src/t32/t32_gop33.h` is that glue;
`tools/eprc_t33_gop_oracle.py` runs the vendor's under unicorn (CreateChn, then random `SetChnFrmRate` /
`SetGOPSize` between pictures), `tests/t32/gop33_test.c`: 300 scenarios, 26403 pictures, 5128 sends equal. Under one
second the vendor sends GOP 0 and `i264e_ratecontrol_start` divides by it (trap); OpenIMP keeps the GOP.

## QEMU (`-M ingenic-t32`, no RC statistics / motion search in the HERA model)

Runs `run-oi-cbr264` / `run-oi-cbr265` (OpenIMP, CBR 1000 kbit/s, 1920x1080, GOP 15, 30 pictures): the
picture QP moves with the coded size (36 38 37 36 34 ... 15), EFE_FRM_QP of every command list equals the slice
QP, both streams decode with ffmpeg without errors and show the colour bars. Device proof pending (no T32 camera).

T33 revision (H.264, 2026-11-12): the same CBR run gives the picture QPs 34 31 30 29 ... 18, then 15 from the second
GOP on (sizes ~200 bytes, far below the budget), the sequence the vendor controller gives under unicorn for the same
sizes; H.264 and H.265 decode with ffmpeg without errors.

After the IMP-layer check and with the slice block in the list (2026-11-12, `run-oi-cbr264` against the same run of
vendor 2.2.0, `SetDefaultParam` defaults, initial QP -1): picture QPs 45 42 39 36 33 30 29 .. 21, 23 (IDR), 20 .. 15,
equal to the vendor's QEMU run and to the vendor controller under unicorn on OpenIMP's coded sizes; all 30 command
lists (753 / 930 pairs, 27546 in all) equal the vendor's but for the buffer addresses; decode clean.

## T33 request timing: RequestIDR / RequestGDR / RequestPskip on H.265 (#791)

Vendor 2.0.2.1 `i265e_encode` (0x90d48 in Ghidra, file offset 0x80d48) takes the picture (`recvPic`), decides its
type with the pending requests (`decTypeAndRef`) and only then waits for the previous picture's work
(`waitList`, timeout 10000) before it hands the picture to the encode worker (`workReceive` 0xdf118) and returns.
`i264e_encode` (0x8a71c) runs the picture to the end (`i264e_threadpool_run` / `_wait_all`) before it returns.
So on H.265 the vendor decides picture N+1 as soon as its frame arrives, also while picture N is still being
encoded; on H.264 only after picture N is done. OpenIMP decides each picture when the encoder thread starts it
(after the previous one is done), for both codecs.

The difference shows only when one picture takes longer than the frame interval. QEMU at 1920x1088 (HERA model,
15 fps): a RequestIDR made after stream 9 gives the vendor IDR at stream 11, OpenIMP at stream 10 (v5g2 / o5g2);
RequestGDR at 20 gives the vendor masks in pictures 21-25, OpenIMP 20-24; RequestPskip at 8 gives the vendor skipped
picture 9. At 640x368 (one picture well inside the frame interval; on silicon a 1080p picture should also fit, not
measured: no T33 camera at hand) the vendor acts on the same picture as OpenIMP: IDR at 10, masks 20-24 (QEMU v5s / o5s, `T32_ENC_W=640
T32_ENC_H=368`), skipped picture 8 (v5pss). OpenIMP keeps the synchronous path: the request rule is the vendor's
(the first picture whose type is decided after the call), only the overlap of two pictures is not modelled.

## H.264 source padding rows (#882, 2026-11-25)

HERA H.264 reads the source down to the 16-aligned height (360 -> 368). Vendor 2.2.0 hands it the capture buffer with
the last picture row repeated into those rows (luma and chroma); the capture driver does not write them. Without the
copy the rows hold stale memory that differs per capture buffer and every P picture codes the last macroblock row
again (QP 40: 411 B instead of 38 B a P picture at 640x360). OpenIMP `enc_pad_rows()` (openimp_t32_encoder.c) does the
same copy before the list is built; command lists were already equal to the vendor's (device tracer
`audit/issue882/tools/heratrace.c`: lists, maps, WOR_VPU_REG, REQBUFS, frame format, padding rows at DQBUF / RUN).

## Device A/B against the vendor 2.2.0 library (#295, 2026-11-25)

PRJ007 module (gc4653, ko 072794b4, sensor port a6f424e9), the same camera and kernel for both libraries. Tool:
`rc32` (one encoder channel from FrameSource 0 = 2560x1440 or 1 = 640x360 at 25 fps, packet sizes and times to a CSV,
the Annex B stream to the host; the bit rate is measured on the stream bytes). Vendor = libimp 2.2.0 with libalog +
libsysutils preloaded, OpenIMP = `build/t32` of next. QP per picture from `ffmpeg -bsf trace_headers` (slice QP),
MB QPs from `-debug qp`, decode with ffmpeg (0 errors in every run), picture quality as frame PSNR / SSIM.
Static scene; the scene brightness drifted over hours, so every comparison that mattered was repeated as
interleaved pairs (OpenIMP run, vendor run, 40 s each, minutes apart). Bit rate in kbit/s, vendor / OpenIMP:

| mode | H.264 main | H.264 sub | H.265 main | H.265 sub | max 1 s window, H.264 main | QP mean I / P, H.264 main |
|---|---|---|---|---|---|---|
| FixQP 32 | 3979 / 3967 | 87 / 87 | 2984 / 2848 | 63 / 62 | 20528 / 20170 | 32.0 / 32.0 in both |
| CBR 4000 / 500 | 4055 / 4067 | 509 / 507 | 4052 / 4046 | 449 / 447 | 7107 / 6987 | 29.6 / 32.3 vs 29.4 / 32.0 |
| VBR | 3548 / 3560 | 427 / 426 | 3821 / 3816 | 434 / 429 | 5109 / 5316 | 29.8 / 32.3 in both |
| SMART | 1230 / 1221 | 298 / 299 | 1134 / 1145 | 311 / 309 | 4830 / 4716 | 33.5 / 36.1 vs 33.5 / 36.0 |
| CVBR | 3278 / 3273 | 410 / 408 | 3285 / 3290 | 408 / 406 | 5402 / 5166 | 30.4 / 32.7 vs 30.4 / 32.6 |
| AVBR | 1135 / 1131 | 301 / 300 | 1235 / 1221 | 314 / 308 | 4806 / 4855 | 33.3 / 36.0 vs 33.3 / 36.1 |

The first picture of every rate-controlled run is coded at QP 45 (initial QP) in both libraries. Parameters (H.264 main,
CBR/VBR 4000 kbit/s): QP window 34..38, minQp 40, maxQp 30, IDR window 38..42, IPfrmQPDelta 0 / 10, changePos 50 / 100,
iBiasLvl -8 / +8, qualityLvl 0 / 7, run-time bit-rate change: same QP range and bit rate in both (details in the
matrix row 27); staticTime 1 / 30 and the other fields are not distinguished by a static scene.

Difference found: FixQP H.264 IDRs after the first one were coded with the QP..QP window (flat MB QP): IDR 67 kB
against 160 kB in the vendor, which keeps the QPG AI deltas (MB QP 22..35 around the slice QP 32) in every picture except
the first of the stream (`first` flag as in the H.265 list). Fixed in `openimp_t32_hera.c` (EFE_FRM_QP), #906: IDR 157 / 157 kB.
Raw data: audit/issue295/rc (not published).

## T32 setters against the vendor 2.2.0 library in QEMU (2026-11-28, #99, #975-#979)

Method: `-M ingenic-t32`, `tools/t32/t32_trace_sample.c` (H.264 / H.265, 640x368, `T32_ENC_QP`) with the HERA capture of QEMU (`-global ingenic-hera.capture`), vendor libimp 2.2.0 and OpenIMP in the same guest image, one setter per three pictures (`T32_ENC_CHANGES`), every picture's register list compared (the input-statistics registers are skipped, as in the T33 comparison #784). FIXQP: equal for SetChnColor2Grey, SetChnHSkip, SetChangeRef, SetChnDenoise, SetMbRC, SetChnHSkipBlackEnhance, SetSrdCfg, SetChnCrop, SetH264TransCfg, SetSuperFrameCfg, SetGDRCfg, RequestGDR (also CBR since the 0x30048 refresh rule), SetChnRcTrigLevel, SetPskipCfg, SetMotionQuality*, H.265 SetChnCrop / SetH265TransCfg; every return code equal.

Vendor behaviour found and now followed on the T32 (CBR, QP 45 42 39 ... sequence equal): SetChnCrop, SetH264TransCfg, SetChnColor2Grey and SetChnHSkip restart the controller at the next IDR (`T32_E1RC_RESTART`); QpgMode / RcTrigLevel / SuperFrameCfg reach the controller (group 2 of #784, no longer T33 only); crop and the chroma QP offsets are applied from the next IDR (SPS frame cropping / conformance window, PPS offsets, H.265 HERA words 0x38004 / 0x4001c / 0x48004 / 0x50004, cb = cr = the larger offset); a refresh or skipped picture clears bit 23 of MCE 0x30048 and a skipped one writes no 0x30048 (revision 6 controller state s+0x2c0 / 0x2c4 as in revision 7).

Fixed since (issue #977, 2026-11-28): vendor 2.2.0 `i264e_encode` (0x87be0) calls `i264e_ratecontrol_start` (0x88f0c) in every mode (its only test is the HW revision P+4 in 4..7), so the T32 FIXQP channel keeps the controller for H.264 and H.265 now (T33 unchanged). With it RequestPskip (H.264 MCE 0x30048, H.265 the skipped picture's EFE / MCE / MD bits and the 0x40400 refresh masks from s+0x430) and SetQpgMode(ROWRC) + SetChnRcTrigLevel (row-RC words 0x40100.., 0x2029c) are equal at FIXQP; the H.265 EFE_FRM_QP word comes from the slice block (s+0x1c2 / 0x1c3 / 0x1c1 / 0xbf9, H265E_T32V_SliceInit), so the IDR after a restart (SetChnCrop, SetH265TransCfg, Color2Grey, SuperFrameCfg) carries the vendor's QP..QP window. SetChnMapRoi (H.264 `i264e_reconfig_roi_set` 0x7fae0 kind 1 -> P+0x250..0x258, flag 4 after the start; H.265 `I265eReconfig_setRoi` 0x959ac -> i265e+0x1a0..0x1a8 before rcStart; DEVIOUS maps through `IMP_Encoder_MappingTransform` 0xf5d9c in the caller's map; JPEG 0x80040002) and SetChnQpgAI (H.264 `i264e_reconfig_qpgai_set` 0x81874 -> P+0x260..0x26d, P+0x25c = SetChnQpgAIEnable from `channel_i264e_encoder_init` 0xe56cc; H.265 `I265eReconfig_setQpgAi` 0x975a4 -> i265e+0x2d8..0x2e5, +0x2d4 the enable, cleared after its picture) feed the controller's maps; the EFE 0x20038, EMC 0x90000 and H.265 EFE_MBQP_CFG bits are taken from the slice block as the SliceInit functions do. Result (640x368, FIXQP 110-picture sequence of all 32 setters for H.264 and H.265, CBR single setters, QpgAI at FIXQP and CBR): every HERA list equal to vendor 2.2.0 in every non-address word. Timing only: the IDR that RequestIDR asks for lands one picture later in some runs (both libraries, more often OpenIMP: the encode thread has started the next picture), and concurrent QEMU runs shift it as well (the "ROI one picture early" of #982 was such a run; isolated runs: vendor and OpenIMP both from picture 11). Raw runs: audit issue977 (not published); QEMU only, no T32 camera run.
