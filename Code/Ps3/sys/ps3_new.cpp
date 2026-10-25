// THUG PS3: the global new/delete operators, for the whole program.
//
// kisak-thug defines them inline in Core/Defines.h, sending every allocation to the game's
// memory manager (KISAK_ORIGINAL_ALLOCATOR). MSVC ignores 'inline' on these (warning C4595),
// so on PC they replace the operators everywhere. GCC keeps inline definitions per file
// instead, and C++14's sized delete (used by std::vector and friends) wasn't defined at all,
// so a buffer from the game's heaps went to newlib's free() and corrupted its heap (the
// console hung in malloc while indexing the streams). Here every form is defined once, so
// the whole program uses one allocator, as on PC.
//
// The game's memory manager isn't thread-safe; a lock makes it safe for the port's own
// threads (the audio mixer decodes music on its thread).

#include <Core/Defines.h>
#include <Sys/Mem/memman.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <sys/mutex.h>
#include <ps3_platform.h>

namespace
{
	sys_mutex_t s_lock;
	volatile bool s_lockReady;

	struct Guard
	{
		Guard()
		{
			if (!s_lockReady)
			{
				// The first allocation is made by a static constructor, before any thread.
				sys_mutex_attr_t attr;
				memset(&attr, 0, sizeof(attr));
				attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
				attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
				attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
				attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
				strcpy(attr.name, "thugnew");
				sysMutexCreate(&s_lock, &attr);
				s_lockReady = true;
			}
			sysMutexLock(s_lock, 0);
		}
		~Guard() { sysMutexUnlock(s_lock); }
	};

	void* Allocate(size_t size, bool nothrow)
	{
		if (size == 0) size = 1;
		void* p;
		{
			Guard g;
			p = Mem::Manager::sHandle().New(size, !nothrow);
		}
		if (!p && !nothrow)
		{
			Ps3Platform::Trace("[mem] operator new: out of memory (%u bytes)", (unsigned)size);
			throw std::bad_alloc();
		}
		return p;
	}

	void Release(void* p)
	{
		if (!p) return;
		Guard g;
		Mem::Manager::sHandle().Delete(p);
	}

	// Over-aligned types (alignas above the heap's 16 bytes) come from memalign instead;
	// only the aligned deletes free them, so the pairing holds.
	void* AllocateAligned(size_t size, std::align_val_t align, bool nothrow)
	{
		if (size == 0) size = 1;
		void* p = memalign((size_t)align, size);
		if (!p && !nothrow) throw std::bad_alloc();
		return p;
	}
}

void* operator new(size_t size) { return Allocate(size, false); }
void* operator new[](size_t size) { return Allocate(size, false); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return Allocate(size, true); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return Allocate(size, true); }

void operator delete(void* p) noexcept { Release(p); }
void operator delete[](void* p) noexcept { Release(p); }
void operator delete(void* p, size_t) noexcept { Release(p); }
void operator delete[](void* p, size_t) noexcept { Release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Release(p); }

void* operator new(size_t size, std::align_val_t a) { return AllocateAligned(size, a, false); }
void* operator new[](size_t size, std::align_val_t a) { return AllocateAligned(size, a, false); }
void* operator new(size_t size, std::align_val_t a, const std::nothrow_t&) noexcept { return AllocateAligned(size, a, true); }
void* operator new[](size_t size, std::align_val_t a, const std::nothrow_t&) noexcept { return AllocateAligned(size, a, true); }

void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { free(p); }
