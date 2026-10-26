// THUG PS3: the decode-ahead buffer of one music or ambient stream (Gel/Music/SDL/p_music.cpp).
//
// A streaming thread decodes each stream (Bink file reads and decoding) into its ring, at the
// stream's own sample rate and channel count. The mixer thread only reads the ring: it
// resamples to 48 kHz, maps the channels onto the game's five (FL FR C BL BR) and adds them,
// scaled, into the mix. It never waits for the streaming thread. If the ring runs dry, the
// stream is silent for the rest of that block and carries on where it was once the decoder
// catches up.
//
// One thread writes (the producer) and one reads (the consumer, the mixer under its lock), so
// the ring needs no lock: each side only stores its own position, with release ordering, and
// loads the other's with acquire ordering.
//
// The resampler reproduces the one the mixer used to run on the decoder directly, including
// when it moved its window: it worked on decodes of kChunk frames, keeping the frame it was on
// and dropping the ones before it each time it needed more. The consumer tracks the frames the
// old code would have had decoded (m_have) and moves its window at the same points, so the
// position arithmetic, and so the samples, come out the same as before.
//
// Rewind: the game asks for one (RequestRestart, under the mixer lock), the producer rewinds
// its decoder and starts a new run of frames (Restart), and the consumer drops whatever was
// buffered before that run once it sees it. Until then the stream is silent.
//
// Plain C++ with GCC/Clang atomic built-ins, so it builds and can be tested on a PC too.
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

class StreamRing
{
public:
	enum { kChunk = 2048 };	// frames per decode, as the mixer used to ask the decoder for

	StreamRing()
		: m_data(0), m_channels(0), m_mask(0), m_step(1.0),
		  m_write(0), m_end(0), m_epoch(0), m_restartAt(0), m_prodEpoch(0),
		  m_read(0), m_wantEpoch(0),
		  m_base(0), m_have(0), m_vend(false), m_pos(0.0), m_consEpoch(0)
	{
	}

	~StreamRing() { free(m_data); }

	// channels: 1, 2 (to the front pair) or 5 (as they are). rate: the decoder's sample rate.
	// capacity: frames, a power of two. The memory comes from malloc: the mixer and streaming
	// threads aren't the game thread, and the game's heaps are for the game thread.
	bool Init(int channels, int rate, uint32_t capacity)
	{
		if ((channels != 1 && channels != 2 && channels != 5) || rate <= 0 ||
		    capacity < 2 * kChunk || (capacity & (capacity - 1)) != 0)
			return false;
		m_data = (float *)malloc(sizeof(float) * channels * capacity);
		if (m_data == 0)
			return false;
		m_channels = channels;
		m_mask = capacity - 1;
		m_step = rate / 48000.0;
		return true;
	}

	bool Valid() const { return m_data != 0; }
	int Channels() const { return m_channels; }

	// --- Producer (the streaming thread) ---

	// Where the next frames go, in one piece, and how many fit (at most `most`). Write them,
	// then Commit how many were written.
	float *WriteSpace(uint32_t most, uint32_t *frames) const
	{
		const uint32_t w = m_write;
		const uint32_t used = w - __atomic_load_n(&m_read, __ATOMIC_ACQUIRE);
		const uint32_t capacity = m_mask + 1;
		uint32_t n = capacity - used;
		const uint32_t at = w & m_mask;
		if (n > capacity - at)
			n = capacity - at;
		if (n > most)
			n = most;
		*frames = n;
		return m_data + (size_t)at * m_channels;
	}

	uint32_t FreeFrames() const
	{
		return m_mask + 1 - (m_write - __atomic_load_n(&m_read, __ATOMIC_ACQUIRE));
	}

	void Commit(uint32_t frames) { __atomic_store_n(&m_write, m_write + frames, __ATOMIC_RELEASE); }

	// The decoder has no more frames (in this run).
	void SetEnd() { __atomic_store_n(&m_end, 1u, __ATOMIC_RELEASE); }
	bool AtEnd() const { return m_end != 0; }

	// True when the game asked for a rewind the producer hasn't done yet.
	bool RestartPending() const { return __atomic_load_n(&m_wantEpoch, __ATOMIC_ACQUIRE) != m_prodEpoch; }

	// Call after rewinding the decoder: frames written from now on are the new run.
	void Restart()
	{
		m_prodEpoch = __atomic_load_n(&m_wantEpoch, __ATOMIC_ACQUIRE);
		m_restartAt = m_write;
		__atomic_store_n(&m_end, 0u, __ATOMIC_RELAXED);
		__atomic_store_n(&m_epoch, m_prodEpoch, __ATOMIC_RELEASE);	// publishes the two above
	}

	// --- Game thread, under the mixer lock ---

	void RequestRestart() { __atomic_store_n(&m_wantEpoch, m_wantEpoch + 1, __ATOMIC_RELEASE); }

	// --- Consumer (the mixer, under its lock) ---

	// Adds up to `frames` 48 kHz five-channel frames, times volume[channel], into out.
	// Returns how many it added; fewer than asked means the stream ended (*ended) or the
	// decoder is behind (the rest of this block is silent for this stream).
	size_t MixInto(float *out, size_t frames, const float *volume, bool *ended)
	{
		*ended = false;
		if (m_consEpoch != m_wantEpoch)
		{
			// A rewind was asked for: wait for the producer's new run, then start on it.
			const uint32_t e = __atomic_load_n(&m_epoch, __ATOMIC_ACQUIRE);
			if (e != m_wantEpoch)
				return 0;
			m_consEpoch = e;
			m_base = m_restartAt;
			m_have = 0;
			m_vend = false;
			m_pos = 0.0;
			__atomic_store_n(&m_read, m_base, __ATOMIC_RELEASE);
		}

		const int ch = m_channels;
		size_t done = 0;
		while (done < frames)
		{
			// Need frames floor(pos) and floor(pos)+1 of the window.
			size_t i0 = (size_t)m_pos;
			if (i0 + 1 >= m_have)
			{
				if (m_vend)
				{
					if (i0 >= m_have)
					{
						*ended = true;
						break;
					}
				}
				else
				{
					// Keep the frame we're on, take kChunk more after the window.
					const uint32_t end = __atomic_load_n(&m_end, __ATOMIC_ACQUIRE);
					const uint32_t written = __atomic_load_n(&m_write, __ATOMIC_ACQUIRE);
					const uint32_t keep = i0 < m_have ? (uint32_t)i0 : m_have;
					const uint32_t decoded = m_base + m_have;
					uint32_t got;
					if (written - decoded >= (uint32_t)kChunk)
						got = kChunk;
					else if (end)
						got = written - decoded;
					else
						break;	// the decoder is behind
					m_base += keep;
					m_pos -= keep;
					m_have = m_have - keep + got;
					if (got < (uint32_t)kChunk)
						m_vend = true;
					__atomic_store_n(&m_read, m_base, __ATOMIC_RELEASE);	// frames before m_base are free
					continue;
				}
			}
			size_t i1 = i0 + 1 < m_have ? i0 + 1 : i0;
			const float *f0 = Frame(m_base + (uint32_t)i0);
			const float *f1 = Frame(m_base + (uint32_t)i1);
			float v[5];
			if (ch == 5 && m_step == 1.0)
			{
				// Already 48 kHz five-channel (the miniaudio decoder): as is.
				for (int c = 0; c < 5; c++)
					v[c] = f0[c];
			}
			else
			{
				float t = (float)(m_pos - i0);
				if (ch == 5)
				{
					for (int c = 0; c < 5; c++)
						v[c] = f0[c] * (1.0f - t) + f1[c] * t;
				}
				else
				{
					float l = f0[0] * (1.0f - t) + f1[0] * t;
					float r = ch > 1 ? f0[1] * (1.0f - t) + f1[1] * t : l;
					if (ch > 1)
					{
						v[0] = l; v[1] = r; v[2] = 0.0f; v[3] = 0.0f; v[4] = 0.0f;
					}
					else
					{
						v[0] = v[1] = v[2] = v[3] = v[4] = l;
					}
				}
			}
			for (int c = 0; c < 5; c++)
				*out++ += v[c] * volume[c];
			++done;
			m_pos += m_step;
		}
		return done;
	}

private:
	StreamRing(const StreamRing &);
	StreamRing &operator=(const StreamRing &);

	const float *Frame(uint32_t index) const { return m_data + (size_t)(index & m_mask) * m_channels; }

	float *m_data;
	int m_channels;
	uint32_t m_mask;		// capacity - 1
	double m_step;			// input frames per 48 kHz frame

	// Positions are frame counts that wrap at 2^32; differences between them are what count.
	// Written by the producer:
	uint32_t m_write;		// frames written
	uint32_t m_end;			// nonzero: no frames after m_write in this run
	uint32_t m_epoch;		// the last rewind done
	uint32_t m_restartAt;	// where that rewind's run starts
	uint32_t m_prodEpoch;	// producer's copy of m_epoch
	// Written by the consumer:
	uint32_t m_read;		// frames before this are free to overwrite
	// Written by the game thread, under the mixer lock:
	uint32_t m_wantEpoch;	// rewinds asked for

	// Consumer only: the resampler's window (frames m_base .. m_base + m_have) and position.
	uint32_t m_base;
	uint32_t m_have;
	bool m_vend;			// the window reaches the end of the stream
	double m_pos;
	uint32_t m_consEpoch;
};
