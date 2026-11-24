Softboot on ABI 3 (T104.3 D9). PROVENANCE: a copy of `apps/softboot` of TRX64U-MultiCore (branch `t239.3`, commit `8ff28ba9`, the commit SDK 0.4.1 is packed from),
MultiCore's port onto ABI 3 of our firmware's softboot application: `src/softboot.c` is a copy of `software/ultimate-app/apps/softboot/src/softboot.c` (the TRX64 line's
softboot application, GPL-3.0; submodule commit `aa2ef5dc`). Changed by that port: `app_main` is `softboot_main` (the entry is `glue.cc`: with no file it asks for the image);
the calls of ABI 2 land on the base's service tables through the SDK's `sdk/u2p_compat.c` (`flash_read` and `machine_handover` are the base's `coreapp/src/appsvc.cc`,
`base/src/coreapp/src/appsvc.cc` in the SDK). Against MultiCore's copy, only the Makefile's include (the SDK's `mk/app.mk`), this README and the paths that comments name
are changed here.
