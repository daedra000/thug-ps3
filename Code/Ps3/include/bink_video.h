// Bink video decoder: decodes the video track of a Bink 1 file (.bik) to YUV 4:2:0 planes.
//
// THUG's PC movies (Data/movies/bik/*.bik) are Bink 1 revision 'i'. This decodes their
// video frames (the audio packets in each frame are skipped; BinkAudio plays those). It has no
// dependencies beyond the C++ standard library, so tools/bink-video-test can check it on a PC
// against ffmpeg, frame for frame; the output is bit-identical to ffmpeg's yuv420p.
//
// Derived from FFmpeg's Bink video decoder (libavcodec/bink.c, binkdata.h, binkdsp.c;
// Copyright (c) 2009 Konstantin Shishkov, Copyright (C) 2011 Peter Ross) and its Bink demuxer
// (libavformat/bink.c). Like FFmpeg, this file is free software: you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License as published by the Free
// Software Foundation; either version 2.1 of the License, or (at your option) any later
// version. It is distributed WITHOUT ANY WARRANTY; see the GNU LGPL for details.
#pragma once

#include <istream>
#include <vector>
#include <stdint.h>

class BinkVideo
{
public:
	BinkVideo();
	~BinkVideo();

	// Reads the header and frame index from the stream, which must stay open while decoding.
	// Returns false if it isn't a Bink 1 file this decoder handles (revisions 'f'..'k', no
	// alpha plane; the old 'b' revision isn't supported).
	bool Open(std::istream* in);

	int Width() const { return m_width; }
	int Height() const { return m_height; }
	double Fps() const { return m_fpsDen ? (double)m_fpsNum / m_fpsDen : 0.0; }
	unsigned FrameCount() const { return m_frameCount; }
	unsigned CurrentFrame() const { return m_nextFrame; }	// frames decoded since Open/Rewind

	// Decodes the next frame; false at the end of the file (or on a read error).
	bool DecodeFrame();

	// The last decoded frame: plane 0 = Y (Width x Height), 1 = U, 2 = V (each
	// (Width+1)/2 x (Height+1)/2), as ffmpeg's yuv420p. Valid until the next DecodeFrame.
	const uint8_t* Plane(int i) const { return m_cur[i]; }
	int Stride(int i) const { return m_stride[i]; }

	// Back to the first frame.
	void Rewind();

private:
	struct Bundle
	{
		int len;			// bits in the "count" field
		int treeNum;		// Huffman tree
		uint8_t syms[16];	// leaf -> symbol
		uint8_t* data;
		uint8_t* end;
		uint8_t* curDec;	// end of the decoded part (0: nothing more this plane)
		uint8_t* curPtr;	// next value to use
	};

	bool DecodePlane(class BinkVideoBits& gb, int plane, bool chroma);
	bool ReadTree(class BinkVideoBits& gb, Bundle& b);
	bool ReadBlockTypes(class BinkVideoBits& gb, Bundle& b);
	bool ReadColors(class BinkVideoBits& gb, Bundle& b);
	bool ReadPatterns(class BinkVideoBits& gb, Bundle& b);
	bool ReadMotion(class BinkVideoBits& gb, Bundle& b);
	bool ReadDcs(class BinkVideoBits& gb, Bundle& b, bool hasSign);
	bool ReadRuns(class BinkVideoBits& gb, Bundle& b);

	std::istream* m_in;
	std::vector<uint32_t> m_frameOffsets;	// one per video frame, plus the file end
	unsigned m_frameCount;
	unsigned m_nextFrame;
	int m_width, m_height;
	uint32_t m_fpsNum, m_fpsDen;
	int m_version;			// revision letter
	int m_audioTracks;
	bool m_swapPlanes;

	Bundle m_bundle[9];
	Bundle m_colHigh[16];	// trees for the high nibble of colours (only tree data used)
	int m_colLastVal;
	std::vector<uint8_t> m_bundleMem;

	// Two frames: the one being decoded and the previous one (motion reference).
	std::vector<uint8_t> m_frameMem[2];
	uint8_t* m_planes[2][3];
	uint8_t* m_cur[3];		// last decoded frame
	int m_stride[3];
	int m_curIndex;			// which of the two holds the last decoded frame (-1: none yet)

	std::vector<uint8_t> m_packet;
};
