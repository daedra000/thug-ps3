// Checks the RSX backend's shader maths on a PC: D3D9 device state goes through
// rsx_pipeline (keys, constants) and rsx_shadergen (programs), the programs run in
// interp.h, and the results are compared with D3D9's fixed-function maths written out
// independently here. Run by tools/rsx-asm-test/run.sh.
#include "interp.h"
#include "../../Code/Ps3/rsx/rsx_pipeline.h"
#include "../../Code/Ps3/rsx/rsx_shadergen.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

using namespace RsxShaderGen;
using namespace RsxPipeline;

static int g_checks, g_failures;

static void Expect(const char* what, float got, float want, float tol = 1e-3f)
{
	++g_checks;
	float err = fabsf(got - want), scale = fabsf(want) > 1 ? fabsf(want) : 1;
	if (err <= tol * scale) return;
	++g_failures;
	printf("FAIL %s: got %g, want %g\n", what, got, want);
}

static float Rnd(float lo, float hi) { return lo + (hi - lo) * (float)(rand() % 100000) / 100000.0f; }

// ---- reference maths (row vectors, as D3D)
struct M4 { float m[4][4]; };
static M4 Mul(const M4& a, const M4& b)
{
	M4 r;
	for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
	{ r.m[i][j] = 0; for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j]; }
	return r;
}
static void Xform(const float* v, const M4& m, float* out)
{
	for (int j = 0; j < 4; ++j) out[j] = v[0] * m.m[0][j] + v[1] * m.m[1][j] + v[2] * m.m[2][j] + v[3] * m.m[3][j];
}
static M4 Inverse(const M4& a)
{
	// Gauss-Jordan
	float t[4][8];
	for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) t[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0f : 0.0f);
	for (int c = 0; c < 4; ++c)
	{
		int p = c;
		for (int r = c + 1; r < 4; ++r) if (fabsf(t[r][c]) > fabsf(t[p][c])) p = r;
		for (int j = 0; j < 8; ++j) { float x = t[c][j]; t[c][j] = t[p][j]; t[p][j] = x; }
		float d = t[c][c];
		for (int j = 0; j < 8; ++j) t[c][j] /= d;
		for (int r = 0; r < 4; ++r) if (r != c) { float f = t[r][c]; for (int j = 0; j < 8; ++j) t[r][j] -= f * t[c][j]; }
	}
	M4 r;
	for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i][j] = t[i][j + 4];
	return r;
}
static M4 RandomRigid()
{
	// rotation from random axis/angle + translation
	float ax = Rnd(-1, 1), ay = Rnd(-1, 1), az = Rnd(-1, 1), l = sqrtf(ax * ax + ay * ay + az * az);
	ax /= l; ay /= l; az /= l;
	float an = Rnd(0, 6.28f), c = cosf(an), s = sinf(an), t = 1 - c;
	M4 m = { { { t * ax * ax + c, t * ax * ay + s * az, t * ax * az - s * ay, 0 },
	           { t * ax * ay - s * az, t * ay * ay + c, t * ay * az + s * ax, 0 },
	           { t * ax * az + s * ay, t * ay * az - s * ax, t * az * az + c, 0 },
	           { Rnd(-50, 50), Rnd(-50, 50), Rnd(-50, 50), 1 } } };
	return m;
}
static M4 Perspective(float fov, float aspect, float zn, float zf)
{
	float ys = 1.0f / tanf(fov / 2), xs = ys / aspect;
	M4 m = { { { xs, 0, 0, 0 }, { 0, ys, 0, 0 }, { 0, 0, zf / (zf - zn), 1 }, { 0, 0, -zn * zf / (zf - zn), 0 } } };
	return m;
}
static void ToD3D(const M4& a, D3DMATRIX& d) { memcpy(d.m, a.m, sizeof(d.m)); }
static void Norm3(float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A device with D3D9's defaults for what the pipeline reads.
static IDirect3DDevice9* NewDevice()
{
	static char storage[sizeof(IDirect3DDevice9)] __attribute__((aligned(16)));
	memset(storage, 0, sizeof(storage));
	IDirect3DDevice9* d = (IDirect3DDevice9*)storage;
	for (int i = 0; i < 512; ++i) { memset(&d->m_transforms[i], 0, sizeof(D3DMATRIX)); for (int k = 0; k < 4; ++k) d->m_transforms[i].m[k][k] = 1; }
	d->m_rs[D3DRS_LIGHTING] = TRUE;
	d->m_rs[D3DRS_COLORVERTEX] = TRUE;
	d->m_rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
	d->m_rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
	d->m_rs[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
	d->m_rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
	d->m_rs[D3DRS_LOCALVIEWER] = TRUE;
	for (int s = 0; s < 8; ++s)
	{
		d->m_tss[s][D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
		d->m_tss[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
		d->m_tss[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
		d->m_tss[s][D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
		d->m_tss[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
		d->m_tss[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
		d->m_tss[s][D3DTSS_TEXCOORDINDEX] = s;
	}
	d->m_viewport.X = 0; d->m_viewport.Y = 0; d->m_viewport.Width = 1280; d->m_viewport.Height = 720;
	d->m_viewport.MinZ = 0; d->m_viewport.MaxZ = 1;
	return d;
}

// Runs the vertex program for the device's state on one vertex (attribute values in `in`).
static bool RunVertex(IDirect3DDevice9* dev, DWORD fvf, int numTexOut, const Vec4* attribs, NvInterp** outInterp)
{
	dev->m_fvf = fvf;
	FvfLayout l = Layout(fvf);
	VsKey k;
	FillVsKey(dev, l, numTexOut, k);
	std::string src = VertexProgram(k);
	NvInterp* vm = new NvInterp(src);
	float c[kVcCount][4];
	FillVsConstants(dev, c);
	for (int i = 0; i < kVcCount; ++i) for (int j = 0; j < 4; ++j) vm->c[i][j] = c[i][j];
	for (int i = 0; i < 16; ++i) vm->in[i] = attribs[i];
	if (!vm->Run()) { printf("FAIL vertex program: %s\n%s\n", vm->error.c_str(), src.c_str()); ++g_failures; return false; }
	*outInterp = vm;
	return true;
}

// A D3DCOLOR as the RSX reads it: four bytes in memory order, host-order word (A,R,G,B).
static Vec4 ColorAttrib(D3DCOLOR c)
{
	Vec4 v = { { ((c >> 24) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f } };
	return v;
}

static void TestTransform()
{
	for (int iter = 0; iter < 50; ++iter)
	{
		IDirect3DDevice9* dev = NewDevice();
		dev->m_rs[D3DRS_LIGHTING] = FALSE;
		M4 W = RandomRigid(), V = RandomRigid(), P = Perspective(Rnd(0.8f, 1.6f), 16.0f / 9, Rnd(0.5f, 5), Rnd(500, 5000));
		W.m[0][0] *= 2.0f;	// a scale too
		ToD3D(W, dev->m_transforms[256]); ToD3D(V, dev->m_transforms[D3DTS_VIEW]); ToD3D(P, dev->m_transforms[D3DTS_PROJECTION]);
		dev->m_viewport.X = (DWORD)Rnd(0, 100); dev->m_viewport.Y = (DWORD)Rnd(0, 100);
		dev->m_viewport.Width = 640 + (DWORD)Rnd(0, 600); dev->m_viewport.Height = 360 + (DWORD)Rnd(0, 300);
		dev->m_viewport.MinZ = Rnd(0, 0.2f); dev->m_viewport.MaxZ = Rnd(0.8f, 1);

		Vec4 a[16]; memset(a, 0, sizeof(a));
		float pos[4] = { Rnd(-20, 20), Rnd(-20, 20), Rnd(-20, 20), 1 };
		a[0][0] = pos[0]; a[0][1] = pos[1]; a[0][2] = pos[2]; a[0][3] = 1;
		NvInterp* vm;
		if (!RunVertex(dev, D3DFVF_XYZ, 0, a, &vm)) return;
		float ref[4];
		Xform(pos, Mul(Mul(W, V), P), ref);
		const Vec4& o = vm->out[0];
		Expect("clip x", o[0], ref[0]); Expect("clip y", o[1], ref[1]); Expect("clip w", o[3], ref[3]);
		Expect("clip z (GL)", o[2], 2 * ref[2] - ref[3]);
		// through the backend's viewport transform -> D3D screen position (+ half pixel)
		float scale[4], offset[4];
		ViewportTransform(dev->m_viewport, scale, offset);
		if (fabsf(o[3]) > 1e-3f)
		{
			float wx = o[0] / o[3] * scale[0] + offset[0], wy = o[1] / o[3] * scale[1] + offset[1], wz = o[2] / o[3] * scale[2] + offset[2];
			const D3DVIEWPORT9& vp = dev->m_viewport;
			float nx = ref[0] / ref[3], ny = ref[1] / ref[3], nz = ref[2] / ref[3];
			Expect("window x", wx, (nx + 1) * vp.Width / 2 + vp.X + 0.5f, 1e-4f);
			Expect("window y", wy, (1 - ny) * vp.Height / 2 + vp.Y + 0.5f, 1e-4f);
			Expect("window z", wz, vp.MinZ + nz * (vp.MaxZ - vp.MinZ), 1e-4f);
		}
		delete vm;
	}
}

static void TestPretransformed()
{
	for (int iter = 0; iter < 30; ++iter)
	{
		IDirect3DDevice9* dev = NewDevice();
		dev->m_viewport.X = (DWORD)Rnd(0, 50); dev->m_viewport.Y = (DWORD)Rnd(0, 50);
		Vec4 a[16]; memset(a, 0, sizeof(a));
		float x = Rnd(0, 1280), y = Rnd(0, 720), z = Rnd(0, 1), rhw = Rnd(0.01f, 2);
		a[0][0] = x; a[0][1] = y; a[0][2] = z; a[0][3] = rhw;
		D3DCOLOR col = 0x80402010u + (rand() & 0x0F0F0F);
		a[kAttrDiffuse] = ColorAttrib(col);
		NvInterp* vm;
		if (!RunVertex(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, 0, a, &vm)) return;
		float scale[4], offset[4];
		ViewportTransform(dev->m_viewport, scale, offset);
		const Vec4& o = vm->out[0];
		Expect("rhw w", o[3], 1 / rhw);
		Expect("rhw window x", o[0] / o[3] * scale[0] + offset[0], x + 0.5f, 1e-4f);
		Expect("rhw window y", o[1] / o[3] * scale[1] + offset[1], y + 0.5f, 1e-4f);
		Expect("rhw window z", o[2] / o[3] * scale[2] + offset[2], z, 1e-4f);
		const Vec4& c = vm->out[1];
		Expect("diffuse r", c[0], ((col >> 16) & 0xFF) / 255.0f); Expect("diffuse g", c[1], ((col >> 8) & 0xFF) / 255.0f);
		Expect("diffuse b", c[2], (col & 0xFF) / 255.0f); Expect("diffuse a", c[3], (col >> 24) / 255.0f);
		delete vm;
	}
}

static void TestLighting()
{
	for (int iter = 0; iter < 60; ++iter)
	{
		IDirect3DDevice9* dev = NewDevice();
		M4 W = RandomRigid(), V = RandomRigid(), P = Perspective(1.2f, 1.7f, 1, 1000);
		W.m[1][0] *= 1.5f; W.m[1][1] *= 1.5f; W.m[1][2] *= 1.5f;	// non-uniform scale: tests the normal matrix
		ToD3D(W, dev->m_transforms[256]); ToD3D(V, dev->m_transforms[D3DTS_VIEW]); ToD3D(P, dev->m_transforms[D3DTS_PROJECTION]);
		const bool spec = iter & 1, local = (iter >> 1) & 1, vcol = (iter >> 2) & 1;
		dev->m_rs[D3DRS_SPECULARENABLE] = spec;
		dev->m_rs[D3DRS_LOCALVIEWER] = local;
		dev->m_rs[D3DRS_COLORVERTEX] = vcol;
		dev->m_rs[D3DRS_AMBIENT] = 0xFF101820;
		D3DMATERIAL9& m = dev->m_material;
		m.Diffuse.r = Rnd(0, 1); m.Diffuse.g = Rnd(0, 1); m.Diffuse.b = Rnd(0, 1); m.Diffuse.a = Rnd(0, 1);
		m.Ambient.r = Rnd(0, 1); m.Ambient.g = Rnd(0, 1); m.Ambient.b = Rnd(0, 1); m.Ambient.a = 1;
		m.Specular.r = Rnd(0, 1); m.Specular.g = Rnd(0, 1); m.Specular.b = Rnd(0, 1);
		m.Emissive.r = Rnd(0, 0.2f); m.Emissive.g = Rnd(0, 0.2f); m.Emissive.b = Rnd(0, 0.2f);
		m.Power = Rnd(2, 40);
		int nl = 1 + iter % 3;
		for (int i = 0; i < nl; ++i)
		{
			D3DLIGHT9& L = dev->m_lights[i];
			memset(&L, 0, sizeof(L));
			L.Type = (i == 1) ? D3DLIGHT_POINT : D3DLIGHT_DIRECTIONAL;
			L.Diffuse.r = Rnd(0, 1); L.Diffuse.g = Rnd(0, 1); L.Diffuse.b = Rnd(0, 1);
			L.Specular.r = Rnd(0, 1); L.Specular.g = Rnd(0, 1); L.Specular.b = Rnd(0, 1);
			L.Ambient.r = Rnd(0, 0.3f); L.Ambient.g = Rnd(0, 0.3f); L.Ambient.b = Rnd(0, 0.3f);
			L.Direction.x = Rnd(-1, 1); L.Direction.y = Rnd(-1, 1); L.Direction.z = Rnd(-1, 1);
			L.Position.x = Rnd(-30, 30); L.Position.y = Rnd(-30, 30); L.Position.z = Rnd(-30, 30);
			L.Range = 1000; L.Attenuation0 = 0.5f; L.Attenuation1 = 0.02f; L.Attenuation2 = 0.001f;
			dev->m_lightEnabled[i] = TRUE;
		}
		D3DCOLOR vdiff = 0xC0806040u ^ (rand() & 0x3F3F3F), vspec = 0x00204060u ^ (rand() & 0x1F1F1F);
		float pos[4] = { Rnd(-10, 10), Rnd(-10, 10), Rnd(-10, 10), 1 }, n[3] = { Rnd(-1, 1), Rnd(-1, 1), Rnd(-1, 1) };
		Norm3(n);
		Vec4 a[16]; memset(a, 0, sizeof(a));
		for (int i = 0; i < 3; ++i) { a[0][i] = pos[i]; a[kAttrNormal][i] = n[i]; }
		a[0][3] = 1;
		a[kAttrDiffuse] = ColorAttrib(vdiff);
		a[kAttrSpecular] = ColorAttrib(vspec);
		NvInterp* vm;
		if (!RunVertex(dev, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR, 0, a, &vm)) return;

		// Reference, in world space (D3D lights in camera space; same result for a rigid view).
		float wp[4]; Xform(pos, W, wp);
		M4 Winv = Inverse(W);
		float wn[3] = { n[0] * Winv.m[0][0] + n[1] * Winv.m[0][1] + n[2] * Winv.m[0][2],
		                n[0] * Winv.m[1][0] + n[1] * Winv.m[1][1] + n[2] * Winv.m[1][2],
		                n[0] * Winv.m[2][0] + n[1] * Winv.m[2][1] + n[2] * Winv.m[2][2] };
		Norm3(wn);
		M4 Vinv = Inverse(V);
		float eye[3] = { Vinv.m[3][0], Vinv.m[3][1], Vinv.m[3][2] };
		float camZ[3] = { Vinv.m[2][0], Vinv.m[2][1], Vinv.m[2][2] };	// view +z in world
		float toEye[3];
		if (local) { toEye[0] = eye[0] - wp[0]; toEye[1] = eye[1] - wp[1]; toEye[2] = eye[2] - wp[2]; }
		else { toEye[0] = -camZ[0]; toEye[1] = -camZ[1]; toEye[2] = -camZ[2]; }
		Norm3(toEye);
		float Md[4] = { m.Diffuse.r, m.Diffuse.g, m.Diffuse.b, m.Diffuse.a }, Ms[3] = { m.Specular.r, m.Specular.g, m.Specular.b };
		if (vcol) { for (int i = 0; i < 4; ++i) Md[i] = a[kAttrDiffuse][(i + 1) & 3]; for (int i = 0; i < 3; ++i) Ms[i] = a[kAttrSpecular][i + 1]; }
		float ga[3] = { 0x10 / 255.0f, 0x18 / 255.0f, 0x20 / 255.0f };
		float col[3], sp[3] = { 0, 0, 0 };
		col[0] = m.Emissive.r + ga[0] * m.Ambient.r; col[1] = m.Emissive.g + ga[1] * m.Ambient.g; col[2] = m.Emissive.b + ga[2] * m.Ambient.b;
		for (int i = 0; i < nl; ++i)
		{
			const D3DLIGHT9& L = dev->m_lights[i];
			float Ldir[3], att = 1;
			if (L.Type == D3DLIGHT_DIRECTIONAL) { Ldir[0] = -L.Direction.x; Ldir[1] = -L.Direction.y; Ldir[2] = -L.Direction.z; Norm3(Ldir); }
			else
			{
				Ldir[0] = L.Position.x - wp[0]; Ldir[1] = L.Position.y - wp[1]; Ldir[2] = L.Position.z - wp[2];
				float d = sqrtf(Dot3(Ldir, Ldir));
				Norm3(Ldir);
				att = 1.0f / (L.Attenuation0 + L.Attenuation1 * d + L.Attenuation2 * d * d);
			}
			float ndl = Dot3(wn, Ldir); if (ndl < 0) ndl = 0;
			const float La[3] = { L.Ambient.r, L.Ambient.g, L.Ambient.b }, Ld[3] = { L.Diffuse.r, L.Diffuse.g, L.Diffuse.b }, Ls[3] = { L.Specular.r, L.Specular.g, L.Specular.b };
			const float Ma[3] = { m.Ambient.r, m.Ambient.g, m.Ambient.b };
			for (int c = 0; c < 3; ++c) col[c] += att * (La[c] * Ma[c] + Ld[c] * Md[c] * ndl);
			if (spec && ndl > 0)
			{
				float H[3] = { Ldir[0] + toEye[0], Ldir[1] + toEye[1], Ldir[2] + toEye[2] };
				Norm3(H);
				float ndh = Dot3(wn, H); if (ndh < 0) ndh = 0;
				float f = powf(ndh, m.Power) * att;
				for (int c = 0; c < 3; ++c) sp[c] += Ls[c] * Ms[c] * f;
			}
		}
		const Vec4& o = vm->out[1];
		for (int c = 0; c < 3; ++c) { float want = col[c] > 1 ? 1 : col[c]; Expect(c == 0 ? "lit r" : c == 1 ? "lit g" : "lit b", o[c], want, 2e-3f); }
		Expect("lit alpha", o[3], Md[3]);
		if (spec)
			for (int c = 0; c < 3; ++c) { float want = sp[c] > 1 ? 1 : sp[c]; Expect(local ? "specular local" : "specular infinite", vm->out[2][c], want, 2e-3f); }
		delete vm;
	}
}

static void TestTexcoordsAndFog()
{
	for (int iter = 0; iter < 30; ++iter)
	{
		IDirect3DDevice9* dev = NewDevice();
		dev->m_rs[D3DRS_LIGHTING] = FALSE;
		M4 W = RandomRigid(), V = RandomRigid();
		ToD3D(W, dev->m_transforms[256]); ToD3D(V, dev->m_transforms[D3DTS_VIEW]);
		// stage 0: 2D texture transform (UV wibble style, translation in row 3); stage 1: passthru set 1
		M4 T = { { { Rnd(0.5f, 2), Rnd(-0.3f, 0.3f), 0, 0 }, { Rnd(-0.3f, 0.3f), Rnd(0.5f, 2), 0, 0 }, { Rnd(-1, 1), Rnd(-1, 1), 1, 0 }, { 0, 0, 0, 1 } } };
		ToD3D(T, dev->m_transforms[D3DTS_TEXTURE0]);
		dev->m_tss[0][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_COUNT2;
		dev->m_tss[1][D3DTSS_TEXCOORDINDEX] = 1;
		dev->m_rs[D3DRS_FOGENABLE] = TRUE;
		dev->m_rs[D3DRS_FOGVERTEXMODE] = D3DFOG_LINEAR;
		float fs = Rnd(10, 50), fe = fs + Rnd(20, 100);
		memcpy(&dev->m_rs[D3DRS_FOGSTART], &fs, 4); memcpy(&dev->m_rs[D3DRS_FOGEND], &fe, 4);

		Vec4 a[16]; memset(a, 0, sizeof(a));
		float pos[4] = { Rnd(-30, 30), Rnd(-30, 30), Rnd(-30, 30), 1 };
		for (int i = 0; i < 4; ++i) a[0][i] = pos[i];
		float u0 = Rnd(-2, 2), v0 = Rnd(-2, 2), u1 = Rnd(0, 1), v1 = Rnd(0, 1);
		a[kAttrTex0][0] = u0; a[kAttrTex0][1] = v0; a[kAttrTex0 + 1][0] = u1; a[kAttrTex0 + 1][1] = v1;
		NvInterp* vm;
		if (!RunVertex(dev, D3DFVF_XYZ | D3DFVF_TEX2, 2, a, &vm)) return;
		float in0[4] = { u0, v0, 1, 0 }, t0[4];
		Xform(in0, T, t0);
		Expect("tex0 u (transformed)", vm->out[8][0], t0[0]); Expect("tex0 v (transformed)", vm->out[8][1], t0[1]);
		Expect("tex1 u", vm->out[9][0], u1); Expect("tex1 v", vm->out[9][1], v1);
		float e[4]; Xform(pos, Mul(W, V), e);
		float f = (fe - e[2]) / (fe - fs); f = f < 0 ? 0 : f > 1 ? 1 : f;
		Expect("fog factor", vm->out[3][0], f);
		delete vm;
	}
}

// ---- fragment side
static Vec4 V4(float a, float b, float c, float d) { Vec4 v = { { a, b, c, d } }; return v; }
static float Sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static Vec4 RunFragment(const std::string& src, const Vec4& diffuse, const Vec4& specular, const Vec4* tex, const float (*consts)[4], int nconsts, float fog = 1)
{
	NvInterp fm(src);
	for (int i = 0; i < nconsts; ++i) for (int j = 0; j < 4; ++j) fm.c[i][j] = consts[i][j];
	fm.in[16] = diffuse; fm.in[17] = specular; fm.in[18] = V4(fog, 0, 0, 0);
	for (int t = 0; t < 4; ++t) fm.in[20 + t] = V4((float)t, 0, 0, 1);
	fm.sample = [&](int unit, const Vec4&, bool) { return tex[unit]; };
	if (!fm.Run()) { printf("FAIL fragment program: %s\n%s\n", fm.error.c_str(), src.c_str()); ++g_failures; }
	return fm.out[1];
}

static Vec4 Rgba() { return V4(Rnd(0, 1), Rnd(0, 1), Rnd(0, 1), Rnd(0, 1)); }

static void TestCombiners()
{
	struct OpRef { int op; const char* name; };
	static const OpRef ops[] = {
		{ D3DTOP_SELECTARG1, "SELECTARG1" }, { D3DTOP_SELECTARG2, "SELECTARG2" }, { D3DTOP_MODULATE, "MODULATE" },
		{ D3DTOP_MODULATE2X, "MODULATE2X" }, { D3DTOP_MODULATE4X, "MODULATE4X" }, { D3DTOP_ADD, "ADD" },
		{ D3DTOP_ADDSIGNED, "ADDSIGNED" }, { D3DTOP_ADDSIGNED2X, "ADDSIGNED2X" }, { D3DTOP_SUBTRACT, "SUBTRACT" },
		{ D3DTOP_ADDSMOOTH, "ADDSMOOTH" }, { D3DTOP_BLENDDIFFUSEALPHA, "BLENDDIFFUSEALPHA" }, { D3DTOP_BLENDTEXTUREALPHA, "BLENDTEXTUREALPHA" },
		{ D3DTOP_BLENDFACTORALPHA, "BLENDFACTORALPHA" }, { D3DTOP_MODULATEALPHA_ADDCOLOR, "MODULATEALPHA_ADDCOLOR" },
		{ D3DTOP_DOTPRODUCT3, "DOTPRODUCT3" }, { D3DTOP_MULTIPLYADD, "MULTIPLYADD" }, { D3DTOP_LERP, "LERP" } };
	for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi)
	for (int iter = 0; iter < 10; ++iter)
	{
		IDirect3DDevice9* dev = NewDevice();
		const int op = ops[oi].op;
		// Every other run uses the same op for alpha (the colour result must survive the
		// alpha half: a scratch register clash once turned MODULATE2X colour into its alpha).
		const bool alphaSame = (iter & 1) && op != D3DTOP_DOTPRODUCT3 && op != D3DTOP_MODULATEALPHA_ADDCOLOR;
		dev->m_tss[0][D3DTSS_COLOROP] = op;
		dev->m_tss[0][D3DTSS_COLORARG0] = D3DTA_SPECULAR;
		dev->m_tss[0][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
		dev->m_tss[0][D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
		dev->m_tss[0][D3DTSS_ALPHAOP] = alphaSame ? op : D3DTOP_MODULATE;
		dev->m_tss[0][D3DTSS_ALPHAARG0] = D3DTA_SPECULAR;
		dev->m_tss[0][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
		dev->m_tss[0][D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
		dev->m_tss[1][D3DTSS_COLOROP] = D3DTOP_DISABLE;
		D3DCOLOR tf = 0x80000000u | (rand() & 0xFFFFFF);
		dev->m_rs[D3DRS_TEXTUREFACTOR] = tf;
		FsKey k;
		const bool has[4] = { true, false, false, false };
		FillFsKey(dev, k, has);
		std::string src = FixedFunctionFragmentProgram(k);
		Vec4 diff = Rgba(), spec = Rgba(), tex[4] = { Rgba(), Rgba(), Rgba(), Rgba() };
		float consts[2][4];
		ColorToFloats(tf, consts[0]);
		ColorToFloats(0, consts[1]);
		Vec4 r = RunFragment(src, diff, spec, tex, consts, 2);

		const Vec4 &a1 = tex[0], &a2 = diff, &a0 = spec;
		float want[4];
		for (int c = 0; c < 4; ++c)
		{
			float x = 0;
			switch (op)
			{
			case D3DTOP_SELECTARG1: x = a1[c]; break;
			case D3DTOP_SELECTARG2: x = a2[c]; break;
			case D3DTOP_MODULATE: x = a1[c] * a2[c]; break;
			case D3DTOP_MODULATE2X: x = 2 * a1[c] * a2[c]; break;
			case D3DTOP_MODULATE4X: x = 4 * a1[c] * a2[c]; break;
			case D3DTOP_ADD: x = a1[c] + a2[c]; break;
			case D3DTOP_ADDSIGNED: x = a1[c] + a2[c] - 0.5f; break;
			case D3DTOP_ADDSIGNED2X: x = 2 * (a1[c] + a2[c] - 0.5f); break;
			case D3DTOP_SUBTRACT: x = a1[c] - a2[c]; break;
			case D3DTOP_ADDSMOOTH: x = a1[c] + a2[c] - a1[c] * a2[c]; break;
			case D3DTOP_BLENDDIFFUSEALPHA: x = a1[c] * diff[3] + a2[c] * (1 - diff[3]); break;
			case D3DTOP_BLENDTEXTUREALPHA: x = a1[c] * tex[0][3] + a2[c] * (1 - tex[0][3]); break;
			case D3DTOP_BLENDFACTORALPHA: x = a1[c] * consts[0][3] + a2[c] * (1 - consts[0][3]); break;
			case D3DTOP_MODULATEALPHA_ADDCOLOR: x = a1[c] + a1[3] * a2[c]; break;
			case D3DTOP_DOTPRODUCT3: x = 4 * ((a1[0] - .5f) * (a2[0] - .5f) + (a1[1] - .5f) * (a2[1] - .5f) + (a1[2] - .5f) * (a2[2] - .5f)); break;
			case D3DTOP_MULTIPLYADD: x = a0[c] + a1[c] * a2[c]; break;
			case D3DTOP_LERP: x = a0[c] * a1[c] + (1 - a0[c]) * a2[c]; break;
			}
			want[c] = Sat(x);
		}
		char what[64];
		for (int c = 0; c < 3; ++c) { snprintf(what, sizeof(what), "%s %c", ops[oi].name, "rgb"[c]); Expect(what, r[c], want[c]); }
		snprintf(what, sizeof(what), "%s alpha%s", ops[oi].name, alphaSame ? " (same op)" : "");
		Expect(what, r[3], alphaSame ? want[3] : tex[0][3] * diff[3]);
	}

	// Two stages, temp result, specular add and fog.
	for (int iter = 0; iter < 20; ++iter)
	{
		IDirect3DDevice9* dev = NewDevice();
		dev->m_tss[0][D3DTSS_COLOROP] = D3DTOP_MODULATE;		// t0 * diffuse
		dev->m_tss[1][D3DTSS_COLOROP] = D3DTOP_BLENDTEXTUREALPHA;	// lerp(t1.a, t1, current)
		dev->m_tss[1][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
		dev->m_tss[1][D3DTSS_COLORARG2] = D3DTA_CURRENT;
		dev->m_tss[1][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
		dev->m_tss[1][D3DTSS_ALPHAARG1] = D3DTA_CURRENT;
		dev->m_rs[D3DRS_SPECULARENABLE] = TRUE;
		dev->m_rs[D3DRS_FOGENABLE] = TRUE;
		dev->m_rs[D3DRS_FOGVERTEXMODE] = D3DFOG_LINEAR;
		D3DCOLOR fogc = rand() & 0xFFFFFF;
		dev->m_rs[D3DRS_FOGCOLOR] = fogc;
		FsKey k;
		const bool has[4] = { true, true, false, false };
		FillFsKey(dev, k, has);
		std::string src = FixedFunctionFragmentProgram(k);
		Vec4 diff = Rgba(), spec = Rgba(), tex[4] = { Rgba(), Rgba(), Rgba(), Rgba() };
		float consts[2][4];
		ColorToFloats(0xFFFFFFFF, consts[0]);
		ColorToFloats(fogc, consts[1]);
		float fog = Rnd(0, 1);
		Vec4 r = RunFragment(src, diff, spec, tex, consts, 2, fog);
		for (int c = 0; c < 3; ++c)
		{
			float s0 = Sat(tex[0][c] * diff[c]);
			float s1 = Sat(tex[1][c] * tex[1][3] + s0 * (1 - tex[1][3]));
			float withSpec = Sat(s1 + spec[c]);
			float want = fog * withSpec + (1 - fog) * consts[1][c];
			Expect("two stages + specular + fog", r[c], want);
		}
		Expect("two stages alpha", r[3], tex[0][3]);
	}
}

static void TestPixelShaders()
{
	// The game's fixed ps_1_1 shaders (Gfx/DX9/NX/render.cpp), with hand-written references.
	for (int iter = 0; iter < 30; ++iter)
	{
		Vec4 v0 = Rgba(), v1 = Rgba(), t[4] = { Rgba(), Rgba(), Rgba(), Rgba() };
		float c[9][4];
		for (int i = 0; i < 9; ++i) for (int j = 0; j < 4; ++j) c[i][j] = Rnd(0, 1);

		// aPs11TexT0MulR0: r0.rgb = 4 * v0 * sat(t0 * c0); r0.a = 2 * v0.a * t0.a
		std::string s = PixelShaderProgram("ps.1.1\r\n tex t0 \r\n mul r0.rgb, t0, c0 \r\n + mul_x2 r0.a, v0.a, t0.a \r\n mul_x4\tr0.rgb, v0, r0 \r\n", false);
		Vec4 r = RunFragment(s, v0, v1, t, c, 9);
		for (int k = 0; k < 3; ++k) Expect("TexT0MulR0 rgb", r[k], Sat(4 * v0[k] * Sat(t[0][k] * c[0][k])));
		Expect("TexT0MulR0 a", r[3], Sat(2 * v0[3] * t[0][3]));

		// aPs11MulX2R0Rgb: r0.rgb = 2 * v0 * c0; r0.a = v0.a
		s = PixelShaderProgram("ps.1.1\r\n mul_x2\tr0.rgb, v0, c0\r\n + mov r0.a, v0.a\r\n", false);
		r = RunFragment(s, v0, v1, t, c, 9);
		for (int k = 0; k < 3; ++k) Expect("MulX2R0Rgb rgb", r[k], Sat(2 * v0[k] * c[0][k]));
		Expect("MulX2R0Rgb a", r[3], v0[3]);

		// aPs11TexT0MulX2_0: r0 = 2 * c4.a * t0.a (all four)
		s = PixelShaderProgram("ps.1.1\r\n tex t0\r\n mul_x2 r0,c4.a,t0.a\r\n", false);
		r = RunFragment(s, v0, v1, t, c, 9);
		for (int k = 0; k < 4; ++k) Expect("TexT0MulX2_0", r[k], Sat(2 * c[4][3] * t[0][3]));

		// A two-pass material: base * vertex colour, detail blended by its alpha, then x2 by v0.
		s = PixelShaderProgram("ps.1.1\ntex t0\ntex t1\nmul_x2 r0.a, v0.a, t0.a\n+mov r0.rgb, t0\nmov r1.rgb, t1\n+mul_x2_sat r1.a, v0.a, t1.a\nlrp r0.rgb,r1.a,r1,r0\nmul_x2 r0.rgb, r0, v0\n", true);
		float fog = Rnd(0, 1);
		r = RunFragment(s, v0, v1, t, c, 9, fog);
		float r1a = Sat(2 * v0[3] * t[1][3]);
		for (int k = 0; k < 3; ++k)
		{
			float base = t[0][k], blended = r1a * t[1][k] + (1 - r1a) * base, out = Sat(2 * blended * v0[k]);
			Expect("material 2-pass rgb (+fog)", r[k], fog * out + (1 - fog) * c[8][k]);
		}
		Expect("material 2-pass a", r[3], Sat(2 * v0[3] * t[0][3]));
	}
}

int main()
{
	srand(1234);
	TestTransform();
	TestPretransformed();
	TestLighting();
	TestTexcoordsAndFog();
	TestCombiners();
	TestPixelShaders();
	printf("%d checks, %d failed\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
