// THUG PS3: D3D9 objects (see d3d9.h). All state and resource data live here in
// main memory; Ps3Gfx (the backend) mirrors what it needs onto the RSX. The RSX backend may
// free the main-memory copy of a texture or static vertex buffer once it has uploaded it and
// it stops changing; locking the resource again brings the copy back (read back from RSX
// memory, see d3d9_rsx_draw.cpp).

#include <d3d9.h>
#include <d3dx9.h>
#include <ps3_gfx_backend.h>

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

static IDirect3DDevice9* g_device = NULL;

// newlib's allocator returns NULL when the pool is used up. Each kind of object logs its
// first failure (with the size asked for) so a later crash elsewhere can be traced back
// here; after that the same failure would only flood the log.
enum OomKind { OOM_SURFACE, OOM_TEXTURE, OOM_VERTEXBUFFER, OOM_INDEXBUFFER, OOM_SHADER, OOM_KINDS };

static void LogOutOfMemory(OomKind kind, const char* what, unsigned long bytes)
{
	static bool logged[OOM_KINDS];
	if (logged[kind]) return;
	logged[kind] = true;
	printf("[gfx] out of memory: %lu bytes for %s\n", bytes, what);
}

// Render and depth targets are drawn by the RSX into its own memory; the shim's main-memory
// copy of their pixels is never read, so it is only allocated if something locks one.
static bool IsGpuTarget(DWORD usage)
{
	return (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
}

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------
static bool IsDxt(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

static UINT BytesPerPixel(D3DFORMAT f)
{
	switch (f)
	{
	case D3DFMT_R8G8B8:
		return 3;
	case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
	case D3DFMT_B8G8R8A8: case D3DFMT_R8G8B8A8: case D3DFMT_V16U16: case D3DFMT_G16R16:
	case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_D24S8: case D3DFMT_D24X8:
	case D3DFMT_D24X4S4: case D3DFMT_D32: case D3DFMT_F24S8: case D3DFMT_INDEX32: case D3DFMT_R32F:
	case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_D24FS8:
		return 4;
	case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F:
		return 8;
	case D3DFMT_A32B32G32R32F:
		return 16;
	case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_AL8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
		return 1;
	default:
		return 2;	// the 16-bit colour, luminance, depth and index formats
	}
}

static UINT RowPitch(D3DFORMAT f, UINT width)
{
	if (IsDxt(f))
		return ((width + 3) / 4) * (f == D3DFMT_DXT1 ? 8 : 16);
	return width * BytesPerPixel(f);
}

static UINT Rows(D3DFORMAT f, UINT height)
{
	return IsDxt(f) ? (height + 3) / 4 : height;
}

// ---------------------------------------------------------------------------
// Resource base
// ---------------------------------------------------------------------------
HRESULT IDirect3DResource9::GetDevice(IDirect3DDevice9** pp)
{
	*pp = g_device;
	return g_device ? S_OK : D3DERR_INVALIDCALL;
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------
IDirect3DSurface9::IDirect3DSurface9(UINT w, UINT h, D3DFORMAT fmt, DWORD usage, IDirect3DTexture9* owner, UINT level)
	: IDirect3DResource9(D3DRTYPE_SURFACE), m_width(w), m_height(h), m_format(fmt), m_usage(usage),
	  m_owner(owner), m_level(level), m_bits(NULL), m_pitch(RowPitch(fmt, w)), m_backend(NULL)
{
	// A NULL m_bits here (out of memory) is caught by the Create* call that made the surface.
	if (!owner && !IsGpuTarget(usage))
		m_bits = (BYTE*)calloc(1, m_pitch * Rows(fmt, h));
}

// Gives a stand-alone surface its main-memory pixels if it has none yet.
static bool SurfaceBits(IDirect3DSurface9* s)
{
	if (s->m_bits) return true;
	unsigned long bytes = (unsigned long)s->m_pitch * Rows(s->m_format, s->m_height);
	s->m_bits = (BYTE*)calloc(1, bytes);
	if (!s->m_bits) LogOutOfMemory(OOM_SURFACE, "a surface", bytes);
	return s->m_bits != NULL;
}

IDirect3DSurface9::~IDirect3DSurface9()
{
	Ps3Gfx::SurfaceReleased(this);
	free(m_bits);
}

HRESULT IDirect3DSurface9::GetDesc(D3DSURFACE_DESC* d)
{
	memset(d, 0, sizeof(*d));
	d->Format = m_format;
	d->Type = D3DRTYPE_SURFACE;
	d->Usage = m_usage;
	d->Pool = D3DPOOL_DEFAULT;
	d->Width = m_width;
	d->Height = m_height;
	return S_OK;
}

HRESULT IDirect3DSurface9::LockRect(D3DLOCKED_RECT* r, const RECT* rect, DWORD flags)
{
	if (m_owner)
		return m_owner->LockRect(m_level, r, rect, flags);
	if (!SurfaceBits(this))
	{
		r->pBits = NULL;
		r->Pitch = 0;
		return E_OUTOFMEMORY;
	}
	BYTE* bits = m_bits;
	if (rect)
		bits += rect->top / (IsDxt(m_format) ? 4 : 1) * m_pitch
		      + (IsDxt(m_format) ? rect->left / 4 * (m_format == D3DFMT_DXT1 ? 8 : 16) : rect->left * BytesPerPixel(m_format));
	r->pBits = bits;
	r->Pitch = m_pitch;
	return S_OK;
}

HRESULT IDirect3DSurface9::UnlockRect()
{
	if (m_owner)
		return m_owner->UnlockRect(m_level);
	return S_OK;
}

HRESULT IDirect3DSurface9::GetContainer(REFIID, void** pp)
{
	*pp = m_owner;
	if (m_owner) m_owner->AddRef();
	return m_owner ? S_OK : E_FAIL;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
// Main-memory copies held by textures (each also has its own copy in RSX memory), for the
// [gfx] frame lines in the log. Only copies that exist are counted: a render target without
// one, or a texture whose copy the backend released after upload, adds nothing.
volatile unsigned g_ps3TextureCount;
volatile unsigned long long g_ps3TextureRamBytes;

// Allocates a texture's main-memory copy of all its levels, zeroed, if it has none yet.
static bool TextureBits(IDirect3DTexture9* t)
{
	if (t->m_data) return true;
	t->m_data = (BYTE*)memalign(128, t->m_size ? t->m_size : 128);
	if (!t->m_data)
	{
		LogOutOfMemory(OOM_TEXTURE, "a texture", t->m_size);
		return false;
	}
	memset(t->m_data, 0, t->m_size);
	g_ps3TextureCount = g_ps3TextureCount + 1;
	g_ps3TextureRamBytes = g_ps3TextureRamBytes + t->m_size;
	return true;
}

IDirect3DTexture9::IDirect3DTexture9(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
	: IDirect3DBaseTexture9(D3DRTYPE_TEXTURE), m_width(w), m_height(h), m_levels(0), m_usage(usage),
	  m_format(fmt), m_pool(pool), m_data(NULL), m_size(0), m_dirty(true), m_backend(NULL),
	  m_lockDepth(0), m_dataReleased(false), m_keepData(false)
{
	if (w == 0) w = m_width = 1;
	if (h == 0) h = m_height = 1;

	// levels == 0 means a full chain down to 1x1.
	UINT full = 1;
	for (UINT s = (w > h ? w : h); s > 1; s >>= 1) ++full;
	m_levels = (levels == 0 || levels > full) ? full : levels;
	if (m_levels > 16) m_levels = 16;

	UINT offset = 0;
	for (UINT i = 0; i < m_levels; ++i)
	{
		m_offsets[i] = offset;
		offset += (LevelSize(i) + 127) & ~127u;	// 128-byte aligned levels (RSX-friendly)
	}
	m_size = offset;
	memset(m_surfaces, 0, sizeof(m_surfaces));
	// A NULL m_data here (out of memory) is caught by CreateTexture.
	if (!IsGpuTarget(usage))
		TextureBits(this);
}

IDirect3DTexture9::~IDirect3DTexture9()
{
	Ps3Gfx::TextureReleased(this);
	for (UINT i = 0; i < 16; ++i)
	{
		if (m_surfaces[i])
		{
			m_surfaces[i]->m_owner = NULL;	// any surface still held now has no pixels
			m_surfaces[i]->Release();
		}
	}
	if (m_data)
	{
		free(m_data);
		g_ps3TextureCount = g_ps3TextureCount - 1;
		g_ps3TextureRamBytes = g_ps3TextureRamBytes - m_size;
	}
}

UINT IDirect3DTexture9::LevelWidth(UINT level) const  { UINT w = m_width >> level; return w ? w : 1; }
UINT IDirect3DTexture9::LevelHeight(UINT level) const { UINT h = m_height >> level; return h ? h : 1; }
UINT IDirect3DTexture9::LevelPitch(UINT level) const  { return RowPitch(m_format, LevelWidth(level)); }
UINT IDirect3DTexture9::LevelSize(UINT level) const   { return LevelPitch(level) * Rows(m_format, LevelHeight(level)); }

HRESULT IDirect3DTexture9::GetLevelDesc(UINT level, D3DSURFACE_DESC* d)
{
	if (level >= m_levels) return D3DERR_INVALIDCALL;
	memset(d, 0, sizeof(*d));
	d->Format = m_format;
	d->Type = D3DRTYPE_SURFACE;
	d->Usage = m_usage;
	d->Pool = m_pool;
	d->Width = LevelWidth(level);
	d->Height = LevelHeight(level);
	return S_OK;
}

HRESULT IDirect3DTexture9::GetSurfaceLevel(UINT level, IDirect3DSurface9** pp)
{
	if (level >= m_levels) { *pp = NULL; return D3DERR_INVALIDCALL; }
	if (!m_surfaces[level])
		m_surfaces[level] = new IDirect3DSurface9(LevelWidth(level), LevelHeight(level), m_format, m_usage, this, level);
	m_surfaces[level]->AddRef();
	*pp = m_surfaces[level];
	return S_OK;
}

HRESULT IDirect3DTexture9::LockRect(UINT level, D3DLOCKED_RECT* r, const RECT* rect, DWORD)
{
	if (level >= m_levels) return D3DERR_INVALIDCALL;
	// Under the backend's lock: it may release the copy of a texture it has uploaded (and
	// reads it back here if so), but never while a lock is open.
	Ps3Gfx::DeviceLock();
	bool ok = m_dataReleased ? Ps3Gfx::RestoreTextureData(this) : true;
	if (ok) ok = TextureBits(this);	// render targets get theirs on their first lock
	if (ok) ++m_lockDepth;
	Ps3Gfx::DeviceUnlock();
	if (!ok)
	{
		r->pBits = NULL;
		r->Pitch = 0;
		return E_OUTOFMEMORY;
	}
	BYTE* bits = LevelBits(level);
	UINT pitch = LevelPitch(level);
	if (rect)
	{
		if (IsDxt(m_format))
			bits += (rect->top / 4) * pitch + (rect->left / 4) * (m_format == D3DFMT_DXT1 ? 8 : 16);
		else
			bits += rect->top * pitch + rect->left * BytesPerPixel(m_format);
	}
	r->pBits = bits;
	r->Pitch = pitch;
	return S_OK;
}

HRESULT IDirect3DTexture9::UnlockRect(UINT)
{
	Ps3Gfx::DeviceLock();
	if (m_lockDepth) --m_lockDepth;
	m_dirty = true;
	Ps3Gfx::DeviceUnlock();
	Ps3Gfx::TextureChanged(this);
	return S_OK;
}

// ---------------------------------------------------------------------------
// Vertex / index buffers
// ---------------------------------------------------------------------------
IDirect3DVertexBuffer9::IDirect3DVertexBuffer9(UINT size, DWORD usage, DWORD fvf, D3DPOOL pool)
	: IDirect3DResource9(D3DRTYPE_VERTEXBUFFER), m_size(size), m_usage(usage), m_fvf(fvf), m_pool(pool),
	  m_dirty(true), m_backend(NULL), m_lockDepth(0), m_dataReleased(false), m_keepData(false)
{
	// A NULL m_data here (out of memory) is caught by the Create* call.
	m_data = (BYTE*)memalign(128, size ? size : 128);
	if (m_data) memset(m_data, 0, size);
	m_changes.Reset(size);
}

IDirect3DVertexBuffer9::~IDirect3DVertexBuffer9()
{
	Ps3Gfx::VertexBufferReleased(this);
	free(m_data);
}

// Static buffers (no D3DUSAGE_DYNAMIC) lock under the backend's lock, which may have released
// their main-memory copy after uploading it (read back here if so) but never while a lock is
// open. Dynamic buffers keep their copy and skip the lock (they are locked many times a frame).
HRESULT IDirect3DVertexBuffer9::Lock(UINT offset, UINT size, void** pp, DWORD flags)
{
	if (!(m_usage & D3DUSAGE_DYNAMIC))
	{
		Ps3Gfx::DeviceLock();
		const bool ok = !m_dataReleased || Ps3Gfx::RestoreVertexBufferData(this, (flags & D3DLOCK_DISCARD) == 0);
		if (ok) ++m_lockDepth;
		Ps3Gfx::DeviceUnlock();
		if (!ok) { *pp = NULL; return E_OUTOFMEMORY; }
	}
	m_changes.Locked(offset, size, flags);
	*pp = m_data + offset;
	return S_OK;
}

HRESULT IDirect3DVertexBuffer9::Unlock()
{
	if (!(m_usage & D3DUSAGE_DYNAMIC))
	{
		Ps3Gfx::DeviceLock();
		if (m_lockDepth) --m_lockDepth;
		m_changes.Unlocked(m_size, m_dirty);
		Ps3Gfx::DeviceUnlock();
	}
	else
		m_changes.Unlocked(m_size, m_dirty);
	Ps3Gfx::VertexBufferChanged(this, 0, m_size);
	return S_OK;
}

HRESULT IDirect3DVertexBuffer9::GetDesc(D3DVERTEXBUFFER_DESC* d)
{
	memset(d, 0, sizeof(*d));
	d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = m_usage;
	d->Pool = m_pool; d->Size = m_size; d->FVF = m_fvf;
	return S_OK;
}

IDirect3DIndexBuffer9::IDirect3DIndexBuffer9(UINT size, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
	: IDirect3DResource9(D3DRTYPE_INDEXBUFFER), m_size(size), m_usage(usage), m_format(fmt), m_pool(pool),
	  m_dirty(true), m_backend(NULL)
{
	// A NULL m_data here (out of memory) is caught by the Create* call.
	m_data = (BYTE*)memalign(128, size ? size : 128);
	if (m_data) memset(m_data, 0, size);
	m_changes.Reset(size);
}

IDirect3DIndexBuffer9::~IDirect3DIndexBuffer9()
{
	Ps3Gfx::IndexBufferReleased(this);
	free(m_data);
}

HRESULT IDirect3DIndexBuffer9::Lock(UINT offset, UINT size, void** pp, DWORD flags)
{
	m_changes.Locked(offset, size, flags);
	*pp = m_data + offset;
	return S_OK;
}

HRESULT IDirect3DIndexBuffer9::Unlock()
{
	m_changes.Unlocked(m_size, m_dirty);
	Ps3Gfx::IndexBufferChanged(this, 0, m_size);
	return S_OK;
}

HRESULT IDirect3DIndexBuffer9::GetDesc(D3DINDEXBUFFER_DESC* d)
{
	memset(d, 0, sizeof(*d));
	d->Format = m_format; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = m_usage; d->Pool = m_pool; d->Size = m_size;
	return S_OK;
}

// ---------------------------------------------------------------------------
// Shaders / declarations
// ---------------------------------------------------------------------------
static UINT CountShaderTokens(const DWORD* t)
{
	// D3D shader bytecode ends with 0x0000FFFF.
	UINT n = 0;
	while (t[n] != 0x0000FFFF && n < 65536) ++n;
	return n + 1;
}

IDirect3DVertexShader9::IDirect3DVertexShader9(const DWORD* tokens)
	: IDirect3DResource9(D3DRTYPE_SURFACE), m_backend(NULL)
{
	m_numTokens = tokens ? CountShaderTokens(tokens) : 0;
	m_tokens = (DWORD*)malloc(m_numTokens * sizeof(DWORD) + 4);
	if (tokens && m_tokens) memcpy(m_tokens, tokens, m_numTokens * sizeof(DWORD));
}

IDirect3DVertexShader9::~IDirect3DVertexShader9()
{
	Ps3Gfx::ShaderReleased(this);
	free(m_tokens);
}

IDirect3DPixelShader9::IDirect3DPixelShader9(const DWORD* tokens)
	: IDirect3DResource9(D3DRTYPE_SURFACE), m_backend(NULL)
{
	m_numTokens = tokens ? CountShaderTokens(tokens) : 0;
	m_tokens = (DWORD*)malloc(m_numTokens * sizeof(DWORD) + 4);
	if (tokens && m_tokens) memcpy(m_tokens, tokens, m_numTokens * sizeof(DWORD));
}

IDirect3DPixelShader9::~IDirect3DPixelShader9()
{
	Ps3Gfx::ShaderReleased(this);
	free(m_tokens);
}

IDirect3DVertexDeclaration9::IDirect3DVertexDeclaration9(const D3DVERTEXELEMENT9* e)
	: IDirect3DResource9(D3DRTYPE_SURFACE), m_count(0)
{
	while (e && e[m_count].Stream != 0xFF && m_count < 31)
	{
		m_elements[m_count] = e[m_count];
		++m_count;
	}
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
IDirect3DDevice9::IDirect3DDevice9(const D3DPRESENT_PARAMETERS* pp)
{
	m_pp = *pp;
	memset(m_rs, 0, sizeof(m_rs));
	memset(m_tss, 0, sizeof(m_tss));
	memset(m_ss, 0, sizeof(m_ss));
	memset(m_textures, 0, sizeof(m_textures));
	memset(m_lights, 0, sizeof(m_lights));
	memset(m_lightEnabled, 0, sizeof(m_lightEnabled));
	memset(&m_material, 0, sizeof(m_material));
	memset(m_vsConst, 0, sizeof(m_vsConst));
	memset(m_psConst, 0, sizeof(m_psConst));
	memset(m_streams, 0, sizeof(m_streams));
	memset(m_streamOffset, 0, sizeof(m_streamOffset));
	memset(m_streamStride, 0, sizeof(m_streamStride));
	for (int i = 0; i < 512; ++i)
		D3DXMatrixIdentity((D3DXMATRIX*)&m_transforms[i]);
	m_fvf = 0; m_decl = NULL; m_vs = NULL; m_ps = NULL; m_indices = NULL; m_backend = NULL;
	m_vsSerial = 1;

	// D3D9 default render states that matter to the renderer.
	m_rs[D3DRS_ZENABLE] = D3DZB_TRUE;
	m_rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
	m_rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
	m_rs[D3DRS_ZWRITEENABLE] = TRUE;
	m_rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
	m_rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
	m_rs[D3DRS_CULLMODE] = D3DCULL_CCW;
	m_rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
	m_rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
	m_rs[D3DRS_STENCILFAIL] = m_rs[D3DRS_STENCILZFAIL] = m_rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
	m_rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
	m_rs[D3DRS_STENCILMASK] = m_rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
	m_rs[D3DRS_CLIPPING] = TRUE;
	m_rs[D3DRS_LIGHTING] = TRUE;
	m_rs[D3DRS_COLORVERTEX] = TRUE;
	m_rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
	m_rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
	m_rs[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
	m_rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
	m_rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
	m_rs[D3DRS_POINTSIZE] = 0x3F800000;	// 1.0f
	m_rs[D3DRS_POINTSIZE_MAX] = 0x42800000;	// 64.0f
	for (int s = 0; s < 8; ++s)
	{
		m_tss[s][D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
		m_tss[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
		m_tss[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
		m_tss[s][D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
		m_tss[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
		m_tss[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
		m_tss[s][D3DTSS_TEXCOORDINDEX] = s;
	}
	for (int s = 0; s < 16; ++s)
	{
		m_ss[s][D3DSAMP_ADDRESSU] = m_ss[s][D3DSAMP_ADDRESSV] = m_ss[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
		m_ss[s][D3DSAMP_MAGFILTER] = m_ss[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
		m_ss[s][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
		m_ss[s][D3DSAMP_MAXANISOTROPY] = 1;
	}

	m_viewport.X = m_viewport.Y = 0;
	m_viewport.Width = m_pp.BackBufferWidth;
	m_viewport.Height = m_pp.BackBufferHeight;
	m_viewport.MinZ = 0.0f; m_viewport.MaxZ = 1.0f;
	m_scissor.left = m_scissor.top = 0;
	m_scissor.right = m_pp.BackBufferWidth; m_scissor.bottom = m_pp.BackBufferHeight;

	m_backBuffer = new IDirect3DSurface9(m_pp.BackBufferWidth, m_pp.BackBufferHeight, m_pp.BackBufferFormat, D3DUSAGE_RENDERTARGET, NULL, 0);
	m_depthBuffer = new IDirect3DSurface9(m_pp.BackBufferWidth, m_pp.BackBufferHeight, D3DFMT_D24S8, D3DUSAGE_DEPTHSTENCIL, NULL, 0);
	m_renderTarget = m_backBuffer;
	m_depthTarget = m_depthBuffer;

	g_device = this;
}

IDirect3DDevice9::~IDirect3DDevice9()
{
	Ps3Gfx::Shutdown();
	g_device = NULL;
}

UINT IDirect3DDevice9::GetAvailableTextureMem()
{
	return Ps3Gfx::VideoMemoryFree();
}

HRESULT IDirect3DDevice9::GetDeviceCaps(D3DCAPS9* caps)
{
	IDirect3D9 d3d;
	return d3d.GetDeviceCaps(0, D3DDEVTYPE_HAL, caps);
}

HRESULT IDirect3DDevice9::GetDisplayMode(UINT, D3DDISPLAYMODE* mode)
{
	mode->Width = m_pp.BackBufferWidth;
	mode->Height = m_pp.BackBufferHeight;
	mode->RefreshRate = 60;
	mode->Format = D3DFMT_X8R8G8B8;
	return S_OK;
}

HRESULT IDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS*)
{
	return S_OK;	// the TV mode doesn't change while the game runs
}

HRESULT IDirect3DDevice9::Present(const RECT*, const RECT*, HWND, const void*)
{
	Ps3Gfx::Present(this);
	return S_OK;
}

HRESULT IDirect3DDevice9::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** pp)
{
	m_backBuffer->AddRef();
	*pp = m_backBuffer;
	return S_OK;
}

HRESULT IDirect3DDevice9::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** pp, HANDLE*)
{
	IDirect3DTexture9* t = new IDirect3DTexture9(w, h, levels, usage, fmt, pool);
	if (!t->m_data && !IsGpuTarget(usage))
	{
		t->Release();
		*pp = NULL;
		return E_OUTOFMEMORY;
	}
	*pp = t;
	return S_OK;
}

HRESULT IDirect3DDevice9::CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** pp, HANDLE*)
{
	IDirect3DVertexBuffer9* vb = new IDirect3DVertexBuffer9(len, usage, fvf, pool);
	if (!vb->m_data)
	{
		LogOutOfMemory(OOM_VERTEXBUFFER, "a vertex buffer", len);
		vb->Release();
		*pp = NULL;
		return E_OUTOFMEMORY;
	}
	*pp = vb;
	return S_OK;
}

HRESULT IDirect3DDevice9::CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** pp, HANDLE*)
{
	IDirect3DIndexBuffer9* ib = new IDirect3DIndexBuffer9(len, usage, fmt, pool);
	if (!ib->m_data)
	{
		LogOutOfMemory(OOM_INDEXBUFFER, "an index buffer", len);
		ib->Release();
		*pp = NULL;
		return E_OUTOFMEMORY;
	}
	*pp = ib;
	return S_OK;
}

HRESULT IDirect3DDevice9::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9** pp, HANDLE*)
{
	*pp = new IDirect3DSurface9(w, h, fmt, D3DUSAGE_RENDERTARGET, NULL, 0);
	return S_OK;
}

HRESULT IDirect3DDevice9::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9** pp, HANDLE*)
{
	*pp = new IDirect3DSurface9(w, h, fmt, D3DUSAGE_DEPTHSTENCIL, NULL, 0);
	return S_OK;
}

HRESULT IDirect3DDevice9::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL, IDirect3DSurface9** pp, HANDLE*)
{
	IDirect3DSurface9* s = new IDirect3DSurface9(w, h, fmt, 0, NULL, 0);
	if (!s->m_bits)
	{
		LogOutOfMemory(OOM_SURFACE, "a surface", (unsigned long)s->m_pitch * Rows(fmt, h));
		s->Release();
		*pp = NULL;
		return E_OUTOFMEMORY;
	}
	*pp = s;
	return S_OK;
}

HRESULT IDirect3DDevice9::StretchRect(IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE)
{
	return S_OK;	// done by the backend when it supports render-to-texture copies
}

HRESULT IDirect3DDevice9::SetRenderTarget(DWORD index, IDirect3DSurface9* s)
{
	if (index != 0) return S_OK;
	m_renderTarget = s ? s : m_backBuffer;
	m_viewport.X = m_viewport.Y = 0;
	m_viewport.Width = m_renderTarget->m_width;
	m_viewport.Height = m_renderTarget->m_height;
	++m_vsSerial;	// the viewport feeds the vertex constants
	return S_OK;
}

HRESULT IDirect3DDevice9::GetRenderTarget(DWORD, IDirect3DSurface9** pp)
{
	m_renderTarget->AddRef();
	*pp = m_renderTarget;
	return S_OK;
}

HRESULT IDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9* s)
{
	m_depthTarget = s;
	return S_OK;
}

HRESULT IDirect3DDevice9::GetDepthStencilSurface(IDirect3DSurface9** pp)
{
	if (m_depthTarget) m_depthTarget->AddRef();
	*pp = m_depthTarget;
	return m_depthTarget ? S_OK : D3DERR_NOTAVAILABLE;
}

HRESULT IDirect3DDevice9::BeginScene()
{
	Ps3Gfx::BeginFrame(this);
	return S_OK;
}

HRESULT IDirect3DDevice9::EndScene()
{
	return S_OK;
}

HRESULT IDirect3DDevice9::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	Ps3Gfx::Clear(this, count, rects, flags, color, z, stencil);
	return S_OK;
}

HRESULT IDirect3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m)
{
	if ((UINT)t < 512 && memcmp(&m_transforms[t], m, sizeof(*m)))
	{
		m_transforms[t] = *m;
		++m_vsSerial;
	}
	return S_OK;
}

HRESULT IDirect3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m)
{
	if ((UINT)t < 512) *m = m_transforms[t];
	return S_OK;
}

HRESULT IDirect3DDevice9::SetViewport(const D3DVIEWPORT9* vp) { if (memcmp(&m_viewport, vp, sizeof(*vp))) { m_viewport = *vp; ++m_vsSerial; } return S_OK; }
HRESULT IDirect3DDevice9::GetViewport(D3DVIEWPORT9* vp) { *vp = m_viewport; return S_OK; }
HRESULT IDirect3DDevice9::SetMaterial(const D3DMATERIAL9* m) { if (memcmp(&m_material, m, sizeof(*m))) { m_material = *m; ++m_vsSerial; } return S_OK; }
HRESULT IDirect3DDevice9::GetMaterial(D3DMATERIAL9* m) { *m = m_material; return S_OK; }
HRESULT IDirect3DDevice9::SetLight(DWORD i, const D3DLIGHT9* l) { if (i < 8 && memcmp(&m_lights[i], l, sizeof(*l))) { m_lights[i] = *l; ++m_vsSerial; } return S_OK; }
HRESULT IDirect3DDevice9::GetLight(DWORD i, D3DLIGHT9* l) { if (i < 8) *l = m_lights[i]; return S_OK; }
HRESULT IDirect3DDevice9::LightEnable(DWORD i, BOOL on) { if (i < 8 && m_lightEnabled[i] != on) { m_lightEnabled[i] = on; ++m_vsSerial; } return S_OK; }
HRESULT IDirect3DDevice9::GetLightEnable(DWORD i, BOOL* on) { *on = i < 8 ? m_lightEnabled[i] : FALSE; return S_OK; }

HRESULT IDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE s, DWORD v)
{
	if ((UINT)s < D3DRS_MAX && m_rs[s] != v)
	{
		m_rs[s] = v;
		if (s == D3DRS_AMBIENT || s == D3DRS_FOGSTART || s == D3DRS_FOGEND || s == D3DRS_POINTSIZE) ++m_vsSerial;
	}
	return S_OK;
}
HRESULT IDirect3DDevice9::GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) { *v = (UINT)s < D3DRS_MAX ? m_rs[s] : 0; return S_OK; }

HRESULT IDirect3DDevice9::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD v)
{
	if (stage < 8 && (UINT)s < D3DTSS_MAX) m_tss[stage][s] = v;
	return S_OK;
}

HRESULT IDirect3DDevice9::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD* v)
{
	*v = (stage < 8 && (UINT)s < D3DTSS_MAX) ? m_tss[stage][s] : 0;
	return S_OK;
}

HRESULT IDirect3DDevice9::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE s, DWORD v)
{
	if (sampler < 16 && (UINT)s < D3DSAMP_MAX) m_ss[sampler][s] = v;
	return S_OK;
}

HRESULT IDirect3DDevice9::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE s, DWORD* v)
{
	*v = (sampler < 16 && (UINT)s < D3DSAMP_MAX) ? m_ss[sampler][s] : 0;
	return S_OK;
}

HRESULT IDirect3DDevice9::SetTexture(DWORD stage, IDirect3DBaseTexture9* t)
{
	if (stage < 16) m_textures[stage] = t;
	return S_OK;
}

HRESULT IDirect3DDevice9::GetTexture(DWORD stage, IDirect3DBaseTexture9** pp)
{
	*pp = stage < 16 ? m_textures[stage] : NULL;
	if (*pp) (*pp)->AddRef();
	return S_OK;
}

HRESULT IDirect3DDevice9::SetScissorRect(const RECT* r) { m_scissor = *r; return S_OK; }
HRESULT IDirect3DDevice9::GetScissorRect(RECT* r) { *r = m_scissor; return S_OK; }

HRESULT IDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** pp)
{
	*pp = new IDirect3DVertexDeclaration9(e);
	return S_OK;
}

HRESULT IDirect3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9* d) { m_decl = d; return S_OK; }
HRESULT IDirect3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9** pp) { *pp = m_decl; return S_OK; }
HRESULT IDirect3DDevice9::SetFVF(DWORD fvf) { m_fvf = fvf; m_decl = NULL; return S_OK; }
HRESULT IDirect3DDevice9::GetFVF(DWORD* fvf) { *fvf = m_fvf; return S_OK; }

HRESULT IDirect3DDevice9::CreateVertexShader(const DWORD* tokens, IDirect3DVertexShader9** pp)
{
	IDirect3DVertexShader9* vs = new IDirect3DVertexShader9(tokens);
	if (!vs->m_tokens)
	{
		LogOutOfMemory(OOM_SHADER, "a shader", (unsigned long)vs->m_numTokens * sizeof(DWORD) + 4);
		vs->Release();
		*pp = NULL;
		return E_OUTOFMEMORY;
	}
	Ps3Gfx::ShaderCreated(vs);
	*pp = vs;
	return S_OK;
}

HRESULT IDirect3DDevice9::SetVertexShader(IDirect3DVertexShader9* s) { m_vs = s; return S_OK; }
HRESULT IDirect3DDevice9::GetVertexShader(IDirect3DVertexShader9** pp) { *pp = m_vs; return S_OK; }

HRESULT IDirect3DDevice9::SetVertexShaderConstantF(UINT reg, const float* data, UINT count)
{
	if (reg >= 256) return D3DERR_INVALIDCALL;
	if (reg + count > 256) count = 256 - reg;
	memcpy(m_vsConst[reg], data, count * 16);
	return S_OK;
}

HRESULT IDirect3DDevice9::GetVertexShaderConstantF(UINT reg, float* data, UINT count)
{
	if (reg >= 256) return D3DERR_INVALIDCALL;
	if (reg + count > 256) count = 256 - reg;
	memcpy(data, m_vsConst[reg], count * 16);
	return S_OK;
}

HRESULT IDirect3DDevice9::CreatePixelShader(const DWORD* tokens, IDirect3DPixelShader9** pp)
{
	IDirect3DPixelShader9* ps = new IDirect3DPixelShader9(tokens);
	if (!ps->m_tokens)
	{
		LogOutOfMemory(OOM_SHADER, "a shader", (unsigned long)ps->m_numTokens * sizeof(DWORD) + 4);
		ps->Release();
		*pp = NULL;
		return E_OUTOFMEMORY;
	}
	Ps3Gfx::ShaderCreated(ps);
	*pp = ps;
	return S_OK;
}

HRESULT IDirect3DDevice9::SetPixelShader(IDirect3DPixelShader9* s) { m_ps = s; return S_OK; }
HRESULT IDirect3DDevice9::GetPixelShader(IDirect3DPixelShader9** pp) { *pp = m_ps; return S_OK; }

HRESULT IDirect3DDevice9::SetPixelShaderConstantF(UINT reg, const float* data, UINT count)
{
	if (reg >= 32) return D3DERR_INVALIDCALL;
	if (reg + count > 32) count = 32 - reg;
	memcpy(m_psConst[reg], data, count * 16);
	return S_OK;
}

HRESULT IDirect3DDevice9::GetPixelShaderConstantF(UINT reg, float* data, UINT count)
{
	if (reg >= 32) return D3DERR_INVALIDCALL;
	if (reg + count > 32) count = 32 - reg;
	memcpy(data, m_psConst[reg], count * 16);
	return S_OK;
}

HRESULT IDirect3DDevice9::SetStreamSource(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride)
{
	if (stream >= 4) return D3DERR_INVALIDCALL;
	m_streams[stream] = vb; m_streamOffset[stream] = offset; m_streamStride[stream] = stride;
	return S_OK;
}

HRESULT IDirect3DDevice9::GetStreamSource(UINT stream, IDirect3DVertexBuffer9** pp, UINT* offset, UINT* stride)
{
	if (stream >= 4) return D3DERR_INVALIDCALL;
	*pp = m_streams[stream];
	if (*pp) (*pp)->AddRef();
	if (offset) *offset = m_streamOffset[stream];
	if (stride) *stride = m_streamStride[stream];
	return S_OK;
}

HRESULT IDirect3DDevice9::SetIndices(IDirect3DIndexBuffer9* ib) { m_indices = ib; return S_OK; }
HRESULT IDirect3DDevice9::GetIndices(IDirect3DIndexBuffer9** pp) { *pp = m_indices; if (*pp) (*pp)->AddRef(); return S_OK; }

static UINT VerticesForPrims(D3DPRIMITIVETYPE type, UINT prims)
{
	switch (type)
	{
	case D3DPT_POINTLIST:     return prims;
	case D3DPT_LINELIST:      return prims * 2;
	case D3DPT_LINESTRIP:     return prims + 1;
	case D3DPT_TRIANGLELIST:  return prims * 3;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_TRIANGLEFAN:   return prims + 2;
	case D3DPT_QUADLIST:      return prims * 4;
	default:                  return 0;
	}
}

HRESULT IDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count)
{
	Ps3Gfx::DrawCall c;
	memset(&c, 0, sizeof(c));
	c.type = type; c.primCount = count; c.startVertex = start;
	c.vertexCount = VerticesForPrims(type, count);
	Ps3Gfx::Draw(this, c);
	return S_OK;
}

HRESULT IDirect3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVerts, UINT startIndex, UINT primCount)
{
	Ps3Gfx::DrawCall c;
	memset(&c, 0, sizeof(c));
	c.type = type; c.primCount = primCount; c.baseVertex = baseVertex; c.minIndex = minIndex;
	c.vertexCount = numVerts; c.startIndex = startIndex; c.indexed = true;
	Ps3Gfx::Draw(this, c);
	return S_OK;
}

HRESULT IDirect3DDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primCount, const void* data, UINT stride)
{
	Ps3Gfx::DrawCall c;
	memset(&c, 0, sizeof(c));
	c.type = type; c.primCount = primCount; c.vertexData = data; c.vertexStride = stride;
	c.vertexCount = VerticesForPrims(type, primCount);
	Ps3Gfx::Draw(this, c);
	return S_OK;
}

HRESULT IDirect3DDevice9::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVerts, UINT primCount, const void* indices, D3DFORMAT ifmt, const void* data, UINT stride)
{
	Ps3Gfx::DrawCall c;
	memset(&c, 0, sizeof(c));
	c.type = type; c.primCount = primCount; c.minIndex = minIndex; c.vertexCount = numVerts;
	c.vertexData = data; c.vertexStride = stride; c.indexData = indices; c.indexFormat = ifmt; c.indexed = true;
	Ps3Gfx::Draw(this, c);
	return S_OK;
}

// ---------------------------------------------------------------------------
// IDirect3D9
// ---------------------------------------------------------------------------
static IDirect3D9 g_d3d;

IDirect3D9* Direct3DCreate9(UINT)
{
	return &g_d3d;
}

HRESULT IDirect3D9::GetAdapterDisplayMode(UINT, D3DDISPLAYMODE* mode)
{
	if (g_device)
		return g_device->GetDisplayMode(0, mode);
	mode->Width = 1280; mode->Height = 720; mode->RefreshRate = 60; mode->Format = D3DFMT_X8R8G8B8;
	return S_OK;
}

HRESULT IDirect3D9::GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9* id)
{
	memset(id, 0, sizeof(*id));
	strcpy(id->Driver, "rsx");
	strcpy(id->Description, "PlayStation 3 RSX");
	id->VendorId = 0x10DE;	// NVIDIA
	return S_OK;
}

HRESULT IDirect3D9::GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9* caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
	caps->MaxAnisotropy = 16;
	caps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
	caps->MaxTextureBlendStages = 8;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxStreams = 4;
	caps->MaxStreamStride = 255;
	caps->MaxPrimitiveCount = 0xFFFFF;
	caps->MaxVertexIndex = 0xFFFFF;
	caps->MaxPointSize = 64.0f;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 96;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 3);
	caps->PixelShader1xMaxValue = 1.0f;
	return S_OK;
}

HRESULT IDirect3D9::CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev)
{
	if (!Ps3Gfx::Init(pp))
	{
		*dev = NULL;
		return D3DERR_NOTAVAILABLE;
	}
	*dev = new IDirect3DDevice9(pp);
	return S_OK;
}
