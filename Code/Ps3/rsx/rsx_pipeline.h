// THUG PS3: D3D9 device state -> the RSX shader generator's keys and constants.
//
// The part of the RSX backend that turns the game's fixed-function state (FVF, transforms,
// lights, material, fog, texture stages) into a VsKey / FsKey and the vertex-program
// constants. It has no RSX dependencies, so tools/rsx-asm-test runs it on a PC against
// reference D3D9 maths.
#ifndef PS3_RSX_PIPELINE_H
#define PS3_RSX_PIPELINE_H

#include <d3d9.h>
#include "rsx_shadergen.h"

namespace RsxPipeline
{
	using namespace RsxShaderGen;

	struct FvfLayout
	{
		uint32_t stride;
		int pos, posSize, normal, diffuse, specular;
		int tex[8], texSize[8];
		int numTex;
		bool rhw;
	};

	FvfLayout Layout(DWORD fvf);
	void FillVsKey(const IDirect3DDevice9* dev, const FvfLayout& l, int numTexOut, VsKey& k);
	void FillVsConstants(const IDirect3DDevice9* dev, float (*c)[4]);	// kVcCount registers
	void FillFsKey(const IDirect3DDevice9* dev, FsKey& k, const bool* hasTexture);	// hasTexture[4]
	bool FogEnabled(const IDirect3DDevice9* dev);
	const char* PixelShaderSource(const IDirect3DPixelShader9* ps);	// the text D3DXAssembleShader kept
	void ColorToFloats(D3DCOLOR c, float* out);
	float AsFloat(DWORD d);

	// The viewport transform the backend loads: GL-style depth, y flipped, D3D9's half-pixel offset.
	void ViewportTransform(const D3DVIEWPORT9& vp, float* scale, float* offset);
}

#endif // PS3_RSX_PIPELINE_H
