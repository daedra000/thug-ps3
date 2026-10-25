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

// No SYS_PROCESS_PARAM: builds that set one (priority 1001, 1 MB stack, 1 MB malloc
// pages, as GridSiege does) went back to the XMB before main() on a console, while
// builds with the defaults ran into the game's start-up. Bigger stacks go to threads.

// Boot trail: thug-ps3-boot.txt in USRDIR and in /dev_hdd0/tmp, written with plain lv2
// calls from the first line of main() on, so a crash before the log opens still says
// how far start-up got. (A priority-101 static constructor wrote it from even earlier,
// but builds with it never reached main() on a console: it ran before the C library
// was set up.)
static void BootMarkTo(const char* path, const char* what)
{
	s32 fd;
	if (sysFsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, NULL, 0) != 0)
		return;
	u64 put;
	sysFsWrite(fd, what, strlen(what), &put);
	sysFsWrite(fd, "\n", 1, &put);
	sysLv2FsFsync(fd);	// on disc even if the console is switched off next
	sysFsClose(fd);
	sysFsChmod(path, 0100666);	// readable over FTP
}

static void BootMark(const char* what)
{
	BootMarkTo(PS3_USRDIR "/thug-ps3-boot.txt", what);
	BootMarkTo("/dev_hdd0/tmp/thug-ps3-boot.txt", what);
}


namespace
{
	volatile bool s_quit = false;
	volatile bool s_menuOpen = false;

	// The log: everything printed to stdout/stderr goes to USRDIR/thug-ps3.log
	// and to the TTY (visible with a debug console or ps3load).
	s32 s_logFd = -1;

	int LogWrite(void*, const char* buf, int n)
	{
		u32 written;
		sysTtyWrite(0, buf, n, &written);
		if (s_logFd >= 0)
		{
			u64 put;
			sysFsWrite(s_logFd, buf, n, &put);
			sysLv2FsFsync(s_logFd);	// survive a power-off (a hung headless build is often switched off)
		}
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
		BootMark(s_logFd >= 0 ? "log open" : "log could not be opened");

		FILE* f = funopen(NULL, NULL, LogWrite, NULL, NULL);
		if (f)
		{
			setvbuf(f, NULL, _IONBF, 0);	// every write goes straight to the log (and is fsync'd)
			stdout = f;
			stderr = f;
		}
	}

	volatile u64 s_quitTime;
	volatile const char* s_lastStep = "start";
	volatile bool s_sysThread;

	// Written straight to the log file, bypassing stdio (used from the system thread).
	void RawLog(const char* text)
	{
		u32 written;
		sysTtyWrite(0, text, strlen(text), &written);
		if (s_logFd < 0) return;
		u64 put;
		sysFsWrite(s_logFd, text, strlen(text), &put);
		sysLv2FsFsync(s_logFd);
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
			if (now - lastBeat >= 5000000)
			{
				lastBeat = now;
				char line[256];
				snprintf(line, sizeof(line), "[alive] %u s, last step: %s\n", (unsigned)((now - start) / 1000000), (const char*)s_lastStep);
				RawLog(line);
			}
			if (s_quit && s_quitTime && now - s_quitTime > 3000000)
			{
				RawLog("[quit] the game did not exit within 3 s of Quit Game: exiting now\n");
				sysProcessExit(0);
			}
			usleep(50000);
		}
	}
}

namespace Ps3Platform
{

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
	if (what) s_lastStep = what;
	printf("[step] %s (system memory free: %u KB)\n", what ? what : "?", freeKB);
	fflush(stdout);
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
	sysFsUnlink(PS3_USRDIR "/thug-ps3-boot.txt");
	sysFsUnlink("/dev_hdd0/tmp/thug-ps3-boot.txt");
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
		s_sysThread = sysThreadCreate(&tid, SystemThread, NULL, 1000, 64 * 1024, 0, (char*)"thug-sysutil") == 0;
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

} // namespace Ps3Platform
