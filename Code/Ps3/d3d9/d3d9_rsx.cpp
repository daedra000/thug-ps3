// THUG PS3: the RSX backend of the D3D9 layer.
//
// Stage 1 (this file so far): video set-up at the TV's resolution, double-buffered
// display flipped on vsync, and the game's clears. Draw calls are counted but not
// drawn yet; resources stay in main memory. The next stages add textured 2D
// (menus, loading screens), then the 3D meshes and their shaders.
//
// Video set-up follows the PS3 ports proven on hardware (re3-PS3, Quake2PS3):
// rsxInit once, 720p when the TV takes it, otherwise the TV's own mode.
//
// Command buffer: every group of commands reserves its space first (CmdReserve) through
// a correct call to libgcm's segment callback. PSL1GHT's own call (rsxContextCallback)
// can leave garbage in the command stream when it changes segments, which locks the RSX
// up at random-looking frames. Found by rsxkit (https://github.com/ricowp81/rsxkit,
// src/fifo.c, MIT); the same fix cured the lock-ups of the GridSiege GPU renderer.

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

namespace Ps3Gfx
{

enum { kBuffers = 2 };
static const u32 kCommandBufferSize = 1 * 1024 * 1024;
static const u32 kIoSize = 16 * 1024 * 1024;	// main memory mapped for the RSX (holds the command buffer)

static gcmContextData* s_ctx;
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
static bool s_frameReady;	// the previous flip has been waited for
enum { kLabelIndex = 255 };

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
static void CmdReserve(u32 words)
{
	if (!s_ctx || s_ctx->current + words <= s_ctx->end) return;
	FifoCallback(s_ctx, words);
	++s_segmentChanges;
}

static void GpuLog(const char* fmt, ...)
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
static bool WaitIdle(const char* what)
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

static void WaitFlip()
{
	// Bounded: a hung RSX leaves a line in the log instead of a silent freeze.
	int waited = 0;
	bool told = false;
	while (gcmGetFlipStatus() != 0)
	{
		usleep(200);
		waited += 200;
		if (waited > 1000000 && !told) { told = true; GpuLog("frame %u: previous frame still not shown after 1 s", s_frames); }
		if (waited > 3000000) { GpuLog("frame %u: gave up waiting for the flip", s_frames); break; }
	}
	gcmResetFlipStatus();
}

// Called before the first command of a frame: the previous flip must be done before
// its buffer is drawn into again. Waiting here rather than right after queueing the
// flip lets the CPU build the next frame while the TV waits for vsync.
static void FrameBegin()
{
	if (s_frameReady) return;
	if (!s_firstFrame) WaitFlip();
	else gcmResetFlipStatus();
	s_firstFrame = false;
	s_frameReady = true;
	// Change segments here, between frames, if the frame might not fit.
	CmdReserve(64 * 1024);
	SetRenderTarget();
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
			s_color[i] = (u32*)rsxMemalign(64, s_pitch * s_height);
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
		s_depth = (u32*)rsxMemalign(64, s_depthPitch * s_height);
		rsxAddressToOffset(s_depth, &s_depthOffset);

		s_cur = 0;
		gcmResetFlipStatus();
		s_ready = true;
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
	CmdReserve(64);
	// The RSX waits for the flip before running the next frame's commands; the CPU
	// waits for it at the start of the next frame (FrameBegin).
	gcmSetFlip(s_ctx, s_cur);
	rsxFlushBuffer(s_ctx);
	gcmSetWaitFlip(s_ctx);
	s_cur = (s_cur + 1) % kBuffers;
	s_frameReady = false;

	++s_frames;
	if (s_frames <= 3 || (s_frames % 600) == 0)
		GpuLog("frame %u alive (%u draws not drawn yet, %u segment changes)", s_frames, s_draws, s_segmentChanges);
	s_draws = 0;
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

void Draw(IDirect3DDevice9*, const DrawCall&)
{
	++s_draws;
}

void TextureChanged(IDirect3DTexture9*) {}
void TextureReleased(IDirect3DTexture9*) {}
void SurfaceReleased(IDirect3DSurface9*) {}
void VertexBufferChanged(IDirect3DVertexBuffer9*, UINT, UINT) {}
void VertexBufferReleased(IDirect3DVertexBuffer9*) {}
void IndexBufferChanged(IDirect3DIndexBuffer9*, UINT, UINT) {}
void IndexBufferReleased(IDirect3DIndexBuffer9*) {}
void ShaderCreated(IDirect3DVertexShader9*) {}
void ShaderCreated(IDirect3DPixelShader9*) {}
void ShaderReleased(IDirect3DVertexShader9*) {}
void ShaderReleased(IDirect3DPixelShader9*) {}

UINT VideoMemoryFree()
{
	return 200 * 1024 * 1024;
}

} // namespace Ps3Gfx

#endif // !PS3_HEADLESS
