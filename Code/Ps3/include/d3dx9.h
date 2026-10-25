// THUG PS3: the D3DX9 math and helper subset used by the renderer.
#ifndef PS3_D3DX9_H
#define PS3_D3DX9_H

#include "d3d9.h"
#include <math.h>

#define D3DX_PI    3.141592654f
#define D3DX_1BYPI 0.318309886f
#define D3DXToRadian(d) ((d) * (D3DX_PI / 180.0f))
#define D3DXToDegree(r) ((r) * (180.0f / D3DX_PI))

#define D3DX_DEFAULT        ((UINT)-1)
#define D3DX_FILTER_NONE    0x00000001
#define D3DX_FILTER_POINT   0x00000002
#define D3DX_FILTER_LINEAR  0x00000003
#define D3DX_FILTER_BOX     0x00000005

typedef enum _D3DXIMAGE_FILEFORMAT { D3DXIFF_BMP = 0, D3DXIFF_JPG = 1, D3DXIFF_TGA = 2, D3DXIFF_PNG = 3, D3DXIFF_DDS = 4 } D3DXIMAGE_FILEFORMAT;

// ---------------------------------------------------------------------------
// Vectors / matrices
// ---------------------------------------------------------------------------
struct D3DXVECTOR2
{
	float x, y;
	D3DXVECTOR2() {}
	D3DXVECTOR2(float _x, float _y) : x(_x), y(_y) {}
	operator float*() { return &x; }
	operator const float*() const { return &x; }
};

struct D3DXVECTOR3 : public D3DVECTOR
{
	D3DXVECTOR3() {}
	D3DXVECTOR3(const float* f) { x = f[0]; y = f[1]; z = f[2]; }
	D3DXVECTOR3(const D3DVECTOR& v) { x = v.x; y = v.y; z = v.z; }
	D3DXVECTOR3(float _x, float _y, float _z) { x = _x; y = _y; z = _z; }
	operator float*() { return &x; }
	operator const float*() const { return &x; }
	D3DXVECTOR3& operator+=(const D3DXVECTOR3& v) { x += v.x; y += v.y; z += v.z; return *this; }
	D3DXVECTOR3& operator-=(const D3DXVECTOR3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
	D3DXVECTOR3& operator*=(float f) { x *= f; y *= f; z *= f; return *this; }
	D3DXVECTOR3& operator/=(float f) { float i = 1.0f / f; x *= i; y *= i; z *= i; return *this; }
	D3DXVECTOR3 operator+() const { return *this; }
	D3DXVECTOR3 operator-() const { return D3DXVECTOR3(-x, -y, -z); }
	D3DXVECTOR3 operator+(const D3DXVECTOR3& v) const { return D3DXVECTOR3(x + v.x, y + v.y, z + v.z); }
	D3DXVECTOR3 operator-(const D3DXVECTOR3& v) const { return D3DXVECTOR3(x - v.x, y - v.y, z - v.z); }
	D3DXVECTOR3 operator*(float f) const { return D3DXVECTOR3(x * f, y * f, z * f); }
	D3DXVECTOR3 operator/(float f) const { float i = 1.0f / f; return D3DXVECTOR3(x * i, y * i, z * i); }
	friend D3DXVECTOR3 operator*(float f, const D3DXVECTOR3& v) { return v * f; }
	bool operator==(const D3DXVECTOR3& v) const { return x == v.x && y == v.y && z == v.z; }
	bool operator!=(const D3DXVECTOR3& v) const { return !(*this == v); }
};

struct D3DXVECTOR4
{
	float x, y, z, w;
	D3DXVECTOR4() {}
	D3DXVECTOR4(const float* f) : x(f[0]), y(f[1]), z(f[2]), w(f[3]) {}
	D3DXVECTOR4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
	D3DXVECTOR4(const D3DVECTOR& v, float _w) : x(v.x), y(v.y), z(v.z), w(_w) {}
	operator float*() { return &x; }
	operator const float*() const { return &x; }
	D3DXVECTOR4 operator+(const D3DXVECTOR4& v) const { return D3DXVECTOR4(x + v.x, y + v.y, z + v.z, w + v.w); }
	D3DXVECTOR4 operator-(const D3DXVECTOR4& v) const { return D3DXVECTOR4(x - v.x, y - v.y, z - v.z, w - v.w); }
	D3DXVECTOR4 operator*(float f) const { return D3DXVECTOR4(x * f, y * f, z * f, w * f); }
	D3DXVECTOR4& operator*=(float f) { x *= f; y *= f; z *= f; w *= f; return *this; }
};

struct D3DXMATRIX : public D3DMATRIX
{
	D3DXMATRIX() {}
	D3DXMATRIX(const float* f) { memcpy(m, f, sizeof(m)); }
	D3DXMATRIX(const D3DMATRIX& o) { memcpy(m, o.m, sizeof(m)); }
	D3DXMATRIX(float f11, float f12, float f13, float f14, float f21, float f22, float f23, float f24,
	           float f31, float f32, float f33, float f34, float f41, float f42, float f43, float f44)
	{
		_11 = f11; _12 = f12; _13 = f13; _14 = f14; _21 = f21; _22 = f22; _23 = f23; _24 = f24;
		_31 = f31; _32 = f32; _33 = f33; _34 = f34; _41 = f41; _42 = f42; _43 = f43; _44 = f44;
	}
	float& operator()(UINT r, UINT c) { return m[r][c]; }
	float operator()(UINT r, UINT c) const { return m[r][c]; }
	operator float*() { return &_11; }
	operator const float*() const { return &_11; }
	D3DXMATRIX operator*(const D3DXMATRIX& b) const
	{
		D3DXMATRIX o;
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				o.m[r][c] = m[r][0] * b.m[0][c] + m[r][1] * b.m[1][c] + m[r][2] * b.m[2][c] + m[r][3] * b.m[3][c];
		return o;
	}
	D3DXMATRIX& operator*=(const D3DXMATRIX& b) { *this = *this * b; return *this; }
};

struct D3DXCOLOR
{
	float r, g, b, a;
	D3DXCOLOR() {}
	D3DXCOLOR(float _r, float _g, float _b, float _a) : r(_r), g(_g), b(_b), a(_a) {}
	D3DXCOLOR(DWORD c)
	{
		const float f = 1.0f / 255.0f;
		r = f * (float)(BYTE)(c >> 16); g = f * (float)(BYTE)(c >> 8);
		b = f * (float)(BYTE)(c >> 0); a = f * (float)(BYTE)(c >> 24);
	}
	operator DWORD() const
	{
		DWORD R = r >= 1.0f ? 0xff : r <= 0.0f ? 0x00 : (DWORD)(r * 255.0f + 0.5f);
		DWORD G = g >= 1.0f ? 0xff : g <= 0.0f ? 0x00 : (DWORD)(g * 255.0f + 0.5f);
		DWORD B = b >= 1.0f ? 0xff : b <= 0.0f ? 0x00 : (DWORD)(b * 255.0f + 0.5f);
		DWORD A = a >= 1.0f ? 0xff : a <= 0.0f ? 0x00 : (DWORD)(a * 255.0f + 0.5f);
		return (A << 24) | (R << 16) | (G << 8) | B;
	}
	operator float*() { return &r; }
	operator const float*() const { return &r; }
};

struct D3DXQUATERNION { float x, y, z, w; };
struct D3DXPLANE { float a, b, c, d; };

static inline D3DXMATRIX* D3DXMatrixIdentity(D3DXMATRIX* o)
{
	memset(o->m, 0, sizeof(o->m)); o->_11 = o->_22 = o->_33 = o->_44 = 1.0f; return o;
}
static inline D3DXMATRIX* D3DXMatrixMultiply(D3DXMATRIX* o, const D3DXMATRIX* a, const D3DXMATRIX* b)
{
	D3DXMATRIX t = *a * *b; *o = t; return o;
}
static inline D3DXMATRIX* D3DXMatrixTranspose(D3DXMATRIX* o, const D3DXMATRIX* a)
{
	D3DXMATRIX t; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) t.m[r][c] = a->m[c][r];
	*o = t; return o;
}
static inline D3DXMATRIX* D3DXMatrixTranslation(D3DXMATRIX* o, float x, float y, float z)
{
	D3DXMatrixIdentity(o); o->_41 = x; o->_42 = y; o->_43 = z; return o;
}
static inline D3DXMATRIX* D3DXMatrixScaling(D3DXMATRIX* o, float x, float y, float z)
{
	D3DXMatrixIdentity(o); o->_11 = x; o->_22 = y; o->_33 = z; return o;
}
static inline D3DXMATRIX* D3DXMatrixRotationX(D3DXMATRIX* o, float a)
{
	float s = sinf(a), c = cosf(a); D3DXMatrixIdentity(o); o->_22 = c; o->_23 = s; o->_32 = -s; o->_33 = c; return o;
}
static inline D3DXMATRIX* D3DXMatrixRotationY(D3DXMATRIX* o, float a)
{
	float s = sinf(a), c = cosf(a); D3DXMatrixIdentity(o); o->_11 = c; o->_13 = -s; o->_31 = s; o->_33 = c; return o;
}
static inline D3DXMATRIX* D3DXMatrixRotationZ(D3DXMATRIX* o, float a)
{
	float s = sinf(a), c = cosf(a); D3DXMatrixIdentity(o); o->_11 = c; o->_12 = s; o->_21 = -s; o->_22 = c; return o;
}
D3DXMATRIX* D3DXMatrixInverse(D3DXMATRIX* o, float* det, const D3DXMATRIX* a);

static inline D3DXVECTOR3* D3DXVec3Normalize(D3DXVECTOR3* o, const D3DXVECTOR3* v)
{
	float l = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
	if (l > 0.0f) { float i = 1.0f / l; *o = D3DXVECTOR3(v->x * i, v->y * i, v->z * i); } else *o = D3DXVECTOR3(0, 0, 0);
	return o;
}
static inline float D3DXVec3Dot(const D3DXVECTOR3* a, const D3DXVECTOR3* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
static inline float D3DXVec3Length(const D3DXVECTOR3* v) { return sqrtf(D3DXVec3Dot(v, v)); }
static inline D3DXVECTOR3* D3DXVec3Cross(D3DXVECTOR3* o, const D3DXVECTOR3* a, const D3DXVECTOR3* b)
{
	D3DXVECTOR3 t(a->y * b->z - a->z * b->y, a->z * b->x - a->x * b->z, a->x * b->y - a->y * b->x); *o = t; return o;
}
static inline D3DXVECTOR3* D3DXVec3Scale(D3DXVECTOR3* o, const D3DXVECTOR3* v, float s) { *o = *v * s; return o; }
static inline D3DXVECTOR4* D3DXVec3Transform(D3DXVECTOR4* o, const D3DXVECTOR3* v, const D3DXMATRIX* m)
{
	D3DXVECTOR4 t(v->x * m->_11 + v->y * m->_21 + v->z * m->_31 + m->_41,
	              v->x * m->_12 + v->y * m->_22 + v->z * m->_32 + m->_42,
	              v->x * m->_13 + v->y * m->_23 + v->z * m->_33 + m->_43,
	              v->x * m->_14 + v->y * m->_24 + v->z * m->_34 + m->_44);
	*o = t; return o;
}
static inline D3DXVECTOR3* D3DXVec3TransformCoord(D3DXVECTOR3* o, const D3DXVECTOR3* v, const D3DXMATRIX* m)
{
	D3DXVECTOR4 t; D3DXVec3Transform(&t, v, m);
	float iw = (t.w != 0.0f) ? 1.0f / t.w : 0.0f;
	*o = D3DXVECTOR3(t.x * iw, t.y * iw, t.z * iw); return o;
}
static inline D3DXVECTOR3* D3DXVec3TransformNormal(D3DXVECTOR3* o, const D3DXVECTOR3* v, const D3DXMATRIX* m)
{
	D3DXVECTOR3 t(v->x * m->_11 + v->y * m->_21 + v->z * m->_31,
	              v->x * m->_12 + v->y * m->_22 + v->z * m->_32,
	              v->x * m->_13 + v->y * m->_23 + v->z * m->_33);
	*o = t; return o;
}
static inline D3DXVECTOR4* D3DXVec4Transform(D3DXVECTOR4* o, const D3DXVECTOR4* v, const D3DXMATRIX* m)
{
	D3DXVECTOR4 t(v->x * m->_11 + v->y * m->_21 + v->z * m->_31 + v->w * m->_41,
	              v->x * m->_12 + v->y * m->_22 + v->z * m->_32 + v->w * m->_42,
	              v->x * m->_13 + v->y * m->_23 + v->z * m->_33 + v->w * m->_43,
	              v->x * m->_14 + v->y * m->_24 + v->z * m->_34 + v->w * m->_44);
	*o = t; return o;
}
static inline D3DXMATRIX* D3DXMatrixLookAtRH(D3DXMATRIX* o, const D3DXVECTOR3* eye, const D3DXVECTOR3* at, const D3DXVECTOR3* up)
{
	D3DXVECTOR3 z = *eye - *at, x, y; D3DXVec3Normalize(&z, &z);
	D3DXVec3Cross(&x, up, &z); D3DXVec3Normalize(&x, &x); D3DXVec3Cross(&y, &z, &x);
	*o = D3DXMATRIX(x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0,
	                -D3DXVec3Dot(&x, eye), -D3DXVec3Dot(&y, eye), -D3DXVec3Dot(&z, eye), 1);
	return o;
}
static inline D3DXMATRIX* D3DXMatrixOrthoRH(D3DXMATRIX* o, float w, float h, float zn, float zf)
{
	D3DXMatrixIdentity(o); o->_11 = 2.0f / w; o->_22 = 2.0f / h; o->_33 = 1.0f / (zn - zf); o->_43 = zn / (zn - zf); return o;
}
static inline D3DXMATRIX* D3DXMatrixPerspectiveRH(D3DXMATRIX* o, float w, float h, float zn, float zf)
{
	memset(o->m, 0, sizeof(o->m));
	o->_11 = 2.0f * zn / w; o->_22 = 2.0f * zn / h; o->_33 = zf / (zn - zf); o->_34 = -1.0f; o->_43 = zn * zf / (zn - zf);
	return o;
}
static inline D3DXMATRIX* D3DXMatrixPerspectiveFovRH(D3DXMATRIX* o, float fovy, float aspect, float zn, float zf)
{
	float ys = 1.0f / tanf(fovy * 0.5f), xs = ys / aspect;
	memset(o->m, 0, sizeof(o->m));
	o->_11 = xs; o->_22 = ys; o->_33 = zf / (zn - zf); o->_34 = -1.0f; o->_43 = zn * zf / (zn - zf);
	return o;
}

// ---------------------------------------------------------------------------
// Shader assembly and texture helpers (Code/Ps3/d3d9/d3dx_ps3.cpp)
// ---------------------------------------------------------------------------
class ID3DXBuffer
{
public:
	ID3DXBuffer(UINT size) : m_size(size), m_data(size ? malloc(size) : 0) {}
	~ID3DXBuffer() { free(m_data); }
	void* GetBufferPointer() { return m_data; }
	DWORD GetBufferSize() { return m_size; }
	ULONG AddRef() { return 1; }
	ULONG Release() { delete this; return 0; }
private:
	UINT m_size;
	void* m_data;
};
typedef ID3DXBuffer* LPD3DXBUFFER;
typedef struct _D3DXMACRO { LPCSTR Name; LPCSTR Definition; } D3DXMACRO;
typedef void* LPD3DXINCLUDE;

// Assembles vs_1_x / ps_1_x shader source into D3D shader tokens. The RSX backend
// translates those tokens into RSX programs when the shader is created.
HRESULT D3DXAssembleShader(LPCSTR src, UINT len, const D3DXMACRO* defines, LPD3DXINCLUDE include,
                           DWORD flags, LPD3DXBUFFER* shader, LPD3DXBUFFER* errors);

HRESULT D3DXCreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage,
                          D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** pp);
HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface9* dst, const void* dstPal, const RECT* dstRect,
                                   IDirect3DSurface9* src, const void* srcPal, const RECT* srcRect,
                                   DWORD filter, D3DCOLOR colorKey);
HRESULT D3DXSaveTextureToFileA(LPCSTR file, D3DXIMAGE_FILEFORMAT fmt, IDirect3DBaseTexture9* tex, const void* pal);
#define D3DXSaveTextureToFile D3DXSaveTextureToFileA

#endif // PS3_D3DX9_H
