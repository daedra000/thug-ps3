// THUG PS3: RSX resources, programs, render state and draws for the D3D9 layer.
//
// - Resources: the D3D9 objects keep a main-memory copy of every texture, vertex and
//   index buffer (d3d9_device.cpp). The RSX gets its own copy in RSX local memory, made
//   when a draw first needs it and again after the game changes it. Buffers changed
//   while the RSX may still be reading them, and the *UP draws' data, go through a
//   per-frame ring (three frames deep; the frame fence makes the slot three frames back
//   free).
// - Programs: the vertex program comes from the fixed-function state (the game draws
//   with SetFVF), the fragment program from the texture stages or the game's ps_1_1
//   source; both are generated (Code/Ps3/rsx/rsx_shadergen.cpp), assembled at run time
//   (rsx_asm.cpp) and cached. Fragment-program constants live in the program's code,
//   so a program with new constant values is copied to the ring and patched.
// - Conventions: vertex colours are host-order ARGB words read as 4 bytes (A,R,G,B);
//   texels are host-order (d3d9 layer, see docs/ps3-byte-order.md); the viewport is
//   GL-style with D3D9's half-pixel offset; D3D depth 0..w is moved to -w..w in the
//   vertex program.
// - Render targets: textures created as render targets (or in a depth format) get a
//   linear A8R8G8B8 / Z24S8 buffer in RSX memory that draws go to and that is sampled
//   as a linear texture; standalone surfaces get a buffer of their own.
//
// Switches for testing on a console (an empty file in USRDIR turns one on):
//   rsx_linear_textures.txt  never swizzle textures (and so no mipmaps)
//   rsx_flip_cull.txt        swap which triangle winding is culled
//   rsx_no_draw.txt          clear and flip only, draw nothing

#include <ps3_platform.h>

#if !PS3_HEADLESS

#include <ps3_gfx_backend.h>
#include <ps3_fs.h>
#include "d3d9_rsx_internal.h"
#include "../rsx/rsx_asm.h"
#include "../rsx/rsx_shadergen.h"
#include "../rsx/rsx_pipeline.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <map>
#include <string>
#include <vector>
#include <rsx/rsx.h>

namespace Ps3Gfx
{

using namespace RsxShaderGen;
using namespace RsxPipeline;

// The shader generator's D3D values must be D3D9's.
static_assert(D3D::TOP_LERP == D3DTOP_LERP && D3D::TOP_DISABLE == D3DTOP_DISABLE, "D3DTOP values");
static_assert(D3D::TA_TEMP == D3DTA_TEMP && D3D::TA_COMPLEMENT == D3DTA_COMPLEMENT, "D3DTA values");

// ---------------------------------------------------------------------------
// Settings, statistics
// ---------------------------------------------------------------------------
static bool s_drawReady;
static bool s_forceLinear, s_flipCull, s_noDraw;

static struct Stats
{
	unsigned draws, drawn, skipped, uploads, uploadKB, ringKB, programs, programFailures, ringFull, offscreen;
} s_stats;
static char s_statsLine[256];

static bool FlagFile(const char* name)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", Ps3Fs::AppDir(), name);
	return Ps3Fs::Exists(path);
}

// Logs a message the first `limit` times its key is seen.
static void LogLimited(const std::string& key, int limit, const char* fmt, ...)
{
	static std::map<std::string, int> seen;
	int& n = seen[key];
	if (n++ >= limit) return;
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	GpuLog("%s", buf);
}

// ---------------------------------------------------------------------------
// RSX local memory and the per-frame ring
// ---------------------------------------------------------------------------
enum { kRingFrames = 3 };
static const u32 kRingSize = 6 * 1024 * 1024;	// per frame
static u8* s_ring;
static u32 s_ringOffset;	// RSX offset of s_ring
static u32 s_ringUsed;
static u32 s_ringBase;		// this frame's slot

static void* LocalAlloc(u32 size, u32 align, u32* offset)
{
	void* p = rsxMemalign(align, size);
	if (!p) return NULL;
	if (rsxAddressToOffset(p, offset) != 0) { rsxFree(p); return NULL; }
	return p;
}

// RSX memory the GPU may still read: freed once the frame that last could use it is done.
// While frame s_frameSeq is being built, frames up to s_frameSeq - 2 have finished
// (FrameBegin waits on the frame fence); one more frame of margin is kept.
struct DeferredFree { void* mem; u32 frame; };
static std::vector<DeferredFree> s_deferredFrees;

static void DeferFree(void* mem)
{
	if (!mem) return;
	DeferredFree d = { mem, s_frameSeq };
	s_deferredFrees.push_back(d);
}

static void RunDeferredFrees()
{
	size_t keep = 0;
	for (size_t i = 0; i < s_deferredFrees.size(); ++i)
	{
		if (s_deferredFrees[i].frame + 3 <= s_frameSeq)
			rsxFree(s_deferredFrees[i].mem);
		else
			s_deferredFrees[keep++] = s_deferredFrees[i];
	}
	s_deferredFrees.resize(keep);
}

// Frame of last use, stored +1 so 0 means never used.
static bool GpuMayUse(u32 usedPlusOne)
{
	return usedPlusOne != 0 && usedPlusOne - 1 + 3 > s_frameSeq;
}

// Space in this frame's ring slot, or NULL when it is full.
static u8* RingAlloc(u32 size, u32 align, u32* offset)
{
	u32 at = (s_ringUsed + align - 1) & ~(align - 1);
	if (at + size > kRingSize)
	{
		++s_stats.ringFull;
		LogLimited("ringfull", 5, "frame ring full (%u KB): a draw was skipped", kRingSize / 1024);
		return NULL;
	}
	s_ringUsed = at + size;
	*offset = s_ringOffset + s_ringBase + at;
	return s_ring + s_ringBase + at;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
struct RsxTexture
{
	u8* mem;
	u32 offset, bytes;
	gcmTexture desc;
	bool valid;
	bool renderTarget;	// drawn by the RSX: never uploaded from the main-memory copy
	bool depth;			// a depth render target
	u32 pitch;
	u32 usedPlusOne;	// frame of the last draw that sampled it, +1 (0: never)
};

static const RsxTexture* s_bound[4];
static DWORD s_boundSampler[4][4];

static bool IsDepthFormat(D3DFORMAT f)
{
	return f == D3DFMT_D24S8 || f == D3DFMT_D24X8 || f == D3DFMT_D16 || f == D3DFMT_D32 || f == D3DFMT_D24X4S4 || f == D3DFMT_D24FS8 || f == D3DFMT_D16_LOCKABLE;
}

static bool IsRenderTarget(const IDirect3DTexture9* t)
{
	return (t->m_usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || IsDepthFormat(t->m_format);
}

static RsxTexture s_white;	// bound where a program samples a stage with no texture

static bool IsPow2(u32 v) { return v && !(v & (v - 1)); }

static u32 Remap(u32 a, u32 r, u32 g, u32 b, u32 typeA = GCM_TEXTURE_REMAP_TYPE_REMAP, u32 typeRGB = GCM_TEXTURE_REMAP_TYPE_REMAP)
{
	return (typeRGB << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) | (typeRGB << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
	       (typeRGB << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) | (typeA << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
	       (b << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) | (g << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
	       (r << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) | (a << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
}

static u32 IdentityRemap()
{
	return Remap(GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_G, GCM_TEXTURE_REMAP_COLOR_B);
}

// NV40 swizzle of a w x h image: x and y bits interleaved from the bottom (x first)
// while both dimensions have bits left, then the larger dimension's remaining bits.
static void SwizzleTables(u32 w, u32 h, std::vector<u32>& xt, std::vector<u32>& yt)
{
	xt.assign(w, 0);
	yt.assign(h, 0);
	int xbit[16], ybit[16], c = 0;
	for (int i = 0; i < 16; ++i)
	{
		xbit[i] = ybit[i] = -1;
		if ((1u << i) < w) xbit[i] = c++;
		if ((1u << i) < h) ybit[i] = c++;
	}
	for (u32 x = 0; x < w; ++x)
		for (int i = 0; i < 16; ++i)
			if (xbit[i] >= 0 && (x >> i) & 1) xt[x] |= 1u << xbit[i];
	for (u32 y = 0; y < h; ++y)
		for (int i = 0; i < 16; ++i)
			if (ybit[i] >= 0 && (y >> i) & 1) yt[y] |= 1u << ybit[i];
}

struct FormatInfo { u32 gcm, remap, bpp; bool compressed, ok; };

static FormatInfo MapFormat(D3DFORMAT f)
{
	FormatInfo fi = { 0, IdentityRemap(), 4, false, true };
	const u32 A = GCM_TEXTURE_REMAP_COLOR_A, R = GCM_TEXTURE_REMAP_COLOR_R, G = GCM_TEXTURE_REMAP_COLOR_G, B = GCM_TEXTURE_REMAP_COLOR_B;
	switch (f)
	{
	case D3DFMT_A8R8G8B8: fi.gcm = GCM_TEXTURE_FORMAT_A8R8G8B8; break;
	case D3DFMT_X8R8G8B8: fi.gcm = GCM_TEXTURE_FORMAT_A8R8G8B8; fi.remap = Remap(A, R, G, B, GCM_TEXTURE_REMAP_TYPE_ONE); break;
	case D3DFMT_A1R5G5B5: fi.gcm = GCM_TEXTURE_FORMAT_A1R5G5B5; fi.bpp = 2; break;
	case D3DFMT_X1R5G5B5: fi.gcm = GCM_TEXTURE_FORMAT_A1R5G5B5; fi.bpp = 2; fi.remap = Remap(A, R, G, B, GCM_TEXTURE_REMAP_TYPE_ONE); break;
	case D3DFMT_R5G6B5:   fi.gcm = GCM_TEXTURE_FORMAT_R5G6B5; fi.bpp = 2; break;
	case D3DFMT_A4R4G4B4: fi.gcm = GCM_TEXTURE_FORMAT_A4R4G4B4; fi.bpp = 2; break;
	case D3DFMT_L8:       fi.gcm = GCM_TEXTURE_FORMAT_B8; fi.bpp = 1; fi.remap = Remap(B, B, B, B, GCM_TEXTURE_REMAP_TYPE_ONE); break;
	case D3DFMT_A8:       fi.gcm = GCM_TEXTURE_FORMAT_B8; fi.bpp = 1; fi.remap = Remap(B, B, B, B, GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_ZERO); break;
	case D3DFMT_DXT1:     fi.gcm = GCM_TEXTURE_FORMAT_DXT1; fi.compressed = true; fi.bpp = 8; break;
	case D3DFMT_DXT2: case D3DFMT_DXT3: fi.gcm = GCM_TEXTURE_FORMAT_DXT23; fi.compressed = true; fi.bpp = 16; break;
	case D3DFMT_DXT4: case D3DFMT_DXT5: fi.gcm = GCM_TEXTURE_FORMAT_DXT45; fi.compressed = true; fi.bpp = 16; break;
	default: fi.ok = false; break;
	}
	return fi;
}

static RsxTexture* RenderTargetTexture(IDirect3DTexture9* t, RsxTexture* r);

// Makes the RSX copy of a texture current. Returns NULL if it can't be drawn.
static RsxTexture* Texture(IDirect3DTexture9* t)
{
	RsxTexture* r = (RsxTexture*)t->m_backend;
	if (r && r->valid && r->renderTarget && t->m_dirty)
	{
		// Its pixels live in RSX memory, drawn by the GPU; the CPU-side copy a LockRect
		// changed isn't uploaded over them (no read-back either). Say so once.
		LogLimited("rtlock", 1, "a render-target texture was locked and written by the CPU: not shown");
		t->m_dirty = false;
	}
	if (r && r->valid && (!t->m_dirty || r->renderTarget)) return r;
	if (!r) { r = new RsxTexture; memset(r, 0, sizeof(*r)); t->m_backend = r; }
	if (IsRenderTarget(t)) return RenderTargetTexture(t, r);

	FormatInfo fi = MapFormat(t->m_format);
	if (!fi.ok)
	{
		char key[32];
		snprintf(key, sizeof(key), "fmt%d", (int)t->m_format);
		LogLimited(key, 1, "texture format %d is not supported yet (drawn white)", (int)t->m_format);
		r->valid = false;
		t->m_dirty = false;
		return NULL;
	}

	const u32 w = t->m_width, h = t->m_height;
	const bool pot = IsPow2(w) && IsPow2(h);
	const bool swizzle = !fi.compressed && pot && w >= 16 && h >= 16 && !s_forceLinear;
	const u32 levels = (swizzle || (fi.compressed && pot && !s_forceLinear)) ? t->m_levels : 1;
	const u32 linearPitch = fi.compressed ? ((w + 3) / 4) * fi.bpp : ((w * fi.bpp + 63) & ~63u);

	// Level sizes, packed one after another.
	u32 total = 0, sizes[16];
	for (u32 l = 0; l < levels; ++l)
	{
		u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
		if (fi.compressed) sizes[l] = ((lw + 3) / 4) * ((lh + 3) / 4) * fi.bpp;
		else if (swizzle) sizes[l] = lw * lh * fi.bpp;
		else sizes[l] = linearPitch * lh;
		total += sizes[l];
	}

	// Never rewrite memory the RSX may still be sampling: a texture used in the last few
	// frames gets new memory, and the old is freed once those frames are done.
	if (r->mem && (r->bytes < total || GpuMayUse(r->usedPlusOne)))
	{
		DeferFree(r->mem);
		r->mem = NULL;
	}
	if (!r->mem)
	{
		r->mem = (u8*)LocalAlloc(total, 128, &r->offset);
		r->bytes = r->mem ? total : 0;
		if (!r->mem)
		{
			LogLimited("texmem", 5, "out of RSX memory for a %ux%u texture", w, h);
			r->valid = false;
			return NULL;
		}
	}

	// Build the RSX layout in main memory, then one copy (scattered writes to RSX memory are slow).
	// malloc, not new: a big temporary stays out of the game's heaps (ps3_new.cpp).
	u8* staging = (u8*)malloc(total ? total : 1);
	if (!staging) return NULL;
	u32 at = 0;
	for (u32 l = 0; l < levels; ++l)
	{
		u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
		const u8* src = t->LevelBits(l);
		const u32 srcPitch = t->LevelPitch(l);
		u8* dst = staging + at;
		if (fi.compressed)
		{
			u32 rowBytes = ((lw + 3) / 4) * fi.bpp, rows = (lh + 3) / 4;
			for (u32 y = 0; y < rows; ++y) memcpy(dst + y * rowBytes, src + y * srcPitch, rowBytes);
		}
		else if (swizzle)
		{
			std::vector<u32> xt, yt;
			SwizzleTables(lw, lh, xt, yt);
			for (u32 y = 0; y < lh; ++y)
				for (u32 x = 0; x < lw; ++x)
					memcpy(dst + (xt[x] | yt[y]) * fi.bpp, src + y * srcPitch + x * fi.bpp, fi.bpp);
		}
		else
		{
			for (u32 y = 0; y < lh; ++y) memcpy(dst + y * linearPitch, src + y * srcPitch, lw * fi.bpp);
		}
		at += sizes[l];
	}
	memcpy(r->mem, staging, total);
	free(staging);
	// The texture cache may hold texels of whatever used this memory before; and any unit
	// bound to this texture needs its new offset and format.
	CmdReserve(32);
	rsxInvalidateTextureCache(s_ctx, GCM_INVALIDATE_TEXTURE);
	for (int u = 0; u < 4; ++u) if (s_bound[u] == r) s_bound[u] = NULL;

	memset(&r->desc, 0, sizeof(r->desc));
	r->desc.format = fi.gcm | ((swizzle || (fi.compressed && levels > 1)) ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
	r->desc.mipmap = levels;
	r->desc.dimension = GCM_TEXTURE_DIMS_2D;
	r->desc.cubemap = GCM_FALSE;
	r->desc.remap = fi.remap;
	r->desc.width = w;
	r->desc.height = h;
	r->desc.depth = 1;
	r->desc.location = GCM_LOCATION_RSX;
	r->desc.pitch = swizzle ? 0 : linearPitch;
	r->desc.offset = r->offset;
	r->valid = true;
	t->m_dirty = false;
	++s_stats.uploads;
	s_stats.uploadKB += total / 1024;
	return r;
}

// A texture the game draws into: a linear A8R8G8B8 (or Z24S8) buffer in RSX memory, sampled
// as a linear texture. Its first level only.
static RsxTexture* RenderTargetTexture(IDirect3DTexture9* t, RsxTexture* r)
{
	const u32 w = t->m_width, h = t->m_height;
	r->depth = IsDepthFormat(t->m_format);
	r->pitch = (w * 4 + 63) & ~63u;
	if (!r->mem)
	{
		r->mem = (u8*)LocalAlloc(r->pitch * h, 4096, &r->offset);
		if (!r->mem) { LogLimited("rtmem", 5, "out of RSX memory for a %ux%u render target", w, h); return NULL; }
		r->bytes = r->pitch * h;
		memset(r->mem, 0, r->bytes);
	}
	memset(&r->desc, 0, sizeof(r->desc));
	r->desc.format = (r->depth ? GCM_TEXTURE_FORMAT_DEPTH24_D8 : GCM_TEXTURE_FORMAT_A8R8G8B8) | GCM_TEXTURE_FORMAT_LIN;
	r->desc.mipmap = 1;
	r->desc.dimension = GCM_TEXTURE_DIMS_2D;
	r->desc.remap = IdentityRemap();
	r->desc.width = w;
	r->desc.height = h;
	r->desc.depth = 1;
	r->desc.location = GCM_LOCATION_RSX;
	r->desc.pitch = r->pitch;
	r->desc.offset = r->offset;
	r->renderTarget = true;
	r->valid = true;
	t->m_dirty = false;
	return r;
}

static bool MakeWhite()
{
	const u32 w = 16, h = 16, pitch = 64;
	s_white.mem = (u8*)LocalAlloc(pitch * h, 128, &s_white.offset);
	if (!s_white.mem) return false;
	memset(s_white.mem, 0xFF, pitch * h);
	memset(&s_white.desc, 0, sizeof(s_white.desc));
	s_white.desc.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	s_white.desc.mipmap = 1;
	s_white.desc.dimension = GCM_TEXTURE_DIMS_2D;
	s_white.desc.remap = IdentityRemap();
	s_white.desc.width = w;
	s_white.desc.height = h;
	s_white.desc.depth = 1;
	s_white.desc.location = GCM_LOCATION_RSX;
	s_white.desc.pitch = pitch;
	s_white.desc.offset = s_white.offset;
	s_white.valid = true;
	return true;
}

static u8 Wrap(DWORD a)
{
	switch (a)
	{
	case D3DTADDRESS_MIRROR: return GCM_TEXTURE_MIRRORED_REPEAT;
	case D3DTADDRESS_CLAMP: return GCM_TEXTURE_CLAMP_TO_EDGE;
	case D3DTADDRESS_BORDER: return GCM_TEXTURE_BORDER;
	case D3DTADDRESS_MIRRORONCE: return GCM_TEXTURE_MIRROR_ONCE_CLAMP_TO_EDGE;
	default: return GCM_TEXTURE_REPEAT;
	}
}

static void BindTexture(IDirect3DDevice9* dev, int unit, const RsxTexture* r)
{
	const_cast<RsxTexture*>(r)->usedPlusOne = s_frameSeq + 1;
	const DWORD* ss = dev->m_ss[unit];
	DWORD key[4] = { ss[D3DSAMP_MINFILTER] | (ss[D3DSAMP_MAGFILTER] << 8) | (ss[D3DSAMP_MIPFILTER] << 16),
	                 ss[D3DSAMP_ADDRESSU] | (ss[D3DSAMP_ADDRESSV] << 8), ss[D3DSAMP_BORDERCOLOR], ss[D3DSAMP_MAXANISOTROPY] };
	if (s_bound[unit] == r && !memcmp(s_boundSampler[unit], key, sizeof(key))) return;
	s_bound[unit] = r;
	memcpy(s_boundSampler[unit], key, sizeof(key));

	const u32 levels = r->desc.mipmap;
	const DWORD minF = ss[D3DSAMP_MINFILTER], magF = ss[D3DSAMP_MAGFILTER], mipF = levels > 1 ? ss[D3DSAMP_MIPFILTER] : D3DTEXF_NONE;
	const bool minLinear = minF != D3DTEXF_POINT && minF != D3DTEXF_NONE;
	u8 min;
	if (mipF == D3DTEXF_NONE) min = minLinear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
	else if (mipF == D3DTEXF_POINT) min = minLinear ? GCM_TEXTURE_LINEAR_MIPMAP_NEAREST : GCM_TEXTURE_NEAREST_MIPMAP_NEAREST;
	else min = minLinear ? GCM_TEXTURE_LINEAR_MIPMAP_LINEAR : GCM_TEXTURE_NEAREST_MIPMAP_LINEAR;
	const u8 mag = (magF == D3DTEXF_POINT || magF == D3DTEXF_NONE) ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
	const u8 aniso = (minF == D3DTEXF_ANISOTROPIC && levels > 1) ? GCM_TEXTURE_MAX_ANISO_4 : GCM_TEXTURE_MAX_ANISO_1;

	CmdReserve(128);
	rsxLoadTexture(s_ctx, unit, &r->desc);
	rsxTextureControl(s_ctx, unit, GCM_TRUE, 0 << 8, (levels - 1) << 8, aniso);
	rsxTextureFilter(s_ctx, unit, 0, min, mag, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(s_ctx, unit, Wrap(ss[D3DSAMP_ADDRESSU]), Wrap(ss[D3DSAMP_ADDRESSV]), GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
	rsxTextureBorderColor(s_ctx, unit, ss[D3DSAMP_BORDERCOLOR]);
}

// ---------------------------------------------------------------------------
// Vertex and index buffers
// ---------------------------------------------------------------------------
struct RsxBuffer
{
	u8* mem;
	u32 offset, size;
	u32 persistentUsed;	// frame of the last draw that read the persistent copy (b->mem), +1 (0: never)
	u32 ringFrame, ringOffset;
	bool stale;			// the newest data is only in this frame's ring copy
};

// The RSX offset of a buffer's data for a draw in this frame, uploading as needed.
// changes says what the CPU wrote since the last upload (Ps3BufferChanges in d3d9.h).
static bool BufferOffset(const BYTE* data, u32 size, bool& dirty, const Ps3BufferChanges& changes, void*& backend, u32* offset)
{
	RsxBuffer* b = (RsxBuffer*)backend;
	if (!b) { b = new RsxBuffer; memset(b, 0, sizeof(*b)); backend = b; }
	const u32 now = s_frameSeq;
	const bool idle = !GpuMayUse(b->persistentUsed);	// the RSX is done with the persistent copy

	if (b->mem && b->size < size)
	{
		DeferFree(b->mem);	// grown: the old copy goes once the RSX is done with it
		b->mem = NULL;
	}
	if (!b->mem)
	{
		b->mem = (u8*)LocalAlloc(size ? size : 16, 128, &b->offset);
		if (!b->mem) { LogLimited("bufmem", 5, "out of RSX memory for a %u-byte buffer", size); return false; }
		b->size = size;
		b->persistentUsed = 0;
		memcpy(b->mem, data, size);
		b->stale = false;
		dirty = false;
		CmdReserve(32);
		rsxInvalidateVertexCache(s_ctx);
	}

	if (dirty)
	{
		if (idle || (changes.noOverwrite && !b->stale))
		{
			// Written in place: either the RSX is done with it, or the game locked with
			// D3DLOCK_NOOVERWRITE, promising not to touch what pending draws use (dynamic
			// buffers appended to this way would otherwise go through the ring whole).
			u32 lo = changes.lo, hi = changes.hi > size ? size : changes.hi;
			if (b->stale) { lo = 0; hi = size; }	// (idle) the persistent copy is behind: refresh it all
			if (hi > lo) memcpy(b->mem + lo, data + lo, hi - lo);
			b->stale = false;
			CmdReserve(32);
			rsxInvalidateVertexCache(s_ctx);
		}
		else
		{
			// Rewritten while the RSX may still read the old data: this frame uses a ring copy.
			u32 off;
			u8* p = RingAlloc(size, 128, &off);
			if (!p) return false;
			memcpy(p, data, size);
			b->ringFrame = now;
			b->ringOffset = off;
			b->stale = true;
			s_stats.ringKB += size / 1024;
		}
		dirty = false;
	}
	else if (b->stale && b->ringFrame != now)
	{
		if (idle)
		{
			memcpy(b->mem, data, size);	// catch the persistent copy up once the RSX is done with it
			b->stale = false;
			CmdReserve(32);
			rsxInvalidateVertexCache(s_ctx);
		}
		else
		{
			u32 off;
			u8* p = RingAlloc(size, 128, &off);
			if (!p) return false;
			memcpy(p, data, size);
			b->ringFrame = now;
			b->ringOffset = off;
			s_stats.ringKB += size / 1024;
		}
	}

	if (b->stale)
		*offset = b->ringOffset;
	else
	{
		*offset = b->offset;
		b->persistentUsed = now + 1;	// only uses of the persistent copy keep it busy
	}
	return true;
}

// ---------------------------------------------------------------------------
// Programs
// ---------------------------------------------------------------------------
struct VpEntry
{
	std::vector<unsigned char> blob;
	rsxVertexProgram* prog;
	void* ucode;
};

struct FpEntry
{
	std::vector<unsigned char> blob;
	rsxFragmentProgram* prog;
	u8* ucode;			// in the blob
	u32 ucodeSize;
	u8* local;			// persistent copy in RSX memory (default constants)
	u32 localOffset;
	struct Const { int reg; std::vector<u32> offsets; };
	std::vector<Const> consts;
	float last[9][4];	// constant values in the last patched copy
	u32 lastFrame, lastOffset;
	int texMask;		// stages it samples
};

static std::map<std::string, VpEntry*> s_vps;
static std::map<std::string, FpEntry*> s_fps;
static const VpEntry* s_currentVp;
static u32 s_currentFpOffset = 0xFFFFFFFF;
static float s_vc[kVcCount][4];
static bool s_vcValid[kVcCount];

static VpEntry* VertexProgramFor(const VsKey& key)
{
	std::string k((const char*)&key, sizeof(key));
	std::map<std::string, VpEntry*>::iterator it = s_vps.find(k);
	if (it != s_vps.end()) return it->second;

	VpEntry* e = new VpEntry;
	e->prog = NULL;
	e->ucode = NULL;
	std::string src = VertexProgram(key), err;
	if (RsxAsm::AssembleVertexProgram(src.c_str(), e->blob, &err))
	{
		e->prog = (rsxVertexProgram*)&e->blob[0];
		u32 size;
		rsxVertexProgramGetUCode(e->prog, &e->ucode, &size);
		++s_stats.programs;
	}
	else
	{
		++s_stats.programFailures;
		GpuLog("vertex program failed (%s):\n%s", err.c_str(), src.c_str());
	}
	s_vps[k] = e;
	return e;
}

static inline u32 SwapHalves(float f)
{
	u32 u;
	memcpy(&u, &f, 4);
	return (u >> 16) | (u << 16);
}

static FpEntry* FragmentProgramFrom(const std::string& key, const std::string& src, int texMask)
{
	std::map<std::string, FpEntry*>::iterator it = s_fps.find(key);
	if (it != s_fps.end()) return it->second;

	FpEntry* e = new FpEntry;
	memset(e->last, 0, sizeof(e->last));
	e->prog = NULL;
	e->local = NULL;
	e->lastFrame = 0xFFFFFFFF;
	e->texMask = texMask;
	std::string err;
	if (!src.empty() && RsxAsm::AssembleFragmentProgram(src.c_str(), e->blob, &err))
	{
		e->prog = (rsxFragmentProgram*)&e->blob[0];
		void* ucode;
		rsxFragmentProgramGetUCode(e->prog, &ucode, &e->ucodeSize);
		e->ucode = (u8*)ucode;
		e->local = (u8*)LocalAlloc(e->ucodeSize, 64, &e->localOffset);
		if (e->local) memcpy(e->local, e->ucode, e->ucodeSize);
		else e->prog = NULL;

		// Where each named constant (c0, c1...) sits in the code.
		const u8* base = &e->blob[0];
		const rsxProgramConst* consts = (const rsxProgramConst*)(base + e->prog->const_off);
		for (int i = 0; i < e->prog->num_const; ++i)
		{
			if (consts[i].is_internal || consts[i].index == 0xFFFFFFFF || !consts[i].name_off) continue;
			const char* name = (const char*)(base + consts[i].name_off);
			if (name[0] != 'c') continue;
			FpEntry::Const c;
			c.reg = atoi(name + 1);
			const rsxConstOffsetTable* table = (const rsxConstOffsetTable*)(base + consts[i].index);
			for (u32 k = 0; k < table->num; ++k) c.offsets.push_back(table->offset[k]);
			if (c.reg >= 0 && c.reg < 9 && !c.offsets.empty()) e->consts.push_back(c);
		}
		++s_stats.programs;
	}
	else
	{
		++s_stats.programFailures;
		GpuLog("fragment program failed (%s):\n%s", err.c_str(), src.c_str());
	}
	s_fps[key] = e;
	return e;
}

// Loads a fragment program with the given constants (c0..c8).
static bool UseFragmentProgram(FpEntry* e, const float (*values)[4])
{
	u32 offset = e->localOffset;
	if (!e->consts.empty())
	{
		bool same = e->lastFrame == s_frameSeq;
		for (size_t i = 0; same && i < e->consts.size(); ++i)
			same = !memcmp(e->last[e->consts[i].reg], values[e->consts[i].reg], 16);
		if (same)
			offset = e->lastOffset;
		else
		{
			u8* p = RingAlloc(e->ucodeSize, 64, &offset);
			if (!p) return false;
			memcpy(p, e->ucode, e->ucodeSize);
			for (size_t i = 0; i < e->consts.size(); ++i)
			{
				const float* v = values[e->consts[i].reg];
				u32 words[4] = { SwapHalves(v[0]), SwapHalves(v[1]), SwapHalves(v[2]), SwapHalves(v[3]) };
				for (size_t k = 0; k < e->consts[i].offsets.size(); ++k)
					memcpy(p + e->consts[i].offsets[k], words, 16);
				memcpy(e->last[e->consts[i].reg], v, 16);
			}
			e->lastFrame = s_frameSeq;
			e->lastOffset = offset;
		}
	}
	if (offset != s_currentFpOffset)
	{
		CmdReserve(64);
		rsxLoadFragmentProgramLocation(s_ctx, e->prog, offset, GCM_LOCATION_RSX);
		s_currentFpOffset = offset;
	}
	return true;
}

static void UseVertexProgram(const VpEntry* e, const float (*c)[4])
{
	if (e != s_currentVp)
	{
		CmdReserve(64 + e->prog->num_insn * 6);
		rsxLoadVertexProgram(s_ctx, e->prog, e->ucode);
		s_currentVp = e;
	}
	// Constants that changed, in runs.
	for (int i = 0; i < kVcCount;)
	{
		if (s_vcValid[i] && !memcmp(s_vc[i], c[i], 16)) { ++i; continue; }
		int j = i;
		while (j < kVcCount && !(s_vcValid[j] && !memcmp(s_vc[j], c[j], 16))) { memcpy(s_vc[j], c[j], 16); s_vcValid[j] = true; ++j; }
		CmdReserve(16 + (j - i) * 5);
		rsxLoadVertexProgramParameterBlock(s_ctx, i, j - i, &c[i][0]);
		i = j;
	}
}

// ---------------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------------
struct GpuState
{
	u32 blendEnable, srcBlend, dstBlend, blendOp, blendColor;
	u32 alphaTest, alphaFunc, alphaRef;
	u32 depthTest, depthWrite, depthFunc;
	u32 cullEnable, cullFace;
	u32 colorMask;
	u32 offsetEnable;
	float offsetFactor, offsetUnits;
	u32 shade;
};
static GpuState s_state;
static bool s_stateValid;

struct ViewState { D3DVIEWPORT9 vp; RECT scissor; u32 scissorOn; };
static ViewState s_view;
static bool s_viewValid;

static u16 Blend(DWORD b, bool alpha)
{
	switch (b)
	{
	case D3DBLEND_ZERO: return GCM_ZERO;
	case D3DBLEND_ONE: return GCM_ONE;
	case D3DBLEND_SRCCOLOR: return alpha ? GCM_SRC_ALPHA : GCM_SRC_COLOR;
	case D3DBLEND_INVSRCCOLOR: return alpha ? GCM_ONE_MINUS_SRC_ALPHA : GCM_ONE_MINUS_SRC_COLOR;
	case D3DBLEND_SRCALPHA: return GCM_SRC_ALPHA;
	case D3DBLEND_INVSRCALPHA: return GCM_ONE_MINUS_SRC_ALPHA;
	case D3DBLEND_DESTALPHA: return GCM_DST_ALPHA;
	case D3DBLEND_INVDESTALPHA: return GCM_ONE_MINUS_DST_ALPHA;
	case D3DBLEND_DESTCOLOR: return alpha ? GCM_DST_ALPHA : GCM_DST_COLOR;
	case D3DBLEND_INVDESTCOLOR: return alpha ? GCM_ONE_MINUS_DST_ALPHA : GCM_ONE_MINUS_DST_COLOR;
	case D3DBLEND_SRCALPHASAT: return GCM_SRC_ALPHA_SATURATE;
	case D3DBLEND_BLENDFACTOR: return alpha ? GCM_CONSTANT_ALPHA : GCM_CONSTANT_COLOR;
	case D3DBLEND_INVBLENDFACTOR: return alpha ? GCM_ONE_MINUS_CONSTANT_ALPHA : GCM_ONE_MINUS_CONSTANT_COLOR;
	default: return GCM_ONE;
	}
}

static u32 Compare(DWORD f)
{
	switch (f)
	{
	case D3DCMP_NEVER: return GCM_NEVER;
	case D3DCMP_LESS: return GCM_LESS;
	case D3DCMP_EQUAL: return GCM_EQUAL;
	case D3DCMP_LESSEQUAL: return GCM_LEQUAL;
	case D3DCMP_GREATER: return GCM_GREATER;
	case D3DCMP_NOTEQUAL: return GCM_NOTEQUAL;
	case D3DCMP_GREATEREQUAL: return GCM_GEQUAL;
	default: return GCM_ALWAYS;
	}
}

static void ApplyState(IDirect3DDevice9* dev, bool hasDepth)
{
	const DWORD* rs = dev->m_rs;
	GpuState s;
	memset(&s, 0, sizeof(s));
	s.blendEnable = rs[D3DRS_ALPHABLENDENABLE] != 0;
	s.srcBlend = rs[D3DRS_SRCBLEND];
	s.dstBlend = rs[D3DRS_DESTBLEND];
	s.blendOp = rs[D3DRS_BLENDOP];
	s.blendColor = rs[D3DRS_BLENDFACTOR];
	s.alphaTest = rs[D3DRS_ALPHATESTENABLE] != 0;
	s.alphaFunc = rs[D3DRS_ALPHAFUNC];
	s.alphaRef = rs[D3DRS_ALPHAREF] & 0xFF;
	s.depthTest = rs[D3DRS_ZENABLE] != D3DZB_FALSE && hasDepth;
	s.depthWrite = s.depthTest && rs[D3DRS_ZWRITEENABLE];
	s.depthFunc = rs[D3DRS_ZFUNC];
	s.cullEnable = rs[D3DRS_CULLMODE] != D3DCULL_NONE;
	s.cullFace = rs[D3DRS_CULLMODE];
	s.colorMask = rs[D3DRS_COLORWRITEENABLE];
	float bias = AsFloat(rs[D3DRS_DEPTHBIAS]), slope = AsFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
	s.offsetEnable = bias != 0.0f || slope != 0.0f;
	s.offsetFactor = slope;
	s.offsetUnits = bias * 16777215.0f;
	s.shade = rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT ? GCM_SHADE_MODEL_FLAT : GCM_SHADE_MODEL_SMOOTH;

	if (s_stateValid && !memcmp(&s, &s_state, sizeof(s))) return;
	s_state = s;
	s_stateValid = true;

	CmdReserve(160);
	rsxSetBlendEnable(s_ctx, s.blendEnable ? GCM_TRUE : GCM_FALSE);
	if (s.blendEnable)
	{
		u16 sc = Blend(s.srcBlend, false), dc = Blend(s.dstBlend, false), sa = Blend(s.srcBlend, true), da = Blend(s.dstBlend, true);
		if (s.srcBlend == D3DBLEND_BOTHSRCALPHA) { sc = sa = GCM_SRC_ALPHA; dc = da = GCM_ONE_MINUS_SRC_ALPHA; }
		if (s.srcBlend == D3DBLEND_BOTHINVSRCALPHA) { sc = sa = GCM_ONE_MINUS_SRC_ALPHA; dc = da = GCM_SRC_ALPHA; }
		rsxSetBlendFunc(s_ctx, sc, dc, sa, da);
		u16 eq = GCM_FUNC_ADD;
		if (s.blendOp == D3DBLENDOP_SUBTRACT) eq = GCM_FUNC_SUBTRACT;
		else if (s.blendOp == D3DBLENDOP_REVSUBTRACT) eq = GCM_FUNC_REVERSE_SUBTRACT;
		else if (s.blendOp == D3DBLENDOP_MIN) eq = GCM_FUNC_MIN;
		else if (s.blendOp == D3DBLENDOP_MAX) eq = GCM_FUNC_MAX;
		rsxSetBlendEquation(s_ctx, eq, eq);
		rsxSetBlendColor(s_ctx, s.blendColor, 0);
	}
	rsxSetAlphaTestEnable(s_ctx, s.alphaTest ? GCM_TRUE : GCM_FALSE);
	rsxSetAlphaFunc(s_ctx, Compare(s.alphaFunc), s.alphaRef);
	rsxSetDepthTestEnable(s_ctx, s.depthTest ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthWriteEnable(s_ctx, s.depthWrite ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthFunc(s_ctx, Compare(s.depthFunc));
	// D3D culls by screen-space winding (y down); the viewport flips y, so a triangle D3D
	// calls clockwise is counter-clockwise to the RSX. D3DCULL_CCW (the default) culls
	// what the RSX sees as clockwise: front face CCW, cull back.
	rsxSetCullFaceEnable(s_ctx, s.cullEnable ? GCM_TRUE : GCM_FALSE);
	rsxSetFrontFace(s_ctx, GCM_FRONTFACE_CCW);
	bool cullBack = s.cullFace == D3DCULL_CCW;
	if (s_flipCull) cullBack = !cullBack;
	rsxSetCullFace(s_ctx, cullBack ? GCM_CULL_BACK : GCM_CULL_FRONT);
	u32 mask = 0;
	if (s.colorMask & D3DCOLORWRITEENABLE_RED) mask |= GCM_COLOR_MASK_R;
	if (s.colorMask & D3DCOLORWRITEENABLE_GREEN) mask |= GCM_COLOR_MASK_G;
	if (s.colorMask & D3DCOLORWRITEENABLE_BLUE) mask |= GCM_COLOR_MASK_B;
	if (s.colorMask & D3DCOLORWRITEENABLE_ALPHA) mask |= GCM_COLOR_MASK_A;
	rsxSetColorMask(s_ctx, mask);
	rsxSetPolygonOffsetFillEnable(s_ctx, s.offsetEnable ? GCM_TRUE : GCM_FALSE);
	if (s.offsetEnable) rsxSetPolygonOffset(s_ctx, s.offsetFactor, s.offsetUnits);
	rsxSetShadeModel(s_ctx, s.shade);
}

static void ApplyView(IDirect3DDevice9* dev)
{
	ViewState v;
	memset(&v, 0, sizeof(v));
	v.vp = dev->m_viewport;
	v.scissor = dev->m_scissor;
	v.scissorOn = dev->m_rs[D3DRS_SCISSORTESTENABLE] != 0;
	if (s_viewValid && !memcmp(&v, &s_view, sizeof(v))) return;
	s_view = v;
	s_viewValid = true;

	const D3DVIEWPORT9& vp = v.vp;
	// GL-style depth (-1..1 -> MinZ..MaxZ), y flipped, plus D3D9's half-pixel offset.
	float scale[4], offset[4];
	ViewportTransform(vp, scale, offset);
	int x = vp.X, y = vp.Y, w = vp.Width, h = vp.Height;
	if (v.scissorOn)
	{
		int x2 = x + w, y2 = y + h;
		if (v.scissor.left > x) x = v.scissor.left;
		if (v.scissor.top > y) y = v.scissor.top;
		if (v.scissor.right < x2) x2 = v.scissor.right;
		if (v.scissor.bottom < y2) y2 = v.scissor.bottom;
		w = x2 > x ? x2 - x : 0;
		h = y2 > y ? y2 - y : 0;
	}
	CmdReserve(96);
	rsxSetViewport(s_ctx, vp.X, vp.Y, vp.Width, vp.Height, 0.0f, 1.0f, scale, offset);
	for (int i = 0; i < 8; ++i) rsxSetViewportClip(s_ctx, i, 4096, 4096);
	rsxSetScissor(s_ctx, x, y, w, h);
}

// ---------------------------------------------------------------------------
// Draws
// ---------------------------------------------------------------------------
static u32 s_attribMask;	// vertex attributes bound by the last draw

static u8 PrimType(D3DPRIMITIVETYPE t, bool* ok)
{
	*ok = true;
	switch (t)
	{
	case D3DPT_LINELIST: return GCM_TYPE_LINES;
	case D3DPT_LINESTRIP: return GCM_TYPE_LINE_STRIP;
	case D3DPT_TRIANGLELIST: return GCM_TYPE_TRIANGLES;
	case D3DPT_TRIANGLESTRIP: return GCM_TYPE_TRIANGLE_STRIP;
	case D3DPT_TRIANGLEFAN: return GCM_TYPE_TRIANGLE_FAN;
	case D3DPT_QUADLIST: return GCM_TYPE_QUADS;
	default: *ok = false; return 0;	// point lists (sprites) come later
	}
}

static u32 VertexCount(D3DPRIMITIVETYPE type, u32 prims)
{
	switch (type)
	{
	case D3DPT_LINELIST: return prims * 2;
	case D3DPT_LINESTRIP: return prims + 1;
	case D3DPT_TRIANGLELIST: return prims * 3;
	case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return prims + 2;
	case D3DPT_QUADLIST: return prims * 4;
	default: return prims;
	}
}

static void BindAttributes(const FvfLayout& l, u32 base, u32 stride)
{
	u32 mask = 0;
	CmdReserve(256);
	rsxBindVertexArrayAttrib(s_ctx, kAttrPos, 0, base + l.pos, stride, l.posSize, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	mask |= 1u << kAttrPos;
	if (l.normal >= 0) { rsxBindVertexArrayAttrib(s_ctx, kAttrNormal, 0, base + l.normal, stride, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX); mask |= 1u << kAttrNormal; }
	if (l.diffuse >= 0) { rsxBindVertexArrayAttrib(s_ctx, kAttrDiffuse, 0, base + l.diffuse, stride, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX); mask |= 1u << kAttrDiffuse; }
	if (l.specular >= 0) { rsxBindVertexArrayAttrib(s_ctx, kAttrSpecular, 0, base + l.specular, stride, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX); mask |= 1u << kAttrSpecular; }
	for (int i = 0; i < l.numTex; ++i)
	{
		rsxBindVertexArrayAttrib(s_ctx, kAttrTex0 + i, 0, base + l.tex[i], stride, l.texSize[i], GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		mask |= 1u << (kAttrTex0 + i);
	}
	// Everything else off (as the re3 / reVC PS3 ports and Grid Siege 3D do).
	for (int a = 0; a < 16; ++a)
		if (!(mask & (1u << a)) && (s_attribMask & (1u << a) || s_attribMask == 0xFFFFFFFF))
			rsxBindVertexArrayAttrib(s_ctx, a, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	s_attribMask = mask;
	rsxInvalidateVertexCache(s_ctx);
}

static bool Skip(const char* why)
{
	++s_stats.skipped;
	LogLimited(why, 3, "draw skipped: %s", why);
	return false;
}

static bool DoDraw(IDirect3DDevice9* dev, const DrawCall& call)
{
	if (s_noDraw) return false;
	bool hasDepth = false;
	if (!ApplyTarget(dev, &hasDepth)) return false;
	if (dev->m_vs) return Skip("vertex shaders are not supported (the game uses fixed-function)");
	if (!dev->m_fvf) return Skip("no FVF (vertex declarations are not supported)");
	bool typeOk;
	const u8 type = PrimType(call.type, &typeOk);
	if (!typeOk) return Skip("point lists");

	const FvfLayout l = Layout(dev->m_fvf);
	const u32 count = call.indexed ? VertexCount(call.type, call.primCount) : call.vertexCount;
	if (count == 0) return false;

	// Textures for the fragment program.
	const RsxTexture* tex[4] = { NULL, NULL, NULL, NULL };
	for (int s = 0; s < 4; ++s)
	{
		IDirect3DBaseTexture9* bt = dev->m_textures[s];
		if (bt && bt->GetType() == D3DRTYPE_TEXTURE)
			tex[s] = Texture(static_cast<IDirect3DTexture9*>(bt));
	}

	// Fragment program and its constants.
	FpEntry* fp = NULL;
	float fc[9][4];
	memset(fc, 0, sizeof(fc));
	const bool fog = FogEnabled(dev);
	int numTexOut = 0;
	if (dev->m_ps)
	{
		const char* src = PixelShaderSource(dev->m_ps);
		if (!src) return Skip("pixel shader without source");
		char key[48];
		snprintf(key, sizeof(key), "ps:%p:%d", (void*)dev->m_ps, fog ? 1 : 0);
		std::map<std::string, FpEntry*>::iterator it = s_fps.find(key);
		if (it != s_fps.end()) fp = it->second;
		else
		{
			std::string err, prog = PixelShaderProgram(src, fog, &err);
			if (prog.empty()) GpuLog("pixel shader not translated (%s):\n%s", err.c_str(), src);
			int mask = 0;
			for (int t = 0; t < 4; ++t)
			{
				char want[16];
				snprintf(want, sizeof(want), "texture[%d]", t);
				if (prog.find(want) != std::string::npos) mask |= 1 << t;
			}
			fp = FragmentProgramFrom(key, prog, mask);
		}
		for (int i = 0; i < 8; ++i) memcpy(fc[i], dev->m_psConst[i], 16);
		ColorToFloats(dev->m_rs[D3DRS_FOGCOLOR], fc[kFcPsFogColor]);
	}
	else
	{
		FsKey fk;
		const bool has[4] = { tex[0] != NULL, tex[1] != NULL, tex[2] != NULL, tex[3] != NULL };
		FillFsKey(dev, fk, has);
		std::string key = "ff:" + std::string((const char*)&fk, sizeof(fk));
		std::map<std::string, FpEntry*>::iterator it = s_fps.find(key);
		if (it != s_fps.end()) fp = it->second;
		else
		{
			int mask = 0;
			for (int s = 0; s < fk.numStages; ++s) if (fk.stage[s].hasTexture) mask |= 1 << s;
			fp = FragmentProgramFrom(key, FixedFunctionFragmentProgram(fk), mask);
		}
		ColorToFloats(dev->m_rs[D3DRS_TEXTUREFACTOR], fc[kFcTextureFactor]);
		ColorToFloats(dev->m_rs[D3DRS_FOGCOLOR], fc[kFcFogColor]);
	}
	if (!fp || !fp->prog) return Skip("fragment program unavailable");
	for (int t = 0; t < 4; ++t) if (fp->texMask & (1 << t)) numTexOut = t + 1;
	if (!dev->m_ps)
	{
		FsKey fk;
		const bool has[4] = { tex[0] != NULL, tex[1] != NULL, tex[2] != NULL, tex[3] != NULL };
		FillFsKey(dev, fk, has);
		if (fk.numStages > numTexOut) numTexOut = fk.numStages;
	}

	// Vertex program.
	VsKey vk;
	FillVsKey(dev, l, numTexOut, vk);
	VpEntry* vp = VertexProgramFor(vk);
	if (!vp->prog) return Skip("vertex program unavailable");

	// Vertex and index data on the RSX.
	u32 vbase, stride, ioff = 0;
	if (call.vertexData)
	{
		stride = call.vertexStride;
		const u32 verts = call.indexed ? call.minIndex + call.vertexCount : call.vertexCount;
		u8* p = RingAlloc(verts * stride, 128, &vbase);
		if (!p) return false;
		memcpy(p, call.vertexData, verts * stride);
		s_stats.ringKB += verts * stride / 1024;
		if (call.indexed)
		{
			const u32 isize = count * (call.indexFormat == D3DFMT_INDEX32 ? 4 : 2);
			u8* ip = RingAlloc(isize, 16, &ioff);
			if (!ip) return false;
			memcpy(ip, call.indexData, isize);
		}
	}
	else
	{
		IDirect3DVertexBuffer9* vb = dev->m_streams[0];
		if (!vb) return Skip("no vertex buffer");
		stride = dev->m_streamStride[0] ? dev->m_streamStride[0] : l.stride;
		u32 off;
		if (!BufferOffset(vb->m_data, vb->m_size, vb->m_dirty, vb->m_changes, vb->m_backend, &off)) return false;
		vbase = off + dev->m_streamOffset[0];
		if (call.indexed)
		{
			vbase += call.baseVertex * stride;
			IDirect3DIndexBuffer9* ib = dev->m_indices;
			if (!ib) return Skip("no index buffer");
			if (!BufferOffset(ib->m_data, ib->m_size, ib->m_dirty, ib->m_changes, ib->m_backend, &ioff)) return false;
			ioff += call.startIndex * (ib->m_format == D3DFMT_INDEX32 ? 4 : 2);
		}
		else
			vbase += call.startVertex * stride;
	}
	if (stride > 255) return Skip("vertex stride over 255 bytes");

	// State, programs, textures, then the draw.
	ApplyView(dev);
	ApplyState(dev, hasDepth);
	float vc[kVcCount][4];
	FillVsConstants(dev, vc);
	UseVertexProgram(vp, vc);
	if (!UseFragmentProgram(fp, fc)) return false;
	for (int t = 0; t < 4; ++t)
		if (fp->texMask & (1 << t))
			BindTexture(dev, t, tex[t] ? tex[t] : &s_white);
	BindAttributes(l, vbase, stride);

	CmdReserve(64 + count / 256 * 2);
	if (call.indexed)
	{
		const bool i32 = call.vertexData ? call.indexFormat == D3DFMT_INDEX32 : dev->m_indices->m_format == D3DFMT_INDEX32;
		rsxDrawIndexArray(s_ctx, type, ioff, count, i32 ? GCM_INDEX_TYPE_32B : GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
	}
	else
		rsxDrawVertexArray(s_ctx, type, 0, count);
	++s_stats.drawn;
	return true;
}

void Draw(IDirect3DDevice9* dev, const DrawCall& call)
{
	if (!s_drawReady || s_skipFrame) return;
	++s_stats.draws;
	DoDraw(dev, call);
}

// ---------------------------------------------------------------------------
// Set-up, frames, resource notifications
// ---------------------------------------------------------------------------
// Surfaces the game creates on their own (CreateRenderTarget, CreateDepthStencilSurface).
struct RsxSurface { u8* mem; u32 offset, pitch; };

static SurfaceDesc s_target;
static bool s_targetValid;

// RSX memory behind a D3D surface: a render-target texture's level 0, or a standalone surface.
static bool SurfaceMemory(IDirect3DSurface9* surf, u32* offset, u32* pitch)
{
	if (surf->m_owner)
	{
		if (surf->m_level != 0) return false;
		RsxTexture* r = Texture(surf->m_owner);
		if (!r || !r->renderTarget) return false;
		*offset = r->offset;
		*pitch = r->pitch;
		return true;
	}
	RsxSurface* rs = (RsxSurface*)surf->m_backend;
	if (!rs)
	{
		rs = new RsxSurface;
		rs->pitch = (surf->m_width * 4 + 63) & ~63u;
		rs->mem = (u8*)LocalAlloc(rs->pitch * surf->m_height, 4096, &rs->offset);
		if (!rs->mem) { delete rs; LogLimited("surfmem", 5, "out of RSX memory for a %ux%u surface", surf->m_width, surf->m_height); return false; }
		memset(rs->mem, 0, rs->pitch * surf->m_height);
		surf->m_backend = rs;
	}
	*offset = rs->offset;
	*pitch = rs->pitch;
	return true;
}

static void SetSurface(const SurfaceDesc& d)
{
	gcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = d.colorOffset;
	sf.colorPitch[0] = d.colorPitch;
	for (int i = 1; i < GCM_MAX_MRT_COUNT; ++i)
	{
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = d.colorOffset;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	if (d.hasDepth)
	{
		sf.depthOffset = d.depthOffset;
		sf.depthPitch = d.depthPitch;
	}
	else
	{
		// No depth target: depth testing and writes are off (ApplyState), but the surface
		// still names one, so point it at real memory (the display's depth buffer) rather
		// than offset 0, which is a colour buffer.
		SurfaceDesc disp;
		memset(&disp, 0, sizeof(disp));
		DisplaySurface(&disp);
		sf.depthOffset = disp.depthOffset;
		sf.depthPitch = disp.depthPitch;
	}
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = d.width;
	sf.height = d.height;
	CmdReserve(96);
	rsxSetSurface(s_ctx, &sf);
	// What was drawn into a texture before must be read fresh from now on.
	rsxInvalidateTextureCache(s_ctx, GCM_INVALIDATE_TEXTURE);
}

bool ApplyTarget(IDirect3DDevice9* dev, bool* hasDepth)
{
	SurfaceDesc d;
	memset(&d, 0, sizeof(d));	// compared with memcmp: no stray padding bytes
	IDirect3DSurface9* rt = dev->m_renderTarget;
	if (rt == dev->m_backBuffer || !rt)
		DisplaySurface(&d);
	else
	{
		memset(&d, 0, sizeof(d));
		if (!SurfaceMemory(rt, &d.colorOffset, &d.colorPitch))
		{
			++s_stats.offscreen;
			LogLimited("rtfail", 3, "render target %ux%u (format %d, level %u) can't be drawn into", rt->m_width, rt->m_height, (int)rt->m_format, rt->m_level);
			return false;
		}
		d.width = rt->m_width;
		d.height = rt->m_height;
		d.hasDepth = false;
	}

	IDirect3DSurface9* ds = dev->m_depthTarget;
	if (!ds)
		d.hasDepth = false;
	else if (ds == dev->m_depthBuffer)
	{
		SurfaceDesc disp;
		memset(&disp, 0, sizeof(disp));
		DisplaySurface(&disp);
		d.hasDepth = d.width <= disp.width && d.height <= disp.height;
		d.depthOffset = disp.depthOffset;
		d.depthPitch = disp.depthPitch;
	}
	else
	{
		d.hasDepth = ds->m_width >= d.width && ds->m_height >= d.height && SurfaceMemory(ds, &d.depthOffset, &d.depthPitch);
	}

	if (!s_targetValid || memcmp(&d, &s_target, sizeof(d)))
	{
		s_target = d;
		s_targetValid = true;
		SetSurface(d);
		s_viewValid = false;
	}
	if (hasDepth) *hasDepth = d.hasDepth;
	return true;
}

void InvalidateDrawState()
{
	s_stateValid = false;
	s_viewValid = false;
}

bool DrawInit()
{
	s_forceLinear = FlagFile("rsx_linear_textures.txt");
	s_flipCull = FlagFile("rsx_flip_cull.txt");
	s_noDraw = FlagFile("rsx_no_draw.txt");
	if (s_forceLinear) GpuLog("rsx_linear_textures.txt: textures stay linear");
	if (s_flipCull) GpuLog("rsx_flip_cull.txt: culling the other winding");
	if (s_noDraw) GpuLog("rsx_no_draw.txt: nothing is drawn");

	s_ring = (u8*)LocalAlloc(kRingSize * kRingFrames, 1024 * 1024, &s_ringOffset);
	if (!s_ring || !MakeWhite())
	{
		GpuLog("out of RSX memory for the frame ring");
		return false;
	}
	CmdReserve(64);
	rsxSetZMinMaxControl(s_ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetUserClipPlaneControl(s_ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	rsxSetColorMaskMrt(s_ctx, 0);
	s_attribMask = 0xFFFFFFFF;
	s_drawReady = true;
	GpuLog("draw path ready: %u KB frame ring x %d in RSX memory", kRingSize / 1024, kRingFrames);
	return true;
}

void DrawFrameStart()
{
	if (!s_drawReady) return;
	s_ringBase = (s_frameSeq % kRingFrames) * kRingSize;
	s_ringUsed = 0;
	RunDeferredFrees();
	// Commands go to a fresh stream position each frame; re-send state on first use.
	s_stateValid = false;
	s_viewValid = false;
	memset(&s_target, 0, sizeof(s_target));
	DisplaySurface(&s_target);	// FrameBegin bound the back buffer
	s_targetValid = true;
	s_currentVp = NULL;
	s_currentFpOffset = 0xFFFFFFFF;
	memset(s_vcValid, 0, sizeof(s_vcValid));
	memset(s_bound, 0, sizeof(s_bound));
	s_attribMask = 0xFFFFFFFF;
	CmdReserve(32);
	rsxInvalidateTextureCache(s_ctx, GCM_INVALIDATE_TEXTURE);
	rsxInvalidateVertexCache(s_ctx);
}

const char* DrawStats()
{
	snprintf(s_statsLine, sizeof(s_statsLine),
		"%u draws, %u drawn, %u skipped, %u to unusable targets; %u texture uploads (%u KB), %u KB through the ring, %u programs (%u failed), ring full %u",
		s_stats.draws, s_stats.drawn, s_stats.skipped, s_stats.offscreen, s_stats.uploads, s_stats.uploadKB,
		s_stats.ringKB, s_stats.programs, s_stats.programFailures, s_stats.ringFull);
	unsigned programs = s_stats.programs, failures = s_stats.programFailures;
	memset(&s_stats, 0, sizeof(s_stats));
	s_stats.programs = programs;
	s_stats.programFailures = failures;
	return s_statsLine;
}

static void FreeTexture(void* backend)
{
	RsxTexture* r = (RsxTexture*)backend;
	if (!r) return;
	for (int u = 0; u < 4; ++u) if (s_bound[u] == r) s_bound[u] = NULL;
	DeferFree(r->mem);	// the RSX may still read it: freed once those frames are done
	delete r;
}

static void FreeBuffer(void* backend)
{
	RsxBuffer* b = (RsxBuffer*)backend;
	if (!b) return;
	DeferFree(b->mem);
	delete b;
}

void TextureChanged(IDirect3DTexture9*) {}	// uploaded on the next draw that uses it (m_dirty)
void TextureReleased(IDirect3DTexture9* tex) { FreeTexture(tex->m_backend); tex->m_backend = NULL; }
void SurfaceReleased(IDirect3DSurface9* surf)
{
	RsxSurface* rs = surf->m_owner ? NULL : (RsxSurface*)surf->m_backend;
	if (rs)
	{
		DeferFree(rs->mem);
		delete rs;
	}
	surf->m_backend = NULL;
	s_targetValid = false;
}
void VertexBufferChanged(IDirect3DVertexBuffer9*, UINT, UINT) {}
void VertexBufferReleased(IDirect3DVertexBuffer9* vb) { FreeBuffer(vb->m_backend); vb->m_backend = NULL; }
void IndexBufferChanged(IDirect3DIndexBuffer9*, UINT, UINT) {}
void IndexBufferReleased(IDirect3DIndexBuffer9* ib) { FreeBuffer(ib->m_backend); ib->m_backend = NULL; }
void ShaderCreated(IDirect3DVertexShader9*) {}
void ShaderCreated(IDirect3DPixelShader9*) {}
void ShaderReleased(IDirect3DVertexShader9*) {}
void ShaderReleased(IDirect3DPixelShader9* ps)
{
	// Drop its cached programs (the key holds the pointer, which may be reused).
	for (int fog = 0; fog < 2; ++fog)
	{
		char key[48];
		snprintf(key, sizeof(key), "ps:%p:%d", (void*)ps, fog);
		std::map<std::string, FpEntry*>::iterator it = s_fps.find(key);
		if (it == s_fps.end()) continue;
		FpEntry* e = it->second;
		s_fps.erase(it);
		if (e)
		{
			DeferFree(e->local);	// the RSX may still run it: freed once those frames are done
			delete e;
		}
	}
}

UINT VideoMemoryFree()
{
	return 128 * 1024 * 1024;
}

} // namespace Ps3Gfx

#endif // !PS3_HEADLESS
