# T30 H.265 (HEVC): Radix encoder (#208, closed)

Status 2026-11-06 (device-proven in b41, see the b41 results below): `IMP_Encoder_CreateChn(PT_H265)` on T30 encodes on Radix by default; `OPENIMP_T30_H265=0` turns it off (CreateChn returns -1 as before). The libimp exports `int OpenIMP_Cap_H265` (1 = H.265 available, cleared by a constructor when the env is 0) for streamers; the channel encodes on Radix (`src/t30/radix/t30_radix_encoder.c`). Proven in the QEMU T30 machine
with synthetic pictures; on a T30X (run b40) Radix runs, VPS/SPS/PPS are byte-equal to the vendor and IDR pictures are
correct; the P-picture drift seen there had two causes that are fixed since (below); the re-run b41 on the T30X lab camera proved them (vendor-equal CBR within 0.1-2.4 %), and the live rate-control change on H.265 is vendor-equal since #228 (closed, b49/b51). This page
describes the hardware path and what of it is in the tree. Only the real T30 build contains it (not T20/T21/T23).

## Hardware and kernel

The T30 encodes H.265 with **Radix**, a block separate from Helix (H.264/JPEG), at 0x13600000 with its own interrupt.
Its kernel driver is GPL and built into every thingino T30 kernel (`CONFIG_VPU_RADIX=y` in
`board/ingenic/xburst1/kernel/3.10.14/t30.generic.config`; `drivers/video/soc_vpu/radix/radix.c`). It is reached through the
same `/dev/soc_vpu` channel ioctls as Helix: request with codec direction `HWH265ENC` (0x10), run with the command list
address, completion returns the slice-data length (`SDE_SLBL`). No kernel change is needed.

## Split between library and hardware

Like the vendor libimp 1.0.5 (an x265-derived encoder, "i265e"):

- the library writes VPS, SPS, PPS, SEI and the slice header, and inserts the emulation prevention bytes;
- Radix writes the slice data (CABAC) into the bitstream buffer;
- per picture the library builds a command list of (value, `0x80000000 | register`) pairs from a 0x1204-byte parameter block
  (source/reconstruction/reference addresses, size, QP, CABAC context states, mode-decision, motion-search, loop-filter, ROI,
  QP-generator and rate-control fields) and starts it.

## In the tree

| file | what |
|---|---|
| `src/t30/radix/t30_radix_sliceinfo.h` | the parameter block: field names and offsets as printed by the vendor debug dumper |
| `src/t30/radix/t30_radix_cmdlist.c/.h` | the command list builder (GPL-2.0, register map from the kernel's `radix.h`) |
| `src/t30/radix/t30_radix_intp.h` | HEVC luma/chroma interpolation formats, generated from `radix.h` by `tools/gen_radix_intp.py` |
| `tests/t30/radix/radix_cmdlist_test.c` | host test (`make -C tests/t30 check`): 64 parameter blocks must give the vendor's lists |
| `src/t30/radix/t30_radix_cabac.c/.h` | the 156 initial CABAC context states for slice type and QP (HEVC initValue tables, vendor context order) |
| `src/t30/radix/t30_radix_hevc.c/.h` | VPS, SPS (with VUI/HRD), PPS and slice header RBSP writers and the NAL framing (start code, emulation prevention) |
| `tests/t30/radix/radix_cabac_test.c`, `radix_hevc_test.c` | host tests against vendor hashes (`make -C tests/t30 check`) |
| `tests/t30/radix/hevc_diff.c` | desk tool: CABAC, header writers and NAL framing against the vendor functions under qemu-mipsel |
| `src/t30/radix/t30_radix_encoder.c/.h` | the encoder object: soc_vpu channel (codec direction 0x10, vpu_id 0x4000001), command list, NV12 reconstruction/reference pair with the vendor's 16-sample border, bitstream window, `EPRC30_*` (codec 1) for QP, parameter block and QP map, the 19 statistics registers, re-encode, VPS/SPS/PPS + slice NAL as `sliceEncap` |
| `tests/t30/radix_encoder_test.c` | host test of the encoder object with a fake soc_vpu (access units, NAL types, emulation prevention, GOP, reference swap, failure path) |
| `t30_hevc_channel_params()` (`t30_radix_hevc.c`) | the VPS/SPS/PPS values the vendor announces for a channel (size, frame rate, RC mode, bit rate) |
| `tests/t30/radix/hevc_cfg_diff.c`, `radix_hevc_chan_test.c` | desk tool (vendor `IMP_Encoder_CreateChn`/`I265e` setup + `codeVPS/SPS/PPS` under qemu-mipsel, 5000 random channels, 0 differ) and host test (500 channels, vendor hash) |
| `tests/t30/radix/radix_diff.c` | desk tool: compares the builder with the vendor function under qemu-mipsel (`make -C tests/t30 radix-diff O=<thingino output> VENDOR_LIBIMP=<libimp.so> QEMU_MIPSEL=<qemu-mipsel>`) |

The builder gives the same list as the vendor library word for word (25 000 random blocks, I and P pictures).

## Still to do

The T30X device proof: re-run of b40 with the two fixes (P pictures decode clean over 300 frames, IDR/POC pattern,
bit rate against the vendor's 1080p25 CBR 2000 = 2034 kbit/s, VBR, 2048x1536 and 1280x720, H.265 next to H.264, channel
restarts). After it passes, the matrix cell becomes device-proven and `OPENIMP_T30_H265` can default to on.

## Device run b40 (T30X, 2026-11-06)

Radix on silicon: one interrupt per picture, no oops or timeout, 0 GetStream errors; VPS/SPS/PPS byte-identical to the
vendor in all four common configurations, first QPs equal (40 42 41 38 ...), IDR pictures show the same scene and colours
as the vendor's. P pictures drifted. Causes, both fixed after the run and covered by `radix_encoder_test`:
- the first P picture after an IDR carried POC 0 (the count before the increment; vendor `decTypeAndRef`: the picture
  count since the IDR);
- the reference chroma stride (block 0x34) was halved; the vendor `codec_enc` writes the reference picture's stride as
  is and halves only the reconstruction's (block 0x30). QEMU did not show it (its model ignores the strides).

## Run-time changes

As in the vendor library (Ghidra, libimp 1.0.5): `IMP_Encoder_SetChnAttrRcMode` has an H.265 branch (`i265e_set_param`
3, `I265eReconfig::setRc`), the frame-rate/GOP path (`encoder_get_fps_mask`, used by `SetChnFrmRate` / `SetGOPSize`)
sends `set_param` 5/7 and requests an IDR, and `I265e::reconfig` applies the latched values on an IDR only
(`ReconfigIdrFrame` -> `I265e::reset`: `configure`, levels, `initVPS/SPS/PPS`, `I265eRatectrl::rcInit` on the running
controller). OpenIMP: `OpenIMP_T30_RadixUpdateParams` (fed with a snapshot of the channel parameters before every
picture) latches the change, the next IDR rebuilds the parameter sets and calls `EPRC30_Reinit`, which runs the setup and
VIDEO_CFG again on the live controller without clearing it, as rcInit does. Proof: `tools/eprc_t30_oracle.py LIB
--h265-reconf` runs rcInit a second time on the vendor controller; `tests/eprc/eprc_t30_h265_reconf_vectors.txt`
(12 changes, 3520 decisions incl. block and QP-map CRC) 0 differ; a fresh controller instead gives 450 differences.
`IMP_Encoder_RequestIDR` works as before.
`IMP_Encoder_SetChnAttrRcMode` on an H.265 channel reads the H.265 attribute layout (CBR: `staticTime` before
`outBitRate`, `flucLvl` last; VBR/SMART: `flucLvl` in place of H.264's `gopRelation`) as the vendor branch does
(`src/t40/codec_rc_helix.h`, #228; before, a live CBR change took `staticTime` as the bit rate).

## ROI on H.265 (#404)

Vendor 1.0.5 (Ghidra, project t30lib): `IMP_Encoder_SetChnROI` 0xc1ec0 packs the region like H.264 (enable, relative, QP as bytes, the corners sorted
and divided by 16: 7 bytes) and calls `i265e_set_param` -> `I265eReconfig::setRoi` 0x6ff0c: index 0..15 (else -1), written to the
*pending* table, dirty bit 4.  `ReconfigPFrame` 0x6f6d8 (every picture) copies the pending table to the live one (i265e + 0xc0, 16 x 7 bytes) and
`codec_enc` 0x6cf4c copies the live table to the slice info (block 0x3b), which the command list programs (0x20044.., 0x2004c..).
`IMP_Encoder_GetChnROI` 0xc22e8 (index 0..7 else -1) reads the *live* table (`I265eReconfig::getRoi` 0x6fff0), so a read-back right after a Set is
all zero until a picture was encoded.  OpenIMP does the same (`h265_roi_pend` / `h265_roi` in codec-t40.c, `OpenIMP_T30_RadixSetRoi`; host tests
p2_t30_enc_test and radix_encoder_test).  A new channel starts with an empty table (before, the slot kept the region of an earlier H.264 channel).
timps: ROI is H.264 only and its tier on T30 is "unsupported" (`src/roi_caps.h`), prudynt sets no ROI.

## Not ported (vendor has it, no streamer on T30 reaches it)

- The eprc ROI offsets / Gaussian SAS map (E+148/149) and the other A+152 variants: debug switches only, 0 in every
  vendor T30 path (`eprc_default_set_T30`, rcInit does not set them).
raptor was not checked (UNVERIFIED).

## User data SEI and chroma QP offsets (#399)

Vendor libimp 1.0.5, Ghidra. `IMP_Encoder_InsertUserData` (0xc3ed4) copies the payload into a cache buffer of the channel's
user-data queue (H.265: the length word, then the bytes; H.264 puts the H.264 UUID in front instead). The encoder group thread
(`on_encoder_group_data_update`) drains the whole queue before every picture into `i265e` (entry: size, payload type 5, bytes);
`I265e::sliceEncap` writes, after VPS/SPS/PPS of an IDR and the HRD SEIs, per entry `SEI::write` (payload type 5, size
16 + length in 0xff steps, `SEIUserDataUnregistered::writeSEI`: the 16-byte `m_uuid_iso_iec_11578` = 2ca2de09-b517-47db-bb55-a4fe7fc2fc4e,
then the bytes), `writeByteAlignment`, `NALList::serialize` with NAL type 0x27 (prefix SEI), then the slice. OpenIMP:
`OpenIMP_T30_RadixSetUserData` (the queue of up to 2 payloads of 1024 bytes the channel attribute allows, handed over before each
picture) and `radix_user_sei`.

`IMP_Encoder_Set/GetH265TransCfg` (0xc5688 / 0xc585c) pass `{cb, cr}` to `i265e_set_param/get_param` 0xe: `I265eReconfig::setTrans`
stores the pair and flags 0x4000, `ReconfigIdrFrame` copies it to the i265e parameters and `I265e::reset` -> `initPPS` writes it
into the PPS (`pps_cb_qp_offset` / `pps_cr_qp_offset`) at the next IDR; `getTrans` reads the coded values (default -3 / -3,
`i265e_param_default`). No range check. OpenIMP: `OpenIMP_T30_RadixSetChromaQp/GetChromaQp`, applied on the next IDR (also
after a rate-control change); a Set before the first picture is applied when the encoder is created. The Radix command list
takes the cb offset as before (`RSI_FRAME_CQP_OFFSET`). Host test: `radix_encoder_test`.

## QEMU proof (2026-11-06)

`tests/t30/radix/radix_smoke.c` (`make -C tests/t30 radix-smoke O=<thingino output>`) feeds synthetic NV12 pictures (gradient,
moving box) from rmem to the encoder object on the real `/dev/soc_vpu` driver of the `qemu_t30x` image; the QEMU T30 machine's
Radix model replays the command list and encodes with the AVPU HEVC core.  640x360, 60 pictures each for CBR 500 kbit/s, FIXQP
30 and VBR: one radix interrupt per picture, no oops or timeout, IDR every 25 pictures; ffmpeg decodes all three streams without an
error message (HEVC Main, level 2.1), 60 of 60 distinct pictures each, and the decoded pictures show the synthetic pattern with the
box at its position.  The model does not check most registers, the QP generator or the statistics (they read 0), so this proves the
channel, buffers, command-list addresses, parameter sets, slice header, NAL framing and the rate-control loop, not silicon
behaviour.  The IMP path (`apitest enc` with `OPENIMP_T30_H265=1`) creates the H.265 channel (CreateChn 0, attributes read back)
but gets no pictures in QEMU: FrameSource `EnableChn` fails for every channel there (`set format failed`, H.264 likewise; the
QEMU T30 frame path to the encoder is not modelled, README-T30).

## Channel parameter sets

`IMP_Encoder_CreateChn(PT_H265)` in the vendor library fills `i265e_param_t` from the channel attributes (size, frame rate
reduced to lowest terms with 25/1 for 0, GOP, RC fields, HSkip type) and builds the `I265e`, whose `configure`,
`enforceLevel`, `determineLevel` and `initVPS/SPS/PPS` set the parameter sets.  With the i265e defaults only size, frame
rate and bit rate reach them: the size is clipped to 256..4096 x 16..2304 and padded to a multiple of 16 x 8 (the vendor
codes the padded picture, there is no conformance window), the VUI timing is the frame rate, level and tier follow the x265
level table with the bit rate in kbit/s (0 for FIXQP, the CBR target or VBR/SMART maximum otherwise, clipped by
`configure`), high tier allowed.  The VPS carries `vps_extension_flag` 1 and 16 bits built from two `random()` values unless
`/tmp/closeextvpflag` exists (`Entropy::codeVPS`).
