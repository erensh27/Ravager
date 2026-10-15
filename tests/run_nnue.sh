#!/bin/sh
# Oracle parity is checked separately.
set -eu
cd "$(dirname "$0")/.."
NET=$(realpath "${1:-nets/Ravager_NET.nnue}")
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
COMMON="$(find src -maxdepth 1 -name '*.c' ! -name uci.c) nnue/nnue.c nnue/inference.c src/tb/tbprobe.c"
${CC:-gcc} -O3 -std=c11 -pthread -Isrc -Innue -Itests $COMMON tests/nnue_incremental.c -lm  -o "$OUT/incremental"
${CC:-gcc} -O1 -g -std=c11 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -Innue -Itests $COMMON tests/special.c -lm  -o "$OUT/special"
${CC:-gcc} -O3 -std=c11 -pthread -Isrc -Innue -Itests $COMMON tests/nnue_loader.c -lm  -o "$OUT/loader"
for tier in scalar avx2 avx512; do
 RAVAGER_NNUE_TIER=$tier "$OUT/incremental" "$NET"
 RAVAGER_NNUE_TIER=$tier ASAN_OPTIONS=detect_leaks=0 "$OUT/special" "$NET"
done
"$OUT/loader" "$NET"
