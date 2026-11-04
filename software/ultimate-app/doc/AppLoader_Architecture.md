# U64 App Loader Architecture (arc42)

**Status:** Decisions locked v0.2 (Phase 1 ready for implementation)
**Author:** Alex (DKL) + Claude
**Date:** 2026-04-24
**Branch target:** `feature/app-loader`

---

## 1. Introduction & Goals

### 1.1 Requirements Overview

Enable loading and running external "Apps" on the Ultimate 64 Nios II firmware — instead of compiling every tool (CartDumper, CartWriter, Demos, experiments) statically into the base firmware.

Goals:

- Developers (including Alex, Mike, community) can build, ship and iterate on apps without rebuilding/flashing the full firmware.
- Base firmware exposes a stable ABI (filesystem, UI overlay, C64 bus, USB, config).
- Apps are loaded from USB / SD card via the File Browser.
- One app runs at a time. No preemptive multitasking. No background apps.
- App crash may crash the system — acceptable in Phase 1 (no MMU).

### 1.2 Non-Goals

- No multitasking, no background apps, no IPC.
- No MMU-based isolation (Nios II has no MMU in U64).
- No dynamic linking with unresolved symbols (apps get a fixed syscall table, not a full ELF loader).
- No app store, no signing (Phase 1). May come later.
- One `.u2a` carries both a Nios II (U64) and a RISC-V (U64-II / C64U) build, see D-10.

### 1.3 Quality Goals

| Priority | Quality     | Motivation                                                |
|----------|-------------|-----------------------------------------------------------|
| 1        | Simplicity  | MVP must be small and understandable. ~500 LOC in firmware. |
| 2        | Stability   | Base firmware must not regress when feature is unused.    |
| 3        | ABI stability | Apps built today should run on future firmware.         |
| 4        | Testability | Build + run loop must work from source tree without flashing. |

### 1.4 Stakeholders

| Role            | Contact          | Expectation                                     |
|-----------------|------------------|-------------------------------------------------|
| Firmware owner  | Alex (DKL)       | Clean integration, mergeable to upstream later. |
| App author      | Alex, Mike       | Easy SDK, stable headers, good docs.            |
| End user        | C64/U64 owner    | Click `.u2a` file in browser → app runs.        |

---

## 2. Constraints

### 2.1 Technical

- **CPU:** Nios II/f (U64) — 32-bit RISC-like, no MMU, GP-relative sdata.
- **Memory map (from `nios_appl_bsp/linker.x`, v3.15):**
  - DDR RAM: `0x00000000 .. 0x04000000` (64 MB); firmware heap capped at `0xea0000`.
  - Updater `0x03000000 .. 0x03C00000`, cartridge ROM `0x03C00000 .. 0x04000000`.
  - Onchip: `0x30000000 .. 0x30030000` (192 KB, code+stack+bss of firmware).
- **Firmware is built statically linked.** Symbol table not available at runtime.
- **Toolchain:** `nios2-elf-gcc` via Altera BSP. No ld.so, no libdl.
- **Build host:** Apple Container CLI (`build/build.sh`). App SDK must live alongside.
- **FreeRTOS-based firmware.** Tasks must not be disrupted by app.

### 2.2 Organizational

- Master branch stays shippable. Work happens on `feature/app-loader`.
- Docs in `doc/`. Code in `software/`. No churn in upstream dirs unless needed.
- Do not break existing build targets (u64, u2, u2+, u64ii).

### 2.3 Conventions

- File extension: `.u2a` (Ultimate2 App). MIME reuse of CBM conventions not required.
- SDK lives under `software/appsdk/` with headers, linker script, sample app.

---

## 3. Context & Scope

### 3.1 Business Context

```
                            +-------------------+
                            |  App developer    |
                            |  (Alex, Mike,     |
                            |   community)      |
                            +--------+----------+
                                     | builds .u2a
                                     v
                            +-------------------+
                            |  USB stick / SD   |
                            +--------+----------+
                                     | inserted into U64
                                     v
+-------------+    File Browser   +-------------------+   renders UI   +---------+
| End user    +-----------------> |  U64 Firmware     +--------------> | HDMI/   |
| (C64+U64)   | selects .u2a      |  + App Loader     |                | C64 scr |
+-------------+                   +-------------------+                +---------+
                                     |
                                     v bus access
                                  +-----+
                                  | C64 |
                                  +-----+
```

### 3.2 Technical Scope

In scope:
- App binary format (`.u2a` with header + payload).
- Loader code in firmware (validate, load, jump, cleanup).
- Syscall/ABI table exposed to app.
- SDK: header file, linker script, Makefile, sample "hello" app.
- File Browser integration: recognize `.u2a`, "Run App" menu entry.

Out of scope (Phase 1):
- Multi-app, background apps, IPC.
- Networking ABI (apps get it later via same pattern).
- Signing, sandboxing, resource limits.

---

## 4. Solution Strategy

1. **Fixed load address.** Reserve 2 MB of DDR RAM right below the cartridge ROM for app code+data+bss. No relocation needed. App's linker script knows `ORIGIN = 0x03A00000`.
2. **Raw binary payload + header.** No ELF parser in firmware. App build pipeline: link → `objcopy -O binary` → prepend header → `.u2a`.
3. **Syscall table (vtable).** Firmware publishes a `u2p_api_t*` pointer passed to `app_main(api, argc, argv)`. App uses macros (`ui_printf(...)`) that dereference the table. Table grows via append-only versioning (`api_v1`, `api_v2`, ...).
4. **FreeRTOS task.** App runs inside a dedicated "app_task" FreeRTOS task (stack ~64 KB). Returning from `app_main` = normal exit. Crash = task trap = system crash (Phase 1).
5. **File Browser hook.** Detect `.u2a` extension, add Action → new `SubsysCommand` for `SUBSYSID_APP_LOADER`.

---

## 5. Building Block View

### 5.1 Level 1 — Whitebox

```
+------------------------------------------------------+
|                    U64 Firmware                      |
|                                                      |
|  +----------------+   +---------------------------+  |
|  | File Browser   +-->|  AppLoader (new subsys)   |  |
|  +----------------+   |                           |  |
|                       |  - validate header        |  |
|  +----------------+   |  - load payload to        |  |
|  | FatFs /        |<--+    0x03A00000             |  |
|  | FileManager    |   |  - zero bss               |  |
|  +----------------+   |  - spawn app_task         |  |
|                       |  - on return: cleanup     |  |
|  +----------------+   +------------+--------------+  |
|  | UserInterface  |<---------------|                 |
|  +----------------+                | calls via       |
|  +----------------+                | u2p_api_t    |
|  | C64 / CartCtrl |<---------------|                 |
|  +----------------+                v                 |
|                       +---------------------------+  |
|                       |   Loaded App (0x03E0..)   |  |
|                       |                           |  |
|                       |  int app_main(api,argc,a) |  |
|                       +---------------------------+  |
+------------------------------------------------------+
```

### 5.2 Components

**AppLoader** (`software/application/app_loader/`)
- `app_loader.cc/.h` — SubSystem class, entry action, task lifecycle.
- `app_format.h` — `.u2a` header and variant table structs, magic, version constants.
- `app_manifest.cc/.h` — parses and validates the embedded JSON manifest.
- `app_api.h` — `u2p_api_t` public ABI table (shared with SDK).
- `app_api_impl.cc` — implementation of each syscall, builds global `g_app_api_v1`.

**App SDK** (`software/appsdk/`)
- `include/u2p_app.h` — header included by every app.
- `include/u2p_app_api.h` — copy of `app_api.h` (kept in sync).
- `linker/app_nios2.lds`, `linker/app_riscv.lds` — linker scripts pinning code at `0x03A00000`.
- `crt0/app_crt0_nios2.S`, `crt0/app_crt0_riscv.S` — entry stubs: save the return address, call `app_main`, return.
- `app.mk` — build rules for both variants; fails the build if the RISC-V code uses `gp`.
- `tools/mk_u2a.py` — packs one ELF per CPU and the manifest into a `.u2a` file.
- `samples/hello/` — hello-world app, builds into `hello.u2a`.

**File Browser integration**
- `filemanager.cc` / `tree_browser.cc` — recognize `.u2a`, wire up Action → `SubsysCommand(ui, SUBSYSID_APP_LOADER, APP_CMD_RUN, 0, path, filename)`.

---

## 6. Runtime View

### 6.1 Load + Run

```
User          FileBrowser      AppLoader         FreeRTOS      App
 |                |                 |                |           |
 | selects .u2a   |                 |                |           |
 |--------------->|                 |                |           |
 |                | SubsysCommand   |                |           |
 |                | (APP_CMD_RUN)   |                |           |
 |                |---------------->|                |           |
 |                |                 | fopen, read    |           |
 |                |                 | header         |           |
 |                |                 | validate magic |           |
 |                |                 | version        |           |
 |                |                 | read payload   |           |
 |                |                 | to 0x03A00000  |           |
 |                |                 | memset bss     |           |
 |                |                 | xTaskCreate    |           |
 |                |                 | "app_task"     |           |
 |                |                 |--------------->|           |
 |                |                 |                | jumps to  |
 |                |                 |                | _app_start|
 |                |                 |                |---------->|
 |                |                 |                |           | runs
 |                |                 |                |           | app_main()
 |                |                 |                |           |
 |                |                 |                |<----------|
 |                |                 |                |  returns  |
 |                |                 | vTaskDelete    |           |
 |                |                 | free memory    |           |
 |                | SSRET_OK        |                |           |
 |                |<----------------|                |           |
```

### 6.2 App calling syscall

```c
// inside app code:
const u2p_api_t *api;   // passed into app_main

api->ui_printf("Hello from app\n");
api->c64_poke(0xDE00, 0x00);
FILE *f = api->fopen("/Usb0/data.bin", "rb");
```

Each function pointer lives in a table at a firmware-local address. No relocation in the app needed to call them.

---

## 7. Deployment View

### 7.1 Firmware Layout

`ultimate.app` includes loader code. New C files in `software/application/app_loader/`. Adds ~8-12 KB to firmware image. No flash partition changes.

### 7.2 App Layout

`.u2a` file structure (format version 2; field tables in `doc/AppLoader_Spec.md` section 1):

```
+--------------------------------------------------+
| u2p_app_header_t  (64 bytes)                     |
|   magic, header_size, format_version = 2,        |
|   api_version_min/max, variant_count,            |
|   manifest_size/offset, name[28]                 |
+--------------------------------------------------+
| u2p_app_variant_t x variant_count (24 bytes each)|
|   arch (1 = Nios II, 2 = RISC-V), load_addr,     |
|   entry_offset, payload_offset/size, bss_size    |
+--------------------------------------------------+
| manifest (minified JSON + NUL, optional)         |
+--------------------------------------------------+
| payload per variant (code + rodata + data)       |
+--------------------------------------------------+
```

The loader picks the variant of its own CPU. BSS is not part of the payload; the loader zeros
`[load_addr + payload_size, load_addr + payload_size + bss_size)`.

### 7.3 Memory Map Update

```
0x00000000 -- DDR RAM start (avalon2mem_0)
             firmware image, heap up to __alt_heap_limit (0xea0000)
...
0x03000000 -- updater image (only during a firmware update, ends ~0x0355A098)
0x03A00000 -- APP_LOAD_BASE (2 MB reserved for app)
0x03BFFFFF -- APP_LOAD_END
0x03C00000 -- cartridge ROM (large cart support, v3.15)
0x04000000 -- end of DDR
```

No linker patch is needed: the firmware heap ends at `0xea0000`. The app area moved down from `0x03E00000` when v3.15 put the cartridge ROM at `0x03C00000`.

---

## 8. Crosscutting Concepts

### 8.1 ABI Table

The ABI is `software/app_loader/app_api.h`, major version 2, described in
`doc/AppLoader_Spec.md` section 5. The firmware passes a root table with one
pointer per area (`sys`, `mem`, `ui`, `fs`, `c64`, `media`, `config`, `diag`);
every table carries its own `size`, grows only at its end, and an app tests a
newer field with `U2P_HAS()`. A NULL area means the firmware or build does not
offer it.

Still left out on purpose (D-3): freezing and unfreezing the C64, which the
user does through the menu, and raw framebuffer access.

### 8.2 App Entry

```c
// crt0/app_crt0_riscv.S: a0 = api, a1 = ctx, calls app_main and returns its result

int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx) {
    api->sys->log("Hello from %s\n", ctx->app_path);
    if (ctx->selected_file) {
        api->sys->log("Opened with file: %s\n", ctx->selected_file);
    }
    return 0;
}
```

### 8.3 Error Handling

- The loader validates the header, variant table and manifest in the order of
  `doc/AppLoader_Spec.md` section 1.3; every failure shows a popup and aborts the launch.
- Task crash → watchdog / unhandled exception → panic. Phase 1 accepts system reboot.

### 8.4 Versioning

- `format_version`: bump when binary format changes (very rare).
- `api_version_min/max`: the ABI majors the app works with; the loader needs its
  own major in that range. Within a major, tables only grow and `U2P_HAS()`
  tells an app what the running firmware has.

---

## 9. Architecture Decisions

### ADR-1: Raw binary + header, not ELF

**Decision:** `.u2a` is `[header][payload]`, not ELF.

**Why:** No ELF parser in firmware (cost: 10-20 KB code, bug surface). Link-time fix-up is simpler. ABI version negotiation via header is enough.

**Trade-off:** Apps cannot have unresolved symbols besides the syscall table. OK for MVP.

### ADR-2: Fixed load address, no relocation

**Decision:** App code pinned at `0x03A00000` (`0x03E00000` before v3.15).

**Why:** No PIC toolchain quirks, no runtime relocation, no SDA/GOT games. Simplest possible.

**Trade-off:** Only one app at a time; the 2 MB stay reserved even when no app is loaded, and the updater image must stay below them. App max size = 2 MB (code+data+bss).

**Alternative rejected:** `-fPIC -mpic` — Nios II PIC is quirky, SDA pointer games, extra work in crt0.

### ADR-3: Syscall table passed to `app_main()`, not imported

**Decision:** App does not import firmware symbols. Firmware passes `const u2p_api_t *api` as first arg to `app_main`.

**Why:** No symbol resolution in loader. Append-only versioning. App can be linked against the SDK header without knowing firmware addresses.

**Trade-off:** All calls go through a pointer-indirection. Negligible perf impact for this domain.

### ADR-4: App runs in its own FreeRTOS task

**Decision:** Loader does `xTaskCreate("app_task", _app_start, stack=64KB, ...)`.

**Why:** Keeps firmware event loop responsive. Task cleanup on return is trivial (`vTaskDelete`). Stack isolated from firmware stacks.

**Trade-off:** App cannot directly influence scheduler priorities (phase 1 accepts). Task creation adds a few ms latency — fine.

### ADR-5: File Browser entry via SubSystem

**Decision:** New `SubSystem` with ID `SUBSYSID_APP_LOADER`.

**Why:** Reuses existing command-dispatch pattern. "Run App" appears as action in browser alongside "Load PRG" etc.

### ADR-6: `.u2a` extension, MIME not relevant

**Decision:** Four-char magic `U2AP`, extension `.u2a`.

**Why:** Short, unique, not in conflict with C64 file extensions.

---

## 10. Quality Requirements

| Quality     | Scenario                                                                         |
|-------------|----------------------------------------------------------------------------------|
| Stability   | Firmware builds + boots identically when no `.u2a` is ever loaded.               |
| Footprint   | Loader + ABI impl adds < 16 KB to `ultimate.app`.                                |
| ABI stability | Hello app built today runs on firmware a year from now without recompile.      |
| DX          | `cd samples/hello && make` produces `hello.u2a`. One-liner from source.          |
| Debuggability | On load failure, firmware logs `U2A: magic mismatch at offs 0`, etc.           |

---

## 11. Risks & Technical Debt

| Risk                                                    | Mitigation                                          |
|---------------------------------------------------------|-----------------------------------------------------|
| App overwrites firmware memory (no MMU)                 | Phase 1: accept. Later: MPU if ever gained.         |
| Stack overflow in app crashes task                      | FreeRTOS stack-overflow hook; 64 KB default.        |
| ABI field added in wrong place breaks old apps          | Append-only rule; review checklist in doc.          |
| Two developers pick same `load_addr` for Phase 2 multi-app | Phase 1 is single fixed addr; multi later.       |
| RISC-V (U64-II) needs completely different format       | Same header/ABI concept, different load_addr + toolchain. |

---

## 12. Glossary

| Term        | Meaning                                                         |
|-------------|-----------------------------------------------------------------|
| `.u2a`      | Ultimate2 App file — this spec's binary format.                 |
| SDK         | Headers + linker script + sample app under `software/appsdk/`.  |
| ABI table   | `u2p_api_t` struct of function pointers.                     |
| `app_main`  | User's entry point: `int app_main(const u2p_api_t *api, int argc, char **argv)`. |
| AppLoader   | New SubSystem in firmware that validates, loads, runs an app.   |

---

## 13. Design Decisions (locked 2026-04-24)

### D-1: Heap — shared firmware heap + alloc-tracking

App-malloc/free gehen durch Firmware-Heap (5+ MB). Loader hält Linked-List aller App-Allokationen. Bei App-Exit werden verbliebene Einträge automatisch `vPortFree`'d (Leak-cleanup).

- Korruptions-Isolation NICHT vorhanden (Phase 1 Risiko akzeptiert).
- Kein fester Heap-Cap, App kann sehr große Buffer allozieren.
- Code-Aufwand: ~200 LOC Linked-List im Loader.

### D-2: UI — Freeze-only + Screen-API

App läuft ausschließlich im Freeze-Mode. Loader prüft `u64_config.start_mode == MODE_FREEZE` — sonst Popup "Apps require Freeze mode" und refuse. Kein Overlay-while-C64-running in Phase 1, keine Raw-Framebuffer-API.

App nutzt Firmware-`UserInterface` via Screen-API (tty_printf, ui_popup, ui_menu, ui_progress_*, ui_screen_*). Demo/Grafik-Apps später via separater Config-Mode + Framebuffer-Escape (append-only ABI-Erweiterung).

### D-3: C64 State — App's Verantwortung

Freeze/Unfreeze/Mode sind Firmware-Domain (User entscheidet via Firmware-UI: Reset, Power-Cycle, Unfreeze). App fasst das NICHT an — entsprechende Funktionen sind NICHT Teil der ABI.

App bekommt Bus-Primitives (`c64_peek/poke/dma_read/dma_write`). Wenn App C64-State ändert (Cart-Register, Banks), save's sie selbst am Entry und restored bei allen Exit-Pfaden. Crash = State bleibt kaputt, User macht Reset via Firmware.

Loader-Cleanup beschränkt sich auf Alloc-Tracking (D-1). Kein Bus-Rollback.

### D-4: Entry — typisierter Context, kein argc/argv

Signatur:
```c
int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx);
```

```c
typedef struct {
    uint32_t     struct_size;       // sizeof, für Forward-Compat
    const char  *app_path;          // immer gesetzt, z.B. "/Usb0/apps/foo.u2a"
    const char  *selected_file;     // NULL oder Pfad (File-Browser-Kontext)
    uint32_t     launch_source;     // enum: BROWSER, MENU, HTTP, DIRECT
    const char  *extra_args;        // NULL oder String
    void        *_reserved[8];
} u2p_app_context_t;
```

Append-only erweiterbar. Launch-Source-Enum wächst mit.

Stand heute (ABI 2): `doc/AppLoader_Spec.md` Abschnitt 4 — `size`, `args`, `action`, `has_ui`, `reply`, kein `_reserved`.

### D-5: Config — Datei neben .u2a (Phase 1)

Keine Firmware-Config-API für die App-eigenen Einstellungen (die Menü-Einstellungen gehen seit ABI 2 über `api->config`). App schreibt `<app_path ohne .u2a>.cfg` selbst (via `api->fopen/fwrite`). SDK liefert `app_config_path()` Helper.

Flash-persistent Config-API kommt später als ABI-Erweiterung, falls nötig.

---

## 13a. Design Decisions v1 (locked 2026-09-13)

### D-6: Manifest in the .u2a header

The app declares what it handles: file extensions with their context-menu label
(e.g. `.crt` -> "Write to Cartridge") and task-menu entries (category + label).
The firmware reads the manifest from the header without running the app.

### D-7: Central registry, filled automatically

`/Flash/apps.json` lists every known app: name, version, path and a copy of its
manifest. An app is registered the first time it is started with "Run App",
from any medium (Flash, USB, SD, ...). The file browser's context menu and the
task menu read the registry and offer matching apps. Several apps may claim the
same extension; each one gets its own entry.

### D-8: Stale entries are removed when picked

The registry is not checked while menus are built. When the user picks an entry
whose `.u2a` no longer exists, the firmware shows "App not found, entry removed"
and de-registers it. A briefly unplugged stick therefore costs nothing until an
entry is actually used.

### D-9: Offer a copy to Flash

When an app is registered from removable media (USB, SD), the firmware offers to
copy the `.u2a` to Flash, so the app stays available without that medium.

### D-10: Multi-architecture .u2a

The header carries a variant table: architecture, entry offset, payload offset
and size, bss size. `mk_u2a.py` packs a Nios II and a RISC-V build of the same
source. U64 and U64-II share the memory map, so both variants load at
`0x03A00000`. RISC-V apps are linked with `-mno-relax` and never touch `gp`,
which belongs to the firmware (`__global_pointer$`). Loader and ABI are built
for the `u64` and `u64ii` targets; the loader picks the variant of its own CPU.

### D-11: Cartridge tools only as an app

Cart Inspector, Dump Cartridge and Flash Cartridge leave the firmware
(`c64_subsys`, `filetype_crt`, target Makefiles). The `trex_crt` app provides
them 1:1 and reaches the menus through its manifest: task-menu entries for the
tools and `.crt` -> "Write to Cartridge". `launch_source` and `selected_file`
tell the app which tool to open and with which file.

---

## 13b. Design Decisions (2026-09-29)

### D-12: ABI major 2, one table per area

The flat v1 table ran out of reserved slots and had no size field, so an app
could not tell whether a newer call was there. Major 2 splits it into area
tables with a size each, adds `media` and `config` in the terms of the REST
API, and a `diag` area that release builds leave NULL. There is one header,
`software/app_loader/app_api.h`; the apps build against `software/appsdk`
instead of carrying copies. With a single user, v1 apps are simply rebuilt.

### D-13: Apps over REST

`GET /v1/apps` and `PUT /v1/apps/{id}:run` start registered apps without a
screen. Apps are not resident, so they do not register routes of their own:
the routes are fixed, and the manifest's `rest` list says which actions may be
started. The answer carries the exit code and what the app handed to
`ctx->reply`, which makes firmware tests scriptable while the device runs.

---

## 14. Next Steps

Branch `feature/app-loader` (candidate for the upstream PR):

1. ABI additions, append-only in the reserved slots: non-blocking keymapped key
   read (`ui_getch`, -1 = none, -2 = exit), popup returning `BUTTON_*`, cartridge
   preference read, cartridge bus setup identical to `CartBase` (save and clear
   `C64_DMA_MEMONLY`, leave `C64_MODE` alone), file creation with an "exists,
   overwrite?" prompt, UI colours, a borderless full screen. Fix `ui_confirm`
   (compare against `BUTTON_YES`). `KEY_*` / `BUTTON_*` defines in the SDK.
   Fill `selected_file`, `launch_source` and the picked menu entry in the context.
2. Manifest format (D-6) in `app_format.h` and `mk_u2a.py`.
3. Registry, context-menu and task-menu integration, de-registration, copy to
   Flash (D-7 .. D-9).
4. RISC-V: crt0, linker script, variant table, loader selection, `u64ii`
   Makefile. Test `hello.u2a` in the ue2emu emulator.
5. Remove the cartridge tools from the firmware (D-11).
6. Upstream conventions: `.clang-format`, no stale TODO comments, all CI targets
   build (`u2_rv`, `u2plus`, `u2pl_no_esp`, `u64_no_esp`, `u64ii_no_esp`),
   `app_space` size gate.

Branch `feature/trex-crt-app`, on top of `feature/app-loader`:

7. Port `CartBase`, `CartDumper`, `CartWriter` and `CartInspector` 1:1 to C
   (`cart_base.c`, `cart_dumper.c`, `cart_writer.c`, `cart_inspector.c`,
   `ui_shim.c`, `main.c`), including the v3.15 fixes (access check,
   `C64_DMA_MEMONLY`, discarded read after `C64_MODE`). `cartlib/` goes away.
8. Hardware test on a U64: dump Yeti Mountain, write and verify an EasyFlash,
   Inspector.

Later, for the PR: split into reviewable PRs, host unit tests for header and
manifest parsing, E2E coverage, user docs in `GideonZ/1541u-documentation`,
signing if apps are ever sold.
