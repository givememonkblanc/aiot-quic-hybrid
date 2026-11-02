#!/usr/bin/env bash
# usage: build_variant.sh <s2 method> <name> [extra -D options]
# Options of the s2 build of the method, windowed rate estimator, into vendor/build-draft21-s3-<name>.
set -uo pipefail
ROOT=/home/ryzen395/aiot_quic_baseline
LOGS=$ROOT/experiments/moq/vendor/build-logs; mkdir -p $LOGS
m=$1; name=$2; shift 2
cd "$ROOT"
export IDF_TOOLS_PATH="$ROOT/experiments/vlm_split/vendor/idf-tools"
source "$ROOT/experiments/vlm_split/vendor/esp-idf-v5.3.6/export.sh" >/dev/null 2>&1
B="$ROOT/experiments/moq/vendor/build-draft21-s3-$name"
mkdir -p "$B"
[ -f "$B/sdkconfig.copy" ] || cp "$ROOT/experiments/moq/private/sdkconfig-ota-size" "$B/sdkconfig.copy"
opts=()
while IFS= read -r line; do
  key=${line%%:*}; val=${line#*=}
  [ "$key" = AIOT_MOQT_RATE_ESTIMATOR ] && val=2
  opts+=("-D$key=$val")
done < <(grep -E "^AIOT" "$ROOT/experiments/moq/vendor/build-draft21-s2-$m/CMakeCache.txt")
# Memory: the host shows 31 GB to Linux (the rest is the GPU carve-out) and the services take
# about 15 GB. the build runs one compiler per CPU, 32, at up to 400 MB each, which ran the
# host out of memory. The build therefore uses 8 jobs inside a scope limited to 6 GB, so that
# only the build fails if it ever needs more.
JOBS=${AIOT_BUILD_JOBS:-8}
{ idf.py -B "$B" -DSDKCONFIG="$B/sdkconfig.copy" "${opts[@]}" "$@" -DPROJECT_VER="moqt21-draft-s3-$name" reconfigure &&
  systemd-run --user --scope --quiet -p MemoryMax=6G -p MemorySwapMax=0 /usr/bin/cmake --build "$B" -j "$JOBS"; } > "$LOGS/$name.log" 2>&1
echo "exit=$?" >> "$LOGS/$name.log"
tail -1 "$LOGS/$name.log"
