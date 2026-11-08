#!/bin/sh
# Host tests of trex_crt (T57 spec 8.1): the app's real C files against a mock u2p_api_t and cart models.
# Host cc only, no container, about a minute. Exit 0 = all PASS.
#   run.sh             build and run
#   run.sh --golden    print the stock-traffic digests. golden/stock.txt comes from the unmodified app (029da8ef):
#                      TREX_SRC=<its src, ui_shim.c with the HOST_TEST guard> TREX_CFLAGS=-DSTOCK_ONLY run.sh --golden
here=$(cd "$(dirname "$0")" && pwd)
src=${TREX_SRC:-$here/../src}
sdk=$here/../../../software/appsdk
out=${TREX_HOST_OUT:-$here/build}
mkdir -p "$out"
CC=${CC:-cc}
CF="$TREX_CFLAGS -O1 -g -D_FORTIFY_SOURCE=0 -Wall -Wno-unused-function -DHOST_TEST -I$sdk/include -I$src -I$here -I$here/../../../software/io/c64 -std=gnu11"
objs=""
for f in main cart_base cart_fmt cart_port cart_port_u2p cart_flash cart_screen cart_view cart_view_text cart_dumper cart_writer cart_inspector eapi_drivers ui_shim; do
  $CC $CF -c "$src/$f.c" -o "$out/$f.o" || exit 1
  objs="$objs $out/$f.o"
done
$CC $CF -c "$sdk/lib/u2p_ui.c" -o "$out/u2p_ui.o" || exit 1
for f in mock carts crtbuf harness; do
  $CC $CF -c "$here/$f.c" -o "$out/$f.o" || exit 1
  objs="$objs $out/$f.o"
done
$CC -o "$out/trex_host_test" $objs "$out/u2p_ui.o" || exit 1

# S5: the cores link and run without any view - no cart_view_text.c, no cart_screen.c, no ui_shim.c, no main.c
core=""
for f in cart_base cart_fmt cart_port cart_port_u2p cart_flash cart_view cart_dumper cart_writer eapi_drivers; do
  core="$core $out/$f.o"
done
$CC $CF -c "$here/core_only.c" -o "$out/core_only.o" || exit 1
$CC -o "$out/trex_core_test" $core "$out/core_only.o" "$out/mock.o" "$out/carts.o" "$out/crtbuf.o" || exit 1

# --golden prints the digests to stdout, so nothing else may print there
golden=0
for a in "$@"; do [ "$a" = "--golden" ] && golden=1; done
cd "$here"
[ $golden = 1 ] || nice -n 15 "$out/trex_core_test" || exit 1
exec nice -n 15 "$out/trex_host_test" "$@"
