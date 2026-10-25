// A small interpreter for the NV40 assembly rsx_shadergen writes ("!!VP2.0" / "!!ARBfp1.0",
// the subset it uses), so the generated programs' maths can be checked on a PC. It runs
// the assembly text, not cgcomp's microcode: it checks the generators, while run.sh
// checks the assembler against cgcomp.
#ifndef RSX_INTERP_H
#define RSX_INTERP_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <functional>

struct Vec4 { float v[4]; float& operator[](int i) { return v[i]; } float operator[](int i) const { return v[i]; } };

class NvInterp
{
public:
	// Inputs and outputs, by name ("v[3]", "fragment.texcoord[1]", "result.color"...).
	Vec4 c[64];
	Vec4 in[32];		// VP: v[0..15]; FP: see InputIndex()
	Vec4 out[24];		// see OutputIndex()
	std::function<Vec4(int unit, const Vec4& coord, bool projected)> sample;
	std::string error;

	explicit NvInterp(const std::string& program) { memset(c, 0, sizeof(c)); Parse(program); }

	bool Run()
	{
		memset(R, 0, sizeof(R));
		memset(out, 0, sizeof(out));
		for (size_t i = 0; i < m_code.size() && error.empty(); ++i) Exec(m_code[i]);
		return error.empty();
	}

	static int InputIndex(const std::string& name)
	{
		if (name.compare(0, 2, "v[") == 0) return atoi(name.c_str() + 2);
		if (name == "fragment.color.primary" || name == "fragment.color") return 16;
		if (name == "fragment.color.secondary") return 17;
		if (name == "fragment.fogcoord") return 18;
		if (name.compare(0, 18, "fragment.texcoord[") == 0) return 20 + atoi(name.c_str() + 18);
		return -1;
	}

	static int OutputIndex(const std::string& name)
	{
		if (name == "result.position") return 0;
		if (name == "result.color" || name == "result.color.primary") return 1;
		if (name == "result.color.secondary") return 2;
		if (name == "result.fogcoord") return 3;
		if (name == "result.pointsize") return 4;
		if (name.compare(0, 16, "result.texcoord[") == 0) return 8 + atoi(name.c_str() + 16);
		return -1;
	}

private:
	enum Kind { kTemp, kConst, kInput, kOutput };
	struct Operand { Kind kind; int index; int swz[4]; bool neg, abs; int mask; };
	struct Insn { std::string op; bool sat; Operand dst; Operand src[3]; int nsrc; int texUnit; };

	Vec4 R[32];
	std::vector<Insn> m_code;

	static std::string Trim(const std::string& s)
	{
		size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
		return a == std::string::npos ? "" : s.substr(a, b - a + 1);
	}

	bool ParseOperand(std::string s, Operand& o, bool dest)
	{
		s = Trim(s);
		o.neg = o.abs = false;
		o.mask = 0xF;
		for (int i = 0; i < 4; ++i) o.swz[i] = i;
		if (!s.empty() && s[0] == '-') { o.neg = true; s = Trim(s.substr(1)); }
		if (!s.empty() && s[0] == '|') { o.abs = true; s = s.substr(1, s.size() - 2); }
		// Split off a trailing swizzle / mask.
		std::string reg = s, swz;
		size_t dot = s.find_last_of('.');
		if (dot != std::string::npos)
		{
			bool isSwz = dot + 1 < s.size();
			for (size_t k = dot + 1; k < s.size(); ++k) if (!strchr("xyzw", s[k])) isSwz = false;
			if (isSwz) { reg = s.substr(0, dot); swz = s.substr(dot + 1); }
		}
		if (reg[0] == 'R' || reg[0] == 'H') { o.kind = kTemp; o.index = atoi(reg.c_str() + 1); }
		else if (reg.compare(0, 2, "c[") == 0) { o.kind = kConst; o.index = atoi(reg.c_str() + 2); }
		else if (OutputIndex(reg) >= 0) { o.kind = kOutput; o.index = OutputIndex(reg); }
		else if (InputIndex(reg) >= 0) { o.kind = kInput; o.index = InputIndex(reg); }
		else { error = "unknown register " + reg; return false; }
		if (dest)
		{
			if (!swz.empty()) { o.mask = 0; for (size_t k = 0; k < swz.size(); ++k) o.mask |= 1 << (strchr("xyzw", swz[k]) - "xyzw"); }
		}
		else if (!swz.empty())
		{
			for (int i = 0; i < 4; ++i)
			{
				char ch = swz[i < (int)swz.size() ? i : swz.size() - 1];
				o.swz[i] = strchr("xyzw", ch) - "xyzw";
			}
		}
		return true;
	}

	void Parse(const std::string& text)
	{
		size_t pos = 0;
		while (pos < text.size() && error.empty())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) end = text.size();
			std::string line = Trim(text.substr(pos, end - pos));
			pos = end + 1;
			if (line.empty() || line[0] == '!') continue;
			if (line.compare(0, 6, "#const") == 0)
			{
				int idx = atoi(line.c_str() + line.find('[') + 1);
				float a, b, cc, d;
				if (sscanf(line.c_str() + line.find('=') + 1, "%f %f %f %f", &a, &b, &cc, &d) == 4)
				{ c[idx][0] = a; c[idx][1] = b; c[idx][2] = cc; c[idx][3] = d; }
				continue;
			}
			if (line[0] == '#' || line == "END") continue;
			if (line[line.size() - 1] == ';') line.erase(line.size() - 1);
			size_t sp = line.find(' ');
			Insn in;
			in.op = line.substr(0, sp);
			in.sat = false;
			in.texUnit = -1;
			size_t us = in.op.find("_SAT");
			if (us != std::string::npos) { in.sat = true; in.op = in.op.substr(0, us); }
			std::vector<std::string> args;
			std::string rest = line.substr(sp + 1), cur;
			for (size_t i = 0; i < rest.size(); ++i)
			{
				if (rest[i] == ',') { args.push_back(cur); cur.clear(); }
				else cur += rest[i];
			}
			args.push_back(cur);
			if (in.op == "TEX" || in.op == "TXP")
			{
				// TEX dst, coord, texture[n], 2D
				ParseOperand(args[0], in.dst, true);
				ParseOperand(args[1], in.src[0], false);
				in.texUnit = atoi(args[2].c_str() + args[2].find('[') + 1);
				in.nsrc = 1;
			}
			else
			{
				ParseOperand(args[0], in.dst, true);
				in.nsrc = (int)args.size() - 1;
				for (int i = 0; i < in.nsrc && i < 3; ++i) ParseOperand(args[i + 1], in.src[i], false);
			}
			m_code.push_back(in);
		}
	}

	Vec4 Read(const Operand& o)
	{
		Vec4 r, src;
		switch (o.kind)
		{
		case kTemp: src = R[o.index]; break;
		case kConst: src = c[o.index]; break;
		case kInput: src = in[o.index]; break;
		default: src = out[o.index]; break;
		}
		for (int i = 0; i < 4; ++i)
		{
			float x = src[o.swz[i]];
			if (o.abs) x = fabsf(x);
			if (o.neg) x = -x;
			r[i] = x;
		}
		return r;
	}

	void Write(const Operand& o, Vec4 v, bool sat)
	{
		Vec4* d = o.kind == kTemp ? &R[o.index] : o.kind == kOutput ? &out[o.index] : NULL;
		if (!d) { error = "write to a read-only register"; return; }
		for (int i = 0; i < 4; ++i)
		{
			if (!(o.mask & (1 << i))) continue;
			float x = v[i];
			if (sat) x = x < 0 ? 0 : x > 1 ? 1 : x;
			(*d)[i] = x;
		}
	}

	void Exec(const Insn& in)
	{
		Vec4 a = in.nsrc > 0 ? Read(in.src[0]) : Vec4(), b = in.nsrc > 1 ? Read(in.src[1]) : Vec4(), cc = in.nsrc > 2 ? Read(in.src[2]) : Vec4(), r;
		const std::string& op = in.op;
		if (op == "MOV") r = a;
		else if (op == "ADD") for (int i = 0; i < 4; ++i) r[i] = a[i] + b[i];
		else if (op == "MUL") for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i];
		else if (op == "MAD") for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i] + cc[i];
		else if (op == "MAX") for (int i = 0; i < 4; ++i) r[i] = a[i] > b[i] ? a[i] : b[i];
		else if (op == "MIN") for (int i = 0; i < 4; ++i) r[i] = a[i] < b[i] ? a[i] : b[i];
		else if (op == "SGE") for (int i = 0; i < 4; ++i) r[i] = a[i] >= b[i] ? 1.0f : 0.0f;
		else if (op == "SLT") for (int i = 0; i < 4; ++i) r[i] = a[i] < b[i] ? 1.0f : 0.0f;
		else if (op == "LRP") for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i] + (1 - a[i]) * cc[i];
		else if (op == "CMP") for (int i = 0; i < 4; ++i) r[i] = a[i] < 0 ? b[i] : cc[i];
		else if (op == "DP3") { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; for (int i = 0; i < 4; ++i) r[i] = d; }
		else if (op == "DP4") { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; for (int i = 0; i < 4; ++i) r[i] = d; }
		else if (op == "RCP") for (int i = 0; i < 4; ++i) r[i] = 1.0f / a[0];
		else if (op == "RSQ") for (int i = 0; i < 4; ++i) r[i] = 1.0f / sqrtf(fabsf(a[0]));
		else if (op == "POW") for (int i = 0; i < 4; ++i) r[i] = powf(a[0], b[0]);
		else if (op == "TEX" || op == "TXP")
		{
			if (!sample) { error = "no sampler"; return; }
			r = sample(in.texUnit, a, op == "TXP");
		}
		else { error = "unsupported op " + op; return; }
		Write(in.dst, r, in.sat);
	}
};

#endif // RSX_INTERP_H
