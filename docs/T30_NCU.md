# T30 NCU denoise (openimp #701)

The T30 has an NCU (noise cancel unit, register block 0x13090000) between the ISP core and the
scaler. The kernel driver only streams it (open-tx-isp `tx_isp_t30_ncu_stream_on`: strides, frame
size, start, 0x24 bits 7:0). The denoise itself is configured from user space by libimp through
`/dev/mem`, from `/etc/sensor/ncu_<sensor>.bin`. Without that file the vendor uses Temper.

## Vendor behaviour (reference: stock ABUS firmware libimp 1.0.4, md5 962be4bc)

| Step | Vendor (nm address) | OpenIMP |
|---|---|---|
| bin probe | AddSensor 0xc5ed0: fread 2 words, 3D mode = `(word1 >> 16) & 3` | `t30_ncu_bin_probe`, EnableSensor (`tseries_t30_temper_alloc`) |
| mode 0 (NCU always) | `ncu_hal_init`, `ncu_hal_buf_size`, IMP_Alloc "ncubuf", `ncu_hal_buf_init`, `ncu_ctrl_bypass(0)` | same |
| mode 1 (Temper day, NCU night) | IMP_Alloc w*h*4 (imx307: 0x847c00 = 1952x1112x4), `ncu_hal_init`, `ncu_hal_buf_init`, `ncu_ctrl_bypass(1)`; Temper uses the same buffer (EnableTuning 0x8000084) | same |
| mode 2 / no file | Temper only | unchanged (w*h*4 + guard, #697) |
| NCU geometry | 1.0.5 EnableSensor: width/height = CREATE_LINKS (0x800456d0) result = ISP output ispw x isph, vi max only if 0 | same (`tseries_t30_link_wh`); the vi max (ABUS 1948x1109 vs NCU input 1920x1080) stalls the NCU: device test 1 |
| `ncu_hal_init` 0xe29fc | psn/sta strides 0x68..0xac, 0x24 low byte kept, fread 0x1548 bytes, day set active, first download | `t30_ncu_hal_init` (debug server `/tmp/ncudbg.cfg` not ported) |
| buffers 0xe52e0 / 0xe4fc8 | ref0 = ref1 = phys (Y) / phys+w*h (UV), statistics at phys+w*h*3/2, 0x88 = 49, 0x94 = rows | `t30_ncu_hal_buf_init` |
| per-gain download 0xd94b0 | 122 registers 0x180..0x400 from 9-point gain tables (linear / step interpolation), 0x0c = 4 ... 5 | `t30_ncu_par104.c` (transcribed, oracle-checked) |
| 1 s update | tuning daemon: `isp_tuning_func_update_total_gain`, then `ncu_ctrl_update(total gain)` unless mode 2 or gain 0 | tuning worker (`tseries_tuning_worker`) |
| night (SetISPRunningMode 1) | mode 1: ISP 0x13380040 \|= 0x400 (Temper bypass), Temper API off, `ncu_ctrl_bypass(0)`, night set; mode 0: night set | same |
| day | mode 1: `ncu_ctrl_bypass(1)`, Temper API on, 0x400 cleared (before the driver switch); mode 0: day set | same |

Bin layout 1.0.4: 0x1548 bytes = size word, day set (0xaa0 bytes) at +4, night set at +4+0xaa0.
Each set: flags (+0), function enables (+4, to 0x24 bits 31:8), 9 gain points (+0x08..+0x28),
then tuning values (mostly 9-point tables).

## Bin layout 1.0.5 (#703)

Vendor libimp 1.0.5 (T30X library, gcc 5.4.0 build, md5 ad636e05): the generic `ncu_*.bin` of the
ingenic-sdk `sensor-iq/t30/` (one identical file for every sensor, 3D mode 0). 0xec8 bytes =
size word, day set (0x760 bytes) at +4, night set at +4+0x760, one more word. Each set starts with the
flags word (bits 31:24 version 1, `(flags & 0x3ffff) >> 16` = 3D mode, bits 1-2 debug logging, bit 3
also writes 0x04 = 2, bit 4 also writes 0x24, bit 0 stops the 1 s update).

| Step | Vendor 1.0.5 (nm address) | OpenIMP |
|---|---|---|
| bin probe | EnableSensor 0xd3fe4: 2 words, `flags >> 24 == 1` else assert | `t30_ncu_bin_probe` (no assert: invalid = Temper) |
| load | `ncu_load_param` 0xde400 fread 0xec8, `ncu_set_params` 0xd7f28 | `t30_ncu_hal_init` (layout from the probe) |
| active set | `ncu_ctrl_update_param` 0xd80a0: day or night set to `g_ncu_t30_param`, download with `isp_get_total_gain` | same |
| bypass 0 | `ncu_func_bypass` 0xd834c: enables from set+4, engine word set+0x30, bit set+0x2c | same, offsets of the layout |
| per-gain download | `ncu_func_par_download` 0xd86d8 (0x5b2c bytes) | `t30_ncu_par105.c` (transcribed, oracle-checked) |
| mode 2 / 3 / no file | Temper only (mode 3 not an error as in 1.0.4) | Temper only |

1.0.5 asserts on 1.0.4 bins and 1.0.4 cannot read 1.0.5 bins; OpenIMP reads both (beyond vendor).
- The NCU debug server (`/tmp/ncudbg.cfg`) and `/tmp/ncu_sta_get_flag`.

## Test

- `tests/t30/ncu_test.c`: (1.0.4 and 1.0.5, `fixtures/ncu105_synth_mode{0,1}.txt` recorded from the 1.0.5
  library with the same oracle tool) the whole sequence (init, buffers, bypass, night and day sets, 10 gains)
  against the vendor register trace in `tests/t30/fixtures/ncu104_synth_mode{0,1}.txt`, recorded by
  `tools/t30_ncu/ncu_oracle.c` (vendor libimp 1.0.4 under qemu-mipsel, register accessors
  preempted) on the synthetic bin of `tests/t30/ncu_synth_bin.h`. Write for write identical, also
  when cross-built for mipsel and run under qemu-mipsel.
- 1.0.5 during development: an SDK sensor-iq bin (private, not published) at 19 gains and 145 randomised
  bins (flags, sizes, modes) identical (`tools/t30_ncu/ncu_host_trace.c` against the oracle).
- During development: the ABUS bin at 19 gains (day and night) and 75 randomised bins identical.

## Firmware

thingino `BR2_SENSOR_1_NCU_FILE` installs the file as `/etc/sensor/ncu_<sensor>.bin`.
