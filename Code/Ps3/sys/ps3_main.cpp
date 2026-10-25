// THUG PS3: platform start-up, log and system pump (see ps3_platform.h).

#include <ps3_platform.h>
#include <ps3_fs.h>

#include <stdio.h>
#include <string.h>
#include <lv2/sysfs.h>
#include <sys/tty.h>
#include <sys/file.h>
#include <sys/mutex.h>
#include <sysutil/sysutil.h>
#include <sysmodule/sysmodule.h>
#include <net/net.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <unistd.h>
#include <ppu-lv2.h>
#include <exception>
#include <stdlib.h>
#include <stdarg.h>

namespace Audio { void Shutdown(bool wait); }	// Code/Ps3/audio/ps3_audio.cpp

// No SYS_PROCESS_PARAM: builds that set one (priority 1001, 1 MB stack, 1 MB malloc
// pages, as GridSiege does) went back to the XMB before main() on a console, while
// builds with the defaults ran into the game's start-up. Bigger stacks go to threads.

// Boot trail: thug-ps3-boot.txt in USRDIR and in /dev_hdd0/tmp, written with plain lv2
// calls from the first line of main() on, so a crash before the log opens still says
// how far start-up got. (A priority-101 static constructor wrote it from even earlier,
// but builds with it never reached main() on a console: it ran before the C library
// was set up.)
extern "C" s32 Ps3TrailOpen(int which, s32* fd);	// ps3_diag.cpp: truncates on a run's first write
extern "C" const char* Ps3UsbRoot();				// ps3_diag.cpp: first USB drive, or NULL

static void BootMarkTo(int which, const char* what)
{
	s32 fd;
	if (Ps3TrailOpen(which, &fd) != 0)
		return;
	u64 put;
	sysFsWrite(fd, what, strlen(what), &put);
	sysFsWrite(fd, "\n", 1, &put);
	sysLv2FsFsync(fd);	// on disc even if the console is switched off next
	sysFsClose(fd);
}

static void BootMark(const char* what)
{
	BootMarkTo(0, what);	// USRDIR
	BootMarkTo(1, what);	// /dev_hdd0/tmp
	BootMarkTo(2, what);	// a USB drive, if one is plugged in
}


namespace
{
	volatile bool s_quit = false;
	volatile bool s_menuOpen = false;

	// The log: everything printed to stdout/stderr goes to USRDIR/thug-ps3.log
	// and to the TTY (visible with a debug console or ps3load).
	s32 s_logFd = -1;
	s32 s_usbLogFd = -1;		// a copy of the log on a USB drive, if one is plugged in
	// Every write is fsync'd during start-up (a hung headless build is often switched off);
	// once the game runs, the system thread syncs about once a second instead.
	volatile bool s_syncEveryWrite = true;
	volatile u64 s_syncUntil = 0;		// keep syncing every write until then (sysGetSystemTime)
	volatile bool s_logDirty = false;

	void WriteLogFile(const char* text, size_t n)
	{
		const bool sync = s_syncEveryWrite || sysGetSystemTime() < s_syncUntil;
		u64 put;
		if (s_usbLogFd >= 0)
		{
			sysFsWrite(s_usbLogFd, text, n, &put);
			if (sync) sysLv2FsFsync(s_usbLogFd);
		}
		if (s_logFd < 0) return;
		sysFsWrite(s_logFd, text, n, &put);
		if (sync)
			sysLv2FsFsync(s_logFd);
		else
			s_logDirty = true;
	}
	FILE* s_logFile = NULL;		// the funopen'd log, for AttachThreadLog

	int LogWrite(void*, const char* buf, int n)
	{
		u32 written;
		sysTtyWrite(0, buf, n, &written);
		WriteLogFile(buf, n);
		return n;
	}

	void OpenLog()
	{
		char path[600], old[600];
		snprintf(path, sizeof(path), "%s/thug-ps3.log", Ps3Fs::AppDir());
		snprintf(old, sizeof(old), "%s/thug-ps3.old.log", Ps3Fs::AppDir());
		sysFsUnlink(old);
		sysLv2FsRename(path, old);
		if (sysFsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &s_logFd, NULL, 0) != 0)
		{
			s_logFd = -1;
			BootMark("could not open the log in the app folder; trying USRDIR");
			if (sysFsOpen(PS3_USRDIR "/thug-ps3.log", SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &s_logFd, NULL, 0) != 0)
				s_logFd = -1;
		}
		if (s_logFd >= 0) sysFsChmod(path, 0100666);
		if (s_logFd >= 0) sysFsChmod(PS3_USRDIR "/thug-ps3.log", 0100666);	// the fallback, if used
		BootMark(s_logFd >= 0 ? "log open" : "log could not be opened");

		// A copy on the first USB drive, if any (checked on a PC without FTP).
		if (const char* usb = Ps3UsbRoot())
		{
			char usbPath[64];
			snprintf(usbPath, sizeof(usbPath), "%s/thug-ps3.log", usb);
			if (sysFsOpen(usbPath, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &s_usbLogFd, NULL, 0) != 0)
				s_usbLogFd = -1;
			BootMark(s_usbLogFd >= 0 ? "USB log open" : "USB log could not be opened");
		}

		FILE* f = funopen(NULL, NULL, LogWrite, NULL, NULL);
		if (f)
		{
			setvbuf(f, NULL, _IONBF, 0);	// every write goes straight to the log (and is fsync'd)
			stdout = f;
			stderr = f;
			s_logFile = f;
		}
	}

	volatile u64 s_quitTime;
	volatile const char* s_lastStep = "start";
	char s_stepText[160] = "start";
	char s_traceText[200] = "";
	volatile bool s_sysThread;

	// Written straight to the log file, bypassing stdio (used from the system thread).
	void RawLog(const char* text)
	{
		u32 written;
		sysTtyWrite(0, text, strlen(text), &written);
		WriteLogFile(text, strlen(text));
	}

	void SystemCallback(u64 status, u64, void*)
	{
		switch (status)
		{
		case SYSUTIL_EXIT_GAME:  s_quit = true; s_quitTime = sysGetSystemTime(); break;
		case SYSUTIL_MENU_OPEN:  s_menuOpen = true; break;
		case SYSUTIL_MENU_CLOSE: s_menuOpen = false; break;
		default: break;
		}
	}
}

namespace
{
	// XMB events serviced all the time (the game's loop doesn't run during loads), a
	// heartbeat in the log every 5 s, and a forced exit 3 s after "Quit Game" if the game
	// hasn't gone by then. (Grid Siege runs its system callbacks on a thread too.)
	void SystemThread(void*)
	{
		u64 start = sysGetSystemTime(), lastBeat = start;
		for (;;)
		{
			sysUtilCheckCallback();
			u64 now = sysGetSystemTime();
			if (s_logDirty && s_logFd >= 0)
			{
				s_logDirty = false;
				sysLv2FsFsync(s_logFd);
				if (s_usbLogFd >= 0) sysLv2FsFsync(s_usbLogFd);
			}
			if (now - lastBeat >= 5000000)
			{
				lastBeat = now;
				char line[384];
				if (g_ps3Frame)
					snprintf(line, sizeof(line), "[alive] %u s, main loop frame %u, task %p, at %s, script %s, cfunc %p, member 0x%08x, "
						"interp steps %u in 0x%08x line %u token %u, runscript 0x%08x, last step: %s\n",
						(unsigned)((now - start) / 1000000), g_ps3Frame, (const void*)g_ps3CurrentTask, g_ps3Where ? (const char*)g_ps3Where : "-",
						g_ps3ScriptName ? (const char*)g_ps3ScriptName : "-", (const void*)g_ps3CFunction, g_ps3MemberFunction,
						g_ps3ScriptSteps, g_ps3ScriptSub, g_ps3ScriptLine, g_ps3ScriptToken, g_ps3RunScript, (const char*)s_lastStep);
				else
					snprintf(line, sizeof(line), "[alive] %u s, last step: %s%s%s\n", (unsigned)((now - start) / 1000000), (const char*)s_lastStep,
						s_traceText[0] ? " | last trace: " : "", s_traceText);
				RawLog(line);
			}
			if (s_quit && s_quitTime && now - s_quitTime > 3000000)
			{
				RawLog("[quit] the game did not exit within 3 s of Quit Game: exiting now\n");
				Audio::Shutdown(false);	// stop the port; don't wait for a mixer the game may be blocking
				if (s_logFd >= 0) sysLv2FsFsync(s_logFd);
	if (s_usbLogFd >= 0) sysLv2FsFsync(s_usbLogFd);
				sysProcessExit(0);
			}
			usleep(50000);
		}
	}
}

volatile bool g_ps3TraceTasks = false;
volatile unsigned g_ps3Frame = 0;
volatile const void* g_ps3CurrentTask = 0;
volatile const char* g_ps3Where = 0;
volatile const char* g_ps3ScriptName = 0;
volatile const void* g_ps3CFunction = 0;
volatile unsigned g_ps3MemberFunction = 0;
volatile unsigned g_ps3ScriptSteps = 0;
volatile unsigned g_ps3ScriptSub = 0;
volatile unsigned g_ps3ScriptLine = 0;
volatile unsigned g_ps3ScriptToken = 0;
volatile unsigned g_ps3RunScript = 0;

namespace Ps3Platform
{

// Top of the game thread's stack, for the stack use shown on each [step] line.
static char* volatile s_stackTop = NULL;
static sys_ppu_thread_t s_gameThread;
static const u64 kGameStack = 1024 * 1024;

void RawWrite(const char* text)
{
	RawLog(text);
}

void AttachThreadLog()
{
	if (!s_logFile) return;
	// newlib sets up a thread's stdio on its first use (__sinit), re-initialising the FILEs
	// its stdout/stderr point at. Make that happen to this thread's own FILEs first, or it
	// would clobber the shared log FILE.
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	stdout = s_logFile;
	stderr = s_logFile;
}

void Trace(const char* fmt, ...)
{
	char text[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	// Read by the system thread without a lock: write the text, then its terminator.
	size_t len = strlen(text);
	s_traceText[0] = 0;
	memcpy(s_traceText + 1, text + 1, len);	// includes the NUL; text[0] last
	s_traceText[0] = text[0];
	char line[210];
	snprintf(line, sizeof(line), "%s\n", text);
	RawLog(line);
}

void MemoryInfo(unsigned* freeKB, unsigned* totalKB)
{
	struct { u32 total, available; } info = { 0, 0 };
	lv2syscall1(352, (u64)&info);	// sys_memory_get_user_memory_size
	if (freeKB) *freeKB = info.available / 1024;
	if (totalKB) *totalKB = info.total / 1024;
}

void Step(const char* what)
{
	unsigned freeKB = 0;
	MemoryInfo(&freeKB, NULL);
	if (what)
	{
		snprintf(s_stepText, sizeof(s_stepText), "%s", what);
		s_lastStep = s_stepText;
	}
	unsigned stackKB = 0;
	sys_ppu_thread_t self = 0;
	sysThreadGetId(&self);
	if (s_stackTop && self == s_gameThread) stackKB = (unsigned)((s_stackTop - (char*)__builtin_frame_address(0)) / 1024);
	// Straight to the log, not printf: it works on any thread, whatever stdio is doing.
	char line[300];
	snprintf(line, sizeof(line), "[step] %s (system memory free: %u KB, stack used: %u KB of %u KB)\n", what ? what : "?", freeKB, stackKB, (unsigned)(kGameStack / 1024));
	RawLog(line);
}

static void OnTerminate()
{
	// An exception nobody caught (std::bad_alloc from newlib's heap, say): say which.
	const char* what = "unknown";
	try { if (std::current_exception()) std::rethrow_exception(std::current_exception()); }
	catch (const std::exception& e) { what = e.what(); }
	catch (...) {}
	printf("[fatal] uncaught C++ exception: %s\n", what);
	BootMark("fatal: uncaught C++ exception");
	BootMark(what);
	fflush(stdout);
	abort();
}

void Init(int argc, char** argv)
{
	// Unbuffered from the first line: nothing is lost if the next instruction crashes.
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	BootMark("MAIN STARTED");
	fprintf(stderr, "MAIN STARTED\n");
	std::set_terminate(OnTerminate);
	if (argc > 0 && argv && argv[0]) BootMark(argv[0]);
	if (argc > 0)
		Ps3Fs::SetAppPath(argv[0]);
	OpenLog();
	printf("MAIN STARTED\n");
	printf("THUG PS3 (%s build), running from %s\n", PS3_HEADLESS ? "headless" : "normal", Ps3Fs::AppDir());
	ReadDiagnosticSwitches();
	if (NoThreads()) printf("[diag] nothreads.txt: no threads of our own; the game's CreateThread calls are refused\n");
	if (MemoryCap()) printf("[diag] memcap.txt: allocations over 10 MB fail during start-up\n");
	{
		unsigned freeKB, totalKB;
		MemoryInfo(&freeKB, &totalKB);
		printf("[mem] system memory: %u KB free of %u KB\n", freeKB, totalKB);
	}

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, SystemCallback, NULL);
	if (!NoThreads())
	{
		sys_ppu_thread_t tid;
		// Above the game thread (lower number = higher priority), so XMB events and the forced
		// quit still run while the game busy-waits.
		s_sysThread = sysThreadCreate(&tid, SystemThread, NULL, 500, 64 * 1024, 0, (char*)"thug-sysutil") == 0;
		printf("[sys] system thread %s\n", s_sysThread ? "running" : "could not start");
	}

	if (sysModuleLoad(SYSMODULE_NET) == 0 && netInitialize() == 0)
		printf("[net] initialised\n");
	else
		printf("[net] not available\n");

	char data[600];
	snprintf(data, sizeof(data), "%s/Data", Ps3Fs::AppDir());
	bool isDir = false;
	if (!Ps3Fs::Exists(data, &isDir) || !isDir)
		printf("[data] %s is missing: copy the Data folder of the PC game there\n", data);
}

void Pump()
{
	if (!s_sysThread) sysUtilCheckCallback();	// otherwise the system thread does it
}

bool QuitRequested()
{
	return s_quit;
}

bool SystemMenuOpen()
{
	return s_menuOpen;
}

void FlushLog()
{
	fflush(stdout);
}

bool OnGameThread()
{
	if (!s_gameThread) return true;
	sys_ppu_thread_t self = 0;
	sysThreadGetId(&self);
	return self == s_gameThread;
}

void LogStartupFinished()
{
	// Keep every line on disc for the main loop's first 30 s too: a freeze or power-off
	// early on would otherwise lose the last second of the log.
	s_syncUntil = sysGetSystemTime() + 30 * 1000000ull;
	s_syncEveryWrite = false;
}

void ExitGame(int code)
{
	printf("[quit] exiting (code %d)\n", code);
	Audio::Shutdown(true);
	fflush(stdout);
	if (s_logFd >= 0) sysLv2FsFsync(s_logFd);
	if (s_usbLogFd >= 0) sysLv2FsFsync(s_usbLogFd);
	sysProcessExit(code);
}

} // namespace Ps3Platform

// The default main thread stack is 64 KB, and builds that asked for a bigger one with
// SYS_PROCESS_PARAM never reached main(). So main() only opens the log and runs the game
// (GameMain, in Code/Sk/Main.cpp) on a thread with a 1 MB stack, then waits for it.
int GameMain(int argc, char** argv);

namespace
{
	struct GameArgs { int argc; char** argv; int result; };

	void GameThread(void* p)
	{
		GameArgs* a = (GameArgs*)p;
		Ps3Platform::s_stackTop = (char*)__builtin_frame_address(0);
		sysThreadGetId(&Ps3Platform::s_gameThread);
		Ps3Platform::AttachThreadLog();
		Ps3Platform::Step("game thread started");
		a->result = GameMain(a->argc, a->argv);
		// The game's own shutdown has run; leave with the mixer stopped.
		Ps3Platform::ExitGame(a->result);
		sysThreadExit(0);
	}
}

int main(int argc, char** argv)
{
	Ps3Platform::Init(argc, argv);
	static GameArgs args;
	args.argc = argc; args.argv = argv; args.result = 0;
	sys_ppu_thread_t id;
	char name[] = "thug_game";
	s32 err = sysThreadCreate(&id, GameThread, &args, 1000, Ps3Platform::kGameStack, THREAD_JOINABLE, name);
	if (err != 0)
	{
		// Not on the 64 KB main thread stack: it overflows.
		printf("[sys] couldn't start the game thread (error %d)\n", (int)err);
		BootMark("game thread failed");
		Ps3Platform::ExitGame(1);
	}
	u64 ret = 0;
	sysThreadJoin(id, &ret);
	fflush(stdout);
	return args.result;
}
