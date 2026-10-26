#include "ima.h"

namespace IMA
{
	// IMA functions
	void IMA::SetPointer(const unsigned char *data, size_t size, bool isPcm16, size_t blockAlign)
	{
		pcm16 = isPcm16;
		block_align = blockAlign > 4 ? blockAlign : 0;
		block = data;
		block_sample = 0;
		// Set data pointer
		ima_data = data;
		ima_size = size;

		// Setup IMA stream
		ima_pointer = ima_data;
		ima_i = 0;

		ima_index = 0;
		ima_step = 0;
	}

	void IMA::Rewind()
	{
		block = ima_data;
		block_sample = 0;

		// Setup IMA stream
		ima_pointer = ima_data;
		ima_i = 0;

		ima_index = 0;
		ima_step = 0;
	}

	bool IMA::AtEnd()
	{
		if (block_align)
			return block + 4 > ima_data + ima_size;
		return ima_pointer >= ima_data + ima_size;
	}

	// Standard IMA ADPCM step (the step size before the index update).
	signed short IMA::DecodeNibble(unsigned char nibble)
	{
		const signed long step = ima_step_table[ima_index];
		signed long diff = step >> 3;
		if (nibble & 1) diff += step >> 2;
		if (nibble & 2) diff += step >> 1;
		if (nibble & 4) diff += step;
		if (nibble & 8) ima_step -= diff;
		else ima_step += diff;
		if (ima_step < -32768) ima_step = -32768;
		else if (ima_step > 32767) ima_step = 32767;

		int index = ima_index + ima_index_table[nibble & 7];
		ima_index = (signed char)(index < 0 ? 0 : index > 88 ? 88 : index);
		return (signed short)ima_step;
	}

	signed short IMA::Fetch()
	{
		// If we're at the end of the buffer, return 0
		if (IMA::AtEnd())
			return 0;

		// Xbox ADPCM: header sample, then 2 per data byte; 1 + 2 * (block_align - 4) a block.
		if (block_align)
		{
			const unsigned char *end = ima_data + ima_size;
			size_t avail = (size_t)(end - block);
			size_t bytes = avail < block_align ? avail : block_align;
			unsigned per_block = 1 + 2 * (unsigned)(bytes - 4);
			if (block_sample == 0)
			{
				ima_step = (signed short)(block[0] | (block[1] << 8));
				ima_index = (signed char)(block[2] > 88 ? 88 : block[2]);
			}
			signed short s;
			if (block_sample == 0)
				s = (signed short)ima_step;
			else
			{
				unsigned n = block_sample - 1;
				unsigned char byte = block[4 + (n >> 1)];
				s = DecodeNibble((n & 1) ? (byte >> 4) : (byte & 0xF));
			}
			if (++block_sample >= per_block)
			{
				block += block_align;
				block_sample = 0;
			}
			return s;
		}

		// 16-bit PCM: little-endian samples, read byte by byte (any host byte order)
		if (pcm16)
		{
			if (ima_pointer + 1 >= ima_data + ima_size)
			{
				ima_pointer = ima_data + ima_size;
				return 0;
			}
			signed short s = (signed short)(ima_pointer[0] | (ima_pointer[1] << 8));
			ima_pointer += 2;
			return s;
		}

		// Get current nibble
		unsigned char nibble;
		if (ima_i & 1)
		{
			nibble = *ima_pointer >> 4;
			ima_pointer++;
		}
		else
		{
			nibble = *ima_pointer & 0xF;
		}
		ima_i ^= 1;

		// Update index
		unsigned char ima_magn = nibble & 7;
		unsigned char ima_sign = nibble >> 3;
		ima_index += ima_index_table[ima_magn];

		if (ima_index < 0)
			ima_index = 0;
		else if (ima_index > 88)
			ima_index = 88;

		// Update step
		signed long ima_delta = (ima_step_table[ima_index] >> 3) + ((ima_magn * ima_step_table[ima_index]) >> 2);
		if (ima_sign)
			ima_delta = -ima_delta;

		ima_step += ima_delta;
		if (ima_step < -0x7FFF)
			ima_step = -0x7FFF;
		else if (ima_step > 0x7FFF)
			ima_step = 0x7FFF;

		return (signed short)ima_step;
	}
}
