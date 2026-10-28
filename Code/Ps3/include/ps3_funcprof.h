// THUG PS3: function-level CPU profile of the game thread, in the profile build only
// (make -f Makefile.ps3 PROFILE=1: PS3_FUNCPROF=1 and the renderer's files compiled with
// -finstrument-functions). See ps3_funcprof.cpp.
#ifndef PS3_FUNCPROF_H
#define PS3_FUNCPROF_H

#ifndef PS3_FUNCPROF
#define PS3_FUNCPROF 0
#endif

namespace Ps3FuncProf
{
#if PS3_FUNCPROF
	void Start() __attribute__((no_instrument_function));		// on the game thread, once
	void FrameEnd() __attribute__((no_instrument_function));	// each main loop frame: the log report every 600
#else
	inline void Start() {}
	inline void FrameEnd() {}
#endif
}

#endif
