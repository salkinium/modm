#!/bin/sh
# Builds the fuzzers into $OUT (default: build/) for program units of 8 and 32 bytes:
#   FDB=<FlashDB source>  (default: ext/armink/flashdb)
#   ZMS=<zms.c folder>    (default: ext/zephyrproject-rtos/zms/zms)
#   SUFFIX=<name suffix>  (e.g. -upstream)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../..
FDB=${FDB:-$ROOT/ext/armink/flashdb}
ZMS=${ZMS:-$ROOT/ext/zephyrproject-rtos/zms/zms}
OUT=${OUT:-$HERE/build}
mkdir -p "$OUT/cfg"
printf '#define FDB_USING_KVDB\n#define FDB_USING_TSDB\n#define FDB_USING_FAL_MODE\n#define FDB_PRINT(...) ((void)0)\n' > "$OUT/cfg/fdb_cfg.h"
printf '#define CONFIG_ZMS_LOOKUP_CACHE 1\n#define CONFIG_ZMS_LOOKUP_CACHE_SIZE 32\n#define CONFIG_ZMS_LOOKUP_CACHE_MANUAL 0\n#define CONFIG_ZMS_DATA_CRC 1\n#define CONFIG_ZMS_NO_DOUBLE_WRITE 1\n' > "$OUT/cfg/zms_config.h"
CFLAGS="-O2 -g -I$OUT/cfg -I$HERE -I$ROOT/ext/armink"
for unit in 8 32; do
	cc $CFLAGS -DFDB_WRITE_GRAN=$((unit * 8)) -I"$FDB/inc" -o "$OUT/fdb$unit$SUFFIX" "$HERE/fdb_fuzz.c" \
		"$FDB/src/fdb.c" "$FDB/src/fdb_kvdb.c" "$FDB/src/fdb_tsdb.c" "$FDB/src/fdb_utils.c"
done
cc $CFLAGS -o "$OUT/golden" "$HERE/golden_fuzz.c"
cc $CFLAGS -I"$ROOT/ext/zephyrproject-rtos/zms/shim" -I"$ZMS" -o "$OUT/zms$SUFFIX" "$HERE/zms_fuzz.c" \
	"$ZMS/zms.c" "$ROOT/ext/zephyrproject-rtos/zms/zms_port.c"
