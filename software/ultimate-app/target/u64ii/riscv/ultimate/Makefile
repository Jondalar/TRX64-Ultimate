
CROSS	     = riscv32-unknown-elf-
ELFTYPE      = elf32-littleriscv
ARCHITECTURE = riscv
# CPU architecture and ABI
MARCH ?= rv32im
MABI  ?= ilp32
# The core has no divider: link libraries built without the M extension
LIBMARCH ?= rv32i

include ../../../common/environment.mk
# /home/gideon/proj/ultimate/target/u64ii/riscv/ultimate/result/ultimate.elf
PRJ      =  ultimate
FINAL    =  $(RESULT)/$(PRJ).app $(RESULT)/$(PRJ).elf $(OUTPUT)/$(PRJ).sim

SRCS_C   =	itu.c \
			dump_hex.c \
			assert.c \
			profiler.c \
			croutine.c \
			event_groups.c \
			list.c \
			queue.c \
			tasks.c \
			timers.c \
			port.c \
			heap_4.c \
			ff.c \
			ffsystem.c \
			ffunicode.c \
			time.c \
			mdio.c \
			sid_coeff.c \
			riscv_main.c \
			do_ctors.c \
			malloc_lock.c \
			server.c \
			http_protocol.c \
			middleware.c \
			multipart.c \
			cmd_buffer.c \
			url.c \
			trx_lens.c

SRCS_CC	 =  u64ii_init.cc \
			color_timings.cc \
			hdmi_scan.cc \
			usb_hwinit.cc \
			i2c_drv.cc \
			hw_i2c_drv.cc \
			nau8822.cc \
			small_printf.cc \
			stream.cc \
			stream_uart.cc \
			memory_wrap.cc \
			init_function.cc \
			flash.cc \
			w25q_flash.cc \
			s25fl_flash.cc \
			s25fl_l_flash.cc \
			product.cc \
			config.cc \
			subsys.cc \
			filemanager.cc \
			file_device.cc \
			file_partition.cc \
			embedded_d64.cc \
			embedded_t64.cc \
			embedded_iso.cc \
			embedded_fat.cc \
			ramdisk.cc \
			rtc_dummy.cc \
			c64.cc \
			joystick_output.cc \
			c64_crt.cc \
			trx_cart.cc \
			trx_io.cc \
			trx_video.cc \
			trx_lv_port.cc \
			trx_ui.cc \
			trx_ring.cc \
			trx_lens_fw.cc \
			trx_timewarp_ui.cc \
			u64_machine.cc \
			c64_subsys.cc \
			screen.cc \
			keyboard_c64.cc \
			disk_image.cc \
			c1541.cc \
			wd177x.cc \
			mfmdisk.cc \
			mystring.cc \
			path.cc \
			pattern.cc \
			blockdev.cc \
			blockdev_file.cc \
			blockdev_ram.cc \
			blockdev_flash.cc \
			disk.cc \
			partition.cc \
			file_system.cc \
			diskio.cc \
			directory.cc \
			file.cc \
			filesystem_iso9660.cc \
			filesystem_root.cc \
			filesystem_d64.cc \
			filesystem_t64.cc \
			filesystem_fat.cc \
			sd_card.cc \
			sdio.cc \
			sdcard_manager.cc \
			size_str.cc \
			userinterface.cc \
			ui_elements.cc \
			form.cc \
			user_file_interaction.cc \
			editor.cc \
			hex_editor.cc \
			tree_browser.cc \
			tree_browser_state.cc \
			config_menu.cc \
			commodore_menu.cc \
			context_menu.cc \
			task_menu.cc \
			filetype_d64.cc \
			filetype_g64.cc \
			filetype_t64.cc \
			filetype_prg.cc \
			filetype_u2p.cc \
			filetype_reu.cc \
			filetype_crt.cc \
			filetype_sid.cc \
			filetype_bin.cc \
			filetype_cfg.cc \
			filetype_vpl.cc \
			filetype_u2a.cc \
			app_loader.cc \
			app_api_impl.cc \
			app_api_net.cc \
			app_manifest.cc \
			app_registry.cc \
			route_apps.cc \
			host_stream.cc \
			keyboard_vt100.cc \
			screen_vt100.cc \
			usb_base.cc \
			usb_device.cc \
			usb_scsi.cc \
			usb_hub.cc \
			usb_ax88772.cc \
			usb_hid.cc \
			usb_ms_cbi.cc \
			keyboard_usb.cc \
			network_config.cc \
			network_interface.cc \
			vfs.cc \
			ftpd.cc \
			tape_controller.cc \
			tape_recorder.cc \
			command_intf.cc \
			dos.cc \
			network_target.cc \
			control_target.cc \
			filetype_tap.cc \
			socket_stream.cc \
			rmii_interface.cc \
			home_directory.cc \
			reu_preloader.cc \
			bling_board.cc \
			led_strip.cc \
			data_streamer.cc \
			configio.cc \
			u64_config.cc \
			sid_editor.cc \
			sid_device.cc \
			sid_device_fpgasid.cc \
			sid_device_swinsid.cc \
			sid_device_armsid.cc \
			sid_device_pdsid.cc \
			sid_device_sidkick.cc \
			acia.cc \
			modem.cc \
			socket_gui.cc \
			socket_dma.cc \
			syslog.cc \
			listener_socket.cc \
			system_info.cc \
			ult_syscalls.cc \
			httpd.cc \
			json.cc \
			routes.cc \
			route_files.cc \
			route_drives.cc \
			route_runners.cc \
			route_configs.cc \
			route_machine.cc \
			route_timewarp.cc \
			route_input.cc \
			route_streams.cc \
			http_request.cc \
			http_target.cc \
			assembly_search.cc \
			assembly.cc \
			ftp_client.cc \
			filesystem_a64.cc \
			filesystem_ftp.cc \
			dma_uart.cc \
			esp32.cc \
			network_esp32.cc \
			wifi_cmd.cc \
			wifi.cc \
			sntp_time.cc \
			$(PRJ).cc


SRCS_CC +=  iec_interface.cc \
			iec_drive.cc \
			iec_channel.cc \
			iec_printer.cc \
			iec_ui.cc \
			mps_printer.cc \
			mps_printer_cbm.cc \
			mps_printer_epson.cc \
			mps_printer_ibmgp.cc \
			mps_printer_ibmpp.cc \
			mps_chargen.cc \
			mps_charset.cc \
			mps_png.cc \
			iec_ulticopy.cc \
			cbmdos_parser.cc \
			softiec_target.cc

VPATH +=	$(PATH_SW)/FreeRTOS/Source/portable/risc-v
VPATH +=	$(PATH_SW)/FreeRTOS/Source/portable/risc-v/chip_specific_extensions/RV32I_CLINT_no_extensions
VPATH +=	$(PATH_SW)/portable/riscv
VPATH +=	$(PATH_SW)/6502/sidcrt/target
VPATH +=	$(PATH_SW)/u64ii
VPATH +=	$(PATH_SW)/u64
VPATH +=	$(PATH_SW)/u64ctrl/main
VPATH +=	$(PATH_SW)/lvgl_port

# TRX64 Ultimate U5: the debug service (docs/specs/U5-observer-service.md). The
# monitor, its JSON-RPC server on 4312 and the machine control are these
# sources — the same ones the host tests build (apps/trxmon/host) — compiled
# into the firmware and driven through g_app_api. Left out: libc_min.c and
# trx_lens.c, which the firmware has itself.
VPATH +=	$(PATH_SW)/../apps/trxmon/src
SRCS_C +=	main.c rpc.c mon.c mach.c cond.c ring.c trace.c hw_trx.c \
			dis6502.c asm6502.c spans.c ui.c mjson.c ws.c mlib.c
SRCS_CC +=	trx_service.cc

# LVGL (TRX64-Ultimate T24): software/lvgl, built by target/libs/riscv/lvgl into
# liblvgl.a; lv_conf.h is in software/lvgl_port. A logo converted by
# tools/lvgl_logo.sh (software/lvgl_port/assets/trx_logo.c) goes on the splash.
# A full splash picture from tools/splash_rle.py (assets/trx_splash.c) replaces
# logo and title; the logo is then left out.
VPATH +=	$(PATH_SW)/lvgl_port/assets
LVGL_SPLASH = $(wildcard $(PATH_SW)/lvgl_port/assets/trx_splash.c)
ifneq ($(LVGL_SPLASH),)
SRCS_C += trx_splash.c
LVGL_LOGO =
else
LVGL_LOGO = $(wildcard $(PATH_SW)/lvgl_port/assets/trx_logo.c)
endif
ifneq ($(LVGL_LOGO),)
SRCS_C += trx_logo.c
endif

SRCS_ASM =

SRCS_ASMS = crt0.S port_asm.S

SRCS_6502 = bootcrt.tas cmd_test_rom.tas ulticopy.tas default_kernal.tas

SRCS_BIN =  basic.bin chars.bin module.bin sidcrt.bin muscrt.bin musplayer.bin default_chars.bin raminit.bin

SRCS_IEC = iec_code.iec

SRCS_NANO = nano_minimal.nan

PATH_INC =  $(addprefix -I, $(VPATH))
OPTIONS  = -march=$(MARCH) -mabi=$(MABI) $(EFFORT) -ffunction-sections -fdata-sections -nostartfiles -mno-div
OPTIONS += -gdwarf-2 -Os -DRISCV -DU64=2 -DUSB2513 -DOS -DIOBASE=0x10000000 -DU2P_IO_BASE=0x10100000 -DCLOCK_FREQ=100000000 -Wno-write-strings -DFP_SUPPORT=1 -DMACHINE_MONITOR=1
# The application carries no code that lifts the flash write protection or
# programs the ESP32 (doc/specs/SPEC-06 R3): even an application that runs
# wild has nothing in memory to do it with. The updaters keep that code.
OPTIONS += -DFLASH_EXTRA_PROTECTION=1
OPTIONS += -I$(PATH_SW)/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
# The image is full: from the end of .bss to the top of memory (linker.x) only the
# boot/ISR stack is left (127 KB before LVGL). LVGL's code and font and the
# splash picture or logo (up to ~145 KB) come out of the FreeRTOS heap instead: 8 MB - 384 KB.
# A larger logo needs this lowered further (TRX64-Ultimate docs/specs/T24-lvgl-ui.md §11).
OPTIONS += -DconfigTOTAL_HEAP_SIZE=0x7A0000
# U5: the service's ping says where the monitor comes from; the real build id of
# this binary goes out beside it as firmware_git (app_api sys table).
OPTIONS += -DTRXMON_BUILD=\"in-firmware\"
OPTIONS += -I$(PATH_SW)/appsdk/include
ifneq ($(LVGL_LOGO),)
OPTIONS += -DTRX_HAVE_LOGO=1
endif
ifneq ($(LVGL_SPLASH),)
OPTIONS += -DTRX_HAVE_SPLASH=1
endif
COPTIONS = $(OPTIONS) -std=gnu99
CPPOPT   = $(OPTIONS) -std=gnu++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fpermissive
LINK 	 = linker.x
LIBS    = ../../../libs/riscv/lwip/result/liblwip.a
LIBS   += ../../../libs/riscv/lvgl/result/liblvgl.a
#LFLAGS   = --gc-sections -msys-crt0=$(BSP)/obj/HAL/src/crt0.o
LFLAGS   = --gc-sections --wrap=malloc --wrap=_malloc_r --wrap=free --wrap=_free_r --wrap=realloc --wrap=_realloc_r --wrap=calloc --wrap=_calloc_r
LINKMETHOD = ld

include ../../../common/rules.mk

mps_png.o: mps_png.cc
	@echo Compiling $(<F) optimized for speed
	@$(CPP) $(CPPOPT) -O3 $(PATH_INC) -B. -c -o $(OUTPUT)/$(@F) $<
	@$(CPP) -MM $(PATH_INC) $< >$(OUTPUT)/$(@F:.o=.d)

$(PATH_SW)/6502/sidcrt/target/sidcrt.bin::
	make -C $(PATH_SW)/6502/sidcrt

$(PATH_SW)/6502/sidcrt/target/muscrt.bin::
	make -C $(PATH_SW)/6502/sidcrt

$(PATH_SW)/6502/sidcrt/target/musplayer.bin::
	make -C $(PATH_SW)/6502/sidcrt
