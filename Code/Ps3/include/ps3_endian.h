// THUG PS3: helpers for reading little-endian PC game data on the big-endian PS3.
//
// Loaders that read PC data should go through these so the byte order is handled
// in one place. On a little-endian host they compile to plain loads.
#ifndef PS3_ENDIAN_H
#define PS3_ENDIAN_H

#include <stdint.h>
#include <string.h>

namespace Endian
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	enum { kHostIsLittle = 0 };
#else
	enum { kHostIsLittle = 1 };
#endif

	static inline uint16_t Swap16(uint16_t v) { return __builtin_bswap16(v); }
	static inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }
	static inline uint64_t Swap64(uint64_t v) { return __builtin_bswap64(v); }

	// Little-endian value -> host value.
	static inline uint16_t Le16(uint16_t v) { return kHostIsLittle ? v : Swap16(v); }
	static inline uint32_t Le32(uint32_t v) { return kHostIsLittle ? v : Swap32(v); }
	static inline uint64_t Le64(uint64_t v) { return kHostIsLittle ? v : Swap64(v); }
	static inline float LeF(float f)
	{
		uint32_t u; memcpy(&u, &f, 4); u = Le32(u); memcpy(&f, &u, 4); return f;
	}

	// Swap a value of any 1/2/4/8-byte type in place (to/from little-endian).
	template<typename T> static inline void FixLe(T& v)
	{
		if (kHostIsLittle || sizeof(T) == 1) return;
		if (sizeof(T) == 2) { uint16_t u; memcpy(&u, &v, 2); u = Swap16(u); memcpy(&v, &u, 2); }
		else if (sizeof(T) == 4) { uint32_t u; memcpy(&u, &v, 4); u = Swap32(u); memcpy(&v, &u, 4); }
		else if (sizeof(T) == 8) { uint64_t u; memcpy(&u, &v, 8); u = Swap64(u); memcpy(&v, &u, 8); }
	}

	// Swap an array of 16- or 32-bit words in place.
	static inline void FixLe16Array(void* p, size_t count)
	{
		if (kHostIsLittle) return;
		uint16_t* w = (uint16_t*)p;
		for (size_t i = 0; i < count; ++i) w[i] = Swap16(w[i]);
	}
	static inline void FixLe32Array(void* p, size_t count)
	{
		if (kHostIsLittle) return;
		uint32_t* w = (uint32_t*)p;
		for (size_t i = 0; i < count; ++i) w[i] = Swap32(w[i]);
	}

	// Unaligned little-endian reads from a byte stream.
	static inline uint16_t ReadLe16(const void* p) { uint16_t v; memcpy(&v, p, 2); return Le16(v); }
	static inline uint32_t ReadLe32(const void* p) { uint32_t v; memcpy(&v, p, 4); return Le32(v); }
	static inline float    ReadLeF(const void* p)  { float v; memcpy(&v, p, 4); return LeF(v); }
}

#endif // PS3_ENDIAN_H
