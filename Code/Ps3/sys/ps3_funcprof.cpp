// THUG PS3: a function-level CPU profile for the profile build (make PROFILE=1).
//
// The renderer's files are compiled with -finstrument-functions there, so every call into
// one of their functions calls __cyg_profile_func_enter and every return __cyg_profile_func_exit.
// On the game thread these time each function (PPU timebase) and keep, per function, the calls,
// the inclusive time and the self time (inclusive less the instrumented functions it called).
// About every 600 frames the log gets the 40 functions with the most self time a frame:
//
//   [fprof] 600 frames, ms a frame (self / with callees / calls a frame):
//   [fprof]   1.23 / 4.56 / 1012  Ps3Gfx::DoDraw(...)
//
// Names come from USRDIR/thug-ps3.sym, the ELF's symbol table, which the profile build puts in
// the PKG. The probes cost time themselves (a few tens of nanoseconds each), most visible in
// small functions called very often, so the profile ranks the work; it doesn't time it exactly.
// Nothing in here is instrumented, and it only runs in the profile build.
#if PS3_FUNCPROF
#include <ps3_funcprof.h>
#include <ps3_fs.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define NOPROF __attribute__((no_instrument_function))

extern "C" void __cyg_profile_func_enter(void* fn, void* site) NOPROF;
extern "C" void __cyg_profile_func_exit(void* fn, void* site) NOPROF;

namespace
{
	struct FnStat
	{
		const void* fn;
		unsigned long long calls, incl, self;
	};
	enum { kSlots = 8192, kDepth = 512, kTop = 40 };
	FnStat s_stats[kSlots];
	unsigned s_used;
	bool s_full;

	struct Frame
	{
		FnStat* stat;
		const void* fn;
		unsigned long long start, child;
	};
	Frame s_stack[kDepth];
	int s_depth;

	unsigned long long s_threadPtr;	// r13 of the game thread (the profile covers only it)
	bool s_on;
	unsigned s_frames;
	unsigned long long s_lost;		// exits that didn't match the stack (longjmp, other threads' mix-ups)

	inline unsigned long long NOPROF Tb() { return __builtin_ppc_get_timebase(); }
	inline unsigned long long NOPROF ThreadPtr() { unsigned long long r; __asm__ volatile("mr %0,13" : "=r"(r)); return r; }

	FnStat* NOPROF Find(const void* fn)
	{
		unsigned long long h = (unsigned long long)(uintptr_t)fn;
		h = (h >> 2) * 0x9E3779B97F4A7C15ull;
		unsigned i = (unsigned)(h >> 51) & (kSlots - 1);
		for (unsigned n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1))
		{
			FnStat& s = s_stats[i];
			if (s.fn == fn) return &s;
			if (!s.fn)
			{
				if (s_used >= kSlots - 64) { s_full = true; return NULL; }
				s.fn = fn;
				++s_used;
				return &s;
			}
		}
		return NULL;
	}
}

extern "C" void __cyg_profile_func_enter(void* fn, void*)
{
	if (!s_on || ThreadPtr() != s_threadPtr) return;
	if (s_depth >= kDepth) { ++s_depth; return; }
	Frame& f = s_stack[s_depth++];
	f.fn = fn;
	f.stat = Find(fn);
	f.child = 0;
	f.start = Tb();
}

extern "C" void __cyg_profile_func_exit(void* fn, void*)
{
	if (!s_on || ThreadPtr() != s_threadPtr) return;
	const unsigned long long now = Tb();
	if (s_depth > kDepth) { --s_depth; return; }
	if (s_depth <= 0) { ++s_lost; return; }
	// Normally the top of the stack; a few levels down if frames were skipped (a longjmp).
	int d = s_depth - 1;
	for (int k = 0; d >= 0 && k < 4 && s_stack[d].fn != fn; ++k) --d;
	if (d < 0 || s_stack[d].fn != fn) { ++s_lost; return; }
	if (d != s_depth - 1) s_lost += s_depth - 1 - d;
	Frame& f = s_stack[d];
	const unsigned long long incl = now - f.start;
	if (f.stat)
	{
		++f.stat->calls;
		f.stat->incl += incl;
		f.stat->self += incl > f.child ? incl - f.child : 0;
	}
	s_depth = d;
	if (d > 0) s_stack[d - 1].child += incl;
}

namespace Ps3FuncProf
{
	void NOPROF Start()
	{
		s_threadPtr = ThreadPtr();
		s_on = true;
		printf("[fprof] function profile on (game thread)\n");
	}

	// Names for the given function descriptors from thug-ps3.sym ("address type name" per line,
	// as ppu-nm writes it); found ones are copied into names[i] (cap bytes each).
	static void NOPROF Names(const void* const* fns, int n, char (*names)[160])
	{
		for (int i = 0; i < n; ++i) snprintf(names[i], 160, "0x%08llx", (unsigned long long)(uintptr_t)fns[i]);
		char path[600];
		snprintf(path, sizeof(path), "%s/thug-ps3.sym", Ps3Fs::AppDir());
		FILE* f = fopen(path, "r");
		if (!f) return;
		char line[1024];
		while (fgets(line, sizeof(line), f))
		{
			char* end = NULL;
			const unsigned long long a = strtoull(line, &end, 16);
			if (end == line) continue;
			for (int i = 0; i < n; ++i)
			{
				if ((unsigned long long)(uintptr_t)fns[i] != a) continue;
				// "<address> <type> <name>"
				const char* p = end;
				while (*p == ' ') ++p;
				if (*p) ++p;	// the type letter
				while (*p == ' ') ++p;
				size_t len = strcspn(p, "\r\n");
				if (len > 159) len = 159;
				memcpy(names[i], p, len);
				names[i][len] = 0;
			}
		}
		fclose(f);
	}

	void NOPROF FrameEnd()
	{
		if (!s_on || ++s_frames < 600) return;
		s_on = false;	// the report itself isn't profiled

		// The kTop functions with the most self time.
		const FnStat* top[kTop];
		int n = 0;
		for (unsigned i = 0; i < kSlots; ++i)
		{
			const FnStat* s = &s_stats[i];
			if (!s->fn || !s->calls) continue;
			if (n < kTop) { top[n++] = s; }
			else if (s->self > top[n - 1]->self) top[n - 1] = s;
			else continue;
			for (int k = n - 1; k > 0 && top[k]->self > top[k - 1]->self; --k) { const FnStat* t = top[k]; top[k] = top[k - 1]; top[k - 1] = t; }
		}
		const void* fns[kTop];
		for (int i = 0; i < n; ++i) fns[i] = top[i]->fn;
		static char names[kTop][160];
		Names(fns, n, names);

		const unsigned long long perMs = 79800, frames = s_frames;
		printf("[fprof] %u frames, ms a frame (self / with callees / calls a frame)%s; %llu unmatched exits:\n",
			s_frames, s_full ? " (table full: some functions left out)" : "", s_lost);
		for (int i = 0; i < n; ++i)
		{
			const FnStat* s = top[i];
			const unsigned long long self = s->self * 100 / frames / perMs, incl = s->incl * 100 / frames / perMs;
			printf("[fprof] %3u.%02u / %3u.%02u / %6llu  %s\n", (unsigned)(self / 100), (unsigned)(self % 100),
				(unsigned)(incl / 100), (unsigned)(incl % 100), s->calls / frames, names[i]);
		}
		fflush(stdout);

		for (unsigned i = 0; i < kSlots; ++i) { s_stats[i].calls = s_stats[i].incl = s_stats[i].self = 0; }
		s_frames = 0;
		s_lost = 0;
		// Frames still on the stack keep timing; what they called before is dropped with the reset.
		for (int d = 0; d < s_depth && d < kDepth; ++d) s_stack[d].child = 0;
		s_on = true;
	}
}
#endif
