// Decodes a .bik with Code/Ps3/audio/bink_audio.cpp and compares it with ffmpeg's decode
// (raw float32 little-endian interleaved: ffmpeg -i x.bik -vn -f f32le -c:a pcm_f32le ref.raw).
// usage: bink-audio-test file.bik [ref.raw] [out.raw]
#include <bink_audio.h>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstdio>
#include <chrono>

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s file.bik [ref.raw] [out.raw]\n", argv[0]); return 2; }
	std::ifstream f(argv[1], std::ios::binary);
	BinkAudio b;
	if (!b.Open(&f)) { printf("FAIL: can't open %s\n", argv[1]); return 1; }
	std::vector<float> out, buf(4096 * b.Channels());
	auto t0 = std::chrono::steady_clock::now();
	size_t n;
	while ((n = b.Read(&buf[0], 4096)) > 0) out.insert(out.end(), buf.begin(), buf.begin() + n * b.Channels());
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	double len = out.size() / (double)b.Channels() / b.SampleRate();
	printf("%s: %d Hz, %d ch, %.1f s of audio decoded in %.3f s\n", argv[1], b.SampleRate(), b.Channels(), len, secs);
	if (argc > 3) { FILE* o = fopen(argv[3], "wb"); fwrite(out.data(), 4, out.size(), o); fclose(o); }
	if (argc < 3) return 0;
	std::ifstream r(argv[2], std::ios::binary | std::ios::ate);
	std::vector<float> ref(r.tellg() / 4);
	r.seekg(0); r.read((char*)ref.data(), ref.size() * 4);
	double maxErr = 0, sig = 0, err = 0;
	size_t m = std::min(ref.size(), out.size());
	for (size_t i = 0; i < m; ++i) { double d = out[i] - ref[i]; maxErr = std::max(maxErr, std::fabs(d)); err += d * d; sig += (double)ref[i] * ref[i]; }
	double snr = err > 0 ? 10 * std::log10(sig / err) : 999;
	// Rewind must give the same samples again.
	b.Rewind();
	std::vector<float> again(buf.size());
	size_t k = b.Read(&again[0], 4096);
	bool rewindOk = k > 0 && std::equal(again.begin(), again.begin() + k * b.Channels(), out.begin());
	bool ok = out.size() == ref.size() && snr > 80 && rewindOk;
	printf("  samples %zu (ffmpeg %zu), max error %.2e, SNR %.1f dB, rewind %s -> %s\n", out.size(), ref.size(), maxErr, snr, rewindOk ? "ok" : "BAD", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
