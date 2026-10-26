#!/usr/bin/env bash
# Builds tools/bink-video-test on the host and checks Code/Ps3/video/bink_video.cpp against
# ffmpeg on each given .bik (every frame must match ffmpeg's yuv420p output exactly).
#   tools/bink-video-test/run.sh file.bik [file.bik ...]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${BINK_TEST_OUT:-/tmp/bink-video-test}"
FFMPEG="${FFMPEG:-ffmpeg}"
mkdir -p "$OUT"
g++ -O2 -std=c++17 -Wall -I"$ROOT/Code/Ps3/include" "$ROOT/tools/bink-video-test/main.cpp" \
	"$ROOT/Code/Ps3/video/bink_video.cpp" -o "$OUT/bink-video-test"
status=0
for f in "$@"; do
	ref="$OUT/$(basename "$f" .bik).yuv"
	"$FFMPEG" -v error -y -i "$f" -an -fps_mode passthrough -f rawvideo -pix_fmt yuv420p "$ref"
	"$OUT/bink-video-test" "$f" "$ref" || status=1
	rm -f "$ref"
done
exit $status
