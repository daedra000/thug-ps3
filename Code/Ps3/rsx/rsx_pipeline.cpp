// THUG PS3: D3D9 device state -> shader keys and constants (see rsx_pipeline.h).

#include "rsx_pipeline.h"

#include <string.h>
#include <math.h>

namespace RsxPipeline
{

float AsFloat(DWORD d) { float f; memcpy(&f, &d, 4); return f; }

void ColorToFloats(D3DCOLOR c, float* out)
{
	out[0] = ((c >> 16) & 0xFF) / 255.0f;
	out[1] = ((c >> 8) & 0xFF) / 255.0f;
	out[2] = (c & 0xFF) / 255.0f;
	out[3] = ((c >> 24) & 0xFF) / 255.0f;
}

static void Mul(D3DMATRIX& o, const D3DMATRIX& a, const D3DMATRIX& b)
{
	D3DMATRIX r;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
	o = r;
}

// D3D row-vector matrix -> four constant rows that DP4 with a column vector.
static void Columns(float (*c)[4], const D3DMATRIX& m, int count)
{
	for (int i = 0; i < count; ++i)
		for (int k = 0; k < 4; ++k)
			c[i][k] = m.m[k][i];
}



FvfLayout Layout(DWORD fvf)
{
	FvfLayout l;
	memset(&l, 0, sizeof(l));
	l.normal = l.diffuse = l.specular = -1;
	uint32_t at = 0;
	DWORD pos = fvf & D3DFVF_POSITION_MASK;
	l.rhw = pos == D3DFVF_XYZRHW;
	l.pos = 0;
	l.posSize = l.rhw ? 4 : 3;
	at = l.rhw ? 16 : 12;
	if (pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5) at += 4 * ((pos - D3DFVF_XYZB1) / 2 + 1);	// blend weights (not used)
	if (fvf & D3DFVF_NORMAL) { l.normal = at; at += 12; }
	if (fvf & D3DFVF_PSIZE) at += 4;
	if (fvf & D3DFVF_DIFFUSE) { l.diffuse = at; at += 4; }
	if (fvf & D3DFVF_SPECULAR) { l.specular = at; at += 4; }
	l.numTex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (l.numTex > 8) l.numTex = 8;
	for (int i = 0; i < l.numTex; ++i)
	{
		static const int sizes[4] = { 2, 3, 4, 1 };
		l.texSize[i] = sizes[(fvf >> (16 + i * 2)) & 3];
		l.tex[i] = at;
		at += 4 * l.texSize[i];
	}
	l.stride = at;
	return l;
}

void FillVsKey(const IDirect3DDevice9* dev, const FvfLayout& l, int numTexOut, VsKey& k)
{
	memset(&k, 0, sizeof(k));
	const DWORD* rs = dev->m_rs;
	k.rhw = l.rhw;
	k.normal = l.normal >= 0;
	k.diffuse = l.diffuse >= 0;
	k.specular = l.specular >= 0;
	for (int i = 0; i < l.numTex; ++i) k.texSize[i] = l.texSize[i];
	k.lighting = rs[D3DRS_LIGHTING] && k.normal && !k.rhw;
	if (k.lighting)
	{
		int n = 0;
		for (int i = 0; i < 8 && n < 4; ++i)
		{
			if (!dev->m_lightEnabled[i]) continue;
			k.light[n++] = dev->m_lights[i].Type == D3DLIGHT_DIRECTIONAL ? kLightDirectional : kLightPoint;
		}
		const bool cv = rs[D3DRS_COLORVERTEX] != 0;
		k.diffuseSrc = cv ? rs[D3DRS_DIFFUSEMATERIALSOURCE] : kSrcMaterial;
		k.ambientSrc = cv ? rs[D3DRS_AMBIENTMATERIALSOURCE] : kSrcMaterial;
		k.specularSrc = cv ? rs[D3DRS_SPECULARMATERIALSOURCE] : kSrcMaterial;
		k.emissiveSrc = cv ? rs[D3DRS_EMISSIVEMATERIALSOURCE] : kSrcMaterial;
		k.specularEnable = rs[D3DRS_SPECULARENABLE] != 0;
		k.localViewer = rs[D3DRS_LOCALVIEWER] != 0;
	}
	k.fog = rs[D3DRS_FOGENABLE] && (rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE || rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE);
	k.numTexOut = numTexOut > 4 ? 4 : numTexOut;
	for (int s = 0; s < k.numTexOut; ++s)
	{
		DWORD tci = dev->m_tss[s][D3DTSS_TEXCOORDINDEX], ttff = dev->m_tss[s][D3DTSS_TEXTURETRANSFORMFLAGS];
		k.tex[s].set = tci & 7;
		switch (tci & 0xFFFF0000)
		{
		case D3DTSS_TCI_CAMERASPACENORMAL: k.tex[s].gen = kGenNormal; break;
		case D3DTSS_TCI_CAMERASPACEPOSITION: k.tex[s].gen = kGenPosition; break;
		case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: case D3DTSS_TCI_SPHEREMAP: k.tex[s].gen = kGenReflection; break;
		default: k.tex[s].gen = kGenPassthru; break;
		}
		k.tex[s].count = ttff & 0xFF;
		if (k.tex[s].count > 4) k.tex[s].count = 0;
		k.tex[s].projected = (ttff & D3DTTFF_PROJECTED) != 0;
	}
}

void FillVsConstants(const IDirect3DDevice9* dev, float (*c)[4])
{
	memset(c, 0, sizeof(float) * 4 * kVcCount);
	const D3DMATRIX& world = dev->m_transforms[256];
	const D3DMATRIX& view = dev->m_transforms[D3DTS_VIEW];
	const D3DMATRIX& proj = dev->m_transforms[D3DTS_PROJECTION];
	D3DMATRIX wv, wvp;
	Mul(wv, world, view);
	Mul(wvp, wv, proj);
	Columns(c + kVcClip, wvp, 4);
	Columns(c + kVcWorldView, wv, 4);

	// Normals: rows of the inverse of WV's 3x3.
	float a = wv._11, b = wv._12, cc = wv._13, d = wv._21, e = wv._22, f = wv._23, g = wv._31, h = wv._32, i = wv._33;
	float det = a * (e * i - f * h) - b * (d * i - f * g) + cc * (d * h - e * g);
	float inv = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
	float n[3][3] = {
		{ (e * i - f * h) * inv, (cc * h - b * i) * inv, (b * f - cc * e) * inv },
		{ (f * g - d * i) * inv, (a * i - cc * g) * inv, (cc * d - a * f) * inv },
		{ (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv } };
	for (int r = 0; r < 3; ++r) { c[kVcNormal + r][0] = n[r][0]; c[kVcNormal + r][1] = n[r][1]; c[kVcNormal + r][2] = n[r][2]; }

	const DWORD* rs = dev->m_rs;
	ColorToFloats(rs[D3DRS_AMBIENT], c[kVcAmbient]);
	const D3DMATERIAL9& m = dev->m_material;
	memcpy(c[kVcMatDiffuse], &m.Diffuse, 16);
	memcpy(c[kVcMatAmbient], &m.Ambient, 16);
	memcpy(c[kVcMatSpecular], &m.Specular, 16);
	memcpy(c[kVcMatEmissive], &m.Emissive, 16);
	float fs = AsFloat(rs[D3DRS_FOGSTART]), fe = AsFloat(rs[D3DRS_FOGEND]);
	c[kVcParams][0] = m.Power;
	c[kVcParams][1] = fe;
	c[kVcParams][2] = fe != fs ? 1.0f / (fe - fs) : 0.0f;
	c[kVcConsts][0] = 0.0f; c[kVcConsts][1] = 1.0f; c[kVcConsts][2] = 0.5f; c[kVcConsts][3] = 2.0f;
	const D3DVIEWPORT9& vp = dev->m_viewport;
	float W = vp.Width ? (float)vp.Width : 1.0f, H = vp.Height ? (float)vp.Height : 1.0f;
	c[kVcScreen][0] = 2.0f / W;
	c[kVcScreen][1] = -2.0f / H;
	c[kVcScreen][2] = -1.0f - 2.0f * vp.X / W;
	c[kVcScreen][3] = 1.0f + 2.0f * vp.Y / H;
	c[kVcPoint][0] = AsFloat(rs[D3DRS_POINTSIZE]);

	int n2 = 0;
	for (int l = 0; l < 8 && n2 < 4; ++l)
	{
		if (!dev->m_lightEnabled[l]) continue;
		const D3DLIGHT9& L = dev->m_lights[l];
		float* q = c[kVcLights + n2 * 5];
		if (L.Type == D3DLIGHT_DIRECTIONAL)
		{
			float dx = L.Direction.x * view._11 + L.Direction.y * view._21 + L.Direction.z * view._31;
			float dy = L.Direction.x * view._12 + L.Direction.y * view._22 + L.Direction.z * view._32;
			float dz = L.Direction.x * view._13 + L.Direction.y * view._23 + L.Direction.z * view._33;
			float len = sqrtf(dx * dx + dy * dy + dz * dz);
			float s = len > 1e-20f ? -1.0f / len : 0.0f;
			q[0] = dx * s; q[1] = dy * s; q[2] = dz * s; q[3] = 0.0f;
		}
		else
		{
			const D3DVECTOR& p = L.Position;
			q[0] = p.x * view._11 + p.y * view._21 + p.z * view._31 + view._41;
			q[1] = p.x * view._12 + p.y * view._22 + p.z * view._32 + view._42;
			q[2] = p.x * view._13 + p.y * view._23 + p.z * view._33 + view._43;
			q[3] = 1.0f;
		}
		memcpy(q + 4, &L.Diffuse, 16);
		memcpy(q + 8, &L.Specular, 16);
		memcpy(q + 12, &L.Ambient, 16);
		q[16] = L.Attenuation0; q[17] = L.Attenuation1; q[18] = L.Attenuation2; q[19] = L.Range > 0 ? L.Range : 1e30f;
		if (q[16] == 0 && q[17] == 0 && q[18] == 0) q[16] = 1.0f;
		++n2;
	}
	for (int s = 0; s < 4; ++s)
		Columns(c + kVcTexMatrix + s * 4, dev->m_transforms[D3DTS_TEXTURE0 + s], 4);
}

// The fixed-function texture stages as a fragment-program key.
void FillFsKey(const IDirect3DDevice9* dev, FsKey& k, const bool* hasTexture)
{
	memset(&k, 0, sizeof(k));
	int n = 0;
	for (; n < 4; ++n)
	{
		const DWORD* t = dev->m_tss[n];
		if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
		TssStage& st = k.stage[n];
		st.colorOp = (uint8_t)t[D3DTSS_COLOROP];
		st.colorArg0 = (uint8_t)t[D3DTSS_COLORARG0];
		st.colorArg1 = (uint8_t)t[D3DTSS_COLORARG1];
		st.colorArg2 = (uint8_t)t[D3DTSS_COLORARG2];
		st.alphaOp = (uint8_t)t[D3DTSS_ALPHAOP];
		st.alphaArg0 = (uint8_t)t[D3DTSS_ALPHAARG0];
		st.alphaArg1 = (uint8_t)t[D3DTSS_ALPHAARG1];
		st.alphaArg2 = (uint8_t)t[D3DTSS_ALPHAARG2];
		if (st.alphaOp == D3DTOP_DISABLE) { st.alphaOp = D3DTOP_SELECTARG1; st.alphaArg1 = D3DTA_CURRENT; }
		st.resultTemp = t[D3DTSS_RESULTARG] == D3DTA_TEMP;
		st.projected = (t[D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) != 0;
		st.hasTexture = hasTexture[n];
	}
	k.numStages = n;
	k.specularAdd = dev->m_rs[D3DRS_SPECULARENABLE] != 0;
	k.fog = dev->m_rs[D3DRS_FOGENABLE] && (dev->m_rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE || dev->m_rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE);
}

// The source text D3DXAssembleShader kept in the shader's comment token (d3dx_ps3.cpp).
const char* PixelShaderSource(const IDirect3DPixelShader9* ps)
{
	if (!ps || ps->m_numTokens < 4) return NULL;
	const DWORD* t = ps->m_tokens;
	if ((t[1] & 0xFFFF) != 0xFFFE || memcmp(&t[2], "PS3S", 4)) return NULL;
	return (const char*)&t[3];
}


bool FogEnabled(const IDirect3DDevice9* dev)
{
	const DWORD* rs = dev->m_rs;
	return rs[D3DRS_FOGENABLE] && (rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE || rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE);
}

void ViewportTransform(const D3DVIEWPORT9& vp, float* scale, float* offset)
{
	scale[0] = vp.Width * 0.5f;
	scale[1] = vp.Height * -0.5f;
	scale[2] = (vp.MaxZ - vp.MinZ) * 0.5f;
	scale[3] = 0.0f;
	offset[0] = vp.X + vp.Width * 0.5f + 0.5f;
	offset[1] = vp.Y + vp.Height * 0.5f + 0.5f;
	offset[2] = (vp.MaxZ + vp.MinZ) * 0.5f;
	offset[3] = 0.0f;
}

} // namespace RsxPipeline
