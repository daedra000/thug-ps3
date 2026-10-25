// Bink audio decoder: plays the audio track of a Bink 1 file (.bik).
//
// The PC version of THUG keeps its music and streamed sounds (Data/streams/music/*.bik) as
// audio-only Bink files. This decodes them to float samples. It has no dependencies beyond
// the C++ standard library, so tools/bink-audio-test can check it on a PC against ffmpeg.
//
// Written from the format description at https://wiki.multimedia.cx/index.php/Bink_Audio and
// .../Bink_Container; ffmpeg's decoder was the reference its output was compared with.
#pragma once

#include <istream>
#include <vector>
#include <stdint.h>

class BinkAudio
{
public:
	BinkAudio();
	~BinkAudio();

	// Reads the header from the stream, which must stay open while decoding. Returns false if
	// it isn't a Bink 1 file with an audio track this decoder handles.
	bool Open(std::istream* in);

	int SampleRate() const { return m_sampleRate; }
	int Channels() const { return m_channels; }

	// Decodes up to `frames` sample frames (interleaved, Channels() floats each, roughly -1..1)
	// and returns how many were written; fewer than asked means the end of the file.
	size_t Read(float* out, size_t frames);

	// Back to the first sample.
	void Rewind();

private:
	struct Transform;

	bool DecodeNextVideoFrame();
	bool DecodePacket(const uint8_t* data, size_t size);
	bool DecodeBlock(class BinkBits& bits, float* out);

	std::istream* m_in;
	std::vector<uint32_t> m_frameOffsets;	// one per video frame, plus the file end
	size_t m_nextFrame;

	int m_sampleRate;
	int m_channels;			// output channels
	bool m_useDct;
	bool m_versionB;		// 'BIKb' files store floats differently
	int m_blockChannels;	// channels coded per block: the DCT codes each, RDFT one interleaved
	int m_frameLen;			// transform size
	int m_overlapLen;
	int m_numBands;
	unsigned m_bands[26];
	float m_root;
	float m_quantTable[96];
	bool m_first;

	std::vector<float> m_previous;	// last block's overlap, per coded channel
	std::vector<float> m_blockOut;	// one block's samples, per coded channel
	std::vector<float> m_pending;	// decoded, interleaved, not yet read
	size_t m_pendingPos;
	std::vector<uint8_t> m_packet;
	Transform* m_transform;
};
