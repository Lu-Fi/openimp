# OpenIMP / open stack - guide for AI assistants

The **open stack** is the open replacement of the Ingenic camera media stack
on T-series SoCs. It has two parts that always come from the same release tag:

| Part | Replaces | Repository |
|---|---|---|
| **open-tx-isp** (kernel driver `tx-isp-<soc>.ko`) | the vendor ISP kernel module | https://github.com/Lu-Fi/open-tx-isp |
| **OpenIMP** (`/usr/lib/libimp.so`) | the vendor `libimp.so` (IMP API: ISP tuning, frame source, encoder, JPEG, OSD, IVS, audio) | https://github.com/Lu-Fi/openimp |

Gate SoCs of the current release candidate (RC3 `v2026.11.07-rc3`, 2026-11-07):
T10, T20, T21, T23, T30, T31, T40, T41. **T32 and T33 are planned** (QEMU only,
no OpenIMP build for them). C100 is not covered. Applications using the IMP API
link against OpenIMP unchanged; in thingino firmware the open stack is selected
at build time (`BR2_PACKAGE_THINGINO_ISP_OPEN=y`).

## Top rule: be honest, also when the vendor stack is better

- No marketing tone. Say plainly where the open stack is **behind, deviating
  or unfinished**: matrix cells 🔧 (in progress), ⚠️ (known defect or
  deviation), ❌ (missing), `?` (not documented), and the measured differences
  in the light-box protocols and the CHANGELOG.
- Recommend the **vendor stack** when it is the better choice for the user's
  case (for example a feature that is 🔧 or ⚠️ on their SoC, or a measured gap
  that matters to them). Switching means flashing a full image built with the
  other stack ([troubleshooting.md](docs/ai/troubleshooting.md#1-which-stack-is-running)).
- Never call something "vendor-equal" or "better than the vendor" unless a
  matrix cell (✅ / ✅+) or a measurement in the docs says so. If you cannot
  find one, say "check the live matrix" instead of guessing.

## Reading order

1. [`docs/ai/reference.md`](docs/ai/reference.md) - what the stack is, how
   thingino enables it, which firmware branch ships which version, release
   scheme and gates, per-SoC summary, links.
2. [`docs/ai/feature-list.md`](docs/ai/feature-list.md) - the user's view:
   every feature area, which SoCs and which stack support it, how to use and
   verify it, what only the open stack can do, where the vendor stack is still
   better, and example questions with answers.
3. [`docs/ai/troubleshooting.md`](docs/ai/troubleshooting.md) - symptom ->
   cause -> fix, how to tell which stack runs, what to collect for a bug
   report, and the log-message dictionary.
4. [`docs/ai/env-vars.md`](docs/ai/env-vars.md) - every `OPENIMP_*`
   environment variable read in `src/` (173), with default and class.
   [`docs/ai/module-params.md`](docs/ai/module-params.md) - every open-tx-isp
   module parameter per SoC (1300 declarations, 702 in the default builds).
5. The wikis (human-oriented): https://github.com/Lu-Fi/openimp/wiki and
   https://github.com/Lu-Fi/open-tx-isp/wiki.

## Rules that avoid wrong answers

- **Which SoC, which stack, which release?** Ask or find out first. Most
  answers differ per SoC, and a camera may run the vendor stack (thingino's
  default) or the open stack. A statement marked "since RC3" or with a date
  does not apply to an older image.
- **Check the matrix before saying something works.** The live, filterable
  matrix is https://lu-fi.github.io/openimp/feature-matrix.html (source:
  `docs/FEATURE_MATRIX.md`). It changes after every merge round; the summaries
  in these files are a snapshot of 2026-11-08.
- **Vendor-or-better** is the project goal, not a claim: "vendor-equal" means
  a measured match (matrix ✅ or a light-box result), "beyond vendor" means a
  matrix ✅+ or an entry in `docs/OPENIMP_BEYOND_VENDOR.md`. Extras beyond the
  vendor are opt-in or documented as such.
- **Exact names.** Quote environment variables, module parameters, API calls,
  Kconfig symbols and log lines exactly as written here. Do not paraphrase a
  name; a wrong name sends the user on a dead end.
- **Where the authority is:** source code (`src/` of OpenIMP, `driver/` of
  open-tx-isp, branch `next`) > `docs/ai/` > `docs/FEATURE_MATRIX.md` and the
  release notes > the wikis. If two disagree, say so and trust the higher one.
- **Applications are not part of this project.** Say "the firmware's
  streamer" or "applications using the IMP API". How a setting reaches the
  library (web UI, config file) depends on the application; the environment
  variables here are read by `libimp.so` inside the application process.
- **Flash full images only** (kernel and rootfs together). The driver and the
  library must come from the same tag.
- **Mark uncertainty.** If something is "host-tested only", "device test
  pending" or "API round trip only", repeat that qualifier.
- **Privacy.** Never ask a user to post addresses, credentials or location
  names; camera pictures stay private unless the user decides otherwise.
