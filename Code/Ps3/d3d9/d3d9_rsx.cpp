// THUG PS3: the RSX backend of the D3D9 layer: video set-up, the command buffer, frames,
// flips and clears. Resources, programs, render state and draws are in d3d9_rsx_draw.cpp.
//
// Video set-up follows the PS3 ports proven on hardware (re3-PS3, Quake2PS3):
// rsxInit once, 720p when the TV takes it, otherwise the TV's own mode.
//
// Command buffer: every group of commands reserves its space first (CmdReserve) through
// a correct call to libgcm's segment callback. PSL1GHT's own call (rsxContextCallback)
// can leave garbage in the command stream when it changes segments, which locks the RSX
// up at random-looking frames. Found by rsxkit (https://github.com/ricowp81/rsxkit,
// src/fifo.c, MIT); the same fix cured the lock-ups of the GridSiege GPU renderer.
//
// XMB screenshots (and anything else that holds the display) follow Grid Siege 3D's
// rules, all found on a console:
// - 1 MB of RSX-mapped main memory holding a 256 KB command buffer (tiny3d's layout;
//   larger mappings made the system keep our RSX channel after a screenshot). So GPU
//   resources go in RSX local memory, never in mapped main memory.
// - Display buffers on 1 MB boundaries (64-byte alignment froze the screenshot).
// - The RSX never waits for a flip (no gcmSetWaitFlip). The CPU checks the last flip
//   before starting a frame: while the system holds it the frame is skipped, and after
//   about a second a frame is queued anyway (at most 20 in a row).
// - A frame-fence label is written after each frame's commands.

#include <ps3_platform.h>

#if !PS3_HEADLESS

#include <ps3_gfx_backend.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sys/systime.h>
#include "d3d9_rsx_internal.h"

namespace Ps3Gfx
{

enum { kBuffers = 2 };
static const u32 kCommandBufferSize = 256 * 1024;
static const u32 kIoSize = 1024 * 1024;	// main memory mapped for the RSX: the command buffer only

gcmContextData* s_ctx;
static void* s_ioBuffer;
static bool s_ready;
static u32 s_width = 1280, s_height = 720, s_pitch;
static u32* s_color[kBuffers];
static u32 s_colorOffset[kBuffers];
static u32* s_depth;
static u32 s_depthOffset, s_depthPitch;
static u32 s_cur;
static unsigned s_frames, s_draws;
static unsigned s_segmentChanges;
static u32 s_label = 1;
static bool s_firstFrame = true;
static bool s_frameReady;	// this frame has passed the flip gate
bool s_skipFrame;	// the system holds the display: this frame is not drawn
static bool s_flipPending;
static int s_heldFrames, s_forcedRun;
static unsigned s_skipped, s_forced;
u32 s_frameSeq;
enum { kLabelIndex = 255, kFrameLabel = 253 };

// A correct call to libgcm's segment callback (see the top of the file).
static s32 __attribute__((noinline)) FifoCallback(gcmContextData* context, u32 count)
{
	register u64 r3 __asm__("r3") = (u64)(uintptr_t)context;
	register u64 r4 __asm__("r4") = count;
	u32 opd = (u32)(uintptr_t)context->callback;	// 32-bit function descriptor: entry, TOC
	__asm__ __volatile__(
		"stdu 1,-128(1)\n\t"
		"std 2,40(1)\n\t"
		"lwz 0,0(%2)\n\t"
		"lwz 2,4(%2)\n\t"
		"mtctr 0\n\t"
		"bctrl\n\t"
		"ld 2,40(1)\n\t"
		"addi 1,1,128\n\t"
		: "+r"(r3), "+r"(r4)
		: "b"((u64)opd)
		: "r0", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "ctr", "lr", "xer",
		  "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
	return (s32)r3;
}

// Makes room for `words` command words, so PSL1GHT's functions never change segments.
void CmdReserve(u32 words)
{
	if (!s_ctx || s_ctx->current + words <= s_ctx->end) return;
	FifoCallback(s_ctx, words);
	++s_segmentChanges;
}

void GpuLog(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	printf("[gfx] ");
	vprintf(fmt, args);
	printf("\n");
	va_end(args);
	Ps3Platform::FlushLog();	// on disc straight away, in case the RSX hangs next
}

// Waits until the RSX has run everything queued so far (bounded, logged).
bool WaitIdle(const char* what)
{
	CmdReserve(16);
	rsxSetWriteBackendLabel(s_ctx, kLabelIndex, s_label);
	rsxFlushBuffer(s_ctx);
	for (int waited = 0; *(vu32*)gcmGetLabelAddress(kLabelIndex) != s_label; waited += 30)
	{
		if (waited > 3000000) { GpuLog("%s: RSX did not go idle in 3 s", what); ++s_label; return false; }
		usleep(30);
	}
	++s_label;
	return true;
}

static void SetRenderTarget()
{
	gcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat     = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget     = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]  = s_colorOffset[s_cur];
	sf.colorPitch[0]   = s_pitch;
	for (int i = 1; i < GCM_MAX_MRT_COUNT; ++i)
	{
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = s_colorOffset[s_cur];
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat   = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset   = s_depthOffset;
	sf.depthPitch    = s_depthPitch;
	sf.type          = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias     = GCM_SURFACE_CENTER_1;
	sf.width         = s_width;
	sf.height        = s_height;
	sf.x = 0;
	sf.y = 0;
	CmdReserve(64);
	rsxSetSurface(s_ctx, &sf);
}

// Before a frame's first command: has the previous flip happened? Its buffer is on
// screen until then. Gives a normal vsync a few ms; if the system holds the flip
// (an XMB screenshot, say), the frame is skipped instead of waited for. After about a
// second a frame is queued anyway: the screenshot only lets go once the game flips
// again, and waiting for it froze the console for good (Grid Siege v2.6.5, v3.10.3).
static bool FlipGate()
{
	if (s_firstFrame) { gcmResetFlipStatus(); s_firstFrame = false; return true; }
	if (!s_flipPending) return true;
	for (int t = 0; t < 60 && gcmGetFlipStatus() != 0; ++t) usleep(250);	// up to one vsync
	if (gcmGetFlipStatus() == 0) s_forcedRun = 0;
	if (gcmGetFlipStatus() != 0 && s_heldFrames < 60)
	{
		if (++s_heldFrames == 30) GpuLog("frame %u: the system is holding the display", s_frames);
		usleep(4000);	// the system has the display: give it the CPU too
		return false;
	}
	if (gcmGetFlipStatus() != 0 && s_forcedRun >= 20) { usleep(4000); return false; }	// the command buffer must not fill up
	if (gcmGetFlipStatus() != 0) { ++s_forced; ++s_forcedRun; }
	s_heldFrames = 0;
	s_flipPending = false;
	gcmResetFlipStatus();
	return true;
}

// Called before the first command of a frame (clear, draw or present).
static void FrameBegin()
{
	if (s_frameReady) return;
	s_frameReady = true;
	s_skipFrame = !FlipGate();
	if (s_skipFrame) { ++s_skipped; return; }
	// The RSX must have finished the frame before last, which read the buffer we are
	// about to draw into (the flip status alone can't say so after a forced flip).
	for (int t = 0; t < 4000 && *(vu32*)gcmGetLabelAddress(kFrameLabel) + 1 < s_frameSeq; ++t) usleep(250);
	// Change segments here, between frames, if the frame might not fit.
	CmdReserve(32 * 1024);
	SetRenderTarget();
	DrawFrameStart();
}

bool Init(D3DPRESENT_PARAMETERS* pp)
{
	if (!s_ready)
	{
		s_ioBuffer = memalign(1024 * 1024, kIoSize);
		if (!s_ioBuffer || rsxInit(&s_ctx, kCommandBufferSize, kIoSize, s_ioBuffer) != 0 || !s_ctx)
		{
			printf("[gfx] rsxInit failed\n");
			return false;
		}

		videoState state;
		videoGetState(0, 0, &state);
		s32 mode = state.displayMode.resolution;
		u8 aspect = state.displayMode.aspect;
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720, VIDEO_ASPECT_AUTO, 0))
		{
			mode = VIDEO_RESOLUTION_720;
			aspect = VIDEO_ASPECT_16_9;
		}
		videoResolution res;
		if (videoGetResolution(mode, &res) != 0)
		{
			printf("[gfx] videoGetResolution failed\n");
			return false;
		}
		s_width = res.width;
		s_height = res.height;
		s_pitch = s_width * 4;

		// Idle the RSX before reconfiguring the video output.
		CmdReserve(32);
		rsxSetWriteBackendLabel(s_ctx, kLabelIndex, s_label);
		rsxSetWaitLabel(s_ctx, kLabelIndex, s_label);
		++s_label;
		WaitIdle("start-up");

		videoConfiguration vc;
		memset(&vc, 0, sizeof(vc));
		vc.resolution = mode;
		vc.format = VIDEO_BUFFER_FORMAT_XRGB;
		vc.pitch = s_pitch;
		vc.aspect = aspect;
		if (videoConfigure(0, &vc, NULL, 0) != 0)
		{
			printf("[gfx] videoConfigure failed\n");
			return false;
		}
		for (int waited = 0; waited < 300; ++waited)
		{
			if (videoGetState(0, 0, &state) != 0 || state.state != 3)
				break;
			usleep(10000);
		}
		gcmSetFlipMode(GCM_FLIP_VSYNC);

		for (int i = 0; i < kBuffers; ++i)
		{
			s_color[i] = (u32*)rsxMemalign(1024 * 1024, s_pitch * s_height);	// 1 MB: see the top of the file
			if (!s_color[i])
			{
				printf("[gfx] out of video memory for the display\n");
				return false;
			}
			memset(s_color[i], 0, s_pitch * s_height);
			rsxAddressToOffset(s_color[i], &s_colorOffset[i]);
			gcmSetDisplayBuffer(i, s_colorOffset[i], s_pitch, s_width, s_height);
		}
		s_depthPitch = s_width * 4;
		s_depth = (u32*)rsxMemalign(1024 * 1024, s_depthPitch * s_height);
		if (!s_depth)
		{
			printf("[gfx] out of video memory for the depth buffer\n");
			return false;
		}
		rsxAddressToOffset(s_depth, &s_depthOffset);

		*(vu32*)gcmGetLabelAddress(kFrameLabel) = 0;	// the frame fence starts clear
		s_cur = 0;
		gcmResetFlipStatus();
		s_ready = true;
		if (!DrawInit())
			GpuLog("draw set-up failed: frames will be cleared but not drawn");
		printf("[gfx] RSX ready: %ux%u (stage 1: clears only, no drawing yet)\n", s_width, s_height);
	}

	pp->BackBufferWidth = s_width;
	pp->BackBufferHeight = s_height;
	pp->BackBufferFormat = D3DFMT_A8R8G8B8;
	return true;
}

void Shutdown()
{
	// The game never shuts the device down before exiting.
}

void BeginFrame(IDirect3DDevice9*)
{
	if (s_ready) FrameBegin();
}

void Present(IDirect3DDevice9*)
{
	if (!s_ready) return;
	FrameBegin();	// a frame with no draws or clears still flips a valid buffer
	s_frameReady = false;
	++s_frames;
	if (!s_skipFrame)
	{
		CmdReserve(64);
		rsxSetWriteBackendLabel(s_ctx, kFrameLabel, ++s_frameSeq);	// the frame fence
		// Flip at the next vsync. No gcmSetWaitFlip: the CPU checks the flip before the
		// next frame (FlipGate), so nothing is queued behind a held flip.
		gcmSetFlip(s_ctx, s_cur);
		rsxFlushBuffer(s_ctx);
		s_flipPending = true;
		s_cur = (s_cur + 1) % kBuffers;
	}
	if (s_frames <= 3 || (s_frames % 600) == 0)
		GpuLog("frame %u alive (%s; %u segment changes, %u frames skipped and %u flips forced while the display was held)",
			s_frames, DrawStats(), s_segmentChanges, s_skipped, s_forced);
}

void Clear(IDirect3DDevice9* dev, DWORD, const D3DRECT*, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	if (!s_ready) return;
	u32 mask = 0;
	if (flags & D3DCLEAR_TARGET)  mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	if (flags & D3DCLEAR_ZBUFFER) mask |= GCM_CLEAR_Z;
	if (flags & D3DCLEAR_STENCIL) mask |= GCM_CLEAR_S;
	if (!mask) return;

	FrameBegin();
	if (s_skipFrame || !TargetIsDisplay(dev)) return;
	const D3DVIEWPORT9& vp = dev->m_viewport;
	CmdReserve(64);
	rsxSetScissor(s_ctx, (u16)vp.X, (u16)vp.Y, (u16)vp.Width, (u16)vp.Height);
	rsxSetColorMask(s_ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetDepthWriteEnable(s_ctx, GCM_TRUE);
	rsxSetClearColor(s_ctx, color);	// D3DCOLOR is ARGB, as the RSX wants
	u32 depth = (u32)(z * 16777215.0f);
	rsxSetClearDepthStencil(s_ctx, (depth << 8) | (stencil & 0xff));
	rsxClearSurface(s_ctx, mask);
}


} // namespace Ps3Gfx

#endif // !PS3_HEADLESS
