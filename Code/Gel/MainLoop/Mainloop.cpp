/*****************************************************************************
**																			**
**			              Neversoft Entertainment			                **
**																		   	**
**				   Copyright (C) 1999 - All Rights Reserved				   	**
**																			**
******************************************************************************
**																			**
**	Project:		GEL (Game Engine Library)								**
**																			**
**	Module:			Main Loop (ML) 											**
**																			**
**	File name:		mainloop.cpp											**
**																			**
**	Created:		05/27/99	-	mjb										**
**																			**
**	Description:	Main loop and support code								**
**																			**
*****************************************************************************/


/*****************************************************************************
**							  	  Includes									**
*****************************************************************************/

#include <gel/mainloop.h>

#include <core/defines.h>
#include <core/singleton.h>
#include <core/task.h>

#include <gfx/nx.h>

#ifndef __PLAT_WN32__
    #include <sys/profiler.h>
#else
	#include <Core/Defines_IDA.h>
#endif

#include <gel/scripting/symboltable.h>

#ifdef	__PLAT_NGPS__
    #include <gel/collision/batchtricoll.h>
    #include <gel/soundfx/ngps/p_sfx.h>

    namespace NxPs2
    {
        void	WaitForRendering();
		void 	StuffAfterGSFinished();
    }
#endif

/*****************************************************************************
**								DBG Information								**
*****************************************************************************/

Dbg_DefineProject( GEL, "GEL Library" )

// lwss add
#ifdef __PLAT_WN32__
void Win32_ApplyInputs();
#ifdef __PLAT_PS3__
#include <ps3_platform.h>
#include <sys/systime.h>
#include <gel/scripting/script.h>
#include <gfx/DX9/NX/nx_init.h>
#include <sk/modules/FrontEnd/FrontEnd.h>
#include <string.h>
#endif
#ifdef __PLAT_PS3__
// Main loop progress for the log: the frame number, and each phase of the first two frames.
static unsigned s_ps3Frame = 0;
unsigned long long g_ps3FrameGateTicks;	// added to by the renderer (d3d9_rsx.cpp): flip and RSX waits at frame starts
// This frame's CPU time in the world render's stages (p_nx.cpp, 0-8) and the draw calls (9,
// d3d9_rsx_draw.cpp), for the profile of the frames over 20 ms.
enum { kStages = 10 };
unsigned long long g_ps3StageTicks[kStages];
static const char* const s_stageNames[kStages] = { "shadows", "opaque world", "splats", "opaque instances", "sky",
	"semi instances pre", "semi world", "particles", "semi instances post", "of which draw calls" };
volatile unsigned g_ps3FlipsQueued;	// counted by the renderer (d3d9_rsx.cpp): flips queued, for the frame pacing
static unsigned long long s_profGateLast;
#define PS3_PHASE(name) do { if (s_ps3Frame <= 2) Ps3Platform::Step(name); } while (0)

// Frame profiler: each main loop frame is split into phases, timed with the PPU timebase (a
// register read, not a system call), and a summary goes to the log about every 10 s. Nothing
// is allocated per frame.
namespace
{
	enum { kProfWait, kProfLogic, kProfPre, kProfRender, kProfPresent, kProfOther, kProfPhases };
	const char* const s_profNames[kProfPhases] = { "wait", "logic", "pre", "render", "present", "other" };

	struct FrameProfile
	{
		u64 tbPerMs;				// timebase ticks per millisecond (79800 on a PS3)
		u64 windowStart;
		u64 sum[kProfPhases];		// totals over this window
		u64 frames, skipped, slow, longFrames;
		u64 worstTotal;
		u64 worst[kProfPhases];		// the slowest frame's phases
		unsigned worstFrame;
		u64 slowSum[kProfPhases];	// totals over the frames over 20 ms
		u64 gate, slowGate;			// of render and present: waiting for a flip or the RSX (FrameBegin)
		u64 stage[kStages], slowStage[kStages];	// world render stages (g_ps3StageTicks)
	};
	FrameProfile s_prof;

	inline u64 ProfNow()
	{
		return __builtin_ppc_get_timebase();
	}

	// Ticks as "12.34" (milliseconds, two decimals) at p; returns the new end of the text.
	char* ProfMs(char* p, char* end, u64 ticks)
	{
		const u64 us = s_prof.tbPerMs ? ticks * 1000 / s_prof.tbPerMs : 0;
		int n = snprintf(p, end - p, "%u.%02u", (unsigned)(us / 1000), (unsigned)(us % 1000 / 10));
		return (n > 0 && n < end - p) ? p + n : end - 1;
	}

	// One frame's phase times: added to the window, and the summary written once it is 10 s old.
	void ProfCommit(const u64 ph[kProfPhases], bool skipped, unsigned frame, u64 now, u64 gate)
	{
		FrameProfile& p = s_prof;
		if (!p.tbPerMs)
		{
			u64 f = sysGetTimebaseFrequency();
			if (f < 1000000 || f > 1000000000) f = 79800000;
			p.tbPerMs = f / 1000;
			p.windowStart = now;
		}
		u64 total = 0;
		for (int i = 0; i < kProfPhases; ++i) total += ph[i];
		// A frame over half a second is a load or a movie: counted, but kept out of the averages.
		if (total > 500 * p.tbPerMs)
			++p.longFrames;
		else
		{
			for (int i = 0; i < kProfPhases; ++i) p.sum[i] += ph[i];
			++p.frames;
			if (skipped) ++p.skipped;
			p.gate += gate;
			for (int i = 0; i < kStages; ++i) p.stage[i] += g_ps3StageTicks[i];
			if (total > 20 * p.tbPerMs)
			{
				++p.slow;
				for (int i = 0; i < kProfPhases; ++i) p.slowSum[i] += ph[i];
				p.slowGate += gate;
				for (int i = 0; i < kStages; ++i) p.slowStage[i] += g_ps3StageTicks[i];
			}
			if (total > p.worstTotal)
			{
				p.worstTotal = total;
				p.worstFrame = frame;
				for (int i = 0; i < kProfPhases; ++i) p.worst[i] = ph[i];
			}
		}
		if (now - p.windowStart < 10000 * p.tbPerMs)
			return;

		char line[1800], *o = line, *end = line + sizeof(line);
		o += snprintf(o, end - o, "[prof] %u frames (%u not drawn, %u over 20 ms, %u loads left out) avg ms:",
			(unsigned)p.frames, (unsigned)p.skipped, (unsigned)p.slow, (unsigned)p.longFrames);
		u64 avgTotal = 0;
		for (int i = 0; i < kProfPhases && o < end - 32; ++i)
		{
			const u64 avg = p.frames ? p.sum[i] / p.frames : 0;
			avgTotal += avg;
			o += snprintf(o, end - o, "%s %s ", i ? "," : "", s_profNames[i]);
			o = ProfMs(o, end, avg);
		}
		if (o < end - 32) { o += snprintf(o, end - o, " = "); o = ProfMs(o, end, avgTotal); }
		if (o < end - 64) { o += snprintf(o, end - o, " (waiting for a flip or the RSX "); o = ProfMs(o, end, p.frames ? p.gate / p.frames : 0); o += snprintf(o, end - o, " of it)"); }
		if (p.slow && o < end - 64)
		{
			o += snprintf(o, end - o, "; frames over 20 ms avg:");
			for (int i = 0; i < kProfPhases && o < end - 32; ++i)
			{
				o += snprintf(o, end - o, "%s %s ", i ? "," : "", s_profNames[i]);
				o = ProfMs(o, end, p.slowSum[i] / p.slow);
			}
			if (o < end - 40) { o += snprintf(o, end - o, " (flip/RSX wait "); o = ProfMs(o, end, p.slowGate / p.slow); o += snprintf(o, end - o, ")"); }
		}
		// The world render's stages: every frame's average, then the slow frames'.
		if (o < end - 48) o += snprintf(o, end - o, "; world render avg (slow frames):");
		for (int i = 0; i < kStages && o < end - 64; ++i)
		{
			o += snprintf(o, end - o, "%s %s ", i ? "," : "", s_stageNames[i]);
			o = ProfMs(o, end, p.frames ? p.stage[i] / p.frames : 0);
			o += snprintf(o, end - o, " (");
			o = ProfMs(o, end, p.slow ? p.slowStage[i] / p.slow : 0);
			o += snprintf(o, end - o, ")");
		}
		if (o < end - 48) { o += snprintf(o, end - o, "; worst frame %u: ", p.worstFrame); o = ProfMs(o, end, p.worstTotal); o += snprintf(o, end - o, " ms ("); }
		for (int i = 0; i < kProfPhases && o < end - 32; ++i)
		{
			o += snprintf(o, end - o, "%s%s ", i ? ", " : "", s_profNames[i]);
			o = ProfMs(o, end, p.worst[i]);
		}
		if (o < end - 3) { *o++ = ')'; *o++ = '\n'; *o = 0; }
		else { line[sizeof(line) - 2] = '\n'; line[sizeof(line) - 1] = 0; }
		Ps3Platform::RawWrite(line);

		const u64 tbPerMs = p.tbPerMs;
		memset(&p, 0, sizeof(p));
		p.tbPerMs = tbPerMs;
		p.windowStart = now;
	}
}
// Time since the last mark goes to the given phase.
#define PS3_PROF_MARK(phase) do { const u64 n_ = ProfNow(); profPhase[phase] += n_ - profLast; profLast = n_; } while (0)
#else
#define PS3_PHASE(name) do { } while (0)
#endif
void Win32_ProcessMsgPump();
void Win32_GameNetUpdate();

void D3D_ResetTextures();
void D3D_ResetBuffers();
void D3D_ReleaseRenderSurfaces();

void D3D_ReCreateTextures();
void D3D_ReCreateBuffers();
void D3D_RecreateRenderSurfaces();
#endif
// lwss end

namespace Mdl
{
	void	Rail_DebugRender();			// for debugging
}

namespace Mlp
{




/*****************************************************************************
**								  Externals									**
*****************************************************************************/


/*****************************************************************************
**								   Defines									**
*****************************************************************************/


/*****************************************************************************
**								Private Types								**
*****************************************************************************/


/*****************************************************************************
**								 Private Data								**
*****************************************************************************/

DefineSingletonClass( Manager, "Main Loop Manager" );

/*****************************************************************************
**								 Public Data								**
*****************************************************************************/


/*****************************************************************************
**							  Private Prototypes							**
*****************************************************************************/


/*****************************************************************************
**							   Private Functions							**
*****************************************************************************/

Manager::Manager( void )
{
	
	
	start_render_hook = NULL;
	end_render_hook = NULL;
	done = false;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

Manager::~Manager( void )
{
	
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

inline void		Manager::service_system( void )
{
	

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PushContext( 255, 0, 0 );
#	endif // __USE_PROFILER__
#endif

	system_task_stack.Process (currently_profiling);

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
#	endif // __USE_PROFILER__
#endif
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

inline void		Manager::game_logic( void )
{
	
	
#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PushContext( 0, 255, 0 );
#	endif // __USE_PROFILER__
#endif

//	printf ("\nTiming Logic\n"); 
	logic_task_stack.Process(currently_profiling);

//	printf ("\nDumping Task List\n\n");	
//	logic_task_stack.Dump();

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
#	endif // __USE_PROFILER__
#endif
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

inline	void		Manager::render_frame( void )
{
	
	
//	printf ("############################## render_frame #############################\n");	
	

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PushContext( 0, 0, 255 );
#	endif // __USE_PROFILER__
#endif

#	ifdef __PLAT_NGC__
	if( start_render_hook )
	{
		start_render_hook->Call();
	}
	if( !display_tasks_paused )
	{
		display_task_stack.Process( currently_profiling );
	}
#	else
	if (!display_tasks_paused)
	{
		// If paused don't call the start_render_hook
		// as this clears the none-visible frame buffer
		// which will result in a flash when display is unpaused
		if ( start_render_hook )			// set up for rendering
		{
			start_render_hook->Call();
		}

//		printf ("\nTiming render\n"); 
	
		display_task_stack.Process(currently_profiling);		// service rendering routines
	}
#	endif // __PLAT_NGC__
	
#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
	Sys::CPUProfiler->PushContext( 255, 255, 0 );
#	endif // __USE_PROFILER__
#endif

	if ( end_render_hook )				// end rendering
	{
		end_render_hook->Call();
	}
#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
#	endif // __USE_PROFILER__
#endif
}

/*****************************************************************************
**							   Public Functions								**
*****************************************************************************/

void		Manager::MainLoop( void )
{
#ifdef __PLAT_PS3__
	Ps3Platform::StartupFinished();
	Ps3Platform::Step("main loop started");
#endif
	bool old_flag = done;	// push the current done flag

	done = false;

// lwss add
#ifdef __PLAT_WN32__
	void* timeout;
	SystemParametersInfoA(SPI_GETSCREENSAVETIMEOUT, 0, &timeout, 0);
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);

	int v3 = (frequency.QuadPart / 60) >> 32;
	DWORD v4 = (frequency.QuadPart / 60);

	LARGE_INTEGER perf;
	QueryPerformanceCounter(&perf);

	int v12 = 0;
	while (true)
	{
		// g_debugMeshSubmitCounter = 0; // optional

		if (timeout)
		{
			if (v12++ >= 200)
			{
				v12 = 0;
				SystemParametersInfoA(SPI_SETSCREENSAVETIMEOUT, (UINT)timeout, 0, 0); // KISAKTODO: check logic here...
			}
		}
		NxXbox::g_disableRendering = false;
		Win32_ApplyInputs();
		Win32_ProcessMsgPump();
		Win32_GameNetUpdate();

		if (g_isWindowInFocus)
		{
			break;
		}

		if (g_hasJustEnteredNetworkGame)
		{
			NxXbox::g_disableRendering = true;
			break;
		}

		do
		{
			Sleep(100u);
			Win32_ProcessMsgPump();
		} while (!g_isWindowInFocus);

		if (done)
		{
			done = old_flag;
			return;
		}
	}

#endif

	while ( !done )
	{
// lwss Add
#ifdef __PLAT_WN32__
		//if (g_windowJustWentOutOfFocus)
		//{
		//	if (D3DDevice_TestCooperativeLevel() != D3DERR_DEVICELOST)
		//	{
		//		D3D_ResetTextures();
		//		D3D_ResetBuffers();
		//		D3D_ReleaseRenderSurfaces();
		//		if (D3DDevice_Reset(&NxXbox::EngineGlobals.params) >= 0)
		//		{
		//			D3D_ReCreateTextures();
		//			D3D_ReCreateBuffers();
		//			D3D_RecreateRenderSurfaces();
		//			g_windowJustWentOutOfFocus = false;
		//		}
		//	}
		//	//NxXbox::g_disableRendering = true;
		//}

		static int s_mainLoopCounter = 0;

#ifdef __PLAT_PS3__
		u64 profPhase[kProfPhases] = { 0 };
		u64 profLast = ProfNow();
#endif
#ifdef __PLAT_PS3__
		// PS3 pacing: the display paces the loop. A frame that flipped already waited for the
		// vsync at its start (the renderer's flip gate), so it doesn't also wait for a 60 Hz
		// timer of its own: the two clocks drifted against each other (the display runs at
		// 59.94 Hz), and a frame that had just waited for the timer then waited a whole extra
		// vsync for the flip, 33 ms for a frame with 5 ms of work. The timer still paces frames
		// that drew nothing (loading, the system holding the display), but not the catch-up
		// frames below. Frames that ran late are counted up, and a frame is left undrawn
		// (up to 4 in a row) to let the game logic catch up, as before.
		{
			static unsigned long long s_prevStart = 0;
			static long long s_late = 0;
			static unsigned s_flipsSeen = 0;
			static bool s_prevSkipped = false;
			const long long tick = (long long)(frequency.QuadPart / 60);
			const bool flipped = g_ps3FlipsQueued != s_flipsSeen;
			s_flipsSeen = g_ps3FlipsQueued;
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			if (!flipped && !s_prevSkipped && s_prevStart)
			{
				for (;;)
				{
					const long long remaining = (long long)(s_prevStart + tick) - (long long)now.QuadPart;
					if (remaining <= 0) break;
					// Sleep while more than 1.5 ms remain (lv2 may oversleep a little), then yield.
					const long long remainingUs = remaining * 1000000 / frequency.QuadPart;
					if (remainingUs > 1500)
						sysUsleep((u32)(remainingUs > 20000 ? 19000 : remainingUs - 1000));
					else
						Sleep(0);
					QueryPerformanceCounter(&now);
				}
			}
			PS3_PROF_MARK(kProfWait);
			const long long elapsed = s_prevStart ? (long long)(now.QuadPart - s_prevStart) : tick;
			s_prevStart = now.QuadPart;
			// Up to 0.5% over a tick is on time: the display's 59.94 Hz.
			s_late += elapsed - tick - tick / 200;
			if (s_late < 0) s_late = 0;
			if (s_late > 20 * tick) s_late = 0;	// a load or a long hold: start counting again
			NxXbox::g_disableRendering = false;
			if (s_late > tick / 2 && s_mainLoopCounter < 4 && g_isWindowInFocus)
			{
				++s_mainLoopCounter;
				NxXbox::g_disableRendering = true;
			}
			else
				s_mainLoopCounter = 0;
			s_prevSkipped = NxXbox::g_disableRendering;
			perf.QuadPart = now.QuadPart;	// (unused on PS3 from here)
		}
#else
		LARGE_INTEGER perfcount;
		QueryPerformanceCounter(&perfcount);
		LONG itr;
		for (itr = perfcount.HighPart; perfcount.QuadPart < __PAIR64__(v3, v4) + perf.QuadPart; itr = perfcount.HighPart)
		{
#ifdef __PLAT_PS3__
			// Sleep while more than 1.5 ms remain (waking 1 ms early, as lv2 may oversleep a
			// little), and only yield in a loop for the rest. Spinning the whole wait kept one of
			// the PPU's two hardware threads busy for nothing, away from the loader and mixer.
			const LONGLONG remainingUs = (LONGLONG)(__PAIR64__(v3, v4) + perf.QuadPart - perfcount.QuadPart) * 1000000 / frequency.QuadPart;
			if (remainingUs > 1500)
				sysUsleep((u32)(remainingUs > 20000 ? 19000 : remainingUs - 1000));
			else
				Sleep(0);
#else
			Sleep(0);
#endif
			QueryPerformanceCounter(&perfcount);
		}
#ifdef __PLAT_PS3__
		PS3_PROF_MARK(kProfWait);
#endif
		DWORD v8 = (__PAIR64__(itr, perfcount.LowPart) - perf.QuadPart) >> 32;
		DWORD v7 = perfcount.LowPart - perf.LowPart;
		if (__PAIR64__(itr, perfcount.LowPart) - perf.QuadPart > 20 * __PAIR64__(v3, v4))
		{
			perf.QuadPart = __PAIR64__(itr, perfcount.LowPart) - __PAIR64__(v3, v4);
			v7 = v4;
			v8 = v3;
		}
#ifdef __PLAT_PS3__
		// Draw unless this frame is skipped below. (g_disableRendering is otherwise only
		// cleared once, before the loop, so the first skipped frame stopped all drawing.)
		NxXbox::g_disableRendering = false;
#endif
		if (s_mainLoopCounter >= 4 && g_isWindowInFocus)
		{
			s_mainLoopCounter = 0;
		}
		else
		{
			if (__PAIR64__(v8, v7) < __PAIR64__(v3, v4) + (__PAIR64__(v3, v4) >> 1) && g_isWindowInFocus)
			{
				s_mainLoopCounter = 0;
			}
			else
			{
				//++dword_69B6F0;
				++s_mainLoopCounter;
				//NxXbox::g_disableRendering = true;
#ifdef __PLAT_PS3__
				// Running behind the 60 Hz game clock: skip drawing this frame (at most 4 in a
				// row) so the game logic catches up. Without this the game ran one logic step
				// per drawn frame, so it went into slow motion whenever drawing took too long.
				NxXbox::g_disableRendering = true;
#endif
			}
		}
		perf.QuadPart += __PAIR64__(v3, v4);
#endif

		Tmr::Vblank();
		Tmr::PreUpdateTimerInfo();
	
#endif
// lwss end
#ifdef __PLAT_PS3__
		{
			// The first frames, then every 10 s or so (60 Hz), in the log.
			++s_ps3Frame;
			g_ps3Frame = s_ps3Frame;
			// Hitches: a main loop frame that took over 50 ms (outside the first frames,
			// which load), with where the previous frame ended up, to find the freezes.
			{
				static u64 s_ps3LastFrameTime = 0;
				static unsigned s_ps3Hitches = 0;
				const u64 now = sysGetSystemTime();
				g_ps3FrameTime = now;
				// Over 50 ms (three missed vsyncs): with what the sampler saw that frame.
				if (s_ps3LastFrameTime && s_ps3Frame > 30 && now - s_ps3LastFrameTime > 50000 && s_ps3Hitches < 300)
				{
					++s_ps3Hitches;
					printf("[hitch] frame %u took %u ms (task %p, at %s, script 0x%08x line %u)\n", s_ps3Frame - 1,
						(unsigned)((now - s_ps3LastFrameTime) / 1000), (const void*)g_ps3CurrentTask,
						g_ps3Where ? (const char*)g_ps3Where : "-", g_ps3ScriptSub, g_ps3ScriptLine);
					fflush(stdout);
					Ps3Platform::LogHitchSamples(s_ps3Frame - 1);
				}
				// A frame over a second is nearly always a level load or unload: log the heaps
				// as it left them.
				if (s_ps3LastFrameTime && s_ps3Frame > 30 && now - s_ps3LastFrameTime > 1000000)
					Ps3Platform::MemoryReport("after a long frame (a load?)");
				s_ps3LastFrameTime = now;
			}
			// The XMB in-game menu: pause the game while it is open, the way the game's own
			// pause does (skaters, objects, spawned scripts and sound effects stop), and resume
			// on close if this paused it. Drawing carries on, so the XMB overlay keeps updating.
			{
				static bool s_xmbPaused = false;
				const bool menuOpen = Ps3Platform::SystemMenuOpen();
				Mdl::FrontEnd* front = Mdl::FrontEnd::Instance();
				if (menuOpen && !s_xmbPaused && front && s_ps3Frame > 30 && !front->GamePaused())
				{
					printf("[sys] XMB menu open: game paused\n");
					front->PauseGame(true);
					s_xmbPaused = true;
				}
				else if (!menuOpen && s_xmbPaused)
				{
					s_xmbPaused = false;
					if (front && front->GamePaused())
					{
						printf("[sys] XMB menu closed: game resumed\n");
						front->PauseGame(false);
					}
				}
			}
			g_ps3TraceTasks = s_ps3Frame <= 2;
			// On a 16:9 picture, start in the game's own widescreen mode (the Xbox version
			// did this when the dashboard was set to widescreen): a 16:9 projection and a
			// wider view, instead of the 4:3 picture stretched across the screen. The
			// screen mode can still be changed in the game's options.
			if (s_ps3Frame == 2)
			{
				const float w = (float)NxXbox::EngineGlobals.backbuffer_width, h = (float)NxXbox::EngineGlobals.backbuffer_height;
				const uint32 widescreen = Script::GenerateCRC("screen_setup_widescreen");
				if (h > 0.0f && w / h > 1.5f && Script::ScriptExists(widescreen))
				{
					printf("[gfx] %.0fx%.0f picture: widescreen mode\n", w, h);
					Script::RunScript(widescreen);
				}
			}
			if (s_ps3Frame <= 5 || s_ps3Frame == 10 || s_ps3Frame == 30 || (s_ps3Frame <= 600 ? s_ps3Frame % 60 == 0 : s_ps3Frame % 600 == 0))
			{
				char text[48];
				snprintf(text, sizeof(text), "main loop frame %u", s_ps3Frame);
				Ps3Platform::Step(text);
			}
		}
#endif
		if (trigger_profiling)
		{
			printf ("\nProfiling.....Start of main loop\n\n");
			trigger_profiling--;
			currently_profiling = 1;
		}
		

//#ifndef __PLAT_WN32__

#ifdef	__PLAT_NGPS__		
// for profiling on the PS2, we do a bit of the code from sPreRender here
// so we can time the final waiting for GS		
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PushContext( 0,0,255 );		// blue = wait for VU/DMA/GPU to finish
#	endif		
		NxPs2::WaitForRendering();		// PS2 Specific, wait for prior frame's DMA to finish	
		NxPs2::StuffAfterGSFinished();		// PS2 Specific, DMA altering stuff run after previous frame's DMA finished
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
		Sys::CPUProfiler->PushContext( 128,0,0 );		// red = wait for vblank
#	endif		
		int framerate = Script::GetInteger(0x3214c818/*"lock_framerate"*/);
		if (framerate)	// normal vsync
		{
			static uint64 next_vblanks= 0;
			// Still call VSync, as it updates some functions
			Tmr::VSync();
			while (Tmr::GetVblanks()<next_vblanks)
			{
				// just hanging until vblanks gets big enough
			}
			next_vblanks = Tmr::GetVblanks() + framerate;
		}
		else
		{
			Tmr::VSync1();
		}
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
#	endif		
#endif				
#ifndef	__PLAT_NGC__		
#	ifdef __USE_PROFILER__
		Sys::Profiler::sStartFrame();		  		
#	endif		
#endif		
#ifdef __PLAT_PS3__
		PS3_PROF_MARK(kProfOther);
#endif
#ifdef __PLAT_WN32__
		if (!NxXbox::g_disableRendering)
#endif
		{
			PS3_PHASE("frame: sPreRender");
			Nx::CEngine::sPreRender();			 			// start rendering previous frame's DMA list
		}

#ifdef	__PLAT_NGPS__		
		Sfx::CSpuManager::sUpdateStatus();				// Garrett: This should go into some system task, but I'll put it here for now
#endif

//		Sys::Profiler::sStartFrame();		  		
//#endif

#ifdef	__PLAT_NGC__		
#	ifdef __USE_PROFILER__
		Sys::Profiler::sStartFrame();		  		
#	endif		
#	endif		
					 
#ifdef __PLAT_PS3__
		PS3_PROF_MARK(kProfPre);
#endif
		PS3_PHASE("frame: service_system");
		service_system();

#if	defined(__PLAT_NGPS__) && defined(BATCH_TRI_COLLISION)
		// Enable VU0 collision
		bool got_vu0 = Nx::CBatchTriCollMan::sUseVU0Micro();
		Dbg_Assert(got_vu0);
#endif

 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( -1, (void*)0, (void*)-1);
//		snProfSetFlagValue(0x01);
 #endif

		PS3_PHASE("frame: game_logic");
		game_logic();		

 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( 4, (void*)NULL, (void*)-1);
 #endif		


#if	defined(__PLAT_NGPS__) && defined(BATCH_TRI_COLLISION)
		// Disable VU0 collision
		if (got_vu0)
		{
			Nx::CBatchTriCollMan::sDisableVU0Micro();
		}
#endif

//#ifndef __PLAT_WN32__
		// Display the memory contents, (if memview is active)
		//MemView_Display();
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PushContext( 255, 255, 0 );  // yellow = render world
#	endif		

 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( -1, (void*)0, (void*)-1);
//		snProfSetFlagValue(0x01);
 #endif
		
#ifdef __PLAT_PS3__
		PS3_PROF_MARK(kProfLogic);
#endif
		PS3_PHASE("frame: sRenderWorld");
		Nx::CEngine::sRenderWorld();		
#ifdef	__PLAT_NGC__		
#ifdef		__USE_PROFILER__
		Sys::Render_Profiler();		
#endif
#endif
		// Mick: bit of a patch here, we need some better debug hooks
		//Mdl::Rail_DebugRender();
#	ifdef __USE_PROFILER__
			Sys::CPUProfiler->PushContext( 0, 0, 0 );	 	// Black (Under Yellow) = sPostRender
#	endif // __USE_PROFILER__
#ifdef __PLAT_PS3__
		PS3_PROF_MARK(kProfRender);
#endif
#ifdef __PLAT_WN32__
		if (!NxXbox::g_disableRendering)
#endif		
		{
			PS3_PHASE("frame: sPostRender");
			Nx::CEngine::sPostRender();		  // Previous frames profiler is rendered here
		}
#ifdef __PLAT_PS3__
		PS3_PROF_MARK(kProfPresent);
#endif
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
#	endif		


	

#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
#	endif		
		PS3_PHASE("frame: OncePerRender");
		Tmr::OncePerRender();
		PS3_PHASE("frame: done");
#ifdef __PLAT_PS3__
		PS3_PROF_MARK(kProfOther);
		ProfCommit(profPhase, NxXbox::g_disableRendering, s_ps3Frame, profLast, g_ps3FrameGateTicks - s_profGateLast);
		memset(g_ps3StageTicks, 0, sizeof(g_ps3StageTicks));
		s_profGateLast = g_ps3FrameGateTicks;
#endif
 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( 4, (void*)NULL, (void*)-1);
 #endif		
//#endif		
		
		currently_profiling = false;

	}

	done = old_flag;
}

void Manager::ProfileTasks(int n)
{
	trigger_profiling = n;
	currently_profiling = 0;
}


/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		Manager::QuitLoop( void )
{
	

	Dbg_Notify( "Exiting..." );

	done = true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		Manager::DoGameLogic( void )
{
	game_logic();
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

} // namespace Mlp

