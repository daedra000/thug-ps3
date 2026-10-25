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

	// newlib gives every thread its own stdout/stderr: call at the start of each new thread
	// so its printf output goes to the log too.
	void AttachThreadLog();

	// Called when the game's main loop starts: stop fsyncing every log write.
	void LogStartupFinished();

	// True on the game thread (and on the main thread before the game thread starts).
	bool OnGameThread();

	// Stop audio, flush the log and leave (the game's own teardown isn't needed on PS3).
	void ExitGame(int code);

	// A line written straight to the log from any thread (no stdio); the latest one is also
	// shown on the [alive] heartbeat lines.
	void Trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

	// Text straight to the log; never allocates (usable inside the malloc wrappers).
	void RawWrite(const char* text);

	// Log every malloc/calloc/realloc/memalign/free while on (ps3_diag.cpp).
	void SetAllocTrace(bool on);
}

#endif // PS3_PLATFORM_H
