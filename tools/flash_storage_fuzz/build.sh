#!/bin/sh
# Builds the fuzzers into $OUT (default: build/):
#   ZMS=<zms.c folder>    (default: ext/zephyrproject-rtos/zephyr/zms, the Zephyr release)
#   PATCH=0               (do not apply our fixes in zms.c.patch, e.g. for upstream)
#   SUFFIX=<name suffix>  (e.g. -upstream)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../..
SHIM=$ROOT/ext/zephyrproject-rtos/zms
OUT=${OUT:-$HERE/build}
mkdir -p "$OUT/cfg"
# A copy of ZMS with our fixes applied, like the modm:zms module does
SRC=${ZMS:-$ROOT/ext/zephyrproject-rtos/zephyr/zms}
ZMS=$OUT/zms-src$SUFFIX
rm -rf "$ZMS" && mkdir -p "$ZMS" && cp -R "$SRC/." "$ZMS/"
if [ "${PATCH:-1}" != 0 ]; then patch -s -p1 -d "$ZMS" < "$SHIM/zms.c.patch"; fi
printf '#define CONFIG_ZMS_LOOKUP_CACHE 1\n#define CONFIG_ZMS_LOOKUP_CACHE_SIZE 32\n#define CONFIG_ZMS_LOOKUP_CACHE_MANUAL 0\n#define CONFIG_ZMS_DATA_CRC 1\n#define CONFIG_ZMS_NO_DOUBLE_WRITE 1\n' > "$OUT/cfg/zms_config.h"
CFLAGS="-O2 -g -I$OUT/cfg -I$HERE"
cc $CFLAGS -o "$OUT/golden" "$HERE/golden_fuzz.c"
# The port uses the CRCs of modm::math
c++ -std=c++20 $CFLAGS -I"$SHIM/shim" -I"$ROOT/src" -c -o "$OUT/zms_port.o" "$SHIM/zms_port.cpp"
cc $CFLAGS -I"$SHIM/shim" -I"$ZMS" -o "$OUT/zms$SUFFIX" "$HERE/zms_fuzz.c" "$ZMS/zms.c" "$OUT/zms_port.o"

# Coverage guided with libFuzzer, ASan and UBSan (Apple clang has no libFuzzer runtime)
LFCC=${LFCC:-$(ls /opt/homebrew/opt/llvm*/bin/clang 2>/dev/null | tail -1)}
if [ -n "$LFCC" ]; then
	LFFLAGS="-O1 -g -DLIBFUZZER -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -I$OUT/cfg -I$HERE"
	$LFCC $LFFLAGS -x c++ -std=c++20 -I"$SHIM/shim" -I"$ROOT/src" -c -o "$OUT/zms_port-lf.o" "$SHIM/zms_port.cpp"
	$LFCC $LFFLAGS -DMAXV=1800 -I"$SHIM/shim" -I"$ZMS" -o "$OUT/zms-lf$SUFFIX" \
		"$HERE/zms_fuzz.c" "$ZMS/zms.c" "$OUT/zms_port-lf.o"
	$LFCC $LFFLAGS -o "$OUT/golden-lf" "$HERE/golden_fuzz.c"
fi
