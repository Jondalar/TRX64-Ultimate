# U64 App SDK

Build external apps (`.u2a` files) that run on Ultimate 64 firmware with
the App Loader feature enabled.

## Layout

```
appsdk/
├── README.md           <- this file
├── include/
│   ├── u2p_app.h       <- main header to include in your app
│   ├── u2p_app_api.h   <- the ABI (includes software/app_loader/app_api.h)
│   └── u2p_ui.h        <- dialogs that redraw the app's screen when they close
├── lib/
│   └── u2p_ui.c        <- add $(SDK)/lib/u2p_ui.c to SRCS to use u2p_ui.h
├── app.mk              <- build rules for both variants (included by app Makefiles)
├── linker/
│   ├── app_nios2.lds   <- Nios II linker script (fixed load addr 0x03A00000)
│   └── app_riscv.lds   <- RISC-V linker script (fixed load addr 0x03A00000)
├── crt0/
│   ├── app_crt0_nios2.S <- tiny startup stub (entry -> app_main), Nios II
│   └── app_crt0_riscv.S <- tiny startup stub (entry -> app_main), RISC-V
├── tools/
│   └── mk_u2a.py       <- packs ELF builds (+ manifest) into a .u2a file
└── samples/
    └── hello/          <- minimal "hello world" app
```

## Build the hello sample

```sh
cd software/appsdk/samples/hello
make
# produces hello.u2a (format version 2, a Nios II and a RISC-V variant)
```

A `.u2a` file holds a 64-byte header, a variant table with one build per CPU
(`nios2`, `riscv32`), an optional JSON manifest and the payloads. The layout
and the manifest rules are in `doc/AppLoader_Spec.md`:

```sh
python3 tools/mk_u2a.py --variant nios2=app_nios2.elf --variant riscv32=app_riscv.elf \
    [--manifest manifest.json] --name app --out app.u2a
```

The `nios2-elf-gcc` (U64) and `riscv32-unknown-elf-gcc` (U64-II, C64U)
toolchains must be on `$PATH`. If you only have the build container:

```sh
./build/build_app.sh hello
```

The RISC-V variant must never use `gp`, the firmware's global pointer. `app.mk`
compiles it with `-msmall-data-limit=0 -mno-relax`, links with `--no-relax` and
fails the build if `objdump -d` shows any `gp` reference.

## Write your own app

```c
#include "u2p_app.h"

int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx) {
    api->sys->log("hello\n");
    return 0;
}
```

Set `APP`, `SDK` and `SRCS` (optionally `MANIFEST`) in a Makefile and include
`app.mk`, like `samples/hello/Makefile`.

## ABI

The ABI is major version 2, defined in `software/app_loader/app_api.h` and
described in `doc/AppLoader_Spec.md` section 5. `app_main` gets a root table
with one pointer per area: `api->sys`, `mem`, `ui`, `fs`, `c64`, `media`,
`config`, `diag`. Tables only grow at their end; to use a call that is newer
than the oldest firmware you target, test it with `U2P_HAS(api->ui, fkey)`.

Apps in `apps/` use this SDK directly (`SDK = ../../software/appsdk`) and are
built with `build/build_app.sh <app>`, which mounts the whole repository.

A manifest may list `rest` actions: those can be started with
`PUT /v1/apps/{id}:run`, without a screen (`ctx->has_ui == 0`); the app answers
through `ctx->reply()`.

`u2p_app.h` also defines the firmware's `BUTTON_*` and `KEY_*` values.

See `doc/AppLoader_Architecture.md` for the full design.
