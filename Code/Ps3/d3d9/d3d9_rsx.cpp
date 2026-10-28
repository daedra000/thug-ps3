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
//
// Display and depth buffers (the approach of the reVC PS3 port's ps3mem.cpp and the
// PSL1GHT rsxtest_tile sample):
// - Three display buffers by default. The CPU starts a frame once at most one flip is still
//   waiting for its vsync (counted by a flip handler), so a frame that misses a vsync no
//   longer holds the next one up. USRDIR/rsx_double_buffer.txt keeps two buffers and the
//   old flip-status gate exactly as it was.
// - Colour and depth buffers in tile regions: the memory controller spreads their rows over
//   the memory banks, the depth buffer is compressed (Z32_SEPSTENCIL) and has ZCULL, which
//   drops hidden fragments in blocks before they are shaded. Tiling is invisible to the CPU,
//   the texture unit and the display, so nothing else changes but the pitch, which comes from
//   the tiled pitch table (gcmGetTiledPitchSize). Tile regions start on 64 KB boundaries
//   and cover whole 64 KB units of 32-row multiples; the display buffers' 1 MB alignment
//   covers the first. ZCULL covers the depth buffer rounded up to 64 x 64 pixel blocks.
//   USRDIR/rsx_notiles.txt turns all of it off (linear buffers, pitch width * 4, as before);
//   USRDIR/rsx_no_zcull.txt keeps the tiles and compression but not ZCULL.

#include <ps3_platform.h>

#if !PS3_HEADLESS

#include <ps3_gfx_backend.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sys/systime.h>
#include <lv2/mutex.h>
#include <ps3_fs.h>
#include "d3d9_rsx_internal.h"

// Timebase ticks the game thread spent at a frame's start waiting for a flip to happen or for
// the RSX to catch up (FrameBegin), for the main loop's profile (defined in Mainloop.cpp).
extern unsigned long long g_ps3FrameGateTicks;

extern "C" const char* Ps3UsbRoot();	// ps3_diag.cpp: first USB drive, or NULL

namespace Ps3Gfx
{

enum { kMaxBuffers = 3 };
// A New Jersey frame is about 3 MB of commands. With a 256 KB buffer the CPU could never be
// more than 256 KB ahead of the RSX: every segment change waited for it, so the two took turns
// instead of working at once. 4 MB holds more than a frame (the frame fence lets the CPU get
// one frame ahead, no more).
static const u32 kCommandBufferSize = 4 * 1024 * 1024;
static const u32 kIoSize = 5 * 1024 * 1024;	// main memory mapped for the RSX: the command buffer only

gcmContextData* s_ctx;
static void* s_ioBuffer;
static bool s_ready;
static u32 s_width = 1280, s_height = 720, s_pitch;
static u32 s_buffers = kMaxBuffers;	// display buffers in use: 3, or 2 with rsx_double_buffer.txt
static u32* s_color[kMaxBuffers];
static u32 s_colorOffset[kMaxBuffers];
static u32* s_depth;
static u32 s_depthOffset, s_depthPitch;
static u32 s_cur;
// Tile regions: 0..2 the display buffers, 3 the depth buffer. ZCULL region 0 is the depth buffer.
enum { kDepthTile = kMaxBuffers };
static bool s_tiled;		// the display and depth buffers are in tile regions
static bool s_zcomp;		// the depth buffer's tile region is compressed
static bool s_zcull;		// ZCULL is bound to the depth buffer
// Triple buffering: flips queued by Present, flips done (the flip handler counts them), and
// flips given up on while the system held the display (see FlipGate).
static u32 s_flipsQueued, s_flipsWaived;
static volatile u32 s_flipsDone;
static unsigned s_frames, s_draws;
static unsigned s_segmentChanges;
static u64 s_cmdWaitTicks;			// timebase ticks spent in segment changes (waiting for the RSX), since the last report
static unsigned s_lastSegmentChanges;	// s_segmentChanges at the last report
static u32 s_label = 1;
static bool s_firstFrame = true;
static bool s_frameReady;	// this frame has passed the flip gate
bool s_skipFrame;	// the system holds the display: this frame is not drawn
static bool s_frameAborted;	// the command buffer ran out mid-frame: the rest of the frame is dropped
static unsigned s_fenceTimeouts;
static bool s_flipPending;
static int s_heldFrames, s_forcedRun;
static unsigned s_skipped, s_forced;
u32 s_frameSeq;
enum { kLabelIndex = 255, kFrameLabel = 253, kFrameStartLabel = 252 };

// GPU frame times. The RSX writes the frame number to kFrameStartLabel when it reaches a
// frame's first commands and to kFrameLabel (the fence) after its last, both backend labels
// like the fence already is. The hitch sampler thread reads both every millisecond
// (GpuSample) and notes when each changes: end minus start is how long the RSX spent on the
// frame, waiting for the CPU's commands included. Frames over 16.7 ms there can't make 60 fps.
static volatile u32 s_gpuLastStart, s_gpuLastEnd;
static volatile u64 s_gpuStartAt[8];		// when the start label showed frame n (slot n % 8)
static volatile u32 s_gpuStartFrame[8];	// and which frame that was
static volatile u64 s_gpuSumUs, s_gpuWorstUs;
static volatile u32 s_gpuFrames, s_gpuOver16, s_gpuOver33;
// When the CPU finished queueing frame n (Present, before its fence): the time from there to
// the fence passing is how long the RSX still had to work once it had the whole frame. Near
// zero means the RSX keeps up with the CPU (the CPU is the limit); large means the RSX is.
static volatile u64 s_cpuDoneAt[8];
static volatile u32 s_cpuDoneFrame[8];
static volatile u64 s_tailSumUs;
static volatile u32 s_tailFrames, s_tailOver4;
static void GpuSample(unsigned long long now)
{
	const u32 start = *(vu32*)gcmGetLabelAddress(kFrameStartLabel);
	const u32 end = *(vu32*)gcmGetLabelAddress(kFrameLabel);
	if (start != s_gpuLastStart)
	{
		s_gpuStartAt[start & 7] = now;
		s_gpuStartFrame[start & 7] = start;
		s_gpuLastStart = start;
	}
	if (end != s_gpuLastEnd)
	{
		const u64 at = s_gpuStartAt[end & 7];
		if (s_cpuDoneFrame[end & 7] == end && now > s_cpuDoneAt[end & 7])
		{
			const u64 tail = now - s_cpuDoneAt[end & 7];
			if (tail < 2000000)
			{
				s_tailSumUs += tail;
				++s_tailFrames;
				if (tail > 4000) ++s_tailOver4;
			}
		}
		if (s_gpuStartFrame[end & 7] == end)	// this frame's start was seen
		{
			const u64 us = at && now > at ? now - at : 0;
			if (us && us < 2000000)
			{
				s_gpuSumUs += us;
				if (us > s_gpuWorstUs) s_gpuWorstUs = us;
				++s_gpuFrames;
				if (us > 16700) ++s_gpuOver16;
				if (us > 33300) ++s_gpuOver33;
			}
		}
		s_gpuLastEnd = end;
	}
}

// Frame capture, for finding rendering bugs: at frame kCaptureFrame and then every
// kCaptureEvery frames (about 30 s), one frame's draws are logged ([cap] lines) and the
// finished picture is saved in USRDIR (and on a USB drive) as thug-frame-NNN.bmp, NNN
// counting up and wrapping after kCaptureFiles (so a long session keeps the last hour).
// Off unless an empty USRDIR/rsx_capture.txt exists: each capture stalled the game for
// about 10 s (the draw log plus reading the picture back from video memory, twice).
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
	// malloc, not a std::vector: the game's heaps stay out of the renderer (ps3_new.cpp
	// sends new on the game thread to whichever heap the game has pushed).
	u8* row = (u8*)calloc(1, padded);
	if (!row) { fclose(f); printf("[gfx] capture: out of memory for %s\n", path); return; }
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
		fwrite(row, 1, padded, f);
	}
	free(row);
	fclose(f);
	printf("[gfx] capture: saved %s\n", path);
}

// The test switches: an empty file of that name in USRDIR.
static bool FlagFile(const char* name)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", Ps3Fs::AppDir(), name);
	return Ps3Fs::Exists(path);
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
	const u64 t0 = __builtin_ppc_get_timebase();
	s32 r = FifoCallback(s_ctx, words);
	s_cmdWaitTicks += __builtin_ppc_get_timebase() - t0;
	++s_segmentChanges;
	if (r != 0 || s_ctx->current + words > s_ctx->end)
	{
		static int logged;
		if (logged++ < 5)
			printf("[gfx] command buffer: no room for %u words (callback %d, %u free)\n", words, (int)r,
				(unsigned)(s_ctx->end - s_ctx->current));
		// Writing on would go past the segment (or through PSL1GHT's own callback, which can
		// leave garbage in the stream), so the rest of this frame is dropped: draws and clears
		// check s_skipFrame, and Present only fences what was queued.
		if (s_frameReady && !s_skipFrame)
		{
			s_skipFrame = true;
			s_frameAborted = true;
		}
	}
}

u32 FramesDone()
{
	return *(vu32*)gcmGetLabelAddress(kFrameLabel);
}

bool ZcullActive()
{
	return s_zcull;
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
	sf.type          = GCM_SURFACE_TYPE_LINEAR;	// also for tiled buffers: tiling is the memory controller's
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
//
// With three buffers the frame's buffer was last shown three flips ago, so it is free once
// all but the newest queued flip have happened: the flip before last put another buffer on
// screen, and flips happen in order. The flip handler counts them (as in the reVC port and
// the PSL1GHT tiling sample); the hold and force rules are the same as with two buffers.
static void FlipHandler(const u32 head)
{
	(void)head;
	s_flipsDone = s_flipsDone + 1;
}

// Flips queued and not yet done, less those given up on while the display was held.
static s32 FlipsOutstanding()
{
	s32 n = (s32)(s_flipsQueued - s_flipsDone - s_flipsWaived);
	if (n < 0)
	{
		// Flips given up on have happened since (the system let the display go): stop
		// discounting them, or later flips would be missed.
		s_flipsWaived += (u32)n;
		n = 0;
	}
	return n;
}

static bool FlipGateTriple()
{
	if (s_firstFrame) { gcmResetFlipStatus(); s_firstFrame = false; return true; }
	const s32 allowed = (s32)s_buffers - 2;	// the newest flip may still be waiting for its vsync
	// Up to two vsyncs: one vsync (16.7 ms) is the most a flip should take, and the old limit
	// of 60 x 250 us (15 ms) gave up just before it, dropping a frame now and then in play.
	for (int t = 0; t < 140 && FlipsOutstanding() > allowed; ++t) usleep(250);
	if (FlipsOutstanding() <= allowed) { s_forcedRun = 0; s_heldFrames = 0; return true; }
	if (s_heldFrames < 60)
	{
		if (++s_heldFrames == 30) GpuLog("frame %u: the system is holding the display", s_frames);
		usleep(4000);	// the system has the display: give it the CPU too
		return false;
	}
	if (s_forcedRun >= 20) { usleep(4000); return false; }	// the command buffer must not fill up
	++s_forced;
	++s_forcedRun;
	s_flipsWaived += (u32)(FlipsOutstanding() - allowed);	// drawn into anyway, as with two buffers
	s_heldFrames = 0;
	return true;
}

static bool FlipGate()
{
	if (s_buffers > 2) return FlipGateTriple();
	if (s_firstFrame) { gcmResetFlipStatus(); s_firstFrame = false; return true; }
	if (!s_flipPending) return true;
	for (int t = 0; t < 140 && gcmGetFlipStatus() != 0; ++t) usleep(250);	// up to two vsyncs (see FlipGateTriple)
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
	const unsigned long long gate0 = __builtin_ppc_get_timebase();
	s_skipFrame = !FlipGate();
	if (s_skipFrame) { ++s_skipped; g_ps3FrameGateTicks += __builtin_ppc_get_timebase() - gate0; return; }
	// The RSX must have finished the frame before last, which read the buffer we are
	// about to draw into (the flip status alone can't say so after a forced flip).
	int t = 0;
	for (; t < 4000 && FramesDone() + 1 < s_frameSeq; ++t) usleep(250);
	g_ps3FrameGateTicks += __builtin_ppc_get_timebase() - gate0;
	if (t == 4000)
	{
		// Still behind after 1 s. Drawing on would reuse the frame ring slot and the back
		// buffer the RSX may still be reading, so this frame is skipped instead (nothing is
		// queued, so Present neither fences nor flips) and the next frame waits again. The
		// log says so now and then, the way WaitIdle does.
		++s_fenceTimeouts;
		if (s_fenceTimeouts <= 5 || s_fenceTimeouts % 60 == 0)
			GpuLog("frame fence: the RSX is over a frame behind after 1 s (label %u, frame %u): frame skipped (%u so far)",
				FramesDone(), s_frameSeq, s_fenceTimeouts);
		s_skipFrame = true;
		++s_skipped;
		return;
	}
	// Change segments here, between frames, if the frame might not fit. libgcm's segments
	// are 32 KB, and a fresh one has 7167 words free, so ask for less than that: 7168
	// could never be met, and forced a segment change at the start of every frame.
	CmdReserve(6 * 1024);
	if (s_skipFrame) return;	// no room: CmdReserve dropped the frame
	rsxSetWriteBackendLabel(s_ctx, kFrameStartLabel, s_frameSeq + 1);	// GpuSample: this frame starts
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
		const bool noTiles = FlagFile("rsx_notiles.txt");
		const bool noZcull = FlagFile("rsx_no_zcull.txt");
		const bool doubleBuffer = FlagFile("rsx_double_buffer.txt");
		s_buffers = doubleBuffer ? 2 : kMaxBuffers;
		// A tiled surface's pitch must come from the tiled pitch table (5120 bytes at 720p,
		// 8192 at 1080p); the display takes it too.
		s_tiled = !noTiles;
		s_pitch = s_width * 4;
		if (s_tiled)
		{
			const u32 p = gcmGetTiledPitchSize(s_width * 4);
			if (p >= s_width * 4) s_pitch = p;
			else { s_tiled = false; printf("[gfx] no tiled pitch for %u bytes: linear buffers\n", s_width * 4); }
		}

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

		// A tile region covers whole 32-row groups and whole 64 KB units.
		const u32 tileRows = (s_height + GCM_TILE_LOCAL_ALIGN_HEIGHT - 1) & ~(u32)(GCM_TILE_LOCAL_ALIGN_HEIGHT - 1);
		const u32 colorBytes = s_tiled ? ((s_pitch * tileRows + GCM_TILE_ALIGN_SIZE - 1) & ~(u32)(GCM_TILE_ALIGN_SIZE - 1)) : s_pitch * s_height;
		u32 tilesBound = 0;
		for (u32 i = 0; i < s_buffers; ++i)
		{
			s_color[i] = (u32*)rsxMemalign(1024 * 1024, colorBytes);	// 1 MB: see the top of the file (and 64 KB for a tile region)
			if (!s_color[i] && s_buffers > 2 && i == 2)
			{
				// No room for a third buffer: two, with the old flip gate.
				printf("[gfx] out of video memory for a third display buffer: double buffering\n");
				s_buffers = 2;
				break;
			}
			if (!s_color[i])
			{
				printf("[gfx] out of video memory for the display\n");
				return false;
			}
			memset(s_color[i], 0, colorBytes);
			rsxAddressToOffset(s_color[i], &s_colorOffset[i]);
			gcmSetDisplayBuffer(i, s_colorOffset[i], s_pitch, s_width, s_height);
			if (s_tiled)
			{
				// A buffer whose region couldn't be set up stays linear: with the same pitch it
				// is drawn and shown the same, only slower.
				s32 r = gcmSetTileInfo(i, GCM_LOCATION_RSX, s_colorOffset[i], colorBytes, s_pitch, GCM_COMPMODE_DISABLED, 0, 0);
				if (r == 0) r = gcmBindTile(i);
				if (r == 0) tilesBound |= 1u << i;
				else printf("[gfx] tile region %u for display buffer %u failed (%d): linear\n", i, i, (int)r);
			}
		}
		s_depthPitch = s_pitch;	// the same pitch table entry (4 bytes a pixel either way)
		const u32 depthBytes = colorBytes;
		s_depth = (u32*)rsxMemalign(1024 * 1024, depthBytes);
		if (!s_depth)
		{
			printf("[gfx] out of video memory for the depth buffer\n");
			return false;
		}
		rsxAddressToOffset(s_depth, &s_depthOffset);
		if (s_tiled)
		{
			// Compressed depth (separate stencil), bank 2 as in the samples; if the compressed
			// region is refused, a plain tiled one.
			s32 r = gcmSetTileInfo(kDepthTile, GCM_LOCATION_RSX, s_depthOffset, depthBytes, s_depthPitch, GCM_COMPMODE_Z32_SEPSTENCIL, 0, 2);
			if (r == 0) r = gcmBindTile(kDepthTile);
			s_zcomp = r == 0;
			if (r != 0)
			{
				printf("[gfx] compressed tile region for the depth buffer failed (%d): uncompressed\n", (int)r);
				r = gcmSetTileInfo(kDepthTile, GCM_LOCATION_RSX, s_depthOffset, depthBytes, s_depthPitch, GCM_COMPMODE_DISABLED, 0, 2);
				if (r == 0) r = gcmBindTile(kDepthTile);
			}
			if (r == 0) tilesBound |= 1u << kDepthTile;
			else printf("[gfx] tile region for the depth buffer failed (%d): linear\n", (int)r);
			// ZCULL over the depth buffer in 64 x 64 blocks; its memory holds 3 M pixels.
			const u32 zw = (s_width + GCM_ZCULL_ALIGN_WIDTH - 1) & ~(u32)(GCM_ZCULL_ALIGN_WIDTH - 1);
			const u32 zh = (s_height + GCM_ZCULL_ALIGN_HEIGHT - 1) & ~(u32)(GCM_ZCULL_ALIGN_HEIGHT - 1);
			if (!noZcull && (tilesBound & (1u << kDepthTile)) && (s_depthOffset & (GCM_ZCULL_ALIGN_OFFSET - 1)) == 0 &&
				zw * zh <= GCM_ZCULL_RAM_SIZE_MAX)
			{
				gcmSetZcull(0, s_depthOffset, zw, zh, 0, GCM_ZCULL_Z24S8, GCM_SURFACE_CENTER_1, GCM_ZCULL_LESS, GCM_ZCULL_LONES,
					GCM_SCULL_SFUNC_LESS, 1, 0xff);
				s_zcull = true;
			}
		}
		s_tiled = tilesBound != 0;
		const u32 colorMask = (1u << s_buffers) - 1;
		printf("[gfx] display: %u buffers (%s), %ux%u, pitch %u, %s; depth %s%s, ZCULL %s; "
			"switches: rsx_double_buffer.txt %s, rsx_notiles.txt %s, rsx_no_zcull.txt %s\n",
			s_buffers, s_buffers > 2 ? "triple, flip handler gate" : "double, flip status gate", s_width, s_height, s_pitch,
			(tilesBound & colorMask) == colorMask ? "tiled" : (tilesBound & colorMask) ? "partly tiled" : "linear",
			(tilesBound & (1u << kDepthTile)) ? "tiled" : "linear", s_zcomp ? " and compressed" : "", s_zcull ? "on" : "off",
			doubleBuffer ? "present" : "absent", noTiles ? "present" : "absent", noZcull ? "present" : "absent");
		if (s_buffers > 2) gcmSetFlipHandler(FlipHandler);

		*(vu32*)gcmGetLabelAddress(kFrameLabel) = 0;	// the frame fence starts clear
		*(vu32*)gcmGetLabelAddress(kFrameStartLabel) = 0;
		g_ps3GpuSampler = GpuSample;
		s_cur = 0;
		gcmResetFlipStatus();
		s_ready = true;
		{
			char path[600];
			snprintf(path, sizeof(path), "%s/rsx_capture.txt", Ps3Fs::AppDir());
			s_captureOff = !Ps3Fs::Exists(path);
			if (!s_captureOff) printf("[gfx] rsx_capture.txt: capturing a frame every 30 s\n");
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

static sys_lwmutex_t s_deviceLock;
static volatile bool s_deviceLockReady;

static int s_deviceLockOff = -1;	// test switch: USRDIR/no_device_lock.txt

void DeviceLock()
{
	if (s_deviceLockOff < 0)
	{
		char path[600];
		snprintf(path, sizeof(path), "%s/no_device_lock.txt", Ps3Fs::AppDir());
		s_deviceLockOff = Ps3Fs::Exists(path) ? 1 : 0;
		if (s_deviceLockOff) printf("[gfx] no_device_lock.txt: the renderer runs without its lock\n");
	}
	if (s_deviceLockOff) return;
	if (!s_deviceLockReady)
	{
		// First use is on the game thread at start-up, before the loading-bar timer runs.
		sys_lwmutex_attr_t attr;
		memset(&attr, 0, sizeof(attr));
		attr.attr_protocol = SYS_LWMUTEX_PROTOCOL_PRIO;
		attr.attr_recursive = SYS_LWMUTEX_ATTR_RECURSIVE;
		strcpy(attr.name, "d3ddev");
		sysLwMutexCreate(&s_deviceLock, &attr);
		s_deviceLockReady = true;
	}
	sysLwMutexLock(&s_deviceLock, 0);
}

void DeviceUnlock()
{
	if (s_deviceLockOff) return;
	sysLwMutexUnlock(&s_deviceLock);
}

void BeginFrame(IDirect3DDevice9*)
{
	DeviceGuard guard;
	if (s_ready) FrameBegin();
}

void Present(IDirect3DDevice9*)
{
	DeviceGuard guard;
	if (!s_ready) return;
	FrameBegin();	// a frame with no draws or clears still flips a valid buffer
	s_frameReady = false;
	++s_frames;
	if (s_frameAborted)
	{
		// The command buffer ran out part-way through: what was queued still runs, and it may
		// read this frame's ring slot and memory freed after it. Fence it (no flip: the
		// picture is incomplete) so the ring and the deferred frees stay in step; if even the
		// fence doesn't fit, the frame counter stays put and the next frame waits on it.
		s_frameAborted = false;
		CmdReserve(16);
		if (s_ctx->current + 16 <= s_ctx->end)
		{
			rsxSetWriteBackendLabel(s_ctx, kFrameLabel, ++s_frameSeq);
			rsxFlushBuffer(s_ctx);
		}
		s_capture = false;
	}
	else if (!s_skipFrame)
	{
		CmdReserve(64);
		s_cpuDoneAt[(s_frameSeq + 1) & 7] = sysGetSystemTime();
		s_cpuDoneFrame[(s_frameSeq + 1) & 7] = s_frameSeq + 1;
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
		++s_flipsQueued;
		s_cur = (s_cur + 1) % s_buffers;
	}
	if (s_frames <= 3 || (s_frames % 600) == 0)
	{
		// Command-buffer waits: time the CPU spent in segment changes, waiting for the RSX to
		// free space (79800 timebase ticks a millisecond).
		const unsigned frames = s_frames <= 3 ? 1 : 600;
		const unsigned segs = s_segmentChanges - s_lastSegmentChanges;
		const unsigned waitUs = (unsigned)(s_cmdWaitTicks * 1000 / 79800);
		GpuLog("frame %u alive (%s; %u segment changes, %u frames skipped and %u flips forced while the display was held; "
			"command buffer: %u segment changes and %u.%02u ms waiting for the RSX a frame; "
			"RSX %u.%02u ms a frame (worst %u.%02u), %u of %u frames over 16.7 ms, %u over 33.3 ms; "
			"RSX still busy %u.%02u ms after the CPU queued the frame, %u of %u frames over 4 ms)",
			s_frames, DrawStats(), s_segmentChanges, s_skipped, s_forced,
			segs / frames, waitUs / frames / 1000, waitUs / frames % 1000 / 10,
			(unsigned)(s_gpuFrames ? s_gpuSumUs / s_gpuFrames / 1000 : 0), (unsigned)(s_gpuFrames ? s_gpuSumUs / s_gpuFrames % 1000 / 10 : 0),
			(unsigned)(s_gpuWorstUs / 1000), (unsigned)(s_gpuWorstUs % 1000 / 10), (unsigned)s_gpuOver16, (unsigned)s_gpuFrames, (unsigned)s_gpuOver33,
			(unsigned)(s_tailFrames ? s_tailSumUs / s_tailFrames / 1000 : 0), (unsigned)(s_tailFrames ? s_tailSumUs / s_tailFrames % 1000 / 10 : 0),
			(unsigned)s_tailOver4, (unsigned)s_tailFrames);
		s_tailSumUs = 0;
		s_tailFrames = s_tailOver4 = 0;
		s_gpuSumUs = s_gpuWorstUs = 0;
		s_gpuFrames = s_gpuOver16 = s_gpuOver33 = 0;
		s_lastSegmentChanges = s_segmentChanges;
		s_cmdWaitTicks = 0;
	}
}

void Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	DeviceGuard guard;
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
	if (s_skipFrame) return;	// no room left: CmdReserve dropped the frame
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
		if (s_skipFrame) break;	// no room left: CmdReserve dropped the frame
		if (mask & GCM_CLEAR_Z) ZcullDepthClear(x0, y0, x1, y1);
		rsxSetScissor(s_ctx, (u16)x0, (u16)y0, (u16)(x1 - x0), (u16)(y1 - y0));
		rsxClearSurface(s_ctx, mask);
	}
	InvalidateDrawState();	// scissor, colour mask and depth writes changed
}


} // namespace Ps3Gfx

#endif // !PS3_HEADLESS
