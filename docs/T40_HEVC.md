# T40 HEVC (H.265) on the AVPU

Status: device-tested on a T40XP (Eufy S350, SC830AI, vendor kernel driver), 2026-10-10.

## What blocked it

`IMP_Encoder_CreateChn` refused HEVC on every SoC except T31 and T41
(`openimp_p2_encoder.c`), and the codec only enabled the HEVC parts (codec
flag, VPS/SPS/PPS + slice header writer, HEVC EP1 lambda table, HEVC EP2
size, AVPU selection for codec 1) for T31/T41.  The T40 command list had no
HEVC variant.  There is no hardware or firmware difference: the vendor T40
libimp 1.3.1 uses the T31 `SliceParamToCmdRegsEnc1` packer.

## Vendor T40 HEVC command lists (T40XP, libimp on the board, CBR)

Captured with an LD_PRELOAD that dumps each pushed command list
(`0x83e0` address, `0x83e4` push) from `/dev/mem`; ch0 3840x2160 at
4 Mbit/s, ch1 1280x720 at 1 Mbit/s, 20 fps.  Differences from the T40 AVC
template:

| word | vendor HEVC | note |
|------|-------------|------|
| 0x00 | `0x91700c11` | codec 1, CTB 32, TU 4..32 (as T31) |
| 0x02 | `0xc010ad50`, P `0xc090ad50` | bit 31 always, bit 23 on P |
| 0x06/0x07/0x0b | CTB grid | as T31 |
| 0x08 | IDR `0x77000000`, P `0x33000000` | AVC P: `0x11000000` |
| 0x09 | `0xfc058000` | vendor runs scaling lists + transform skip; OpenIMP clears bits 31/30/15 and signals none (as T31) |
| 0x0a | `area_1k << 16 \| 0x5000` | AVC area, HEVC PCM size |
| 0x0c..0x11 | POC words `2n` | AVC numbering, not the T31 HEVC `n` |
| 0x12 | bit 31, bits 27:24 = `(min(512 / ceil(w/64), 128) - 1) & 0xf` | L2 cache rows (UpdateCommand); 4K 7, 720p 8 |
| 0x13 | AVC formula `ceil(w/64) - 1` | unchanged |
| 0x14 | HWRC grid on the CTB grid | 4K `0xf40001ce`, 720p `0xf4000107` |
| 0x15 | T40 AVC target x 2 on the CTB grid | 4K `0x1d1a`/`0x14ca`, 720p `0x2856`/`0x1ccf` |
| 0x16 | `0x3f000000 \| bitrate * groups / (ctbs * fps)` | `0xc4`, `0x10f` |
| 0x18 | restart bits on IDR and first P (T31 rule) | |
| 0x19..0x1f | 0 | no inline Enc2 |
| 0x2d | EP3 slot: IDR one slot above P | |
| 0x64/0x65 | 0 | AVC writes width/height |

Addresses, pitches, map sizes and the MV offset are the AVC ones.

Vendor reference streams (same settings) decode clean with
`ffmpeg -err_detect aggressive`: ch0 190 frames, ch1 168 frames,
about 4.7 and 1.05 Mbit/s.

## Hardware rate control (HWRC) is off by default

With HWRC on (cmd[9] bit 16, cmd[0x14..0x18], PPS `cu_qp_delta_enabled_flag`)
the stream desynchronises ffmpeg in some P pictures ("cu_qp_delta outside
the valid range", 3..26 per 10 s).  Command words, EP1 and EP2 match the
vendor dump; the remaining differences are the EP3 row targets, cmd[9]
bits 31/30/15 (vendor: scaling lists and transform skip) and the QP range.
Toggling bit 15 or TMVP and the EP2 placement did not change it; with HWRC
off every stream decodes clean.  OpenIMP therefore uses picture-level QP;
`OPENIMP_T40_HEVC_HWRC=1` re-enables HWRC for tests.  EP2 sits behind a
CTB-row WPP table, as in the vendor lists.

## Device results (OpenIMP, 13a3657 + HWRC off)

| run | ch0 3840x2160 | ch1 1280x720 |
|-----|---------------|--------------|
| H.265 CBR 4000/1000 kbit/s, 10 s | 164 frames, 322 kbit/s, clean | 190 frames, 121 kbit/s, clean |
| H.265 CBR 16000/4000 kbit/s, 10 s | 165 frames, 4319 kbit/s, one `CABAC_MAX_BIN` message | 189 frames, 1084 kbit/s, clean |
| H.264 CBR 4000/1000 kbit/s, 5 s | 99 frames, 1510 kbit/s, clean | 100 frames, 372 kbit/s, clean |
| vendor H.265 4000/1000 | 190 frames, about 4.7 Mbit/s | 168 frames, about 1.05 Mbit/s |

Clean = `ffmpeg -err_detect aggressive` without messages; VA-API HEVC
decodes all streams.  The low bitrates are the open-loop picture QP of
T40; the frame-level rate control of `claude/t40-rc` acts on the same
picture QP and covers HEVC as well.

## Implementation

`avpu_t40_hevc_fill_cmd()` in `src/t40/codec-t40.c`; headers, EP1/EP2 and
knobs are the T31 ones (`OPENIMP_T40_HEVC_HWRC`, `_CABAC_INIT`, `_TMVP`).
T40 publishes one pack per access unit (`h265NalType` IDR_W_RADL/TRAIL_R),
like T41.
