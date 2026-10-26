// THUG PS3: the subset of the Direct3D 9 API that kisak-thug's DX9 renderer uses,
// implemented on the PS3 (Code/Ps3/d3d9/). Enum values match the real SDK so the
// renderer code and any stored state values behave the same as on PC.
//
// Objects are plain C++ classes (no COM vtables): the renderer only ever calls
// them through the D3DDevice_* macros in core/defines.h or directly via ->.
#ifndef PS3_D3D9_H
#define PS3_D3D9_H

#include "ps3_compat.h"

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------
typedef DWORD D3DCOLOR;
#define D3DCOLOR_ARGB(a,r,g,b) ((D3DCOLOR)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))
#define D3DCOLOR_RGBA(r,g,b,a) D3DCOLOR_ARGB(a,r,g,b)
#define D3DCOLOR_XRGB(r,g,b)   D3DCOLOR_ARGB(0xff,r,g,b)
#define D3DCOLOR_COLORVALUE(r,g,b,a) D3DCOLOR_RGBA((DWORD)((r)*255.f),(DWORD)((g)*255.f),(DWORD)((b)*255.f),(DWORD)((a)*255.f))

typedef struct _D3DVECTOR { float x, y, z; } D3DVECTOR;
typedef struct _D3DCOLORVALUE { float r, g, b, a; } D3DCOLORVALUE;
typedef struct _D3DRECT { LONG x1, y1, x2, y2; } D3DRECT;

typedef struct _D3DMATRIX {
	union {
		struct {
			float _11, _12, _13, _14;
			float _21, _22, _23, _24;
			float _31, _32, _33, _34;
			float _41, _42, _43, _44;
		};
		float m[4][4];
	};
} D3DMATRIX;

typedef struct _D3DVIEWPORT9 { DWORD X, Y, Width, Height; float MinZ, MaxZ; } D3DVIEWPORT9;
typedef D3DVIEWPORT9 D3DVIEWPORT8;

typedef struct _D3DMATERIAL9 {
	D3DCOLORVALUE Diffuse, Ambient, Specular, Emissive;
	float Power;
} D3DMATERIAL9;
typedef D3DMATERIAL9 D3DMATERIAL8;

typedef enum _D3DLIGHTTYPE { D3DLIGHT_POINT = 1, D3DLIGHT_SPOT = 2, D3DLIGHT_DIRECTIONAL = 3, D3DLIGHT_FORCE_DWORD = 0x7fffffff } D3DLIGHTTYPE;
typedef struct _D3DLIGHT9 {
	D3DLIGHTTYPE Type;
	D3DCOLORVALUE Diffuse, Specular, Ambient;
	D3DVECTOR Position, Direction;
	float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
} D3DLIGHT9;
typedef D3DLIGHT9 D3DLIGHT8;

typedef struct _D3DGAMMARAMP { WORD red[256], green[256], blue[256]; } D3DGAMMARAMP;

// ---------------------------------------------------------------------------
// Formats (D3D9 values; Xbox-only formats map onto their nearest D3D9 value)
// ---------------------------------------------------------------------------
typedef enum _D3DFORMAT {
	D3DFMT_UNKNOWN = 0,
	D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23,
	D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27,
	D3DFMT_A8 = 28, D3DFMT_A8R3G3B2 = 29, D3DFMT_X4R4G4B4 = 30, D3DFMT_A2B10G10R10 = 31,
	D3DFMT_A8B8G8R8 = 32, D3DFMT_X8B8G8R8 = 33, D3DFMT_G16R16 = 34, D3DFMT_A2R10G10B10 = 35,
	D3DFMT_A16B16G16R16 = 36, D3DFMT_A8P8 = 40, D3DFMT_P8 = 41, D3DFMT_L8 = 50, D3DFMT_A8L8 = 51,
	D3DFMT_A4L4 = 52, D3DFMT_V8U8 = 60, D3DFMT_L6V5U5 = 61, D3DFMT_X8L8V8U8 = 62, D3DFMT_Q8W8V8U8 = 63,
	D3DFMT_V16U16 = 64, D3DFMT_A2W10V10U10 = 67,
	D3DFMT_UYVY = MAKEFOURCC('U','Y','V','Y'), D3DFMT_YUY2 = MAKEFOURCC('Y','U','Y','2'),
	D3DFMT_DXT1 = MAKEFOURCC('D','X','T','1'), D3DFMT_DXT2 = MAKEFOURCC('D','X','T','2'),
	D3DFMT_DXT3 = MAKEFOURCC('D','X','T','3'), D3DFMT_DXT4 = MAKEFOURCC('D','X','T','4'),
	D3DFMT_DXT5 = MAKEFOURCC('D','X','T','5'),
	D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71, D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77,
	D3DFMT_D24X4S4 = 79, D3DFMT_D16 = 80, D3DFMT_D32F_LOCKABLE = 82, D3DFMT_D24FS8 = 83,
	D3DFMT_L16 = 81,
	D3DFMT_VERTEXDATA = 100, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
	D3DFMT_Q16W16V16U16 = 110, D3DFMT_R16F = 111, D3DFMT_G16R16F = 112, D3DFMT_A16B16G16R16F = 113,
	D3DFMT_R32F = 114, D3DFMT_G32R32F = 115, D3DFMT_A32B32G32R32F = 116,

	// Xbox-only names still referenced by the renderer. Values above 0x1000 are ours.
	D3DFMT_LIN_A8R8G8B8 = D3DFMT_A8R8G8B8, D3DFMT_LIN_X8R8G8B8 = D3DFMT_X8R8G8B8,
	D3DFMT_LIN_R5G6B5 = D3DFMT_R5G6B5, D3DFMT_LIN_A1R5G5B5 = D3DFMT_A1R5G5B5,
	D3DFMT_LIN_X1R5G5B5 = D3DFMT_X1R5G5B5, D3DFMT_LIN_A4R4G4B4 = D3DFMT_A4R4G4B4,
	D3DFMT_LIN_A8 = D3DFMT_A8, D3DFMT_LIN_L8 = D3DFMT_L8, D3DFMT_LIN_A8L8 = D3DFMT_A8L8,
	D3DFMT_LIN_L16 = D3DFMT_L16, D3DFMT_LIN_V16U16 = D3DFMT_V16U16, D3DFMT_LIN_D24S8 = D3DFMT_D24S8,
	D3DFMT_LIN_D16 = D3DFMT_D16, D3DFMT_LIN_A8B8G8R8 = D3DFMT_A8B8G8R8,
	D3DFMT_AL8 = 0x1001, D3DFMT_LIN_AL8 = D3DFMT_AL8,
	D3DFMT_B8G8R8A8 = 0x1002, D3DFMT_LIN_B8G8R8A8 = D3DFMT_B8G8R8A8,
	D3DFMT_R8G8B8A8 = 0x1003, D3DFMT_LIN_R8G8B8A8 = D3DFMT_R8G8B8A8,
	D3DFMT_R4G4B4A4 = 0x1004, D3DFMT_LIN_R4G4B4A4 = D3DFMT_R4G4B4A4,
	D3DFMT_R5G5B5A1 = 0x1005, D3DFMT_LIN_R5G5B5A1 = D3DFMT_R5G5B5A1,
	D3DFMT_R6G5B5 = 0x1006, D3DFMT_LIN_R6G5B5 = D3DFMT_R6G5B5,
	D3DFMT_G8B8 = 0x1007, D3DFMT_LIN_G8B8 = D3DFMT_G8B8,
	D3DFMT_R8B8 = 0x1008, D3DFMT_LIN_R8B8 = D3DFMT_R8B8,
	D3DFMT_F24S8 = 0x1009, D3DFMT_LIN_F24S8 = D3DFMT_F24S8,
	D3DFMT_F16 = 0x100a, D3DFMT_LIN_F16 = D3DFMT_F16,

	D3DFMT_FORCE_DWORD = 0x7fffffff
} D3DFORMAT;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
typedef enum _D3DPRIMITIVETYPE {
	D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3, D3DPT_TRIANGLELIST = 4,
	D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6,
	D3DPT_QUADLIST = 8,   // Xbox
	D3DPT_FORCE_DWORD = 0x7fffffff
} D3DPRIMITIVETYPE;

typedef enum _D3DTRANSFORMSTATETYPE {
	D3DTS_VIEW = 2, D3DTS_PROJECTION = 3,
	D3DTS_TEXTURE0 = 16, D3DTS_TEXTURE1, D3DTS_TEXTURE2, D3DTS_TEXTURE3,
	D3DTS_TEXTURE4, D3DTS_TEXTURE5, D3DTS_TEXTURE6, D3DTS_TEXTURE7,
	D3DTS_FORCE_DWORD = 0x7fffffff
} D3DTRANSFORMSTATETYPE;
#define D3DTS_WORLDMATRIX(i) (D3DTRANSFORMSTATETYPE)((i) + 256)
#define D3DTS_WORLD D3DTS_WORLDMATRIX(0)

typedef enum _D3DRENDERSTATETYPE {
	D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_SHADEMODE = 9, D3DRS_ZWRITEENABLE = 14,
	D3DRS_ALPHATESTENABLE = 15, D3DRS_LASTPIXEL = 16, D3DRS_SRCBLEND = 19, D3DRS_DESTBLEND = 20,
	D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
	D3DRS_DITHERENABLE = 26, D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28,
	D3DRS_SPECULARENABLE = 29, D3DRS_FOGCOLOR = 34, D3DRS_FOGTABLEMODE = 35, D3DRS_FOGSTART = 36,
	D3DRS_FOGEND = 37, D3DRS_FOGDENSITY = 38, D3DRS_RANGEFOGENABLE = 48, D3DRS_STENCILENABLE = 52,
	D3DRS_STENCILFAIL = 53, D3DRS_STENCILZFAIL = 54, D3DRS_STENCILPASS = 55, D3DRS_STENCILFUNC = 56,
	D3DRS_STENCILREF = 57, D3DRS_STENCILMASK = 58, D3DRS_STENCILWRITEMASK = 59,
	D3DRS_TEXTUREFACTOR = 60, D3DRS_WRAP0 = 128, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3,
	D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_CLIPPING = 136,
	D3DRS_LIGHTING = 137, D3DRS_AMBIENT = 139, D3DRS_FOGVERTEXMODE = 140, D3DRS_COLORVERTEX = 141,
	D3DRS_LOCALVIEWER = 142, D3DRS_NORMALIZENORMALS = 143, D3DRS_DIFFUSEMATERIALSOURCE = 145,
	D3DRS_SPECULARMATERIALSOURCE = 146, D3DRS_AMBIENTMATERIALSOURCE = 147,
	D3DRS_EMISSIVEMATERIALSOURCE = 148, D3DRS_VERTEXBLEND = 151, D3DRS_CLIPPLANEENABLE = 152,
	D3DRS_POINTSIZE = 154, D3DRS_POINTSIZE_MIN = 155, D3DRS_POINTSPRITEENABLE = 156,
	D3DRS_POINTSCALEENABLE = 157, D3DRS_POINTSCALE_A = 158, D3DRS_POINTSCALE_B = 159,
	D3DRS_POINTSCALE_C = 160, D3DRS_MULTISAMPLEANTIALIAS = 161, D3DRS_MULTISAMPLEMASK = 162,
	D3DRS_PATCHEDGESTYLE = 163, D3DRS_DEBUGMONITORTOKEN = 165, D3DRS_POINTSIZE_MAX = 166,
	D3DRS_INDEXEDVERTEXBLENDENABLE = 167, D3DRS_COLORWRITEENABLE = 168, D3DRS_TWEENFACTOR = 170,
	D3DRS_BLENDOP = 171, D3DRS_POSITIONDEGREE = 172, D3DRS_NORMALDEGREE = 173,
	D3DRS_SCISSORTESTENABLE = 174, D3DRS_SLOPESCALEDEPTHBIAS = 175, D3DRS_ANTIALIASEDLINEENABLE = 176,
	D3DRS_TWOSIDEDSTENCILMODE = 185, D3DRS_CCW_STENCILFAIL = 186, D3DRS_CCW_STENCILZFAIL = 187,
	D3DRS_CCW_STENCILPASS = 188, D3DRS_CCW_STENCILFUNC = 189, D3DRS_COLORWRITEENABLE1 = 190,
	D3DRS_COLORWRITEENABLE2 = 191, D3DRS_COLORWRITEENABLE3 = 192, D3DRS_BLENDFACTOR = 193,
	D3DRS_SRGBWRITEENABLE = 194, D3DRS_DEPTHBIAS = 195, D3DRS_SEPARATEALPHABLENDENABLE = 206,
	D3DRS_SRCBLENDALPHA = 207, D3DRS_DESTBLENDALPHA = 208, D3DRS_BLENDOPALPHA = 209,
	D3DRS_MAX = 210,
	D3DRS_FORCE_DWORD = 0x7fffffff
} D3DRENDERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {
	D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4,
	D3DTSS_ALPHAARG1 = 5, D3DTSS_ALPHAARG2 = 6, D3DTSS_BUMPENVMAT00 = 7, D3DTSS_BUMPENVMAT01 = 8,
	D3DTSS_BUMPENVMAT10 = 9, D3DTSS_BUMPENVMAT11 = 10, D3DTSS_TEXCOORDINDEX = 11,
	D3DTSS_BUMPENVLSCALE = 22, D3DTSS_BUMPENVLOFFSET = 23, D3DTSS_TEXTURETRANSFORMFLAGS = 24,
	D3DTSS_COLORARG0 = 26, D3DTSS_ALPHAARG0 = 27, D3DTSS_RESULTARG = 28, D3DTSS_CONSTANT = 32,
	D3DTSS_MAX = 33,
	D3DTSS_FORCE_DWORD = 0x7fffffff
} D3DTEXTURESTAGESTATETYPE;

typedef enum _D3DSAMPLERSTATETYPE {
	D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_ADDRESSW = 3, D3DSAMP_BORDERCOLOR = 4,
	D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7, D3DSAMP_MIPMAPLODBIAS = 8,
	D3DSAMP_MAXMIPLEVEL = 9, D3DSAMP_MAXANISOTROPY = 10, D3DSAMP_SRGBTEXTURE = 11,
	D3DSAMP_ELEMENTINDEX = 12, D3DSAMP_DMAPOFFSET = 13, D3DSAMP_MAX = 14,
	D3DSAMP_FORCE_DWORD = 0x7fffffff
} D3DSAMPLERSTATETYPE;

typedef enum _D3DTEXTUREOP {
	D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
	D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT,
	D3DTOP_ADDSMOOTH, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_BLENDFACTORALPHA,
	D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_BLENDCURRENTALPHA, D3DTOP_PREMODULATE,
	D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA,
	D3DTOP_MODULATEINVALPHA_ADDCOLOR, D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_BUMPENVMAP,
	D3DTOP_BUMPENVMAPLUMINANCE, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP,
	D3DTOP_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREOP;

#define D3DTA_SELECTMASK     0x0000000f
#define D3DTA_DIFFUSE        0x00000000
#define D3DTA_CURRENT        0x00000001
#define D3DTA_TEXTURE        0x00000002
#define D3DTA_TFACTOR        0x00000003
#define D3DTA_SPECULAR       0x00000004
#define D3DTA_TEMP           0x00000005
#define D3DTA_CONSTANT       0x00000006
#define D3DTA_COMPLEMENT     0x00000010
#define D3DTA_ALPHAREPLICATE 0x00000020

typedef enum _D3DTEXTUREFILTERTYPE {
	D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2, D3DTEXF_ANISOTROPIC = 3,
	D3DTEXF_PYRAMIDALQUAD = 6, D3DTEXF_GAUSSIANQUAD = 7, D3DTEXF_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREFILTERTYPE;

typedef enum _D3DTEXTUREADDRESS {
	D3DTADDRESS_WRAP = 1, D3DTADDRESS_MIRROR = 2, D3DTADDRESS_CLAMP = 3, D3DTADDRESS_BORDER = 4,
	D3DTADDRESS_MIRRORONCE = 5, D3DTADDRESS_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREADDRESS;

typedef enum _D3DBLEND {
	D3DBLEND_ZERO = 1, D3DBLEND_ONE, D3DBLEND_SRCCOLOR, D3DBLEND_INVSRCCOLOR, D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA, D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA, D3DBLEND_DESTCOLOR,
	D3DBLEND_INVDESTCOLOR, D3DBLEND_SRCALPHASAT, D3DBLEND_BOTHSRCALPHA, D3DBLEND_BOTHINVSRCALPHA,
	D3DBLEND_BLENDFACTOR, D3DBLEND_INVBLENDFACTOR, D3DBLEND_FORCE_DWORD = 0x7fffffff
} D3DBLEND;

typedef enum _D3DBLENDOP {
	D3DBLENDOP_ADD = 1, D3DBLENDOP_SUBTRACT, D3DBLENDOP_REVSUBTRACT, D3DBLENDOP_MIN, D3DBLENDOP_MAX,
	D3DBLENDOP_FORCE_DWORD = 0x7fffffff
} D3DBLENDOP;

typedef enum _D3DCMPFUNC {
	D3DCMP_NEVER = 1, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL, D3DCMP_GREATER,
	D3DCMP_NOTEQUAL, D3DCMP_GREATEREQUAL, D3DCMP_ALWAYS, D3DCMP_FORCE_DWORD = 0x7fffffff
} D3DCMPFUNC;

typedef enum _D3DSTENCILOP {
	D3DSTENCILOP_KEEP = 1, D3DSTENCILOP_ZERO, D3DSTENCILOP_REPLACE, D3DSTENCILOP_INCRSAT,
	D3DSTENCILOP_DECRSAT, D3DSTENCILOP_INVERT, D3DSTENCILOP_INCR, D3DSTENCILOP_DECR,
	D3DSTENCILOP_FORCE_DWORD = 0x7fffffff
} D3DSTENCILOP;

typedef enum _D3DCULL { D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3, D3DCULL_FORCE_DWORD = 0x7fffffff } D3DCULL;
typedef enum _D3DFILLMODE { D3DFILL_POINT = 1, D3DFILL_WIREFRAME = 2, D3DFILL_SOLID = 3, D3DFILL_FORCE_DWORD = 0x7fffffff } D3DFILLMODE;
typedef enum _D3DSHADEMODE { D3DSHADE_FLAT = 1, D3DSHADE_GOURAUD = 2, D3DSHADE_PHONG = 3, D3DSHADE_FORCE_DWORD = 0x7fffffff } D3DSHADEMODE;
typedef enum _D3DZBUFFERTYPE { D3DZB_FALSE = 0, D3DZB_TRUE = 1, D3DZB_USEW = 2, D3DZB_FORCE_DWORD = 0x7fffffff } D3DZBUFFERTYPE;
typedef enum _D3DFOGMODE { D3DFOG_NONE = 0, D3DFOG_EXP = 1, D3DFOG_EXP2 = 2, D3DFOG_LINEAR = 3, D3DFOG_FORCE_DWORD = 0x7fffffff } D3DFOGMODE;
typedef enum _D3DMATERIALCOLORSOURCE { D3DMCS_MATERIAL = 0, D3DMCS_COLOR1 = 1, D3DMCS_COLOR2 = 2, D3DMCS_FORCE_DWORD = 0x7fffffff } D3DMATERIALCOLORSOURCE;
typedef enum _D3DVERTEXBLENDFLAGS { D3DVBF_DISABLE = 0, D3DVBF_1WEIGHTS = 1, D3DVBF_2WEIGHTS = 2, D3DVBF_3WEIGHTS = 3, D3DVBF_TWEENING = 255, D3DVBF_0WEIGHTS = 256 } D3DVERTEXBLENDFLAGS;
typedef enum _D3DTEXTURETRANSFORMFLAGS { D3DTTFF_DISABLE = 0, D3DTTFF_COUNT1 = 1, D3DTTFF_COUNT2 = 2, D3DTTFF_COUNT3 = 3, D3DTTFF_COUNT4 = 4, D3DTTFF_PROJECTED = 256, D3DTTFF_FORCE_DWORD = 0x7fffffff } D3DTEXTURETRANSFORMFLAGS;

#define D3DTSS_TCI_PASSTHRU                    0x00000000
#define D3DTSS_TCI_CAMERASPACENORMAL           0x00010000
#define D3DTSS_TCI_CAMERASPACEPOSITION         0x00020000
#define D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR 0x00030000
#define D3DTSS_TCI_SPHEREMAP                   0x00040000

#define D3DCOLORWRITEENABLE_RED   (1L<<0)
#define D3DCOLORWRITEENABLE_GREEN (1L<<1)
#define D3DCOLORWRITEENABLE_BLUE  (1L<<2)
#define D3DCOLORWRITEENABLE_ALPHA (1L<<3)
#define D3DCOLORWRITEENABLE_ALL   0xf

#define D3DWRAP_U 0x00000001
#define D3DWRAP_V 0x00000002
#define D3DWRAP_W 0x00000004
#define D3DWRAPCOORD_0 0x00000001
#define D3DWRAPCOORD_1 0x00000002

typedef enum _D3DRESOURCETYPE {
	D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3, D3DRTYPE_VOLUMETEXTURE = 4,
	D3DRTYPE_CUBETEXTURE = 5, D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7,
	D3DRTYPE_FORCE_DWORD = 0x7fffffff
} D3DRESOURCETYPE;

typedef enum _D3DPOOL { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3, D3DPOOL_FORCE_DWORD = 0x7fffffff } D3DPOOL;
typedef enum _D3DMULTISAMPLE_TYPE { D3DMULTISAMPLE_NONE = 0, D3DMULTISAMPLE_NONMASKABLE = 1, D3DMULTISAMPLE_2_SAMPLES = 2, D3DMULTISAMPLE_4_SAMPLES = 4, D3DMULTISAMPLE_FORCE_DWORD = 0x7fffffff } D3DMULTISAMPLE_TYPE;
typedef enum _D3DSWAPEFFECT { D3DSWAPEFFECT_DISCARD = 1, D3DSWAPEFFECT_FLIP = 2, D3DSWAPEFFECT_COPY = 3, D3DSWAPEFFECT_FORCE_DWORD = 0x7fffffff } D3DSWAPEFFECT;
typedef enum _D3DDEVTYPE { D3DDEVTYPE_HAL = 1, D3DDEVTYPE_REF = 2, D3DDEVTYPE_SW = 3, D3DDEVTYPE_NULLREF = 4, D3DDEVTYPE_FORCE_DWORD = 0x7fffffff } D3DDEVTYPE;
typedef enum _D3DBACKBUFFER_TYPE { D3DBACKBUFFER_TYPE_MONO = 0, D3DBACKBUFFER_TYPE_LEFT = 1, D3DBACKBUFFER_TYPE_RIGHT = 2, D3DBACKBUFFER_TYPE_FORCE_DWORD = 0x7fffffff } D3DBACKBUFFER_TYPE;

#define D3DUSAGE_RENDERTARGET   0x00000001L
#define D3DUSAGE_DEPTHSTENCIL   0x00000002L
#define D3DUSAGE_DYNAMIC        0x00000200L
#define D3DUSAGE_WRITEONLY      0x00000008L
#define D3DUSAGE_SOFTWAREPROCESSING 0x00000010L
#define D3DUSAGE_DONOTCLIP      0x00000020L
#define D3DUSAGE_POINTS         0x00000040L
#define D3DUSAGE_AUTOGENMIPMAP  0x00000400L

#define D3DLOCK_READONLY        0x00000010L
#define D3DLOCK_DISCARD         0x00002000L
#define D3DLOCK_NOOVERWRITE     0x00001000L
#define D3DLOCK_NOSYSLOCK       0x00000800L
#define D3DLOCK_NO_DIRTY_UPDATE 0x00008000L

#define D3DCLEAR_TARGET  0x00000001L
#define D3DCLEAR_ZBUFFER 0x00000002L
#define D3DCLEAR_STENCIL 0x00000004L

#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020L
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040L
#define D3DCREATE_MIXED_VERTEXPROCESSING    0x00000080L
#define D3DCREATE_PUREDEVICE                0x00000010L
#define D3DCREATE_MULTITHREADED             0x00000004L
#define D3DCREATE_FPU_PRESERVE              0x00000002L

#define D3DPRESENT_INTERVAL_DEFAULT   0x00000000L
#define D3DPRESENT_INTERVAL_ONE       0x00000001L
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000L
#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 0x00000001
#define D3DADAPTER_DEFAULT 0
#define D3DSGR_NO_CALIBRATION 0x00000000L
#define D3DSGR_CALIBRATE      0x00000001L
#define D3DENUM_WHQL_LEVEL    0x00000002L

#define D3DERR_DEVICELOST     ((HRESULT)0x88760868)
#define D3DERR_DEVICENOTRESET ((HRESULT)0x88760869)
#define D3DERR_NOTAVAILABLE   ((HRESULT)0x8876086a)
#define D3DERR_INVALIDCALL    ((HRESULT)0x8876086c)
#define D3DERR_OUTOFVIDEOMEMORY ((HRESULT)0x8876017c)
#define D3D_OK S_OK

// Flexible vertex format bits
#define D3DFVF_RESERVED0  0x001
#define D3DFVF_POSITION_MASK 0x400E
#define D3DFVF_XYZ        0x002
#define D3DFVF_XYZRHW     0x004
#define D3DFVF_XYZB1      0x006
#define D3DFVF_XYZB2      0x008
#define D3DFVF_XYZB3      0x00a
#define D3DFVF_XYZB4      0x00c
#define D3DFVF_XYZB5      0x00e
#define D3DFVF_XYZW       0x4002
#define D3DFVF_NORMAL     0x010
#define D3DFVF_PSIZE      0x020
#define D3DFVF_DIFFUSE    0x040
#define D3DFVF_SPECULAR   0x080
#define D3DFVF_TEXCOUNT_MASK  0xf00
#define D3DFVF_TEXCOUNT_SHIFT 8
#define D3DFVF_TEX0 0x000
#define D3DFVF_TEX1 0x100
#define D3DFVF_TEX2 0x200
#define D3DFVF_TEX3 0x300
#define D3DFVF_TEX4 0x400
#define D3DFVF_LASTBETA_UBYTE4   0x1000
#define D3DFVF_LASTBETA_D3DCOLOR 0x8000
#define D3DFVF_TEXTUREFORMAT2 0
#define D3DFVF_TEXTUREFORMAT1 3
#define D3DFVF_TEXTUREFORMAT3 1
#define D3DFVF_TEXTUREFORMAT4 2
#define D3DFVF_TEXCOORDSIZE1(i) (D3DFVF_TEXTUREFORMAT1 << ((i)*2 + 16))
#define D3DFVF_TEXCOORDSIZE2(i) (D3DFVF_TEXTUREFORMAT2)
#define D3DFVF_TEXCOORDSIZE3(i) (D3DFVF_TEXTUREFORMAT3 << ((i)*2 + 16))
#define D3DFVF_TEXCOORDSIZE4(i) (D3DFVF_TEXTUREFORMAT4 << ((i)*2 + 16))

// Vertex declarations
typedef enum _D3DDECLTYPE {
	D3DDECLTYPE_FLOAT1 = 0, D3DDECLTYPE_FLOAT2 = 1, D3DDECLTYPE_FLOAT3 = 2, D3DDECLTYPE_FLOAT4 = 3,
	D3DDECLTYPE_D3DCOLOR = 4, D3DDECLTYPE_UBYTE4 = 5, D3DDECLTYPE_SHORT2 = 6, D3DDECLTYPE_SHORT4 = 7,
	D3DDECLTYPE_UBYTE4N = 8, D3DDECLTYPE_SHORT2N = 9, D3DDECLTYPE_SHORT4N = 10,
	D3DDECLTYPE_USHORT2N = 11, D3DDECLTYPE_USHORT4N = 12, D3DDECLTYPE_UDEC3 = 13,
	D3DDECLTYPE_DEC3N = 14, D3DDECLTYPE_FLOAT16_2 = 15, D3DDECLTYPE_FLOAT16_4 = 16,
	D3DDECLTYPE_UNUSED = 17
} D3DDECLTYPE;
typedef enum _D3DDECLMETHOD { D3DDECLMETHOD_DEFAULT = 0 } D3DDECLMETHOD;
typedef enum _D3DDECLUSAGE {
	D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_BLENDWEIGHT, D3DDECLUSAGE_BLENDINDICES,
	D3DDECLUSAGE_NORMAL, D3DDECLUSAGE_PSIZE, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TANGENT,
	D3DDECLUSAGE_BINORMAL, D3DDECLUSAGE_TESSFACTOR, D3DDECLUSAGE_POSITIONT, D3DDECLUSAGE_COLOR,
	D3DDECLUSAGE_FOG, D3DDECLUSAGE_DEPTH, D3DDECLUSAGE_SAMPLE
} D3DDECLUSAGE;
typedef struct _D3DVERTEXELEMENT9 { WORD Stream; WORD Offset; BYTE Type; BYTE Method; BYTE Usage; BYTE UsageIndex; } D3DVERTEXELEMENT9;
#define D3DDECL_END() { 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 }

// ---------------------------------------------------------------------------
// Descriptions
// ---------------------------------------------------------------------------
typedef struct _D3DSURFACE_DESC {
	D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool;
	D3DMULTISAMPLE_TYPE MultiSampleType; DWORD MultiSampleQuality; UINT Width; UINT Height;
} D3DSURFACE_DESC;
typedef struct _D3DLOCKED_RECT { INT Pitch; void* pBits; } D3DLOCKED_RECT;
typedef struct _D3DVERTEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; DWORD FVF; } D3DVERTEXBUFFER_DESC;
typedef struct _D3DINDEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; } D3DINDEXBUFFER_DESC;
typedef struct _D3DDISPLAYMODE { UINT Width; UINT Height; UINT RefreshRate; D3DFORMAT Format; } D3DDISPLAYMODE;

typedef struct _D3DPRESENT_PARAMETERS_ {
	UINT BackBufferWidth, BackBufferHeight; D3DFORMAT BackBufferFormat; UINT BackBufferCount;
	D3DMULTISAMPLE_TYPE MultiSampleType; DWORD MultiSampleQuality; D3DSWAPEFFECT SwapEffect;
	HWND hDeviceWindow; BOOL Windowed; BOOL EnableAutoDepthStencil; D3DFORMAT AutoDepthStencilFormat;
	DWORD Flags; UINT FullScreen_RefreshRateInHz; UINT PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct _D3DCAPS9 {
	D3DDEVTYPE DeviceType; UINT AdapterOrdinal; DWORD Caps, Caps2, Caps3, PresentationIntervals;
	DWORD CursorCaps, DevCaps, PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps;
	DWORD AlphaCmpCaps, ShadeCaps, TextureCaps, TextureFilterCaps, CubeTextureFilterCaps;
	DWORD VolumeTextureFilterCaps, TextureAddressCaps, VolumeTextureAddressCaps, LineCaps;
	DWORD MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio;
	DWORD MaxAnisotropy; float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom;
	float ExtentsAdjust; DWORD StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages;
	DWORD MaxSimultaneousTextures, VertexProcessingCaps, MaxActiveLights, MaxUserClipPlanes;
	DWORD MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex; float MaxPointSize;
	DWORD MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride, VertexShaderVersion;
	DWORD MaxVertexShaderConst, PixelShaderVersion; float PixelShader1xMaxValue;
} D3DCAPS9;
typedef D3DCAPS9 D3DCAPS8;

#define D3DPTADDRESSCAPS_WRAP          0x00000001L
#define D3DPTADDRESSCAPS_MIRROR        0x00000002L
#define D3DPTADDRESSCAPS_CLAMP         0x00000004L
#define D3DPTADDRESSCAPS_BORDER        0x00000008L
#define D3DPTADDRESSCAPS_INDEPENDENTUV 0x00000010L
#define D3DPTADDRESSCAPS_MIRRORONCE    0x00000020L

#define D3DVS_VERSION(major, minor) (0xFFFE0000 | ((major) << 8) | (minor))
#define D3DPS_VERSION(major, minor) (0xFFFF0000 | ((major) << 8) | (minor))

typedef struct _D3DADAPTER_IDENTIFIER9 {
	char Driver[512]; char Description[512]; char DeviceName[32];
	LARGE_INTEGER DriverVersion; DWORD VendorId, DeviceId, SubSysId, Revision;
	GUID DeviceIdentifier; DWORD WHQLLevel;
} D3DADAPTER_IDENTIFIER9;

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------
namespace Ps3Gfx { struct Resource; }

class IDirect3DDevice9;

class IDirect3DResource9
{
public:
	IDirect3DResource9(D3DRESOURCETYPE type) : m_refs(1), m_type(type), m_priority(0) {}
	virtual ~IDirect3DResource9() {}
	ULONG AddRef() { return ++m_refs; }
	ULONG Release() { ULONG r = --m_refs; if (r == 0) delete this; return r; }
	D3DRESOURCETYPE GetType() const { return m_type; }
	DWORD SetPriority(DWORD p) { DWORD o = m_priority; m_priority = p; return o; }
	DWORD GetPriority() const { return m_priority; }
	HRESULT GetDevice(IDirect3DDevice9** pp);
	BOOL IsBusy() const { return FALSE; }
	void BlockUntilNotBusy() {}
	void PreLoad() {}
protected:
	ULONG m_refs;
	D3DRESOURCETYPE m_type;
	DWORD m_priority;
};

class IDirect3DTexture9;

class IDirect3DSurface9 : public IDirect3DResource9
{
public:
	IDirect3DSurface9(UINT w, UINT h, D3DFORMAT fmt, DWORD usage, IDirect3DTexture9* owner, UINT level);
	~IDirect3DSurface9();
	HRESULT GetDesc(D3DSURFACE_DESC* d);
	HRESULT LockRect(D3DLOCKED_RECT* r, const RECT* rect, DWORD flags);
	HRESULT UnlockRect();
	HRESULT GetContainer(REFIID, void** pp);

	UINT m_width, m_height;
	D3DFORMAT m_format;
	DWORD m_usage;
	IDirect3DTexture9* m_owner;	// not ref-counted (the texture owns its levels)
	UINT m_level;
	BYTE* m_bits;				// only for stand-alone surfaces; render/depth targets get it on first LockRect
	UINT m_pitch;
	void* m_backend;			// RSX surface state
};

class IDirect3DBaseTexture9 : public IDirect3DResource9
{
public:
	IDirect3DBaseTexture9(D3DRESOURCETYPE t) : IDirect3DResource9(t) {}
	virtual DWORD GetLevelCount() = 0;
	DWORD SetLOD(DWORD) { return 0; }
	DWORD GetLOD() { return 0; }
};

class IDirect3DTexture9 : public IDirect3DBaseTexture9
{
public:
	IDirect3DTexture9(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool);
	~IDirect3DTexture9();
	DWORD GetLevelCount() { return m_levels; }
	HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC* d);
	HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface9** pp);
	HRESULT LockRect(UINT level, D3DLOCKED_RECT* r, const RECT* rect, DWORD flags);
	HRESULT UnlockRect(UINT level);

	// Byte layout of one mip level (DXT formats are 4x4 blocks).
	UINT LevelWidth(UINT level) const;
	UINT LevelHeight(UINT level) const;
	UINT LevelPitch(UINT level) const;
	UINT LevelSize(UINT level) const;
	// NULL while the texture has no main-memory copy (render targets get one only if locked;
	// the RSX backend releases an uploaded texture's copy and LockRect brings it back).
	BYTE* LevelBits(UINT level) const { return m_data ? m_data + m_offsets[level] : NULL; }

	UINT m_width, m_height, m_levels;
	DWORD m_usage;
	D3DFORMAT m_format;
	D3DPOOL m_pool;
	BYTE* m_data;
	UINT m_offsets[16];
	UINT m_size;
	IDirect3DSurface9* m_surfaces[16];
	bool m_dirty;
	void* m_backend;	// RSX copy (created/updated lazily on bind)
	// The main-memory copy's life (Ps3Gfx::RestoreTextureData; changed with the device lock held):
	UINT m_lockDepth;		// LockRect calls not yet unlocked: the copy is in use
	bool m_dataReleased;	// the backend freed m_data after uploading it; LockRect reads it back
	bool m_keepData;		// locked again after a release: the copy stays from now on
};

class IDirect3DCubeTexture9 : public IDirect3DBaseTexture9
{
public:
	IDirect3DCubeTexture9() : IDirect3DBaseTexture9(D3DRTYPE_CUBETEXTURE) {}
	DWORD GetLevelCount() { return 1; }
};

// What a buffer's Lock/Unlock calls changed since its last upload to the RSX: the byte range,
// and whether every lock was D3DLOCK_NOOVERWRITE (the caller promises not to touch data a
// pending draw uses, so the range can be written straight into the RSX copy).
struct Ps3BufferChanges
{
	UINT lockOffset, lockSize;
	DWORD lockFlags;
	UINT lo, hi;
	bool noOverwrite;

	void Reset(UINT size) { lockOffset = 0; lockSize = 0; lockFlags = 0; lo = 0; hi = size; noOverwrite = false; }
	void Locked(UINT offset, UINT size, DWORD flags) { lockOffset = offset; lockSize = size; lockFlags = flags; }
	void Unlocked(UINT total, bool& dirty)
	{
		UINT a = lockOffset, b = lockSize ? lockOffset + lockSize : total;
		if (b > total) b = total;
		bool nooverwrite = (lockFlags & D3DLOCK_NOOVERWRITE) != 0;
		if (lockFlags & D3DLOCK_DISCARD) { a = 0; b = total; nooverwrite = false; }
		if (!dirty) { lo = a; hi = b; noOverwrite = nooverwrite; }
		else { if (a < lo) lo = a; if (b > hi) hi = b; noOverwrite = noOverwrite && nooverwrite; }
		dirty = true;
	}
};

class IDirect3DVertexBuffer9 : public IDirect3DResource9
{
public:
	IDirect3DVertexBuffer9(UINT size, DWORD usage, DWORD fvf, D3DPOOL pool);
	~IDirect3DVertexBuffer9();
	HRESULT Lock(UINT offset, UINT size, void** pp, DWORD flags);
	HRESULT Unlock();
	HRESULT GetDesc(D3DVERTEXBUFFER_DESC* d);
	UINT m_size; DWORD m_usage; DWORD m_fvf; D3DPOOL m_pool;
	BYTE* m_data;			// NULL while released by the backend (static buffers; Lock brings it back)
	bool m_dirty;
	Ps3BufferChanges m_changes;
	void* m_backend;
	// As for textures (Ps3Gfx::RestoreVertexBufferData), for buffers without D3DUSAGE_DYNAMIC.
	UINT m_lockDepth;
	bool m_dataReleased;
	bool m_keepData;
};

class IDirect3DIndexBuffer9 : public IDirect3DResource9
{
public:
	IDirect3DIndexBuffer9(UINT size, DWORD usage, D3DFORMAT fmt, D3DPOOL pool);
	~IDirect3DIndexBuffer9();
	HRESULT Lock(UINT offset, UINT size, void** pp, DWORD flags);
	HRESULT Unlock();
	HRESULT GetDesc(D3DINDEXBUFFER_DESC* d);
	UINT m_size; DWORD m_usage; D3DFORMAT m_format; D3DPOOL m_pool;
	BYTE* m_data;
	bool m_dirty;
	Ps3BufferChanges m_changes;
	void* m_backend;
};

class IDirect3DVertexShader9 : public IDirect3DResource9
{
public:
	IDirect3DVertexShader9(const DWORD* tokens);
	~IDirect3DVertexShader9();
	DWORD* m_tokens;	// D3D shader bytecode (vs_1_1), kept for the RSX translator
	UINT m_numTokens;
	void* m_backend;
};

class IDirect3DPixelShader9 : public IDirect3DResource9
{
public:
	IDirect3DPixelShader9(const DWORD* tokens);
	~IDirect3DPixelShader9();
	DWORD* m_tokens;	// D3D shader bytecode (ps_1_x)
	UINT m_numTokens;
	void* m_backend;
};

class IDirect3DVertexDeclaration9 : public IDirect3DResource9
{
public:
	IDirect3DVertexDeclaration9(const D3DVERTEXELEMENT9* e);
	D3DVERTEXELEMENT9 m_elements[32];
	UINT m_count;
};

class IDirect3DStateBlock9 : public IDirect3DResource9
{
public:
	IDirect3DStateBlock9() : IDirect3DResource9(D3DRTYPE_SURFACE) {}
	HRESULT Capture() { return S_OK; }
	HRESULT Apply() { return S_OK; }
};

class IDirect3DQuery9 : public IDirect3DResource9
{
public:
	IDirect3DQuery9() : IDirect3DResource9(D3DRTYPE_SURFACE) {}
	HRESULT Issue(DWORD) { return S_OK; }
	HRESULT GetData(void* p, DWORD size, DWORD) { if (p && size >= 4) *(DWORD*)p = 1; return S_OK; }
};
typedef enum _D3DQUERYTYPE { D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9 } D3DQUERYTYPE;
#define D3DISSUE_END   (1 << 0)
#define D3DISSUE_BEGIN (1 << 1)
#define D3DGETDATA_FLUSH (1 << 0)

class IDirect3DSwapChain9 : public IDirect3DResource9
{
public:
	IDirect3DSwapChain9() : IDirect3DResource9(D3DRTYPE_SURFACE) {}
};

class IDirect3DDevice9
{
public:
	IDirect3DDevice9(const D3DPRESENT_PARAMETERS* pp);
	~IDirect3DDevice9();
	ULONG AddRef() { return 1; }
	ULONG Release() { return 1; }

	HRESULT TestCooperativeLevel() { return S_OK; }
	UINT GetAvailableTextureMem();
	HRESULT EvictManagedResources() { return S_OK; }
	HRESULT GetDeviceCaps(D3DCAPS9* caps);
	HRESULT GetDisplayMode(UINT, D3DDISPLAYMODE* mode);
	HRESULT Reset(D3DPRESENT_PARAMETERS* pp);
	HRESULT Present(const RECT*, const RECT*, HWND, const void*);
	HRESULT GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** pp);
	void SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP*) {}
	void GetGammaRamp(UINT, D3DGAMMARAMP*) {}

	HRESULT CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** pp, HANDLE* shared);
	HRESULT CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*) { return E_NOTIMPL; }
	HRESULT CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** pp, HANDLE* shared);
	HRESULT CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** pp, HANDLE* shared);
	HRESULT CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9** pp, HANDLE*);
	HRESULT CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9** pp, HANDLE*);
	HRESULT CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL, IDirect3DSurface9** pp, HANDLE*);
	HRESULT UpdateSurface(IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*) { return S_OK; }
	HRESULT UpdateTexture(IDirect3DBaseTexture9*, IDirect3DBaseTexture9*) { return S_OK; }
	HRESULT GetRenderTargetData(IDirect3DSurface9*, IDirect3DSurface9*) { return S_OK; }
	HRESULT StretchRect(IDirect3DSurface9* src, const RECT*, IDirect3DSurface9* dst, const RECT*, D3DTEXTUREFILTERTYPE);
	HRESULT ColorFill(IDirect3DSurface9*, const RECT*, D3DCOLOR) { return S_OK; }

	HRESULT SetRenderTarget(DWORD index, IDirect3DSurface9* s);
	HRESULT GetRenderTarget(DWORD index, IDirect3DSurface9** pp);
	HRESULT SetDepthStencilSurface(IDirect3DSurface9* s);
	HRESULT GetDepthStencilSurface(IDirect3DSurface9** pp);

	HRESULT BeginScene();
	HRESULT EndScene();
	HRESULT Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);

	HRESULT SetTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m);
	HRESULT GetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m);
	HRESULT MultiplyTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) { return S_OK; }
	HRESULT SetViewport(const D3DVIEWPORT9* vp);
	HRESULT GetViewport(D3DVIEWPORT9* vp);
	HRESULT SetMaterial(const D3DMATERIAL9* m);
	HRESULT GetMaterial(D3DMATERIAL9* m);
	HRESULT SetLight(DWORD i, const D3DLIGHT9* l);
	HRESULT GetLight(DWORD i, D3DLIGHT9* l);
	HRESULT LightEnable(DWORD i, BOOL on);
	HRESULT GetLightEnable(DWORD i, BOOL* on);
	HRESULT SetClipPlane(DWORD, const float*) { return S_OK; }

	HRESULT SetRenderState(D3DRENDERSTATETYPE s, DWORD v);
	HRESULT GetRenderState(D3DRENDERSTATETYPE s, DWORD* v);
	HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD v);
	HRESULT GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD* v);
	HRESULT SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE s, DWORD v);
	HRESULT GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE s, DWORD* v);
	HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture9* t);
	HRESULT GetTexture(DWORD stage, IDirect3DBaseTexture9** pp);
	HRESULT SetScissorRect(const RECT* r);
	HRESULT GetScissorRect(RECT* r);
	HRESULT CreateStateBlock(DWORD, IDirect3DStateBlock9** pp) { *pp = new IDirect3DStateBlock9(); return S_OK; }
	HRESULT BeginStateBlock() { return S_OK; }
	HRESULT EndStateBlock(IDirect3DStateBlock9** pp) { *pp = new IDirect3DStateBlock9(); return S_OK; }
	HRESULT ValidateDevice(DWORD* passes) { if (passes) *passes = 1; return S_OK; }
	HRESULT SetSoftwareVertexProcessing(BOOL) { return S_OK; }
	BOOL GetSoftwareVertexProcessing() { return FALSE; }
	HRESULT CreateQuery(D3DQUERYTYPE, IDirect3DQuery9** pp) { if (pp) *pp = new IDirect3DQuery9(); return S_OK; }

	HRESULT CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** pp);
	HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9* d);
	HRESULT GetVertexDeclaration(IDirect3DVertexDeclaration9** pp);
	HRESULT SetFVF(DWORD fvf);
	HRESULT GetFVF(DWORD* fvf);
	HRESULT CreateVertexShader(const DWORD* tokens, IDirect3DVertexShader9** pp);
	HRESULT SetVertexShader(IDirect3DVertexShader9* s);
	HRESULT GetVertexShader(IDirect3DVertexShader9** pp);
	HRESULT SetVertexShaderConstantF(UINT reg, const float* data, UINT count);
	HRESULT GetVertexShaderConstantF(UINT reg, float* data, UINT count);
	HRESULT SetVertexShaderConstantI(UINT, const int*, UINT) { return S_OK; }
	HRESULT SetVertexShaderConstantB(UINT, const BOOL*, UINT) { return S_OK; }
	HRESULT CreatePixelShader(const DWORD* tokens, IDirect3DPixelShader9** pp);
	HRESULT SetPixelShader(IDirect3DPixelShader9* s);
	HRESULT GetPixelShader(IDirect3DPixelShader9** pp);
	HRESULT SetPixelShaderConstantF(UINT reg, const float* data, UINT count);
	HRESULT GetPixelShaderConstantF(UINT reg, float* data, UINT count);
	HRESULT SetPixelShaderConstantI(UINT, const int*, UINT) { return S_OK; }
	HRESULT SetPixelShaderConstantB(UINT, const BOOL*, UINT) { return S_OK; }

	HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride);
	HRESULT GetStreamSource(UINT stream, IDirect3DVertexBuffer9** pp, UINT* offset, UINT* stride);
	HRESULT SetStreamSourceFreq(UINT, UINT) { return S_OK; }
	HRESULT SetIndices(IDirect3DIndexBuffer9* ib);
	HRESULT GetIndices(IDirect3DIndexBuffer9** pp);

	HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count);
	HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVerts, UINT startIndex, UINT primCount);
	HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primCount, const void* data, UINT stride);
	HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVerts, UINT primCount, const void* indices, D3DFORMAT ifmt, const void* data, UINT stride);

	// ---- state (read by the RSX backend when a draw is issued) ----
	D3DPRESENT_PARAMETERS m_pp;
	DWORD m_rs[D3DRS_MAX];
	DWORD m_tss[8][D3DTSS_MAX];
	DWORD m_ss[16][D3DSAMP_MAX];
	IDirect3DBaseTexture9* m_textures[16];
	D3DMATRIX m_transforms[512];
	D3DVIEWPORT9 m_viewport;
	D3DMATERIAL9 m_material;
	D3DLIGHT9 m_lights[8];
	BOOL m_lightEnabled[8];
	RECT m_scissor;
	DWORD m_fvf;
	IDirect3DVertexDeclaration9* m_decl;
	IDirect3DVertexShader9* m_vs;
	IDirect3DPixelShader9* m_ps;
	float m_vsConst[256][4];
	float m_psConst[32][4];
	IDirect3DVertexBuffer9* m_streams[4];
	UINT m_streamOffset[4], m_streamStride[4];
	IDirect3DIndexBuffer9* m_indices;
	IDirect3DSurface9* m_backBuffer;
	IDirect3DSurface9* m_depthBuffer;
	IDirect3DSurface9* m_renderTarget;
	IDirect3DSurface9* m_depthTarget;
	void* m_backend;
};

class IDirect3D9
{
public:
	ULONG AddRef() { return 1; }
	ULONG Release() { return 1; }
	UINT GetAdapterCount() { return 1; }
	HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE* mode);
	UINT GetAdapterModeCount(UINT, D3DFORMAT) { return 1; }
	HRESULT EnumAdapterModes(UINT a, D3DFORMAT, UINT, D3DDISPLAYMODE* mode) { return GetAdapterDisplayMode(a, mode); }
	HRESULT GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9* id);
	HRESULT CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) { return S_OK; }
	HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) { return S_OK; }
	HRESULT GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9* caps);
	HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev);
};

#define D3D_SDK_VERSION 32
IDirect3D9* Direct3DCreate9(UINT sdk);

// COM-style pointer typedefs
typedef IDirect3D9*                  LPDIRECT3D9;
typedef IDirect3DDevice9*            LPDIRECT3DDEVICE9;
typedef IDirect3DResource9*          LPDIRECT3DRESOURCE9;
typedef IDirect3DBaseTexture9*       LPDIRECT3DBASETEXTURE9;
typedef IDirect3DTexture9*           LPDIRECT3DTEXTURE9;
typedef IDirect3DCubeTexture9*       LPDIRECT3DCUBETEXTURE9;
typedef IDirect3DSurface9*           LPDIRECT3DSURFACE9;
typedef IDirect3DVertexBuffer9*      LPDIRECT3DVERTEXBUFFER9;
typedef IDirect3DIndexBuffer9*       LPDIRECT3DINDEXBUFFER9;
typedef IDirect3DVertexShader9*      LPDIRECT3DVERTEXSHADER9;
typedef IDirect3DPixelShader9*       LPDIRECT3DPIXELSHADER9;
typedef IDirect3DVertexDeclaration9* LPDIRECT3DVERTEXDECLARATION9;
typedef IDirect3DStateBlock9*        LPDIRECT3DSTATEBLOCK9;
typedef IDirect3DQuery9*             LPDIRECT3DQUERY9;
typedef IDirect3DSwapChain9*         LPDIRECT3DSWAPCHAIN9;

// The renderer still names a few Xbox (D3D8-era) types; on PS3 they are the D3D9 ones.
typedef IDirect3DDevice9       IDirect3DDevice8;
typedef IDirect3DTexture9      IDirect3DTexture8;
typedef IDirect3DSurface9      IDirect3DSurface8;
typedef IDirect3DVertexBuffer9 IDirect3DVertexBuffer8;
typedef IDirect3DIndexBuffer9  IDirect3DIndexBuffer8;
typedef IDirect3DBaseTexture9  IDirect3DBaseTexture8;
typedef IDirect3DTexture9*     LPDIRECT3DTEXTURE8;
typedef IDirect3DSurface9*     LPDIRECT3DSURFACE8;
typedef IDirect3DBaseTexture9* LPDIRECT3DBASETEXTURE8;
typedef IDirect3DVertexBuffer9* LPDIRECT3DVERTEXBUFFER8;
class IDirect3DPalette8 : public IDirect3DResource9
{
public:
	IDirect3DPalette8() : IDirect3DResource9(D3DRTYPE_SURFACE) { memset(m_entries, 0, sizeof(m_entries)); }
	HRESULT Lock(D3DCOLOR** pp, DWORD) { *pp = m_entries; return S_OK; }
	HRESULT Unlock() { return S_OK; }
	D3DCOLOR m_entries[256];
};
typedef IDirect3DPalette8* LPDIRECT3DPALETTE8;

#endif // PS3_D3D9_H
