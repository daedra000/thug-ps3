// THUG PS3: RSX resources, programs, render state and draws for the D3D9 layer.
//
// - Resources: the D3D9 objects keep a main-memory copy of every texture, vertex and
//   index buffer (d3d9_device.cpp); textures and static vertex buffers that stop changing
//   give theirs up (see "Main-memory copies of uploaded resources"). The RSX gets its own
//   copy in RSX local memory, made when a draw first needs it and again after the game
//   changes it. Buffers changed
//   while the RSX may still be reading them, and the *UP draws' data, go through a
//   per-frame ring (three frames deep; the frame fence makes the slot three frames back
//   free). A buffer rewritten while the RSX may still read it is renamed into the ring for
//   the frame, and only the bytes the frame's draws read are copied there.
// - When the RSX is done: every resource records the frame that last used it, and the
//   frame fence's label (FramesDone) says which frames have finished, so memory is reused
//   or freed as soon as the RSX is past it rather than a fixed number of frames later.
// - Programs: the vertex program comes from the fixed-function state (the game draws
//   with SetFVF), the fragment program from the texture stages or the game's ps_1_1
//   source; both are generated (Code/Ps3/rsx/rsx_shadergen.cpp), assembled at run time
//   (rsx_asm.cpp) and cached. Fragment-program constants live in the program's code,
//   so a program with new constant values is a patched copy: a few per program are kept
//   in RSX memory and reused across frames, and the ring takes the rest.
// - Allocation: the backend's own records come from malloc, never operator new or the
//   default STL allocator (see NewRecord).
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
//   rsx_flip_cull.txt        swap which triangle winding is culled (back to the old mapping)
//   rsx_no_draw.txt          clear and flip only, draw nothing
//   rsx_depth_clamp.txt      clamp depth instead of clipping at the near and far planes
//   rsx_keep_texture_data.txt  keep textures' main-memory copies after upload
//   rsx_keep_buffer_data.txt   keep static vertex buffers' main-memory copies after upload
//   (and in d3d9_rsx.cpp: rsx_notiles.txt, rsx_no_zcull.txt, rsx_double_buffer.txt)

#include <ps3_platform.h>

#if !PS3_HEADLESS

#include <ps3_gfx_backend.h>
#include <ps3_fs.h>
#include "d3d9_rsx_internal.h"
#include "d3d9_swizzle.h"
#include "../rsx/rsx_asm.h"
#include "../rsx/rsx_shadergen.h"
#include "../rsx/rsx_pipeline.h"

// Textures' main-memory copies, counted in d3d9_device.cpp (for DrawStats).
extern volatile unsigned g_ps3TextureCount;
extern volatile unsigned long long g_ps3TextureRamBytes;

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <rsx/rsx.h>

extern unsigned long long g_ps3StageTicks[];	// Mainloop.cpp: this frame's CPU time by stage (9: the draw calls)
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
// a175dd3's draw-path savings, all on. The flashing surfaces came from reusing patched
// fragment programs (see UseFragmentProgram), not from these: bisect builds and the
// test-savings-on build (afb41d7) were steady with them on. Kept as constants so each can be
// turned off again in a test build.
static const bool s_wholeBuffers = false;	// F4: renamed buffers copied whole; no NOOVERWRITE writes into a ring copy
static const bool s_rebindAttribs = false;	// F3: vertex attributes bound on every draw
static const bool s_resendState = false;	// F10: render state, viewport and scissor sent on every draw
static const bool s_resendPrograms = false;	// programs and vertex constants sent on every draw
static const bool s_lateFrees = false;	// M7: freed RSX memory held until the frame being built is done
static const bool s_noTexSpares = false;	// M6: no spare blocks for textures updated every frame
static const bool s_noKick = false;		// F6: no mid-frame command-buffer flushes

static struct Stats
{
	unsigned draws, drawn, skipped, uploads, uploadKB, programs, programFailures, ringFull, offscreen;
	unsigned overflowKB;	// ring-full fallbacks: allocated on the side instead of dropping the draw
	unsigned texBlocks;		// re-uploads that needed a new RSX block (none was idle)
	unsigned fpPatches;		// patched fragment-program copies written (kept or in the ring)
	unsigned kicks;			// mid-frame command-buffer flushes
	unsigned targetWaits;	// RSX idle waits after drawing into a texture
	unsigned replayed, recorded, packetFails;	// draw packets (see "Draw packets")
	unsigned long long ringBytes;
} s_stats;
static char s_statsLine[1400];

// Where a draw's CPU time goes (timebase ticks, 79800 a millisecond), summed over the frames
// since the last report: the frames are CPU-bound (the RSX finishes within a millisecond of
// the CPU queueing a frame), so this is what to cut.
enum { kDpKeys, kDpVp, kDpBuffers, kDpState, kDpVsConst, kDpFp, kDpBind, kDpSubmit, kDpPhases };
static const char* const kDpNames[kDpPhases] = { "textures+fp lookup", "vp lookup", "vertex/index data", "view+state", "vs constants", "fp load", "textures+attributes", "draw cmds" };
static unsigned long long s_dp[kDpPhases], s_dpTotal;
static unsigned s_dpFrames;
#define DP_MARK(phase) do { const unsigned long long n_ = __builtin_ppc_get_timebase(); s_dp[phase] += n_ - dpLast; dpLast = n_; } while (0)

static bool FlagFile(const char* name)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", Ps3Fs::AppDir(), name);
	return Ps3Fs::Exists(path);
}

// ---------------------------------------------------------------------------
// The backend's own allocations
// ---------------------------------------------------------------------------
// Records (RsxTexture, RsxBuffer, RsxSurface, program entries, tables) come from malloc.
// On the game thread ps3_new.cpp sends operator new to whichever of the game's heaps is
// pushed at the time, so a texture first drawn during a level load would leave its record
// in that level's heap, and std::map nodes would scatter over whatever heap each draw ran
// under. malloc keeps them in newlib for the renderer's life. Records are plain data,
// zeroed here and released with FreeRecord (never delete: the pairs must match).
template <typename T> static T* NewRecord() { return (T*)calloc(1, sizeof(T)); }
static void FreeRecord(void* p) { free(p); }

// An open-addressing hash table of plain-data keys (compared with memcmp, so callers zero
// their keys, padding included, before filling them) to pointers, in malloc'd memory.
// Static instances need no constructor: zero is an empty table.
template <typename K, typename V>
class PodMap
{
public:
	V Find(const K& k) const
	{
		if (!m_cap) return NULL;
		for (u32 i = Hash(k) & (m_cap - 1);; i = (i + 1) & (m_cap - 1))
		{
			const Slot& s = m_slots[i];
			if (s.state == kEmpty) return NULL;
			if (s.state == kFull && !memcmp(&s.key, &k, sizeof(K))) return s.value;
		}
	}
	// k must not be in the table already. False if the table couldn't grow (out of memory).
	bool Insert(const K& k, V v)
	{
		if ((m_used + 1) * 4 > m_cap * 3 && !Grow()) return false;
		u32 i = Hash(k) & (m_cap - 1);
		while (m_slots[i].state == kFull) i = (i + 1) & (m_cap - 1);
		if (m_slots[i].state == kEmpty) ++m_used;
		memcpy(&m_slots[i].key, &k, sizeof(K));
		m_slots[i].value = v;
		m_slots[i].state = kFull;
		++m_count;
		return true;
	}
	V Erase(const K& k)
	{
		if (!m_cap) return NULL;
		for (u32 i = Hash(k) & (m_cap - 1);; i = (i + 1) & (m_cap - 1))
		{
			Slot& s = m_slots[i];
			if (s.state == kEmpty) return NULL;
			if (s.state == kFull && !memcmp(&s.key, &k, sizeof(K)))
			{
				s.state = kDeleted;	// a tombstone keeps later keys of the chain reachable
				--m_count;
				return s.value;
			}
		}
	}
	u32 Count() const { return m_count; }

private:
	enum { kEmpty = 0, kFull, kDeleted };
	struct Slot { K key; V value; u8 state; };
	Slot* m_slots;
	u32 m_cap, m_count, m_used;	// m_used: full slots and tombstones

	static u32 Hash(const K& k)
	{
		const u8* p = (const u8*)&k;
		u32 h = 2166136261u;	// FNV-1a
		for (size_t i = 0; i < sizeof(K); ++i) { h ^= p[i]; h *= 16777619u; }
		return h ^ (h >> 15);
	}
	// Rehashes into a table at most half full (the same size when only tombstones filled it).
	bool Grow()
	{
		u32 cap = m_cap ? m_cap : 64;
		while ((m_count + 1) * 2 > cap) cap *= 2;
		Slot* slots = (Slot*)calloc(cap, sizeof(Slot));
		if (!slots) return false;
		for (u32 i = 0; i < m_cap; ++i)
		{
			if (m_slots[i].state != kFull) continue;
			u32 j = Hash(m_slots[i].key) & (cap - 1);
			while (slots[j].state == kFull) j = (j + 1) & (cap - 1);
			memcpy(&slots[j], &m_slots[i], sizeof(Slot));	// keys byte for byte, padding included
		}
		free(m_slots);
		m_slots = slots;
		m_cap = cap;
		m_used = m_count;
		return true;
	}
};

// Logs a message the first `limit` times its key is seen. A small fixed table: keys are a
// handful of literals and texture formats; once it is full, new keys share one count.
static void LogLimited(const char* key, int limit, const char* fmt, ...)
{
	struct Seen { char key[40]; int n; };
	static Seen seen[64];
	static int used, shared;
	int* count = NULL;
	for (int i = 0; i < used && !count; ++i)
		if (!strncmp(seen[i].key, key, sizeof(seen[i].key) - 1)) count = &seen[i].n;
	if (!count && used < 64)
	{
		strncpy(seen[used].key, key, sizeof(seen[used].key) - 1);
		count = &seen[used++].n;
	}
	if (!count) count = &shared;
	int& n = *count;
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

// Frame of last use, stored +1 so 0 means never used: the frame fence's label reaches that
// value once the RSX has finished the frame (FramesDone). The signed difference keeps the
// test right should the counters ever wrap.
// FramesDone reads the fence label the RSX writes, an uncached read; a few per draw were a
// good part of the draw path's CPU time. Draws use a copy refreshed every 32 calls and at each
// frame start. It can only be behind the real label, which makes GpuMayUse err towards "still
// in use" (an extra rename or deferred free), never the other way.
static u32 s_doneCache;
static unsigned s_doneAge;
static bool s_freshDone;	// rsx_fresh_label.txt: read the label every time (a test switch)
static bool s_vsAlways;		// rsx_vs_always.txt: recompute the vertex constants for every draw (a test switch)
static bool s_slowDraws;		// rsx_slow_draws.txt: spin 2.5 us in every draw, nothing else changed (a test switch)
// rsx_fp_reuse.txt: fragment programs reused (kept copies, this frame's copy when the constants
// match) instead of a fresh copy per draw. They flashed in October before the wait after
// texture targets (s_noTargetWait) existed, which may have been the real cause (a test switch).
static bool s_fpReuse;
static bool s_noTargetWait;	// rsx_no_target_wait.txt: no RSX idle wait after drawing into a texture (a test switch)
static inline u32 DoneCached()
{
	if (s_freshDone) return FramesDone();
	if (++s_doneAge >= 32)
	{
		s_doneAge = 0;
		s_doneCache = FramesDone();
	}
	return s_doneCache;
}

static bool GpuMayUse(u32 usedPlusOne)
{
	return usedPlusOne != 0 && (s32)(DoneCached() - usedPlusOne) < 0;
}

// RSX memory the GPU may still read: freed once the frame that last used it is done. The
// fence label decides, so memory last used frames ago (most of a level being unloaded) is
// freed on the spot, and the rest as soon as the RSX gets past it: not only at the next
// frame start, which a load that presents nothing would hold off with both levels' memory.
struct DeferredFree { void* mem; u32 usedPlusOne; };
static DeferredFree* s_deferred;	// malloc'd (see NewRecord)
static u32 s_deferredCount, s_deferredCap;
static u32 s_deferredScanAt;		// FramesDone() at the last scan

static void RunDeferredFrees()
{
	s_doneCache = s_deferredScanAt = FramesDone();
	s_doneAge = 0;
	u32 keep = 0;
	for (u32 i = 0; i < s_deferredCount; ++i)
	{
		if (!GpuMayUse(s_deferred[i].usedPlusOne))
			rsxFree(s_deferred[i].mem);
		else
			s_deferred[keep++] = s_deferred[i];
	}
	s_deferredCount = keep;
}

static void DeferFree(void* mem, u32 usedPlusOne)
{
	if (!mem) return;
	if (s_lateFrees && (usedPlusOne == 0 || (s32)(usedPlusOne - (s_frameSeq + 1)) < 0)) usedPlusOne = s_frameSeq + 1;
	if (!GpuMayUse(usedPlusOne)) { rsxFree(mem); return; }
	if (s_deferredCount == s_deferredCap)
	{
		const u32 cap = s_deferredCap ? s_deferredCap * 2 : 256;
		DeferredFree* d = (DeferredFree*)realloc(s_deferred, cap * sizeof(DeferredFree));
		if (!d)
		{
			// Freeing now could pull memory from under the RSX: lose it instead.
			LogLimited("deferoom", 3, "out of memory for the deferred-free list: RSX memory leaked");
			return;
		}
		s_deferred = d;
		s_deferredCap = cap;
	}
	DeferredFree d = { mem, usedPlusOne };
	s_deferred[s_deferredCount++] = d;
	if (DoneCached() != s_deferredScanAt) RunDeferredFrees();	// the RSX has moved on since the last scan
}

// Out of RSX memory with frees still waiting: wait for the RSX to run everything queued so
// far (bounded, WaitIdle), after which memory last used by an earlier frame can't be in use
// any more, and free it. Memory stamped with this frame stays: ring overflow blocks are
// still read by this frame's later draws.
static bool ReclaimDeferred()
{
	if (!s_deferredCount) return false;
	RunDeferredFrees();
	if (s_deferredCount && WaitIdle("reclaiming RSX memory"))
	{
		u32 keep = 0;
		for (u32 i = 0; i < s_deferredCount; ++i)
		{
			if (s_deferred[i].usedPlusOne != s_frameSeq + 1)
				rsxFree(s_deferred[i].mem);
			else
				s_deferred[keep++] = s_deferred[i];
		}
		s_deferredCount = keep;
	}
	return true;
}

static void* LocalAlloc(u32 size, u32 align, u32* offset)
{
	void* p = rsxMemalign(align, size);
	if (!p && ReclaimDeferred())
	{
		LogLimited("reclaim", 3, "out of RSX memory for %u bytes: waited for the RSX to free what it was done with", size);
		p = rsxMemalign(align, size);
	}
	if (!p) return NULL;
	if (rsxAddressToOffset(p, offset) != 0) { rsxFree(p); return NULL; }
	return p;
}

// Space in this frame's ring slot. When the slot is full, a block of its own instead, freed
// once this frame is done (counted as ringFull): dropping the draw showed as flickering
// geometry. NULL only when RSX memory is out too.
static u8* RingAlloc(u32 size, u32 align, u32* offset)
{
	u32 at = (s_ringUsed + align - 1) & ~(align - 1);
	if (at + size > kRingSize)
	{
		++s_stats.ringFull;
		u8* p = (u8*)LocalAlloc(size ? size : 16, align > 128 ? align : 128, offset);
		if (!p)
		{
			LogLimited("ringfull", 5, "frame ring full (%u KB) and no RSX memory for %u bytes more: a draw was skipped", kRingSize / 1024, size);
			return NULL;
		}
		LogLimited("ringoverflow", 3, "frame ring full (%u KB): %u bytes allocated on the side", kRingSize / 1024, size);
		DeferFree(p, s_frameSeq + 1);
		s_stats.overflowKB += (size + 1023) / 1024;
		return p;
	}
	s_ringUsed = at + size;
	*offset = s_ringOffset + s_ringBase + at;
	return s_ring + s_ringBase + at;
}

// Vertex or index data was written into RSX memory: the next draw invalidates the vertex
// cache first (a draw's own one-word invalidate is not the full sequence).
static bool s_vertexWritten;

// Mid-frame flush: hands what is queued to the RSX instead of letting it idle until a
// segment fills or Present. Every kKickDraws draws, after a new render target, and after a
// big texture upload.
enum { kKickDraws = 48 };
static unsigned s_drawsSinceKick;
static void Kick()
{
	s_drawsSinceKick = 0;
	if (s_noKick) return;
	rsxFlushBuffer(s_ctx);
	++s_stats.kicks;
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
	u32 usedPlusOne;	// frame of the last draw that sampled or drew into it, +1 (0: never)
	u32 uploadedPlusOne;	// frame of the last upload, +1
	u32 quickUploads;	// uploads in a row that came within a few frames of the one before
	// Earlier blocks of a texture the game rewrites every frame or two (the movie player,
	// fonts): taken back once the RSX is done with them, instead of a new block and a
	// deferred free on every update.
	struct Spare { u8* mem; u32 offset, bytes, usedPlusOne; } spare[2];
	// How the last upload laid the levels out in mem, for reading them back (RestoreTextureData).
	u8 layout;			// kLayoutLinear, kLayoutSwizzled or kLayoutCompressed
	u8 bpp;				// bytes a texel (a 4x4 block for DXT)
	u32 levels;			// levels uploaded, packed one after another
	u32 linearPitch;	// row pitch of linear levels
	u32 total;			// bytes uploaded
};
enum { kLayoutLinear, kLayoutSwizzled, kLayoutCompressed };

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

// The texture's block may still be read by the RSX (or is too small): set it aside. A
// texture that keeps being uploaded every few frames swaps in a spare block the RSX is done
// with, or parks the busy one as a spare; otherwise the block is freed once the RSX is done
// with it, and so are the spares (the texture has stopped changing every frame). Two quick
// uploads in a row are asked for, so a texture rewritten once just after it was first drawn
// doesn't keep a spare block for good.
static void RetireTextureBlock(RsxTexture* r, u32 total)
{
	const bool frequent = r->quickUploads >= 2 && !s_noTexSpares;
	if (frequent && r->bytes >= total)
	{
		for (int i = 0; i < 2; ++i)
		{
			RsxTexture::Spare& sp = r->spare[i];
			if (sp.mem && sp.bytes >= total && !GpuMayUse(sp.usedPlusOne))
			{
				const RsxTexture::Spare cur = { r->mem, r->offset, r->bytes, r->usedPlusOne };
				r->mem = sp.mem; r->offset = sp.offset; r->bytes = sp.bytes; r->usedPlusOne = sp.usedPlusOne;
				sp = cur;
				return;
			}
		}
		for (int i = 0; i < 2; ++i)
		{
			RsxTexture::Spare& sp = r->spare[i];
			if (!sp.mem)
			{
				const RsxTexture::Spare cur = { r->mem, r->offset, r->bytes, r->usedPlusOne };
				sp = cur;
				r->mem = NULL;
				r->bytes = 0;
				return;
			}
		}
	}
	DeferFree(r->mem, r->usedPlusOne);
	r->mem = NULL;
	r->bytes = 0;
	if (!frequent)
	{
		for (int i = 0; i < 2; ++i)
		{
			DeferFree(r->spare[i].mem, r->spare[i].usedPlusOne);
			memset(&r->spare[i], 0, sizeof(r->spare[i]));
		}
	}
}

// ---------------------------------------------------------------------------
// Main-memory copies of uploaded resources
// ---------------------------------------------------------------------------
// A texture or static vertex buffer the game has stopped changing exists twice, in main RAM
// (the D3D object's m_data) and in RSX memory. Once the RSX copy has gone unchanged for
// kReleaseAfterFrames frames, the main-memory copy is freed. If the game locks the resource
// again after that, the D3D object asks for its copy back (RestoreTextureData,
// RestoreVertexBufferData, with the device lock held): it is read back from RSX memory (and
// de-swizzled), once; from then on the resource keeps its copy (m_keepData). Not released:
// render targets, dynamic resources, anything locked at the time, textures whose upload left
// levels out (non power-of-two textures upload their first level only) or whose format isn't
// uploaded, and index buffers (draws scan their indices for the range of a changing vertex
// buffer; without them the whole buffer would go through the ring).
// Switches: rsx_keep_texture_data.txt and rsx_keep_buffer_data.txt keep every copy.
enum { kReleaseAfterFrames = 30 };
static bool s_keepTextureData, s_keepBufferData;
static struct ResidencyStats
{
	unsigned texReleased, texRestored, vbReleased, vbRestored;
	unsigned long long texReleasedBytes, vbReleasedBytes;	// main RAM given back so far, less what came back
} s_residency;

// Frames since a resource's RSX copy was last written (stamp is frame + 1; 0: never).
static u32 FramesSince(u32 plusOne)
{
	return plusOne ? s_frameSeq + 1 - plusOne : 0;
}

static void ReleaseTextureData(IDirect3DTexture9* t, RsxTexture* r)
{
	if (s_keepTextureData || t->m_keepData || t->m_lockDepth || t->m_dirty || (t->m_usage & D3DUSAGE_DYNAMIC)) return;
	if (!r->valid || !r->mem || r->levels != t->m_levels || r->quickUploads) return;
	if (FramesSince(r->uploadedPlusOne) < kReleaseAfterFrames) return;
	free(t->m_data);
	t->m_data = NULL;
	t->m_dataReleased = true;
	::g_ps3TextureCount = ::g_ps3TextureCount - 1;
	::g_ps3TextureRamBytes = ::g_ps3TextureRamBytes - t->m_size;
	++s_residency.texReleased;
	s_residency.texReleasedBytes += t->m_size;
}

// Unpacks the RSX copy's levels (laid out as RsxTexture records) into data, the D3D layout.
static bool ReadBackTexture(const IDirect3DTexture9* t, const RsxTexture* r, u8* data)
{
	// One sequential read of RSX memory (the CPU reads it slowly, scattered reads slower still).
	u8* staging = (u8*)malloc(r->total ? r->total : 1);
	if (!staging) return false;
	memcpy(staging, r->mem, r->total);
	u32 at = 0;
	bool ok = true;
	for (u32 l = 0; l < r->levels && ok; ++l)
	{
		const u32 lw = t->LevelWidth(l), lh = t->LevelHeight(l);
		u8* dst = data + t->m_offsets[l];
		const u32 dstPitch = t->LevelPitch(l);
		const u8* src = staging + at;
		u32 size;
		if (r->layout == kLayoutCompressed)
		{
			const u32 rowBytes = ((lw + 3) / 4) * r->bpp, rows = (lh + 3) / 4;
			size = rowBytes * rows;
			for (u32 y = 0; y < rows; ++y) memcpy(dst + y * dstPitch, src + y * rowBytes, rowBytes);
		}
		else if (r->layout == kLayoutSwizzled)
		{
			size = lw * lh * r->bpp;
			u32* xt = (u32*)malloc((lw + lh) * sizeof(u32));
			if (!xt) { ok = false; break; }
			u32* yt = xt + lw;
			Ps3Swizzle::Tables(lw, lh, xt, yt);
			ok = Ps3Swizzle::Deswizzle(dst, dstPitch, src, lw, lh, r->bpp, xt, yt);
			free(xt);
		}
		else
		{
			size = r->linearPitch * lh;
			for (u32 y = 0; y < lh; ++y) memcpy(dst + y * dstPitch, src + y * r->linearPitch, lw * r->bpp);
		}
		at += size;
		if (at > r->total) ok = false;
	}
	free(staging);
	return ok;
}

bool RestoreTextureData(IDirect3DTexture9* t)
{
	DeviceGuard guard;
	if (t->m_data) { t->m_dataReleased = false; return true; }
	u8* data = (u8*)memalign(128, t->m_size ? t->m_size : 128);
	if (!data) { LogLimited("texrestoreoom", 3, "out of memory for %u bytes to read a %ux%u texture back into", t->m_size, t->m_width, t->m_height); return false; }
	memset(data, 0, t->m_size);
	const RsxTexture* r = (const RsxTexture*)t->m_backend;
	if (!r || !r->valid || !r->mem || r->renderTarget || r->levels != t->m_levels || !ReadBackTexture(t, r, data))
		LogLimited("texrestorefail", 3, "a released %ux%u texture couldn't be read back from RSX memory: it starts blank", t->m_width, t->m_height);
	else
		LogLimited("texrestore", 10, "a %ux%u texture (format %d) was locked again after its main-memory copy was released: read back from RSX memory (%u KB), kept from now on",
			t->m_width, t->m_height, (int)t->m_format, t->m_size / 1024);
	t->m_data = data;
	t->m_dataReleased = false;
	t->m_keepData = true;
	::g_ps3TextureCount = ::g_ps3TextureCount + 1;
	::g_ps3TextureRamBytes = ::g_ps3TextureRamBytes + t->m_size;
	++s_residency.texRestored;
	s_residency.texReleasedBytes -= t->m_size;
	return true;
}

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
	if (r && r->valid && (!t->m_dirty || r->renderTarget))
	{
		if (t->m_data && !r->renderTarget) ReleaseTextureData(t, r);
		return r;
	}
	if (!r)
	{
		r = NewRecord<RsxTexture>();
		if (!r) { LogLimited("texrec", 3, "out of memory for a texture record"); return NULL; }
		t->m_backend = r;
	}
	if (IsRenderTarget(t)) return RenderTargetTexture(t, r);

	// CreateTexture refuses a texture whose main-memory copy couldn't be allocated, so this
	// shouldn't happen; but there would be nothing to upload, so draw it white.
	if (!t->m_data)
	{
		LogLimited("texnodata", 1, "a %ux%u texture has no pixels to upload (drawn white)", t->m_width, t->m_height);
		r->valid = false;
		t->m_dirty = false;
		return NULL;
	}

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

	// Never rewrite memory the RSX may still be sampling: a texture used in a frame the RSX
	// hasn't finished gets another block (RetireTextureBlock).
	const bool quick = r->uploadedPlusOne && s_frameSeq + 1 - r->uploadedPlusOne <= 4;
	r->quickUploads = quick ? (r->quickUploads < 1000 ? r->quickUploads + 1 : r->quickUploads) : 0;
	if (r->mem && (r->bytes < total || GpuMayUse(r->usedPlusOne)))
		RetireTextureBlock(r, total);
	if (!r->mem)
	{
		r->mem = (u8*)LocalAlloc(total, 128, &r->offset);
		r->bytes = r->mem ? total : 0;
		r->usedPlusOne = 0;
		if (r->uploadedPlusOne) ++s_stats.texBlocks;
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
			u32* xt = (u32*)malloc((lw + lh) * sizeof(u32));
			if (!xt) { free(staging); return NULL; }
			u32* yt = xt + lw;
			Ps3Swizzle::Tables(lw, lh, xt, yt);
			// Typed copies in 8 x 8 blocks (d3d9_swizzle.h): a memcpy call per texel made big
			// uploads slow, and row order scattered the stores across the whole level.
			Ps3Swizzle::Swizzle(dst, src, srcPitch, lw, lh, fi.bpp, xt, yt);
			free(xt);
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
	r->uploadedPlusOne = s_frameSeq + 1;
	r->layout = (u8)(fi.compressed ? kLayoutCompressed : swizzle ? kLayoutSwizzled : kLayoutLinear);
	r->bpp = (u8)fi.bpp;
	r->levels = levels;
	r->linearPitch = linearPitch;
	r->total = total;
	t->m_dirty = false;
	++s_stats.uploads;
	s_stats.uploadKB += total / 1024;
	if (total >= 256 * 1024) Kick();	// the RSX can work through what's queued while the CPU goes on
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
	// While the persistent copy is behind (stale), the frame's draws read a renamed copy in
	// the ring (or a ring-overflow block). Room for the whole buffer is taken, but only the
	// bytes draws read are copied into it, from the main-memory copy (always current):
	// [ringLo, ringHi) holds them.
	u32 ringFrame, ringOffset;
	u8* ring;
	u32 ringLo, ringHi;
	bool stale;			// the newest data is only in this frame's ring copy
	u32 changedPlusOne;	// frame of the last change the CPU made (uploaded or renamed), +1
};

// A fresh ring copy of the buffer for this frame, nothing copied into it yet.
static bool RenameBuffer(RsxBuffer* b, u32 size)
{
	u32 off;
	u8* p = RingAlloc(size ? size : 16, 128, &off);
	if (!p) return false;
	b->ring = p;
	b->ringOffset = off;
	b->ringFrame = s_frameSeq;
	b->ringLo = b->ringHi = 0;
	b->stale = true;
	return true;
}

// Copies [lo, hi) into this frame's ring copy where it isn't there yet. The copy stays one
// range: a gap between it and the new bytes is filled too.
static void FillRenamed(RsxBuffer* b, const BYTE* data, u32 lo, u32 hi)
{
	if (hi <= lo) return;
	u32 copied = 0;
	if (b->ringHi <= b->ringLo)
	{
		memcpy(b->ring + lo, data + lo, hi - lo);
		copied = hi - lo;
		b->ringLo = lo;
		b->ringHi = hi;
	}
	else
	{
		if (lo < b->ringLo) { memcpy(b->ring + lo, data + lo, b->ringLo - lo); copied += b->ringLo - lo; b->ringLo = lo; }
		if (hi > b->ringHi) { memcpy(b->ring + b->ringHi, data + b->ringHi, hi - b->ringHi); copied += hi - b->ringHi; b->ringHi = hi; }
	}
	if (copied)
	{
		s_stats.ringBytes += copied;
		s_vertexWritten = true;
	}
}

// The RSX offset of a buffer's data for a draw in this frame, uploading as needed.
// changes says what the CPU wrote since the last upload (Ps3BufferChanges in d3d9.h);
// [needLo, needHi) is the part of the buffer this draw reads.
static bool BufferOffset(const BYTE* data, u32 size, bool& dirty, const Ps3BufferChanges& changes, void*& backend,
	u32 needLo, u32 needHi, u32* offset)
{
	RsxBuffer* b = (RsxBuffer*)backend;
	if (!b)
	{
		b = NewRecord<RsxBuffer>();
		if (!b) { LogLimited("bufrec", 3, "out of memory for a buffer record"); return false; }
		backend = b;
	}
	const u32 now = s_frameSeq;
	const bool idle = !GpuMayUse(b->persistentUsed);	// the RSX is done with the persistent copy
	if (!data && (!b->mem || dirty || b->stale))
	{
		// A released main-memory copy (ReleaseVertexBufferData) is only ever let go of while the
		// RSX copy is complete and current, and comes back before the game can change it.
		LogLimited("bufnodata", 3, "a %u-byte buffer has no main-memory copy to upload from: draw skipped", size);
		return false;
	}
	if (dirty || !b->mem) b->changedPlusOne = now + 1;
	if (s_wholeBuffers) { needLo = 0; needHi = size; }
	if (needHi > size) needHi = size;
	if (needLo > needHi) needLo = needHi;

	if (b->mem && b->size < size)
	{
		DeferFree(b->mem, b->persistentUsed);	// grown: the old copy goes once the RSX is done with it
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
		s_vertexWritten = true;
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
			s_vertexWritten = true;
		}
		else if (changes.noOverwrite && b->ringFrame == now && !s_wholeBuffers)
		{
			// Appended to with NOOVERWRITE after this frame renamed it: the same promise covers
			// the ring copy, so the changed bytes it already holds are rewritten in place (the
			// ones it doesn't hold are copied when a draw reads them).
			u32 lo = changes.lo > b->ringLo ? changes.lo : b->ringLo;
			u32 hi = changes.hi < b->ringHi ? changes.hi : b->ringHi;
			if (hi > lo)
			{
				memcpy(b->ring + lo, data + lo, hi - lo);
				s_stats.ringBytes += hi - lo;
				s_vertexWritten = true;
			}
		}
		else
		{
			// Rewritten (DISCARD or a plain lock) while the RSX may still read the old data:
			// renamed, this frame's draws read a ring copy.
			if (!RenameBuffer(b, size)) return false;
		}
		dirty = false;
	}
	else if (b->stale && b->ringFrame != now)
	{
		if (idle)
		{
			memcpy(b->mem, data, size);	// catch the persistent copy up once the RSX is done with it
			b->stale = false;
			s_vertexWritten = true;
		}
		else if (!RenameBuffer(b, size))
			return false;
	}

	if (b->stale)
	{
		FillRenamed(b, data, needLo, needHi);
		*offset = b->ringOffset;
	}
	else
	{
		*offset = b->offset;
		b->persistentUsed = now + 1;	// only uses of the persistent copy keep it busy
	}
	return true;
}

// A static vertex buffer's main-memory copy (see "Main-memory copies of uploaded resources"),
// after a draw that used the persistent RSX copy.
static void ReleaseVertexBufferData(IDirect3DVertexBuffer9* vb)
{
	const RsxBuffer* b = (const RsxBuffer*)vb->m_backend;
	if (s_keepBufferData || !vb->m_data || !b || !b->mem || b->stale || b->size < vb->m_size) return;
	if (vb->m_keepData || vb->m_lockDepth || vb->m_dirty || (vb->m_usage & D3DUSAGE_DYNAMIC)) return;
	if (FramesSince(b->changedPlusOne) < kReleaseAfterFrames) return;
	free(vb->m_data);
	vb->m_data = NULL;
	vb->m_dataReleased = true;
	++s_residency.vbReleased;
	s_residency.vbReleasedBytes += vb->m_size;
}

bool RestoreVertexBufferData(IDirect3DVertexBuffer9* vb, bool contents)
{
	DeviceGuard guard;
	if (vb->m_data) { vb->m_dataReleased = false; return true; }
	u8* data = (u8*)memalign(128, vb->m_size ? vb->m_size : 128);
	if (!data) { LogLimited("vbrestoreoom", 3, "out of memory for %u bytes to read a vertex buffer back into", vb->m_size); return false; }
	const RsxBuffer* b = (const RsxBuffer*)vb->m_backend;
	if (!contents)
		memset(data, 0, vb->m_size);	// D3DLOCK_DISCARD: the old contents are not wanted
	else if (b && b->mem && !b->stale && b->size >= vb->m_size)
	{
		memcpy(data, b->mem, vb->m_size);	// RSX memory, read by the CPU: slow, but once per buffer
		LogLimited("vbrestore", 10, "a static %u-byte vertex buffer was locked again after its main-memory copy was released: read back from RSX memory, kept from now on", vb->m_size);
	}
	else
	{
		memset(data, 0, vb->m_size);
		LogLimited("vbrestorefail", 3, "a released %u-byte vertex buffer couldn't be read back from RSX memory: it starts blank", vb->m_size);
	}
	vb->m_data = data;
	vb->m_dataReleased = false;
	vb->m_keepData = true;
	++s_residency.vbRestored;
	s_residency.vbReleasedBytes -= vb->m_size;
	return true;
}

// The smallest and largest of `count` indices (16 or 32 bits).
static void IndexRange(const u8* idx, bool i32, u32 count, u32* lo, u32* hi)
{
	u32 mn = 0xFFFFFFFF, mx = 0;
	if (i32)
	{
		const u32* p = (const u32*)idx;
		for (u32 i = 0; i < count; ++i) { const u32 v = p[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
	}
	else
	{
		const u16* p = (const u16*)idx;
		for (u32 i = 0; i < count; ++i) { const u32 v = p[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
	}
	*lo = mn;
	*hi = mx;
}

// ---------------------------------------------------------------------------
// Programs
// ---------------------------------------------------------------------------
struct VpEntry
{
	u8* blob;			// the assembled program (malloc'd)
	rsxVertexProgram* prog;
	void* ucode;
};

// A patched copy of a fragment program (its constants written into the code), kept in RSX
// memory across frames: draws that come back to the same constant values reuse it.
struct FpSlot
{
	u8* mem;
	u32 offset;
	u32 usedPlusOne;	// frame of the last draw that ran it, +1
	u32 stamp;			// least recently used goes first
	float vals[9][4];
};
enum { kFpSlots = 4 };

struct FpEntry
{
	u8* blob;			// the assembled program (malloc'd)
	rsxFragmentProgram* prog;
	u8* ucode;			// in the blob
	u32 ucodeSize;
	u8* local;			// persistent copy in RSX memory (default constants)
	u32 localOffset;
	u32 localUsed;		// frame of the last draw that ran the persistent copy, +1
	struct Const { int reg; u32 first, count; };	// offsets[first..first+count) in the code
	Const consts[9];
	int numConsts;
	u32* offsets;		// malloc'd
	FpSlot slots[kFpSlots];
	float last[9][4];	// constant values in this frame's last ring copy
	u32 lastFrame, lastOffset;
	int texMask;		// stages it samples
};

// Program caches, keyed by plain data (no std::string keys built per draw). A fragment
// program is either the fixed-function one for an FsKey, or a pixel shader's (by pointer,
// with or without fog).
struct FpKey
{
	u8 kind;			// 0: fixed function (fs), 1: pixel shader (ps)
	u8 fog;
	u8 pad[6];
	u64 ps;
	FsKey fs;
};
static PodMap<VsKey, VpEntry*> s_vps;
static PodMap<FpKey, FpEntry*> s_fps;
// Consecutive draws mostly use the same programs: the last key of each is checked first.
static VsKey s_lastVsKey;
static VpEntry* s_lastVp;
static FpKey s_lastFpKey;
static FpEntry* s_lastFp;
static u32 s_fpStamp;
static const VpEntry* s_currentVp;
static u32 s_currentFpOffset = 0xFFFFFFFF;
static float s_vc[kVcCount][4];
// The device's m_vsSerial when the constants on the RSX (s_vc) were last brought up to date; 0
// after anything that cleared s_vcValid. A draw with the same serial skips FillVsConstants and
// the comparison.
static unsigned s_vcSerial;
static bool s_vcValid[kVcCount];

// The assembler fills a std::vector; the program is kept in a malloc'd copy of it.
static u8* KeepBlob(const std::vector<unsigned char>& v)
{
	if (v.empty()) return NULL;
	u8* p = (u8*)memalign(16, v.size());
	if (p) memcpy(p, &v[0], v.size());
	return p;
}

static VpEntry* VertexProgramFor(const VsKey& key)
{
	if (s_lastVp && !memcmp(&key, &s_lastVsKey, sizeof(key))) return s_lastVp;
	VpEntry* e = s_vps.Find(key);
	if (!e)
	{
		e = NewRecord<VpEntry>();
		if (!e) { LogLimited("vprec", 3, "out of memory for a vertex program record"); return NULL; }
		std::string src = VertexProgram(key), err;
		{
			std::vector<unsigned char> blob;
			if (RsxAsm::AssembleVertexProgram(src.c_str(), blob, &err))
				e->blob = KeepBlob(blob);
		}
		if (e->blob)
		{
			e->prog = (rsxVertexProgram*)e->blob;
			u32 size;
			rsxVertexProgramGetUCode(e->prog, &e->ucode, &size);
			++s_stats.programs;
		}
		else
		{
			++s_stats.programFailures;
			GpuLog("vertex program failed (%s):\n%s", err.c_str(), src.c_str());
		}
		if (!s_vps.Insert(key, e)) LogLimited("vpmap", 3, "out of memory for the vertex program table");
	}
	memcpy(&s_lastVsKey, &key, sizeof(key));
	s_lastVp = e;
	return e;
}

static inline u32 SwapHalves(float f)
{
	u32 u;
	memcpy(&u, &f, 4);
	return (u >> 16) | (u << 16);
}

static FpEntry* FragmentProgramFrom(const FpKey& key, const std::string& src, int texMask)
{
	FpEntry* e = NewRecord<FpEntry>();
	if (!e) { LogLimited("fprec", 3, "out of memory for a fragment program record"); return NULL; }
	e->lastFrame = 0xFFFFFFFF;
	e->texMask = texMask;
	std::string err;
	if (!src.empty())
	{
		std::vector<unsigned char> blob;
		if (RsxAsm::AssembleFragmentProgram(src.c_str(), blob, &err))
		{
			e->blob = KeepBlob(blob);
			if (!e->blob) err = "out of memory";
		}
	}
	if (e->blob)
	{
		e->prog = (rsxFragmentProgram*)e->blob;
		void* ucode;
		rsxFragmentProgramGetUCode(e->prog, &ucode, &e->ucodeSize);
		e->ucode = (u8*)ucode;
		e->local = (u8*)LocalAlloc(e->ucodeSize, 64, &e->localOffset);
		if (e->local) memcpy(e->local, e->ucode, e->ucodeSize);
		else { e->prog = NULL; err = "out of RSX memory"; }

		// Where each named constant (c0, c1...) sits in the code.
		const u8* base = e->blob;
		const rsxProgramConst* consts = e->prog ? (const rsxProgramConst*)(base + e->prog->const_off) : NULL;
		u32 total = 0;
		for (int pass = 0; pass < 2 && e->prog; ++pass)
		{
			if (pass == 1)
			{
				if (!total) break;
				e->offsets = (u32*)malloc(total * sizeof(u32));
				if (!e->offsets) { e->prog = NULL; err = "out of memory"; break; }
				total = 0;
			}
			for (int i = 0; i < e->prog->num_const; ++i)
			{
				if (consts[i].is_internal || consts[i].index == 0xFFFFFFFF || !consts[i].name_off) continue;
				const char* name = (const char*)(base + consts[i].name_off);
				if (name[0] != 'c') continue;
				const int reg = atoi(name + 1);
				const rsxConstOffsetTable* table = (const rsxConstOffsetTable*)(base + consts[i].index);
				if (reg < 0 || reg >= 9 || !table->num) continue;
				if (pass == 1)
				{
					if (e->numConsts >= 9) break;
					FpEntry::Const& c = e->consts[e->numConsts++];
					c.reg = reg;
					c.first = total;
					c.count = table->num;
					for (u32 k = 0; k < table->num; ++k) e->offsets[total + k] = table->offset[k];
				}
				total += table->num;
			}
		}
		if (e->prog) ++s_stats.programs;
	}
	if (!e->prog)
	{
		++s_stats.programFailures;
		GpuLog("fragment program failed (%s):\n%s", err.c_str(), src.c_str());
	}
	if (!s_fps.Insert(key, e)) LogLimited("fpmap", 3, "out of memory for the fragment program table");
	return e;
}

static void FreeFragmentProgram(FpEntry* e)
{
	if (s_lastFp == e) s_lastFp = NULL;
	// The RSX may still run these: freed once those frames are done.
	DeferFree(e->local, e->localUsed);
	for (int i = 0; i < kFpSlots; ++i) DeferFree(e->slots[i].mem, e->slots[i].usedPlusOne);
	free(e->offsets);
	free(e->blob);
	FreeRecord(e);
}

// Do the given constants match the ones in `vals`, for the registers the program reads?
static bool SameConsts(const FpEntry* e, const float (*vals)[4], const float (*values)[4])
{
	for (int i = 0; i < e->numConsts; ++i)
		if (memcmp(vals[e->consts[i].reg], values[e->consts[i].reg], 16)) return false;
	return true;
}

// Writes the program's code with the given constants patched in to p (RSX memory).
static void WritePatched(const FpEntry* e, u8* p, const float (*values)[4], float (*vals)[4])
{
	memcpy(p, e->ucode, e->ucodeSize);
	for (int i = 0; i < e->numConsts; ++i)
	{
		const FpEntry::Const& c = e->consts[i];
		const float* v = values[c.reg];
		u32 words[4] = { SwapHalves(v[0]), SwapHalves(v[1]), SwapHalves(v[2]), SwapHalves(v[3]) };
		for (u32 k = 0; k < c.count; ++k)
			memcpy(p + e->offsets[c.first + k], words, 16);
		memcpy(vals[c.reg], v, 16);
	}
	++s_stats.fpPatches;
}

// Loads a fragment program with the given constants (c0..c8).
static bool UseFragmentProgram(FpEntry* e, const float (*values)[4])
{
	u32 offset = e->localOffset;
	const u32 usedPlusOne = s_frameSeq + 1;
	if (e->numConsts && !s_fpReuse)
	{
		// A fresh copy in the ring on every draw. Kept copies (F5) and reusing this frame's
		// copy when the constants matched were the last difference between main, which
		// flashed with every other switch on, and the steady bisect builds.
		u8* fresh = RingAlloc(e->ucodeSize, 64, &offset);
		if (!fresh) return false;
		WritePatched(e, fresh, values, e->last);
		e->lastFrame = s_frameSeq;
		e->lastOffset = offset;
		CmdReserve(64);
		rsxLoadFragmentProgramLocation(s_ctx, e->prog, offset, GCM_LOCATION_RSX);
		s_currentFpOffset = offset;
		return true;
	}
	if (e->numConsts)
	{
		FpSlot* slot = NULL;
		for (int i = 0; i < kFpSlots && !slot; ++i)
			if (e->slots[i].mem && SameConsts(e, e->slots[i].vals, values)) slot = &e->slots[i];
		if (slot)
			offset = slot->offset;	// a kept copy with these values
		else if (e->lastFrame == s_frameSeq && SameConsts(e, e->last, values))
			offset = e->lastOffset;	// this frame's ring copy
		else
		{
			// A new copy: in an empty slot, else in the least recently used slot the RSX is
			// done with; with every slot busy, in the ring for this frame.
			for (int i = 0; i < kFpSlots && !slot; ++i)
			{
				if (e->slots[i].mem) continue;
				e->slots[i].mem = (u8*)LocalAlloc(e->ucodeSize, 64, &e->slots[i].offset);
				if (e->slots[i].mem) slot = &e->slots[i];
				break;
			}
			if (!slot)
			{
				for (int i = 0; i < kFpSlots; ++i)
				{
					FpSlot& c = e->slots[i];
					if (c.mem && !GpuMayUse(c.usedPlusOne) && (!slot || c.stamp < slot->stamp)) slot = &c;
				}
			}
			if (slot)
			{
				WritePatched(e, slot->mem, values, slot->vals);
				offset = slot->offset;
				if (offset == s_currentFpOffset) s_currentFpOffset = 0xFFFFFFFF;	// new code at that address: load it again
			}
			else
			{
				u8* p = RingAlloc(e->ucodeSize, 64, &offset);
				if (!p) return false;
				WritePatched(e, p, values, e->last);
				e->lastFrame = s_frameSeq;
				e->lastOffset = offset;
			}
		}
		if (slot)
		{
			slot->usedPlusOne = usedPlusOne;
			slot->stamp = ++s_fpStamp;
		}
	}
	else
		e->localUsed = usedPlusOne;
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
	if (!c) return;	// the constants on the RSX are already this draw's (s_vcSerial)
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
	u32 zcull;		// ZCULL on for this draw (see below)
};
static GpuState s_state;
static bool s_stateValid;

// ZCULL keeps, per block of the display's depth buffer, the farthest depth there, and throws
// out fragments behind it, which is right for LESS, LEQUAL and EQUAL tests (fragments behind
// everything fail those anyway). So it is on only for those, and only while the display's
// depth buffer is bound (the region is that buffer's). A draw that writes depth farther than
// what is there (GREATER, GEQUAL, NOTEQUAL or ALWAYS with writes on), or a depth clear of
// part of the buffer, leaves ZCULL's values behind the buffer's: they are rebuilt
// (invalidated) before the next draw that uses ZCULL. A clear of the whole buffer resets them.
static bool s_zcullStale;
static u32 s_displayDepthOffset;	// the display's depth buffer (DrawInit)
static SurfaceDesc s_target;	// the bound target (ApplyTarget)
static bool s_targetValid;

static bool OnDisplayDepth()
{
	return s_targetValid && s_target.hasDepth && s_target.depthOffset == s_displayDepthOffset;
}

static bool FarDepthWrite(DWORD func)
{
	return func == D3DCMP_GREATER || func == D3DCMP_GREATEREQUAL || func == D3DCMP_NOTEQUAL || func == D3DCMP_ALWAYS;
}

void ZcullDepthClear(s32 x0, s32 y0, s32 x1, s32 y1)
{
	if (!ZcullActive() || !OnDisplayDepth()) return;
	SurfaceDesc disp;
	memset(&disp, 0, sizeof(disp));
	DisplaySurface(&disp);
	if (x0 <= 0 && y0 <= 0 && x1 >= (s32)disp.width && y1 >= (s32)disp.height)
	{
		// The whole buffer: the clear resets ZCULL too, if it is on while clearing.
		rsxSetZCullEnable(s_ctx, GCM_TRUE, GCM_FALSE);
		s_zcullStale = false;
	}
	else
		s_zcullStale = true;
}

struct ViewState { D3DVIEWPORT9 vp; RECT scissor; u32 scissorOn; };
static ViewState s_view;
static bool s_viewValid;
static s32 s_scissorSent[4];	// x, y, w, h last sent (valid with s_viewValid)
static bool s_clipsSent;		// the viewport clips, sent once per frame and render target

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
	const bool zcull = ZcullActive();
	if (zcull)
	{
		const bool onDisplay = OnDisplayDepth();
		const DWORD f = s.depthFunc;
		s.zcull = s.depthTest && onDisplay && (f == D3DCMP_LESS || f == D3DCMP_LESSEQUAL || f == D3DCMP_EQUAL);
		if (s.zcull && s_zcullStale)
		{
			CmdReserve(8);
			rsxSetZCullInvalidate(s_ctx);
			s_zcullStale = false;
		}
		if (s.depthWrite && onDisplay && FarDepthWrite(f)) s_zcullStale = true;	// this draw (ZCULL off)
	}

	if (s_stateValid && !memcmp(&s, &s_state, sizeof(s))) return;
	// Only the blocks that changed are sent; everything after a frame start, a new render
	// target or a clear (s_stateValid false).
	const bool all = !s_stateValid;
	const GpuState& o = s_state;

	CmdReserve(160);
	if (all || s.blendEnable != o.blendEnable) rsxSetBlendEnable(s_ctx, s.blendEnable ? GCM_TRUE : GCM_FALSE);
	if (s.blendEnable)
	{
		// Factors, equation and colour aren't sent while blending is off, so all go when it
		// comes on.
		const bool on = all || !o.blendEnable;
		if (on || s.srcBlend != o.srcBlend || s.dstBlend != o.dstBlend)
		{
			u16 sc = Blend(s.srcBlend, false), dc = Blend(s.dstBlend, false), sa = Blend(s.srcBlend, true), da = Blend(s.dstBlend, true);
			if (s.srcBlend == D3DBLEND_BOTHSRCALPHA) { sc = sa = GCM_SRC_ALPHA; dc = da = GCM_ONE_MINUS_SRC_ALPHA; }
			if (s.srcBlend == D3DBLEND_BOTHINVSRCALPHA) { sc = sa = GCM_ONE_MINUS_SRC_ALPHA; dc = da = GCM_SRC_ALPHA; }
			rsxSetBlendFunc(s_ctx, sc, dc, sa, da);
		}
		if (on || s.blendOp != o.blendOp)
		{
			u16 eq = GCM_FUNC_ADD;
			if (s.blendOp == D3DBLENDOP_SUBTRACT) eq = GCM_FUNC_SUBTRACT;
			else if (s.blendOp == D3DBLENDOP_REVSUBTRACT) eq = GCM_FUNC_REVERSE_SUBTRACT;
			else if (s.blendOp == D3DBLENDOP_MIN) eq = GCM_FUNC_MIN;
			else if (s.blendOp == D3DBLENDOP_MAX) eq = GCM_FUNC_MAX;
			rsxSetBlendEquation(s_ctx, eq, eq);
		}
		if (on || s.blendColor != o.blendColor) rsxSetBlendColor(s_ctx, s.blendColor, 0);
	}
	if (all || s.alphaTest != o.alphaTest) rsxSetAlphaTestEnable(s_ctx, s.alphaTest ? GCM_TRUE : GCM_FALSE);
	if (all || s.alphaFunc != o.alphaFunc || s.alphaRef != o.alphaRef) rsxSetAlphaFunc(s_ctx, Compare(s.alphaFunc), s.alphaRef);
	if (all || s.depthTest != o.depthTest) rsxSetDepthTestEnable(s_ctx, s.depthTest ? GCM_TRUE : GCM_FALSE);
	if (all || s.depthWrite != o.depthWrite) rsxSetDepthWriteEnable(s_ctx, s.depthWrite ? GCM_TRUE : GCM_FALSE);
	if (all || s.depthFunc != o.depthFunc) rsxSetDepthFunc(s_ctx, Compare(s.depthFunc));
	// D3D culls by screen-space winding (y down). On a console the opposite of the original
	// mapping proved right: with it, models showed their insides (a jersey's back print seen
	// mirrored through the front) and ground seen from above was culled away. So D3DCULL_CCW
	// culls the RSX's front (CCW) faces. rsx_flip_cull.txt brings back the old mapping.
	if (all || s.cullEnable != o.cullEnable) rsxSetCullFaceEnable(s_ctx, s.cullEnable ? GCM_TRUE : GCM_FALSE);
	if (all) rsxSetFrontFace(s_ctx, GCM_FRONTFACE_CCW);
	if (all || s.cullFace != o.cullFace)
	{
		bool cullBack = s.cullFace != D3DCULL_CCW;
		if (s_flipCull) cullBack = !cullBack;
		rsxSetCullFace(s_ctx, cullBack ? GCM_CULL_BACK : GCM_CULL_FRONT);
	}
	if (all || s.colorMask != o.colorMask)
	{
		u32 mask = 0;
		if (s.colorMask & D3DCOLORWRITEENABLE_RED) mask |= GCM_COLOR_MASK_R;
		if (s.colorMask & D3DCOLORWRITEENABLE_GREEN) mask |= GCM_COLOR_MASK_G;
		if (s.colorMask & D3DCOLORWRITEENABLE_BLUE) mask |= GCM_COLOR_MASK_B;
		if (s.colorMask & D3DCOLORWRITEENABLE_ALPHA) mask |= GCM_COLOR_MASK_A;
		rsxSetColorMask(s_ctx, mask);
	}
	if (all || s.offsetEnable != o.offsetEnable) rsxSetPolygonOffsetFillEnable(s_ctx, s.offsetEnable ? GCM_TRUE : GCM_FALSE);
	if (s.offsetEnable && (all || !o.offsetEnable || memcmp(&s.offsetFactor, &o.offsetFactor, sizeof(float)) ||
		memcmp(&s.offsetUnits, &o.offsetUnits, sizeof(float))))
		rsxSetPolygonOffset(s_ctx, s.offsetFactor, s.offsetUnits);
	if (all || s.shade != o.shade) rsxSetShadeModel(s_ctx, s.shade);
	if (zcull && (all || s.zcull != o.zcull))
	{
		if (all) rsxSetZCullControl(s_ctx, GCM_ZCULL_LESS, GCM_ZCULL_LONES);
		rsxSetZCullEnable(s_ctx, s.zcull ? GCM_TRUE : GCM_FALSE, GCM_FALSE);
	}
	s_state = s;
	s_stateValid = true;
}

static void ApplyView(IDirect3DDevice9* dev)
{
	ViewState v;
	memset(&v, 0, sizeof(v));
	v.vp = dev->m_viewport;
	v.scissor = dev->m_scissor;
	v.scissorOn = dev->m_rs[D3DRS_SCISSORTESTENABLE] != 0;
	if (s_viewValid && !memcmp(&v, &s_view, sizeof(v))) return;
	const bool all = !s_viewValid;
	const bool vpChanged = all || memcmp(&v.vp, &s_view.vp, sizeof(v.vp));
	s_view = v;
	s_viewValid = true;

	const D3DVIEWPORT9& vp = v.vp;
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
	if (vpChanged)
	{
		// GL-style depth (-1..1 -> MinZ..MaxZ), y flipped, plus D3D9's half-pixel offset.
		float scale[4], offset[4];
		ViewportTransform(vp, scale, offset);
		rsxSetViewport(s_ctx, vp.X, vp.Y, vp.Width, vp.Height, 0.0f, 1.0f, scale, offset);
	}
	// The clips never change (the whole 4096 x 4096 range): once per frame and render target.
	if (!s_clipsSent)
	{
		for (int i = 0; i < 8; ++i) rsxSetViewportClip(s_ctx, i, 4096, 4096);
		s_clipsSent = true;
	}
	const s32 sc[4] = { x, y, w, h };
	if (all || memcmp(sc, s_scissorSent, sizeof(sc)))
	{
		rsxSetScissor(s_ctx, x, y, w, h);
		memcpy(s_scissorSent, sc, sizeof(sc));
	}
}

// ---------------------------------------------------------------------------
// Draws
// ---------------------------------------------------------------------------
static u32 s_attribMask;	// vertex attributes bound by the last draw
// The last binding (layout, base, stride): a draw with the same one sends nothing.
static FvfLayout s_attrLayout;
static u32 s_attrBase, s_attrStride;
static bool s_attrValid;

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

// FvfLayout's fields up to and including rhw (not its tail padding).
static bool SameLayout(const FvfLayout& a, const FvfLayout& b)
{
	return !memcmp(&a, &b, offsetof(FvfLayout, rhw) + sizeof(a.rhw));
}

static void BindAttributes(const FvfLayout& l, u32 base, u32 stride)
{
	if (s_attrValid && base == s_attrBase && stride == s_attrStride && SameLayout(l, s_attrLayout)) return;
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
	memcpy(&s_attrLayout, &l, sizeof(l));
	s_attrBase = base;
	s_attrStride = stride;
	s_attrValid = true;
	// No vertex-cache invalidate here: the draw invalidates it when vertex data was written
	// (s_vertexWritten), and rsxDrawIndexArray / rsxDrawVertexArray send their own.
}

static bool Skip(const char* why)
{
	++s_stats.skipped;
	LogLimited(why, 3, "draw skipped: %s", why);
	if (s_capture) printf("[cap]   skipped: %s\n", why);
	return false;
}

// The frame capture's log line for one draw: what the draw used, so a wrong-looking part
// of the picture (thug-frame-NNN.bmp) can be matched to its draw.
static unsigned s_capDraw;
static void LogCapturedDraw(IDirect3DDevice9* dev, const DrawCall& call, const FvfLayout& l, u32 count, u32 stride,
	const RsxTexture* const* tex, const FpEntry* fp, bool hasDepth)
{
	const DWORD* rs = dev->m_rs;
	char line[1536];
	int n = snprintf(line, sizeof(line), "[cap] #%u prim %d x%u verts %u%s fvf 0x%x stride %u %s%s | blend %d %d/%d op %d | atest %d f%d ref %d | z %d w%d f%d | cull %d | tfactor %08x | vp %d,%d %dx%d",
		s_capDraw++, (int)call.type, (unsigned)call.primCount, (unsigned)count, call.vertexData ? " UP" : "", (unsigned)dev->m_fvf, (unsigned)stride,
		dev->m_ps ? "ps" : "ff", fp && fp->texMask ? "" : " (no textures sampled)",
		(int)rs[D3DRS_ALPHABLENDENABLE], (int)rs[D3DRS_SRCBLEND], (int)rs[D3DRS_DESTBLEND], (int)rs[D3DRS_BLENDOP],
		(int)rs[D3DRS_ALPHATESTENABLE], (int)rs[D3DRS_ALPHAFUNC], (int)(rs[D3DRS_ALPHAREF] & 0xFF),
		(int)(rs[D3DRS_ZENABLE] && hasDepth), (int)rs[D3DRS_ZWRITEENABLE], (int)rs[D3DRS_ZFUNC], (int)rs[D3DRS_CULLMODE],
		(unsigned)rs[D3DRS_TEXTUREFACTOR], (int)dev->m_viewport.X, (int)dev->m_viewport.Y, (int)dev->m_viewport.Width, (int)dev->m_viewport.Height);
	// The first vertex, as the CPU wrote it.
	const u8* v0 = NULL;
	if (call.vertexData) v0 = (const u8*)call.vertexData + (call.indexed ? call.minIndex * stride : 0);
	else if (dev->m_streams[0])
		v0 = dev->m_streams[0]->m_data ? dev->m_streams[0]->m_data + dev->m_streamOffset[0] + (call.indexed ? call.baseVertex * stride : call.startVertex * stride) : NULL;	// NULL: released (see ReleaseVertexBufferData)
	if (v0)
	{
		float p[4] = { 0, 0, 0, 0 };
		memcpy(p, v0 + l.pos, l.posSize * 4);
		n += snprintf(line + n, sizeof(line) - n, " | v0 pos %.1f,%.1f,%.2f", p[0], p[1], p[2]);
		if (l.diffuse >= 0) { u32 c; memcpy(&c, v0 + l.diffuse, 4); n += snprintf(line + n, sizeof(line) - n, " col %08x", (unsigned)c); }
		if (l.numTex > 0) { float uv[2]; memcpy(uv, v0 + l.tex[0], 8); n += snprintf(line + n, sizeof(line) - n, " uv %.3f,%.3f", uv[0], uv[1]); }
		// The next two vertices too (a degenerate triangle draws nothing).
		for (int k = 1; k <= 2 && k < (int)count; ++k)
		{
			const u8* vk = v0 + k * stride;
			float q[4] = { 0, 0, 0, 0 }, uv[2] = { 0, 0 };
			memcpy(q, vk + l.pos, l.posSize * 4);
			if (l.numTex > 0) memcpy(uv, vk + l.tex[0], 8);
			n += snprintf(line + n, sizeof(line) - n, " | v%d %.1f,%.1f rhw %.2f uv %.3f,%.3f", k, q[0], q[1], l.posSize > 3 ? q[3] : 1.0f, uv[0], uv[1]);
		}
	}
	printf("%s\n", line);
	for (int s = 0; s < 4; ++s)
	{
		const DWORD* ts = dev->m_tss[s];
		IDirect3DBaseTexture9* bt = dev->m_textures[s];
		if (!bt && (s > 0 || ts[D3DTSS_COLOROP] == D3DTOP_DISABLE)) { if (s == 0) printf("[cap]   stage 0 disabled, no texture\n"); break; }
		char tx[256] = "no texture";
		if (bt && bt->GetType() == D3DRTYPE_TEXTURE)
		{
			IDirect3DTexture9* t = static_cast<IDirect3DTexture9*>(bt);
			int tn = snprintf(tx, sizeof(tx), "tex %p fmt %d %ux%u levels %u usage 0x%x%s%s", (void*)t, (int)t->m_format, t->m_width, t->m_height, t->m_levels,
				(unsigned)t->m_usage, tex[s] ? "" : " NOT ON RSX (drawn white)", tex[s] && tex[s]->renderTarget ? " render target" : "");
			// What the CPU copy holds (32-bit formats): texels with any alpha, and the brightest.
			if ((t->m_format == D3DFMT_A8R8G8B8 || t->m_format == D3DFMT_X8R8G8B8) && t->LevelBits(0))
			{
				const u8* bits = (const u8*)t->LevelBits(0);
				const u32 pitch = t->LevelPitch(0);
				u32 withAlpha = 0, maxA = 0, maxRgb = 0;
				for (u32 y = 0; y < t->m_height; ++y)
					for (u32 x = 0; x < t->m_width; ++x)
					{
						u32 c; memcpy(&c, bits + y * pitch + x * 4, 4);
						u32 a = c >> 24, m = ((c >> 16) & 0xFF) > (c & 0xFF) ? (c >> 16) & 0xFF : c & 0xFF;
						if (a) ++withAlpha;
						if (a > maxA) maxA = a;
						if (m > maxRgb) maxRgb = m;
					}
				snprintf(tx + tn, sizeof(tx) - tn, " [%u texels with alpha, max alpha %02x, max r/b %02x]", withAlpha, maxA, maxRgb);
			}
		}
		else if (bt) snprintf(tx, sizeof(tx), "texture of type %d (not 2D: not supported)", (int)bt->GetType());
		const DWORD* ss = dev->m_ss[s];
		printf("[cap]   stage %d: %s | color op %d %x %x alpha op %d %x %x | tci %x ttf %x | addr %d %d filt %d %d %d%s\n", s, tx,
			(int)ts[D3DTSS_COLOROP], (unsigned)ts[D3DTSS_COLORARG1], (unsigned)ts[D3DTSS_COLORARG2],
			(int)ts[D3DTSS_ALPHAOP], (unsigned)ts[D3DTSS_ALPHAARG1], (unsigned)ts[D3DTSS_ALPHAARG2],
			(unsigned)ts[D3DTSS_TEXCOORDINDEX], (unsigned)ts[D3DTSS_TEXTURETRANSFORMFLAGS],
			(int)ss[D3DSAMP_ADDRESSU], (int)ss[D3DSAMP_ADDRESSV], (int)ss[D3DSAMP_MINFILTER], (int)ss[D3DSAMP_MAGFILTER], (int)ss[D3DSAMP_MIPFILTER],
			fp && (fp->texMask & (1 << s)) ? "" : " (not sampled)");
		if (ts[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
	}
}

// A draw's vertex and index data on the RSX (uploaded or renamed as needed): the vertex
// base, the stride and the index offset.
static bool DrawBuffers(IDirect3DDevice9* dev, const DrawCall& call, const FvfLayout& l, u32 count,
	u32* vbaseOut, u32* strideOut, u32* ioffOut)
{
	u32 vbase, stride, ioff = 0;
	if (call.vertexData)
	{
		stride = call.vertexStride;
		const u32 verts = call.indexed ? call.minIndex + call.vertexCount : call.vertexCount;
		u8* p = RingAlloc(verts * stride, 128, &vbase);
		if (!p) return false;
		memcpy(p, call.vertexData, verts * stride);
		s_stats.ringBytes += verts * stride;
		if (call.indexed)
		{
			const u32 isize = count * (call.indexFormat == D3DFMT_INDEX32 ? 4 : 2);
			u8* ip = RingAlloc(isize, 16, &ioff);
			if (!ip) return false;
			memcpy(ip, call.indexData, isize);
			s_stats.ringBytes += isize;
		}
		s_vertexWritten = true;
	}
	else
	{
		IDirect3DVertexBuffer9* vb = dev->m_streams[0];
		if (!vb) return Skip("no vertex buffer");
		stride = dev->m_streamStride[0] ? dev->m_streamStride[0] : l.stride;
		IDirect3DIndexBuffer9* ib = call.indexed ? dev->m_indices : NULL;
		if (call.indexed && !ib) return Skip("no index buffer");
		const u32 isz = ib && ib->m_format == D3DFMT_INDEX32 ? 4 : 2;

		// The bytes this draw reads, for a buffer that may have to be renamed into the ring
		// (only those are copied there). Indexed draws scan their indices for the range
		// rather than trusting MinIndex/NumVertices, which PC drivers ignore. Anything odd
		// falls back to the whole buffer.
		u32 vLo = 0, vHi = vb->m_size;
		const RsxBuffer* vbb = (const RsxBuffer*)vb->m_backend;
		if (vb->m_dirty || (vbb && vbb->stale))
		{
			long long first = -1, last = -1;
			if (!call.indexed)
			{
				first = call.startVertex;
				last = (long long)call.startVertex + count - 1;
			}
			else if (((unsigned long long)call.startIndex + count) * isz <= ib->m_size && ib->m_data)
			{
				u32 mn, mx;
				IndexRange(ib->m_data + call.startIndex * isz, isz == 4, count, &mn, &mx);
				first = (long long)call.baseVertex + mn;
				last = (long long)call.baseVertex + mx;
			}
			const long long lo = (long long)dev->m_streamOffset[0] + first * stride;
			long long hi = (long long)dev->m_streamOffset[0] + last * stride + stride;
			if (hi > (long long)vb->m_size) hi = vb->m_size;
			if (first >= 0 && last >= first && lo >= 0 && lo < hi) { vLo = (u32)lo; vHi = (u32)hi; }
		}
		u32 off;
		if (!BufferOffset(vb->m_data, vb->m_size, vb->m_dirty, vb->m_changes, vb->m_backend, vLo, vHi, &off)) return false;
		if (vb->m_data) ReleaseVertexBufferData(vb);
		vbase = off + dev->m_streamOffset[0];
		if (call.indexed)
		{
			vbase += call.baseVertex * stride;
			const u32 iLo = call.startIndex * isz;
			if (!BufferOffset(ib->m_data, ib->m_size, ib->m_dirty, ib->m_changes, ib->m_backend, iLo, iLo + count * isz, &ioff)) return false;
			ioff += iLo;
		}
		else
			vbase += call.startVertex * stride;
	}
	*vbaseOut = vbase;
	*strideOut = stride;
	*ioffOut = ioff;
	return true;
}

// ---------------------------------------------------------------------------
// Draw packets: a draw's commands, recorded once and replayed
// ---------------------------------------------------------------------------
// Most of a frame's draws are the level's meshes, drawn the same way frame after frame: the
// same buffers, textures and render state. Working each one out again cost about 4 us a
// draw, 8-10 ms in a busy view. So an indexed draw from buffers records the commands it sends
// for its render state, view, fragment program, textures and vertex attributes, and the draw
// itself, sent with every cache in this file reset first, so the block stands on its own. Next
// time the same draw (same buffers and range) comes with exactly the same device state
// (compared whole: render, stage and sampler states, textures, shaders and their constants,
// viewport, scissor, targets, FVF, lights), the same texture layouts and the same buffer
// offsets, the block is copied into the command buffer instead. Only the fragment program's
// address is patched: it still gets a fresh copy every draw (reusing copies flashes). The
// vertex program and its constants are sent before the block, as for any draw. Anything that
// doesn't match records the draw again; draws that keep changing stop being recorded.
// rsx_no_packets.txt turns them off.
static bool s_noPackets;

struct DrawPacket
{
	// What the commands were made from.
	DWORD rs[D3DRS_MAX];
	DWORD tss[4][D3DTSS_MAX];
	DWORD ss[4][D3DSAMP_MAX];
	IDirect3DBaseTexture9* textures[4];
	float psConst[8][4];
	D3DVIEWPORT9 viewport;
	RECT scissor;
	DWORD fvf;
	IDirect3DVertexDeclaration9* decl;
	IDirect3DVertexShader9* vs;
	IDirect3DPixelShader9* ps;
	IDirect3DSurface9* renderTarget;
	IDirect3DSurface9* depthTarget;
	BOOL lightEnabled[8];
	D3DLIGHTTYPE lightType[8];
	UINT streamOffset, streamStride;
	bool hasDepth, i32;
	u32 vbase, stride, ioff;
	const RsxTexture* tex[4];	// bound per unit (NULL: unit unused)
	gcmTexture texDesc[4];
	FpEntry* fp;
	float fc[9][4];
	VpEntry* vp;
	FvfLayout layout;
	bool zcull, farWrite;		// ZCULL on for the draw / it writes depth farther than what is there
	u32 epoch;					// s_packetEpoch when recorded
	u32 lastUse;				// frame of the last replay
	u32 fpWord;					// index of the fragment program address in cmds
	u32 words;
	u32 cmds[1];				// (words of them)
};

struct PacketSlot
{
	IDirect3DVertexBuffer9* vb;
	IDirect3DIndexBuffer9* ib;
	INT baseVertex;
	UINT startIndex, primCount;
	u32 type;
	DrawPacket* p[2];			// two: a mesh drawn in two passes keeps both
	u16 records, replays;		// since the last check (recording stops for draws that keep changing)
	u32 noRecordUntil;			// frame
};
enum { kPacketSlots = 8192, kMaxPacketWords = 1024 };
static PacketSlot* s_slots;
static u32 s_packetEpoch = 1;	// bumped when a pixel shader goes (its programs are freed)
static size_t s_packetBytes;
static const size_t kMaxPacketBytes = 16u << 20;

static bool PacketMatches(const DrawPacket* p, IDirect3DDevice9* dev, bool hasDepth)
{
	if (p->epoch != s_packetEpoch || p->hasDepth != hasDepth) return false;
	if (p->fvf != dev->m_fvf || p->decl != dev->m_decl || p->vs != dev->m_vs || p->ps != dev->m_ps ||
		p->renderTarget != dev->m_renderTarget || p->depthTarget != dev->m_depthTarget ||
		p->streamOffset != dev->m_streamOffset[0] || p->streamStride != dev->m_streamStride[0])
		return false;
	if (memcmp(p->textures, dev->m_textures, sizeof(p->textures))) return false;
	if (memcmp(p->rs, dev->m_rs, sizeof(p->rs))) return false;
	if (memcmp(p->tss, dev->m_tss, sizeof(p->tss))) return false;
	if (memcmp(p->ss, dev->m_ss, sizeof(p->ss))) return false;
	if (memcmp(p->psConst, dev->m_psConst, sizeof(p->psConst))) return false;
	if (memcmp(&p->viewport, &dev->m_viewport, sizeof(p->viewport)) || memcmp(&p->scissor, &dev->m_scissor, sizeof(p->scissor))) return false;
	if (memcmp(p->lightEnabled, dev->m_lightEnabled, sizeof(p->lightEnabled))) return false;
	for (int i = 0; i < 8; ++i) if (p->lightEnabled[i] && p->lightType[i] != dev->m_lights[i].Type) return false;
	// The textures as uploaded now (a texture written since is uploaded on the full path).
	for (int t = 0; t < 4; ++t)
	{
		if (!p->tex[t] || p->tex[t] == &s_white) continue;
		IDirect3DBaseTexture9* bt = dev->m_textures[t];
		if (!bt || bt->GetType() != D3DRTYPE_TEXTURE) return false;
		IDirect3DTexture9* tx = static_cast<IDirect3DTexture9*>(bt);
		const RsxTexture* r = (const RsxTexture*)tx->m_backend;
		if (tx->m_dirty || r != p->tex[t] || !r->valid || memcmp(&r->desc, &p->texDesc[t], sizeof(r->desc))) return false;
	}
	return true;
}

static PacketSlot* SlotFor(IDirect3DDevice9* dev, const DrawCall& call)
{
	if (!s_slots) return NULL;
	IDirect3DVertexBuffer9* vb = dev->m_streams[0];
	IDirect3DIndexBuffer9* ib = dev->m_indices;
	unsigned long long h = (unsigned long long)(uintptr_t)vb * 0x9E3779B97F4A7C15ull;
	h ^= ((unsigned long long)(uintptr_t)ib + call.startIndex * 31u + call.primCount * 131u + (u32)call.baseVertex * 7u + call.type) * 0xC2B2AE3D27D4EB4Full;
	PacketSlot* slot = &s_slots[(h >> 40) & (kPacketSlots - 1)];
	if (slot->vb != vb || slot->ib != ib || slot->startIndex != call.startIndex || slot->primCount != call.primCount ||
		slot->baseVertex != call.baseVertex || slot->type != (u32)call.type)
	{
		// Another draw: the slot goes to this one.
		for (int k = 0; k < 2; ++k) if (slot->p[k]) { s_packetBytes -= sizeof(DrawPacket) + slot->p[k]->words * 4; free(slot->p[k]); }
		memset(slot, 0, sizeof(*slot));
		slot->vb = vb;
		slot->ib = ib;
		slot->startIndex = call.startIndex;
		slot->primCount = call.primCount;
		slot->baseVertex = call.baseVertex;
		slot->type = (u32)call.type;
	}
	return slot;
}

// The vertex program and its constants, for a draw (sent before its block).
static void VertexProgramAndConstants(IDirect3DDevice9* dev, const VpEntry* vp)
{
	if (dev->m_vsSerial != s_vcSerial || s_resendPrograms || s_vsAlways)
	{
		float vc[kVcCount][4];
		FillVsConstants(dev, vc);
		UseVertexProgram(vp, vc);
		s_vcSerial = dev->m_vsSerial;
	}
	else
		UseVertexProgram(vp, NULL);
}

// After a block: the RSX's state is the block's, which the caches here don't know.
static void ForgetSentState()
{
	s_stateValid = false;
	s_viewValid = false;
	s_clipsSent = false;
	s_attrValid = false;
	s_attribMask = 0xFFFFFFFF;
	memset(s_bound, 0, sizeof(s_bound));
	s_currentFpOffset = 0xFFFFFFFF;
}

static bool ReplayPacket(IDirect3DDevice9* dev, const DrawCall& call, PacketSlot* slot, u32 count, bool hasDepth)
{
	DrawPacket* p = NULL;
	for (int k = 0; k < 2 && !p; ++k)
		if (slot->p[k] && PacketMatches(slot->p[k], dev, hasDepth)) p = slot->p[k];
	if (!p) return false;
	u32 vbase, stride, ioff;
	if (!DrawBuffers(dev, call, p->layout, count, &vbase, &stride, &ioff)) return false;
	if (vbase != p->vbase || stride != p->stride || ioff != p->ioff || p->i32 != (dev->m_indices->m_format == D3DFMT_INDEX32)) return false;

	VertexProgramAndConstants(dev, p->vp);
	// The fragment program: a fresh copy, as for any draw.
	u32 offset = p->fp->localOffset;
	if (p->fp->numConsts)
	{
		u8* fresh = RingAlloc(p->fp->ucodeSize, 64, &offset);
		if (!fresh) return false;
		WritePatched(p->fp, fresh, p->fc, p->fp->last);
		p->fp->lastFrame = s_frameSeq;
		p->fp->lastOffset = offset;
	}
	else
		p->fp->localUsed = s_frameSeq + 1;
	CmdReserve(p->words + 32);
	if (s_skipFrame) return true;
	if (s_vertexWritten)
	{
		rsxInvalidateVertexCache(s_ctx);
		s_vertexWritten = false;
	}
	if (p->zcull && s_zcullStale)
	{
		rsxSetZCullInvalidate(s_ctx);
		s_zcullStale = false;
	}
	u32* out = (u32*)s_ctx->current;
	memcpy(out, p->cmds, p->words * 4);
	out[p->fpWord] = (GCM_LOCATION_RSX + 1) | (offset & 0x1fffffff);
	s_ctx->current = out + p->words;
	ForgetSentState();
	s_currentFpOffset = offset;
	if (p->farWrite) s_zcullStale = true;
	for (int t = 0; t < 4; ++t) if (p->tex[t]) const_cast<RsxTexture*>(p->tex[t])->usedPlusOne = s_frameSeq + 1;
	p->lastUse = s_frameSeq;
	if (slot->replays < 60000) ++slot->replays;
	++s_stats.drawn;
	++s_stats.replayed;
	if (++s_drawsSinceKick >= kKickDraws) Kick();
	return true;
}

static void RecordPacket(IDirect3DDevice9* dev, PacketSlot* slot, const u32* start, u32 segBefore, bool hasDepth,
	const FvfLayout& l, const RsxTexture* const* tex, FpEntry* fp, const float (*fc)[4], VpEntry* vp, u32 vbase, u32 stride, u32 ioff)
{
	const u32* end = (const u32*)s_ctx->current;
	if (SegmentChanges() != segBefore || end <= start || (u32)(end - start) > kMaxPacketWords) { ++s_stats.packetFails; return; }
	const u32 words = (u32)(end - start);
	// The fragment program's address: the word after its FP_ADDRESS method.
	u32 fpWord = 0xFFFFFFFF;
	for (u32 i = 0; i + 1 < words; ++i)
		if (start[i] == ((1u << 18) | 0x08E4u))	// RSX_METHOD(NV40TCL_FP_ADDRESS, 1), as librsx writes it { fpWord = i + 1; break; }
	if (fpWord == 0xFFFFFFFF) { ++s_stats.packetFails; return; }
	const size_t bytes = sizeof(DrawPacket) + words * 4;
	if (s_packetBytes + bytes > kMaxPacketBytes) { ++s_stats.packetFails; return; }
	DrawPacket* p = (DrawPacket*)malloc(bytes);
	if (!p) { ++s_stats.packetFails; return; }
	s_packetBytes += bytes;
	memcpy(p->rs, dev->m_rs, sizeof(p->rs));
	memcpy(p->tss, dev->m_tss, sizeof(p->tss));
	memcpy(p->ss, dev->m_ss, sizeof(p->ss));
	memcpy(p->textures, dev->m_textures, sizeof(p->textures));
	memcpy(p->psConst, dev->m_psConst, sizeof(p->psConst));
	p->viewport = dev->m_viewport;
	p->scissor = dev->m_scissor;
	p->fvf = dev->m_fvf;
	p->decl = dev->m_decl;
	p->vs = dev->m_vs;
	p->ps = dev->m_ps;
	p->renderTarget = dev->m_renderTarget;
	p->depthTarget = dev->m_depthTarget;
	memcpy(p->lightEnabled, dev->m_lightEnabled, sizeof(p->lightEnabled));
	for (int i = 0; i < 8; ++i) p->lightType[i] = dev->m_lights[i].Type;
	p->streamOffset = dev->m_streamOffset[0];
	p->streamStride = dev->m_streamStride[0];
	p->hasDepth = hasDepth;
	p->i32 = dev->m_indices->m_format == D3DFMT_INDEX32;
	p->vbase = vbase;
	p->stride = stride;
	p->ioff = ioff;
	for (int t = 0; t < 4; ++t)
	{
		p->tex[t] = (fp->texMask & (1 << t)) ? (tex[t] ? tex[t] : &s_white) : NULL;
		if (p->tex[t]) p->texDesc[t] = p->tex[t]->desc; else memset(&p->texDesc[t], 0, sizeof(p->texDesc[t]));
	}
	p->fp = fp;
	memcpy(p->fc, fc, sizeof(p->fc));
	p->vp = vp;
	p->layout = l;
	p->zcull = s_state.zcull != 0;
	p->farWrite = ZcullActive() && s_state.depthWrite && OnDisplayDepth() && FarDepthWrite(s_state.depthFunc);
	p->epoch = s_packetEpoch;
	p->lastUse = s_frameSeq;
	p->fpWord = fpWord;
	p->words = words;
	memcpy(p->cmds, start, words * 4);
	// Into the empty way, else over the one replayed longest ago.
	int k = !slot->p[0] ? 0 : !slot->p[1] ? 1 : (slot->p[0]->lastUse <= slot->p[1]->lastUse ? 0 : 1);
	if (slot->p[k]) { s_packetBytes -= sizeof(DrawPacket) + slot->p[k]->words * 4; free(slot->p[k]); }
	slot->p[k] = p;
	++slot->records;
	++s_stats.recorded;
	// A draw recorded over and over without being replayed much: leave it alone for a while.
	if (slot->records >= 8)
	{
		if (slot->replays < slot->records * 4) slot->noRecordUntil = s_frameSeq + 600;
		slot->records = slot->replays = 0;
	}
}

static bool DoDraw(IDirect3DDevice9* dev, const DrawCall& call)
{
	unsigned long long dpLast = __builtin_ppc_get_timebase();
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

	// A recorded draw (see "Draw packets").
	PacketSlot* slot = NULL;
	if (!s_noPackets && call.indexed && !call.vertexData && !s_capture && !s_resendState && !s_resendPrograms &&
		dev->m_streams[0] && dev->m_indices)
	{
		slot = SlotFor(dev, call);
		if (slot && ReplayPacket(dev, call, slot, count, hasDepth)) return true;
		if (slot && (s32)(s_frameSeq - slot->noRecordUntil) < 0) slot = NULL;
	}

	// Textures for the fragment program.
	const RsxTexture* tex[4] = { NULL, NULL, NULL, NULL };
	for (int s = 0; s < 4; ++s)
	{
		IDirect3DBaseTexture9* bt = dev->m_textures[s];
		if (bt && bt->GetType() == D3DRTYPE_TEXTURE)
			tex[s] = Texture(static_cast<IDirect3DTexture9*>(bt));
	}

	// Fragment program and its constants. The key is plain data: a pixel shader by pointer
	// (and fog), or the fixed-function stages (FillFsKey, once per draw).
	FpEntry* fp = NULL;
	float fc[9][4];
	memset(fc, 0, sizeof(fc));
	const bool fog = FogEnabled(dev);
	int numTexOut = 0;
	FpKey key;
	memset(&key, 0, sizeof(key));
	if (dev->m_ps)
	{
		key.kind = 1;
		key.fog = fog ? 1 : 0;
		key.ps = (u64)(uintptr_t)dev->m_ps;
	}
	else
	{
		const bool has[4] = { tex[0] != NULL, tex[1] != NULL, tex[2] != NULL, tex[3] != NULL };
		FillFsKey(dev, key.fs, has);
	}
	if (s_lastFp && !memcmp(&key, &s_lastFpKey, sizeof(key)))
		fp = s_lastFp;
	else
	{
		fp = s_fps.Find(key);
		if (!fp && dev->m_ps)
		{
			const char* src = PixelShaderSource(dev->m_ps);
			if (!src) return Skip("pixel shader without source");
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
		else if (!fp)
		{
			int mask = 0;
			for (int s = 0; s < key.fs.numStages; ++s) if (key.fs.stage[s].hasTexture) mask |= 1 << s;
			fp = FragmentProgramFrom(key, FixedFunctionFragmentProgram(key.fs), mask);
		}
		if (fp)
		{
			memcpy(&s_lastFpKey, &key, sizeof(key));
			s_lastFp = fp;
		}
	}
	if (dev->m_ps)
	{
		for (int i = 0; i < 8; ++i) memcpy(fc[i], dev->m_psConst[i], 16);
		ColorToFloats(dev->m_rs[D3DRS_FOGCOLOR], fc[kFcPsFogColor]);
	}
	else
	{
		ColorToFloats(dev->m_rs[D3DRS_TEXTUREFACTOR], fc[kFcTextureFactor]);
		ColorToFloats(dev->m_rs[D3DRS_FOGCOLOR], fc[kFcFogColor]);
	}
	if (!fp || !fp->prog) return Skip("fragment program unavailable");
	for (int t = 0; t < 4; ++t) if (fp->texMask & (1 << t)) numTexOut = t + 1;
	if (!dev->m_ps && key.fs.numStages > numTexOut) numTexOut = key.fs.numStages;

	DP_MARK(kDpKeys);
	// Vertex program.
	VsKey vk;
	FillVsKey(dev, l, numTexOut, vk);
	VpEntry* vp = VertexProgramFor(vk);
	if (!vp || !vp->prog) return Skip("vertex program unavailable");
	DP_MARK(kDpVp);

	// Vertex and index data on the RSX.
	u32 vbase, stride, ioff = 0;
	if (!DrawBuffers(dev, call, l, count, &vbase, &stride, &ioff)) return false;
	if (stride > 255) return Skip("vertex stride over 255 bytes");
	DP_MARK(kDpBuffers);

	// Programs and constants, state, textures, then the draw.
	if (s_resendState) { s_stateValid = false; s_viewValid = false; s_clipsSent = false; }
	if (s_resendPrograms) { s_currentVp = NULL; s_currentFpOffset = 0xFFFFFFFF; memset(s_vcValid, 0, sizeof(s_vcValid)); }
	if (s_rebindAttribs) { s_attrValid = false; s_attribMask = 0xFFFFFFFF; }
	VertexProgramAndConstants(dev, vp);
	DP_MARK(kDpVsConst);
	// Only after vertex data was written (a175dd3; steady once fragment programs stopped being reused).
	if (s_vertexWritten)
	{
		CmdReserve(8);
		rsxInvalidateVertexCache(s_ctx);
		s_vertexWritten = false;
	}
	// Recording this draw: everything from here is sent in full, into one stretch of the
	// command buffer (room is made first; a segment change on the way spoils the recording).
	const u32* recStart = NULL;
	u32 recSegs = 0;
	if (slot && !s_skipFrame)
	{
		if (ZcullActive() && s_zcullStale)
		{
			CmdReserve(8);
			rsxSetZCullInvalidate(s_ctx);
			s_zcullStale = false;
		}
		CmdReserve(kMaxPacketWords + 64);
		ForgetSentState();
		recStart = (const u32*)s_ctx->current;
		recSegs = SegmentChanges();
	}
	ApplyView(dev);
	ApplyState(dev, hasDepth);
	DP_MARK(kDpState);
	if (!UseFragmentProgram(fp, fc)) return false;
	DP_MARK(kDpFp);
	for (int t = 0; t < 4; ++t)
		if (fp->texMask & (1 << t))
			BindTexture(dev, t, tex[t] ? tex[t] : &s_white);
	BindAttributes(l, vbase, stride);
	DP_MARK(kDpBind);

	if (s_capture) LogCapturedDraw(dev, call, l, count, stride, tex, fp, hasDepth);
	if (s_skipFrame) return false;	// the command buffer ran out on the way here: the frame is dropped
	CmdReserve(64 + count / 256 * 2);
	if (call.indexed)
	{
		const bool i32 = call.vertexData ? call.indexFormat == D3DFMT_INDEX32 : dev->m_indices->m_format == D3DFMT_INDEX32;
		rsxDrawIndexArray(s_ctx, type, ioff, count, i32 ? GCM_INDEX_TYPE_32B : GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
	}
	else
		rsxDrawVertexArray(s_ctx, type, 0, count);
	if (recStart && !s_skipFrame) RecordPacket(dev, slot, recStart, recSegs, hasDepth, l, tex, fp, fc, vp, vbase, stride, ioff);
	++s_stats.drawn;
	if (++s_drawsSinceKick >= kKickDraws) Kick();
	DP_MARK(kDpSubmit);
	return true;
}

void Draw(IDirect3DDevice9* dev, const DrawCall& call)
{
	DeviceGuard guard;
	if (!s_drawReady || s_skipFrame) return;
	if (s_capture && dev->m_renderTarget && dev->m_renderTarget != dev->m_backBuffer)
		printf("[cap] (next draw goes to render target %ux%u format %d)\n", dev->m_renderTarget->m_width, dev->m_renderTarget->m_height, (int)dev->m_renderTarget->m_format);
	++s_stats.draws;
	const unsigned long long t0 = __builtin_ppc_get_timebase();
	if (s_slowDraws)
		while (__builtin_ppc_get_timebase() - t0 < 200) {}	// 200 ticks of 79.8 MHz: 2.5 us
	DoDraw(dev, call);
	const unsigned long long dt = __builtin_ppc_get_timebase() - t0;
	s_dpTotal += dt;
	::g_ps3StageTicks[9] += dt;	// Mainloop.cpp's slow-frame profile: the draw calls
}

// ---------------------------------------------------------------------------
// Set-up, frames, resource notifications
// ---------------------------------------------------------------------------
// Surfaces the game creates on their own (CreateRenderTarget, CreateDepthStencilSurface).
struct RsxSurface { u8* mem; u32 offset, pitch; u32 usedPlusOne; };

// RSX memory behind a D3D surface: a render-target texture's level 0, or a standalone surface.
static bool SurfaceMemory(IDirect3DSurface9* surf, u32* offset, u32* pitch)
{
	if (surf->m_owner)
	{
		if (surf->m_level != 0) return false;
		RsxTexture* r = Texture(surf->m_owner);
		if (!r || !r->renderTarget) return false;
		r->usedPlusOne = s_frameSeq + 1;	// drawn into this frame: its memory stays until the RSX is done
		*offset = r->offset;
		*pitch = r->pitch;
		return true;
	}
	RsxSurface* rs = (RsxSurface*)surf->m_backend;
	if (!rs)
	{
		rs = NewRecord<RsxSurface>();
		if (!rs) { LogLimited("surfrec", 3, "out of memory for a surface record"); return false; }
		rs->pitch = (surf->m_width * 4 + 63) & ~63u;
		rs->mem = (u8*)LocalAlloc(rs->pitch * surf->m_height, 4096, &rs->offset);
		if (!rs->mem) { FreeRecord(rs); LogLimited("surfmem", 5, "out of RSX memory for a %ux%u surface", surf->m_width, surf->m_height); return false; }
		memset(rs->mem, 0, rs->pitch * surf->m_height);
		surf->m_backend = rs;
	}
	rs->usedPlusOne = s_frameSeq + 1;
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
	// A new target: what was cached for the last one is sent again (the old code re-sent
	// all of it with each change), and what was queued for the last target goes to the RSX.
	s_stateValid = false;
	s_clipsSent = false;
	s_attrValid = false;
	Kick();
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
		if (s_targetValid && !s_noTargetWait)
		{
			// Leaving a texture the game drew into (shadows, mostly): the RSX finishes those
			// draws before the next ones start, as they may sample it. Without the wait a draw
			// can read the texture while it is still being written, which only shows when the
			// RSX has work queued back to back, i.e. when the CPU keeps it fed.
			SurfaceDesc disp;
			memset(&disp, 0, sizeof(disp));
			DisplaySurface(&disp);
			if (s_target.colorOffset != disp.colorOffset)
			{
				CmdReserve(8);
				rsxSetWaitForIdle(s_ctx);
				++s_stats.targetWaits;
			}
		}
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
	if (s_flipCull) GpuLog("rsx_flip_cull.txt: the old cull mapping");
	if (s_noDraw) GpuLog("rsx_no_draw.txt: nothing is drawn");
	s_keepTextureData = FlagFile("rsx_keep_texture_data.txt");
	s_keepBufferData = FlagFile("rsx_keep_buffer_data.txt");
	s_freshDone = FlagFile("rsx_fresh_label.txt");
	s_vsAlways = FlagFile("rsx_vs_always.txt");
	s_slowDraws = FlagFile("rsx_slow_draws.txt");
	s_noTargetWait = FlagFile("rsx_no_target_wait.txt");
	s_fpReuse = FlagFile("rsx_fp_reuse.txt");
	s_noPackets = FlagFile("rsx_no_packets.txt");
	if (!s_noPackets) s_slots = (PacketSlot*)calloc(kPacketSlots, sizeof(PacketSlot));
	GpuLog("draw packets: %s", s_noPackets ? "off (rsx_no_packets.txt)" : s_slots ? "on" : "off (no memory for the table)");
	if (s_fpReuse) GpuLog("rsx_fp_reuse.txt: fragment programs reused instead of a fresh copy per draw");
	if (s_noTargetWait) GpuLog("rsx_no_target_wait.txt: no RSX idle wait after drawing into a texture");
	if (s_slowDraws) GpuLog("rsx_slow_draws.txt: every draw spins 2.5 us first");
	if (s_freshDone) GpuLog("rsx_fresh_label.txt: the fence label is read every time");
	if (s_vsAlways) GpuLog("rsx_vs_always.txt: vertex constants recomputed for every draw");
	GpuLog("draw path: a175dd3 savings on, a fresh fragment-program copy per draw");
	GpuLog("main-memory copies of uploaded textures: %s (rsx_keep_texture_data.txt); of static vertex buffers: %s (rsx_keep_buffer_data.txt)",
		s_keepTextureData ? "kept" : "released after 30 unchanged frames", s_keepBufferData ? "kept" : "released after 30 unchanged frames");

	s_ring = (u8*)LocalAlloc(kRingSize * kRingFrames, 1024 * 1024, &s_ringOffset);
	if (!s_ring || !MakeWhite())
	{
		GpuLog("out of RSX memory for the frame ring");
		return false;
	}
	CmdReserve(64);
	// Clip triangles at the near and far planes, as D3D does. Depth clamping instead (what
	// the PSL1GHT samples set) doesn't clip at the near plane, and big triangles with corners
	// behind the camera (the ground around the skater, the sky) weren't drawn at all.
	// rsx_depth_clamp.txt brings the old setting back for comparison.
	const bool clamp = FlagFile("rsx_depth_clamp.txt");
	if (clamp) GpuLog("rsx_depth_clamp.txt: depth clamp on, no near/far clipping");
	rsxSetZMinMaxControl(s_ctx, clamp ? GCM_FALSE : GCM_TRUE, clamp ? GCM_TRUE : GCM_FALSE, GCM_FALSE);
	rsxSetUserClipPlaneControl(s_ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	rsxSetColorMaskMrt(s_ctx, 0);
	s_attribMask = 0xFFFFFFFF;
	{
		SurfaceDesc disp;
		memset(&disp, 0, sizeof(disp));
		DisplaySurface(&disp);
		s_displayDepthOffset = disp.depthOffset;
	}
	s_drawReady = true;
	GpuLog("draw path ready: %u KB frame ring x %d in RSX memory", kRingSize / 1024, kRingFrames);
	return true;
}

void DrawFrameStart()
{
	if (!s_drawReady) return;
	s_capDraw = 0;
	++s_dpFrames;
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
	s_vcSerial = 0;
	s_doneCache = FramesDone();
	s_doneAge = 0;
	memset(s_bound, 0, sizeof(s_bound));
	s_attribMask = 0xFFFFFFFF;
	s_attrValid = false;
	s_clipsSent = false;
	s_vertexWritten = false;	// invalidated just below
	s_drawsSinceKick = 0;
	CmdReserve(32);
	rsxInvalidateTextureCache(s_ctx, GCM_INVALIDATE_TEXTURE);
	rsxInvalidateVertexCache(s_ctx);
}

const char* DrawStats()
{
	snprintf(s_statsLine, sizeof(s_statsLine),
		"%u draws, %u drawn, %u skipped, %u to unusable targets; %u texture uploads (%u KB, %u new blocks), %u KB through the ring, %u programs (%u failed), "
		"%u fragment-program patches, ring full %u (%u KB on the side), %u kicks, %u waits after texture targets, draw packets: %u replayed, %u recorded, %u not recordable (%u KB), %u RSX frees waiting; "
		"%u textures, %u KB of them in main RAM; copies released after upload: %u textures (%u back), %u KB, %u vertex buffers (%u back), %u KB",
		s_stats.draws, s_stats.drawn, s_stats.skipped, s_stats.offscreen, s_stats.uploads, s_stats.uploadKB, s_stats.texBlocks,
		(unsigned)(s_stats.ringBytes / 1024), s_stats.programs, s_stats.programFailures,
		s_stats.fpPatches, s_stats.ringFull, s_stats.overflowKB, s_stats.kicks, s_stats.targetWaits, s_stats.replayed, s_stats.recorded, s_stats.packetFails, (unsigned)(s_packetBytes / 1024), (unsigned)s_deferredCount,
		::g_ps3TextureCount, (unsigned)(::g_ps3TextureRamBytes / 1024),
		s_residency.texReleased, s_residency.texRestored, (unsigned)(s_residency.texReleasedBytes / 1024),
		s_residency.vbReleased, s_residency.vbRestored, (unsigned)(s_residency.vbReleasedBytes / 1024));
	{
		// Draw CPU time per frame, and its parts.
		size_t o = strlen(s_statsLine);
		const unsigned f = s_dpFrames ? s_dpFrames : 1;
		o += snprintf(s_statsLine + o, sizeof(s_statsLine) - o, "; draw calls %u.%02u ms a frame:",
			(unsigned)(s_dpTotal / f / 79800), (unsigned)(s_dpTotal / f * 100 / 79800 % 100));
		for (int i = 0; i < kDpPhases && o < sizeof(s_statsLine) - 48; ++i)
			o += snprintf(s_statsLine + o, sizeof(s_statsLine) - o, " %s %u.%02u", kDpNames[i],
				(unsigned)(s_dp[i] / f / 79800), (unsigned)(s_dp[i] / f * 100 / 79800 % 100));
		memset(s_dp, 0, sizeof(s_dp));
		s_dpTotal = 0;
		s_dpFrames = 0;
	}
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
	// The RSX may still read these: freed once the frames that used them are done.
	DeferFree(r->mem, r->usedPlusOne);
	for (int i = 0; i < 2; ++i) DeferFree(r->spare[i].mem, r->spare[i].usedPlusOne);
	FreeRecord(r);
}

static void FreeBuffer(void* backend)
{
	RsxBuffer* b = (RsxBuffer*)backend;
	if (!b) return;
	DeferFree(b->mem, b->persistentUsed);	// its ring copies belong to the ring (or are deferred already)
	FreeRecord(b);
	s_attrValid = false;	// the next draw binds its attributes again, whatever lands at that address
}

void TextureChanged(IDirect3DTexture9*) {}	// uploaded on the next draw that uses it (m_dirty)
void TextureReleased(IDirect3DTexture9* tex) { DeviceGuard guard; FreeTexture(tex->m_backend); tex->m_backend = NULL; }
void SurfaceReleased(IDirect3DSurface9* surf)
{
	DeviceGuard guard;
	RsxSurface* rs = surf->m_owner ? NULL : (RsxSurface*)surf->m_backend;
	if (rs)
	{
		DeferFree(rs->mem, rs->usedPlusOne);
		FreeRecord(rs);
	}
	surf->m_backend = NULL;
	s_targetValid = false;
}
void VertexBufferChanged(IDirect3DVertexBuffer9*, UINT, UINT) {}
void VertexBufferReleased(IDirect3DVertexBuffer9* vb) { DeviceGuard guard; FreeBuffer(vb->m_backend); vb->m_backend = NULL; }
void IndexBufferChanged(IDirect3DIndexBuffer9*, UINT, UINT) {}
void IndexBufferReleased(IDirect3DIndexBuffer9* ib) { DeviceGuard guard; FreeBuffer(ib->m_backend); ib->m_backend = NULL; }
void ShaderCreated(IDirect3DVertexShader9*) {}
void ShaderCreated(IDirect3DPixelShader9*) {}
void ShaderReleased(IDirect3DVertexShader9*) {}
void ShaderReleased(IDirect3DPixelShader9* ps)
{
	DeviceGuard guard;
	++s_packetEpoch;	// packets may name its fragment programs
	// Drop its cached programs (the key holds the pointer, which may be reused).
	for (int fog = 0; fog < 2; ++fog)
	{
		FpKey key;
		memset(&key, 0, sizeof(key));
		key.kind = 1;
		key.fog = (u8)fog;
		key.ps = (u64)(uintptr_t)ps;
		if (FpEntry* e = s_fps.Erase(key))
			FreeFragmentProgram(e);	// also forgets it as the last program used
	}
	s_lastFp = NULL;
}

UINT VideoMemoryFree()
{
	return 128 * 1024 * 1024;
}

} // namespace Ps3Gfx

#endif // !PS3_HEADLESS
