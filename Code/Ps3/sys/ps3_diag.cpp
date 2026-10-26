// THUG PS3: start-up diagnostics switched on by files in USRDIR (see ps3_platform.h).
//
//   nothreads.txt  no threads of our own: no system thread, audio mixer or Win32 timer
//                  threads; the game's CreateThread calls are refused (and logged)
//   memcap.txt     during start-up, every malloc/calloc/realloc/memalign over 10 MB fails
//
// Every allocation of 1 MB or more during start-up is written to the boot trail
// (thug-ps3-boot.txt) with its size and result, whatever the switches.
//
// The allocator is wrapped at link time (-Wl,--wrap=malloc,... in Makefile.ps3). The
// wrappers must not allocate, so they format numbers by hand and write with plain lv2 calls.
// They also count newlib's bytes in use, for the [mem] report (MemoryReport, at the end).

#include <core/defines.h>
#include <sys/mem/memman.h>
#include <ps3_platform.h>
#include <ps3_fs.h>

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <malloc.h>
#include <lv2/sysfs.h>
#include <sys/file.h>

extern "C"
{
	void* __real_malloc(size_t size);
	void* __real_calloc(size_t n, size_t size);
	void* __real_realloc(void* p, size_t size);
	void* __real_memalign(size_t align, size_t size);
	void __real_free(void* p);
}

namespace Ps3Mem { void Lock(); void Unlock(); }	// ps3_new.cpp: the game heaps' lock
namespace Ps3Gfx { unsigned int VideoMemoryFree(); }	// the RSX backend's figure

// The boot trail files: the first write of a run truncates each (the game's memory manager
// may write before main(), so main() can't simply delete last run's).
// The first USB drive plugged in (/dev_usb000 ... /dev_usb007), or NULL: the boot trail and
// the log are written there too, so a stick can be checked on a PC without FTP.
extern "C" const char* Ps3UsbRoot()
{
	static char root[16];
	static volatile int state;	// 0 not looked yet, 1 found, 2 none
	if (state == 0)
	{
		state = 2;
		for (int i = 0; i < 8; ++i)
		{
			char path[16] = "/dev_usb000";
			path[10] = (char)('0' + i);
			sysFSStat st;
			if (sysFsStat(path, &st) == 0)
			{
				memcpy(root, path, sizeof(path));
				state = 1;
				break;
			}
		}
	}
	return state == 1 ? root : 0;
}

extern "C" s32 Ps3TrailOpen(int which, s32* fd)
{
	static char usbPath[48];
	static const char* paths[3] = { PS3_USRDIR "/thug-ps3-boot.txt", "/dev_hdd0/tmp/thug-ps3-boot.txt", usbPath };
	static volatile bool started[3];
	if (which < 0 || which > 2) return -1;
	if (which == 2)
	{
		const char* usb = Ps3UsbRoot();
		if (!usb) return -1;
		if (!usbPath[0])
		{
			char* p = usbPath;
			for (const char* s = usb; *s; ) *p++ = *s++;
			for (const char* s = "/thug-ps3-boot.txt"; *s; ) *p++ = *s++;
			*p = 0;
		}
	}
	s32 flags = SYS_O_WRONLY | SYS_O_CREAT | (started[which] ? SYS_O_APPEND : SYS_O_TRUNC);
	s32 r = sysFsOpen(paths[which], flags, fd, NULL, 0);
	if (r == 0 && !started[which])
	{
		started[which] = true;
		sysFsChmod(paths[which], 0100666);	// readable over FTP
	}
	return r;
}

namespace
{
	volatile bool s_noThreads;
	volatile bool s_memCap;
	volatile bool s_startup = true;		// until the game's main loop starts
	volatile bool s_inTrail;			// no recursion if writing the trail allocates
	const size_t kLogSize = 1024 * 1024;
	const size_t kCapSize = 10 * 1024 * 1024;

	char* AppendNum(char* p, unsigned long long v)
	{
		char tmp[24];
		int n = 0;
		do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
		while (n) *p++ = tmp[--n];
		return p;
	}

	char* AppendHex(char* p, unsigned long long v)
	{
		*p++ = '0'; *p++ = 'x';
		bool any = false;
		for (int s = 60; s >= 0; s -= 4)
		{
			int d = (int)((v >> s) & 0xF);
			if (d || any || s == 0) { *p++ = "0123456789abcdef"[d]; any = true; }
		}
		return p;
	}

	char* AppendStr(char* p, const char* s)
	{
		while (*s) *p++ = *s++;
		return p;
	}

	void Trail(const char* text, size_t len)
	{
		for (int i = 0; i < 3; ++i)
		{
			s32 fd;
			if (Ps3TrailOpen(i, &fd) != 0) continue;
			u64 put;
			sysFsWrite(fd, text, len, &put);
			sysLv2FsFsync(fd);
			sysFsClose(fd);
		}
	}

	// "[alloc] malloc 52428800 -> 0x40016770" (or "-> refused (memcap)")
	void LogAlloc(const char* fn, size_t size, void* result, bool refused)
	{
		if (!s_startup || s_inTrail || (size < kLogSize && !refused)) return;
		s_inTrail = true;
		char line[128], *p = line;
		p = AppendStr(p, "[alloc] ");
		p = AppendStr(p, fn);
		*p++ = ' ';
		p = AppendNum(p, size);
		p = AppendStr(p, " -> ");
		p = refused ? AppendStr(p, "refused (memcap)") : result ? AppendHex(p, (unsigned long long)(size_t)result) : AppendStr(p, "NULL");
		*p++ = '\n';
		Trail(line, p - line);
		s_inTrail = false;
	}

	volatile bool s_allocTrace;
	volatile int s_inAllocTrace;	// (process-wide: another thread's line may be skipped; diagnostics only)

	// "[alloc] realloc 0x4080 96 -> 0x40a0" straight to the log, while tracing.
	void TraceAlloc(const char* fn, const void* in, size_t size, const void* out, bool before)
	{
		static int lines;
		if (!s_allocTrace || s_inAllocTrace || lines >= 800) return;
		++lines;
		++s_inAllocTrace;
		char line[160], *p = line;
		p = AppendStr(p, before ? "[alloc] > " : "[alloc] < ");
		p = AppendStr(p, fn);
		if (in) { *p++ = ' '; p = AppendHex(p, (unsigned long long)(size_t)in); }
		if (size != (size_t)-1) { *p++ = ' '; p = AppendNum(p, size); }
		if (!before) { p = AppendStr(p, " -> "); p = AppendHex(p, (unsigned long long)(size_t)out); }
		*p++ = '\n';
		*p = 0;
		Ps3Platform::RawWrite(line);
		--s_inAllocTrace;
	}

	bool Refuse(size_t size)
	{
		return s_memCap && s_startup && size > kCapSize;
	}

	// newlib heap use: bytes in live blocks (malloc_usable_size, so including rounding),
	// its peak, and the number of live blocks. Updated atomically; the peak is approximate
	// when two threads race on it, which is fine for a report.
	volatile long s_liveBytes;
	volatile long s_peakBytes;
	volatile long s_liveBlocks;

	inline void CountAlloc(void* p)
	{
		if (!p) return;
		const long now = __sync_add_and_fetch(&s_liveBytes, (long)malloc_usable_size(p));
		__sync_add_and_fetch(&s_liveBlocks, 1);
		if (now > s_peakBytes) s_peakBytes = now;
	}

	inline void CountFree(void* p)
	{
		if (!p) return;
		__sync_sub_and_fetch(&s_liveBytes, (long)malloc_usable_size(p));
		__sync_sub_and_fetch(&s_liveBlocks, 1);
	}

	// Much of the game still squeezes pointers through int or 32-bit fields (the PC code
	// was 32-bit), which only works while every address is below 0x80000000. The heap is
	// at 0x4xxxxxxx today; if an allocation ever lands higher, say so once and loudly,
	// since the failures it causes show up far from here. No allocation in here.
	volatile bool s_highAddressLogged;
	void CheckHighAddress(const char* fn, const void* p)
	{
		if ((uintptr_t)p < 0x80000000u || s_highAddressLogged) return;
		s_highAddressLogged = true;
		char line[160], *q = line;
		q = AppendStr(q, "[alloc] *** WARNING: ");
		q = AppendStr(q, fn);
		q = AppendStr(q, " returned ");
		q = AppendHex(q, (unsigned long long)(uintptr_t)p);
		q = AppendStr(q, ", at or above 0x80000000: pointers kept in ints or 32-bit fields will break\n");
		*q = 0;
		Ps3Platform::RawWrite(line);
	}
}

extern "C"
{
	void* __wrap_malloc(size_t size)
	{
		if (Refuse(size)) { LogAlloc("malloc", size, 0, true); return 0; }
		TraceAlloc("malloc", 0, size, 0, true);
		void* p = __real_malloc(size);
		CountAlloc(p);
		CheckHighAddress("malloc", p);
		TraceAlloc("malloc", 0, size, p, false);
		LogAlloc("malloc", size, p, false);
		return p;
	}

	void* __wrap_calloc(size_t n, size_t size)
	{
		if (Refuse(n * size)) { LogAlloc("calloc", n * size, 0, true); return 0; }
		TraceAlloc("calloc", 0, n * size, 0, true);
		void* p = __real_calloc(n, size);
		CountAlloc(p);
		CheckHighAddress("calloc", p);
		TraceAlloc("calloc", 0, n * size, p, false);
		LogAlloc("calloc", n * size, p, false);
		return p;
	}

	void* __wrap_realloc(void* old, size_t size)
	{
		if (Refuse(size)) { LogAlloc("realloc", size, 0, true); return 0; }
		TraceAlloc("realloc", old, size, 0, true);
		// The old block's size, read before realloc may free it. A failed realloc keeps the
		// old block, except realloc(p, 0), which may free it and return NULL.
		const long oldBytes = old ? (long)malloc_usable_size(old) : 0;
		void* p = __real_realloc(old, size);
		if (p || (old && size == 0))
		{
			if (old) { __sync_sub_and_fetch(&s_liveBytes, oldBytes); __sync_sub_and_fetch(&s_liveBlocks, 1); }
			CountAlloc(p);
		}
		CheckHighAddress("realloc", p);
		TraceAlloc("realloc", old, size, p, false);
		LogAlloc("realloc", size, p, false);
		return p;
	}

	void* __wrap_memalign(size_t align, size_t size)
	{
		if (Refuse(size)) { LogAlloc("memalign", size, 0, true); return 0; }
		TraceAlloc("memalign", 0, size, 0, true);
		void* p = __real_memalign(align, size);
		CountAlloc(p);
		CheckHighAddress("memalign", p);
		TraceAlloc("memalign", 0, size, p, false);
		LogAlloc("memalign", size, p, false);
		return p;
	}

	void __wrap_free(void* p)
	{
		TraceAlloc("free", p, (size_t)-1, 0, true);
		CountFree(p);
		__real_free(p);
		TraceAlloc("free", p, (size_t)-1, 0, false);
	}
}

namespace Ps3Platform
{

void ReadDiagnosticSwitches()
{
	char path[600];
	snprintf(path, sizeof(path), "%s/nothreads.txt", Ps3Fs::AppDir());
	s_noThreads = Ps3Fs::Exists(path);
	snprintf(path, sizeof(path), "%s/memcap.txt", Ps3Fs::AppDir());
	s_memCap = Ps3Fs::Exists(path);
}

void SetAllocTrace(bool on) { s_allocTrace = on; }

bool NoThreads() { return s_noThreads; }
bool MemoryCap() { return s_memCap; }

void StartupFinished()
{
	if (!s_startup) return;
	s_startup = false;
	LogStartupFinished();
	printf("[diag] start-up finished: big-allocation logging%s off\n", s_memCap ? " and the 10 MB cap" : "");
	MemoryReport("start-up finished");
}

void MemoryReport(const char* why)
{
	// Written in pieces straight to the log (no stdio); the line buffer is on the stack.
	char line[512];
	unsigned freeKB = 0, totalKB = 0;
	MemoryInfo(&freeKB, &totalKB);
	struct mallinfo mi = mallinfo();
	snprintf(line, sizeof(line), "[mem] %s: system %u KB free of %u KB; newlib %ld KB in use in %ld blocks (peak %ld KB), "
		"heap %u KB from the system with %u KB of it free; video memory free (RSX backend's figure) %u KB\n",
		why ? why : "-", freeKB, totalKB, s_liveBytes / 1024, s_liveBlocks, s_peakBytes / 1024,
		(unsigned)(mi.arena / 1024), (unsigned)(mi.fordblks / 1024), Ps3Gfx::VideoMemoryFree() / 1024);
	RawWrite(line);

	// The game's heaps (Mem::Manager): used, free inside the heap (fragments), and the largest
	// block a new allocation could get (a fragment or the region's untouched space).
	Ps3Mem::Lock();
	Mem::Manager& mm = Mem::Manager::sHandle();
	int n = 0;
	char* o = line;
	char* const end = line + sizeof(line);
	o += snprintf(o, end - o, "[mem] heaps (used/fragments/largest KB):");
	for (Mem::Heap* heap = mm.FirstHeap(); heap; heap = mm.NextHeap(heap))
	{
		char item[96];
		const int len = snprintf(item, sizeof(item), " %s %d/%d/%d", heap->GetName() ? heap->GetName() : "?",
			heap->mUsedMem.m_count / 1024, heap->mFreeMem.m_count / 1024, heap->LargestFreeBlock() / 1024);
		if (len <= 0 || len >= (int)sizeof(item)) continue;
		if (o + len + 2 >= end)
		{
			*o++ = '\n'; *o = 0;
			RawWrite(line);
			o = line;
			o += snprintf(o, end - o, "[mem]  ");
		}
		memcpy(o, item, (size_t)len + 1);
		o += len;
		++n;
	}
	Ps3Mem::Unlock();
	if (n == 0) o += snprintf(o, end - o, " none yet");
	*o++ = '\n'; *o = 0;
	RawWrite(line);
}

} // namespace Ps3Platform
