// Host check for Code/Ps3/rsx/rsx_asm.cpp: assembles each .vasm/.fasm given on the
// command line and writes NAME.vpo / NAME.fpo next to OUTDIR, for comparison with
// PSL1GHT's cgcomp -a. Built and run by tools/rsx-asm-test/run.sh.
#include "../../Code/Ps3/rsx/rsx_asm.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string ReadAll(const char* path)
{
	std::string s;
	FILE* f = fopen(path, "rb");
	if (!f) return s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
	fclose(f);
	return s;
}

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s OUTDIR FILE.vasm|FILE.fasm...\n", argv[0]); return 2; }
	int failures = 0;
	for (int i = 2; i < argc; ++i)
	{
		std::string src = ReadAll(argv[i]);
		std::string name = argv[i];
		bool vp = name.size() > 5 && name.compare(name.size() - 5, 5, ".vasm") == 0;
		std::vector<unsigned char> out;
		std::string err;
		bool ok = vp ? RsxAsm::AssembleVertexProgram(src.c_str(), out, &err) : RsxAsm::AssembleFragmentProgram(src.c_str(), out, &err);
		if (!ok) { printf("FAIL %s: %s\n", argv[i], err.c_str()); ++failures; continue; }
		std::string base = name.substr(name.find_last_of('/') + 1);
		base = base.substr(0, base.size() - 5);
		std::string dst = std::string(argv[1]) + "/" + base + (vp ? ".vpo" : ".fpo");
		FILE* f = fopen(dst.c_str(), "wb");
		fwrite(out.data(), 1, out.size(), f);
		fclose(f);
	}
	return failures ? 1 : 0;
}
