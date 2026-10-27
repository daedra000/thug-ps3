// THUG PS3: platform start-up and the per-frame system pump.
#ifndef PS3_PLATFORM_H
#define PS3_PLATFORM_H

#ifndef PS3_HEADLESS
#define PS3_HEADLESS 0
#endif

// Set by the main loop for its first frames: the task lists log each task they run.
extern volatile bool g_ps3TraceTasks;
// Main loop progress, shown on the [alive] lines: the frame number and the task running now.
extern volatile unsigned g_ps3Frame;
extern volatile unsigned long long g_ps3FrameTime;	// sysGetSystemTime() when g_ps3Frame last changed
extern volatile const void* g_ps3CurrentTask;
// Where the game thread is (a label set along the main loop's path) and the spawned script
// being updated, for the [alive] lines.
extern volatile const char* g_ps3Where;
extern volatile const char* g_ps3ScriptName;
// The script C function (its descriptor: name it with thug-ps3.map) or member function
// (checksum) running now, 0 when none.
extern volatile const void* g_ps3CFunction;
extern volatile unsigned g_ps3MemberFunction;
// The script interpreter's position: steps executed (all scripts), the script whose code is
// running (a called script's checksum, not the spawned one's), its last line number and token.
extern volatile unsigned g_ps3ScriptSteps;
extern volatile unsigned g_ps3ScriptSub;
extern volatile unsigned g_ps3ScriptLine;
extern volatile unsigned g_ps3ScriptToken;
// The script being run to completion by RunScript (checksum), 0 when none.
extern volatile unsigned g_ps3RunScript;
#define PS3_WHERE(label) (g_ps3Where = (label))

namespace Ps3Platform
{
	// The hitch sampler: what the game thread was running (task, C function, script, member
	// function, main loop label) during main loop frame `frame`, sampled every millisecond,
	// the most frequent first, to the log. For [hitch] lines.
	void LogHitchSamples(unsigned frame);

	// Called first thing in main(): app path, log file (USRDIR/thug-ps3.log, also
	// mirrored to the TTY), system modules, network, XMB callbacks.
	void Init(int argc, char** argv);

	// Services the PS3 system (XMB menu, quit requests). Call once per frame.
	void Pump();

	// The user chose "Quit Game" from the XMB (or the game's own Quit, see RequestQuit).
	bool QuitRequested();

	// The game's Quit menu: leave as for the XMB's Quit Game (the main loop ends, the
	// game shuts down and returns to the XMB; forced after 3 s if it doesn't).
	void RequestQuit();

	// The XMB in-game menu is open (the game should pause).
	bool SystemMenuOpen();

	// Flush the log (before a likely crash, or at exit).
	void FlushLog();

	// A start-up step for the log (with free system memory), written straight to disc.
	void Step(const char* what);

	// Free / total user memory in KB (lv2), for the log.
	void MemoryInfo(unsigned* freeKB, unsigned* totalKB);

	// The memory budget in the log ([mem] lines): system memory, newlib's heap (bytes in use,
	// counted by the malloc wrappers), each game heap's free space and largest free block, and
	// what the RSX backend reports for video memory (ps3_diag.cpp). Step() calls it at most
	// every 2 s once the game is running (level loads are full of steps), and the main loop
	// after a frame over a second.
	void MemoryReport(const char* why);

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
	// Trace, then the log synced to disc: for the steps just before a suspected crash
	// (a crash or power-off can otherwise lose the last 2 s of the log).
	void Mark(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	// One line straight to the log file and TTY (no stdio: safe from the crash handler, while
	// the stopped thread may hold stdio's lock), then synced to disc.
	void LogNow(const char* text);
	// Where the game is, for the crash handler: last step, main loop label, script, C function.
	void LogWhere(const char* prefix);
	// The PPU exception handler (Code/Ps3/sys/ps3_crash.cpp).
	void InstallCrashHandler();
	extern char* volatile s_stackTop;		// the game thread's stack top (crash backtraces)

	// Text straight to the log; never allocates (usable inside the malloc wrappers).
	void RawWrite(const char* text);

	// Log every malloc/calloc/realloc/memalign/free while on (ps3_diag.cpp).
	void SetAllocTrace(bool on);
}

#endif // PS3_PLATFORM_H
