<img src="lightbox-logo.webp" alt="The Light Box" width="160" align="right">

# Light-box protocols (#230)

Vendor-or-better check of the open stack on real cameras: each camera runs the vendor stack (Ingenic ISP module, vendor libimp)
and the open stack (open-tx-isp, OpenIMP) one after the other in the same light box, same lamps, same chart (Datacolor
SpyderCheckr 24), same streaming application built for each libimp.  Every file is a self-contained HTML page with the numbers,
the pass/fail checks, the deviations and the pictures; names are `<vendor>-<soc>-<sensor>-<date>.html`.

| SoC | camera | date | lamps | result |
|---|---|---|---|---|
| T10 | Secuplug SP1, jxh42 | 2026-11-12 10:18 | full 5000 K run both stacks + open supplement 12:36 (reload, motion, aeresp, nightir, d1d4); open: open-tx-isp next 51d50ddc (#358 fix) + OpenIMP libimp next a822a1b6 + timps a7cba61, runtime only; AE overshoot metric fixed (#396, open 6.2 % vs vendor 7.0 %); the fails are #405 (stale first frame since #374) and #406 (reset on streamer restart) | [45 pass / 9 fail](secuplug-t10-jxh42-2026-11-12-1018.html) |
| T10 | Secuplug SP1, jxh42 | 2026-11-11 19:23 | full 5000 K run both stacks (open: open-tx-isp next ff96b3a8 + OpenIMP libimp next df9d225f built from source, runtime only; sensor shvflip=1 #276, OSD band masked for PSNR #304, ch1 #235), Claude picture review; D4 and rc-step fails filed as #358 #359 | [60 pass / 3 fail](secuplug-t10-jxh42-2026-11-11-1923.html) |
| T10 | Secuplug SP1, jxh42 | 2026-11-10 17:40 | full 3000 K run both stacks (open: next with the T20 firmware fixes #244 #246 #252 #253 #262, runtime insmod), 5000 K AWB/colour/AE response/P1, Claude picture/log review | [62 pass / 4 fail](secuplug-t10-jxh42-2026-11-10-1740.html) |
| T10 | Secuplug SP1, jxh42 | 2026-11-07 | 3000 K fair pass + 5000 K colour pass + day/night D1/D4 | [28 pass / 3 differ](secuplug-t10-jxh42-2026-11-07.html) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-08 | 3000 K fair pass + runner supplement + day/night D1/D4 + 5000 K colour pass | [report](wyze-t20x-jxf23-2026-11-08.html) (26 pass / 19 fail, of which 10 invalid by the test setup, see notes) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-08 17:20 | full runner, both stacks, 3000 K + 5000 K AWB/colour, encoder, startup, charts; every fail classified | [report](wyze-t20x-jxf23-2026-11-08-1720.html) (15 pass; 26 fail: 15 with an issue #243-#246, 9 invalid by the test setup, 1 fixed, 1 accepted) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-09 | new runner (preflight, health log, auto-repeat, automatic lamps, Claude picture check): 3000 K full run both stacks + 5000 K AWB/colour/AE response | [report](wyze-t20x-jxf23-2026-11-09.html) (54 pass; 6 fail: 5 with issue #246, 1 test setup) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-10 12:22 | full 3000 K run both stacks with the #262 fix (preset colour matrix) on top of the 11-09 fixes, 5000 K AWB/colour/AE response/P1, D4 day/night at 5000 K, AWB drift check; grey rows on unclipped greys (#259), Claude picture/log review | [58 pass / 0 fail / 1 flaky](wyze-t20x-jxf23-2026-11-10-1222.html) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-09 21:01 | flashed image with the 11-09 fixes (#244 #246 #252 #253): full 3000 K run both stacks + 5000 K AWB/colour/AE response/P1; new rules (relative noise, comp steps from 128, fixed caps, AE step metric) | [report](wyze-t20x-jxf23-2026-11-09-2101.html) (56 pass; 3 fail: 1 grey balance #259, 1 test setup, 1 vendor-equal; 1 flaky) |
| T40 | Eufy T40XP, sc830ai | 2026-11-12 13:30 | 3000 K pass both stacks (5000 K pass follows after the bulb swap); open: open-tx-isp next 36014e3f + OpenIMP libimp next 7e17045f, runtime only; T40 issue rows #226 #227 #239 (pass) and #332; open-only fails filed as #409 (AAC timestamp burst), #410 (AE comp path dependence), #412 (AE overshoot at lamps on); 9 test faults (enc 'decode errors' were audio dts, rc scene ceiling, no WB presets in the streamer on T40, #332 point clipped, restart race) fixed in the runner (#411) | [59 pass / 15 fail](eufy-t40xp-sc830ai-2026-11-12-3000.html) |
