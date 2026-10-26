#!/usr/bin/env bash
# Syntax/type-check every PS3 source with the given checker (default: clang-based ppu-check),
# using the same defines and include paths as the PS3 Makefile. Writes one log per file
# and prints a summary of failing files.
#
#   tools/ps3-check-all.sh [checker] [file-list]
#
# Makefile.ps3 variables for the flags go in PS3_CHECK_VARS, e.g.
#   PS3_CHECK_VARS="ASSERTS=1" tools/ps3-check-all.sh ...            the asserts build
#   PS3_CHECK_VARS="CHECK_WARNINGS=1" PS3_CHECK_KEEP=1 tools/ps3-check-all.sh ...
#                                     the warning-gate set; every file's output is kept
#                                     (then: tools/warning-gate.py check
#                                      tools/warning-baseline-clang.txt "$OUT"/*.log)
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

[ -d "$ROOT/build-ps3/case-includes" ] || make -s -C "$ROOT" -f Makefile.ps3 case-includes >/dev/null
FLAGS=$(make -s -C "$ROOT" -f Makefile.ps3 print-cxxflags-check ${PS3_CHECK_VARS:-})
KEEP="${PS3_CHECK_KEEP:-0}"

check_one() {
  f="$1"; log="$OUT/$(echo "$f" | tr '/' '_').log"
  if ! $CHECK $FLAGS -c "$ROOT/Code/$f" > "$log" 2>&1; then echo "FAIL $f"; elif [ "$KEEP" != 1 ] || [ ! -s "$log" ]; then rm -f "$log"; fi
}
export -f check_one; export CHECK FLAGS ROOT OUT KEEP

xargs -a "$LIST" -P "$(nproc)" -I{} bash -c 'check_one "$@"' _ {} | sort > "$OUT/summary.txt"
echo "$(wc -l < "$OUT/summary.txt") failing of $(wc -l < "$LIST") files (logs in $OUT)"
