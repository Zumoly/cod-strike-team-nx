# Startup files

`crt0_reloc.c`, `nx32_virtmem.c`, `nx_init.c`, `nx_init.h`, `selfproc.c`, `selfproc.h` and
`config/dcr32.ld`, `config/dcr32.specs` come from the android32 runtime in
[mc3_nx](https://github.com/Kaays/mc3_nx) (`runtime/`, upstream commit
`4b5e470a494c529ae77cd490c88a91bfe28828f3`), MIT licensed: see `LICENSE.android32`.

They are used unchanged apart from the small stand-ins `rt_settings.h` and `util.h`, because each
encodes a hardware result that an emulator cannot reproduce. Change them only with a console to test on.

`rt_audout.c` and `rt_audout.h` are copied unchanged from the same runtime: the audout output, proven on hardware.
