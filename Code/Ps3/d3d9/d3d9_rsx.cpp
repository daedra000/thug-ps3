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
#include <vector>
#include <ps3_fs.h>
#include "d3d9_rsx_internal.h"

extern "C" const char* Ps3UsbRoot();	// ps3_diag.cpp: first USB drive, or NULL

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

// Frame capture, for finding rendering bugs: at frame kCaptureFrame and then every
// kCaptureEvery frames (about 30 s), one frame's draws are logged ([cap] lines) and the
// finished picture is saved in USRDIR (and on a USB drive) as thug-frame-NNN.bmp, NNN
// counting up and wrapping after kCaptureFiles (so a long session keeps the last hour).
// An empty USRDIR/rsx_no_capture.txt turns it off. Each capture stalls the game for a
// moment while the picture is written.
bool s_capture;
static const unsigned kCaptureFrame = 600, kCaptureEvery = 1800, kCaptureFiles = 120;
static bool s_captureOff;
static unsigned s_captureIndex;

static void SaveBmp(const char* path, const u32* pixels, u32 w, u32 h, u32 pitch)
{
	FILE* f = fopen(path, "wb");
	if (!f) { printf("[gfx] capture: couldn't write %s\n", path); return; }
	const u32 rowBytes = w * 3, padded = (rowBytes + 3) & ~3u, dataSize = padded * h;
	u8 hdr[54];
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B'; hdr[1] = 'M';
	const u32 fileSize = 54 + dataSize;
	const u32 fields[] = { fileSize, 0, 54, 40, w, h };
	const int at[] = { 2, 6, 10, 14, 18, 22 };
	for (int i = 0; i < 6; ++i) for (int b = 0; b < 4; ++b) hdr[at[i] + b] = (u8)(fields[i] >> (8 * b));
	hdr[26] = 1; hdr[28] = 24;	// planes, bits per pixel
	for (int b = 0; b < 4; ++b) hdr[34 + b] = (u8)(dataSize >> (8 * b));
	fwrite(hdr, 1, 54, f);
	std::vector<u8> row(padded, 0);
	for (u32 y = 0; y < h; ++y)
	{
		const u32* src = (const u32*)((const u8*)pixels + (h - 1 - y) * pitch);	// BMP rows go bottom up
		for (u32 x = 0; x < w; ++x)
		{
			const u32 c = src[x];	// A8R8G8B8 word
			row[x * 3 + 0] = (u8)c;
			row[x * 3 + 1] = (u8)(c >> 8);
			row[x * 3 + 2] = (u8)(c >> 16);
		}
		fwrite(&row[0], 1, padded, f);
	}
	fclose(f);
	printf("[gfx] capture: saved %s\n", path);
}

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
	s32 r = FifoCallback(s_ctx, words);
	++s_segmentChanges;
	if (r != 0 || s_ctx->current + words > s_ctx->end)
	{
		static int logged;
		if (logged++ < 5)
			printf("[gfx] command buffer: no room for %u words (callback %d, %u free)\n", words, (int)r,
				(unsigned)(s_ctx->end - s_ctx->current));
	}
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

void DisplaySurface(SurfaceDesc* d)
{
	d->colorOffset = s_colorOffset[s_cur];
	d->colorPitch = s_pitch;
	d->depthOffset = s_depthOffset;
	d->depthPitch = s_depthPitch;
	d->width = s_width;
	d->height = s_height;
	d->hasDepth = true;
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
	int t = 0;
	for (; t < 4000 && *(vu32*)gcmGetLabelAddress(kFrameLabel) + 1 < s_frameSeq; ++t) usleep(250);
	if (t == 4000)
	{
		static int logged;
		if (logged++ < 5)
			GpuLog("frame fence: the RSX is over a frame behind after 1 s (label %u, frame %u)",
				*(vu32*)gcmGetLabelAddress(kFrameLabel), s_frameSeq);
	}
	// Change segments here, between frames, if the frame might not fit. libgcm's segments
	// are 32 KB, so ask for under that (a bigger request can never be met).
	CmdReserve(7 * 1024);
	SetRenderTarget();
	DrawFrameStart();
	const unsigned frame = s_frames + 1;
	if (!s_captureOff && frame >= kCaptureFrame && (frame - kCaptureFrame) % kCaptureEvery == 0)
	{
		s_capture = true;
		printf("[cap] frame %u, capture %03u: every draw follows, then the picture is saved\n", frame, s_captureIndex % kCaptureFiles);
	}
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
		{
			char path[600];
			snprintf(path, sizeof(path), "%s/rsx_no_capture.txt", Ps3Fs::AppDir());
			s_captureOff = Ps3Fs::Exists(path);
			if (s_captureOff) printf("[gfx] rsx_no_capture.txt: no frame captures\n");
		}
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
		if (s_capture)
		{
			s_capture = false;
			if (WaitIdle("capture"))
			{
				char path[600];
				const unsigned n = s_captureIndex++ % kCaptureFiles;
				snprintf(path, sizeof(path), "%s/thug-frame-%03u.bmp", Ps3Fs::AppDir(), n);
				SaveBmp(path, s_color[s_cur], s_width, s_height, s_pitch);
				if (const char* usb = Ps3UsbRoot())
				{
					snprintf(path, sizeof(path), "%s/thug-frame-%03u.bmp", usb, n);
					SaveBmp(path, s_color[s_cur], s_width, s_height, s_pitch);
				}
			}
			Ps3Platform::FlushLog();
		}
		gcmSetFlip(s_ctx, s_cur);
		rsxFlushBuffer(s_ctx);
		s_flipPending = true;
		s_cur = (s_cur + 1) % kBuffers;
	}
	if (s_frames <= 3 || (s_frames % 600) == 0)
		GpuLog("frame %u alive (%s; %u segment changes, %u frames skipped and %u flips forced while the display was held)",
			s_frames, DrawStats(), s_segmentChanges, s_skipped, s_forced);
}

void Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	if (!s_ready) return;
	u32 mask = 0;
	if (flags & D3DCLEAR_TARGET)  mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	if (flags & D3DCLEAR_ZBUFFER) mask |= GCM_CLEAR_Z;
	if (flags & D3DCLEAR_STENCIL) mask |= GCM_CLEAR_S;
	if (!mask) return;

	FrameBegin();
	bool hasDepth = false;
	if (s_skipFrame || !ApplyTarget(dev, &hasDepth)) return;
	if (!hasDepth) mask &= ~(GCM_CLEAR_Z | GCM_CLEAR_S);
	if (!mask) return;
	const D3DVIEWPORT9& vp = dev->m_viewport;
	CmdReserve(64);
	rsxSetColorMask(s_ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetDepthWriteEnable(s_ctx, GCM_TRUE);
	rsxSetClearColor(s_ctx, color);	// D3DCOLOR is ARGB, as the RSX wants
	if (z < 0.0f) z = 0.0f;
	if (z > 1.0f) z = 1.0f;
	u32 depth = (u32)(z * 16777215.0f);
	rsxSetClearDepthStencil(s_ctx, (depth << 8) | (stencil & 0xff));
	// D3D clears the viewport, or each of the given rectangles within it.
	const s32 vx0 = (s32)vp.X, vy0 = (s32)vp.Y, vx1 = vx0 + (s32)vp.Width, vy1 = vy0 + (s32)vp.Height;
	const DWORD n = (count && rects) ? count : 1;
	for (DWORD i = 0; i < n; ++i)
	{
		s32 x0 = vx0, y0 = vy0, x1 = vx1, y1 = vy1;
		if (count && rects)
		{
			if (rects[i].x1 > x0) x0 = rects[i].x1;
			if (rects[i].y1 > y0) y0 = rects[i].y1;
			if (rects[i].x2 < x1) x1 = rects[i].x2;
			if (rects[i].y2 < y1) y1 = rects[i].y2;
		}
		if (x1 <= x0 || y1 <= y0) continue;
		CmdReserve(32);
		rsxSetScissor(s_ctx, (u16)x0, (u16)y0, (u16)(x1 - x0), (u16)(y1 - y0));
		rsxClearSurface(s_ctx, mask);
	}
	InvalidateDrawState();	// scissor, colour mask and depth writes changed
}


} // namespace Ps3Gfx

#endif // !PS3_HEADLESS
