// THUG PS3: what d3d9_rsx.cpp (display, frames) and d3d9_rsx_draw.cpp (resources,
// programs, state, draws) share. Both are in namespace Ps3Gfx.
#ifndef PS3_D3D9_RSX_INTERNAL_H
#define PS3_D3D9_RSX_INTERNAL_H

#include <rsx/rsx.h>

class IDirect3DDevice9;

namespace Ps3Gfx
{
	extern gcmContextData* s_ctx;
	extern u32 s_frameSeq;		// frames submitted so far (the frame fence counts these)
	extern bool s_skipFrame;	// this frame is not being drawn (the system holds the display)

	void CmdReserve(u32 words);	// room for `words` command words (see d3d9_rsx.cpp)
	void GpuLog(const char* fmt, ...);
	bool WaitIdle(const char* what);

	bool DrawInit();			// after the video set-up
	void DrawFrameStart();		// after a frame passes the flip gate
	const char* DrawStats();	// one line for the log, then resets the counters
	bool TargetIsDisplay(IDirect3DDevice9* dev);	// drawing goes to the back buffer
}

#endif // PS3_D3D9_RSX_INTERNAL_H
