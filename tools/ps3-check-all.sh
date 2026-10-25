#!/usr/bin/env bash
# Syntax/type-check every PS3 source with the given checker (default: clang-based ppu-check),
# using the same defines and include paths as the PS3 Makefile. Writes one log per file
# and prints a summary of failing files.
#
#   tools/ps3-check-all.sh [checker] [file-list]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHECK="${1:-$HOME/ps3sdk/ppu-check}"
LIST="${2:-}"
OUT="${PS3_CHECK_OUT:-/tmp/ps3check}"
mkdir -p "$OUT"; rm -f "$OUT"/*.log

if [ -z "$LIST" ]; then
  LIST="$OUT/sources.txt"
  make -s -C "$ROOT" -f Makefile.ps3 print-sources > "$LIST"
fi

FLAGS=$(make -s -C "$ROOT" -f Makefile.ps3 print-cxxflags-check)

check_one() {
  f="$1"; log="$OUT/$(echo "$f" | tr '/' '_').log"
  if ! $CHECK $FLAGS -c "$ROOT/Code/$f" > "$log" 2>&1; then echo "FAIL $f"; else rm -f "$log"; fi
}
export -f check_one; export CHECK FLAGS ROOT OUT

xargs -a "$LIST" -P "$(nproc)" -I{} bash -c 'check_one "$@"' _ {} | sort > "$OUT/summary.txt"
echo "$(wc -l < "$OUT/summary.txt") failing of $(wc -l < "$LIST") files (logs in $OUT)"
