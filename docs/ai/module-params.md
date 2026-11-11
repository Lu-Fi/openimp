# open-tx-isp module parameters

**Applies to open-tx-isp `next` of 2026-11-08 (RC3 `v2026.11.07-rc3` plus
later commits).** The tables in section 3 are generated from the driver
source: every `module_param*` declaration under `driver/<soc>/` (including the
per-SoC bring-up wrappers), with the type, the permission, the initial value
of the variable in the source and the `MODULE_PARM_DESC` text (or the source
comment when there is none). **1300 declarations; 702 are in the default
build** (T10 54, T20 54, T21 15, T23 93, T30 15, T31 47, T40 249, T41 163,
T32 12). The rest exist only in special builds (section 3 lists them by name).

## 1. How parameters are used

- Set on the module load line. In thingino the load lines are in
  `/etc/modules.d/` (for example `tx-isp-t31 isp_clk=200000000`).
- Read (and, with permission `0644`, change at run time) under
  `/sys/module/tx_isp_<soc>/parameters/<name>` (underscores; check the exact
  directory with `ls /sys/module | grep tx_isp`). Permission `0444` = read-only
  after load; `0` = not in sysfs, load line only.
- The 3.10 kernels (T10..T31) **refuse to load a module given an unknown
  parameter.** A parameter that exists only in a bring-up or trace build makes
  the default module fail to load. Remove such parameters from load scripts.
- Special builds (Kbuild switches, default `n`): `TX_ISP_T23_BRINGUP_PARAMS`,
  `TX_ISP_T40_BRINGUP_PARAMS`, `TX_ISP_T41_BRINGUP_PARAMS` (experiment switches
  and diagnostic counters), `TX_ISP_TRACE` (T10/T20 traces), `TX_ISP_LOAD_STATS`
  (load statistics), `TX_ISP_T41_TMO_SELFTEST`.
- These parameters exist only in the open driver. The vendor module has its
  own, different set.
- Do not recommend bring-up, trace or read-only diagnostic parameters to a
  user as a fix. Recommend only the parameters of section 2, and only for the
  reason given there.
- The source wins: where the wiki shows a different default, the table here
  was read from `next` (for example T41 `isp_clk` is 300000000, and
  `t41_msca_cfg_update` = 2 with `t41_msca_stop_disable` = 1 are the defaults
  since the T41 channel-1 work; older wiki text calls them opt-in 0).

## 2. Parameters that matter to users

| Parameter | SoC | Type / perm | Default | Meaning |
|---|---|---|---|---|
| `isp_clk` | all | int, 0 (T31: 0444) | T10 90000000, T20 90000000, T21 100000000, T23 153000000, T30 90000000, T31 200000000, T40 200000000, T41 300000000 | ISP core clock in Hz. Leave at the default. |
| `print_level` | T21, T23, T30, T31, T40, T41 | int, 0 (T31 0444) | 1 | Driver log verbosity. |
| `qbuf_guard` | T10, T20, T21, T23, T31, T41 | bool, 0644 | 1 | Reject frame buffers outside the rmem window of the kernel command line. `0` only as a debug step. |
| `isp_memopt` | T23, T31, T40, T41 | int, 0 (T31 0444) | T31 1, T41 1, T23 0, T40 0 | Smaller ISP memory layout (T31: fits `rmem=22M`); `0` = the larger OEM layout. |
| `isp_mmap_pool_kb` | T10, T20 | 0444 | 0 | Optional V4L2 MMAP frame pool. OpenIMP uses USERPTR buffers from rmem and never needs it; a V4L2 MMAP client gets one log line with the size to set. |
| `fcrop_upscale_pct` | T23, T31 | uint, 0644 | T31 100, T23 0 | How much MSCA upscale the front-crop check tolerates (100 = 2.0x per axis, larger values clamped). The file exists only in drivers with the safe front-crop path, so an application can test for it. |
| `isp_bypass_graph` | T23, T31, T41 | int, 0444 | 0 | `1` enables the vendor ISP bypass graph (RAW frames); use together with the matching `OPENIMP_*_BYPASS_GRAPH`. T30 has no parameter (the library variable alone enables it). |
| `msca_scratch` | T23 | bool, 0644 | 1 | Park a stopped MSCA output on a scratch area; `0` is the first thing to try if a stop/start hangs. |
| `chan_stop_drain` | T23 | 0644 | 21 | STREAMOFF waits up to N x 10 ms for queued buffers. |
| `source_af` | T23 | bool, 0444 | 1 | AF statistics chain like the stock module; `0` = off. |
| `source_ae_oem` | T23 | bool, 0444 | 1 | AE lifted from the OEM module; `0` = substitute AE. |
| `source_awb_lum_th` | T23 | bool, 0644 | 1 | Rewrite the AWB luminance threshold from the AE mean like stock; `0` = fixed start value. |
| `isp_osd` | T23 | 0644 | 1 | Draw ISP OSD pictures into the MSCA every frame; `0` only stores them. |
| `mdns_ref_hold` | T23 | callback, 0644 | 0 | Hold the 3D-denoise reference weights (used by test pattern mode 5). |
| `t23_isr_enable`, `t31_isr_enable` | T23, T31 | 0644 | all sources except the unused ones | ISP interrupt enable word; `0xffffffff` = stock. |
| `t31_awb_impl` | T31 | callback, 0644 | 2 | AWB colour-temperature code: 0 reference C, 1 fast C, 2 MXU2 after a selftest (falls back to fast C); all bit-exact. |
| `awb_lux_hyst` | T21 | 0644 | 10 | AWB low-light hysteresis band in percent; `0` = vendor behaviour. |
| `awb_ir_freeze` | T21 | 0644 | 1 | Freeze AWB at night and restore the day gains; `0` = vendor behaviour. |
| `warn_stack` | T21 | 0644 | 0 | `1` restores the vendor stack dump after every warning. |
| `simple_3a`, `simple_awb` | T30 | bool, 0 / 0644 | 1 | Calibration-driven AE/AWB path. |
| `t20_simple_ae`, `t20_simple_awb`, `t20_simple_nr` | T10, T20 | bool, 0644 | 1 | Bounded AE / AWB / gain-driven denoise instead of the recovered OEM state graphs. |
| `t40_dmsc_follow`, `t40_dmsc_day_follow` | T40 | bool, 0644 | 1 | Demosaic follows the night bank and total gain (OEM behaviour). |
| `t40_ae_target_oem` | T40 | 0644 | 1 | AE target and metering like the OEM (#84, #232); `0` = old fixed target. |
| `t40_ae_stats_src` | T40 | 0644 | 0 | AE statistics source: 0 hardware while it runs, 1 hardware, 2 frame grid. |
| `t40_ae_af_enable` | T40 | bool, 0644 | 1 | Anti-flicker (`SetAntiFlickerAttr`) reaches the frame AE. |
| `t40_dn_keep_awb1` | T40 | bool, 0644 | 1 | Keep AWB1 at the day state at night; avoids a grey/green-magenta day picture after night to day. |
| `t40_gamma_ev`, `t40_clm` | T40 | bool, 0644 | 1 | OEM gamma EV follow-up; CLM colour LUT from the bank. |
| `t40_dqbuf_wait_ms` | T40 | 0644 | 0 | Blocking DQBUF wait; 0 = poll. |
| `t41_vendor_ae` | T41 | 0444 | 1 | Translated vendor AE per frame; `0` = safe AE controller. |
| `t41_ae_roi_stats` | T41 | 0644 | 1 | ROI AE statistics; needed by `SetFaceAe`. |
| `t41_defog` | T41 | 0444 | 1 | Translated defog chain. |
| `t41_af` | T41 | int, 0444 | 0 | **Opt-in**: AF statistics block; with 0 the AF queries return EPERM. |
| `t41_msca_cfg_update` | T41 | 0644 | 2 | MSCA output geometry latch mode (2 = latch with the output off). |
| `t41_msca_stop_disable` | T41 | 0644 | 1 | Switch a stopped MSCA output off and wait for its frame (stock). |
| `t41_msca_ch1_max_w`, `t41_msca_ch1_max_h` | T41 | 0644 | 1920, 1080 | Largest channel-1 size SET_FMT accepts with `t41_msca_cfg_update` >= 2 (0 = no limit). |
| `t41_isr_enable` | T41 | callback, 0644 | all but AF bit 6, all, LSC/BCSH bits 4+20, 0x3f | ISP IRQ enable words. |

## 3. All parameters per SoC (generated)

"Default" is the initial value in the source (`0 (not initialised)` = a
static variable without initialiser; `computed` = a callback parameter whose
value is computed). A thingino load line can override it. "-" = no
description in the source.

### T10 (module `tx-isp-t10`, sysfs `/sys/module/tx_isp_t10/parameters/`)

54 parameters in the default build, 4 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `isp_clk` | int | 0 (load line only) | `90000000` | isp core clock |
| `isp_mmap_pool_kb` | uint | 0444 | `0 (not initialised)` | V4L2 MMAP frame pool in KiB (default 0: none, USERPTR only; 8192 fits 2x1080p NV12 + sub-streams) |
| `qbuf_guard` | bool | 0644 | `true` | Reject framechan QBUF buffers outside rmem (default 1) |
| `t20_ae_last_curve` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_exposure_log2` | int | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_mean` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_population` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_raw_target` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_target` | uint | 0444 | `0 (not initialised)` | - |
| `t20_antifog_ignored` | uint | 0444 | `0 (not initialised)` | anti-fog requests that found the Iridix block disabled |
| `t20_antifog_level` | uint | 0444 | `0 (not initialised)` | last accepted anti-fog level (0 off, 1 strong, 2 medium, 3 weak) |
| `t20_antifog_variance` | uint | 0644 | `0 (not initialised)` | 1: the anti-fog presets also write the Iridix variance nibbles (0x3c8) at the frame-start commit; 0 (default): strength only |
| `t20_awb_feedback_ready` | bool | 0444 | `0 (not initialised)` | - |
| `t20_awb_last_bg` | uint | 0444 | `0 (not initialised)` | - |
| `t20_awb_last_blue_q8` | uint | 0444 | `0x100` | - |
| `t20_awb_last_color_temperature` | uint | 0444 | `5000` | - |
| `t20_awb_last_gain_00` | uint | 0444 | `0x100` | - |
| `t20_awb_last_gain_01` | uint | 0444 | `0x100` | - |
| `t20_awb_last_gain_10` | uint | 0444 | `0x100` | - |
| `t20_awb_last_gain_11` | uint | 0444 | `0x100` | - |
| `t20_awb_last_population` | uint | 0444 | `0 (not initialised)` | - |
| `t20_awb_last_red_q8` | uint | 0444 | `0x100` | - |
| `t20_awb_last_rg` | uint | 0444 | `0 (not initialised)` | - |
| `t20_awb_stable_ae_frames` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ccm_frame_updates` | uint | 0444 | `0 (not initialised)` | - |
| `t20_cmos_irq_stage` | uint | 0644 | `9` | temporary T20 bring-up limit for the CMOS frame-start path |
| `t20_frame_end_stage` | uint | 0644 | `9` | temporary T20 bring-up stage gate for frame-end IRQ processing |
| `t20_fsm_event_allow_mask` | uint | 0644 | `0x3fff` | temporary T20 bring-up mask for queued firmware event consumers |
| `t20_fsm_irq_allow_mask` | uint | 0644 | `0xff` | temporary T20 bring-up mask for firmware IRQ consumers |
| `t20_iridix_asymmetry_shape` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_asymmetry_signature` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_exposure_factor` | int | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_full_strength_exposure` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_hist_factor` | int | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_maximum` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_minimum` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_no_strength_exposure` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_target` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_lut_init_count` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_lut_mode` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rec709torgb_shape` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rec709torgb_signature` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rgb2rec709_shape` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rgb2rec709_signature` | uint | 0444 | `0 (not initialised)` | - |
| `t20_irq_allow_mask` | uint | 0644 | `0xffff` | temporary T20 bring-up mask for ISP-core interrupt dispatch |
| `t20_mesh_scale_cur` | uint | 0444 | `0xff` | lens-shading scale last written by SetMeshShadingScale (255 = none) |
| `t20_park_fw_after_first_pass` | bool | 0644 | `true` | park the recovered T20 firmware thread after its proven-good first pass (only with t20_simple_awb=1) |
| `t20_sensor_rearm_count` | uint | 0444 | `0 (not initialised)` | - |
| `t20_sensor_rearm_writes` | uint | 0444 | `0 (not initialised)` | - |
| `t20_simple_ae` | bool | 0644 | `true` | use bounded histogram AE instead of the recovered OEM AE state graph |
| `t20_simple_awb` | bool | 0644 | `true` | use bounded metering feedback instead of the recovered OEM AWB state graph |
| `t20_simple_awb_static_only` | bool | 0644 | `0 (not initialised)` | hold the four active IQ static-WB gains for bounded AWB diagnostics |
| `t20_simple_nr` | bool | 0644 | `true` | apply gain-driven ISP denoise from the active IQ calibration tables |
| `t20_simple_sharpen` | bool | 0644 | `0 (not initialised)` | always apply gain-driven ISP sharpening from the compact AE (it already runs whenever the firmware thread is parked) |

- **TX_ISP_TRACE=y only** (4): `t10_runtime_trace`, `t20_runtime_trace`, `t20_trace`, `t20_trace_events`

### T20 (module `tx-isp-t20`, sysfs `/sys/module/tx_isp_t20/parameters/`)

54 parameters in the default build, 4 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `isp_clk` | int | 0 (load line only) | `90000000` | isp core clock |
| `isp_mmap_pool_kb` | uint | 0444 | `0 (not initialised)` | V4L2 MMAP frame pool in KiB (default 0: none, USERPTR only; 8192 fits 2x1080p NV12 + sub-streams) |
| `qbuf_guard` | bool | 0644 | `true` | Reject framechan QBUF buffers outside rmem (default 1) |
| `t20_ae_last_curve` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_exposure_log2` | int | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_mean` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_population` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_raw_target` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ae_last_target` | uint | 0444 | `0 (not initialised)` | - |
| `t20_antifog_ignored` | uint | 0444 | `0 (not initialised)` | anti-fog requests that found the Iridix block disabled |
| `t20_antifog_level` | uint | 0444 | `0 (not initialised)` | last accepted anti-fog level (0 off, 1 strong, 2 medium, 3 weak) |
| `t20_antifog_variance` | uint | 0644 | `0 (not initialised)` | 1: the anti-fog presets also write the Iridix variance nibbles (0x3c8) at the frame-start commit; 0 (default): strength only |
| `t20_awb_feedback_ready` | bool | 0444 | `0 (not initialised)` | - |
| `t20_awb_last_bg` | uint | 0444 | `0 (not initialised)` | - |
| `t20_awb_last_blue_q8` | uint | 0444 | `0x100` | - |
| `t20_awb_last_color_temperature` | uint | 0444 | `5000` | - |
| `t20_awb_last_gain_00` | uint | 0444 | `0x100` | - |
| `t20_awb_last_gain_01` | uint | 0444 | `0x100` | - |
| `t20_awb_last_gain_10` | uint | 0444 | `0x100` | - |
| `t20_awb_last_gain_11` | uint | 0444 | `0x100` | - |
| `t20_awb_last_population` | uint | 0444 | `0 (not initialised)` | - |
| `t20_awb_last_red_q8` | uint | 0444 | `0x100` | - |
| `t20_awb_last_rg` | uint | 0444 | `0 (not initialised)` | - |
| `t20_awb_stable_ae_frames` | uint | 0444 | `0 (not initialised)` | - |
| `t20_ccm_frame_updates` | uint | 0444 | `0 (not initialised)` | - |
| `t20_cmos_irq_stage` | uint | 0644 | `9` | temporary T20 bring-up limit for the CMOS frame-start path |
| `t20_frame_end_stage` | uint | 0644 | `9` | temporary T20 bring-up stage gate for frame-end IRQ processing |
| `t20_fsm_event_allow_mask` | uint | 0644 | `0x3fff` | temporary T20 bring-up mask for queued firmware event consumers |
| `t20_fsm_irq_allow_mask` | uint | 0644 | `0xff` | temporary T20 bring-up mask for firmware IRQ consumers |
| `t20_iridix_asymmetry_shape` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_asymmetry_signature` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_exposure_factor` | int | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_full_strength_exposure` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_hist_factor` | int | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_maximum` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_minimum` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_no_strength_exposure` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_last_target` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_lut_init_count` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_lut_mode` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rec709torgb_shape` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rec709torgb_signature` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rgb2rec709_shape` | uint | 0444 | `0 (not initialised)` | - |
| `t20_iridix_rgb2rec709_signature` | uint | 0444 | `0 (not initialised)` | - |
| `t20_irq_allow_mask` | uint | 0644 | `0xffff` | temporary T20 bring-up mask for ISP-core interrupt dispatch |
| `t20_mesh_scale_cur` | uint | 0444 | `0xff` | lens-shading scale last written by SetMeshShadingScale (255 = none) |
| `t20_park_fw_after_first_pass` | bool | 0644 | `true` | park the recovered T20 firmware thread after its proven-good first pass (only with t20_simple_awb=1) |
| `t20_sensor_rearm_count` | uint | 0444 | `0 (not initialised)` | - |
| `t20_sensor_rearm_writes` | uint | 0444 | `0 (not initialised)` | - |
| `t20_simple_ae` | bool | 0644 | `true` | use bounded histogram AE instead of the recovered OEM AE state graph |
| `t20_simple_awb` | bool | 0644 | `true` | use bounded metering feedback instead of the recovered OEM AWB state graph |
| `t20_simple_awb_static_only` | bool | 0644 | `0 (not initialised)` | hold the four active IQ static-WB gains for bounded AWB diagnostics |
| `t20_simple_nr` | bool | 0644 | `true` | apply gain-driven ISP denoise from the active IQ calibration tables |
| `t20_simple_sharpen` | bool | 0644 | `0 (not initialised)` | always apply gain-driven ISP sharpening from the compact AE (it already runs whenever the firmware thread is parked) |

- **TX_ISP_TRACE=y only** (4): `t10_runtime_trace`, `t20_runtime_trace`, `t20_trace`, `t20_trace_events`

### T21 (module `tx-isp-t21`, sysfs `/sys/module/tx_isp_t21/parameters/`)

15 parameters in the default build, 0 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `adr_bypass` | int | 0644 | `0 (not initialised)` | T21: force TOP bypass bit 4 (ADR) off (diagnostic, default 0) |
| `awb_ir_freeze` | uint | 0644 | `1` | T21: freeze AWB in night mode and restore the day gains on return to day (default 1, 0 = stock) |
| `awb_lux_hyst` | uint | 0644 | `10` | T21: AWB lux/low-light threshold hysteresis band in percent (default 10, 0 = stock) |
| `ct_event_thresh` | uint | 0644 | `50` | T21: skip the CT->CCM/LSC update while the AWB CT moved by at most this many Kelvin (default 50, 0 = stock: every frame) |
| `free_watch` | bool | 0444 | `0 (not initialised)` | - |
| `irqoff_max_us` | uint | 0644 | `0 (not initialised)` | T21 debug: max IRQs-off time of the ISP event thread in us (irqoff_stat=1; write 0 to reset) |
| `irqoff_stat` | bool | 0644 | `0 (not initialised)` | T21 debug: measure the max IRQs-off time of the ISP event thread (default 0) |
| `isp_clk` | int | 0 (load line only) | `100000000` | isp clock freq |
| `print_level` | int | 0 (load line only) | `1` | isp print level |
| `qbuf_guard` | bool | 0644 | `true` | Reject framechan QBUF buffers outside rmem (default 1) |
| `t21_sensor_rearm_count` | uint | 0444 | `0 (not initialised)` | - |
| `t21_sensor_rearm_writes` | uint | 0444 | `0 (not initialised)` | - |
| `t21_sensor_write_always` | int | 0644 | `0 (not initialised)` | 1 queues the sensor exposure/gain every frame even when unchanged (default 0: only on change, as stock) |
| `t21_sensor_writes_skipped` | uint | 0444 | `0 (not initialised)` | - |
| `warn_stack` | bool | 0644 | `0 (not initialised)` | 1: dump_stack() after level>=2 ISP messages (vendor behaviour) |

### T23 (module `tx-isp-t23`, sysfs `/sys/module/tx_isp_t23/parameters/`)

93 parameters in the default build, 111 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `adr_oem_runs` | array of uint | 0444 | `0 (not initialised)` | lifted stock ADR irq,process / defog irq,process runs |
| `ae_oem_debug` | bool | 0644 | `0 (not initialised)` | log the lifted stock AE0's exposure and gain changes |
| `awb_lum_th_last` | uint | 0444 | `0 (not initialised)` | last 0xb038 value written by the AWB interrupt |
| `awb_lum_th_runs` | uint | 0444 | `0 (not initialised)` | AWB interrupts that rewrote 0xb038 (stock tiziano_awb_set_lum_th_freq) |
| `bypass_frames` | uint | 0444 | `0 (not initialised)` | frames completed by the bypass graph (read only) |
| `bypass_lost` | uint | 0444 | `0 (not initialised)` | bypass graph frame-done interrupts without a buffer owner (read only) |
| `chan_drain_ticks_max` | uint | 0444 | `0 (not initialised)` | longest STREAMOFF drain wait in 10 ms steps (read only) |
| `chan_drain_timeouts` | uint | 0444 | `0 (not initialised)` | STREAMOFF drain waits that ended with buffers still queued (read only) |
| `chan_drain_waits` | uint | 0444 | `0 (not initialised)` | STREAMOFF drain waits that found buffers queued in the hardware (read only) |
| `chan_stop_drain` | uint | 0644 | `21` | STREAMOFF: wait up to N x 10 ms for the channel's queued buffers to complete (default 21 = stock, 0 off) |
| `chan_stop_keep_input` | bool | 0644 | `true` | 1 (default, stock): keep the input running after the last frame channel STREAMOFF while tx-isp is streaming; 0 stop it (debug) |
| `clk_name` | charp | 0 (load line only) | `0 (not initialised)` | select the isp parent clock |
| `clka_name` | charp | 0 (load line only) | `0 (not initialised)` | select the axi bus parent clock |
| `crumb_addr` | uint | 0644 | `0 (not initialised)` | crumbs=2: physical address of the crumb page (outside kernel RAM) |
| `crumb_phys` | uint | 0444 | `0 (not initialised)` | physical address of the attached crumb page (read only, 0 = none) |
| `crumbs` | int | 0644 | `0 (not initialised)` | hang step markers: 0 off (default), 1 rmem page after the MDNS buffer, 2 page at crumb_addr |
| `direct_mode` | int | 0 (load line only) | `0 (not initialised)` | isp direct mode |
| `fcrop_upscale_pct` | uint | 0644 | `0 (not initialised)` | front crop: allowed MSCA upscale in percent (0 = none, capped at 100) |
| `isp_bypass_graph` | int | 0444 | `0 (not initialised)` | Enable the vendor ISP bypass graph (link config 1: raw RG12 frames from the VIC on frame channel 0) (default: 0) |
| `isp_ch0_pre_dequeue_interrupt_process` | int | 0 (load line only) | `0 (not initialised)` | isp pre dequeue interrupt process |
| `isp_ch0_pre_dequeue_time` | int | 0 (load line only) | `0 (not initialised)` | isp pre dequeue time, unit ms |
| `isp_ch0_pre_dequeue_valid_lines` | int | 0 (load line only) | `900` | isp pre dequeue valid lines |
| `isp_ch1_dequeue_delay_time` | int | 0 (load line only) | `0 (not initialised)` | isp pre dequeue time, unit ms |
| `isp_clk` | int | 0 (load line only) | `153000000` | isp clock freq |
| `isp_clka` | int | 0 (load line only) | `416000000` | isp aclock freq |
| `isp_day_night_switch_drop_frame_num` | int | 0 (load line only) | `0 (not initialised)` | isp day night switch drop frame number |
| `isp_memopt` | int | 0 (load line only) | `0 (not initialised)` | isp memory optimize |
| `isp_osd` | bool | 0644 | `1` | write ISP OSD pictures into the MSCA per frame (1, default) or only store them (0) |
| `ivdc_mem_line` | int | 0 (load line only) | `0 (not initialised)` | ivdc mem line |
| `ivdc_threshold_line` | int | 0 (load line only) | `0 (not initialised)` | ivdc threshold line |
| `mask_commits` | uint | 0444 | `0 (not initialised)` | - |
| `mdns_ref_hold` | callback | 0644 | `computed` | MDNS reference weight hold bitmask, default 0 = driver computed: 1 luma/chroma ref weight max (0x799c/0x7a6c), 2 fake/mv weight max, 4 r_max/r_min max |
| `msca_fifo_rearm` | bool | 0644 | `0 (not initialised)` | 1: clear and refill a stopped channel's MSCA address FIFO at STREAMON (default 0 = stock, debug) |
| `msca_flip_skip_noop` | bool | 0644 | `true` | 1 (default): no MSCA update request for unchanged mirror/flip bits; 0 always (debug) |
| `msca_flip_state` | callback | 0444 | `computed` | read-only: flip done by sensor/ISP, bits 0 sensor_h, 1 sensor_v, 2 isp_h, 3 isp_v |
| `msca_geom_rejects` | uint | 0444 | `0 (not initialised)` | set-format / front crop requests refused for an MSCA geometry that stalls the output (read only) |
| `msca_keep_enabled` | int | 0644 | `2` | STREAMOFF: 0 clear MSCA enable bit, 1 keep when the input stops too, 2 always keep (default, stock) |
| `msca_rel_origin` | bool | 0644 | `true` | write the channel window origin relative to the global MSCA window |
| `msca_releases` | uint | 0444 | `0 (not initialised)` | MSCA outputs released at a tx-isp session start (read only) |
| `msca_restart_skip` | bool | 0644 | `true` | 1 (default): no MSCA reload on STREAMON of a still-enabled output with unchanged geometry; 0 reload (debug) |
| `msca_scratch` | bool | 0644 | `true` | 1 (default): STREAMOFF parks a kept MSCA output on a scratch area in the ISP buffer; 0 off (debug) |
| `msca_scratch_bytes` | uint | 0444 | `0 (not initialised)` | size of the MSCA scratch area (read only) |
| `msca_scratch_deferred` | uint | 0444 | `0 (not initialised)` | QBUFs of a parked channel held back until its STREAMON (read only) |
| `msca_scratch_frames` | uint | 0444 | `0 (not initialised)` | MSCA completions into the scratch area (read only) |
| `msca_scratch_parks` | uint | 0444 | `0 (not initialised)` | STREAMOFFs that parked the output on the scratch area (read only) |
| `msca_scratch_phys` | uint | 0444 | `0 (not initialised)` | physical address of the MSCA scratch area (read only, 0 = none) |
| `msca_scratch_settle_ticks_max` | uint | 0444 | `0 (not initialised)` | longest wait for parked scratch addresses before an input stop, 10 ms steps (read only) |
| `msca_scratch_settle_timeouts` | uint | 0444 | `0 (not initialised)` | input stops that found a scratch address still in an address FIFO after the wait (read only) |
| `msca_scratch_skips` | uint | 0444 | `0 (not initialised)` | STREAMOFFs that wanted to park but had no (large enough) scratch area (read only) |
| `msca_session_release` | bool | 0644 | `true` | 1 (default): tx-isp STREAMON with the input stopped switches off MSCA outputs left enabled and clears their FIFOs; 0 off (debug) |
| `osd_busy_skips` | uint | 0444 | `0 (not initialised)` | - |
| `osd_commits` | uint | 0444 | `0 (not initialised)` | - |
| `print_level` | int | 0 (load line only) | `1` | isp print level |
| `qbuf_cache_inv` | bool | 0644 | `true` | 1 (default, stock): invalidate the buffer's cache lines at QBUF; 0 off (debug) |
| `qbuf_guard` | bool | 0644 | `true` | Reject framechan QBUF buffers outside rmem (default 1) |
| `snapraw_buffer_bytes` | uint | 0444 | `4U * 1024U * 1024U` | bytes of DMA memory allocated per snapraw capture (lazily) |
| `snapraw_phys_addr` | uint | 0444 | `0 (not initialised)` | physical base of a caller-reserved snapraw DMA region |
| `source_adr_dynamic` | bool | 0644 | `0 (not initialised)` | - |
| `source_adr_internal_enable` | bool | 0644 | `0 (not initialised)` | - |
| `source_adr_oem` | bool | 0444 | `true` | run the stock dynamic ADR and defog lifted from the OEM module (default 1); 0 = the static C ADR/defog |
| `source_ae_compensation` | uint | 0444 | `128U` | - |
| `source_ae_force_packed` | uint | 0644 | `0 (not initialised)` | - |
| `source_ae_hlil` | bool | 0644 | `true` | - |
| `source_ae_hlil_interval` | uint | 0644 | `32` | - |
| `source_ae_hlil_luma` | uint | 0444 | `0 (not initialised)` | - |
| `source_ae_hlil_target` | uint | 0644 | `60` | - |
| `source_ae_oem` | bool | 0444 | `true` | run the stock AE0 algorithm lifted from the OEM module (default 1); 0 = the HLIL AE substitute (needs reload) |
| `source_ae_stats_init` | bool | 0644 | `true` | - |
| `source_af` | bool | 0444 | `true` | T23 AF statistics chain of the stock module: AF block, interrupt and the GetAfHist/SetAfHist/GetAfWeight/SetAfWeight/GetAFMetrices/GetAfZone controls (default 1 = stock, 0 off) |
| `source_awb_hlil` | bool | 0644 | `true` | - |
| `source_awb_hlil_runs` | uint | 0444 | `0 (not initialised)` | - |
| `source_awb_hlil_status` | uint | 0444 | `0 (not initialised)` | - |
| `source_awb_hlil_updates` | uint | 0444 | `0 (not initialised)` | - |
| `source_awb_last_bgain` | uint | 0444 | `0x858` | - |
| `source_awb_last_rgain` | uint | 0444 | `0x820` | - |
| `source_awb_lum_th` | bool | 0644 | `true` | stock per-interrupt AWB luminance threshold 0xb038 from the AE mean (default 1); 0 = fixed startup value |
| `source_bcsh_trace` | bool | 0644 | `0 (not initialised)` | - |
| `source_ccm_tuning_init` | bool | 0644 | `true` | - |
| `source_core_bayer` | uint | 0644 | `UINT_MAX` | T23 Bayer index override; 0xffffffff uses the media-bus code |
| `source_core_tuning_path` | charp | 0644 | `0 (not initialised)` | IQ file override; unset selects /etc/sensor/<sensor>-t23.bin |
| `source_defog_internal_enable` | bool | 0644 | `0 (not initialised)` | - |
| `source_gib_ir_value` | uint | 0644 | `0 (not initialised)` | - |
| `source_lsc_ct` | uint | 0644 | `5000U` | - |
| `source_lsc_initial_flip` | bool | 0644 | `0 (not initialised)` | - |
| `source_lsc_initial_mirror` | bool | 0644 | `0 (not initialised)` | - |
| `source_park_uninitialized_mdns` | bool | 0644 | `0 (not initialised)` | - |
| `source_sdns_internal_enable` | bool | 0644 | `true` | - |
| `source_sdns_wdr` | bool | 0644 | `0 (not initialised)` | - |
| `source_sensor_fps` | uint | 0644 | `0 (not initialised)` | - |
| `source_sharpen_tuning_init` | bool | 0644 | `true` | - |
| `source_total_gain_q16` | uint | 0644 | `0 (not initialised)` | - |
| `t23_isr_enable` | callback | 0644 | `0xffffffffU & ~T23_ISR_UNUSED` | ISP core interrupt enable word (0xb0); default masks the unused sources 12, 13, 16, stock is 0xffffffff |
| `t23_runtime_trace` | int | 0644 | `0 (not initialised)` | enable T23 informational driver logging (KERN_INFO and allow-listed progress notes), default 0 |

- **bring-up build only** (111): `child_platforms`, `csi_rate_override`, `direct_csi_start`, `direct_enable_clks`, `direct_msca_cfg_load`, `direct_msca_qbuf`, `direct_msca_start`, `direct_sensor_stream`, `direct_tisp_stream_regs`, `direct_vic_start`, `enable_irqs_on_streamon`, `log_framechan_payloads`, `name`, `real_platforms`, `recovered_child_probes`, `sensor_i2c_adapter`, `source_adr_height`, `source_adr_initialized`, `source_adr_tuning_init`, `source_adr_width`, `source_ae_hlil_deadband`, `source_ae_hlil_drops`, `source_ae_hlil_ev`, `source_ae_hlil_fast_interval`, `source_ae_hlil_runs`, `source_ae_hlil_sat_permille`, `source_ae_hlil_state`, `source_ae_hlil_status`, `source_ae_hlil_updates`, `source_ae_stats_irqs`, `source_ae_stats_snapshots`, `source_awb_bootstrap_bgain`, `source_awb_bootstrap_rgain`, `source_awb_hlil_bg`, `source_awb_hlil_ct`, `source_awb_hlil_drops`, `source_awb_hlil_interval`, `source_awb_hlil_min_pixels`, `source_awb_hlil_rg`, `source_awb_last_mf_bg`, `source_awb_last_mf_rg`, `source_awb_profile_bbias`, `source_awb_profile_rbias`, `source_awb_static_init`, `source_awb_stats_init`, `source_awb_stats_irqs`, `source_awb_stats_processed_tap`, `source_awb_stats_snapshots`, `source_awb_stats_wide_thresholds`, `source_bcsh_ct_events`, `source_bcsh_ct_region`, `source_bcsh_ev_events`, `source_bcsh_events`, `source_bcsh_neutral`, `source_bcsh_runtime`, `source_bcsh_tuning_init`, `source_ccm_event_count`, `source_ccm_events`, `source_ccm_runtime_ct`, `source_clm_event_count`, `source_clm_events`, `source_clm_region`, `source_clm_runtime_ct`, `source_clm_startup_ct`, `source_clm_tuning_init`, `source_core_bypass`, `source_core_bypass_from_tuning`, `source_core_mode`, `source_core_start`, `source_csccr_init`, `source_csi_stream`, `source_defog_geometry_init`, `source_defog_geometry_runs`, `source_defog_height`, `source_defog_initialized`, `source_defog_tuning_init`, `source_defog_width`, `source_dmsc_sharpness`, `source_dmsc_tuning_init`, `source_dpc_tuning_init`, `source_frame_done`, `source_gamma_tuning_init`, `source_gib_tuning_init`, `source_lsc_event_count`, `source_lsc_events`, `source_lsc_gain_events`, `source_lsc_runtime_ct`, `source_lsc_tuning_init`, `source_mdns_gain_old`, `source_mdns_internal_enable`, `source_mdns_mode`, `source_mdns_paddr`, `source_mdns_restore_count`, `source_mdns_set_count`, `source_mdns_size`, `source_mdns_top_control`, `source_mdns_top_override`, `source_mdns_tuning_init`, `source_mdns_used`, `source_mdns_uv`, `source_msca_curves`, `source_msca_init`, `source_sdns_gain_old`, `source_sdns_tuning_init`, `source_sensor_stream`, `source_stock_stream_regs`, `source_vic_stream`, `source_wb_mode`, `source_ydns_tuning_init`, `text_watch`, `vic_irq_unmask_all`

### T30 (module `tx-isp-t30`, sysfs `/sys/module/tx_isp_t30/parameters/`)

15 parameters in the default build, 0 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `isp_clk` | int | 0 (load line only) | `90000000` | isp core clock |
| `isp_m1_bufs` | int | 0 (load line only) | `2` | isp m1 inter buffers |
| `isp_m2_bufs` | int | 0 (load line only) | `2` | isp inter buffers |
| `ispcrop` | int | 0 (load line only) | `0 (not initialised)` | The function of isp's crop |
| `ispcroptl` | int | 0 (load line only) | `0 (not initialised)` | The begin point of isp's crop |
| `ispcropwh` | int | 0 (load line only) | `0 (not initialised)` | The size of isp's crop |
| `isph` | int | 0 (load line only) | `0 (not initialised)` | The height of isp's output |
| `ispleft` | int | 0 (load line only) | `0 (not initialised)` | The left of isp's output |
| `ispscaler` | int | 0 (load line only) | `0 (not initialised)` | The function of isp's crop |
| `ispscalerwh` | int | 0 (load line only) | `0 (not initialised)` | The size of isp's scaler |
| `isptop` | int | 0 (load line only) | `0 (not initialised)` | The top of isp's output |
| `ispw` | int | 0 (load line only) | `0 (not initialised)` | The width of isp's output |
| `print_level` | int | 0 (load line only) | `1` | isp print level |
| `simple_3a` | bool | 0 (load line only) | `true` | use the calibration-driven recovery AE/AWB path |
| `simple_awb` | bool | 0644 | `true` | enable calibration-driven recovery AWB updates |

### T31 (module `tx-isp-t31`, sysfs `/sys/module/tx_isp_t31/parameters/`)

47 parameters in the default build, 0 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `ae_effect_frame_override` | uint | 0644 | `0 (not initialised)` | Override sensor gain effect delay (0 = tuning, max 14 frames) |
| `ae_fifo_depth_override` | uint | 0644 | `0 (not initialised)` | Override the tuning binary's measured-luma FIFO depth (0 = tuning, max 15) |
| `ae_freeze` | int | 0644 | `0 (not initialised)` | Hold the current sensor exposure tuple (0 = auto, 1 = frozen) |
| `ae_freeze_gain` | int | 0644 | `0 (not initialised)` | Hold only the current sensor and ISP gain tuple (0 = auto, 1 = frozen) |
| `ae_freeze_integration` | int | 0644 | `0 (not initialised)` | Hold only the current sensor integration time (0 = auto, 1 = frozen) |
| `ae_full_frame_mean` | int | 0644 | `0 (not initialised)` | Use the unweighted full-frame AE mean instead of the OEM zone map |
| `csi_dump_enable` | int | 0444 | `0` | Enable CSI periodic dump to /opt/csi.txt (0=off, 1=on) |
| `csi_dump_interval_ms` | int | 0444 | `1000` | CSI dump interval in milliseconds |
| `fcrop_upscale_pct` | uint | 0644 | `100 (`T31_FCROP_UPSCALE_MAX_PCT`)` | front crop: allowed MSCA upscale in percent (default 100 = 2.0x like the vendor, 0 = none, capped at 100) |
| `force_bypass_adr` | int | 0644 | `0` | Force ADR bypass (default: 0 — keep OEM ADR path live unless explicitly isolating it) |
| `force_bypass_defog` | int | 0644 | `0` | Force Defog bypass (default: 0 — match the OEM T31 pipeline) |
| `force_bypass_dpc` | int | 0644 | `0` | Force DPC (bit 2) bypass for AWB bisection |
| `force_bypass_gib` | int | 0644 | `0` | Force GIB (bit 5) bypass (default: 0 — match the OEM T31 pipeline) |
| `force_bypass_lsc` | int | 0644 | `0 (not initialised)` | Force LSC bypass (bits 4 and 6) (default: 0 — use the sensor tuning mesh) |
| `force_bypass_mdns` | int | 0644 | `0` | Force MDNS bypass (default: 0 — MDNS provides temporal denoise) |
| `force_ccm_b` | int | 0644 | `0x400` | Forced CCM blue diagonal gain (0x400 = unity) |
| `force_ccm_bg` | int | 0644 | `0 (not initialised)` | Forced CCM blue-from-green coefficient (signed, 0x400 = unity) |
| `force_ccm_br` | int | 0644 | `0 (not initialised)` | Forced CCM blue-from-red coefficient (signed, 0x400 = unity) |
| `force_ccm_g` | int | 0644 | `0x400` | Forced CCM green diagonal gain (0x400 = unity) |
| `force_ccm_gb` | int | 0644 | `0 (not initialised)` | Forced CCM green-from-blue coefficient (signed, 0x400 = unity) |
| `force_ccm_gr` | int | 0644 | `0 (not initialised)` | Forced CCM green-from-red coefficient (signed, 0x400 = unity) |
| `force_ccm_r` | int | 0644 | `0x400` | Forced CCM red diagonal gain (0x400 = unity) |
| `force_ccm_rb` | int | 0644 | `0 (not initialised)` | Forced CCM red-from-blue coefficient (signed, 0x400 = unity) |
| `force_ccm_rg` | int | 0644 | `0 (not initialised)` | Forced CCM red-from-green coefficient (signed, 0x400 = unity) |
| `force_identity_ccm` | int | 0644 | `0` | Force CCM output to the diagnostic matrix (defaults to identity; default: 0 keeps the OEM CCM path) |
| `force_raw_correction_gain` | int | 0644 | `-1` | Override GIB/DPC gain for raw-correction diagnosis (-1: disabled) |
| `isp_ae_log` | int | 0644 | `0 (not initialised)` | Log per-frame AE diagnostics (0 = off, >= 2: AE_TRACE every N frames) |
| `isp_bypass_all` | int | 0644 | `0` | - |
| `isp_bypass_graph` | int | 0444 | `0 (not initialised)` | Enable the vendor ISP bypass graph (link config 1: raw frames from the VIC on channel 0) (default: 0) |
| `isp_bypass_override` | uint | 0644 | `0` | - |
| `isp_ch0_pre_dequeue_interrupt_process` | int | 0444 | `0 (not initialised)` | isp pre dequeue interrupt process |
| `isp_ch0_pre_dequeue_time` | int | 0444 | `0 (not initialised)` | isp pre dequeue time, unit ms |
| `isp_ch0_pre_dequeue_valid_lines` | int | 0444 | `0 (not initialised)` | isp pre dequeue valid lines |
| `isp_ch1_dequeue_delay_time` | int | 0444 | `0 (not initialised)` | isp pre dequeue time, unit ms |
| `isp_clk` | int | 0444 | `200000000` | isp clock freq |
| `isp_day_night_switch_drop_frame_num` | int | 0444 | `0 (not initialised)` | isp day night switch drop frame number |
| `isp_memopt` | int | 0444 | `1` | isp memory optimize, default 1 (0 = off) |
| `msca_flip_state` | callback | 0444 | `computed` | read-only: flip done by sensor/ISP, bits 0 sensor_h, 1 sensor_v, 2 isp_h, 3 isp_v |
| `print_level` | int | 0444 | `1 (`ISP_WARN_LEVEL`)` | isp print level |
| `qbuf_guard` | bool | 0644 | `true` | Reject framechan QBUF buffers outside rmem (default 1) |
| `sensor_expo_catchups` | uint | 0444 | `0 (not initialised)` | Exposure requests caught while an earlier sensor write was running |
| `t31_awb_impl` | callback | 0644 | `2` | AWB CT detection: 0=reference C, 1=fast C, 2=MXU2 (default; after selftest, fast C otherwise), all bit-exact |
| `t31_awb_selftest` | callback | 0644 | `0 (not initialised)` | write 1: compare reference/fast/MXU2 AWB CT detection on random scenes and time them (dmesg) |
| `t31_awb_selftest_state` | int | 0444 | `0 (not initialised)` | 0 not run, 1 passed, -1 failed |
| `t31_isr_enable` | callback | 0644 | `0xffffffffU & ~T31_ISR_UNUSED` | ISP core interrupt enable word (0xb0); default masks the unused source 16, stock is 0xffffffff |
| `t31_mxu2_cpm` | uint | 0444 | `0 (not initialised)` | CPM 0x10000034 read by the selftest, information only; it is a clock/part register |
| `t31_runtime_trace` | int | 0644 | `0 (not initialised)` | enable T31 runtime informational logging |

### T40 (module `tx-isp-t40`, sysfs `/sys/module/tx_isp_t40/parameters/`)

249 parameters in the default build, 388 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `adr_linear_mode` | array of bool | 0644 | `0 (not initialised)` | - |
| `adr_main_init_stage_limit` | uint | 0644 | `0 (not initialised)` | - |
| `ae_isp_dgain_max` | uint | 0644 | `4096` | - |
| `ae_max_again_from_sensor` | bool | 0644 | `true` | - |
| `ae_sensor_apply_clear_dirty` | bool | 0644 | `true` | - |
| `ae_sensor_apply_force_packed` | uint | 0644 | `0 (not initialised)` | - |
| `ae_sensor_apply_log_skips` | bool | 0644 | `0 (not initialised)` | - |
| `ae_sensor_apply_max_again_index` | uint | 0644 | `25U` | - |
| `ae_soft_max_it_from_sensor` | bool | 0644 | `true` | - |
| `ae_soft_target` | uint | 0644 | `140` | - |
| `ae_soft_updates` | uint | 0444 | `0 (not initialised)` | - |
| `ae_target_comp` | uint | 0444 | `128` | mirror of t40_ae_sc.comp (read-only parameter) |
| `ae_user_max_again_x1024` | uint | 0444 | `0 (not initialised)` | - |
| `ae_user_max_isp_dgain_x1024` | uint | 0444 | `0 (not initialised)` | - |
| `ae_user_max_it` | uint | 0444 | `0 (not initialised)` | - |
| `awb_grayworld_bbias` | uint | 0644 | `1448` | - |
| `awb_grayworld_last_bgain` | uint | 0444 | `0x666` | - |
| `awb_grayworld_last_rgain` | uint | 0444 | `0x5a0` | - |
| `awb_grayworld_rbias` | uint | 0644 | `1193` | - |
| `awb_main_init_stage_limit` | uint | 0644 | `0 (not initialised)` | - |
| `awb_manual_bgain` | uint | 0644 | `0 (not initialised)` | - |
| `awb_manual_rgain` | uint | 0644 | `0 (not initialised)` | - |
| `bcsh_brightness` | uint | 0444 | `128U` | - |
| `bcsh_contrast` | uint | 0444 | `128U` | - |
| `bcsh_ct` | uint | 0644 | `6000` | - |
| `bcsh_ev` | uint | 0644 | `400` | - |
| `bcsh_hue` | uint | 0444 | `128U` | - |
| `bcsh_mode` | uint | 0644 | `2` | OEM tisp_main_init passes 2 |
| `bcsh_saturation` | uint | 0644 | `128U` | - |
| `bcsh_user_full` | bool | 0644 | `true` | - |
| `blc_lit_gain` | uint | 0644 | `212541U` | - |
| `blc_lit_gain_now` | uint | 0444 | `0 (not initialised)` | - |
| `ccm_ct` | uint | 0644 | `5000` | - |
| `ccm_ct_now` | uint | 0444 | `0 (not initialised)` | - |
| `ccm_ct_track` | bool | 0644 | `true` | - |
| `ccm_ev` | uint | 0644 | `400` | - |
| `ccm_ev_now` | uint | 0444 | `0 (not initialised)` | - |
| `clk_name` | charp | 0 (load line only) | `0 (not initialised)` | chose parent clk |
| `cnt_core_ops_init` | uint | 0444 | `0 (not initialised)` | - |
| `cnt_main_long_tgain_update` | uint | 0444 | `0 (not initialised)` | - |
| `cnt_sec_init` | uint | 0444 | `0 (not initialised)` | - |
| `core_bayer_reg8_value` | uint | 0644 | `0x10002` | - |
| `core_isr_frame_deliver_count` | uint | 0444 | `0 (not initialised)` | - |
| `csi_direct_stage_limit` | uint | 0644 | `0 (not initialised)` | - |
| `csi_settle_override` | uint | 0644 | `0 (not initialised)` | - |
| `dmsc_lit_gain` | uint | 0644 | `212541U` | - |
| `dmsc_lit_gain_now` | uint | 0444 | `0 (not initialised)` | - |
| `dmsc_stock_current_overlay` | bool | 0644 | `true` | - |
| `dmsc_stock_high_gain_overlay` | bool | 0644 | `0 (not initialised)` | - |
| `dmsc_stock_overlay_mask` | uint | 0644 | `0xf` | - |
| `dns_gain_ev` | uint | 0644 | `0 (not initialised)` | - |
| `enable_adr_process_work` | bool | 0644 | `0 (not initialised)` | - |
| `enable_adr_reg_writes` | bool | 0644 | `true` | - |
| `enable_ae_sensor_apply` | bool | 0644 | `0 (not initialised)` | - |
| `enable_ae_soft` | bool | 0644 | `0 (not initialised)` | - |
| `enable_awb_grayworld` | bool | 0644 | `0 (not initialised)` | - |
| `enable_awb_reg_writes` | bool | 0644 | `true` | - |
| `enable_awb_set_gain` | bool | 0644 | `true` | - |
| `enable_bcsh` | bool | 0644 | `0 (not initialised)` | - |
| `enable_bcsh_static` | bool | 0644 | `0 (not initialised)` | default off: snapshot does not compose with our CCM (greens shift purple); needs the faithful tisp_bcsh chain |
| `enable_blc_lit` | bool | 0644 | `0 (not initialised)` | - |
| `enable_ccm` | bool | 0644 | `0 (not initialised)` | - |
| `enable_cdns` | bool | 0644 | `0 (not initialised)` | CDNS chroma denoise (TOP bit 19): follows the bank flag like the OEM (set at bank apply); force on with t40_force_active bit 19 (0x80000), off with t40_force_bypass bit 19 |
| `enable_core_bayer_reg8` | bool | 0644 | `0 (not initialised)` | - |
| `enable_core_ops_cb` | bool | 0644 | `true` | - |
| `enable_core_top_sel_reg0c` | bool | 0644 | `0 (not initialised)` | - |
| `enable_dmsc_lit` | bool | 0644 | `0 (not initialised)` | - |
| `enable_dmsc_static` | bool | 0644 | `true` | - |
| `enable_frame_3a` | bool | 0644 | `0 (not initialised)` | - |
| `enable_gib_blc` | bool | 0644 | `0 (not initialised)` | - |
| `enable_gib_lit` | bool | 0644 | `0 (not initialised)` | - |
| `enable_isp_3a_diag` | bool | 0644 | `0 (not initialised)` | - |
| `enable_isp_block_init` | bool | 0644 | `0 (not initialised)` | - |
| `enable_isp_block_init_ae` | bool | 0644 | `0 (not initialised)` | - |
| `enable_isp_block_init_awb` | bool | 0644 | `0 (not initialised)` | - |
| `enable_isp_stats_fanout` | bool | 0644 | `0 (not initialised)` | - |
| `enable_lsc_lit` | bool | 0644 | `0 (not initialised)` | - |
| `enable_mdns` | bool | 0644 | `true` | - |
| `enable_msca_rearm_guard` | bool | 0644 | `0 (not initialised)` | - |
| `enable_msca_spare` | bool | 0644 | `0 (not initialised)` | opt-in until the load-time crash is understood |
| `enable_sdns` | bool | 0644 | `0 (not initialised)` | - |
| `enable_soft_gamma` | bool | 0644 | `0 (not initialised)` | - |
| `enable_t40_color_reg_snapshot` | bool | 0644 | `0 (not initialised)` | - |
| `enable_t40_msca_shadow_fifo_program` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_event_threads` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_main_init_csc_version` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_main_init_dma_kseg0` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_main_init_event_init` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_main_init_scalar_regs` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_main_init_tispinfo_dma` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_main_init_tiziano` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_stream_event_cbs` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_stream_event_init` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_stream_regs_after_video` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tisp_top_regs_final_reapply` | bool | 0644 | `0 (not initialised)` | - |
| `enable_tiziano_param_load` | bool | 0644 | `true` | - |
| `enable_vic_mdma_qbuf_ring` | bool | 0644 | `0 (not initialised)` | - |
| `enable_ydns` | bool | 0644 | `0 (not initialised)` | - |
| `enable_ysp` | bool | 0644 | `0 (not initialised)` | - |
| `force_core_bayer_reg8_value` | bool | 0644 | `0 (not initialised)` | - |
| `force_local_frame_streamoff` | bool | 0644 | `0 (not initialised)` | - |
| `force_tisp_main_init_yuv_input_csc_version` | bool | 0644 | `0 (not initialised)` | - |
| `frame_3a_ch1_idle_ms` | uint | 0644 | `300` | - |
| `frame_3a_ch1_samples` | uint | 0444 | `0 (not initialised)` | - |
| `frame_3a_completed_seq` | uint | 0444 | `0 (not initialised)` | - |
| `frame_3a_completed_uv_phys` | uint | 0444 | `0 (not initialised)` | - |
| `frame_3a_completed_y_phys` | uint | 0444 | `0 (not initialised)` | - |
| `frame_3a_interval` | uint | 0644 | `3` | - |
| `frame_3a_samples` | uint | 0444 | `0 (not initialised)` | - |
| `frame_3a_slide_window` | bool | 0644 | `true` | - |
| `frame_3a_work_runs` | uint | 0444 | `0 (not initialised)` | - |
| `frame_awb_bggr_fast` | bool | 0644 | `true` | - |
| `framechan_neutral_uv_channel_mask` | uint | 0644 | `0x1U` | - |
| `framechan_neutral_uv_on_done` | bool | 0644 | `0 (not initialised)` | - |
| `framechan_neutral_uv_value` | uint | 0644 | `0x80U` | - |
| `gib_blc_offset` | uint | 0644 | `0x100` | - |
| `gib_lit_final_gain` | uint | 0644 | `1261U` | - |
| `gib_lit_gain` | uint | 0644 | `212541U` | - |
| `gib_lit_gain_now` | uint | 0444 | `0 (not initialised)` | - |
| `irq_frame_done_delay_ms` | uint | 0644 | `0 (not initialised)` | - |
| `irq_frame_done_require_content` | bool | 0644 | `0 (not initialised)` | - |
| `isp_block_init_adr_ret` | int | 0444 | `0 (not initialised)` | - |
| `isp_block_init_awb_ret` | int | 0444 | `0 (not initialised)` | - |
| `isp_block_init_count` | uint | 0444 | `0 (not initialised)` | - |
| `isp_block_init_ran` | bool | 0444 | `0 (not initialised)` | - |
| `isp_block_init_skip_no_blob` | uint | 0444 | `0 (not initialised)` | - |
| `isp_block_init_stage_limit` | uint | 0644 | `0 (not initialised)` | - |
| `isp_ch0_pre_dequeue_time` | int | 0 (load line only) | `0 (not initialised)` | isp pre dequeue time, unit ms |
| `isp_clk` | int | 0 (load line only) | `200000000` | isp clock freq |
| `isp_dual_buf` | int | 0 (load line only) | `65538` | isp dual buff |
| `isp_irq_frame_done_status0_mask` | uint | 0644 | `0x1U` | - |
| `isp_memopt` | int | 0 (load line only) | `0 (not initialised)` | isp memory optimize |
| `isp_stats_fanout_adr_status0_mask` | uint | 0644 | `0 (not initialised)` | - |
| `isr_oem_hw_refresh` | int | 0644 | `0 (not initialised)` | core IRQ frame-done: call tisp_hardware_reg_refresh() (OEM 0x7b138). default 0 |
| `lsc_lit_ct` | uint | 0644 | `5000` | CT used for the static init interp |
| `lsc_lit_ct_cool_ct` | uint | 0644 | `6500` | - |
| `lsc_lit_ct_cool_x` | uint | 0644 | `730` | - |
| `lsc_lit_ct_now` | uint | 0444 | `0 (not initialised)` | last CT pushed into the LSC chain |
| `lsc_lit_ct_track` | bool | 0644 | `true` | - |
| `lsc_lit_ct_warm_ct` | uint | 0644 | `3300` | - |
| `lsc_lit_ct_warm_x` | uint | 0644 | `1074` | - |
| `lsc_lit_gain` | uint | 0644 | `0 (not initialised)` | 16.16 log-gain for the static init interp |
| `mdns_buf_phys` | uint | 0644 | `0 (not initialised)` | - |
| `mdns_dbg_init_ret` | uint | 0444 | `0xdeadbeefU` | - |
| `mdns_dbg_reg68` | uint | 0444 | `0 (not initialised)` | - |
| `mdns_wr_bitmap` | array of uint | 0444 | `0 (not initialised)` | - |
| `msca_fifo_depth` | array of uint | 0444 | `0 (not initialised)` | - |
| `msca_fifo_oob_drop_count` | array of uint | 0444 | `0 (not initialised)` | - |
| `msca_rearm_guard_max_skips` | uint | 0644 | `3U` | - |
| `msca_spare_phys` | uint | 0644 | `0x0c800000U` | - |
| `msca_spare_violation_count` | array of uint | 0444 | `0 (not initialised)` | - |
| `oem_event_pre_csi_delay_ms` | uint | 0644 | `0 (not initialised)` | - |
| `oem_event_pre_csi_stage_limit` | uint | 0644 | `0 (not initialised)` | - |
| `oem_event_pre_csi_stream` | bool | 0644 | `0 (not initialised)` | - |
| `panic_dump_phys` | uint | 0644 | `0xe9f0000U` | - |
| `print_level` | int | 0 (load line only) | `1` | isp print level |
| `proc_live_reads` | bool | 0644 | `0 (not initialised)` | - |
| `running_mode` | uint | 0444 | `0 (not initialised)` | - |
| `sensor_full_height_override` | uint | 0644 | `0 (not initialised)` | - |
| `sensor_full_width_override` | uint | 0644 | `0 (not initialised)` | - |
| `skip_tisp_deinit` | bool | 0644 | `true` | - |
| `t40_adr_ev_shift` | int | 0644 | `0 (not initialised)` | - |
| `t40_adr_on` | bool | 0644 | `true` | 1 (with t40_gates_oem_offset=1) runs the ADR chain and clears TOP bit 7 after a successful init (default 1) |
| `t40_ae_af_enable` | bool | 0644 | `true` | antiflicker in the frame AE (SetAntiFlickerAttr) |
| `t40_ae_af_night_bin` | bool | 0644 | `true` | - |
| `t40_ae_af_step_now` | uint | 0444 | `off */ ...` | integration lines per half mains period in use, 0 = off |
| `t40_ae_api` | callback | 0444 | `computed` | - |
| `t40_ae_fine_trim` | uint | 0644 | `1` | OEM fine convergence of the frame AE (#239): trims the ISP digital gain toward target/luma at the integration ceiling; 0 = old behaviour |
| `t40_ae_fine_trims` | uint | 0444 | `0 (not initialised)` | fine-trim moves done (diagnostic) |
| `t40_ae_hi_thr` | uint | 0644 | `0 (not initialised)` | - |
| `t40_ae_max_again_index_eff` | uint | 0444 | `0 (not initialised)` | - |
| `t40_ae_split_legacy` | uint | 0644 | `0 (not initialised)` | - |
| `t40_ae_split_moves` | uint | 0444 | `0 (not initialised)` | exposure-neutral OEM re-splits done (diagnostic) |
| `t40_ae_stats_src` | uint | 0644 | `0 (not initialised)` | - |
| `t40_ae_target_oem` | uint | 0644 | `1` | - |
| `t40_ae_trade_sensor_gain` | bool | 0644 | `true` | - |
| `t40_allow_antiflick` | bool | 0644 | `true` | 0040/0050: Set/Get hand-translated and device-tested (AF2) - the app (anti-flicker setting 1 -> SetAntiFlickerAttr 50 Hz) is the vendor way |
| `t40_antiflick_hz` | uint | 0644 | `bin default (60 Hz), 50/60 = set after AE init like the v...` | 0 = bin default (60 Hz), 50/60 = set after AE init like the vendor app does |
| `t40_awb_ctd` | bool | 0644 | `gray-world fallback */ ...` | T40-AWB-CTD: run the lifted tisp_awb_ct_detect (8 KB fragment lift, heap-corruption suspect N9); off = gray-world fallback |
| `t40_az_live_max_w` | uint | 0644 | `0 (not initialised)` | - |
| `t40_bringup_profile` | bool | 0644 | `true` | - |
| `t40_c2_flip` | uint | 0644 | `0` | 1 = sensor flip hooks on (experimental: sc830ai frames unchanged, may freeze; vendor ignores sensor flip modes) |
| `t40_c2_state` | callback | 0444 | `computed` | - |
| `t40_c2_stray` | uint | 0644 | `0 (not initialised)` | - |
| `t40_clm` | bool | 0644 | `true` | 1 (default): CLM colour LUT (TOP bit 16) from the bank flag with the vendor CT follow-up like the OEM; 0: CLM bypassed |
| `t40_core_fixup_shadow0` | bool | 0644 | `0 (not initialised)` | - |
| `t40_csc_unsafe` | uint | 0644 | `0 (not initialised)` | allow the lifted OEM CSC get/set (stack/bss overflow, debug only) |
| `t40_defog_dn_cache_reset` | bool | 0644 | `true` | defog: 1 (default) drops the curve/transmission caches on a day/night refresh, 0 = OEM (old bank values stay cached) |
| `t40_defog_motion_var` | bool | 0644 | `true` | defog: 1 (default) motion weighting uses the variance over the 5x5 window, 0 = OEM (last cell only) |
| `t40_defog_on` | bool | 0644 | `true` | 1 (with t40_gates_oem_offset=1) runs the defog chain and clears TOP bit 11 after a successful init (default 1) |
| `t40_defog_sum_air` | uint | 0444 | `0 (not initialised)` | - |
| `t40_defog_sum_trans` | uint | 0444 | `0 (not initialised)` | - |
| `t40_defog_trans_reset_all` | bool | 0644 | `true` | defog: 1 (default) drops all 256 transmission cache entries on an EV/strength change, 0 = OEM (only 180) |
| `t40_dmsc_day_follow` | bool | 0644 | `true` | 1 (default): with t40_dmsc_follow the literal DMSC chain also runs in day mode (OEM behaviour); 0: static day DMSC block |
| `t40_dmsc_day_restores` | uint | 0444 | `0 (not initialised)` | - |
| `t40_dmsc_follow` | bool | 0644 | `true` | 1 (default): DMSC follows the active day/night bank and the total gain like the OEM (literal chain); 0 = old profile-1 behaviour (static DMSC) |
| `t40_dn_keep_awb1` | bool | 0644 | `true` | 1 (default): keep AWB1 (TOP bit 6) at the day-bank state at night, avoids the grey/green-magenta day picture after night->day; 0: follow the night bank |
| `t40_dns_ev_boost` | uint | 0644 | `0 (not initialised)` | - |
| `t40_dpc_on` | bool | 0644 | `true` | 1 (with t40_gates_oem_offset=1) runs the DPC chain and clears TOP bit 4 after a successful init (default 1) |
| `t40_dqbuf_wait_ms` | uint | 0644 | `0 (not initialised)` | frame channel DQBUF waits up to this many ms for a done frame (0 = return ENODATA at once) |
| `t40_ev_follow` | bool | 0644 | `true` | - |
| `t40_ev_now` | uint | 0444 | `0 (not initialised)` | - |
| `t40_exit_stats_off` | bool | 0644 | `true` | exit: also clear the AE/AWB statistics enables (0x19000/0x18000) before freeing their buffers |
| `t40_force_active` | uint | 0644 | `0x6020U` | - |
| `t40_force_bypass` | uint | 0644 | `0x20000U` | - |
| `t40_gamma_ev` | bool | 0644 | `true` | T40: OEM gamma EV follow-up (bin strength nodes; 0 = fixed bin curve, the pre-#204 behaviour) |
| `t40_gamma_ev_str` | uint | 0444 | `255U` | - |
| `t40_gamma_ev_writes` | uint | 0444 | `0 (not initialised)` | - |
| `t40_gamma_from_bin` | bool | 0644 | `true` | - |
| `t40_gates_day_blc` | bool | 0644 | `0 (not initialised)` | - |
| `t40_gates_day_gib_blc` | uint | 0644 | `0 (not initialised)` | - |
| `t40_gates_oem_offset` | bool | 0644 | `true` | 1 (default) reads the bin bypass flags at +0x00 for linear sensors like stock tisp_main_init; 0 = old profile behaviour (always +0x20, the WDR set) |
| `t40_generic_profile` | bool | 0644 | `0 (not initialised)` | - |
| `t40_isr_ch1_direct` | bool | 0644 | `true` | - |
| `t40_lce_on` | bool | 0644 | `true` | 1 (with t40_gates_oem_offset=1) runs the LCE chain and clears TOP bit 21 after a successful init (default 1) |
| `t40_msca_fifo_addr_tag_mode` | uint | 0644 | `0U` | - |
| `t40_msca_vendor_fixups` | bool | 0644 | `0 (not initialised)` | - |
| `t40_nv12_height_align` | uint | 0644 | `0` | - |
| `t40_profile_direct_vic_feed` | bool | 0644 | `0 (not initialised)` | - |
| `t40_profile_force_vic_mdma_qbuf_ring` | bool | 0644 | `0 (not initialised)` | - |
| `t40_profile_isp_irq_passthrough` | bool | 0644 | `true` | - |
| `t40_profile_no_direct_addr_source` | uint | 0644 | `0U` | - |
| `t40_profile_no_direct_irq_defaults` | bool | 0644 | `true` | - |
| `t40_quality_profile` | uint | 0644 | `1` | - |
| `t40_quality_top40_override` | uint | 0644 | `0 (not initialised)` | - |
| `t40_release_reset_count` | uint | 0444 | `0 (not initialised)` | - |
| `t40_rmmod_skip_slake` | bool | 0644 | `true` | - |
| `t40_runtime_trace` | int | 0644 | `0 (not initialised)` | 1 = log bring-up and progress notes (default 0: only warnings and errors) |
| `t40_sensor_stream_off_on_stop` | bool | 0644 | `true` | - |
| `t40_stock_host_init_mask` | uint | 0644 | `0 (not initialised)` | - |
| `t40_tisp_par` | int | 0444 | `0 (not initialised)` | 1 enables /dev/tisp parameter get/set (debug build only, needs CAP_SYS_RAWIO) |
| `t40_tisp_stream_vendor` | bool | 0644 | `0 (not initialised)` | - |
| `t40_tuning_bin_path` | charp | 0644 | `""` | - |
| `t40_vawb_diag` | callback | 0444 | `computed` | - |
| `t40_vawb_faults` | uint | 0444 | `0 (not initialised)` | - |
| `t40_vawb_frames` | uint | 0444 | `0 (not initialised)` | - |
| `t40_vawb_loop_trips` | uint | 0444 | `0 (not initialised)` | - |
| `t40_vawb_tbuf` | callback | 0444 | `computed` | - |
| `t40_ysp_on` | bool | 0644 | `true` | 1 lets the bin gates enable YSP (sharpening) despite t40_force_bypass bit 17; active only after a successful init (default 0) |
| `tisp_main_init_csc_version_value` | uint | 0644 | `2` | - |
| `tisp_main_init_reg88_override` | uint | 0644 | `0xffffffffU` | - |
| `tisp_main_init_top40_value` | uint | 0644 | `0x7fffeeff` | - |
| `unresolved_jalr_count` | uint | 0444 | `0 (not initialised)` | - |
| `vic_mdma_qbuf_ring_ctrl_value` | uint | 0644 | `0 (not initialised)` | - |
| `vic_mdma_qbuf_ring_stride_override` | uint | 0644 | `0 (not initialised)` | - |
| `vic_mdma_qbuf_ring_uv_offset_override` | uint | 0644 | `0 (not initialised)` | - |
| `ysp_gain_ev` | uint | 0644 | `0 (not initialised)` | - |
| `ysp_gain_ev_now` | uint | 0444 | `0 (not initialised)` | - |
| `ysp_init_ret` | int | 0444 | `-1` | - |
| `ysp_refresh_blocked` | uint | 0444 | `0 (not initialised)` | - |
| `ysp_refresh_count` | uint | 0444 | `0 (not initialised)` | - |

- **TX_ISP_LOAD_STATS=y only** (3): `t40_adr_cyc`, `t40_adr_lowmem`, `t40_ls_bench`
- **TX_ISP_LOAD_STATS=y only; bring-up build only** (3): `t40_ls_reg_cyc`, `t40_ls_reg_r`, `t40_ls_reg_w`
- **bring-up build only** (382): `activate_core_clk_mask`, `activate_core_direct`, `activate_core_probe_clk_mask`, `activate_core_probe_state`, `activate_core_state_only`, `adr_forced_blocked`, `adr_main_init_direct_count`, `adr_main_init_direct_ret`, `adr_main_init_direct_stage`, `ae_isp_dgain_cur`, `ae_isp_dgain_updates`, `ae_soft_cur_again`, `ae_soft_cur_it`, `ae_soft_hyst`, `ae_soft_last_mean`, `ae_soft_max_it`, `ae_soft_min_it`, `awb_control_frozen`, `awb_control_mode`, `awb_grayworld_interval`, `awb_grayworld_irqs`, `awb_grayworld_updates`, `cdns_dbg_init_ret`, `cdns_gain_now`, `cdns_init_count`, `cdns_refresh_count`, `cdns_skip_count`, `cdns_thresh`, `core_dqbuf_enodata_count`, `core_irq_cfg0_value`, `core_irq_cfg1_value`, `core_irq_errmask0_value`, `core_irq_errmask1_value`, `core_irq_fd_enodata_count`, `core_irq_mask0_value`, `core_irq_mask1_value`, `core_isr_frame_deliver_last_ret`, `core_isr_oem_frame_event`, `core_leg_a_iter_count`, `core_leg_b_iter_count`, `core_msca_fifo_write_count`, `core_ops_cb_call_count`, `core_ops_cb_core_off`, `core_ops_cb_core_path_count`, `core_ops_cb_core_skip`, `core_ops_cb_fire_gate`, `core_ops_cb_fire_require_pending`, `core_ops_cb_first_cb`, `core_ops_cb_first_dev`, `core_ops_cb_first_sec0`, `core_ops_cb_first_status0`, `core_ops_cb_first_status1`, `core_ops_cb_first_walk_index`, `core_ops_cb_first_who`, `core_ops_cb_gate_bypass_count`, `core_ops_cb_gate_skip_count`, `core_ops_cb_gate_skip_nocore_count`, `core_ops_cb_gate_skip_nopending_count`, `core_ops_cb_jiffy_calls`, `core_ops_cb_jiffy_calls_max`, `core_ops_cb_last_cb`, `core_ops_cb_last_dev`, `core_ops_cb_last_irq`, `core_ops_cb_last_is_core`, `core_ops_cb_last_pending`, `core_ops_cb_last_ret`, `core_ops_cb_last_sec_status0`, `core_ops_cb_last_status0`, `core_ops_cb_last_status1`, `core_ops_cb_max_per_jiffy`, `core_ops_cb_slot_mask`, `core_ops_cb_slot_skip`, `core_ops_cb_storm_count`, `core_ops_cb_storm_reset`, `core_ops_cb_storm_tripped`, `core_ops_cb_walk_index`, `core_ops_cb_walk_mask`, `core_ops_cb_walk_path_count`, `core_ops_cb_walk_skip`, `core_start_mode_804_value`, `day_awb_bgain`, `day_awb_rgain`, `delegate_framechan_ioctl`, `delegate_tx_isp_ioctl`, `disable_dmsc_lit`, `dmsc_sharpness`, `dns_ev_applied`, `enable_core_event_gate_1700c`, `enable_core_event_oem_state_sequence`, `enable_core_event_state_2_before_boot`, `enable_core_event_state_3`, `enable_core_event_state_4`, `enable_core_event_tisp_main_init`, `enable_core_irq_after_video`, `enable_core_irq_oem_enable_path`, `enable_core_irq_on_stream`, `enable_core_start_800`, `enable_core_system_reg_write`, `enable_framechan_wait_irq_done`, `enable_framechan_wait_msca_poll`, `enable_irq_frame_done_event`, `enable_isp_irq_ack_only`, `enable_isp_irq_post_dispatch_ack`, `enable_msca_cfg_load_pulse`, `enable_proc_jz_sensor_read`, `enable_t40_msca_cfg_after_video`, `enable_t40_msca_cfg_on_set_fmt`, `enable_t40_msca_cfg_program`, `enable_t40_msca_oem_cfg_load`, `enable_t40_msca_scaler_level_control`, `enable_t40_msca_shadow_commit`, `enable_t40_msca_shadow_kick_834`, `enable_tisp_desc_oem_fill`, `enable_tisp_ipc_oem_gate`, `enable_tisp_ipc_trigger`, `enable_tisp_ipc_trigger_1000`, `enable_tisp_ipc_trigger_1004`, `enable_tisp_main_init_dma_regs`, `enable_tisp_main_init_msca_oem_order`, `enable_tisp_main_init_param_alloc`, `enable_tisp_main_init_scalar_regs_after_video`, `enable_tisp_main_init_sensor_init`, `enable_tisp_main_init_size_reg`, `enable_tisp_main_init_state`, `enable_tisp_main_init_tail_regs`, `enable_tisp_main_init_yuv_input_csc_version`, `enable_tisp_process_init`, `enable_tisp_stream_event_cbs_before_1008`, `enable_tisp_stream_on`, `enable_tisp_stream_preamble_regs`, `enable_tisp_stream_reg_1008_286`, `enable_tisp_stream_reg_1008_value`, `enable_tisp_stream_reg_1010_zero`, `enable_tisp_stream_reg_1014_zero`, `enable_tisp_stream_regs`, `enable_tisp_stream_setup_regs`, `enable_tisp_stream_start_1060`, `enable_vic_irq_ack_filter`, `enable_vic_mdma_internal_ring`, `enable_vic_mdma_kick_370`, `enable_vic_mdma_no_ring`, `enable_vic_mdma_qbuf_rearm_irq`, `flight_recorder`, `fr2_capture`, `fr2_ev_seq`, `fr2_open_seq`, `fr2_reg_seq`, `frame_3a_highlight_level`, `frame_3a_highlight_limit_permille`, `frame_3a_last_highlight_permille`, `frame_3a_last_luma`, `frame_3a_last_u`, `frame_3a_last_v`, `frame_3a_target`, `frame_3a_window_misses`, `frame_3a_work_busy`, `frame_awb_hyst`, `frame_awb_target`, `framechan_dma_sync_dqbuf`, `framechan_dma_sync_dqbuf_dir`, `framechan_dma_sync_qbuf`, `framechan_dma_sync_qbuf_dir`, `framechan_repair_dqbuf`, `framechan_wait_irq_timeout_ms`, `framechan_wait_msca_timeout_ms`, `gib_lit_bayer_now`, `gib_lit_dgain01_now`, `gib_lit_dgain23_now`, `gib_lit_dgain_shift_now`, `gib_lit_map_now`, `irq_frame_done_active_fallback`, `irq_frame_done_addr_source`, `irq_frame_done_channel_mask`, `irq_frame_done_count_only`, `irq_frame_done_defer`, `irq_frame_done_defer_fifo_read`, `irq_frame_done_delivery_mode`, `irq_frame_done_dry_run`, `irq_frame_done_event_limit`, `irq_frame_done_local_match_buf`, `irq_frame_done_local_touch_buf`, `irq_frame_done_min_change_words`, `irq_frame_done_min_nonzero_words`, `irq_frame_done_require_active`, `irq_frame_done_require_fifo`, `irq_frame_done_source_mask`, `irq_func_cb_bank2_dispatch_count`, `irq_func_cb_bank2_null_count`, `irq_func_cb_bank2_tested_count`, `irq_func_cb_dispatch_count`, `irq_func_cb_frame_calls`, `irq_func_cb_frame_count`, `irq_func_cb_last_actual_index`, `irq_func_cb_last_bank`, `irq_func_cb_last_frame_calls`, `irq_func_cb_last_frame_primary_mask`, `irq_func_cb_last_frame_secondary_mask`, `irq_func_cb_last_frame_slots0`, `irq_func_cb_last_frame_slots1`, `irq_func_cb_last_frame_slots2`, `irq_func_cb_last_slot`, `irq_func_cb_last_slot_valid`, `irq_func_cb_log_count`, `irq_func_cb_null_count`, `irq_func_cb_oob_count`, `irq_func_cb_slot_count`, `irq_func_cb_stride_mismatch_count`, `irq_func_cb_tested_count`, `isp_block_init_ae_ret`, `isp_irq_ack_only_disable_after`, `isp_irq_frame_done_sec_status0_mask`, `isp_irq_frame_done_status1_mask`, `isr_drain_diag`, `isr_oem_hw_refresh_count`, `isr_oem_hw_refresh_out`, `isr_oem_hw_refresh_ret`, `isr_oem_state744`, `isr_oem_state764`, `isr_oem_state_load`, `isr_oem_state_load_bad`, `isr_oem_state_load_count`, `isr_spin_bail_count`, `isr_spin_budget`, `isr_tail_poll`, `isr_tail_poll_count`, `isr_wlut_trace`, `isr_wlut_trace_regs`, `lsc_lit_ct_max`, `lsc_lit_ct_min`, `mdns_buf_size`, `msca_cfg_direct_count`, `msca_cfg_direct_last_ret`, `msca_kick834_count`, `msca_kick834_last_value`, `msca_peek`, `msca_peek_groups`, `msca_spare_drop_count`, `msca_spare_push_count`, `panic_dump_enable`, `panic_dump_logbuf_len`, `panic_dump_logbuf_ptr`, `rmem_base_now`, `rmem_size_now`, `sensor_fps_packed`, `sensor_i2c_adapter_fallback`, `skip_core_mmio_writes`, `skip_csi_stream`, `skip_legacy_msca_attr`, `skip_sensor_core_init`, `skip_sensor_video_stream`, `skip_tisp_channel_start`, `skip_vic_irq_enable`, `skip_vic_mdma_writes`, `skip_vic_mipi_start`, `skip_vic_stream`, `t40_ae_diag`, `t40_ae_force_auto`, `t40_ae_limits`, `t40_ae_par_calc`, `t40_ae_par_calc_count`, `t40_ae_sensor_alloc_again`, `t40_awb_proc_stages`, `t40_awb_spec_regs`, `t40_awb_stats_ped`, `t40_awb_stats_ped_now`, `t40_awb_step`, `t40_awb_tgain_last`, `t40_az_deferred`, `t40_az_failed`, `t40_az_live`, `t40_az_live_max_h`, `t40_az_stored`, `t40_bayer_oem_mapper_check`, `t40_core_fixup_40`, `t40_core_fixup_88`, `t40_ct_lsc_writes`, `t40_ct_update`, `t40_dn_event_last_ret`, `t40_dn_event_runs`, `t40_ev_cur`, `t40_ev_cur_n`, `t40_exit_vic_reset`, `t40_fr_big`, `t40_fr_regs`, `t40_gates_awb`, `t40_gates_keep_blc`, `t40_gates_offset_now`, `t40_gates_reg6c`, `t40_gates_reg6c_value`, `t40_gates_reg6c_writes`, `t40_gib_again_calls`, `t40_gib_blc_delta`, `t40_gib_dgain_calls`, `t40_gib_dgain_last`, `t40_gib_dgain_last_in`, `t40_gib_dgain_ok`, `t40_gib_force_init`, `t40_gib_init_calls`, `t40_gib_init_ret`, `t40_gib_oem_active`, `t40_gib_real`, `t40_gib_status_word`, `t40_isr_ch1_irq`, `t40_isr_ch1_pop`, `t40_loopguard`, `t40_msca_cfg_1602c_value`, `t40_msca_cfg_direct`, `t40_msca_ch1_kick`, `t40_msca_clear_load_after_program`, `t40_msca_coeff_override`, `t40_msca_fifo_addr_tag_write`, `t40_msca_fifo_ch0_uv_ctrl`, `t40_msca_fifo_ch0_y_ctrl`, `t40_msca_fifo_ch1_uv_ctrl`, `t40_msca_fifo_ch1_y_ctrl`, `t40_msca_fifo_ch2_uv_ctrl`, `t40_msca_fifo_ch2_y_ctrl`, `t40_msca_fifo_ctrl_after_qbuf`, `t40_msca_fifo_ctrl_on_qbuf`, `t40_msca_fifo_ctrl_on_set_fmt`, `t40_msca_late_work_en`, `t40_msca_oem_1602c`, `t40_msca_oem_coeffs`, `t40_msca_oem_stop`, `t40_msca_oem_stop_count`, `t40_msca_rewrite_coeffs_late`, `t40_msca_rewrite_full_size_late`, `t40_msca_scale_active_only`, `t40_msca_scaler_level`, `t40_msca_shadow_cmd_mask`, `t40_msca_shadow_kick_reg`, `t40_msca_shadow_kick_value`, `t40_msca_shadow_write_load_mask`, `t40_msca_vendor_fixups_2c`, `t40_msca_work_cfg_override`, `t40_msca_work_en_dma_bits`, `t40_msca_work_en_dma_mask`, `t40_msca_work_handshake`, `t40_quality_raw_init`, `t40_release_reset`, `t40_release_reset_last_ack`, `t40_tuning_bcsh_bypass_now`, `t40_tuning_blc_bypass_now`, `t40_tuning_gib_bypass_now`, `t40_tuning_lsc_bypass_now`, `t40_tuning_top_flags_now`, `t40_vawb_ct`, `t40_vawb_debug`, `t40_vawb_dump_req`, `t40_vawb_ev`, `t40_vawb_loop_pc`, `t40_vawb_run`, `t40_vawb_trace`, `t40_vendor_expo_count`, `t40_vendor_expo_last_ret`, `t40_vendor_it_default`, `t40_vic_frame_output`, `t40_vic_frame_output_msca_ctrl`, `t40_vic_frame_output_msca_ctrl2`, `t40_vic_frame_output_msca_e0_clear`, `t40_vic_frame_output_msca_word74`, `t40_vic_frame_output_msca_word8c`, `t40_vic_frame_output_msca_words`, `t40_vic_frame_output_msca_work28`, `t40_worker_stat`, `tisp_ipc_1000_delay_ms`, `tisp_main_init_reg20_value`, `tisp_main_init_reg48_value`, `tisp_main_init_reg60_value`, `tisp_main_init_reg6c_value`, `tisp_stream_reg_1008_offset`, `tisp_stream_reg_1008_value`, `trace_core_top_writes`, `vendor_per_frame`, `vendor_per_frame_regs`, `vic_mdma_internal_channel_mask`, `vic_mdma_internal_count`, `vic_mdma_internal_fmt`, `vic_mdma_internal_layout`, `vic_mdma_internal_phys_base`, `vic_mdma_internal_size`, `ysp_ev_applied`

### T41 (module `tx-isp-t41`, sysfs `/sys/module/tx_isp_t41/parameters/`)

163 parameters in the default build, 91 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `af_bad_index` | uint | 0444 | `0 (not initialised)` | - |
| `af_irqs` | uint | 0444 | `0 (not initialised)` | - |
| `af_live` | int | 0444 | `0 (not initialised)` | - |
| `af_runs` | uint | 0444 | `0 (not initialised)` | - |
| `awb_diag` | array of uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_bgain` | uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_cal` | array of uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_change` | uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_ct_cal` | uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_det` | array of uint | 0444 | `0` | - |
| `awb_diag_frz` | uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_frz_th` | uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_mode` | uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_p` | array of uint | 0444 | `0 (not initialised)` | live AWB param-block fields (fable colour diag) |
| `awb_diag_raw` | array of uint | 0444 | `0 (not initialised)` | - |
| `awb_diag_rgain` | uint | 0444 | `0 (not initialised)` | - |
| `awb_dump_req` | uint | 0644 | `0 (not initialised)` | - |
| `clk_name` | charp | 0 (load line only) | `"vpll"` | select the isp parent clock |
| `clka_name` | charp | 0 (load line only) | `"sclka"` | select the axi bus parent clock |
| `clks_name` | charp | 0 (load line only) | `"sclka"` | select the scaler parent clock |
| `csc_latch_skipped` | uint | 0444 | `0 (not initialised)` | - |
| `csc_latches` | uint | 0444 | `0 (not initialised)` | - |
| `defog_evs` | uint | 0444 | `0 (not initialised)` | - |
| `defog_frames` | uint | 0444 | `0 (not initialised)` | - |
| `defog_irqs` | uint | 0444 | `0 (not initialised)` | - |
| `defog_live` | uint | 0444 | `0 (not initialised)` | - |
| `direct_mode` | int | 0 (load line only) | `0 (not initialised)` | isp direct mode |
| `draw_commits` | uint | 0444 | `0 (not initialised)` | - |
| `heap_decoy_hits` | uint | 0444 | `0 (not initialised)` | - |
| `heap_watch_hits` | uint | 0444 | `0 (not initialised)` | - |
| `irq_func_cb_rejects` | uint | 0444 | `0 (not initialised)` | - |
| `isp_bypass_graph` | int | 0444 | `0 (not initialised)` | Enable the VIC MDMA raw-capture bypass graph (default: 0) |
| `isp_ch0_pre_dequeue_time` | int | 0 (load line only) | `0 (not initialised)` | isp pre dequeue time, unit ms |
| `isp_clk` | int | 0 (load line only) | `300000000` | isp clock freq |
| `isp_clka` | int | 0 (load line only) | `550000000` | isp axi bus clock freq (default 550 MHz, clamped to <= 550 MHz) |
| `isp_clks` | int | 0 (load line only) | `550000000` | isp scaler clock freq |
| `isp_memopt` | int | 0 (load line only) | `1` | isp memory optimize |
| `ivdc_mem_line` | int | 0 (load line only) | `0 (not initialised)` | ivdc mem line |
| `ivdc_threshold_line` | int | 0 (load line only) | `0 (not initialised)` | ivdc threshold line |
| `msca_ctl_deferred` | uint | 0444 | `0 (not initialised)` | - |
| `msca_ctl_failed` | uint | 0444 | `0 (not initialised)` | - |
| `msca_ctl_live` | uint | 0444 | `0 (not initialised)` | - |
| `msca_fmt_refused` | uint | 0444 | `0 (not initialised)` | - |
| `msca_frame_enables` | uint | 0444 | `0 (not initialised)` | - |
| `msca_latch_timeouts` | uint | 0444 | `0 (not initialised)` | - |
| `msca_latch_wait_max_us` | uint | 0444 | `0 (not initialised)` | - |
| `msca_live_disables` | uint | 0444 | `0 (not initialised)` | - |
| `msca_start_latches` | uint | 0444 | `0 (not initialised)` | - |
| `osd_commits` | uint | 0444 | `0 (not initialised)` | - |
| `print_level` | int | 0 (load line only) | `1` | isp print level |
| `proc_watch_hits` | uint | 0444 | `0 (not initialised)` | - |
| `proc_watch_rb_hits` | uint | 0444 | `0 (not initialised)` | - |
| `qbuf_guard` | bool | 0644 | `true` | Reject framechan QBUF buffers outside rmem (default 1) |
| `t41_adr_bad_dma` | uint | 0444 | `~0U` | - |
| `t41_adr_bypass` | int | 0444 | `-1` | ADR bypass policy: -1 follows calibration (default), 0 enables, 1 bypasses; boot-only diagnostic |
| `t41_adr_dropped` | uint | 0444 | `~0U` | - |
| `t41_adr_error` | int | 0444 | `-ENODEV` | - |
| `t41_adr_frames` | uint | 0444 | `~0U, t41_adr_dropped = ~0U` | - |
| `t41_adr_interval` | uint | 0644 | `1` | evaluate ADR every n-th frame (1=every frame, stock) |
| `t41_adr_irqs` | uint | 0444 | `~0U, t41_adr_bad_dma = ~0U` | - |
| `t41_adr_stage` | uint | 0444 | `~0U, t41_adr_tile = ~0U` | - |
| `t41_adr_tile` | uint | 0444 | `~0U` | - |
| `t41_ae_calibrated_target` | uint | 0444 | `0x80000000U` | - |
| `t41_ae_diag` | uint | 0644 | `0 (not initialised)` | safe AE: log every sensor write (1) |
| `t41_ae_meter_error` | int | 0444 | `-EAGAIN` | - |
| `t41_ae_meter_stage` | uint | 0444 | `0 (not initialised)` | safe AE meter: last stage reached (1 static 2 mean 3 tables 4 target-fail 5 ok) |
| `t41_ae_roi_stats` | int | 0644 | `1` | copy the ROI-window AE statistics per frame for the face AE (default 1; 0 = off) |
| `t41_ae_zone_mean` | uint | 0444 | `0x80000000U` | - |
| `t41_af` | int | 0444 | `0 (not initialised)` | 1 = arm the AF statistics block at ISP init (OEM tisp_af path: 0x1a000, IRQ bit 6, 4 x 4 KB DMA ring) and serve GetAfStatistics/GetAFMetricesInfo; 0 = AF off, both answer EPERM (default) |
| `t41_ccm_ct` | uint | 0444 | `5000` | - |
| `t41_checkpoint_ms` | uint | 0 (load line only) | `0 (not initialised)` | diagnostic delay after each TISP init checkpoint |
| `t41_checkpoint_start` | uint | 0 (load line only) | `0 (not initialised)` | first zero-based TISP init checkpoint to delay |
| `t41_clm` | uint | 0444 | `1` | CLM: 0 no LUT (old), 1 OEM LUT by CT (default), 2 bypass |
| `t41_clm_ct_last` | uint | 0444 | `0 (not initialised)` | - |
| `t41_clm_writes` | uint | 0444 | `0 (not initialised)` | - |
| `t41_defer_video_link_setup` | int | 0644 | `-1` | defer video-pad graph mutation while isolating enable-time corruption |
| `t41_defog` | uint | 0444 | `1` | defog: 1 = OEM defog chain (default, registers equal the vendor), 0 = block kept in bypass |
| `t41_dmsc_error` | int | 0444 | `-ENODEV` | - |
| `t41_dmsc_gain` | uint | 0444 | `~0U` | - |
| `t41_dn_apply_ae_irq` | int | 0644 | `1` | 1 = additionally step the day/night transition from the per-frame AE statistics interrupt while no frame-completion interrupt has arrived for 250 ms (idle camera without a consumer; with a consumer the frame-completion path alone applies, as before), 0 = frame-completion interrupt only |
| `t41_dn_bank` | int | 0644 | `0x3ffff` | night/day parameter bank switch steps (bit0 copy, 1 awb, 2 lsc, 3 adr, 4 dmsc, 5 gamma, 6 ccm, 7 bcsh, 8 clm, 9 ysp, 10 mdns, 11 ydns, 12 cdns, 13 sdns, 14 lce, 15 dpc, 16 tmo, 17 blc); 0 = off |
| `t41_dn_bank_count` | uint | 0444 | `0 (not initialised)` | - |
| `t41_dn_bank_ret` | int | 0444 | `0 (not initialised)` | - |
| `t41_dn_top` | int | 0644 | `1` | apply the night/day bank TOP module bypass on a running-mode switch (1 = like stock) |
| `t41_dn_top_last` | uint | 0444 | `0 (not initialised)` | - |
| `t41_dpc_error` | int | 0444 | `-ENODEV` | - |
| `t41_dpc_gain` | uint | 0444 | `~0U` | - |
| `t41_fr_phys` | uint | 0644 | `0 (not initialised)` | - |
| `t41_heap_census` | callback | 0444 | `computed` | - |
| `t41_heap_decoys` | int | 0444 | `0 (not initialised)` | - |
| `t41_heap_watch` | int | 0444 | `0 (not initialised)` | - |
| `t41_isp_wq` | int | 0444 | `1` | 1=own ordered workqueue for AWB/tone work (stock isp_fw_process), 0=system_wq |
| `t41_isr_counts` | array of uint | 0444 | `0 (not initialised)` | per-index irq_func_cb call counters (hard-wired ISR list) |
| `t41_isr_dispatch` | callback | 0644 | `0 (not initialised)` | ISP IRQ dispatch: 0 hard-wired, 1 OEM-style table loop, 2 count-only probe |
| `t41_isr_dispatch_skip` | array of uint | 0644 | `{ (1u << 3) / (1u << 9), 1u << (61 - 32), 0 }` | - |
| `t41_isr_enable` | callback | 0644 | `computed` | ISP IRQ enable words 0x40020,0x40060,0x400a0,0x400b0 (default: all but AF bit 6, all, LSC/BCSH frame-down bits 4+20, 0x3f; stock is all, all, all, 0x3f) |
| `t41_isr_framedown` | bool | 0644 | `true` | call the OEM LSC/BCSH frame-down callbacks (irq_func_cb[36]/[52]) from the ISR (default 1) |
| `t41_late_start_mask` | uint | 0 (load line only) | `63` | capture producers to activate after connected frame-channel stream-on |
| `t41_lce_error` | int | 0444 | `-ENODEV` | - |
| `t41_lce_frames` | uint | 0444 | `~0U` | - |
| `t41_lce_gain` | uint | 0444 | `~0U` | - |
| `t41_lsc_ct` | uint | 0444 | `~0U, t41_lsc_applied_gain = ~0U` | - |
| `t41_lsc_error` | int | 0444 | `-ENODEV` | - |
| `t41_lsc_gain` | uint | 0444 | `~0U` | - |
| `t41_lsc_updates` | uint | 0444 | `~0U, t41_lsc_applied_ct = ~0U, t41_lsc_applied_gain = ~0U` | - |
| `t41_msca_cfg_update` | int | 0644 | `2` | 2 (default) latches MSCA output geometry with the output off (needs t41_msca_stop_disable=1); 1 latches after enable; 0 never latches |
| `t41_msca_ch1_max_h` | uint | 0644 | `1080` | largest ch1 height SET_FMT accepts with t41_msca_cfg_update>=2 (0 = no limit) |
| `t41_msca_ch1_max_w` | uint | 0644 | `1920` | largest ch1 width SET_FMT accepts with t41_msca_cfg_update>=2 (0 = no limit) |
| `t41_msca_enable_at_frame` | int | 0644 | `0 (not initialised)` | - |
| `t41_msca_fifo_ctrl_after_live_qbuf` | int | 0 (load line only) | `1` | negative enables T40-style control after live MSCA QBUF (default off) |
| `t41_msca_flip_skip_noop` | int | 0644 | `1` | 1 (default) skips the MSCA update request when HVFLIP bits are unchanged |
| `t41_msca_freebufs_fifo_clear` | int | 0644 | `0 (not initialised)` | - |
| `t41_msca_latch_timeout_ms` | int | 0644 | `300` | - |
| `t41_msca_live_max_h` | uint | 0644 | `1080` | largest output height AutoZoom/Mask/ScalerLv reprogram while streaming |
| `t41_msca_live_max_w` | uint | 0644 | `1920` | largest output width AutoZoom/Mask/ScalerLv reprogram while streaming (needs t41_msca_cfg_update=2) |
| `t41_msca_regs` | callback | 0444 | `computed` | - |
| `t41_msca_stop_disable` | int | 0644 | `1` | - |
| `t41_native_awb_dropped` | uint | 0444 | `~0U` | - |
| `t41_native_awb_error` | int | 0444 | `-ENODEV` | - |
| `t41_native_awb_frames` | uint | 0444 | `~0U, t41_native_awb_dropped = ~0U` | - |
| `t41_proc_watch` | int | 0444 | `1` | Watch (report only, no restore) the /proc/jz/isp entries every 50 ms (default 1) |
| `t41_raw_ring` | bool | 0644 | `true` | serve the RAW ring controls 0x80000a7/0x80000a8 (`IMP_ISP_RAW_RwControl`); 0 answers -EPERM (default 1 since #320) |
| `t41_rawinject_mb` | uint | 0444 | `10 << 20` | at load time, before memory fragments |
| `t41_reboot_quiesce` | bool | 0644 | `true` | stop and soft-reset the ISP bus master before restart/halt (default 1) |
| `t41_runtime_trace` | int | 0644 | `0 (not initialised)` | enable verbose T41 frame/remote-event/IRQ recovery tracing |
| `t41_safe_ae_effective_target_q8` | uint | 0444 | `~0U` | current attenuation-adjusted safe-AE target in Q8 units |
| `t41_safe_awb_controller` | callback | 0644 | `1` | negative enables and rearms the workqueue-backed T41 AWB DMA controller |
| `t41_stock_awb_stats_profile` | int | 0644 | `-1` | nonpositive reapplies calibration-driven AWB setup after stream reset (legacy name) |
| `t41_stock_dpc_profile` | int | 0644 | `-1` | nonpositive reapplies calibration-driven DPC after stream reset (legacy name) |
| `t41_stock_lce_ram_profile` | int | 0644 | `-1` | nonpositive enables calibrated histogram-driven LCE (legacy name) |
| `t41_stock_msca_unity_curve` | int | 0644 | `-1` | Commit exact-stock ratio-specific MSCA curves through shadow RAM (negative=enabled) |
| `t41_tmo_map_impl` | int | 0644 | `2` | local TMO map: 0=reference C, 1=fast C, 2=MXU3 (default; fast C where the CPU has no MXU3), all bit-exact |
| `t41_tmo_map_interval` | uint | 0644 | `1` | recompute the local TMO map every n-th frame (1=every frame, stock) |
| `t41_tmo_map_trigger` | callback | 0644 | `-1` | Diagnostic scalar TMO: positive EV processes current DMA, zero bypasses |
| `t41_tmo_on_change` | int | 0644 | `0 (not initialised)` | 1=TMO gain/EV interpolation and default registers only on change (stock-like) |
| `t41_tmo_ram_fast` | int | 0644 | `0 (not initialised)` | 1=write TMO RAM data words without a barrier per store (stock-like) |
| `t41_tone_trigger` | int | 0644 | `1` | 1 also runs the per-frame tone worker from the AE statistics interrupt (default 1; 0: TMO interrupt only) |
| `t41_vae_events` | uint | 0644 | `0 (not initialised)` | vendor AE: forward OEM AE event ids (bitmask, default 0) |
| `t41_vendor_ae` | int | 0444 | `1` | run the translated vendor AE per frame (default 1; 0 = safe AE controller) |
| `t41_video_link_dry_run` | int | 0644 | `-1` | resolve and inspect video-pad links without mutating pad state |
| `t41_wdr_fusion_unsafe` | int | 0444 | `0 (not initialised)` | Run the unrecovered WDR fusion body (unsafe, default 0) |
| `tmo_again` | array of uint | 0444 | `0 (not initialised)` | - |
| `tmo_again_commits` | uint | 0444 | `0 (not initialised)` | - |
| `tmo_irq17` | uint | 0444 | `0 (not initialised)` | - |
| `tmo_live` | int | 0444 | `computed` | - |
| `tmo_ready` | int | 0444 | `computed` | - |
| `tone_ev` | uint | 0444 | `0 (not initialised)` | - |
| `tone_gated` | uint | 0444 | `0 (not initialised)` | - |
| `tone_kicks` | uint | 0444 | `0 (not initialised)` | - |
| `tx_isp_bringup_level` | int | 0 (load line only) | `3` | bring-up gate: -1 exports only, 0 shallow, 1 graph, 2 mem, 3 core tuning (default) |
| `tx_isp_core_ioctl_mask` | int | 0 (load line only) | `0 (not initialised)` | isp ioctl calls mask |
| `unresolved_jalr_count` | uint | 0444 | `0 (not initialised)` | - |
| `v4l2_autostart` | bool | 0644 | `true` | start the configured sensor when V4L2 is the first ISP consumer |
| `v4l2_channel` | uint | 0444 | `0 (not initialised)` | first frame-source channel registered through V4L2 (all three exported) |
| `v4l2_sensor_default_boot` | int | 0644 | `0 (not initialised)` | sensor default boot mode |
| `v4l2_sensor_i2c_adapter` | int | 0644 | `0 (not initialised)` | sensor I2C adapter number |
| `v4l2_sensor_i2c_addr` | uint | 0644 | `0x3c` | 7-bit sensor I2C address |
| `v4l2_sensor_id` | uint | 0644 | `0 (not initialised)` | optional sensor instance identifier |
| `v4l2_sensor_mclk` | int | 0644 | `1` | sensor master-clock selector |
| `v4l2_sensor_name` | charp | 0644 | `"os04d10"` | sensor driver name used by V4L2 autostart |
| `v4l2_sensor_power_down_gpio` | int | 0644 | `-1` | sensor power-down GPIO (-1 disables) |
| `v4l2_sensor_power_gpio` | int | 0644 | `-1` | sensor power GPIO (-1 disables) |
| `v4l2_sensor_reset_gpio` | int | 0644 | `-1` | sensor reset GPIO (-1 disables) |
| `v4l2_sensor_video_interface` | int | 0644 | `0 (not initialised)` | sensor video input interface |

- **TX_ISP_LOAD_STATS=y only** (8): `t41_isr_bits`, `t41_isr_calls`, `t41_isr_max_us`, `t41_isr_us`, `t41_load_stats`, `t41_work_count`, `t41_work_max_us`, `t41_work_us`
- **TX_ISP_T41_TMO_SELFTEST=y only** (1): `t41_tmo_map_selftest`
- **bring-up build only** (82): `name`, `t41_ae_calibrated_metering`, `t41_ae_cap_again`, `t41_ae_cap_integration`, `t41_ae_comp`, `t41_ae_flicker_ceiling_lines`, `t41_ae_flicker_floor_lines`, `t41_ae_flicker_frequency_hz`, `t41_ae_flicker_gib_gain_q10`, `t41_ae_flicker_profile`, `t41_ae_target_q8`, `t41_ae_update_frames`, `t41_awb_gain_trigger`, `t41_awb_min_pixels`, `t41_awb_min_zones`, `t41_awb_update_irqs`, `t41_awb_warmup_irqs`, `t41_bcsh_color_model`, `t41_bcsh_error`, `t41_ccm_error`, `t41_cdns_error`, `t41_cdns_gain`, `t41_color_blue_correction_q10`, `t41_color_green_correction_q10`, `t41_defer_csi_start`, `t41_defer_isp_fw_thread`, `t41_defer_isp_start`, `t41_defer_sensor_start`, `t41_defer_vic_irq`, `t41_defer_vic_start`, `t41_ev_fanout_trigger`, `t41_gain_fanout_trigger`, `t41_gamma_error`, `t41_gamma_ev`, `t41_gamma_strength_value`, `t41_gamma_updates`, `t41_mdns_error`, `t41_mdns_gain`, `t41_msca_frame_shadow_commit`, `t41_native_awb`, `t41_safe_ae_again`, `t41_safe_ae_controller`, `t41_safe_ae_gain_q16`, `t41_safe_ae_integration`, `t41_safe_ae_last_mean_q8`, `t41_safe_awb_bank`, `t41_safe_awb_bgain`, `t41_safe_awb_irq_count`, `t41_safe_awb_raw_b_q10`, `t41_safe_awb_raw_r_q10`, `t41_safe_awb_reject_count`, `t41_safe_awb_rgain`, `t41_safe_awb_update_count`, `t41_safe_ev_fanout_mask`, `t41_safe_gain_fanout_mask`, `t41_safe_msca_start`, `t41_safe_tisp_teardown`, `t41_safe_tmo_irq_stats`, `t41_safe_tuning_events`, `t41_sdns_error`, `t41_sdns_gain`, `t41_sensor_limits_error`, `t41_stat_y_out`, `t41_stock_adr_profile`, `t41_stock_awb_gain_a`, `t41_stock_awb_gain_b`, `t41_stock_cdns_profile`, `t41_stock_dmsc_profile`, `t41_stock_mdns_profile`, `t41_stock_spatial_profile`, `t41_stock_tmo_profile`, `t41_stock_ysp_profile`, `t41_stock_ysp_unbypass`, `t41_tmo_dropped`, `t41_tmo_error`, `t41_tmo_frames`, `t41_tmo_replay_trigger`, `t41_vic_frame_sync_work`, `t41_ydns_error`, `t41_ydns_gain`, `t41_ysp_error`, `t41_ysp_gain`

### T32 (module `tx-isp-t32`, sysfs `/sys/module/tx_isp_t32/parameters/`)

12 parameters in the default build, 0 only in special builds.

| Parameter | Type | Perm | Default | Description (source text) |
|---|---|---|---|---|
| `bin_version` | charp | 0444 | `"1.00"` | IQ bin format version the driver accepts |
| `bringup_ae` | uint | 0644 | `0 (not initialised)` | 1: bring-up exposure controller (not the vendor AE), default 0 |
| `bringup_target` | uint | 0644 | `64` | bring-up controller target mean AE histogram bin (0..255) |
| `driver_soc` | charp | 0444 | `"PRJ007"` | SoC name answered to DRIVER_VERSION |
| `driver_version` | charp | 0444 | `"20251107a"` | ISP driver version answered to DRIVER_VERSION (must match libimp) |
| `isp_irq` | int | 0444 | `8 + T32_IRQ_ISP_M0` | Linux IRQ of the ISP core (INTC 29) |
| `isp_memopt` | uint | 0444 | `1500` | MDNS memory ratio in 1/1000 (<=1000 uncompressed, max 2000) |
| `listen_timeout_ms` | uint | 0644 | `2000` | LISTEN_BUF wait limit |
| `lsc_ct` | uint | 0644 | `5000` | colour temperature for the LSC mesh at stream-on (vendor init: 5000) |
| `print_level` | int | 0644 | `1` | isp_printf threshold: 0 info, 1 warning, 2 error |
| `t32_trace` | int | 0644 | `1` | log every ioctl (S1 bring-up) |
| `vic_irq` | int | 0444 | `8 + T32_IRQ_VIC` | Linux IRQ of the VIC (INTC 30) |

