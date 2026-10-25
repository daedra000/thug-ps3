#!/bin/sh
# Checks that the in-game NV40 assembler (Code/Ps3/rsx/rsx_asm.cpp) writes the same
# bytes as PSL1GHT's cgcomp -a, and that every program the shader generators write
# (Code/Ps3/rsx/rsx_shadergen.cpp) assembles. Usage: run.sh PSL1GHT_SOURCE_DIR [SHADER...]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
psl=$1; shift
tmp=$(mktemp -d)
make -s -C "$psl/tools/cgcomp" >/dev/null
g++ -O1 -std=c++11 -w -I"$root/Code/Ps3/rsx/cgcomp" "$here/main.cpp" "$root/Code/Ps3/rsx/rsx_asm.cpp" "$root"/Code/Ps3/rsx/cgcomp/*.cpp -o "$tmp/rsx-asm-test"
g++ -O1 -std=c++11 -w -I"$root/Code/Ps3/rsx/cgcomp" "$here/gen_test.cpp" "$root/Code/Ps3/rsx/rsx_asm.cpp" "$root/Code/Ps3/rsx/rsx_shadergen.cpp" "$root"/Code/Ps3/rsx/cgcomp/*.cpp -o "$tmp/gen-test"
mkdir -p "$tmp/ours" "$tmp/ref" "$tmp/gen"
"$tmp/gen-test" "$tmp/gen"
# the generated programs must also come out of cgcomp itself byte for byte
set -- "$@" "$tmp"/gen/vs00*.vasm "$tmp"/gen/ff00*.fasm "$tmp"/gen/ps0*.fasm
"$tmp/rsx-asm-test" "$tmp/ours" "$@"
status=0
for f in "$@"; do
	b=$(basename "$f"); n=${b%.*}
	case "$f" in
		*.vasm) "$psl/tools/cgcomp/cgcomp" -v -a "$f" "$tmp/ref/$n.vpo" >/dev/null; ext=vpo ;;
		*) "$psl/tools/cgcomp/cgcomp" -f -a "$f" "$tmp/ref/$n.fpo" >/dev/null; ext=fpo ;;
	esac
	if cmp -s "$tmp/ours/$n.$ext" "$tmp/ref/$n.$ext"; then echo "same  $b"; else echo "DIFF  $b"; status=1; fi
done
rm -rf "$tmp"
exit $status
