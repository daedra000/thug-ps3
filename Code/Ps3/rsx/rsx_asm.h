// THUG PS3: NV40 shader assembly at run time.
//
// The game makes most of its shaders while it runs (fixed-function state, ps_1_1
// source assembled by D3DXAssembleShader), so they can't all be built offline. This
// wraps PSL1GHT's cgcomp assembler (Code/Ps3/rsx/cgcomp, MIT) in "-a" mode: it takes
// the same "!!VP2.0" / "!!ARBfp1.0" assembly the GridSiege renderers feed cgcomp
// offline and returns the same bytes cgcomp writes to a .vpo / .fpo file, which
// PSL1GHT loads as rsxVertexProgram / rsxFragmentProgram.
#ifndef PS3_RSX_ASM_H
#define PS3_RSX_ASM_H

#include <string>
#include <vector>

namespace RsxAsm
{
	// On failure returns false and, if `error` is given, says why (cgcomp's own
	// messages also go to stderr, i.e. the log).
	bool AssembleVertexProgram(const char* source, std::vector<unsigned char>& out, std::string* error = 0);
	bool AssembleFragmentProgram(const char* source, std::vector<unsigned char>& out, std::string* error = 0);
}

#endif // PS3_RSX_ASM_H
