// THUG PS3: the "no GPU" backend for HEADLESS=1 builds. The game loads and runs
// with all rendering state kept by the D3D9 layer but nothing drawn, which lets
// the data loading and game logic be tested on a console before the RSX renderer.

#include <ps3_platform.h>

#if PS3_HEADLESS

#include <ps3_gfx_backend.h>
#include <stdio.h>
#include <sys/systime.h>

namespace Ps3Gfx
{

static unsigned s_frames;
static unsigned s_draws;

bool Init(D3DPRESENT_PARAMETERS* pp)
{
	if (pp->BackBufferWidth == 0)  pp->BackBufferWidth = 1280;
	if (pp->BackBufferHeight == 0) pp->BackBufferHeight = 720;
	if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_A8R8G8B8;
	printf("[gfx] headless build: %ux%u, nothing is drawn\n", pp->BackBufferWidth, pp->BackBufferHeight);
	return true;
}

void Shutdown() {}
void BeginFrame(IDirect3DDevice9*) {}

void Present(IDirect3DDevice9*)
{
	++s_frames;
	if ((s_frames % 600) == 0)
		printf("[gfx] frame %u (%u draws in the last one)\n", s_frames, s_draws);
	s_draws = 0;
	sysUsleep(16666);	// roughly 60 frames a second, as with vsync
}

void Clear(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD) {}
void Draw(IDirect3DDevice9*, const DrawCall&) { ++s_draws; }

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

UINT VideoMemoryFree() { return 200 * 1024 * 1024; }

// Nothing is uploaded, so no main-memory copy is ever released and nothing needs locking.
void DeviceLock() {}
void DeviceUnlock() {}
bool RestoreTextureData(IDirect3DTexture9* tex) { tex->m_dataReleased = false; return tex->m_data != NULL; }
bool RestoreVertexBufferData(IDirect3DVertexBuffer9* vb, bool) { vb->m_dataReleased = false; return vb->m_data != NULL; }

} // namespace Ps3Gfx

#endif // PS3_HEADLESS
