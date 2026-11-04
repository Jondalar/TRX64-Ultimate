# App Loader — Implementation Spec

Status: locked 2026-09-13. Implements decisions D-6 .. D-11 of
`doc/AppLoader_Architecture.md`. All multi-byte values are little-endian.
Apps are not released yet, so format version 1 is dropped: every app is rebuilt.

---

## 1. `.u2a` file layout (format version 2)

```
+----------------------+  offset 0
| header (64 bytes)    |
+----------------------+  variant_table_offset (= 64)
| variant table        |  variant_count x 24 bytes
+----------------------+  manifest_offset (0 = none)
| manifest (JSON, NUL) |  manifest_size bytes incl. NUL
+----------------------+
| payload variant 1    |  payload_offset / payload_size per variant
| payload variant 2    |
+----------------------+
```

### 1.1 Header

| Off | Size | Field                  | Value / meaning                              |
|----:|-----:|------------------------|----------------------------------------------|
|   0 |    4 | `magic`                | `"U2AP"` (`0x50413255`)                      |
|   4 |    2 | `header_size`          | 64                                           |
|   6 |    2 | `format_version`       | 2                                            |
|   8 |    2 | `api_version_min`      | lowest ABI version the app needs             |
|  10 |    2 | `api_version_max`      | highest ABI version the app knows            |
|  12 |    2 | `variant_count`        | 1 .. 4                                       |
|  14 |    2 | `manifest_size`        | bytes incl. terminating NUL, 0 = no manifest |
|  16 |    4 | `variant_table_offset` | 64                                           |
|  20 |    4 | `manifest_offset`      | file offset, 0 = no manifest                 |
|  24 |    4 | `flags`                | 0                                            |
|  28 |   28 | `name`                 | display name, zero-padded                    |
|  56 |    8 | `reserved`             | 0                                            |

### 1.2 Variant entry (24 bytes)

| Off | Size | Field            | Value / meaning                                  |
|----:|-----:|------------------|--------------------------------------------------|
|   0 |    2 | `arch`           | 1 = Nios II (U64), 2 = RISC-V rv32 (U64-II/C64U) |
|   2 |    2 | `reserved`       | 0                                                |
|   4 |    4 | `load_addr`      | `U2A_LOAD_ADDR` = `0x03A00000`                   |
|   8 |    4 | `entry_offset`   | entry point relative to `load_addr`              |
|  12 |    4 | `payload_offset` | file offset of the raw binary                    |
|  16 |    4 | `payload_size`   | bytes                                            |
|  20 |    4 | `bss_size`       | bytes to zero after the payload                  |

### 1.3 Loader validation (in this order, each failure = popup + abort)

1. `magic`, `header_size == 64`.
2. `format_version == 2`, otherwise "Old app format. Please rebuild the app."
3. `api_version_min <= U2P_ABI_MAJOR <= api_version_max`: an app that needs a
   newer major gets "This app needs a newer firmware.", one built for an older
   major "This app was built for an older firmware. Please rebuild the app."
   `mk_u2a.py` writes the SDK's major into both fields.
4. A variant with `arch` equal to the firmware CPU (`NIOS` -> 1, `RISCV` -> 2),
   otherwise "This app has no build for this device."
5. `load_addr == U2A_LOAD_ADDR`, `payload_size + bss_size <= U2A_AREA_SIZE`,
   `payload_offset + payload_size <= file size`.
6. If `manifest_size > 0`: `manifest_size <= 4096`, NUL at the last byte, valid
   JSON object per section 2. An invalid manifest aborts the launch.

---

## 2. Manifest

UTF-8 JSON object, NUL-terminated, at most 4096 bytes. Unknown keys are ignored.

```json
{
  "id": "trex_crt",
  "name": "TREX CRT Tool",
  "version": "1.0",
  "filetypes": [
    { "ext": "CRT", "label": "Write to Cartridge", "action": "write" }
  ],
  "tasks": [
    { "category": "C64 Machine", "label": "Dump Cartridge",  "action": "dump" },
    { "category": "C64 Machine", "label": "Cart Inspector",  "action": "inspect" },
    { "category": "C64 Machine", "label": "Flash Cartridge", "action": "flash" }
  ],
  "rest": [ "inspect" ]
}
```

| Key         | Rule                                                                  |
|-------------|-----------------------------------------------------------------------|
| `id`        | required, `[a-z0-9_]{1,31}`, unique key in the registry               |
| `name`      | required, 1..27 chars                                                 |
| `version`   | required, 1..15 chars                                                 |
| `filetypes` | optional, max 8; `ext` without dot, 1..7 chars, compared ignoring case |
| `tasks`     | optional, max 8; `category` 1..31 chars, matches an existing F5 category by name, otherwise a new category is created |
| `label`     | 1..31 chars                                                           |
| `rest`      | optional, max 8 actions `PUT /v1/apps/{id}:run` may start (section 9); without it the app starts only from the menu |
| `action`    | `[a-z0-9_]{1,15}`, passed to the app as `ctx->action`                 |

`mk_u2a.py --manifest <file>` validates these rules, minifies the JSON and
embeds it.

---

## 3. Registry

File `/Flash/config/apps.json` (same directory as the FTP server list).

```json
{
  "version": 1,
  "apps": [
    {
      "id": "trex_crt", "name": "TREX CRT Tool", "version": "1.0",
      "path": "/Flash/apps/trex_crt.u2a",
      "filetypes": [ { "ext": "CRT", "label": "Write to Cartridge", "action": "write" } ],
      "tasks": [ { "category": "C64 Machine", "label": "Dump Cartridge", "action": "dump" } ]
    }
  ]
}
```

- Loaded once at boot after the Flash disk is mounted, saved as a whole file
  (`FA_CREATE_ALWAYS`) after every change. Max 32 apps. Create
  `/Flash/config` one level at a time if missing.
- Key is `id`. Registering an existing `id` replaces the entry (new path,
  version, filetypes, tasks).

### 3.1 Registration (D-7, D-9)

On every "Run App" of a `.u2a` with a manifest, after validation and before the
app starts:

1. If the path is not on `/Flash`, check the registry entry with this `id`:
   - No entry with a `/Flash` path, or that file is gone: popup "Copy <name>
     to Flash so it stays available?" (Yes/No). Yes: create `/Flash/apps`,
     `fcopy` with overwrite, use `/Flash/apps/<file>` as the path. No: use
     the path the app runs from.
   - The `/Flash` file has the same size and content as the app: keep the
     `/Flash` path, no popup.
   - The `/Flash` file differs: popup "Update <name> on Flash with this
     version?" (Yes/No). Yes: `fcopy` the app over that file and keep its
     path. No: keep the `/Flash` path.

   The manifest `version` is not compared, because apps do not change it
   reliably. A failed copy shows "Copy to Flash failed." and the entry gets
   the path the app runs from. Without a user interface there is no popup:
   nothing is copied and an existing `/Flash` copy is kept.
2. Upsert the entry (manifest and version are always updated), save, update
   the task menu (3.3).
3. Start the app from the original path.

Apps without a manifest run as before and are not registered.

### 3.2 File-browser context menu (D-6)

At the end of `BrowsableDirEntry::fetch_context_items` (after the built-in
actions), for files only: for every registry app and every `filetypes` entry
whose `ext` matches the file extension, append an Action with `label`.
Actions are deleted with the menu; they must not own registry data and must
survive a registry change between open and click (look the app up by `id` and
`action` at click time). Two apps for the same extension give two entries.

### 3.3 Task menu (F5)

At boot and after every registry change: for every `tasks` entry, add an Action
to `TasksCollection::getCategory(category, order)` (existing categories are
matched by name; new ones use order 900) and call `setPersistent()`. Entries of
removed apps are hidden (`hide()`), there is no remove API.

Implementation: `TasksCollection` is built on the first F5 open, so the items
are added at that point (and on registry changes afterwards), not at boot.

### 3.4 Launch from a registry entry (D-8)

1. Look up the app by `id`. `fstat(path)`; if the file is missing: popup
   "App not found, entry removed.", remove the entry, save, hide its task
   items, stop.
2. Start through the App Loader with:
   - `selected_file`: full path of the browsed file (context menu), `NULL` (task menu)
   - `launch_source`: `U2P_LAUNCH_BROWSER` (context menu), `U2P_LAUNCH_MENU` (task menu)
   - `action`: the entry's `action` string
3. The existing loader rules (frozen C64, one app at a time) still apply.

---

### 3.5 Refresh against /Flash (T67)

The registry follows the files on `/Flash`, not the other way round: a file
deleted, replaced or copied in by FTP must show up without running it once.

1. At boot (`AppRegistry::init`, after the Flash mount) `refresh(boot)` checks
   only the `/Flash` entries: USB and SD mount later, so entries with another
   path are left alone and never dropped at boot.
   - For every `/Flash` entry: `fstat(path)`. File gone: drop the entry, log
     it. File present: read the manifest from the file (header checks of 1.3
     up to the variant table, the payload of this CPU's variant inside the
     file, then the manifest; same parser as the load). Invalid or no
     manifest: drop, log. `id` differs from the entry: drop, and the file
     counts as a new app (next step). Otherwise the stored manifest is
     replaced by the file's.
   - Scan `/Flash/apps/*.u2a`: every valid file with a manifest whose `id`
     is not registered is registered with that path (no popup, no copy). If
     the `id` is registered, the existing entry stays.
   - `apps.json` is written only if an entry was dropped, added or changed;
     then the task items are updated.
2. A launch that finds its entry stale refreshes the whole registry once
   (`refresh(false)`, all entries). It drops an entry only if its file is gone
   and its drive is there, or the file holds another app. It keeps an entry
   whose drive is not mounted ("Medium not present") and one whose file
   exists but is not a valid app (it may be being written).
   - `PUT /v1/apps/{id}:run`: unknown id or file gone: refresh once, `404`
     if still missing. Drive not mounted: `404` "medium not present". File
     there but invalid or incomplete (FTP upload): `409` "app file incomplete
     or invalid", entry kept.
   - Menu launch with a missing file: popup, `refresh(false)`; with the drive
     not mounted only the popup "Medium not present."
3. All access to the entries goes through `AppRegistry::lock()` (recursive
   mutex): refresh, registration, menus and the REST routes. A launch copies
   what it needs under the lock and releases it before popups and before the
   app runs.
4. The refresh runs on the stack of whoever asks: the boot task has a big one,
   the HTTP listener 1600 words with no overflow check. Its scratch manifests
   (1.3 KB each) are therefore on the heap (`ManifestBuffer`, and the copy in
   `PUT /v1/apps/{id}:run`); a refresh that saved `apps.json` from the HTTP task
   overran that stack (T67).

The refresh never needs the user: every state it leaves can be changed from
the menu (run the app again, delete the file).

---

## 4. Context struct

```c
typedef struct u2p_app_context_s {
    uint32_t     size;           /* sizeof as built into the firmware */
    const char  *app_path;
    const char  *selected_file;  /* browsed file, or `file` of a REST call */
    uint32_t     launch_source;  /* BROWSER, MENU or HTTP */
    const char  *args;           /* `args` of a REST call, else NULL */
    const char  *action;         /* manifest action, NULL for a plain "Run App" */
    int          has_ui;         /* 0 on a REST launch: dialogs only log */
    int        (*reply)(const char *text); /* into the REST answer, else the log */
} u2p_app_context_t;
```

A plain "Run App" sets `selected_file = NULL`, `launch_source = U2P_LAUNCH_BROWSER`,
`action = NULL`.

---

## 5. ABI (major 2)

`software/app_loader/app_api.h` is the only copy; the SDK includes it. The
root table `u2p_api_t` holds `abi_major`, `abi_minor`, its `size`,
`firmware_version` and one pointer per area. Every area table starts with its
own `size`.

| Area | Holds |
|------|-------|
| `sys` | `log`, `delay_ms` (a real sleep: ms > 0 is never shorter than ms and never a bare yield; whole 5 ms ticks rounded up, plus one), `tick_ms`, `flash_read`, `machine_handover` |
| `mem` | `malloc`, `free`, `realloc`, tracked and freed when the app returns |
| `ui` | dialogs, `getkey`/`getch`, `fkey`, `color`, progress bar, windows (`screen_*`, `win_*`), `close_menu` (the menu the app was started from closes when it returns), `screen_open_wide` (the overlay becomes 80 x N cells until `screen_close`; the firmware also takes it back when the app returns or the overlay is given back), `getch_monitor` (`getch` through the monitor keymap: w a s d stay letters, function keys are the physical ones) |
| `fs` | `fopen` .. `fclose`, `exists`, `mkdir`, `dir` (one callback per entry of a directory), `rmdir` (an empty directory); the last two are appended and size-gated (T55) |
| `c64` | `peek`/`poke`, DMA, `dma_read_frozen`, cartridge bus helpers, `cart_detect`, `cart_preference` |
| `media` | `source`, `mount`/`unmount` (as `drives:mount`/`:remove`), `run` (.prg, .crt), `reset` |
| `config` | `get`/`set` by category and item name, as `GET`/`PUT /v1/configs`; `U2P_CFG_PERSIST` also writes the category to flash |
| `diag` | development tools; `NULL` in release builds |
| `net` (2.1) | `listen`, `accept`, `recv`, `send`, `close`, `wait`: non-blocking TCP for an app that serves; the loader closes what is left open |
| `trx` (2.1) | the TRX64 Ultimate debug block: `regs`, `caps`, `acquire`/`release` of run control, `announce` of the app's RPC port; `NULL` on other bitstreams; the loader releases it when the app returns; `ring->log_read` (appended, T55) hands out the records of the debug block's log action from the frame ring |

Rules:

- Tables only grow at their end. Reordering or removing needs a new major.
- An app tests a field newer than the oldest firmware it supports with
  `U2P_HAS(api->area, field)`, which checks the table size and `NULL`.
- Calls that can fail return `U2P_OK` (0) or a positive count, else a negative
  `u2p_err_e`.
- `media->run` and `media->reset` take the machine away from the menu, so the
  loader carries out the last one requested after `app_main` has returned and
  then leaves the menu alone.

Major 1 was one flat table with a reserved tail and no size field. Its apps are
refused (section 1.3) and have to be rebuilt.

---

## 6. Multi-architecture build (D-10)

- Firmware: `app_loader.cc`, `app_api_impl.cc`, `filetype_u2a.cc` and the
  registry sources in `target/u64/nios2/ultimate/Makefile` **and**
  `target/u64ii/riscv/ultimate/Makefile`.
- SDK (`software/appsdk/`, copy in `apps/trex_crt/sdk/`):
  - `crt0/app_crt0_nios2.S` (current file), `crt0/app_crt0_riscv.S`: save `ra`,
    call `app_main(a0, a1)`, restore `ra`, return. Never set `sp` or `gp`.
  - `linker/app_nios2.lds`, `linker/app_riscv.lds`: origin `0x03A00000`, 2 MB.
  - Nios II flags unchanged (`-G0`).
  - RISC-V flags: `-march=rv32i -mabi=ilp32 -mno-relax -msmall-data-limit=0
    -ffreestanding -fno-builtin -nostdlib -Os -ffunction-sections -fdata-sections`,
    link with `--no-relax`. The build fails if `objdump -d` shows any `gp`
    reference in the app.
  - `tools/mk_u2a.py --variant nios2=<elf> --variant riscv32=<elf>
    [--manifest <json>] --name <name> --out <file.u2a>`.
  - Sample Makefiles and `build/build_app.sh` build both variants.
  - Implementation: the compile, link, gp check and `mk_u2a.py` rules live in
    one shared `app.mk` (identical in both SDK copies); app Makefiles only set
    the name, sources and manifest and include it.
- RISC-V instruction cache: the NEORV32 cache is only flushed by `fence.i`,
  which traps on cores without Zifencei (assumed for the U64-II core when no
  debugger is attached). Before an app starts, the loader executes 2 KB of NOPs
  from firmware text (`app_icache_sweep`), which refills every cache block.
- Test the RISC-V variant with `hello.u2a` in the ue2emu emulator (U64-II
  firmware). Cartridge functions cannot run there (no C64 core).
- The loader requires Freeze mode. In the emulator, pause the machine first
  with `PUT /v1/machine:pause`.

---

## 7. Cartridge tools only as an app (D-11)

- Remove Cart Inspector, Dump Cartridge and Flash Cartridge from the firmware:
  `software/io/c64/cart_*.cc/.h`, their menu actions and `MENU_C64_*` defines
  in `c64_subsys`, "Write to Cartridge" in `filetype_crt`, and the sources in
  all target Makefiles.
- Keep upstream behaviour everywhere else; all CI targets must still build.
- The `trex_crt` app provides the tools with the manifest from section 2.

---

## 8. Rules for every implementation step

- Branch: `feature/app-loader` (sections 1-7), `feature/trex-crt-app` for the
  app port. Never push; commit locally.
- Style: upstream `.clang-format` (LLVM base, 4 spaces, 120 columns, braces of
  functions and classes on their own line). No stale "Phase 1 / TODO"
  comments. English code comments.
- GitNexus: impact analysis before editing a symbol, `detect_changes` before
  every commit (see `CLAUDE.md`).
- Build both firmware targets with `build/build.sh u64` and
  `build/build.sh u64ii`, one after the other, never switching branches while a
  build runs. Before a container build, delete macOS host tool binaries left in
  `tools/` by emulator builds (`tools/64tass/64tass`, `tools/64tass/*.o`, and any
  Mach-O file in `tools/`). After a build, revert `software/wifi/raw_u64/sdkconfig`
  and `software/u64ctrl/sdkconfig`.
- Log each step in `.wolf/memory.md`; log fixed bugs in `.wolf/buglog.json`.

---

## 9. REST access

| Call | Result |
|------|--------|
| `GET /v1/apps` | registered apps: `id`, `name`, `version`, `path`, `rest` |
| `GET /v1/apps/{id}` | one of them |
| `PUT /v1/apps/{id}:run?action=&file=&args=` | starts the app, answers when it returns: `exit_code`, `output` (what it gave `ctx->reply`, at most 4 KB) |

- Only actions named in the manifest's `rest` start; `action` defaults to the
  first. Other apps answer 403.
- The launch has `launch_source = U2P_LAUNCH_HTTP`, `has_ui = 0`, `file` as
  `selected_file`, `args` unchanged.
- The loader pauses the C64 for the app (as `machine:pause`) and resumes it
  afterwards, unless the app started a program, a cartridge or a reset.
- One app at a time: a launch while another runs is answered with 423.
- The routes are in `software/api/route_apps.cc`, U64 and U64-II only.
