// THUG PS3: D3DX helpers (see d3dx9.h).

#include <d3dx9.h>
#include <stdio.h>
#include <string.h>

D3DXMATRIX* D3DXMatrixInverse(D3DXMATRIX* o, float* det, const D3DXMATRIX* a)
{
	const float* m = &a->_11;
	float inv[16];

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

	float d = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det) *det = d;
	if (d == 0.0f)
		return NULL;
	d = 1.0f / d;
	for (int i = 0; i < 16; ++i)
		(&o->_11)[i] = inv[i] * d;
	return o;
}

// The renderer assembles its pixel shaders (some built at run time per material)
// from ps.1.x source. The PS3 backend translates shaders itself, so "assembling"
// keeps the source: a version token, the text in a D3D comment token, the end token.
// Ps3Gfx recognises this form (see ps3_shader.cpp).
HRESULT D3DXAssembleShader(LPCSTR src, UINT len, const D3DXMACRO*, LPD3DXINCLUDE, DWORD, LPD3DXBUFFER* shader, LPD3DXBUFFER* errors)
{
	if (errors) *errors = NULL;
	if (!src || !shader) return D3DERR_INVALIDCALL;

	// Version from the source ("ps.1.1", "vs.1.1", "xps.1.1").
	DWORD version = 0xFFFF0101;
	const char* v = src;
	while (*v && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) ++v;
	if (!strncmp(v, "vs", 2)) version = 0xFFFE0101;
	const char* dot = strchr(v, '.');
	if (dot && dot[1] >= '0' && dot[1] <= '9' && dot[2] == '.' && dot[3] >= '0' && dot[3] <= '9')
		version = (version & 0xFFFF0000) | ((dot[1] - '0') << 8) | (dot[3] - '0');

	UINT textDwords = (len + 1 + 3) / 4 + 1;		// + "PS3S" tag
	UINT total = 1 + 1 + textDwords + 1;
	ID3DXBuffer* buf = new ID3DXBuffer(total * 4);
	DWORD* t = (DWORD*)buf->GetBufferPointer();
	memset(t, 0, total * 4);
	t[0] = version;
	t[1] = 0x0000FFFE | (textDwords << 16);			// comment token
	memcpy(&t[2], "PS3S", 4);
	memcpy(&t[3], src, len);
	t[total - 1] = 0x0000FFFF;
	*shader = buf;
	return S_OK;
}

HRESULT D3DXCreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** pp)
{
	return dev->CreateTexture(w, h, levels == D3DX_DEFAULT ? 0 : levels, usage, fmt, pool, pp, NULL);
}

HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface9* dst, const void*, const RECT*, IDirect3DSurface9* src, const void*, const RECT*, DWORD, D3DCOLOR)
{
	// Only used for same-size, same-format copies (screenshots, save icons).
	D3DLOCKED_RECT d, s;
	if (dst->m_format != src->m_format || dst->m_width != src->m_width || dst->m_height != src->m_height)
		return E_NOTIMPL;
	if (FAILED(dst->LockRect(&d, NULL, 0)) || FAILED(src->LockRect(&s, NULL, 0)))
		return E_FAIL;
	UINT rowBytes = (UINT)(d.Pitch < s.Pitch ? d.Pitch : s.Pitch);
	for (UINT y = 0; y < dst->m_height; ++y)
		memcpy((BYTE*)d.pBits + y * d.Pitch, (BYTE*)s.pBits + y * s.Pitch, rowBytes);
	src->UnlockRect();
	dst->UnlockRect();
	return S_OK;
}

HRESULT D3DXSaveTextureToFileA(LPCSTR file, D3DXIMAGE_FILEFORMAT, IDirect3DBaseTexture9*, const void*)
{
	printf("[d3dx] D3DXSaveTextureToFile(%s) not supported on PS3\n", file ? file : "");
	return E_NOTIMPL;
}
