// Decodes the video of a .bik with Code/Ps3/video/bink_video.cpp and compares every frame with
// ffmpeg's decode (raw yuv420p, frames back to back):
//   ffmpeg -i x.bik -an -fps_mode passthrough -f rawvideo -pix_fmt yuv420p ref.yuv
// usage: bink-video-test file.bik [ref.yuv]
// Build: g++ -O2 -std=c++17 -ICode/Ps3/include tools/bink-video-test/main.cpp
//        Code/Ps3/video/bink_video.cpp -o bink-video-test   (tools/bink-video-test/run.sh does all)
#include <bink_video.h>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s file.bik [ref.yuv]\n", argv[0]); return 2; }
	std::ifstream f(argv[1], std::ios::binary);
	BinkVideo b;
	if (!b.Open(&f)) { printf("FAIL: can't open %s\n", argv[1]); return 1; }
	const int w = b.Width(), h = b.Height();
	const int pw[3] = { w, (w + 1) / 2, (w + 1) / 2 }, ph[3] = { h, (h + 1) / 2, (h + 1) / 2 };
	const size_t frameBytes = (size_t)pw[0] * ph[0] + 2 * (size_t)pw[1] * ph[1];
	printf("%s: %dx%d, %.3f fps, %u frames\n", argv[1], w, h, b.Fps(), b.FrameCount());

	FILE* ref = argc > 2 ? fopen(argv[2], "rb") : 0;
	if (argc > 2 && !ref) { printf("FAIL: can't open %s\n", argv[2]); return 1; }
	std::vector<uint8_t> rf(frameBytes), first(frameBytes);

	unsigned frames = 0, mismatched = 0, refFrames = 0;
	int maxDiff = 0;
	double decodeSecs = 0;
	for (;;)
	{
		auto t0 = std::chrono::steady_clock::now();
		bool ok = b.DecodeFrame();
		decodeSecs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (!ok) break;
		if (frames == 0)
		{
			uint8_t* o = &first[0];
			for (int p = 0; p < 3; ++p)
				for (int y = 0; y < ph[p]; ++y, o += pw[p])
					memcpy(o, b.Plane(p) + y * b.Stride(p), pw[p]);
		}
		++frames;
		if (!ref) continue;
		if (fread(&rf[0], 1, frameBytes, ref) != frameBytes) continue;
		++refFrames;
		int frameMax = 0;
		const uint8_t* r = &rf[0];
		for (int p = 0; p < 3; ++p)
			for (int y = 0; y < ph[p]; ++y, r += pw[p])
			{
				const uint8_t* d = b.Plane(p) + y * b.Stride(p);
				for (int x = 0; x < pw[p]; ++x)
				{
					int diff = abs((int)d[x] - (int)r[x]);
					if (diff > frameMax) frameMax = diff;
				}
			}
		if (frameMax)
		{
			if (mismatched < 5) printf("  frame %u differs (max %d)\n", frames - 1, frameMax);
			++mismatched;
		}
		if (frameMax > maxDiff) maxDiff = frameMax;
	}
	if (ref)
	{
		// ffmpeg frames left over = frames we failed to produce.
		while (fread(&rf[0], 1, frameBytes, ref) == frameBytes) ++refFrames;
		fclose(ref);
	}

	// Rewind must give the same first frame again.
	b.Rewind();
	bool rewindOk = b.DecodeFrame();
	const uint8_t* o = &first[0];
	for (int p = 0; p < 3 && rewindOk; ++p)
		for (int y = 0; y < ph[p]; ++y, o += pw[p])
			if (memcmp(o, b.Plane(p) + y * b.Stride(p), pw[p])) { rewindOk = false; break; }

	printf("  decoded %u frames in %.3f s: %.2f ms/frame\n", frames, decodeSecs, frames ? decodeSecs * 1000 / frames : 0.0);
	if (!ref) return frames == b.FrameCount() ? 0 : 1;
	bool pass = frames == refFrames && frames == b.FrameCount() && mismatched == 0 && rewindOk;
	printf("  ffmpeg frames %u, max abs diff %d, mismatched frames %u, rewind %s -> %s\n",
		refFrames, maxDiff, mismatched, rewindOk ? "ok" : "BAD", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
