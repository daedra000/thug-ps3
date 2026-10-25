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

#include <ps3_platform.h>
#include <ps3_fs.h>

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <lv2/sysfs.h>
#include <sys/file.h>

extern "C"
{
	void* __real_malloc(size_t size);
	void* __real_calloc(size_t n, size_t size);
	void* __real_realloc(void* p, size_t size);
	void* __real_memalign(size_t align, size_t size);
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
		static const char* paths[2] = { PS3_USRDIR "/thug-ps3-boot.txt", "/dev_hdd0/tmp/thug-ps3-boot.txt" };
		for (int i = 0; i < 2; ++i)
		{
			s32 fd;
			if (sysFsOpen(paths[i], SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, NULL, 0) != 0) continue;
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

	bool Refuse(size_t size)
	{
		return s_memCap && s_startup && size > kCapSize;
	}
}

extern "C"
{
	void* __wrap_malloc(size_t size)
	{
		if (Refuse(size)) { LogAlloc("malloc", size, 0, true); return 0; }
		void* p = __real_malloc(size);
		LogAlloc("malloc", size, p, false);
		return p;
	}

	void* __wrap_calloc(size_t n, size_t size)
	{
		if (Refuse(n * size)) { LogAlloc("calloc", n * size, 0, true); return 0; }
		void* p = __real_calloc(n, size);
		LogAlloc("calloc", n * size, p, false);
		return p;
	}

	void* __wrap_realloc(void* old, size_t size)
	{
		if (Refuse(size)) { LogAlloc("realloc", size, 0, true); return 0; }
		void* p = __real_realloc(old, size);
		LogAlloc("realloc", size, p, false);
		return p;
	}

	void* __wrap_memalign(size_t align, size_t size)
	{
		if (Refuse(size)) { LogAlloc("memalign", size, 0, true); return 0; }
		void* p = __real_memalign(align, size);
		LogAlloc("memalign", size, p, false);
		return p;
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

bool NoThreads() { return s_noThreads; }
bool MemoryCap() { return s_memCap; }

void StartupFinished()
{
	if (!s_startup) return;
	s_startup = false;
	printf("[diag] start-up finished: big-allocation logging%s off\n", s_memCap ? " and the 10 MB cap" : "");
}

} // namespace Ps3Platform
