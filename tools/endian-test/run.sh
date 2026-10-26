#!/usr/bin/env bash
# Builds and runs the standalone loader tests (no PS3 toolchain, no game data):
#   endian_test.cpp  Code/Core/endian.h round trips, unaligned reads, Fix/Fix*Array bounds
#   bink_fuzz.cpp    the Bink audio/video decoders on synthetic and corrupted files
#
#   tools/endian-test/run.sh MODE...
#     asan    x86-64, ASan + UBSan (little-endian: the helpers must be no-ops)
#     asan32  the same with -m32 (ILP32 like the PC build; needs g++-multilib)
#     ppc64   big-endian powerpc64 under qemu-user, as on the PS3 (needs
#             g++-powerpc64-linux-gnu and qemu-user; -D__CPU_BIG_ENDIAN__=1 like Makefile.ps3)
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tools/endian-test"
OUT="${ENDIAN_TEST_OUT:-$(mktemp -d)}"
FUZZ_ROUNDS="${FUZZ_ROUNDS:-2000}"
mkdir -p "$OUT"
[ $# -gt 0 ] || set -- asan

BINK_SRC="$ROOT/Code/Ps3/audio/bink_audio.cpp $ROOT/Code/Ps3/video/bink_video.cpp"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
WARN="-Wall -Wextra -Wno-unused-parameter"
status=0

run() {
	echo "--- $*"
	"$@" || status=1
}

for mode in "$@"; do
	case "$mode" in
	asan|asan32)
		m=$([ "$mode" = asan32 ] && echo -m32 || true)
		g++ $m -O1 -g -std=c++17 $WARN $SAN -I"$ROOT/Code" "$HERE/endian_test.cpp" -o "$OUT/endian-$mode"
		g++ $m -O1 -g -std=c++17 $WARN $SAN -I"$ROOT/Code/Ps3/include" "$HERE/bink_fuzz.cpp" $BINK_SRC -o "$OUT/bink-fuzz-$mode"
		export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
		run "$OUT/endian-$mode"
		run "$OUT/bink-fuzz-$mode" "$FUZZ_ROUNDS"
		;;
	ppc64)
		CXX="${PPC64_CXX:-powerpc64-linux-gnu-g++}"
		QEMU="${QEMU_PPC64:-qemu-ppc64}"
		$CXX -static -O2 -g -std=c++17 $WARN -D__CPU_BIG_ENDIAN__=1 -I"$ROOT/Code" "$HERE/endian_test.cpp" -o "$OUT/endian-ppc64"
		$CXX -static -O2 -g -std=c++17 $WARN -I"$ROOT/Code/Ps3/include" "$HERE/bink_fuzz.cpp" $BINK_SRC -o "$OUT/bink-fuzz-ppc64"
		file "$OUT/endian-ppc64" | grep -q "MSB" || { echo "endian-ppc64 is not a big-endian binary"; exit 1; }
		run "$QEMU" "$OUT/endian-ppc64"
		run "$QEMU" "$OUT/bink-fuzz-ppc64" "$((FUZZ_ROUNDS / 4))"
		;;
	*)
		echo "unknown mode $mode (asan, asan32, ppc64)"; exit 2 ;;
	esac
done
exit $status
