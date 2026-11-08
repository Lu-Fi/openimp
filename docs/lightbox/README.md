# Light-box protocols (#230)

Vendor-or-better check of the open stack on real cameras: each camera runs the vendor stack (Ingenic ISP module, vendor libimp)
and the open stack (open-tx-isp, OpenIMP) one after the other in the same light box, same lamps, same chart (Datacolor
SpyderCheckr 24), same streaming application built for each libimp.  Every file is a self-contained HTML page with the numbers,
the pass/fail checks, the deviations and the pictures; names are `<vendor>-<soc>-<sensor>-<date>.html`.

| SoC | camera | date | lamps | result |
|---|---|---|---|---|
| T10 | Secuplug SP1, jxh42 | 2026-11-07 | 3000 K fair pass + 5000 K colour pass + day/night D1/D4 | [28 pass / 3 differ](secuplug-t10-jxh42-2026-11-07.html) |
| T20 | Wyze Cam v2, jxf23 | 2026-11-08 | 3000 K fair pass + runner supplement + day/night D1/D4 + 5000 K colour pass | [report](wyze-t20x-jxf23-2026-11-08.html) (26 pass / 19 fail, of which 10 invalid by the test setup, see notes) |
