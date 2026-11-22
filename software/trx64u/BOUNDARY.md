# The boundary of trx64u's libraries: what the moving code takes from outside the SDK (U18, MultiCore T239.3 P1)

Measured 2026-11-22 on MultiCore commit `3b8e2955` (the SDK 0.1.0 source): the core application built from that
commit in `u64-build-env` (`tools/build_coreapp.sh`'s steps, `make -k`), then `riscv64-elf-nm` over the objects of the
files that move (`tools/boundary.py`). A symbol is listed when one of the moving objects uses it and no moving object
defines it; the `memcpy` ... `__ashrdi3` of the SDK's `sdk/libc.c` are left out. "Provider" is the core application's
object that defines it; `(libc/lib .a)` is newlib, libgcc or lwIP. The monitor counts MultiCore's `tm_*` copy of trxmon
(our `apps/trxmon/src` replaces it, D4). The header list comes from the objects' `.d` files.

## Summary by kind

| Kind | Symbols | Users | Note |
|---|---|---|---|
| Registration | `OpRegistry::add_op`, `add_state` | all three | ours: becomes `LibOpDecl` / `LibStateDecl` in `lib_register` |
| Calls between our libraries | ring -> trx64u: `cfg_value`, `trx_target_path`; monitor -> ring: 16 `trx_ring_*` | ringops, monitor | ours: `SvcLib` exports |
| C library beyond `sdk/libc.c` | `printf`, `puts`, `snprintf`, `sprintf`, `sscanf`, `atoi`, `strtoll`, `strcasecmp`, `strspn`, `strstr`, `isalpha`, `toupper`, 64-bit `__divdi3` / `__moddi3` / `__udivdi3` / `__umoddi3`, `__errno` | all | |
| Memory | `malloc`, `calloc`, `realloc`, `free`, `operator new` / `delete` (and `[]`) | all | `SvcMem` has malloc / free / realloc |
| C++ runtime | `__cxa_atexit`, `__dso_handle` | ringfile, ringops | objects with a destructor: `FileSystem_Root`, `mstring` (R3) |
| FreeRTOS | `xTaskCreate`, `vTaskDelete`, `vTaskDelay`, `xTaskGetTickCount`, critical sections, mutexes (`xQueueCreateMutex`, semaphore take / give, `vQueueDelete`) | u64cfg, ring*, monitor, monsrv | `SvcJob` / `SvcSys` cover tasks and delay; no mutex in the API |
| lwIP | `lwip_socket` ... `lwip_close`, `ip4addr_ntoa_r` | monsrv | `SvcNet` |
| Settings | `Settings::find`, `value`, `text`, `set`, `override_value`, `layer`, `extra_get`, `extra_set`; `regset_apply_setting` | trx64u_ops, u64cfg, ringops | no settings call in the API |
| Calling operations | `ops_call`, `ops_state` | trx64u_ops, u64cfg, monitor | |
| Files | `FileManager::fopen`, `fclose`, `delete_file`, `release_mount_point`, `FileSystem_Root`, `mstring`, `pattern_match`, `get_fattime`, `rtc_time_valid`, `nf_copy`, `corefolder_dir`, `app_version_string` | ringfile, ringops, trx64u_ops | `SvcFs` |
| TRX hardware | `trx_io_read32`, `trx_io_write32`, `trx_io_caps`, `trx_io_slim`, `trx_io_resid8_filter`, `trx_io_resid8_bypass` | u64cfg, ring*, monitor | D3: ours (R4 open) |
| Board and app core | `hw_esp32_rpc`, `i2c` / `I2C_Driver::i2c_write_byte`, `ResetHdmiPll`, `SetVideoPll`, `color_timings`, `picture_set_system`, `video_sys_name`, `mouse_sink_scaling`, `keyb_joy_select`, `wasd_to_joy`, `getFpgaCapabilities`, `getMsTimer`, `isCommodoreBoard`, `boot_cold_reset`, `boot_put_builtin_rom`, `ui_set_look`, `ui_set_name_squeeze`, `media_writeback`, `rest_uic`, `uic_json_get` | trx64u_ops, u64cfg, ring*, monsrv | |
| Shared libraries (D12) | C64 cartridges: 12 `c64_cart_*` (`c64_cart_run_modplayer` falls away, D5); SID: `sid_parse`, `sid_plan`, `sid_requests`, `sid_info_text`, `sid_mus_header`, `sid_mus_stereo`; `uci_start`; `tempdisk_set`; drives: `cbm1541_drive`, `cbm1541_slot_mount`, `CbmDrive::create` / `mount` / `eject` / `mode_name` / `set_reset_with_c64`; resampling: `u64_resample.inc` | trx64u_ops, ringops, u64cfg | `SvcLib` exports of MultiCore's libraries |

## The other way: the core application calls into the moving code

These calls must leave the core application (an operation, the library poll, or an export):

- `appsvc`: the `SvcRing` table, 15 `trx_ring_*` (D3: our ring library implements `SvcRing`).
- `c64` (the core descriptor): `trx64u_register`, `u64cfg_register`, `ringops_register`, `monitor_register`,
  `trx64u_sid_service`, `trx64u_sid_cancel`, `trx64u_kernal_put`, `trx64u_basic_put`, `trx64u_load_assoc_cfg`,
  `u64cfg_power_on_keys`, `cfg_value`.
- `run` (the main loop): `mon_serve`, `mon_release`, `ring_autostart`, `trx_ring_engine_init`, `trx_ring_pending`,
  `trx_ring_unfreeze` (R2, the poll).
- `screens`: `mon_render`, `mon_rows`, `mon_line`, `mon_attr`, `mon_cursor`, `mon_sig` (the monitor's screen).
- `diag`: `trx_ring_trace_read`, `trx_ring_trace_status`, `trx_target_path`.
- `menuflows`, `netfs`, `netops`: `cfg_value`, `cfg_text`. These two are generic settings helpers that live in
  `trx64u_ops.cc` today; they belong in the core application, not in our library.
- `c64_cart`: `trx64u_uci_df1c`. `usb_hid`: `u64_get_usb_hid_config_value`.

## Size per library (R5)

From the link map of MultiCore's own trx64u image (`make own OWN_HEAP=0x950000`, commit `3b8e2955`), the sections kept
after `--gc-sections`, without debug sections. `U2A3_IMAGE_MAX` is 1 MiB of image plus bss (`libfmt.h:40`).

| Library | Code + rodata | Data | bss | Image + bss | Of 1 MiB |
|---|---|---|---|---|---|
| trx64u | 57,575 | 28 | 339,036 | 396,639 | 37.8 % |
| ring | 69,873 | 18 | 14,281 | 84,172 | 8.0 % |
| monitor | 170,115 | 284 | 475,133 | 645,532 | 61.6 % |

The monitor counts MultiCore's older `tm_*` copy of trxmon; ours is somewhat larger (trace, rpc, ring). Most of the bss
is static buffers that a resident library holds for good: in `trx64u_ops` three of 64 KiB (`op_load_mp3::buf`,
`reu_load::buf`, `write_ddr::buf`), in trxmon `s_reply` 64 KiB, `s_text` 48 KiB, `s_b64` 43 KiB (`rpc`), `s_fbuf` and
`s_move` 64 KiB each (`mon`). They count against the core's heap (T239.3 D1, D13); sharing them or taking them from
the heap while in use would leave more for sub-apps.

## Raw lists


### trx64u
- settings (12): Settings::extra_get [u64cfg]; Settings::extra_set [u64cfg]; Settings::find [trx64u_ops,u64cfg]; Settings::layer [trx64u_ops]; Settings::override_value [trx64u_ops,u64cfg]; Settings::set [u64cfg]; Settings::text [trx64u_ops,u64cfg]; Settings::value [trx64u_ops,u64cfg]
- (libc/lib .a) (8): atoi [u64cfg]; free [trx64u_ops]; isalpha [trx64u_ops]; malloc [trx64u_ops]; strcasecmp [trx64u_ops,u64cfg]; strspn [u64cfg]; toupper [trx64u_ops]
- c64_cart (7): c64_cart_active [trx64u_ops]; c64_cart_ef_clear [trx64u_ops]; c64_cart_name [trx64u_ops]; c64_cart_path [trx64u_ops]; c64_cart_run_image [trx64u_ops]; c64_cart_run_modplayer [trx64u_ops]; c64_cart_save [trx64u_ops]
- small_printf (7): printf [trx64u_ops,u64cfg]; puts [trx64u_ops,u64cfg]; snprintf [trx64u_ops,u64cfg]; sscanf [u64cfg]
- sidfile (6): sid_info_text [trx64u_ops]; sid_mus_header [trx64u_ops]; sid_mus_stereo [trx64u_ops]; sid_parse [trx64u_ops]; sid_plan [trx64u_ops]; sid_requests [trx64u_ops]
- d1541_drive (5): CbmDrive::create [trx64u_ops]; CbmDrive::eject [trx64u_ops]; CbmDrive::mode_name [trx64u_ops]; CbmDrive::mount [trx64u_ops]; CbmDrive::set_reset_with_c64 [trx64u_ops]
- ops (4): OpRegistry::add_op [trx64u_ops,u64cfg]; OpRegistry::add_state [trx64u_ops,u64cfg]
- boot (2): boot_cold_reset [trx64u_ops]; boot_put_builtin_rom [trx64u_ops]
- hw (2): hw_esp32_rpc [trx64u_ops,u64cfg]
- i2c_drv (2): I2C_Driver::i2c_write_byte [u64cfg]; i2c [u64cfg]
- keyboard_c64 (2): keyb_joy_select [u64cfg]; wasd_to_joy [u64cfg]
- memory_wrap (2): operator delete [trx64u_ops]; operator new [trx64u_ops]
- ops_glue (2): ops_call [trx64u_ops,u64cfg]
- queue (2): xQueueGenericSend [u64cfg]; xQueueSemaphoreTake [u64cfg]
- trx_io (2): trx_io_resid8_bypass [u64cfg]; trx_io_resid8_filter [u64cfg]
- u64ii_init (2): ResetHdmiPll [u64cfg]; SetVideoPll [u64cfg]
- ui (2): ui_set_look [trx64u_ops]; ui_set_name_squeeze [trx64u_ops]
- cbm1541 (1): cbm1541_drive [trx64u_ops]
- color_timings (1): color_timings [u64cfg]
- corefolder (1): corefolder_dir [trx64u_ops]
- engines (1): media_writeback [u64cfg]
- itu (1): getFpgaCapabilities [trx64u_ops]
- mousesink (1): mouse_sink_scaling [u64cfg]
- picture (1): picture_set_system [u64cfg]
- product (1): isCommodoreBoard [trx64u_ops]
- tempdisk (1): tempdisk_set [trx64u_ops]
- uci (1): uci_start [trx64u_ops]
- videomode (1): video_sys_name [u64cfg]

### ring
- (libc/lib .a) (14): __divdi3 [ringfile]; __moddi3 [ringfile]; __udivdi3 [ringfile]; __umoddi3 [ringfile]; atoi [ringfile]; calloc [ringfile]; free [ring,ringfile]; malloc [ring,ringfile]; strcasecmp [ringfile,ringops]; strstr [ringfile]; strtoll [ringfile]
- memory_wrap (12): __cxa_atexit [ringfile,ringops]; __dso_handle [ringfile,ringops]; operator delete [ringfile,ringops]; operator delete[] [ringfile,ringops]; operator new [ringfile,ringops]; operator new[] [ringfile,ringops]
- c64_cart (8): c64_cart_active [ringops]; c64_cart_crt_type [ringops]; c64_cart_for_state [ringops]; c64_cart_image_bytes [ringops]; c64_cart_image_read [ringops]; c64_cart_image_write [ringops]; c64_cart_name [ringops]; c64_cart_path [ringops]
- small_printf (8): printf [ring,ringfile,ringops]; puts [ring,ringfile]; snprintf [ringfile,ringops]; sprintf [ringfile]
- tasks (8): vTaskDelay [ring]; vTaskDelete [ringops]; vTaskEnterCritical [ringfile,ringops]; vTaskExitCritical [ringfile,ringops]; xTaskCreate [ring,ringops]
- queue (7): vQueueDelete [ringfile,ringops]; xQueueCreateMutex [ring,ringfile,ringops]; xQueueGenericSend [ring]; xQueueSemaphoreTake [ring]
- trx_io (7): trx_io_caps [ring,ringfile]; trx_io_read32 [ring,ringfile]; trx_io_slim [ring]; trx_io_write32 [ring,ringfile]
- filemanager (6): FileManager::delete_file [ringfile,ringops]; FileManager::fclose [ringfile]; FileManager::fopen [ringfile]; FileManager::release_mount_point [ringfile,ringops]
- itu (3): getFpgaCapabilities [ringfile]; getMsTimer [ring,ringfile]
- settings (3): Settings::find [ringops]; Settings::override_value [ringops]; Settings::value [ringops]
- filesystem_root (2): FileSystem_Root::FileSystem_Root [ringfile,ringops]
- mystring (2): mstring::~mstring [ringfile,ringops]
- ops (2): OpRegistry::add_op [ringops]; OpRegistry::add_state [ringops]
- pattern (2): pattern_match [ringfile,ringops]
- cbm1541 (1): cbm1541_slot_mount [ringops]
- filesystem_fat,rtc_dummy (1): get_fattime [ringfile]
- gitinfo (1): app_version_string [ringfile]
- newfile (1): nf_copy [ringfile]
- regset (1): regset_apply_setting [ringops]
- rtc_dummy (1): rtc_time_valid [ringfile]

### monitor
- (libc/lib .a) (33): __divdi3 [tm_ring,tm_rpc]; __errno [monsrv]; __moddi3 [tm_ring,tm_rpc,tm_trace]; __udivdi3 [tm_mach,tm_mlib,tm_ring,tm_rpc,tm_trace]; __umoddi3 [tm_mlib,tm_ring,tm_rpc,tm_trace]; calloc [monitor,monsrv]; free [monitor,monsrv]; ip4addr_ntoa_r [monsrv]; lwip_accept [monsrv]; lwip_bind [monsrv]; lwip_close [monsrv]; lwip_fcntl [monsrv]; lwip_getpeername [monsrv]; lwip_htons [monsrv]; lwip_listen [monsrv]; lwip_recv [monsrv]; lwip_send [monsrv]; lwip_setsockopt [monsrv]; lwip_socket [monsrv]; malloc [monitor]; realloc [monitor]
- small_printf (4): printf [monitor,monsrv]; snprintf [monitor,monsrv]
- tasks (4): vTaskDelay [monitor,monsrv]; xTaskGetTickCount [monitor,monsrv]
- ops (2): OpRegistry::add_op [monitor]; OpRegistry::add_state [monitor]
- trx_io (2): trx_io_caps [monitor]; trx_io_write32 [monitor]
- ops_glue (1): ops_state [monitor]
- rest (1): rest_uic [monsrv]
- uic_json (1): uic_json_get [monsrv]

### cross
- ring->trx64u: cfg_value [ringops]; trx_target_path [ringops]
- monitor->ring: trx_ring_commit [monitor]; trx_ring_error [monitor]; trx_ring_find [monitor]; trx_ring_fps [monitor]; trx_ring_fps_x100 [monitor]; trx_ring_frame [monitor]; trx_ring_frame_cycles [monitor]; trx_ring_land [monitor]; trx_ring_landed [monitor]; trx_ring_log_read [monitor]; trx_ring_reachable [monitor]; trx_ring_status [monitor]; trx_ring_trace_ctl [monitor]; trx_ring_trace_read [monitor]; trx_ring_trace_status [monitor]; trx_ring_trace_whowrote [monitor]

### reverse (app core objects calling into the moving code)
- appsvc: trx_ring_commit (ring); trx_ring_error (ring); trx_ring_find (ring); trx_ring_fps (ring); trx_ring_fps_x100 (ring); trx_ring_frame (ring); trx_ring_frame_cycles (ring); trx_ring_land (ring); trx_ring_landed (ring); trx_ring_reachable (ring); trx_ring_status (ring); trx_ring_trace_ctl (ring); trx_ring_trace_read (ring); trx_ring_trace_status (ring); trx_ring_trace_whowrote (ring)
- c64: cfg_value (trx64u); monitor_register (monitor); ringops_register (ring); trx64u_basic_put (trx64u); trx64u_kernal_put (trx64u); trx64u_load_assoc_cfg (trx64u); trx64u_register (trx64u); trx64u_sid_cancel (trx64u); trx64u_sid_service (trx64u); u64cfg_power_on_keys (trx64u); u64cfg_register (trx64u)
- c64_cart: trx64u_uci_df1c (trx64u)
- diag: trx_ring_trace_read (ring); trx_ring_trace_status (ring); trx_target_path (trx64u)
- menuflows: cfg_value (trx64u)
- netfs: cfg_text (trx64u)
- netops: cfg_text (trx64u); cfg_value (trx64u)
- run: cfg_value (trx64u); mon_release (monitor); mon_serve (monitor); ring_autostart (ring); trx_ring_engine_init (ring); trx_ring_pending (ring); trx_ring_unfreeze (ring)
- screens: mon_attr (monitor); mon_cursor (monitor); mon_line (monitor); mon_render (monitor); mon_rows (monitor); mon_sig (monitor)
- usb_hid: u64_get_usb_hid_config_value (trx64u)

### headers outside the SDK and outside the moving set (from the .d files; inline code, macros, types)
- app/edit/lineedit.h [trx64u_ops]
- app/lib/cbm1541/d1541_drive.h [trx64u_ops,ringops]
- app/lib/cbm1541/d1541_gcr.h [trx64u_ops,ringops]
- app/lib/cbm1541/d1541_wd.h [trx64u_ops,ringops]
- app/loaders/kickstart.h [u64cfg]
- app/video/edid.h [trx64u_ops,u64cfg]
- app/video/videomode.h [trx64u_ops,u64cfg]
- coreapp/cores/c64_cart.h [trx64u_ops,ringops]
- coreapp/cores/c64_regs.h [trx64u_ops]
- coreapp/cores/cbm1541.h [trx64u_ops,ringops]
- coreapp/src/board.h [trx64u_ops,u64cfg,ringops,monitor,monsrv]
- coreapp/src/boot.h [trx64u_ops]
- coreapp/src/corefolder.h [trx64u_ops]
- coreapp/src/ctx.h [trx64u_ops,u64cfg,ringops,monitor,monsrv]
- coreapp/src/desc.h [trx64u_ops,u64cfg,ringops,monitor,monsrv]
- coreapp/src/engines.h [u64cfg]
- coreapp/src/fbwin.h [u64cfg]
- coreapp/src/filemgr.h [trx64u_ops]
- coreapp/src/hotkeys.h [trx64u_ops]
- coreapp/src/httpreq.h [monsrv]
- coreapp/src/hw.h [trx64u_ops,u64cfg]
- coreapp/src/io.h [trx64u_ops,u64cfg,ringops,monitor,monsrv]
- coreapp/src/ledload.h [u64cfg]
- coreapp/src/mediamounts.h [trx64u_ops]
- coreapp/src/menuctl.h [trx64u_ops]
- coreapp/src/menuflows.h [trx64u_ops]
- coreapp/src/menuintent.h [trx64u_ops]
- coreapp/src/menutree.h [trx64u_ops]
- coreapp/src/menuvlist.h [trx64u_ops]
- coreapp/src/mousesink.h [u64cfg]
- coreapp/src/newfile.h [trx64u_ops,ringfile,ringops]
- coreapp/src/ops_glue.h [trx64u_ops,u64cfg,ringops,monitor]
- coreapp/src/partial.h [trx64u_ops,u64cfg]
- coreapp/src/picklist.h [trx64u_ops]
- coreapp/src/picmodes.h [trx64u_ops,u64cfg]
- coreapp/src/picture.h [trx64u_ops,u64cfg]
- coreapp/src/rawkeys.h [u64cfg]
- coreapp/src/regset.h [ringops]
- coreapp/src/resid8_filter.h [u64cfg]
- coreapp/src/rest.h [monsrv]
- coreapp/src/romfind.h [trx64u_ops,u64cfg]
- coreapp/src/romset.h [u64cfg]
- coreapp/src/sectors.h [u64cfg]
- coreapp/src/settings.h [trx64u_ops,u64cfg,ringops,monitor,monsrv]
- coreapp/src/sid_decide.h [u64cfg]
- coreapp/src/sidfile.h [trx64u_ops]
- coreapp/src/svcjson.h [monsrv]
- coreapp/src/tempdisk.h [trx64u_ops]
- coreapp/src/u64_resample.inc [u64cfg]
- coreapp/src/uci.h [trx64u_ops]
- coreapp/src/ui.h [trx64u_ops]
- coreapp/src/winslot.h [u64cfg]
- coreapp/src/writeback.h [u64cfg]
- output/gitinfo.h [ringfile]
- software/FreeRTOS/Source/FreeRTOSConfig.h [trx64u_ops,u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/include/FreeRTOS.h [u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/include/deprecated_definitions.h [u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/include/list.h [u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/include/mpu_wrappers.h [u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/include/portable.h [u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/include/projdefs.h [u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/include/queue.h [u64cfg,ring,ringfile,ringops]
- software/FreeRTOS/Source/include/semphr.h [u64cfg,ring,ringfile,ringops]
- software/FreeRTOS/Source/include/task.h [u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/FreeRTOS/Source/portable/risc-v/portmacro.h [trx64u_ops,u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/api/jsmn.h [ringfile]
- software/components/cbmname.h [ring,ringfile,ringops]
- software/components/config.h [ring,ringfile]
- software/components/factory.h [ring,ringfile,ringops]
- software/components/indexed_list.h [ring,ringfile,ringops]
- software/components/managed_array.h [ring,ringfile,ringops]
- software/components/mystring.h [ring,ringfile,ringops]
- software/components/pattern.h [ring,ringfile,ringops]
- software/components/u64_str.h [ring,ringfile,ringops]
- software/filemanager/cached_tree_node.h [ringfile,ringops]
- software/filemanager/filemanager.h [ringfile,ringops]
- software/filemanager/path.h [ring,ringfile,ringops]
- software/filesystem/blockdev.h [ring,ringfile,ringops]
- software/filesystem/directory.h [ring,ringfile,ringops]
- software/filesystem/diskio.h [ring,ringfile,ringops]
- software/filesystem/embedded_fs.h [ringfile,ringops]
- software/filesystem/file.h [ring,ringfile,ringops]
- software/filesystem/file_info.h [ring,ringfile,ringops]
- software/filesystem/file_system.h [ring,ringfile,ringops]
- software/filesystem/filesystem_root.h [ringfile,ringops]
- software/filesystem/fs_errors_flags.h [ring,ringfile,ringops]
- software/filesystem/partition.h [ring,ringfile,ringops]
- software/hostapp/configlist.h [trx64u_ops]
- software/hostapp/cor.h [trx64u_ops,u64cfg]
- software/hostapp/handover.h [trx64u_ops,u64cfg,ringops,monitor,monsrv]
- software/infra/host.h [ring,ringfile]
- software/infra/observer.h [ringfile,ringops]
- software/io/c64/c64.h [ring,ringfile]
- software/io/c64/keyboard.h [ring,ringfile,monitor]
- software/io/c64/screen.h [ring,ringfile]
- software/io/c64/trx_cart.h [ring,ringfile]
- software/io/c64/trx_io.h [u64cfg,ring,ringfile,monitor]
- software/io/flash/flash.h [ring,ringfile]
- software/io/i2c/i2c_drv.h [u64cfg]
- software/lwip/src/include/lwip/arch.h [monsrv]
- software/lwip/src/include/lwip/debug.h [monsrv]
- software/lwip/src/include/lwip/def.h [monsrv]
- software/lwip/src/include/lwip/err.h [monsrv]
- software/lwip/src/include/lwip/errno.h [monsrv]
- software/lwip/src/include/lwip/inet.h [monsrv]
- software/lwip/src/include/lwip/ip4_addr.h [monsrv]
- software/lwip/src/include/lwip/ip6_addr.h [monsrv]
- software/lwip/src/include/lwip/ip_addr.h [monsrv]
- software/lwip/src/include/lwip/mem.h [monsrv]
- software/lwip/src/include/lwip/memp.h [monsrv]
- software/lwip/src/include/lwip/netif.h [monsrv]
- software/lwip/src/include/lwip/opt.h [monsrv]
- software/lwip/src/include/lwip/pbuf.h [monsrv]
- software/lwip/src/include/lwip/priv/mem_priv.h [monsrv]
- software/lwip/src/include/lwip/priv/memp_priv.h [monsrv]
- software/lwip/src/include/lwip/priv/memp_std.h [monsrv]
- software/lwip/src/include/lwip/sockets.h [monsrv]
- software/lwip/src/include/lwip/stats.h [monsrv]
- software/network/config/arch/cc.h [monsrv]
- software/network/config/lwipopts.h [monsrv]
- software/system/integer.h [trx64u_ops,u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/system/iomap.h [trx64u_ops,u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/system/itu.h [trx64u_ops,ring,ringfile,ringops]
- software/system/product.h [trx64u_ops]
- software/system/profiler.h [trx64u_ops,u64cfg,ring,ringfile,ringops,monitor,monsrv]
- software/system/u2p.h [trx64u_ops,u64cfg,ring]
- software/system/u64.h [trx64u_ops,u64cfg,ring]
- software/u64/color_timings.h [u64cfg]
- software/userinterface/config_collection.h [ring,ringfile]
- uicore/uic.h [monsrv]
- vendor/gideon/software/appsdk/include/../../app_loader/app_api.h [monitor]
- vendor/gideon/software/appsdk/include/u2p_app_api.h [monitor]
