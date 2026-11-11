# T32 ISP tuning table (#296)

OpenIMP T32 (PRJ007, vendor 2.x ABI, planned SoC) issues every `IMP_ISP_Tuning_*` call as
`DEFAULT_CMD_ISP_TUNING` (`0xc0105467`) on `/dev/isp-m0` with `{vinum, dir, id, value or pointer}`,
like vendor libimp 2.2.0 (`src/t32/openimp_t32_tuning.c`, built against the vendor T32 2.2.0
public headers, `T32_HEADERS`). The table below is recorded, not guessed: `tools/t32/t32_tuning_sample.c`
calls each function once (each Get after its Set) in QEMU `-M ingenic-t32` under the LD_PRELOAD
recorder, which also logs the bytes behind the value pointer; `tools/t32/t32_tuning_table.py`
turns the log into `tests/t32/vendor-2.2.0-tuning.txt` / `openimp-tuning.txt`, and
`make -C tests/t32 check` compares them (request, VI, direction, id, Set payload, return code).

State 2026-11-11: 97 tuning API steps identical to vendor 2.2.0 (ids, payload bytes, return codes).
Only the sensor frame rate (`0x08000070`) is answered by open-tx-isp `driver/t32` so far (QEMU
round trip: Set 10/1, Get 10/1); every other id returns `ENOTTY` from the driver and is a driver
gap (listed in #296). Payload = bytes behind the pointer the driver gets (the vendor structure,
or the converted driver block for anti-flicker, CCM, CSC and wait-frame).

| API step | id | dir | payload bytes | driver/t32 |
|---|---|---|---|---|
| IMP_ISP_EnableTuning | 0x08000070 | get | value | served |
| IMP_ISP_Tuning_SetSensorFPS | 0x08000070 | set | value | served |
| IMP_ISP_Tuning_GetSensorFPS | 0x08000070 | get | value | served |
| IMP_ISP_Tuning_SetAntiFlickerAttr | 0x08000026 | set | 5 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAntiFlickerAttr | 0x08000026 | get | 5 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetHVFLIP | 0x08000073 | set | 16 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetHVFLIP | 0x08000073 | get | 16 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetBrightness | 0x08000092 | set | 1 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetContrast | 0x08000095 | set | 1 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetSharpness | 0x08000093 | set | 1 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetSaturation | 0x08000094 | set | 1 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetBcshHue | 0x08000081 | set | 1 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetBcshRGBOffsetAttr | 0x0800009b | set | 6 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetBcshRGBOffsetAttr | 0x0800009b | get | 6 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetModuleControl | 0x08000072 | set | 4 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetModuleControl | 0x08000072 | get | 4 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetModule_Ratio | 0x080000a4 | set | 128 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetModule_Ratio | 0x080000a4 | get | 128 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetISPCSCAttr | 0x08000096 | set | 92 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetISPCSCAttr | 0x08000096 | get | 92 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetCCMAttr | 0x08000080 | set | 40 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetCCMAttr | 0x08000080 | get | 40 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetGammaAttr | 0x08000025 | set | 264 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetGammaAttr | 0x08000025 | get | 264 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetStatisConfig | 0x08000000 | set | 72 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetStatisConfig | 0x08000000 | get | 72 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAeExprInfo | 0x08000023 | set | 204 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeExprInfo | 0x08000023 | get | 204 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeOnlyReadAttr | 0x0800002b | get | 48 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAeTargetAttr | 0x0800002d | set | 264 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeTargetAttr | 0x0800002d | get | 264 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAeLightCompensation | 0x0800002e | set | 8 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeLightCompensation | 0x0800002e | get | 8 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAwbAttr | 0x08000010 | set | 60 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAwbAttr | 0x08000010 | get | 60 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAfWeight | 0x08000032 | set | 225 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAfWeight | 0x08000032 | get | 225 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAwbWeight | 0x08000012 | set | 1024 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAwbWeight | 0x08000012 | get | 1024 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAwbStatistics | 0x08000011 | get | 12288 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAwbGlobalStatistics | 0x08000013 | get | 16 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeStatistics | 0x08000022 | get | 1934 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAeWeight | 0x08000021 | set | 232 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeWeight | 0x08000021 | get | 232 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAFMetricesInfo | 0x08000034 | get | 12 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAeExpList | 0x08000027 | set | 324 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeExpList | 0x08000027 | get | 324 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAEFlickerFlag | 0x08000028 | get | 4 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAwbOnlyReadAttr | 0x08000015 | get | 4 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAutoZoom | 0x08000077 | set | 60 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAutoZoom | 0x08000077 | get | 60 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetOSDAttr | 0x080000a1 | set | 32 | driver (#336) |
| IMP_ISP_Tuning_GetOSDAttr | 0x080000a1 | get | 32 | driver (#336) |
| IMP_ISP_Tuning_SetOSDBlock | 0x080000a2 | set | 20 | driver (#336) |
| IMP_ISP_Tuning_GetOSDBlock | 0x080000a2 | get | 20 | driver (#336) |
| IMP_ISP_Tuning_SetWdrOutputMode | 0x08000054 | set | 4 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetWdrOutputMode | 0x08000054 | get | 4 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetScalerLv | 0x080000a6 | set | 12 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAwbCtTrendOffset | 0x08000017 | set | 28 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAwbCtTrendOffset | 0x08000017 | get | 28 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetFaceAe | 0x08000035 | set | 24 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetFaceAe | 0x08000035 | get | 24 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetFaceAeWeiget | 0x0800002a | set | 12 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetFaceAeWeiget | 0x0800002a | get | 12 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetFaceAeLuma | 0x08000029 | get | 8 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetFaceAwb | 0x08000014 | set | 24 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetFaceAwb | 0x08000014 | get | 24 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetTmoFaceae | 0x08000062 | set | 56 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetMultiSensorAeEffect | 0x08000098 | set | 72 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetMultiSensorAeEffect | 0x08000098 | get | 72 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetMultiSensorAWBEffect | 0x08000099 | set | 72 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetMultiSensorAWBEffect | 0x08000099 | get | 72 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetMultiSensorEffect | 0x0800009a | set | 72 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetMultiSensorEffect | 0x0800009a | get | 72 | gap (ENOTTY) |
| IMP_ISP_SetCsccrMode | 0x08000097 | set | 12 | gap (ENOTTY) |
| IMP_ISP_GetCsccrMode | 0x08000097 | get | 12 | gap (ENOTTY) |
| IMP_ISP_HB_SetAttr | 0x080000a7 | set | 2 | gap (ENOTTY) |
| IMP_ISP_HB_GetAttr | 0x080000a7 | get | 2 | gap (ENOTTY) |
| IMP_ISP_Tuning_SetAeStartAttr | tx-isp c0045436 | - | - | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAeStartAttr | tx-isp c0045437 | - | - | gap (ENOTTY) |
| IMP_ISP_Tuning_SetMaskBlock | 0x08000074 | set | 24 | driver (#336) |
| IMP_ISP_Tuning_GetMaskBlock | 0x08000074 | get | 24 | driver (#336) |
| IMP_ISP_Tuning_SetDrawBlock | 0x080000a0 | set | 36 | driver (#336) |
| IMP_ISP_Tuning_GetDrawBlock | 0x080000a0 | get | 36 | driver (#336) |
| IMP_ISP_Tuning_GetSensorAttr | 0x08000033 | get | 20 | gap (ENOTTY) |
| IMP_ISP_Tuning_GetAfStatistics | 0x08000031 | get | 5400 | gap (ENOTTY) |
| IMP_ISP_Tuning_WaitFrameDone | 0x08000091 | get | 24 | gap (ENOTTY) |

Conversions (vendor behaviour kept, RE of libimp 2.2.0):

- BCSH Get (brightness, contrast, sharpness, saturation, hue): answered from the last value set
  (0x80 after `IMP_ISP_EnableTuning`), no request.
- Anti-flicker: driver block `{u32 mode; u8 freq}`; mode < 3, freq < 256.
- CCM: driver block `{u8 manual, u8 sat, 2 pad, u32 c[9]}`, coefficient = |c| * 1024 in 13 bits,
  bit 13 = negative (values >= -1e-5 count as positive); Get divides by 1024 and sign-extends the
  two bytes. The vendor also negates the caller's negative coefficients in place (not done here).
- CSC: mode 6 (user matrix) carries `int(c * 65536 + 0.5)`, Get divides by 65535 (vendor's
  asymmetric scale); other modes send only the mode word (vendor: rest of its stack block
  uninitialised, zero here).
- Wait-frame: driver block = the caller's timeout word, words 2..4 come back.
- Mask / draw block: an RGB colour gets its BT.601 YUV next to it (bytes +3..+5 of the colour). The last
  Set is kept per VI (mask: entry chx * pinum, draw: one slot) and Get puts its colour type and RGB
  bytes back, as the vendor (#336; draw only for pinum 0, the vendor reads past its slot otherwise).
- Sensor fps: value word `num << 16 | den` (no pointer); `IMP_ISP_EnableTuning` reads it once per
  streaming sensor (the vendor hands it to FrameSource; the T32 FrameSource reads the rate per frame).
- AE start (`IMP_ISP_Tuning_Set/GetAeStartAttr`): `/dev/tx-isp` `0xc0045436/7` with `{vinum, w0, w1}`,
  no tuning needed (the vendor Get writes a third word past the 8-byte structure; not here).
- Multi-sensor effect: Set always on VI 0, Get on any VI without a range check.

Not in this table: `IMP_ISP_Tuning_SetISPRunningMode/GetISPRunningMode/SwitchBin` (IQ bin, #289),
the ISP OSD regions (`IMP_ISP_Tuning_*OsdRgn*`, `SetOsdPoolSize`: `src/t32/openimp_t32_isp_osd.c`, #336), `SetVideoDrop` (no
request; the callback is stored), the vendor's multi-process server mode and its
`/tmp/isp_tuning_func` file.
