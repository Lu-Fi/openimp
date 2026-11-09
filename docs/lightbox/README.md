<img src="lightbox-logo.webp" alt="The Light Box" width="160" align="right">

# Light-box protocols (#230)

Vendor-or-better check of the open stack on real cameras: each camera runs the vendor stack (Ingenic ISP module, vendor libimp)
and the open stack (open-tx-isp, OpenIMP) one after the other in the same light box, same lamps, same chart (Datacolor
SpyderCheckr 24), same streaming application built for each libimp.  Every file is a self-contained HTML page with the numbers,
the pass/fail checks, the deviations and the pictures; names are `<vendor>-<soc>-<sensor>-<date>.html`.

| SoC | camera | date | lamps | result |
|---|---|---|---|---|
| T10 | Secuplug SP1, jxh42 | 2026-11-07 | 3000 K fair pass + 5000 K colour pass + day/night D1/D4 | [28 pass / 3 differ](secuplug-t10-jxh42-2026-11-07.html) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-08 | 3000 K fair pass + runner supplement + day/night D1/D4 + 5000 K colour pass | [report](wyze-t20x-jxf23-2026-11-08.html) (26 pass / 19 fail, of which 10 invalid by the test setup, see notes) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-08 17:20 | full runner, both stacks, 3000 K + 5000 K AWB/colour, encoder, startup, charts; every fail classified | [report](wyze-t20x-jxf23-2026-11-08-1720.html) (15 pass; 26 fail: 15 with an issue #243-#246, 9 invalid by the test setup, 1 fixed, 1 accepted) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-09 | new runner (preflight, health log, auto-repeat, automatic lamps, Claude picture check): 3000 K full run both stacks + 5000 K AWB/colour/AE response | [report](wyze-t20x-jxf23-2026-11-09.html) (54 pass; 6 fail: 5 with issue #246, 1 test setup) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-09 21:01 | flashed image with the 11-09 fixes (#244 #246 #252 #253): full 3000 K run both stacks + 5000 K AWB/colour/AE response/P1; new rules (relative noise, comp steps from 128, fixed caps, AE step metric) | [report](wyze-t20x-jxf23-2026-11-09-2101.html) (56 pass; 3 fail: 1 accepted, 1 test setup, 1 vendor-equal; 1 flaky) |
