// THUG PS3: what d3d9_rsx.cpp (display, frames) and d3d9_rsx_draw.cpp (resources,
// programs, state, draws) share. Both are in namespace Ps3Gfx.
#ifndef PS3_D3D9_RSX_INTERNAL_H
#define PS3_D3D9_RSX_INTERNAL_H

#include <rsx/rsx.h>

class IDirect3DDevice9;

namespace Ps3Gfx
{
// One recursive lock around every backend entry point: the shim's stand-in for D3D's
// MULTITHREADED device. The loading bar draws from a timer thread while the game thread
// releases textures and buffers, and both touch the deferred-free list, the bound-texture
// table and the command buffer.
void DeviceLock();
void DeviceUnlock();
struct DeviceGuard
{
	DeviceGuard() { DeviceLock(); }
	~DeviceGuard() { DeviceUnlock(); }
};

	extern gcmContextData* s_ctx;
	extern u32 s_frameSeq;		// frames submitted so far (the frame fence counts these)
	extern bool s_skipFrame;	// this frame is not being drawn (the system holds the display)
	extern bool s_capture;		// this frame is being captured: each draw is logged ([cap] lines)

	void CmdReserve(u32 words);	// room for `words` command words (see d3d9_rsx.cpp); if there is
								// none mid-frame, the rest of the frame is dropped (s_skipFrame)
	// Frames the RSX has finished: the frame fence's label. Frame n (s_frameSeq == n while it
	// is built) is done once this reaches n + 1, so a "used in frame n, +1" stamp compares
	// straight against it.
	u32 FramesDone();
	unsigned SegmentChanges();	// command-buffer segment changes so far (CmdReserve)
	void GpuLog(const char* fmt, ...);
	bool WaitIdle(const char* what);

	bool DrawInit();			// after the video set-up
	void DrawFrameStart();		// after a frame passes the flip gate
	const char* DrawStats();	// one line for the log, then resets the counters
	// The surface draws and clears go to.
	struct SurfaceDesc { u32 colorOffset, colorPitch, depthOffset, depthPitch, width, height; bool hasDepth; };
	void DisplaySurface(SurfaceDesc* out);			// this frame's back buffer and the depth buffer
	bool ApplyTarget(IDirect3DDevice9* dev, bool* hasDepth);	// binds the device's render target; false: can't draw there
	void InvalidateDrawState();						// d3d9_rsx.cpp changed state behind the draw module's back
	// ZCULL on the display's depth buffer (d3d9_rsx.cpp sets it up; off with rsx_notiles.txt or
	// rsx_no_zcull.txt). The draw module turns it on and off per draw and is told before each
	// depth clear of the bound target's [x0, x1) x [y0, y1).
	bool ZcullActive();
	void ZcullDepthClear(s32 x0, s32 y0, s32 x1, s32 y1);
}

#endif // PS3_D3D9_RSX_INTERNAL_H
