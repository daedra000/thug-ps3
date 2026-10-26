// THUG PS3: what the D3D9 layer (Code/Ps3/d3d9) needs from the GPU backend.
//
// The D3D9 objects keep all state and CPU copies of resources; a backend only
// has to put them on screen. Two backends exist: RSX (d3d9_rsx.cpp) and none
// (d3d9_null.cpp, the HEADLESS=1 build).
#ifndef PS3_GFX_BACKEND_H
#define PS3_GFX_BACKEND_H

#include "d3d9.h"

namespace Ps3Gfx
{
	// Video set-up; fills in the back buffer size the TV mode allows.
	bool Init(D3DPRESENT_PARAMETERS* pp);
	void Shutdown();

	void BeginFrame(IDirect3DDevice9* dev);
	void Present(IDirect3DDevice9* dev);
	void Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);

	// Draws with the device's current state. Vertices/indices are either in the
	// bound buffers (vertexData == NULL) or passed directly (the *UP calls).
	struct DrawCall
	{
		D3DPRIMITIVETYPE type;
		UINT vertexCount;		// vertices read (indexed: numVerts from minIndex)
		UINT primCount;
		INT baseVertex;
		UINT minIndex;
		UINT startIndex;		// indexed only
		UINT startVertex;		// non-indexed only
		const void* vertexData;	// *UP: the caller's vertices
		UINT vertexStride;		// *UP
		const void* indexData;	// *UP: the caller's indices
		D3DFORMAT indexFormat;	// *UP
		bool indexed;
	};
	void Draw(IDirect3DDevice9* dev, const DrawCall& call);

	// Resources: the backend may keep GPU copies (resource->m_backend) and is told
	// when CPU data changes or the resource goes away.
	void TextureChanged(IDirect3DTexture9* tex);
	void TextureReleased(IDirect3DTexture9* tex);
	void SurfaceReleased(IDirect3DSurface9* surf);
	void VertexBufferChanged(IDirect3DVertexBuffer9* vb, UINT offset, UINT size);
	void VertexBufferReleased(IDirect3DVertexBuffer9* vb);
	void IndexBufferChanged(IDirect3DIndexBuffer9* ib, UINT offset, UINT size);
	void IndexBufferReleased(IDirect3DIndexBuffer9* ib);
	void ShaderCreated(IDirect3DVertexShader9* vs);
	void ShaderCreated(IDirect3DPixelShader9* ps);
	void ShaderReleased(IDirect3DVertexShader9* vs);
	void ShaderReleased(IDirect3DPixelShader9* ps);

	UINT VideoMemoryFree();

	// The backend's lock (recursive). The D3D objects take it around their Lock calls, so the
	// backend never releases a main-memory copy that is being handed out or written.
	void DeviceLock();
	void DeviceUnlock();
	// The RSX backend frees an uploaded texture's or static vertex buffer's main-memory copy
	// once it stops changing (m_dataReleased). These bring it back when the game locks the
	// resource again: read back from GPU memory (contents false: D3DLOCK_DISCARD, zeroed).
	// False only when there was no memory for it.
	bool RestoreTextureData(IDirect3DTexture9* tex);
	bool RestoreVertexBufferData(IDirect3DVertexBuffer9* vb, bool contents);
}

#endif // PS3_GFX_BACKEND_H
