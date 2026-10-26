// THUG PS3: NV40 programs for the D3D9 pipeline the game uses (see rsx_shadergen.h).

#include "rsx_shadergen.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <vector>

namespace RsxShaderGen
{

// ---------------------------------------------------------------------------
// Assembly writer: one instruction per line, with the NV40 operand rules applied.
// ---------------------------------------------------------------------------
class Writer
{
public:
	explicit Writer(bool fragment) : m_fragment(fragment), m_scratch(0) {}

	std::string text;

	void Line(const std::string& s) { text += s; text += "\n"; }

	// Emits `op dst, srcs...`. If the sources read more than one constant register or
	// more than one input attribute, the extra ones are first copied to scratch temps.
	void Op(const std::string& op, const std::string& dst, const std::string& a, const std::string& b = "", const std::string& c = "")
	{
		std::string src[3] = { a, b, c };
		int n = c.size() ? 3 : b.size() ? 2 : 1;
		std::string seenConst, seenInput;
		for (int i = 0; i < n; ++i)
		{
			std::string base = Base(src[i]);
			bool isConst = base.compare(0, 2, "c[") == 0;
			bool isInput = base.compare(0, 2, "v[") == 0 || base.compare(0, 9, "fragment.") == 0;
			if (!isConst && !isInput) continue;
			std::string& seen = isConst ? seenConst : seenInput;
			if (seen.empty()) { seen = base; continue; }
			if (seen == base) continue;
			// Copy the whole register, then read it with the same modifiers and swizzle.
			std::string tmp = Scratch();
			Line("MOV " + tmp + ", " + base + ";");
			src[i] = Rebase(src[i], base, tmp);
		}
		std::string line = op + " " + dst + ", " + src[0];
		for (int i = 1; i < n; ++i) line += ", " + src[i];
		Line(line + ";");
	}

private:
	bool m_fragment;
	int m_scratch;

	std::string Scratch()
	{
		char buf[8];
		snprintf(buf, sizeof(buf), "R%d", (m_fragment ? 10 : 28) + (m_scratch++ & 3));
		return buf;
	}

	// The register an operand reads: no sign, |abs| or swizzle.
	static std::string Base(const std::string& s)
	{
		size_t i = 0;
		while (i < s.size() && (s[i] == '-' || s[i] == '|')) ++i;
		std::string r = s.substr(i);
		while (!r.empty() && r[r.size() - 1] == '|') r.erase(r.size() - 1);
		size_t dot = r.find_last_of('.');
		if (dot != std::string::npos && dot + 1 < r.size())
		{
			bool swz = true;
			for (size_t k = dot + 1; k < r.size(); ++k)
				if (!strchr("xyzw", r[k])) swz = false;
			if (swz) r = r.substr(0, dot);
		}
		return r;
	}

	static std::string Rebase(const std::string& s, const std::string& base, const std::string& to)
	{
		size_t at = s.find(base);
		return s.substr(0, at) + to + s.substr(at + base.size());
	}
};

static std::string C(int n, const char* swz = "")
{
	char buf[24];
	snprintf(buf, sizeof(buf), "c[%d]%s%s", n, *swz ? "." : "", swz);
	return buf;
}

static std::string V(int n, const char* swz = "")
{
	char buf[24];
	snprintf(buf, sizeof(buf), "v[%d]%s%s", n, *swz ? "." : "", swz);
	return buf;
}

static std::string R(int n, const char* swz = "")
{
	char buf[24];
	snprintf(buf, sizeof(buf), "R%d%s%s", n, *swz ? "." : "", swz);
	return buf;
}

// ---------------------------------------------------------------------------
// Vertex programs
// ---------------------------------------------------------------------------
// Temps: R0 clip position, R1 eye position, R2 eye normal, R3-R6 diffuse/ambient/specular/
// emissive sources, R7 lit colour, R8 lit specular, R9-R15 lighting work, R11 view vector,
// R16 fog, R20-R21 texture coordinates, R28-R31 operand scratch.
static std::string MaterialColour(const VsKey& k, int src, int materialConst)
{
	if (src == kSrcColor1 && k.diffuse) return V(kAttrDiffuse, "yzwx");
	if (src == kSrcColor2 && k.specular) return V(kAttrSpecular, "yzwx");
	return C(materialConst);
}

std::string VertexProgram(const VsKey& k)
{
	Writer w(false);
	const std::string one = C(kVcConsts, "y"), zero = C(kVcConsts, "x");
	w.Line("!!VP2.0");

	const bool lit = k.lighting && k.normal && !k.rhw;
	bool needEye = false, needNormal = lit;
	for (int s = 0; s < k.numTexOut; ++s)
	{
		if (k.tex[s].gen == kGenNormal) needNormal = true;
		if (k.tex[s].gen == kGenPosition) needEye = true;
		if (k.tex[s].gen == kGenReflection) needEye = needNormal = true;
	}
	if (k.rhw) needEye = needNormal = false;
	if (lit || (k.fog && !k.rhw)) needEye = true;
	if (!k.normal) needNormal = false;

	// Position
	if (k.rhw)
	{
		// Screen x, y -> NDC, z (0..1) -> -1..1, then back to clip space with w = 1/rhw so
		// texturing stays perspective-correct.
		w.Op("MAD", R(0, "x"), V(kAttrPos, "x"), C(kVcScreen, "x"), C(kVcScreen, "z"));
		w.Op("MAD", R(0, "y"), V(kAttrPos, "y"), C(kVcScreen, "y"), C(kVcScreen, "w"));
		w.Op("MAD", R(0, "z"), V(kAttrPos, "z"), C(kVcConsts, "w"), "-" + one);
		// THUG's text sets rhw to 0, which D3D9 drew as if it were 1: 1/0 here made every
		// glyph's position infinite, so no text showed. rhw <= 0 counts as 1.
		w.Op("SGE", R(0, "w"), zero, V(kAttrPos, "w"));
		w.Op("ADD", R(0, "w"), R(0, "w"), V(kAttrPos, "w"));
		w.Op("RCP", R(0, "w"), R(0, "w"));
		w.Op("MUL", R(0, "xyz"), R(0), R(0, "w"));
		w.Op("MOV", "result.position", R(0));
	}
	else
	{
		static const char* axis[4] = { "x", "y", "z", "w" };
		for (int i = 0; i < 4; ++i)
			w.Op("DP4", R(0, axis[i]), C(kVcClip + i), V(kAttrPos));
		// D3D's depth runs 0..w; the RSX's (with the viewport set up GL-style) -w..w.
		w.Op("MAD", R(0, "z"), R(0, "z"), C(kVcConsts, "w"), "-" + R(0, "w"));
		w.Op("MOV", "result.position", R(0));
	}

	if (needEye)
	{
		w.Op("DP4", R(1, "x"), C(kVcWorldView + 0), V(kAttrPos));
		w.Op("DP4", R(1, "y"), C(kVcWorldView + 1), V(kAttrPos));
		w.Op("DP4", R(1, "z"), C(kVcWorldView + 2), V(kAttrPos));
		w.Op("MOV", R(1, "w"), one);
	}
	if (needNormal)
	{
		w.Op("DP3", R(2, "x"), C(kVcNormal + 0), V(kAttrNormal));
		w.Op("DP3", R(2, "y"), C(kVcNormal + 1), V(kAttrNormal));
		w.Op("DP3", R(2, "z"), C(kVcNormal + 2), V(kAttrNormal));
		w.Op("DP3", R(2, "w"), R(2), R(2));
		w.Op("RSQ", R(2, "w"), R(2, "w"));
		w.Op("MUL", R(2, "xyz"), R(2), R(2, "w"));
		w.Op("MOV", R(2, "w"), one);
	}

	// Colours
	if (lit)
	{
		w.Op("MOV", R(3), MaterialColour(k, k.diffuseSrc, kVcMatDiffuse));
		w.Op("MOV", R(4), MaterialColour(k, k.ambientSrc, kVcMatAmbient));
		w.Op("MOV", R(5), MaterialColour(k, k.specularSrc, kVcMatSpecular));
		w.Op("MOV", R(6), MaterialColour(k, k.emissiveSrc, kVcMatEmissive));
		// emissive + global ambient * material ambient
		w.Op("MUL", R(7), R(4), C(kVcAmbient));
		w.Op("ADD", R(7, "xyz"), R(7), R(6));
		w.Op("MOV", R(8), zero);
		if (k.specularEnable)
		{
			if (k.localViewer)
			{
				w.Op("MOV", R(11, "xyz"), "-" + R(1));
				w.Op("DP3", R(11, "w"), R(11), R(11));
				w.Op("RSQ", R(11, "w"), R(11, "w"));
				w.Op("MUL", R(11, "xyz"), R(11), R(11, "w"));
			}
			else
				w.Op("MOV", R(11), "-" + C(kVcConsts, "xxyx"));	// (0, 0, -1): towards a viewer at infinity (D3D's eye space looks down +z)
		}
		for (int l = 0; l < 4 && k.light[l] != kLightNone; ++l)
		{
			const int b = kVcLights + l * 5;
			std::string L = C(b), atten;
			if (k.light[l] == kLightPoint)
			{
				// L = normalize(light - P); attenuation 1/(a0 + a1 d + a2 d^2), 0 beyond range
				w.Op("ADD", R(13, "xyz"), C(b), "-" + R(1));
				w.Op("DP3", R(13, "w"), R(13), R(13));
				w.Op("RSQ", R(14, "x"), R(13, "w"));
				w.Op("MUL", R(13, "xyz"), R(13), R(14, "x"));
				w.Op("MUL", R(14, "y"), R(13, "w"), R(14, "x"));
				w.Op("MUL", R(15, "x"), R(13, "w"), C(b + 4, "z"));
				w.Op("MAD", R(15, "x"), R(14, "y"), C(b + 4, "y"), R(15, "x"));
				w.Op("ADD", R(15, "x"), R(15, "x"), C(b + 4, "x"));
				w.Op("RCP", R(15, "x"), R(15, "x"));
				w.Op("SGE", R(15, "y"), C(b + 4, "w"), R(14, "y"));
				w.Op("MUL", R(15, "x"), R(15, "x"), R(15, "y"));
				L = R(13);
				atten = R(15, "x");
			}
			w.Op("DP3", R(9, "x"), R(2), L);
			w.Op("MAX", R(9, "x"), R(9, "x"), zero);
			if (!atten.empty())
				w.Op("MUL", R(9, "x"), R(9, "x"), atten);
			// ambient
			w.Op("MUL", R(10), R(4), C(b + 3));
			if (!atten.empty())
				w.Op("MAD", R(7, "xyz"), R(10), atten, R(7));
			else
				w.Op("ADD", R(7, "xyz"), R(7), R(10));
			// diffuse
			w.Op("MUL", R(10), R(3), C(b + 1));
			w.Op("MAD", R(7, "xyz"), R(10), R(9, "x"), R(7));
			if (k.specularEnable)
			{
				w.Op("ADD", R(12, "xyz"), R(11), L);
				w.Op("DP3", R(12, "w"), R(12), R(12));
				w.Op("RSQ", R(12, "w"), R(12, "w"));
				w.Op("MUL", R(12, "xyz"), R(12), R(12, "w"));
				w.Op("DP3", R(12, "x"), R(2), R(12));
				w.Op("MAX", R(12, "x"), R(12, "x"), zero);
				w.Op("POW", R(12, "x"), R(12, "x"), C(kVcParams, "x"));
				w.Op("SLT", R(12, "y"), zero, R(9, "x"));	// only where the light reaches
				w.Op("MUL", R(12, "x"), R(12, "x"), R(12, "y"));
				if (!atten.empty())
					w.Op("MUL", R(12, "x"), R(12, "x"), atten);
				w.Op("MUL", R(10), R(5), C(b + 2));
				w.Op("MAD", R(8, "xyz"), R(10), R(12, "x"), R(8));
			}
		}
		w.Op("MAX", R(7, "xyz"), R(7), zero);
		w.Op("MIN", R(7, "xyz"), R(7), one);
		w.Op("MOV", R(7, "w"), R(3, "w"));
		w.Op("MOV", "result.color", R(7));
		w.Op("MIN", R(8, "xyz"), R(8), one);
		w.Op("MOV", "result.color.secondary", R(8));
	}
	else
	{
		w.Op("MOV", "result.color", k.diffuse ? V(kAttrDiffuse, "yzwx") : C(kVcConsts, "yyyy"));
		w.Op("MOV", "result.color.secondary", k.specular ? V(kAttrSpecular, "yzwx") : C(kVcConsts, "xxxx"));
	}

	// Fog factor: 1 = no fog. Linear in eye depth.
	if (k.fog)
	{
		if (k.rhw)
			w.Op("MOV", "result.fogcoord.x", k.specular ? V(kAttrSpecular, "x") : one);	// D3D: vertex fog from specular alpha
		else
		{
			w.Op("ADD", R(16, "x"), "-" + R(1, "z"), C(kVcParams, "y"));
			w.Op("MUL", R(16, "x"), R(16, "x"), C(kVcParams, "z"));
			w.Op("MAX", R(16, "x"), R(16, "x"), zero);
			w.Op("MIN", R(16, "x"), R(16, "x"), one);
			w.Op("MOV", "result.fogcoord.x", R(16, "x"));
		}
	}

	// Texture coordinates, one set per stage
	for (int s = 0; s < k.numTexOut && s < 4; ++s)
	{
		const VsKey::Tex& t = k.tex[s];
		char out[32];
		snprintf(out, sizeof(out), "result.texcoord[%d]", s);
		int gen = k.rhw ? kGenPassthru : t.gen;
		if (gen != kGenPassthru && !needNormal && gen != kGenPosition) gen = kGenPassthru;

		// The source as a 4D vector in R20: (u, v, 1, 0) for 2D sets, (x, y, z, 1) for generated ones.
		if (gen == kGenPassthru)
		{
			int set = t.set < 8 ? t.set : 0;
			int size = k.texSize[set];
			if (size == 0)
				w.Op("MOV", R(20), C(kVcConsts, "xxyx"));
			else if (size == 1)
			{
				w.Op("MOV", R(20), C(kVcConsts, "xxyx"));
				w.Op("MOV", R(20, "x"), V(kAttrTex0 + set, "x"));
			}
			else if (size == 2)
			{
				w.Op("MOV", R(20), C(kVcConsts, "xxyx"));
				w.Op("MOV", R(20, "xy"), V(kAttrTex0 + set));
			}
			else if (size == 3)
			{
				w.Op("MOV", R(20), C(kVcConsts, "yyyy"));
				w.Op("MOV", R(20, "xyz"), V(kAttrTex0 + set));
			}
			else
				w.Op("MOV", R(20), V(kAttrTex0 + set));
		}
		else if (gen == kGenNormal)
			w.Op("MOV", R(20), R(2));
		else if (gen == kGenPosition)
			w.Op("MOV", R(20), R(1));
		else
		{
			// reflection of the eye-to-vertex direction: I - 2 (N.I) N
			w.Op("DP3", R(21, "w"), R(1), R(1));
			w.Op("RSQ", R(21, "w"), R(21, "w"));
			w.Op("MUL", R(21, "xyz"), R(1), R(21, "w"));
			w.Op("DP3", R(21, "w"), R(2), R(21));
			w.Op("MUL", R(21, "w"), R(21, "w"), C(kVcConsts, "w"));
			w.Op("MUL", R(20, "xyz"), R(2), R(21, "w"));
			w.Op("ADD", R(20, "xyz"), R(21), "-" + R(20));
			w.Op("MOV", R(20, "w"), one);
		}

		if (t.count >= 1 && t.count <= 4)
		{
			static const char* axis[4] = { "x", "y", "z", "w" };
			for (int i = 0; i < t.count; ++i)
				w.Op("DP4", R(21, axis[i]), C(kVcTexMatrix + s * 4 + i), R(20));
			if (t.count < 4)
				w.Op("MOV", R(21, t.count == 1 ? "yzw" : t.count == 2 ? "zw" : "w"), C(kVcConsts, "xxyy"));
			w.Op("MOV", out, R(21));
		}
		else
			w.Op("MOV", out, R(20));
	}

	if (k.pointSize)
		w.Op("MOV", "result.pointsize.x", C(kVcPoint, "x"));

	w.Line("END");
	return w.text;
}

// ---------------------------------------------------------------------------
// Fragment programs
// ---------------------------------------------------------------------------
// Temps: R0 current, R1 D3D's temp register, R2/R3 stage work, R4-R7 the stages'
// texture samples (t0-t3), R8/R9 argument work, R10-R13 operand scratch. A stage op's
// own scratch must be R10/R11, never R2/R3: the colour result waits in R2 while the
// alpha half runs (scratch in R2 turned every MODULATE2X stage's colour into its alpha).
// Internal constant c[31] = (1, 0, 0.5, 2).
static const int kFcInternal = 31;
static const char* kFpHeaderConsts = "#const c[31] = 1 0 0.5 2";

class FixedFunction
{
public:
	FixedFunction(Writer& w, const FsKey& k) : m_w(w), m_k(k), m_argTemp(0) {}

	std::string Arg(int stage, uint8_t arg, bool alpha)
	{
		std::string reg;
		switch (arg & D3D::TA_SELECTMASK)
		{
		case D3D::TA_DIFFUSE:  reg = "fragment.color.primary"; break;
		case D3D::TA_SPECULAR: reg = "fragment.color.secondary"; break;
		case D3D::TA_CURRENT:  reg = stage == 0 ? "fragment.color.primary" : "R0"; break;
		case D3D::TA_TEXTURE:  reg = m_k.stage[stage].hasTexture ? R(4 + stage) : C(kFcInternal, "xxxx"); break;
		case D3D::TA_TFACTOR:  reg = C(kFcTextureFactor); break;
		case D3D::TA_TEMP:     reg = "R1"; break;
		default:               reg = C(kFcInternal, "xxxx"); break;
		}
		std::string a = reg;
		if ((arg & D3D::TA_ALPHAREPLICATE) || alpha)
			a = Swizzle(reg, "wwww");
		if (arg & D3D::TA_COMPLEMENT)
		{
			std::string t = R(8 + (m_argTemp++ & 1));
			m_w.Op("ADD", t, "-" + a, C(kFcInternal, "xxxx"));
			a = t;
		}
		return a;
	}

	// Writes one stage's colour (mask "xyz") or alpha (mask "w") into dst.
	void Stage(int s, bool alpha, const std::string& dst)
	{
		const TssStage& st = m_k.stage[s];
		const uint8_t op = alpha ? st.alphaOp : st.colorOp;
		const uint8_t a0 = alpha ? st.alphaArg0 : st.colorArg0;
		const uint8_t a1 = alpha ? st.alphaArg1 : st.colorArg1;
		const uint8_t a2 = alpha ? st.alphaArg2 : st.colorArg2;
		const std::string one = C(kFcInternal, "xxxx"), half = C(kFcInternal, "zzzz"), two = C(kFcInternal, "wwww");
		const std::string d = dst + (alpha ? ".w" : ".xyz");
		std::string A1, A2, A0;
		A1 = Arg(s, a1, alpha);
		if (op != D3D::TOP_SELECTARG1)
			A2 = Arg(s, a2, alpha);

		switch (op)
		{
		case D3D::TOP_SELECTARG1: m_w.Op("MOV_SAT", d, A1); break;
		case D3D::TOP_SELECTARG2: m_w.Op("MOV_SAT", d, A2); break;
		case D3D::TOP_MODULATE: case D3D::TOP_PREMODULATE:
			m_w.Op("MUL_SAT", d, A1, A2); break;
		case D3D::TOP_MODULATE2X:
			m_w.Op("MUL", "R10", A1, A2);
			m_w.Op("ADD_SAT", d, "R10", "R10"); break;
		case D3D::TOP_MODULATE4X:
			m_w.Op("MUL", "R10", A1, A2);
			m_w.Op("MUL_SAT", d, "R10", C(kFcInternal, "wwww"));
			m_w.Op("ADD_SAT", d, dst, dst); break;
		case D3D::TOP_ADD: m_w.Op("ADD_SAT", d, A1, A2); break;
		case D3D::TOP_ADDSIGNED:
			m_w.Op("ADD", "R10", A1, A2);
			m_w.Op("ADD_SAT", d, "R10", "-" + half); break;
		case D3D::TOP_ADDSIGNED2X:
			m_w.Op("ADD", "R10", A1, A2);
			m_w.Op("ADD", "R10", "R10", "-" + half);
			m_w.Op("ADD_SAT", d, "R10", "R10"); break;
		case D3D::TOP_SUBTRACT: m_w.Op("ADD_SAT", d, A1, "-" + A2); break;
		case D3D::TOP_ADDSMOOTH:
			m_w.Op("MAD", "R10", "-" + A1, A2, A1);
			m_w.Op("ADD_SAT", d, "R10", A2); break;
		case D3D::TOP_BLENDDIFFUSEALPHA:
			m_w.Op("LRP_SAT", d, "fragment.color.primary.wwww", A1, A2); break;
		case D3D::TOP_BLENDTEXTUREALPHA:
			m_w.Op("LRP_SAT", d, Swizzle(Arg(s, D3D::TA_TEXTURE, false), "wwww"), A1, A2); break;
		case D3D::TOP_BLENDFACTORALPHA:
			m_w.Op("LRP_SAT", d, C(kFcTextureFactor, "wwww"), A1, A2); break;
		case D3D::TOP_BLENDCURRENTALPHA:
			m_w.Op("LRP_SAT", d, Swizzle(Arg(s, D3D::TA_CURRENT, false), "wwww"), A1, A2); break;
		case D3D::TOP_BLENDTEXTUREALPHAPM:
			m_w.Op("ADD", "R11", "-" + Swizzle(Arg(s, D3D::TA_TEXTURE, false), "wwww"), one);
			m_w.Op("MAD_SAT", d, A2, "R11", A1); break;
		case D3D::TOP_MODULATEALPHA_ADDCOLOR:
			m_w.Op("MAD_SAT", d, Swizzle(A1, "wwww"), A2, A1); break;
		case D3D::TOP_MODULATECOLOR_ADDALPHA:
			m_w.Op("MAD_SAT", d, A1, A2, Swizzle(A1, "wwww")); break;
		case D3D::TOP_MODULATEINVALPHA_ADDCOLOR:
			m_w.Op("ADD", "R11", "-" + Swizzle(A1, "wwww"), one);
			m_w.Op("MAD_SAT", d, "R11", A2, A1); break;
		case D3D::TOP_MODULATEINVCOLOR_ADDALPHA:
			m_w.Op("ADD", "R11", "-" + A1, one);
			m_w.Op("MAD_SAT", d, "R11", A2, Swizzle(A1, "wwww")); break;
		case D3D::TOP_DOTPRODUCT3:
			m_w.Op("MAD", "R10", A1, two, "-" + one);
			m_w.Op("MAD", "R11", A2, two, "-" + one);
			m_w.Op("DP3_SAT", "R10", "R10", "R11");
			m_w.Op("MOV", d, "R10"); break;
		case D3D::TOP_MULTIPLYADD:
			A0 = Arg(s, a0, alpha);
			m_w.Op("MAD_SAT", d, A1, A2, A0); break;
		case D3D::TOP_LERP:
			A0 = Arg(s, a0, alpha);
			m_w.Op("LRP_SAT", d, A0, A1, A2); break;
		default:	// bump mapping and anything unknown: pass the current value on
			m_w.Op("MOV", d, Arg(s, D3D::TA_CURRENT, alpha)); break;
		}
	}

	static std::string Swizzle(const std::string& reg, const char* swz)
	{
		// reg is a plain register or "R8" style temp; replace any swizzle it has.
		std::string r = reg;
		size_t dot = r.find_last_of('.');
		if (dot != std::string::npos)
		{
			bool isSwz = true;
			for (size_t k = dot + 1; k < r.size(); ++k)
				if (!strchr("xyzw", r[k])) isSwz = false;
			if (isSwz && dot + 1 < r.size()) r = r.substr(0, dot);
		}
		return r + "." + swz;
	}

private:
	Writer& m_w;
	const FsKey& m_k;
	int m_argTemp;
};

static void FpHeader(Writer& w, int numVars)
{
	w.Line("!!ARBfp1.0");
	for (int i = 0; i < numVars; ++i)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "#var float4 c%d : : c[%d] : 1 : 1", i, i);
		w.Line(buf);
	}
	w.Line(kFpHeaderConsts);
}

static void FpFinish(Writer& w, bool fog, int fogConst)
{
	if (fog)
		w.Op("LRP", "R0.xyz", "fragment.fogcoord.xxxx", "R0", C(fogConst));
	w.Op("MOV", "result.color", "R0");
	w.Line("END");
}

std::string FixedFunctionFragmentProgram(const FsKey& k)
{
	Writer w(true);
	FpHeader(w, 2);
	const int n = k.numStages > 4 ? 4 : k.numStages;

	// Sample each stage's texture once.
	for (int s = 0; s < n; ++s)
	{
		const TssStage& st = k.stage[s];
		if (!st.hasTexture) continue;
		char buf[96];
		snprintf(buf, sizeof(buf), "%s R%d, fragment.texcoord[%d], texture[%d], 2D;", st.projected ? "TXP" : "TEX", 4 + s, s, s);
		w.Line(buf);
	}

	FixedFunction ff(w, k);
	if (n == 0)
		w.Op("MOV", "R0", "fragment.color.primary");
	for (int s = 0; s < n; ++s)
	{
		// Both halves read the previous stage's result, so they go to R2 first when the
		// colour would otherwise overwrite something the alpha still needs.
		const std::string dst = k.stage[s].resultTemp ? "R1" : "R0";
		ff.Stage(s, false, "R3");
		w.Op("MOV", "R2.xyz", "R3");
		ff.Stage(s, true, "R3");
		w.Op("MOV", dst + ".xyz", "R2");
		w.Op("MOV", dst + ".w", "R3");
		if (s == 0 && k.stage[0].resultTemp)
			w.Op("MOV", "R0", "fragment.color.primary");
	}

	if (k.specularAdd)
		w.Op("ADD_SAT", "R0.xyz", "R0", "fragment.color.secondary");
	FpFinish(w, k.fog, kFcFogColor);
	return w.text;
}

// ---------------------------------------------------------------------------
// ps_1_x
// ---------------------------------------------------------------------------
namespace
{
	struct PsCtx
	{
		Writer* w;
		bool v1Written;
		int tmp;
		std::string error;
	};

	std::string Lower(std::string s)
	{
		for (size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]);
		return s;
	}

	std::string Trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && isspace((unsigned char)s[a])) ++a;
		while (b > a && isspace((unsigned char)s[b - 1])) --b;
		return s.substr(a, b - a);
	}

	// A ps register name (r0, t1, v0, c3) -> NV40 operand.
	std::string PsReg(PsCtx& c, const std::string& r, bool asDest)
	{
		if (r.size() < 2) { c.error = "bad register " + r; return "R0"; }
		int n = atoi(r.c_str() + 1);
		switch (r[0])
		{
		case 'r': return R(n);
		case 't': return R(4 + (n & 3));
		case 'c': return C(n & 7);
		case 'v':
			if (n == 0) return "fragment.color.primary";
			if (c.v1Written) return "R12";
			return "fragment.color.secondary";
		}
		c.error = "unknown register " + r;
		return "R0";
	}

	std::string Mask(const std::string& m)
	{
		if (m.empty()) return "";
		std::string out;
		for (size_t i = 0; i < m.size(); ++i)
		{
			char ch = m[i];
			if (ch == 'r' || ch == 'x') out += 'x';
			else if (ch == 'g' || ch == 'y') out += 'y';
			else if (ch == 'b' || ch == 'z') out += 'z';
			else if (ch == 'a' || ch == 'w') out += 'w';
		}
		return out.empty() ? "" : "." + out;
	}

	// A ps source operand with its modifiers -> an NV40 operand (may emit instructions).
	std::string PsSrc(PsCtx& c, std::string s)
	{
		s = Trim(s);
		bool neg = false, complement = false;
		if (s.compare(0, 2, "1-") == 0) { complement = true; s = Trim(s.substr(2)); }
		if (!s.empty() && s[0] == '-') { neg = true; s = Trim(s.substr(1)); }
		std::string swz;
		size_t dot = s.find('.');
		if (dot != std::string::npos) { swz = s.substr(dot + 1); s = s.substr(0, dot); }
		bool bias = false, bx2 = false;
		size_t us = s.find('_');
		if (us != std::string::npos)
		{
			std::string mod = s.substr(us + 1);
			s = s.substr(0, us);
			if (mod == "bias") bias = true;
			else if (mod == "bx2") bx2 = true;
		}
		std::string reg = PsReg(c, s, false);
		if (swz == "a" || swz == "w") reg += ".wwww";
		else if (swz == "b" || swz == "z") reg += ".zzzz";
		else if (swz == "g" || swz == "y") reg += ".yyyy";
		else if (swz == "r" || swz == "x") reg += ".xxxx";
		if (bias || bx2 || complement)
		{
			std::string t = R(8 + (c.tmp++ & 1));
			if (bias) c.w->Op("ADD", t, reg, "-" + C(kFcInternal, "zzzz"));
			else if (bx2) c.w->Op("MAD", t, reg, C(kFcInternal, "wwww"), "-" + C(kFcInternal, "xxxx"));
			else c.w->Op("ADD", t, "-" + reg, C(kFcInternal, "xxxx"));
			reg = t;
		}
		return neg ? "-" + reg : reg;
	}

	std::vector<std::string> SplitArgs(const std::string& s)
	{
		std::vector<std::string> out;
		std::string cur;
		for (size_t i = 0; i < s.size(); ++i)
		{
			if (s[i] == ',') { out.push_back(Trim(cur)); cur.clear(); }
			else cur += s[i];
		}
		if (!Trim(cur).empty()) out.push_back(Trim(cur));
		return out;
	}
}

std::string PixelShaderProgram(const char* source, bool fog, std::string* error)
{
	Writer w(true);
	FpHeader(w, 8);
	PsCtx c;
	c.w = &w;
	c.v1Written = false;
	c.tmp = 0;

	// v1 is read-only in ps_1_1, but the game's gloss-map pass writes it: give it a temp.
	std::string src = Lower(source ? source : "");
	for (size_t p = 0; p < src.size();)
	{
		size_t e = src.find('\n', p);
		if (e == std::string::npos) e = src.size();
		std::string l = Trim(src.substr(p, e - p));
		p = e + 1;
		if (!l.empty() && l[0] == '+') l = Trim(l.substr(1));
		size_t sp = l.find_first_of(" \t");
		if (sp != std::string::npos && Trim(l.substr(sp)).compare(0, 2, "v1") == 0)
		{
			w.Op("MOV", "R12", "fragment.color.secondary");
			c.v1Written = true;
			break;
		}
	}

	// Defaults: r0 starts as black in ps_1_x.
	w.Op("MOV", "R0", C(kFcInternal, "yyyy"));

	size_t pos = 0;
	while (pos <= src.size() && c.error.empty())
	{
		size_t end = src.find('\n', pos);
		if (end == std::string::npos) end = src.size();
		std::string line = src.substr(pos, end - pos);
		pos = end + 1;
		size_t comment = line.find("//");
		if (comment != std::string::npos) line = line.substr(0, comment);
		comment = line.find(';');
		if (comment != std::string::npos) line = line.substr(0, comment);
		line = Trim(line);
		while (!line.empty() && (line[line.size() - 1] == '\\' || line[line.size() - 1] == '\r'))
			line = Trim(line.substr(0, line.size() - 1));
		if (line.empty()) continue;
		if (line[0] == '+') line = Trim(line.substr(1));	// co-issue: issued in order here
		if (line.compare(0, 3, "ps.") == 0 || line.compare(0, 4, "xps.") == 0 || line.compare(0, 3, "ps_") == 0) continue;

		size_t sp = line.find_first_of(" \t");
		std::string op = sp == std::string::npos ? line : line.substr(0, sp);
		std::vector<std::string> args = sp == std::string::npos ? std::vector<std::string>() : SplitArgs(line.substr(sp + 1));

		// instruction modifiers
		bool sat = false;
		int scale = 0;	// 1 x2, 2 x4, -1 d2
		size_t us;
		while ((us = op.find_last_of('_')) != std::string::npos && us > 0)
		{
			std::string mod = op.substr(us + 1);
			op = op.substr(0, us);
			if (mod == "sat") sat = true;
			else if (mod == "x2") scale = 1;
			else if (mod == "x4") scale = 2;
			else if (mod == "d2") scale = -1;
		}

		if (op == "nop" || op == "def") continue;
		if (op == "tex" || op == "texcoord")
		{
			if (args.size() < 1) { c.error = "tex without register"; break; }
			int n = atoi(args[0].c_str() + 1) & 3;
			char buf[96];
			if (op == "tex")
				snprintf(buf, sizeof(buf), "TEX R%d, fragment.texcoord[%d], texture[%d], 2D;", 4 + n, n, n);
			else
				snprintf(buf, sizeof(buf), "MOV_SAT R%d, fragment.texcoord[%d];", 4 + n, n);
			w.Line(buf);
			continue;
		}
		if (args.size() < 2) { c.error = "too few operands: " + line; break; }

		std::string dstReg = args[0], dstMask;
		size_t dot = dstReg.find('.');
		if (dot != std::string::npos) { dstMask = dstReg.substr(dot + 1); dstReg = dstReg.substr(0, dot); }
		const std::string mask = Mask(dstMask);
		std::vector<std::string> s;
		for (size_t i = 1; i < args.size(); ++i) s.push_back(PsSrc(c, args[i]));
		if (!c.error.empty()) break;

		// Compute into R2, then scale/saturate into the destination.
		const std::string t = "R2";
		if (op == "mov") w.Op("MOV", t, s[0]);
		else if (op == "add" && s.size() >= 2) w.Op("ADD", t, s[0], s[1]);
		else if (op == "sub" && s.size() >= 2) w.Op("ADD", t, s[0], "-" + s[1]);
		else if (op == "mul" && s.size() >= 2) w.Op("MUL", t, s[0], s[1]);
		else if (op == "mad" && s.size() >= 3) w.Op("MAD", t, s[0], s[1], s[2]);
		else if (op == "lrp" && s.size() >= 3) w.Op("LRP", t, s[0], s[1], s[2]);
		else if (op == "dp3" && s.size() >= 2) w.Op("DP3", t, s[0], s[1]);
		else if (op == "dp4" && s.size() >= 2) w.Op("DP4", t, s[0], s[1]);
		else if (op == "cnd" && s.size() >= 3)
		{
			// s0 > 0.5 ? s1 : s2   ->   CMP(0.5 - s0 < 0 ? s1 : s2)
			w.Op("ADD", "R3", "-" + s[0], C(kFcInternal, "zzzz"));
			w.Op("CMP", t, "R3", s[1], s[2]);
		}
		else if (op == "cmp" && s.size() >= 3) w.Op("CMP", t, s[0], s[2], s[1]);	// s0 >= 0 ? s1 : s2
		else { c.error = "unsupported instruction: " + op; break; }

		if (scale == 1) w.Op("ADD", t, t, t);
		else if (scale == 2) w.Op("MUL", t, t, C(kFcInternal, "wwww")), w.Op("ADD", t, t, t);
		else if (scale == -1) w.Op("MUL", t, t, C(kFcInternal, "zzzz"));
		// ps_1_x registers hold -1..1
		std::string dst = PsReg(c, dstReg, true) + mask;
		if (sat)
			w.Op("MOV_SAT", dst, t);
		else
		{
			w.Op("MAX", t, t, "-" + C(kFcInternal, "xxxx"));
			w.Op("MIN", dst, t, C(kFcInternal, "xxxx"));
		}
	}

	if (!c.error.empty())
	{
		if (error) *error = c.error;
		return "";
	}
	w.Op("MOV_SAT", "R0", "R0");
	FpFinish(w, fog, kFcPsFogColor);
	return w.text;
}

} // namespace RsxShaderGen
