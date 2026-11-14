# T31 ISP: device results of 2026-09-30 (cam-A, T31X, SC4336P)

Full report with all measurement data: `Lu-Fi/openimp`, branch `claude/t31-test-results-20260930b`,
`test-results/T31_TEST_RESULTS.md` (section "Phase 2"). Only the key points for open-tx-isp are given here.

Tested was `claude/t31-isp-all` (`97cccfa2`) with the thingino patches `0001`–`0006` as the
installed module (thingino `ciao` with `BR2_PACKAGE_THINGINO_ISP_OPEN=y`, complete OTA), libimp
OpenIMP `dfbd90f` or its fix branches, streamer timps.

| Test | `claude/t31-isp-all` | `claude/t31-isp-fixes` (this branch) |
|---|---|---|
| T1 start/stop ×20, T3 `kill -9` ×10, T4 snapshots, T5 second `open()`, T6 idle kick, T7 memory | passed | – |
| T2 day/night ×20 with stream (drop 0) | passed (old module froze once for 4.5 s) | – |
| L2 latency over 24 day/night switches | max. 660 µs, 0 s > 1 ms (old module 1313 µs, 2 s > 1 ms) | – |
| L4 ctxt/s | 2618 (old 2736) | – |
| Tuning 5.1–5.14 | passed except 5.6 (marker suppressed by `print_level=1`); `it_max`/SetAe_IT_MAX work | – |
| L1 `rmmod tx_isp_t31` | **Oops** (`tx_isp_exit` → `tx_isp_vic_remove` → `tx_isp_vic_stop`, uninitialised mutex) | **passed**: 3 cycles, no oops, IRQs/regions free, reload ok |
| T2/L5 with `isp_day_night_switch_drop_frame_num=6` | not testable (rmmod) | **passed** |

Commits on this branch (relative to `97cccfa2`):
- `1d85e525`, `4d83ac64`, `c7c2e7d2` (`claude/t31-isp-rmmod-fix`): VIC mutex init, exit order,
  double `kfree` of the pad arrays in `tx_isp_subdev_deinit`, `misc_deregister` of the frame channels,
  regions/IRQs with the correct dev_id, `release()` for the static platform devices.
- `f70c5276`, `8b727c31`, `0e1c5773` (`claude/t31-isp-csi-mutex`): CSI lock without overlap with the
  OEM raw slots (+`BUILD_BUG_ON` layout check), VIC frame-end completions and custom-AE completion
  initialised.

Open: the known leak of ~160 KiB per load cycle (`tisp_deinit_free`), `clk_put` of the CSI clocks;
rmmod while a stream is running is not protected. The INFO markers of the lifecycle guide
(`stopped channel`, `ISP core quiet after …`) are not visible with `print_level=1` + patch 0006.
