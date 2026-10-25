// THUG PS3: NV40 shader assembly at run time (see rsx_asm.h).
//
// The output code is cgcomp's compileVP() / compileFP() (PSL1GHT tools/cgcomp/source/
// main.cpp, MIT, Copyright (c) 2011 PSL1GHT Development Team), writing into a vector
// instead of a file. It must stay byte-identical to cgcomp -a: tools/rsx-asm-test
// checks that on the build host.
//
// This file includes cgcomp's types.h (which declares its own rsxVertexProgram) and so
// must not include PSL1GHT's rsx headers.

#include "rsx_asm.h"

#include "cgcomp/types.h"
#include "cgcomp/fpparser.h"
#include "cgcomp/vpparser.h"
#include "cgcomp/compilervp.h"
#include "cgcomp/compilerfp.h"

#include <list>

#if defined(__BIG_ENDIAN__) || defined(__PPU__)
#define SWAP16(v) (v)
#define SWAP32(v) (v)
#else
#define SWAP16(v) ((u16)(((v)>>8)|((v)<<8)))
#define SWAP32(v) ((((u32)(v))>>24)|(((u32)(v))<<24)|((((u32)(v))&0xFF00)<<8)|((((u32)(v))&0xFF0000)>>8))
#endif

namespace RsxAsm
{

static u32 endian_fp(u32 v)
{
	return (((v >> 16) & 0xffff) << 0) | (((v >> 0) & 0xffff) << 16);
}

static void Pad(std::vector<unsigned char>& b, u32& lastoff, u32 align)
{
	while (lastoff & (align - 1)) b[lastoff++] = 0;
}

bool AssembleVertexProgram(const char* prg, std::vector<unsigned char>& out, std::string* error)
{
	try
	{
		CVPParser parser;
		CCompilerVP compiler;
		parser.Parse(prg);
		compiler.Compile(&parser);

		struct vertex_program_exec* vpi = compiler.GetInstructions();
		std::list<struct nvfx_relocation> branch_reloc = compiler.GetBranchRelocations();
		for (std::list<struct nvfx_relocation>::iterator it = branch_reloc.begin(); it != branch_reloc.end(); it++)
		{
			struct vertex_program_exec* vpe = &vpi[it->location];
			vpe->data[3] &= ~NV40_VP_INST_IADDRL_MASK;
			vpe->data[3] |= (it->target & 7) << NV40_VP_INST_IADDRL_SHIFT;
			vpe->data[2] &= ~NV40_VP_INST_IADDRH_MASK;
			vpe->data[2] |= ((it->target >> 3) & 0x3f) << NV40_VP_INST_IADDRH_SHIFT;
		}

		std::list<struct nvfx_relocation> const_reloc = compiler.GetConstRelocations();
		for (std::list<struct nvfx_relocation>::iterator it = const_reloc.begin(); it != const_reloc.end(); it++)
		{
			struct vertex_program_exec* vpe = &vpi[it->location];
			vpe->data[1] &= ~NVFX_VP(INST_CONST_SRC_MASK);
			vpe->data[1] |= (it->target) << NVFX_VP(INST_CONST_SRC_SHIFT);
		}

		int n, i;
		u16 magic = ('V' << 8) | 'P';
		u32 lastoff = sizeof(rsxVertexProgram);
		std::vector<unsigned char> b(64 * 1024 + compiler.GetInstructionCount() * 16, 0);
		unsigned char* vertexprogram = &b[0];
		rsxVertexProgram* vp = (rsxVertexProgram*)vertexprogram;

		vp->magic = SWAP16(magic);
		vp->insn_start = SWAP16(0);
		vp->const_start = SWAP16(0);
		vp->num_regs = SWAP16(compiler.GetNumRegs());
		vp->input_mask = SWAP32(compiler.GetInputMask());
		vp->output_mask = SWAP32(compiler.GetOutputMask());

		Pad(b, lastoff, 4);
		rsxProgramAttrib* attribs = (rsxProgramAttrib*)(vertexprogram + lastoff);
		vp->attr_off = SWAP32(lastoff);

		n = 0;
		std::list<param> params = parser.GetParameters();
		for (std::list<param>::iterator it = params.begin(); it != params.end(); it++)
		{
			if (!it->is_const)
			{
				it->user = lastoff + (n * sizeof(rsxProgramAttrib));
				attribs[n].index = SWAP32(it->index);
				attribs[n].name_off = SWAP32(0);
				attribs[n].type = it->type;
				n++;
			}
		}
		vp->num_attr = SWAP16(n);
		lastoff += (n * sizeof(rsxProgramAttrib));

		Pad(b, lastoff, 4);
		rsxProgramConst* consts = (rsxProgramConst*)(vertexprogram + lastoff);
		vp->const_off = SWAP32(lastoff);

		n = 0;
		for (std::list<param>::iterator it = params.begin(); it != params.end(); it++)
		{
			if (it->is_const)
			{
				it->user = lastoff + (n * sizeof(rsxProgramConst));
				consts[n].count = it->count;
				consts[n].type = it->type;
				consts[n].is_internal = it->is_internal;
				consts[n].name_off = SWAP32(0);
				for (i = 0; i < (s32)it->count; i++)
				{
					f32* pVal = it->values[i];
					consts[n].index = SWAP32(it->index + i);
					for (int c = 0; c < 4; ++c)
					{
						consts[n].values[c].f = pVal[c];
						consts[n].values[c].u = SWAP32(consts[n].values[c].u);
					}
					n++;
				}
			}
		}
		vp->num_const = SWAP16(n);
		lastoff += (n * sizeof(rsxProgramConst));

		Pad(b, lastoff, 4);
		for (std::list<param>::iterator it = params.begin(); it != params.end(); it++)
		{
			if (!it->name.empty() && !it->is_internal)
			{
				const char* name = it->name.c_str();
				int off = lastoff;
				while (*name) vertexprogram[lastoff++] = *name++;
				vertexprogram[lastoff++] = 0;
				*((u32*)(vertexprogram + it->user)) = SWAP32(off);
			}
		}

		Pad(b, lastoff, 16);
		vp->ucode_off = SWAP32(lastoff);
		vp->num_insn = SWAP16(compiler.GetInstructionCount());

		u32* dstcodeptr = (u32*)(vertexprogram + lastoff);
		for (i = 0, n = 0; i < compiler.GetInstructionCount(); i++, n += 4, lastoff += 16)
		{
			dstcodeptr[n + 0] = SWAP32(vpi[i].data[0]);
			dstcodeptr[n + 1] = SWAP32(vpi[i].data[1]);
			dstcodeptr[n + 2] = SWAP32(vpi[i].data[2]);
			dstcodeptr[n + 3] = SWAP32(vpi[i].data[3]);
		}

		b.resize(lastoff);
		out.swap(b);
		return true;
	}
	catch (...)
	{
		if (error) *error = "vertex program rejected by the assembler";
		return false;
	}
}

bool AssembleFragmentProgram(const char* prg, std::vector<unsigned char>& out, std::string* error)
{
	try
	{
		CFPParser parser;
		CCompilerFP compiler;
		parser.Parse(prg);
		compiler.Compile(&parser);

		int n, i;
		u16 magic = ('F' << 8) | 'P';
		u32 lastoff = sizeof(rsxFragmentProgram);
		std::vector<unsigned char> b(64 * 1024 + compiler.GetInstructionCount() * 16, 0);
		unsigned char* fragmentprogram = &b[0];
		rsxFragmentProgram* fp = (rsxFragmentProgram*)fragmentprogram;

		fp->magic = SWAP16(magic);
		fp->num_regs = SWAP16(compiler.GetNumRegs());
		fp->fp_control = SWAP32(compiler.GetFPControl());
		fp->texcoords = SWAP16(compiler.GetTexcoords());
		fp->texcoord2D = SWAP16(compiler.GetTexcoord2D());
		fp->texcoord3D = SWAP16(compiler.GetTexcoord3D());

		Pad(b, lastoff, 4);
		fp->attr_off = SWAP32(lastoff);
		rsxProgramAttrib* attribs = (rsxProgramAttrib*)(fragmentprogram + lastoff);

		n = 0;
		std::list<param> params = parser.GetParameters();
		for (std::list<param>::iterator it = params.begin(); it != params.end(); it++)
		{
			if (!it->is_const)
			{
				it->user = lastoff + (n * sizeof(rsxProgramAttrib));
				attribs[n].index = SWAP32(it->index);
				attribs[n].name_off = SWAP32(0);
				attribs[n].type = it->type;
				n++;
			}
		}
		fp->num_attr = SWAP16(n);
		lastoff += (n * sizeof(rsxProgramAttrib));

		Pad(b, lastoff, 4);
		std::list<struct fragment_program_data> const_relocs = compiler.GetConstRelocations();
		for (std::list<param>::iterator it = params.begin(); it != params.end(); it++)
		{
			const param& p = *it;
			if (p.is_const && !p.is_internal)
			{
				for (i = 0; i < p.count; i++)
				{
					s32 k = 0;
					rsxConstOffsetTable* const_table = (rsxConstOffsetTable*)(fragmentprogram + lastoff);
					const_table->num = SWAP32(0);
					for (std::list<struct fragment_program_data>::iterator d = const_relocs.begin(); d != const_relocs.end(); d++)
					{
						if (d->index == (p.index + i))
						{
							u32 offs = (d->offset * 16);
							const_table->offset[k++] = SWAP32(offs);
							d->user = lastoff;
						}
					}
					const_table->num = SWAP32(k);
					lastoff += (4 * k + 4);
				}
			}
		}

		Pad(b, lastoff, 4);
		fp->const_off = SWAP32(lastoff);
		rsxProgramConst* consts = (rsxProgramConst*)(fragmentprogram + lastoff);

		n = 0;
		for (std::list<param>::iterator it = params.begin(); it != params.end(); it++)
		{
			if (it->is_const && !it->is_internal)
			{
				it->user = lastoff + (n * sizeof(rsxProgramConst));
				consts[n].count = it->count;
				consts[n].type = it->type;
				consts[n].is_internal = it->is_internal;
				consts[n].name_off = SWAP32(0);
				for (i = 0; i < it->count; i++)
				{
					s32 table_off = -1;
					for (std::list<struct fragment_program_data>::iterator d = const_relocs.begin(); d != const_relocs.end(); d++)
					{
						if (d->index == (it->index + i))
						{
							table_off = d->user;
							break;
						}
					}
					f32* pVal = it->values[i];
					consts[n].index = SWAP32(table_off);
					for (int c = 0; c < 4; ++c)
					{
						consts[n].values[c].f = pVal[c];
						consts[n].values[c].u = SWAP32(consts[n].values[c].u);
					}
					n++;
				}
			}
		}
		fp->num_const = SWAP16(n);
		lastoff += (n * sizeof(rsxProgramConst));

		Pad(b, lastoff, 4);
		for (std::list<param>::iterator it = params.begin(); it != params.end(); it++)
		{
			if (!it->name.empty() && !it->is_internal)
			{
				const char* name = it->name.c_str();
				int off = lastoff;
				while (*name) fragmentprogram[lastoff++] = *name++;
				fragmentprogram[lastoff++] = 0;
				*((u32*)(fragmentprogram + it->user)) = SWAP32(off);
			}
		}

		Pad(b, lastoff, 16);
		fp->ucode_off = SWAP32(lastoff);
		fp->num_insn = SWAP16(compiler.GetInstructionCount());

		struct fragment_program_exec* fpi = compiler.GetInstructions();
		u32* dstcodeptr = (u32*)(fragmentprogram + lastoff);
		for (i = 0, n = 0; i < compiler.GetInstructionCount(); i++, n += 4, lastoff += 16)
		{
			dstcodeptr[n + 0] = endian_fp(SWAP32(fpi[i].data[0]));
			dstcodeptr[n + 1] = endian_fp(SWAP32(fpi[i].data[1]));
			dstcodeptr[n + 2] = endian_fp(SWAP32(fpi[i].data[2]));
			dstcodeptr[n + 3] = endian_fp(SWAP32(fpi[i].data[3]));
		}

		b.resize(lastoff);
		out.swap(b);
		return true;
	}
	catch (...)
	{
		if (error) *error = "fragment program rejected by the assembler";
		return false;
	}
}

} // namespace RsxAsm
