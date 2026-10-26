// Feeds the Bink decoders (Code/Ps3/audio/bink_audio.cpp, Code/Ps3/video/bink_video.cpp)
// synthetic files: well-formed headers and frame tables around random packet bits, plus
// truncated and corrupted headers. There is no game data in CI, so this doesn't compare
// pictures or sound (tools/bink-*-test do, with real .bik files); it checks that corrupt
// input never reads or writes out of bounds (run under ASan/UBSan, and on big-endian under
// qemu) and that the decoders give up or keep going instead of crashing.
#include <bink_audio.h>
#include <bink_video.h>

#include <sstream>
#include <string>
#include <vector>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0x2545f4914f6cdd1dull;
static uint32_t Next()
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return (uint32_t)(s_rng >> 16);
}

static void Put32(std::string& s, size_t at, uint32_t v)
{
	for (int i = 0; i < 4; ++i) s[at + i] = (char)(uint8_t)(v >> (8 * i));
}
static void Put16(std::string& s, size_t at, uint16_t v)
{
	s[at] = (char)(uint8_t)v;
	s[at + 1] = (char)(uint8_t)(v >> 8);
}

// A Bink file: 44-byte header, one audio track's description, the frame index, then
// frames of [audio packet size][audio bits][video bits], all random bits.
static std::string MakeBink(char revision, int width, int height, int frames, uint16_t rate, uint16_t audioFlags, int maxPacket)
{
	std::string s(44, '\0');
	s[0] = 'B'; s[1] = 'I'; s[2] = 'K'; s[3] = revision;
	Put32(s, 8, (uint32_t)frames);
	Put32(s, 12, (uint32_t)maxPacket);
	Put32(s, 16, (uint32_t)frames);
	Put32(s, 20, (uint32_t)width);
	Put32(s, 24, (uint32_t)height);
	Put32(s, 28, 30);
	Put32(s, 32, 1);
	Put32(s, 36, 0);
	Put32(s, 40, 1);					// one audio track
	if (revision == 'k') s.append(4, '\0');
	const size_t track = s.size();
	s.append(12, '\0');
	Put32(s, track, 0x10000);			// max decoded size
	Put16(s, track + 4, rate);
	Put16(s, track + 6, audioFlags);
	Put32(s, track + 8, 0);				// track id
	const size_t index = s.size();
	s.append((size_t)frames * 4, '\0');
	for (int f = 0; f < frames; ++f)
	{
		Put32(s, index + f * 4, (uint32_t)s.size() | (f == 0 ? 1u : 0u));
		const uint32_t audio = Next() % (uint32_t)maxPacket;
		const uint32_t video = 1 + Next() % (uint32_t)maxPacket;
		const size_t at = s.size();
		s.append(4, '\0');
		Put32(s, at, audio);
		for (uint32_t i = 0; i < audio + video; ++i) s.push_back((char)(uint8_t)Next());
		if (audio >= 4) Put32(s, at + 4, Next() % 4096);	// decoded sample count
	}
	Put32(s, 4, (uint32_t)s.size() - 8);
	return s;
}

static void Corrupt(std::string& s, int edits)
{
	for (int i = 0; i < edits && !s.empty(); ++i)
		s[Next() % s.size()] = (char)(uint8_t)Next();
}

static unsigned long s_audioFrames, s_videoFrames, s_opened;

static void Decode(const std::string& data)
{
	{
		std::istringstream in(data);
		BinkAudio a;
		if (a.Open(&in))
		{
			++s_opened;
			std::vector<float> buf(1024 * 2);
			for (int i = 0; i < 64; ++i)
			{
				size_t n = a.Read(&buf[0], 1024);
				if (!n) break;
				s_audioFrames += n;
			}
			a.Rewind();
			a.Read(&buf[0], 256);
		}
	}
	{
		std::istringstream in(data);
		BinkVideo v;
		if (v.Open(&in))
		{
			++s_opened;
			while (v.DecodeFrame()) ++s_videoFrames;
			v.Rewind();
			v.DecodeFrame();
		}
	}
}

int main(int argc, char** argv)
{
	const int rounds = argc > 1 ? atoi(argv[1]) : 300;
	const char revisions[] = { 'b', 'f', 'g', 'h', 'i', 'k' };
	const uint16_t rates[] = { 11025, 22050, 32000, 44100, 48000 };
	for (int r = 0; r < rounds; ++r)
	{
		const char rev = revisions[Next() % sizeof(revisions)];
		const int w = 8 + (int)(Next() % 120), h = 8 + (int)(Next() % 90);
		const uint16_t flags = (uint16_t)((Next() & 1 ? 0x1000 : 0) | (Next() & 1 ? 0x2000 : 0));
		std::string file = MakeBink(rev, w, h, 1 + (int)(Next() % 6), rates[Next() % 5], flags, 64 + (int)(Next() % 3000));
		Decode(file);
		// Truncated: the index says more than the file has.
		std::string cut = file.substr(0, Next() % file.size());
		Decode(cut);
		// Corrupted anywhere, header included.
		std::string bad = file;
		Corrupt(bad, 1 + (int)(Next() % 8));
		Decode(bad);
	}
	printf("bink-fuzz: %d rounds, %lu opened, %lu audio frames, %lu video frames decoded -> PASS (no crash)\n",
		rounds, s_opened, s_audioFrames, s_videoFrames);
	return 0;
}
