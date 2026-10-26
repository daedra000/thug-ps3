// Bink audio decoder (see bink_audio.h).
//
// A Bink file is a list of video frames; each frame starts with one packet per audio track.
// An audio packet holds a 32-bit decoded size and then blocks of transform coefficients, each
// block padded to 32 bits. A block codes frame_len coefficients per channel (two floats, then
// run-length coded quantised values in critical bands), inverse-transformed (DCT-III, or an
// inverse real DFT for the older variant) and cross-faded into the previous block over
// frame_len / 16 samples.

#include <bink_audio.h>

#include <math.h>
#include <string.h>

// Bits are read least significant first, bytes in order (little-endian bit order).
class BinkBits
{
public:
	BinkBits(const uint8_t* data, size_t bytes) : m_data(data), m_bits(bytes * 8), m_pos(0) {}

	size_t Left() const { return m_pos < m_bits ? m_bits - m_pos : 0; }

	uint32_t Get(int n)
	{
		uint32_t v = 0;
		for (int i = 0; i < n; ++i, ++m_pos)
			if (m_pos < m_bits && ((m_data[m_pos >> 3] >> (m_pos & 7)) & 1))
				v |= 1u << i;
		return v;
	}

	void Skip(size_t n) { m_pos += n; }
	void Align32() { m_pos = (m_pos + 31) & ~(size_t)31; }

private:
	const uint8_t* m_data;
	size_t m_bits;
	size_t m_pos;
};

namespace
{
	const uint16_t kCriticalFreqs[25] = {
		100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000,
		2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500,
	};

	const uint8_t kRunLengths[16] = { 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64 };

	const double kPi = 3.14159265358979323846;

	uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
	uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

	float BitsToFloat(uint32_t u)
	{
		float f;
		memcpy(&f, &u, 4);
		return f;
	}

	// 29-bit float: 5-bit exponent, 23-bit mantissa, sign.
	float ReadFloat(BinkBits& bits)
	{
		int power = (int)bits.Get(5);
		float f = ldexpf((float)bits.Get(23), power - 23);
		return bits.Get(1) ? -f : f;
	}
}

// Inverse transforms of size n, through an n-point complex FFT.
struct BinkAudio::Transform
{
	int n;
	bool dct;
	std::vector<float> re, im;		// FFT work buffers
	std::vector<float> cosT, sinT;	// e^(+2*pi*i*k/n)
	std::vector<float> preC, preS;	// DCT: e^(i*pi*k/(2n))
	std::vector<int> bitrev;

	Transform(int size, bool useDct) : n(size), dct(useDct), re(size), im(size), cosT(size / 2), sinT(size / 2), bitrev(size)
	{
		for (int k = 0; k < n / 2; ++k)
		{
			cosT[k] = (float)cos(2.0 * kPi * k / n);
			sinT[k] = (float)sin(2.0 * kPi * k / n);
		}
		int bits = 0;
		while ((1 << bits) < n) ++bits;
		for (int i = 0; i < n; ++i)
		{
			int r = 0;
			for (int b = 0; b < bits; ++b)
				if (i & (1 << b)) r |= 1 << (bits - 1 - b);
			bitrev[i] = r;
		}
		if (dct)
		{
			preC.resize(n);
			preS.resize(n);
			for (int k = 0; k < n; ++k)
			{
				preC[k] = (float)cos(kPi * k / (2.0 * n));
				preS[k] = (float)sin(kPi * k / (2.0 * n));
			}
		}
	}

	// In place: x[j] = sum_k X[k] e^(+2*pi*i*j*k/n).
	void InverseFft()
	{
		for (int i = 0; i < n; ++i)
		{
			int j = bitrev[i];
			if (j > i)
			{
				float t = re[i]; re[i] = re[j]; re[j] = t;
				t = im[i]; im[i] = im[j]; im[j] = t;
			}
		}
		for (int len = 2; len <= n; len <<= 1)
		{
			int half = len >> 1, step = n / len;
			for (int base = 0; base < n; base += len)
			{
				for (int k = 0; k < half; ++k)
				{
					float wr = cosT[k * step], wi = sinT[k * step];
					int a = base + k, b = a + half;
					float tr = re[b] * wr - im[b] * wi;
					float ti = re[b] * wi + im[b] * wr;
					re[b] = re[a] - tr; im[b] = im[a] - ti;
					re[a] += tr; im[a] += ti;
				}
			}
		}
	}

	// DCT-III: out[m] = sum_k c_k X[k] cos(pi*k*(2m+1)/(2n)), c_0 = 1/2, scaled.
	// Computed as v[j] = Re(sum_k c_k X[k] e^(i*pi*k/(2n)) e^(2*pi*i*j*k/n)), where
	// out[2j] = v[j] and out[2j+1] = v[n-1-j].
	void Dct3(const float* in, float* out, float scale)
	{
		for (int k = 0; k < n; ++k)
		{
			float x = in[k] * (k == 0 ? 0.5f : 1.0f);
			re[k] = x * preC[k];
			im[k] = x * preS[k];
		}
		InverseFft();
		for (int j = 0; j < n / 2; ++j)
		{
			out[2 * j] = re[j] * scale;
			out[2 * j + 1] = re[n - 1 - j] * scale;
		}
	}

	// Inverse real DFT from n/2+1 bins: in[0] = DC, in[1] = Nyquist, then (re, im) pairs.
	// out[j] = scale * (X0 + Xn/2 (-1)^j + 2 sum_k (re_k cos(2*pi*j*k/n) + im_k sin(2*pi*j*k/n)))
	void InverseRdft(const float* in, float* out, float scale)
	{
		re[0] = in[0]; im[0] = 0;
		re[n / 2] = in[1]; im[n / 2] = 0;
		for (int k = 1; k < n / 2; ++k)
		{
			re[k] = in[2 * k]; im[k] = -in[2 * k + 1];
			re[n - k] = in[2 * k]; im[n - k] = in[2 * k + 1];
		}
		InverseFft();
		for (int j = 0; j < n; ++j)
			out[j] = re[j] * scale;
	}
};

BinkAudio::BinkAudio()
	: m_in(0), m_nextFrame(0), m_sampleRate(0), m_channels(0), m_useDct(false), m_versionB(false),
	  m_blockChannels(1), m_frameLen(0), m_overlapLen(0), m_numBands(0), m_root(0), m_first(true),
	  m_pendingPos(0), m_transform(0)
{
}

BinkAudio::~BinkAudio()
{
	delete m_transform;
}

bool BinkAudio::Open(std::istream* in)
{
	m_in = in;
	uint8_t h[48];
	// The real length: header sizes and offsets are checked against it (a corrupt file
	// mustn't ask for gigabytes).
	in->clear();
	in->seekg(0, std::ios::end);
	const std::streamoff length = in->tellg();
	if (length < 44) return false;
	in->seekg(0);
	if (!in->read((char*)h, 44)) return false;
	if (h[0] != 'B' || h[1] != 'I' || h[2] != 'K') return false;
	const uint8_t revision = h[3];
	m_versionB = revision == 'b';

	uint32_t fileSize = Le32(h + 4) + 8;
	if (fileSize > (uint64_t)length) fileSize = (uint32_t)length;
	const uint32_t frames = Le32(h + 8);
	const uint32_t tracks = Le32(h + 40);
	if (frames == 0 || frames > 1000000 || tracks == 0 || tracks > 256) return false;
	if ((uint64_t)frames * 4 + tracks * 12 + 48 > (uint64_t)length) return false;
	if (revision == 'k') in->seekg(4, std::ios::cur);	// a field added in that revision

	// Per track: max decoded size, then sample rate + flags, then an id. Track 0 is played.
	std::vector<uint8_t> t(tracks * 12);
	if (!in->read((char*)&t[0], t.size())) return false;
	const uint8_t* info = &t[tracks * 4];
	const int rate = Le16(info);
	const uint16_t flags = Le16(info + 2);
	if (rate <= 0) return false;
	m_sampleRate = rate;
	m_channels = (flags & 0x2000) ? 2 : 1;
	m_useDct = (flags & 0x1000) != 0;

	// Frame index: offsets (bit 0 = keyframe), the last frame ends at the file end.
	std::vector<uint8_t> idx(frames * 4);
	if (!in->read((char*)&idx[0], idx.size())) return false;
	m_frameOffsets.resize(frames + 1);
	for (uint32_t i = 0; i < frames; ++i)
		m_frameOffsets[i] = Le32(&idx[i * 4]) & ~1u;
	m_frameOffsets[frames] = fileSize;
	for (uint32_t i = 0; i < frames; ++i)
	{
		if (m_frameOffsets[i + 1] <= m_frameOffsets[i]) return false;
	}

	// Codec set-up.
	int frameLenBits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
	int codedRate = rate;
	if (m_useDct)
		m_blockChannels = m_channels;
	else
	{
		// The RDFT variant codes the channels interleaved, as one.
		m_blockChannels = 1;
		codedRate *= m_channels;
		if (!m_versionB && m_channels == 2) frameLenBits += 1;
	}
	m_frameLen = 1 << frameLenBits;
	m_overlapLen = m_frameLen / 16;
	const int halfRate = (codedRate + 1) / 2;
	m_root = m_useDct ? (float)(m_frameLen / (sqrt((double)m_frameLen) * 32768.0))
	                  : (float)(2.0 / (sqrt((double)m_frameLen) * 32768.0));
	for (int i = 0; i < 96; ++i)
		m_quantTable[i] = expf(i * 0.15289164787221953823f) * m_root;
	for (m_numBands = 1; m_numBands < 25; ++m_numBands)
		if (halfRate <= kCriticalFreqs[m_numBands - 1]) break;
	m_bands[0] = 2;
	for (int i = 1; i < m_numBands; ++i)
		m_bands[i] = (unsigned)(kCriticalFreqs[i - 1] * m_frameLen / halfRate) & ~1u;
	m_bands[m_numBands] = m_frameLen;

	delete m_transform;
	m_transform = new Transform(m_frameLen, m_useDct);
	m_previous.assign(m_blockChannels * m_overlapLen, 0.0f);
	m_blockOut.assign(m_blockChannels * m_frameLen, 0.0f);
	Rewind();
	return true;
}

void BinkAudio::Rewind()
{
	m_nextFrame = 0;
	m_first = true;
	m_pending.clear();
	m_pendingPos = 0;
	if (m_in)
	{
		m_in->clear();
		m_in->seekg(0);
	}
}

size_t BinkAudio::Read(float* out, size_t frames)
{
	size_t done = 0;
	while (done < frames)
	{
		if (m_pendingPos >= m_pending.size())
		{
			m_pending.clear();
			m_pendingPos = 0;
			if (!DecodeNextVideoFrame()) break;
			continue;
		}
		size_t avail = (m_pending.size() - m_pendingPos) / m_channels;
		size_t n = frames - done < avail ? frames - done : avail;
		memcpy(out + done * m_channels, &m_pending[m_pendingPos], n * m_channels * sizeof(float));
		m_pendingPos += n * m_channels;
		done += n;
	}
	return done;
}

bool BinkAudio::DecodeNextVideoFrame()
{
	if (!m_in || m_nextFrame + 1 >= m_frameOffsets.size()) return false;
	const uint32_t pos = m_frameOffsets[m_nextFrame];
	const uint32_t size = m_frameOffsets[m_nextFrame + 1] - pos;
	++m_nextFrame;
	// A frame too short to hold a packet size has no audio.
	if (size < 4) return true;
	// A frame starts with track 0's audio packet (its size, then the packet). The other
	// tracks and the video data follow it and are never read, so a movie's audio doesn't
	// pull its video frames off the disk a second time.
	uint8_t head[4];
	m_in->clear();
	m_in->seekg(pos);
	if (!m_in->read((char*)head, 4)) return false;
	const uint32_t audioSize = Le32(head);
	if (audioSize > size - 4) return false;
	if (audioSize < 4) return true;
	m_packet.resize(audioSize);
	if (!m_in->read((char*)&m_packet[0], audioSize)) return false;
	DecodePacket(&m_packet[0], audioSize);
	return true;
}

bool BinkAudio::DecodePacket(const uint8_t* data, size_t size)
{
	BinkBits bits(data, size);
	bits.Skip(32);	// decoded size
	const int samples = m_frameLen - m_overlapLen;
	while (bits.Left() > 0)
	{
		if (!DecodeBlock(bits, &m_blockOut[0])) return false;
		bits.Align32();
		size_t base = m_pending.size();
		if (m_blockChannels == 1)
			m_pending.insert(m_pending.end(), m_blockOut.begin(), m_blockOut.begin() + samples);
		else
		{
			m_pending.resize(base + samples * m_blockChannels);
			for (int i = 0; i < samples; ++i)
				for (int c = 0; c < m_blockChannels; ++c)
					m_pending[base + i * m_blockChannels + c] = m_blockOut[c * m_frameLen + i];
		}
	}
	return true;
}

bool BinkAudio::DecodeBlock(BinkBits& bits, float* out)
{
	float coeffs[4096 + 2];
	float quant[25];

	if (m_useDct) bits.Skip(2);

	for (int ch = 0; ch < m_blockChannels; ++ch)
	{
		float* o = out + ch * m_frameLen;
		if (m_versionB)
		{
			if (bits.Left() < 64) return false;
			coeffs[0] = BitsToFloat(bits.Get(32)) * m_root;
			coeffs[1] = BitsToFloat(bits.Get(32)) * m_root;
		}
		else
		{
			if (bits.Left() < 58) return false;
			coeffs[0] = ReadFloat(bits) * m_root;
			coeffs[1] = ReadFloat(bits) * m_root;
		}

		if (bits.Left() < (size_t)m_numBands * 8) return false;
		for (int i = 0; i < m_numBands; ++i)
		{
			unsigned v = bits.Get(8);
			quant[i] = m_quantTable[v < 95 ? v : 95];
		}

		int k = 0;
		float q = quant[0];
		int i = 2;
		while (i < m_frameLen)
		{
			int j;
			if (m_versionB)
				j = i + 16;
			else if (bits.Get(1))
				j = i + kRunLengths[bits.Get(4)] * 8;
			else
				j = i + 8;
			if (j > m_frameLen) j = m_frameLen;

			int width = (int)bits.Get(4);
			if (width == 0)
			{
				memset(coeffs + i, 0, (j - i) * sizeof(float));
				i = j;
				while (m_bands[k] < (unsigned)i)
					q = quant[k++];
			}
			else
			{
				while (i < j)
				{
					if (m_bands[k] == (unsigned)i)
						q = quant[k++];
					unsigned c = bits.Get(width);
					if (c)
						coeffs[i] = bits.Get(1) ? -q * c : q * c;
					else
						coeffs[i] = 0.0f;
					++i;
				}
			}
		}

		if (m_useDct)
		{
			coeffs[0] *= 2.0f;
			m_transform->Dct3(coeffs, o, 1.0f / m_frameLen * 2.0f);
		}
		else
			m_transform->InverseRdft(coeffs, o, 0.5f);
	}

	// Cross-fade the start of this block into the end of the last one.
	for (int ch = 0; ch < m_blockChannels; ++ch)
	{
		float* o = out + ch * m_frameLen;
		float* prev = &m_previous[ch * m_overlapLen];
		const int count = m_overlapLen * m_blockChannels;
		if (!m_first)
		{
			for (int i = 0, j = ch; i < m_overlapLen; ++i, j += m_blockChannels)
				o[i] = (prev[i] * (count - j) + o[i] * j) / count;
		}
		memcpy(prev, o + m_frameLen - m_overlapLen, m_overlapLen * sizeof(float));
	}
	m_first = false;
	return true;
}
