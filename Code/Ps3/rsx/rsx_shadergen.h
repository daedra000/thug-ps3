// THUG PS3: NV40 programs for the D3D9 pipeline the game uses.
//
// kisak's PC renderer draws with the fixed-function vertex pipeline (SetFVF, transforms,
// lights, fog, texture-coordinate transforms) and, per pixel, either ps_1_1 shaders that
// it writes as text at run time or fixed-function texture stages. This file turns each of
// those into NV40 assembly for rsx_asm (cgcomp's "!!VP2.0" / "!!ARBfp1.0" dialect). It
// knows nothing about the RSX or D3D9 headers, so it also builds on a PC for tests
// (tools/rsx-asm-test).
//
// NV40 rules the writers enforce: an instruction reads at most one constant register and
// one input attribute (extra ones are first copied to a temporary), and fragment programs
// use numbered registers only (cgcomp ignores TEMP declarations).
#ifndef PS3_RSX_SHADERGEN_H
#define PS3_RSX_SHADERGEN_H

#include <string>
#include <stdint.h>

namespace RsxShaderGen
{

// ---------------------------------------------------------------- vertex programs
// Vertex attribute registers (PSL1GHT GCM_VERTEX_ATTRIB_*): position 0, normal 2,
// diffuse 3, specular 4, texture coordinates 8 + set. Colours arrive as four unsigned
// bytes in memory order; D3DCOLORs are host-order ARGB words, so a colour reads A,R,G,B
// and the programs swizzle it to RGBA.
enum { kAttrPos = 0, kAttrNormal = 2, kAttrDiffuse = 3, kAttrSpecular = 4, kAttrTex0 = 8 };

// Constant registers the backend loads (c[N]).
enum
{
	kVcClip = 0,		// 4 rows: object -> clip (D3D's World*View*Proj, transposed)
	kVcWorldView = 4,	// 4 rows: object -> eye
	kVcNormal = 8,		// 3 rows: object -> eye for normals (inverse transpose)
	kVcAmbient = 11,	// D3DRS_AMBIENT, rgba 0..1
	kVcMatDiffuse = 12, kVcMatAmbient = 13, kVcMatSpecular = 14, kVcMatEmissive = 15,
	kVcParams = 16,		// x specular power, y fog end, z 1/(fog end - fog start), w 0
	kVcConsts = 17,		// 0, 1, 0.5, 2
	kVcScreen = 18,		// pre-transformed vertices: 2/W, -2/H, -1 - 2X/W, 1 + 2Y/H
	kVcPoint = 19,		// x point size
	kVcLights = 20,		// 5 per light: direction to light (or eye position), diffuse, specular, ambient, attenuation 0/1/2 + range
	kVcTexMatrix = 40,	// 4 per stage: the texture transform's columns
	kVcCount = 56
};

enum MaterialSource { kSrcMaterial = 0, kSrcColor1 = 1, kSrcColor2 = 2 };
enum TexGen { kGenPassthru = 0, kGenNormal = 1, kGenPosition = 2, kGenReflection = 3 };
enum LightType { kLightNone = 0, kLightDirectional = 1, kLightPoint = 2 };

struct VsKey
{
	uint8_t rhw;			// pre-transformed position (D3DFVF_XYZRHW)
	uint8_t normal, diffuse, specular;
	uint8_t texSize[8];		// floats per input texture-coordinate set (0 = absent)
	uint8_t lighting;
	uint8_t light[4];		// LightType per enabled light, packed from 0
	uint8_t diffuseSrc, ambientSrc, specularSrc, emissiveSrc;
	uint8_t specularEnable, localViewer;
	uint8_t fog;			// 1 = linear fog factor to result.fogcoord
	uint8_t numTexOut;		// texture stages that need coordinates (0..4)
	struct Tex { uint8_t set, gen, count, projected; } tex[4];	// count: transform components, 0 = none
	uint8_t pointSize;
};

std::string VertexProgram(const VsKey& key);

// ---------------------------------------------------------------- fragment programs
// Constant registers for fixed-function stages; ps_1_1 programs use c[0..7] for c0-c7.
enum { kFcTextureFactor = 0, kFcFogColor = 1, kFcPsFogColor = 8 };

struct TssStage
{
	uint8_t colorOp, colorArg0, colorArg1, colorArg2;
	uint8_t alphaOp, alphaArg0, alphaArg1, alphaArg2;
	uint8_t resultTemp;		// D3DTSS_RESULTARG == D3DTA_TEMP
	uint8_t projected;		// D3DTTFF_PROJECTED
	uint8_t hasTexture;		// a texture is bound (D3D reads black/white otherwise)
};

struct FsKey
{
	uint8_t numStages;		// stages before the first D3DTOP_DISABLE (max 4)
	TssStage stage[4];
	uint8_t specularAdd;	// D3DRS_SPECULARENABLE
	uint8_t fog;			// blend towards c[kFcFogColor] by fragment.fogcoord.x
};

std::string FixedFunctionFragmentProgram(const FsKey& key);

// ps_1_x source text, as given to D3DXAssembleShader. On failure returns "" and says why.
std::string PixelShaderProgram(const char* source, bool fog, std::string* error = 0);

// D3D9 values used above (checked against d3d9.h by the backend).
namespace D3D
{
	enum { TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X, TOP_MODULATE4X,
		TOP_ADD, TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH, TOP_BLENDDIFFUSEALPHA,
		TOP_BLENDTEXTUREALPHA, TOP_BLENDFACTORALPHA, TOP_BLENDTEXTUREALPHAPM, TOP_BLENDCURRENTALPHA,
		TOP_PREMODULATE, TOP_MODULATEALPHA_ADDCOLOR, TOP_MODULATECOLOR_ADDALPHA, TOP_MODULATEINVALPHA_ADDCOLOR,
		TOP_MODULATEINVCOLOR_ADDALPHA, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE, TOP_DOTPRODUCT3,
		TOP_MULTIPLYADD, TOP_LERP };
	enum { TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3, TA_SPECULAR = 4, TA_TEMP = 5,
		TA_CONSTANT = 6, TA_SELECTMASK = 0x0F, TA_COMPLEMENT = 0x10, TA_ALPHAREPLICATE = 0x20 };
}

} // namespace RsxShaderGen

#endif // PS3_RSX_SHADERGEN_H
