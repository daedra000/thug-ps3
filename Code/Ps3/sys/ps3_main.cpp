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

// Main thread: priority 1001, 1 MB stack (lv2's default is 64 KB; the game was built for
// Windows' 1 MB). Same as the GridSiege ports.
SYS_PROCESS_PARAM(1001, 0x100000);

// Boot trail: USRDIR/thug-ps3-boot.txt, written with plain lv2 calls from the first static
// constructor on, so a crash before the log opens still says how far start-up got.
static void BootMark(const char* what)
{
	s32 fd;
	if (sysFsOpen(PS3_USRDIR "/thug-ps3-boot.txt", SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, NULL, 0) != 0)
		return;
	u64 put;
	sysFsWrite(fd, what, strlen(what), &put);
	sysFsWrite(fd, "\n", 1, &put);
	sysFsClose(fd);
	sysFsChmod(PS3_USRDIR "/thug-ps3-boot.txt", 0100666);	// readable over FTP
}

__attribute__((constructor(101))) static void BootFirstConstructor()
{
	sysFsUnlink(PS3_USRDIR "/thug-ps3-boot.txt");
	BootMark("static constructors started");
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
			setvbuf(f, NULL, _IOLBF, 1024);
			stdout = f;
			stderr = f;
		}
	}

	void SystemCallback(u64 status, u64, void*)
	{
		switch (status)
		{
		case SYSUTIL_EXIT_GAME:  s_quit = true; break;
		case SYSUTIL_MENU_OPEN:  s_menuOpen = true; break;
		case SYSUTIL_MENU_CLOSE: s_menuOpen = false; break;
		default: break;
		}
	}
}

namespace Ps3Platform
{

void Init(int argc, char** argv)
{
	BootMark("main() reached");
	if (argc > 0 && argv && argv[0]) BootMark(argv[0]);
	if (argc > 0)
		Ps3Fs::SetAppPath(argv[0]);
	OpenLog();
	printf("THUG PS3 (%s build), running from %s\n", PS3_HEADLESS ? "headless" : "normal", Ps3Fs::AppDir());

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, SystemCallback, NULL);

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
	sysUtilCheckCallback();
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
