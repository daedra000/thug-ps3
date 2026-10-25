// Host check for Code/Ps3/rsx/rsx_shadergen.cpp: generates vertex and fragment programs
// for many pipeline states and for the game's ps_1_1 shaders, and assembles each with the
// in-game assembler. Any program the assembler rejects is a bug. Run under valgrind by
// tools/rsx-asm-test/run.sh. With an OUTDIR argument the sources are written there too.
#include "../../Code/Ps3/rsx/rsx_asm.h"
#include "../../Code/Ps3/rsx/rsx_shadergen.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace RsxShaderGen;

static int g_programs, g_failures;
static const char* g_outDir;

static void Check(const std::string& name, const std::string& src, bool vertex)
{
	++g_programs;
	std::vector<unsigned char> out;
	std::string err;
	bool ok = !src.empty() && (vertex ? RsxAsm::AssembleVertexProgram(src.c_str(), out, &err)
	                                  : RsxAsm::AssembleFragmentProgram(src.c_str(), out, &err));
	if (!ok)
	{
		++g_failures;
		printf("FAIL %s: %s\n%s\n", name.c_str(), err.c_str(), src.c_str());
	}
	if (g_outDir)
	{
		std::string path = std::string(g_outDir) + "/" + name + (vertex ? ".vasm" : ".fasm");
		FILE* f = fopen(path.c_str(), "wb");
		if (f) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
	}
}

// The game's fixed shaders (Gfx/DX9/NX/render.cpp) and material shaders in the shapes
// its generator writes.
static const char* kPixelShaders[] = {
	"ps.1.1\r\n \t\t\ttex t0 \r\n \t\t\tmul r0.rgb, t0, c0 \r\n \t\t\t+ mul_x2 r0.a, v0.a, t0.a \r\n \t\t\tmul_x4\tr0.rgb, v0, r0 \r\n",
	"ps.1.1\r\n mul_x2\tr0.rgb, v0, c0\r\n + mov r0.a, v0.a\r\n",
	"ps.1.1\r\n tex t0\r\n mul_x2\tr0,v0.a,t0.a\r\n",
	"ps.1.1\r\n tex t0\r\n mul_x2 r0,c4.a,t0.a\r\n",
	"ps.1.1 \r\n tex t0 \r\n mul r0.rgb,t0,c0 \r\n mul_x4 r0.rgb,v0,r0 \r\n +mul_x2 r0.a,c4.a,t0.a \r\n ",
	"ps.1.1\ntex t0\nmul_x2 r0.a, v0.a, t0.a\n+mov r0.rgb, t0\nmul_x2 r0.rgb, r0, v0\n",
	"ps.1.1\ntex t0\ntex t1\nmul_x2 r0.a, v0.a, t0.a\n+mov r0.rgb, t0\nmov r1.rgb, t1\n+mul_x2_sat r1.a, v0.a, t1.a\nlrp r0.rgb,r1.a,r1,r0\nmul_x2 r0.rgb, r0, v0\n",
	"ps.1.1\ntex t0\ntex t1\ntex t2\nmov r0, t0\nmov r1.rgb, t1\n+mul_x2 r1.a, v0.a, t1.a\nmad r0.rgb,r1,r1.a,r0\nmul_x2 r1.rgb, c2, t2\n+mov r1.a, t2\nmad r0.rgb,-r1,c2.a,r0\nmul_x2 r0.rgb, r0, v0\n",
	"ps.1.1\ntex t0\ntex t1\nmul_x2 r0.a, v0.a, t0.a\n+mul_x2 r0.rgb, c0, t0\nmov r1.rgb, t1\n+mul_x2 r1.a, v0.a, t1.a\nmul v1.rgb,v1,r1.a\nlrp r0.rgb, r0.a, t1, r0\nlrp r0.rgb, r0.a, r0, t1\nmul_x2 r0.rgb, r0, v0\n",
	"ps.1.1\ntex t0\ntex t1\ntex t2\ntex t3\nmov r0, t0\nmov r1.rgb, t1\n+mul_x2 r1.a, v0.a, t1.a\nlrp r0.rgb,c1.a,r1,r0\nmul r0.rgb,r0,c2.a\nmad r0.rgb,r0,t3.a,r0\nmul_x2 r0.rgb, r0, v0\n",
	"ps.1.1\ntex t0\ncnd r0, r0.a, t0, v0\nsub_d2 r1, 1-t0, t0_bias\nadd_x2 r0.rgb, r1, t0_bx2\ndp3_sat r1, t0, v0\n",
};

int main(int argc, char** argv)
{
	g_outDir = argc > 1 ? argv[1] : NULL;
	char name[96];

	// Vertex programs: pre-transformed, unlit, lit (directional/point, each material source,
	// specular, local viewer), fog, every texture-coordinate source and transform size.
	int n = 0;
	for (int rhw = 0; rhw < 2; ++rhw)
	for (int lit = 0; lit < 2; ++lit)
	for (int colours = 0; colours < 4; ++colours)
	for (int fog = 0; fog < 2; ++fog)
	for (int tex = 0; tex < 4; ++tex)
	{
		VsKey k;
		memset(&k, 0, sizeof(k));
		k.rhw = rhw;
		k.normal = !rhw;
		k.diffuse = colours & 1;
		k.specular = (colours >> 1) & 1;
		k.texSize[0] = 2; k.texSize[1] = 2; k.texSize[2] = tex == 3 ? 3 : 2; k.texSize[3] = 1;
		k.lighting = lit;
		k.light[0] = kLightDirectional;
		k.light[1] = (tex & 1) ? kLightPoint : kLightDirectional;
		k.light[2] = tex == 2 ? kLightPoint : kLightNone;
		k.diffuseSrc = colours & 1 ? kSrcColor1 : kSrcMaterial;
		k.ambientSrc = colours == 3 ? kSrcColor2 : kSrcMaterial;
		k.specularSrc = colours == 2 ? kSrcColor2 : kSrcMaterial;
		k.emissiveSrc = kSrcMaterial;
		k.specularEnable = (colours + tex) & 1;
		k.localViewer = tex & 1;
		k.fog = fog;
		k.numTexOut = tex + 1;
		for (int s = 0; s < 4; ++s)
		{
			k.tex[s].set = (s + tex) & 3;
			k.tex[s].gen = (s == 1 && tex >= 1) ? kGenReflection : (s == 2 && tex >= 2) ? kGenNormal : (s == 3) ? kGenPosition : kGenPassthru;
			k.tex[s].count = (tex + s) % 4 == 0 ? 0 : (tex + s) % 4 + 1;
		}
		k.pointSize = tex == 3;
		snprintf(name, sizeof(name), "vs%03d", n++);
		Check(name, VertexProgram(k), true);
	}

	// Fixed-function fragment programs: every colour/alpha op with each argument kind.
	n = 0;
	static const uint8_t args[] = { D3D::TA_DIFFUSE, D3D::TA_CURRENT, D3D::TA_TEXTURE, D3D::TA_TFACTOR,
		D3D::TA_SPECULAR, D3D::TA_TEMP, D3D::TA_TEXTURE | D3D::TA_COMPLEMENT, D3D::TA_DIFFUSE | D3D::TA_ALPHAREPLICATE };
	for (int op = D3D::TOP_SELECTARG1; op <= D3D::TOP_LERP; ++op)
	for (int a = 0; a < 8; a += 1)
	{
		FsKey k;
		memset(&k, 0, sizeof(k));
		k.numStages = 2 + (a & 1);
		for (int s = 0; s < 4; ++s)
		{
			TssStage& st = k.stage[s];
			st.colorOp = s == 0 ? op : D3D::TOP_MODULATE;
			st.alphaOp = s == 0 ? (D3D::TOP_LERP + D3D::TOP_SELECTARG1 - op) : D3D::TOP_SELECTARG1;
			st.colorArg0 = args[(a + 3) & 7]; st.colorArg1 = args[a]; st.colorArg2 = args[(a + 1) & 7];
			st.alphaArg0 = args[(a + 5) & 7]; st.alphaArg1 = args[(a + 2) & 7]; st.alphaArg2 = args[(a + 4) & 7];
			st.resultTemp = (s == 1 && (a & 2));
			st.projected = (s == 2 && (a & 4));
			st.hasTexture = (s + a) % 3 != 0;
		}
		k.specularAdd = a & 1;
		k.fog = (a >> 1) & 1;
		snprintf(name, sizeof(name), "ff%03d", n++);
		Check(name, FixedFunctionFragmentProgram(k), false);
	}
	{
		FsKey k;
		memset(&k, 0, sizeof(k));
		Check("ff_nostages", FixedFunctionFragmentProgram(k), false);
	}

	for (size_t i = 0; i < sizeof(kPixelShaders) / sizeof(kPixelShaders[0]); ++i)
	for (int fog = 0; fog < 2; ++fog)
	{
		std::string err;
		std::string src = PixelShaderProgram(kPixelShaders[i], fog != 0, &err);
		snprintf(name, sizeof(name), "ps%02d%s", (int)i, fog ? "f" : "");
		if (src.empty()) { ++g_programs; ++g_failures; printf("FAIL %s: %s\n", name, err.c_str()); continue; }
		Check(name, src, false);
	}

	printf("%d programs, %d failed\n", g_programs, g_failures);
	return g_failures ? 1 : 0;
}
