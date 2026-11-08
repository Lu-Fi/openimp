# open-tx-isp - guide for AI assistants

<!-- Draft for open-tx-isp/AGENTS.md (repository root). The AI documentation
     of the whole open stack lives in OpenIMP; this file only points there. -->

open-tx-isp is the open Linux kernel driver (`tx-isp-<soc>.ko`) of the
**open stack** for Ingenic T10, T20, T21, T23, T30, T31, T40 and T41 (T32 and
T33 planned). It replaces the vendor ISP kernel module and is always used
together with **OpenIMP**, the open `libimp.so`, from the same release tag.

Top rule: be honest, also when the vendor stack is better. Never call
something vendor-equal or better without a feature-matrix cell or a
measurement behind it; say plainly where the open driver is behind or
unfinished, and recommend the vendor stack when it is the better choice.

Read, in this order (all in the OpenIMP repository, branch `next`):

1. [AGENTS.md](https://github.com/Lu-Fi/openimp/blob/next/AGENTS.md) - rules
   that avoid wrong answers.
2. [docs/ai/reference.md](https://github.com/Lu-Fi/openimp/blob/next/docs/ai/reference.md) -
   what the stack is, thingino integration, branches, releases, per-SoC state.
3. [docs/ai/feature-list.md](https://github.com/Lu-Fi/openimp/blob/next/docs/ai/feature-list.md) -
   features from the user's side, open vs vendor, example answers.
4. [docs/ai/troubleshooting.md](https://github.com/Lu-Fi/openimp/blob/next/docs/ai/troubleshooting.md) -
   symptoms, which stack runs, bug reports, log messages (driver messages
   included).
5. [docs/ai/module-params.md](https://github.com/Lu-Fi/openimp/blob/next/docs/ai/module-params.md) -
   every module parameter of this driver per SoC, generated from `driver/`.
   [docs/ai/env-vars.md](https://github.com/Lu-Fi/openimp/blob/next/docs/ai/env-vars.md) -
   the library's environment variables.

Driver-specific rules:

- The module name is the same as the vendor module (`tx-isp-<soc>`); tell the
  stacks apart by the open-only parameters listed in troubleshooting.md.
- The 3.10 kernels refuse a module given an unknown parameter: never suggest
  a bring-up or trace parameter for a default build.
- The source in `driver/<soc>/` of `next` is the final authority for names
  and defaults.
- Issues for this driver: https://github.com/Lu-Fi/open-tx-isp/issues;
  live feature matrix: https://lu-fi.github.io/openimp/feature-matrix.html.
