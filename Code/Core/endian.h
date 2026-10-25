/*****************************************************************************
**	THUG PS3: reading the PC game data, which is little-endian, on any host.
**
**	On little-endian machines (PC, Xbox) everything here is a plain load and
**	compiles away; on the big-endian PS3 (__CPU_BIG_ENDIAN__) values are
**	byte-swapped. Loaders of PC data call these so the byte order is handled
**	in one place.
*****************************************************************************/

#ifndef __CORE_ENDIAN_H
#define __CORE_ENDIAN_H

#include <string.h>

namespace Endian
{
#if defined(__CPU_BIG_ENDIAN__) && __CPU_BIG_ENDIAN__
	enum { kHostIsLittle = 0 };
#else
	enum { kHostIsLittle = 1 };
#endif

	inline unsigned short Swap16(unsigned short v) { return (unsigned short)((v >> 8) | (v << 8)); }
	inline unsigned int   Swap32(unsigned int v)   { return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24); }
	inline unsigned long long Swap64(unsigned long long v)
	{
		return ((unsigned long long)Swap32((unsigned int)v) << 32) | Swap32((unsigned int)(v >> 32));
	}

	// Little-endian value -> host value (and back: the operation is its own inverse).
	inline unsigned short Le16(unsigned short v) { return kHostIsLittle ? v : Swap16(v); }
	inline unsigned int   Le32(unsigned int v)   { return kHostIsLittle ? v : Swap32(v); }
	inline unsigned long long Le64(unsigned long long v) { return kHostIsLittle ? v : Swap64(v); }
	inline float LeF(float f)
	{
		unsigned int u; memcpy(&u, &f, 4); u = Le32(u); memcpy(&f, &u, 4); return f;
	}

	// Reads from a byte buffer (any alignment).
	inline unsigned short ReadLe16(const void* p) { unsigned short v; memcpy(&v, p, 2); return Le16(v); }
	inline unsigned int   ReadLe32(const void* p) { unsigned int v; memcpy(&v, p, 4); return Le32(v); }
	inline int            ReadLeS32(const void* p) { return (int)ReadLe32(p); }
	inline short          ReadLeS16(const void* p) { return (short)ReadLe16(p); }
	inline float          ReadLeF(const void* p) { float v; memcpy(&v, p, 4); return LeF(v); }

	// Swap any 2/4/8-byte value in place (between little-endian and host order).
	template<typename T> inline void Fix(T& v)
	{
		if (kHostIsLittle || sizeof(T) == 1) return;
		if (sizeof(T) == 2) { unsigned short u; memcpy(&u, &v, 2); u = Swap16(u); memcpy(&v, &u, 2); }
		else if (sizeof(T) == 4) { unsigned int u; memcpy(&u, &v, 4); u = Swap32(u); memcpy(&v, &u, 4); }
		else if (sizeof(T) == 8) { unsigned long long u; memcpy(&u, &v, 8); u = Swap64(u); memcpy(&v, &u, 8); }
	}

	// Swap arrays of 16/32-bit words in place.
	inline void Fix16Array(void* p, size_t count)
	{
		if (kHostIsLittle) return;
		unsigned short* w = (unsigned short*)p;
		for (size_t i = 0; i < count; ++i) w[i] = Swap16(w[i]);
	}
	inline void Fix32Array(void* p, size_t count)
	{
		if (kHostIsLittle) return;
		unsigned int* w = (unsigned int*)p;
		for (size_t i = 0; i < count; ++i) w[i] = Swap32(w[i]);
	}
}

#endif	// __CORE_ENDIAN_H
