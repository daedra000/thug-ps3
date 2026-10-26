// THUG PS3: the NV40 texture swizzle the RSX samples power-of-two textures in, for uploads
// (d3d9_rsx_draw.cpp) and its inverse for reading an uploaded texture back. Plain C++ with no
// PS3 headers, so a host test can check that the two are each other's inverse.
//
// A w x h image's texel (x, y) goes to index xt[x] | yt[y]: the bits of x and y interleaved
// from the bottom (x first) while both have bits left, then the larger dimension's remaining
// bits. Both directions walk the image in 8 x 8 blocks: with both sides 8 or more, a block's
// 64 texels are one run of 64 swizzled indices, so the scattered side of the copy stays
// within a few cache lines instead of striding across the whole level for every row.
#ifndef PS3_D3D9_SWIZZLE_H
#define PS3_D3D9_SWIZZLE_H

#include <stdint.h>
#include <string.h>

namespace Ps3Swizzle
{
	// xt holds w entries, yt h entries.
	inline void Tables(uint32_t w, uint32_t h, uint32_t* xt, uint32_t* yt)
	{
		memset(xt, 0, w * sizeof(uint32_t));
		memset(yt, 0, h * sizeof(uint32_t));
		int xbit[16], ybit[16], c = 0;
		for (int i = 0; i < 16; ++i)
		{
			xbit[i] = ybit[i] = -1;
			if ((1u << i) < w) xbit[i] = c++;
			if ((1u << i) < h) ybit[i] = c++;
		}
		for (uint32_t x = 0; x < w; ++x)
			for (int i = 0; i < 16; ++i)
				if (xbit[i] >= 0 && ((x >> i) & 1)) xt[x] |= 1u << xbit[i];
		for (uint32_t y = 0; y < h; ++y)
			for (int i = 0; i < 16; ++i)
				if (ybit[i] >= 0 && ((y >> i) & 1)) yt[y] |= 1u << ybit[i];
	}

	template <typename T>
	inline void Blocks(uint8_t* swz, uint8_t* lin, uint32_t pitch, uint32_t w, uint32_t h,
		const uint32_t* xt, const uint32_t* yt, bool toSwizzled)
	{
		T* s = (T*)swz;
		for (uint32_t by = 0; by < h; by += 8)
		{
			const uint32_t ye = by + 8 < h ? by + 8 : h;
			for (uint32_t bx = 0; bx < w; bx += 8)
			{
				const uint32_t xe = bx + 8 < w ? bx + 8 : w;
				for (uint32_t y = by; y < ye; ++y)
				{
					T* row = (T*)(lin + y * pitch);
					const uint32_t yo = yt[y];
					if (toSwizzled)
						for (uint32_t x = bx; x < xe; ++x) s[xt[x] | yo] = row[x];
					else
						for (uint32_t x = bx; x < xe; ++x) row[x] = s[xt[x] | yo];
				}
			}
		}
	}

	struct Texel3 { uint8_t b[3]; };

	// Copies between a linear image (rows `pitch` bytes apart) and its swizzled form, bpp bytes
	// a texel (1, 2, 3, 4, 8 or 16). False for any other size.
	inline bool Copy(uint8_t* swz, uint8_t* lin, uint32_t pitch, uint32_t w, uint32_t h, uint32_t bpp,
		const uint32_t* xt, const uint32_t* yt, bool toSwizzled)
	{
		struct T8 { uint32_t v[2]; };
		struct T16 { uint32_t v[4]; };
		switch (bpp)
		{
		case 1: Blocks<uint8_t>(swz, lin, pitch, w, h, xt, yt, toSwizzled); return true;
		case 2: Blocks<uint16_t>(swz, lin, pitch, w, h, xt, yt, toSwizzled); return true;
		case 3: Blocks<Texel3>(swz, lin, pitch, w, h, xt, yt, toSwizzled); return true;
		case 4: Blocks<uint32_t>(swz, lin, pitch, w, h, xt, yt, toSwizzled); return true;
		case 8: Blocks<T8>(swz, lin, pitch, w, h, xt, yt, toSwizzled); return true;
		case 16: Blocks<T16>(swz, lin, pitch, w, h, xt, yt, toSwizzled); return true;
		default: return false;
		}
	}

	// Linear (src, rows srcPitch apart) to swizzled (dst).
	inline bool Swizzle(uint8_t* dst, const uint8_t* src, uint32_t srcPitch, uint32_t w, uint32_t h, uint32_t bpp,
		const uint32_t* xt, const uint32_t* yt)
	{
		return Copy(dst, const_cast<uint8_t*>(src), srcPitch, w, h, bpp, xt, yt, true);
	}

	// Swizzled (src) to linear (dst, rows dstPitch apart).
	inline bool Deswizzle(uint8_t* dst, uint32_t dstPitch, const uint8_t* src, uint32_t w, uint32_t h, uint32_t bpp,
		const uint32_t* xt, const uint32_t* yt)
	{
		return Copy(const_cast<uint8_t*>(src), dst, dstPitch, w, h, bpp, xt, yt, false);
	}
}

#endif // PS3_D3D9_SWIZZLE_H
