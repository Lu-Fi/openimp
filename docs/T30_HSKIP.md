# T30: IMP_Encoder_SetChnHSkip and BlackEnhance (issue #24)

The T30 1.0.5 libimp carries the i264e of the T10/T20 3.12.0 generation (same code, other offsets). The picture
decisions of the skip structures (`i264e_init_skip_header`, `i264e_decide_slice_type_and_rd`) and the
`SetChnHSkipBlackEnhance` slice data (`i264e_icodec_black`) equal `tests/hskip` byte for byte
(`tools/hskip_oracle.py` on the T30 library, layout `LAY30`). What a decoder sees is described in
[T20_HSKIP.md](T20_HSKIP.md); this page lists only what differs on T30 and how OpenIMP wires it.

## What the T30 1.0.5 library does

* `IMP_Encoder_CreateChn` (`channel_i264e_encoder_init`): `rcAttr.attrHSkip.hSkipAttr` goes to i264e parameters
  [0x28..0x2d] (type, m, n, maxSameSceneCnt, bEnableScenecut, bBlackEnhance). i264e parameter [0] is 1, 2 or 3 by
  `get_cpu_id` (T10 / T20 / T30): on every T30 it is 3.
* `i264e_validate_parameters` (0x48d04 in the uclibc 4.7.2 build, run by `i264e_init` and by every
  `i264e_idr_reconfig`): when the rate-control method (parameter [0x2f]) is not 3 (SMART) it clears the six words
  (memset 24 bytes): FIXQP, CBR and VBR channels code the plain GOP whatever the skip attributes are. In SMART the type
  is clipped to 0..6 and m, n follow the type (0/5/6: gop-1, 1; 1: 1, 1; 2: 1, 2; 3/4: 1, gop-1; the T21 table),
  maxSameSceneCnt 0..100. Device proof T30X b63 (CBR): vendor streams of the types 6, 2, 0 (CreateChn) and 5 (run-time
  SetChnHSkip) are plain IPPP, IDR every GOP, no long-term IDR.
* `IMP_Encoder_GetChnHSkip` (0xb39c0): i264e/i265e `get_param(9)` = `i264e_reconfig_hskip_get`, the run-time staging
  words E+0x91608..0x9161c, not the CreateChn attribute: after CreateChn the validated words (b63 CBR: type 2/0/6 read back 0; b64 SMART: type 0
  same 3 -> 0/29/1/3, type 2 -> 2/1/2, type 6 black 1 -> 6/29/1/0 black 1, GOP 30), the
  six words after SetChnHSkip (b63 hs5rt: 5/9/1/2), the black word after SetChnHSkipBlackEnhance (b63 hs6b: black 1).
  A JPEG channel answers zero.
* `IMP_Encoder_SetChnHSkip`: skipType up to the channel's maxHSkipType; an H.264 channel goes to
  `i264e_set_param(9)` -> `i264e_reconfig_hskip_set` (the six words wait, E+0x913b4 bit 0x200), and
  `i264e_idr_reconfig` takes them over with the next IDR picture: new skip header, `i264e_ratecontrol_init`
  again, `i264e_decide_slice_type_and_rd` again. No extra IDR. An H.265 channel goes to `i265e_set_param(9)`.
* `IMP_Encoder_SetChnHSkipBlackEnhance`: `i264e_set_param(10)` -> `i264e_reconfig_black_enhance_set` (bit 0x400 of
  E+0x913b8), applied by `i264e_reconfig` with the next picture, no controller restart.
* Rate controller (`i264e_ratecontrol_start`, branch [0] = 3, `JZ_VPU_RC_FRAME_START_T30`): the GOP-start flag
  E+0x904f4 is `counter % gop == 0`, also for a non-IDR picture of an IDR period of several GOPs; E+0x904f0 gets
  the frame type (OpenIMP's controller port does not read it). The HSkip class byte of T10/T20 is the [0] = 1
  branch only: not on T30.
* Scene-cut IDR of `i264e_decide_slice_type_and_rd` ([0] in 2..3 and E+0x90428 == 4): nothing in the T30 library
  writes E+0x90428 (it is the T20 controller's scene class), so it never fires on T30.
* `i264e_decide_slice_qp` returns at once for [0] > 2: no "base picture at the QP of the last IDR" rule on T30.
* `bEnableScenecut` (`i264e_decide_reset_slice_to_idr`, called by `i264e_slice_write` after the first try of a
  picture, before the re-encode judge): an LBASE / SBASE picture of an H1M type at a whole second that is larger
  than the last IDR picture (E+0x906b8, bits, written by `i264e_ratecontrol_end` for every I slice) is coded again
  as an IDR picture: `i264e_ratecontrol_init` from this try's QP, the structure restarts. The "not below the first
  try's QP" rule is [0] < 3: not on T30.
* No SetChangeRef second try with another reference on T30 (`i264e_ratecontrol_is_reenc`, branch [0] = 3, clears
  the flag).

## OpenIMP

* `src/t30/t30_helix_encoder.c`: `hs_attr` runs the T30 `i264e_validate_parameters` (`HELIX_T30_HS_VALIDATE`,
  `hskip_t21_validate`): the structure runs in SMART only. `src/t40/openimp_p2_encoder.c`: GetChnHSkip returns the
  staging words (`hs_staged`); after CreateChn these are the attribute as `i264e_validate_parameters` left it
  (`p2_t30_hs_created`: zero outside SMART, SMART m/n by type, maxSameSceneCnt 0..100), measured on the T30X in b63
  (CBR) and b64 run 2 (SMART). H.265: not measured, zero after CreateChn.
* `src/t30/t30_helix_encoder.c`: `HELIX_HSKIP` is on for T30 (third reconstruction buffer, reference / reconstruction
  buffer per picture, SPS with two reference frames and frame_num gaps, IDR `long_term_reference_flag`, reference
  list modification and MMCO, black slices without a VPU run), with the T30 rules above: gop-start flag to
  `EPRC30_FrameStart`, no scene IDR, scenecut before the eprc re-encode judge, controller restart at the IDR that
  takes a new skip header. `src/t40/openimp_p2_encoder.c`: CreateChn, SetChnHSkip and SetChnHSkipBlackEnhance reach
  the encoder on T30 as on T10/T20.
* Proof: `tests/t30/helix_encoder_test` (`test_t30_hskip`: SMART, 7 types x 4 attribute sets x black on/off x eprc
  on/off = 112 streams of 48 pictures against `tests/hskip/hskip_vectors_t30smart.txt` (`tools/hskip_oracle.py
  --smart30` on the T30 library), FIXQP and CBR plain GOP for every type (28 streams), run-time change, black
  enhancement at run time, scenecut with and without eprc), `tests/t30/p2_t30_enc_test`; QEMU `-M ingenic-t30` apitest case `hskip`.

## Not done

* H.265 (Radix): the vendor `I265eDecTypeAndRD::decTypeAndRef` has its own skip logic (asserts skip type <= 1:
  N1X with an IDR period of maxSameSceneCnt GOPs, N2X with `getWeakLightScene`). OpenIMP stores the six words only
  for an H.265 channel.
* `i264e_reconfig_hskip_set` also refuses a type above N2X when the encoder was made with two buffers
  (E+0x90d08 == 2); the IMP maxHSkipType check in front covers the same case.
* Device proof of this fix on the T30X: CBR rerun (expect plain IPPP = vendor) and a SMART run (vendor vs open structures); b63 ran CBR with the earlier branch state only.
