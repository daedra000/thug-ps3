// THUG PS3: platform start-up and the per-frame system pump.
#ifndef PS3_PLATFORM_H
#define PS3_PLATFORM_H

#ifndef PS3_HEADLESS
#define PS3_HEADLESS 0
#endif

namespace Ps3Platform
{
	// Called first thing in main(): app path, log file (USRDIR/thug-ps3.log, also
	// mirrored to the TTY), system modules, network, XMB callbacks.
	void Init(int argc, char** argv);

	// Services the PS3 system (XMB menu, quit requests). Call once per frame.
	void Pump();

	// The user chose "Quit Game" from the XMB.
	bool QuitRequested();

	// The XMB in-game menu is open (the game should pause).
	bool SystemMenuOpen();

	// Flush the log (before a likely crash, or at exit).
	void FlushLog();

	// A start-up step for the log (with free system memory), written straight to disc.
	void Step(const char* what);

	// Free / total user memory in KB (lv2), for the log.
	void MemoryInfo(unsigned* freeKB, unsigned* totalKB);

	// Diagnostic switches (empty files in USRDIR, read at the top of main(); ps3_diag.cpp):
	// nothreads.txt - none of our threads, and the game's CreateThread calls refused;
	// memcap.txt - allocations over 10 MB fail during start-up. Allocations of 1 MB or
	// more are written to the boot trail during start-up either way.
	void ReadDiagnosticSwitches();
	bool NoThreads();
	bool MemoryCap();
	void StartupFinished();		// the game's main loop is running
}

#endif // PS3_PLATFORM_H
