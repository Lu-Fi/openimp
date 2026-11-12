# export-check

`make export-check` compares the exported `IMP_*` symbols of each built `build/<soc>/libimp.so` (`nm -D`) with

1. `vendor-<SoC>.txt`: the `IMP_*` names exported by the vendor libimp of that SoC (union of the SDK versions we hold;
   one name per line, sorted), and
2. the `IMP_*` names used by streamer sources (read only checkouts, default `../timps ../prudynt-t ../raptor`, override
   with `STREAMER_DIRS="dir dir"`; vendor header copies (`include/`, `ingenic-headers/`, `3rdparty/`) are skipped).

It prints, per SoC, how many vendor names are not exported and fails (exit 1) if a name that is both vendor-exported
and used by a streamer is missing. `streamer-ignore-<SoC>.txt` lists names that a streamer only mentions in a comment or in
code paths of another SoC (`name  # reason`). SoCs without a built library are skipped (`EXPORT_CHECK_STRICT=1` fails instead).
`BUILD=dir` selects another build directory (default `build/`), `SOCS="T41 T40"` restricts the run; `LIST=1` prints the missing vendor names; `NM=` selects another nm.
T10 uses the T20 build (`build/t20/libimp.so`) when `build/t10` does not exist.

`make release-check` (tools/release-check.sh) builds every SoC and runs this check with `EXPORT_CHECK_STRICT=1`; see BUILD.md.

Exception (T10, T20): the lists follow the 3.12.0 libimp, the one the cameras run. Eight names that only the older 3.9.0 libimp exports
(`IMP_Encoder_Get/SetChnH264Demask`, `Get/SetChnH264Denoise`, `Get/SetChnH264FrmUsedMode`, `Get/SetChnRcAttr`; 3.12.0 renamed the first three to
`ChnDemask`, `ChnDenoise` and `ChnFrmUsedMode` and dropped the RcAttr pair; the 3.9.0 channel-attribute structures differ, so a 3.9.0 program does
not run on OpenIMP anyway) are not listed.

Planned SoCs (T32, T33; not in the default `SOCS`, not a release gate): `SOCS="T32 T33" NM=<T32 toolchain>-nm ./tools/export-check.sh`
after `build-t32.sh` / `build-t33.sh`. State 2026-11-12: T32 372 of 556 vendor names exported, T33 374 of 575 (`vendor-T33.txt` = T33
2.0.2.1 libimp); both miss the same 8 streamer-used names (`IMP_Encoder_SetChnBitRate`, `SetChnQpBounds`, `SetChnROI`,
`SetFisheyeEnableStatus`, `SetMbRC`, `IMP_FrameSource_Get/SetChnFifoAttr`, `SetI2dAttr`), the T32 encoder / FrameSource gaps (#391).
