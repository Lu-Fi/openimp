# QEMU scene AE/AWB regression, 2026-11-11 (QEMU ingenic-0.7.14, open stack)

```
t31-open-grey: PASS
  coverage_end_s               89.867     ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  convergence_s                31.467     ok   (limit <= 45 s from stream start (ev))
  steady_EV_spread_pct         0.0        ok   (limit <= 15 %)
  awb_steady_pct               0.0        ok   (limit <= 5 %)
t31-open-step: PASS
  coverage_end_s               89.4       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  step_seen_near_30s           35.067     ok   (limit 30 +- 8 s)
  settle_after_step_s          11.133     ok   (limit <= 45 s (ev))
  overshoot_pct                -0.0       ok   (limit <= 25 % (ev))
  exposure_follows_step        390960.0   ok   (limit >= 1.5 x)
t31-open-ramp_up: PASS
  coverage_end_s               89.867     ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  exposure_vs_scene_rho        -0.928     ok   (limit <= -0.85 (exposure falls as the scene brightens))
  exposure_direction           0.0        ok   (limit EV falls over the ramp)
t31-open-ramp_down: PASS
  coverage_end_s               87.0       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  exposure_vs_scene_rho        -0.949     ok   (limit <= -0.85 (exposure falls as the scene brightens))
  exposure_direction           30073.846  ok   (limit EV rises over the ramp)
t31-open-tint: PASS
  coverage_end_s               88.267     ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  rg_over_bg_cool_vs_warm      6.448      ok   (limit >= 1.1 (red gain falls, blue rises under warm light))
  rg_over_bg_vs_time_rho       0.955      ok   (limit >= 0.85)
  ct_warm_below_cool           7701.0     ok   (limit colour temperature rises)
t20j-open-grey: PASS
  coverage_end_s               89.8       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  ae_not_railed                0          ok   (limit AE ends between its limits)
  convergence_s                0.0        ok   (limit <= 45 s from stream start (ev))
  steady_EV_spread_pct         0.0        ok   (limit <= 15 %)
  awb_steady_pct               0.0        ok   (limit <= 5 %)
t20j-open-step: PASS
  coverage_end_s               89.8       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  ae_not_railed                0          ok   (limit AE ends between its limits)
  settle_after_step_s          10.467     ok   (limit <= 45 s (ev))
  overshoot_pct                -0.0       ok   (limit <= 25 % (ev))
  exposure_follows_step        24.91      ok   (limit >= 1.5 x)
t20j-open-ramp_up: PASS
  coverage_end_s               88.133     ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  ae_not_railed                0          ok   (limit AE ends between its limits)
  exposure_vs_scene_rho        -1.0       ok   (limit <= -0.85 (exposure falls as the scene brightens))
  exposure_direction           0.384      ok   (limit EV falls over the ramp)
t20j-open-ramp_down: PASS
  coverage_end_s               89.0       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  ae_not_railed                0          ok   (limit AE ends between its limits)
  exposure_vs_scene_rho        -1.0       ok   (limit <= -0.85 (exposure falls as the scene brightens))
  exposure_direction           5.689      ok   (limit EV rises over the ramp)
t20j-open-tint: FAIL
  coverage_end_s               89.2       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  ae_not_railed                0          ok   (limit AE ends between its limits)
  rg_over_bg_cool_vs_warm      1.0        BAD  (limit >= 1.1 (red gain falls, blue rises under warm light))
  rg_over_bg_vs_time_rho       nan        BAD  (limit >= 0.85)
  ct_warm_below_cool           0.0        BAD  (limit colour temperature rises)
t20-open-grey: PASS
  coverage_end_s               87.4       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  convergence_s                30.733     ok   (limit <= 45 s from stream start (ev))
  steady_EV_spread_pct         0.0        ok   (limit <= 15 %)
  awb_steady_pct               0.0        ok   (limit <= 5 %)
t20-open-step: PASS
  coverage_end_s               89.0       ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  settle_after_step_s          43.467     ok   (limit <= 45 s (ev))
  overshoot_pct                -0.0       ok   (limit <= 25 % (ev))
  exposure_follows_step        34813.5    ok   (limit >= 1.5 x)
t20-open-tint: FAIL
  coverage_end_s               88.667     ok   (limit samples reach >= 80 s of the clip)
  stayed_in_day_mode           0          ok   (limit 0 rows in ISP Night mode)
  rg_over_bg_cool_vs_warm      1.0        BAD  (limit >= 1.1 (red gain falls, blue rises under warm light))
  rg_over_bg_vs_time_rho       nan        BAD  (limit >= 0.85)
  ct_warm_below_cool           0.0        BAD  (limit colour temperature rises)
```

t31 = sc2336 (no exposure model in QEMU: open AE loop), t20 = jxf23 (open loop), t20j = T20X image with jxf22 (closed loop). Clips: tools/qemu/gen_scenes.sh. Per run: .raw (guest samples), .csv, .json, .txt; .console/.qemu logs are not kept. Vendor stack: not run (see README).
