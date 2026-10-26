/*****************************************************************************
**	THUG PS3: 32-bit pointer slots for data used in place from files.
**
**	Some file formats (collision, for one) are mapped straight onto structs whose
**	pointer members are 4-byte offsets on disc, fixed up to real pointers after
**	loading. On a 64-bit target the structs would no longer match the files. On
**	the PS3 every address the game uses is below 4 GB, so those members can stay
**	32 bits wide: Ptr32<T> stores a pointer in 4 bytes and behaves like a T*.
**
**	DATA_PTR(T) is Ptr32<T> on 64-bit targets and a plain T* everywhere else, so
**	the PC build is unchanged. PtrBits(p) gives the raw 32-bit value of either.
*****************************************************************************/

#ifndef __CORE_PTR32_H
#define __CORE_PTR32_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// A pointer going into a 32-bit slot. Every PS3 address the game uses is below 4 GB
// today; if one ever isn't, the slot would silently hold a different address, so on
// 64-bit builds say so (once) in the log rather than crash somewhere unrelated later.
inline uint32_t PtrTo32(const void* p)
{
	uintptr_t v = (uintptr_t)p;
#if defined(__LP64__) || defined(_WIN64)
	if (v >> 32)
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			printf("PS3: pointer %p stored in a 32-bit field loses its top half\n", p);
		}
	}
#endif
	return (uint32_t)v;
}

template<typename T>
struct Ptr32
{
	// No constructors: keeps the type trivial, so it can live in unions and in
	// structs that are read straight from files.
	uint32_t m_bits;

	operator T*() const { return (T*)(uintptr_t)m_bits; }
	T* operator->() const { return (T*)(uintptr_t)m_bits; }
	T& operator*() const { return *(T*)(uintptr_t)m_bits; }
	Ptr32& operator=(T* p) { m_bits = PtrTo32(p); return *this; }
	Ptr32& operator+=(ptrdiff_t n) { return *this = (T*)*this + n; }
	Ptr32& operator-=(ptrdiff_t n) { return *this = (T*)*this - n; }
	Ptr32& operator++() { return *this += 1; }
	T* get() const { return (T*)*this; }
};

template<typename T> inline uint32_t PtrBits(T* p) { return (uint32_t)(uintptr_t)p; }
template<typename T> inline uint32_t PtrBits(const Ptr32<T>& p) { return p.m_bits; }

#if defined(__LP64__) || defined(_WIN64)
#define DATA_PTR(T) Ptr32<T>
#else
#define DATA_PTR(T) T*
#endif

#endif	// __CORE_PTR32_H
