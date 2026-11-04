#!/bin/bash
# Run every e2e suite that drives ue2emu instead of a device, against the
# firmware and apps of this tree. CI runs it after `make u64ii` and the app
# builds; it runs the same way on a Mac with ue2emu installed.
#
#   build/ci_emulator_suites.sh [suite ...]     # default: all of them
#
# Needs: target/u64ii/riscv/ultimate/result/ultimate.elf, update.cfw, the .u2a
# files the suites name, and ue2emu ($U64_EMU, else the default path in
# tests/e2e/lib/emulator.py). A suite that lacks something reports SKIP, which
# does not fail the run; a FAIL does.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

export U64_EMU_FIRMWARE="${U64_EMU_FIRMWARE:-$ROOT/target/u64ii/riscv/ultimate/result/ultimate.elf}"
# The cartridge suites drive the emulator's expansion port rather than asking
# for a board to be plugged in.
export U64_CART_SLOT="${U64_CART_SLOT:-emulator}"

# Without a firmware every suite would SKIP and the run would pass empty.
if [ ! -f "$U64_EMU_FIRMWARE" ]; then
    echo "no firmware at $U64_EMU_FIRMWARE: run make u64ii first" >&2
    exit 1
fi

# name | script | arguments, as in run-tests
# Not listed, and why (Alex, 2026-11-04): cart-dumper, cart-writer and
# dialog-keys drive trex_crt's screens in Gideon's menu. Since the CI image
# carries ue2emu 0.6.0 the emulator models our own bitstream for TRX firmware
# (--trx-io auto), the expansion port included, and the three expect the stock
# model: the dumper's status stays empty and the inspector never offers F1:Go.
# Gideon's menu is on its way out - the generic application replaces it
# (TRX64-Ultimate docs/adr/0057, U3) - so these come back as BDD scenarios on
# that application (TRX64-Ultimate T71), not as repairs here. They still run by
# hand:
#
#     python3 tests/e2e/cart/dumper_test.py -m all
#     python3 tests/e2e/cart/writer_test.py -m all
#     python3 tests/e2e/filemanager/dialog_keys_test.py -m all
#
SUITES=(
    "path-picker|tests/e2e/filemanager/path_picker_test.py|-m all"
    "menu-hotkeys|tests/e2e/filemanager/menu_hotkeys_test.py|-m all"
    "smart-mount|tests/e2e/filemanager/smart_mount_test.py|-m all"
    "copy-extension|tests/e2e/filemanager/copy_extension_test.py|-m all"
    "heap-info|tests/e2e/filemanager/heap_info_test.py|-m all"
    "flash-protection|tests/e2e/firmware/flash_protection_test.py|"
    "unzip-app|tests/e2e/filemanager/unzip_app_test.py|"
    "softboot-app|tests/e2e/firmware/softboot_app_test.py|"
    "trx-service-boot|tests/e2e/firmware/trx_service_boot_test.py|"
    "app-rest|tests/e2e/firmware/app_rest_test.py|"
    "menu-joystick|tests/e2e/filemanager/menu_joystick_test.py|"
)

want=("$@")
failed=()
summary=()
for entry in "${SUITES[@]}"; do
    IFS='|' read -r name script args <<< "$entry"
    if [ ${#want[@]} -gt 0 ] && [[ ! " ${want[*]} " == *" $name "* ]]; then
        continue
    fi
    echo "=== $name"
    # shellcheck disable=SC2086
    out="$(python3 "$script" $args 2>&1)"
    status=$?
    echo "$out"
    last="$(printf '%s\n' "$out" | tail -n 1)"
    summary+=("$name: $last")
    if [ $status -ne 0 ]; then
        failed+=("$name")
    fi
done

echo
echo "=== summary"
printf '%s\n' "${summary[@]}"
if [ ${#failed[@]} -gt 0 ]; then
    echo "FAILED: ${failed[*]}"
    exit 1
fi
