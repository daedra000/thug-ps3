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
			s_logFd = -1;

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
