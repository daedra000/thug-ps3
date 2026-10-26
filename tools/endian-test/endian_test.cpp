// Checks Code/Core/endian.h (the little-endian PC data -> host helpers every loader uses)
// on the host it is built for. tools/endian-test/run.sh builds it for big-endian powerpc64
// (run under qemu-user, as on the PS3: -D__CPU_BIG_ENDIAN__=1) and for x86 with
// ASan/UBSan, where the helpers must compile away to plain loads.
//
// Every expected value is written as little-endian bytes, the way the PC files store it,
// so the test doesn't trust the code it checks to produce its own references.
#include <Core/endian.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                                                        \
	do {                                                                        \
		++s_checks;                                                             \
		if (!(cond)) {                                                          \
			++s_failures;                                                       \
			printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);              \
			printf(__VA_ARGS__);                                                \
			printf("\n");                                                       \
		}                                                                       \
	} while (0)

// A deterministic generator: the same values on every host.
static uint64_t s_rng = 0x9e3779b97f4a7c15ull;
static uint64_t Next()
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return s_rng;
}

// The little-endian bytes of a value, built from its numeric value (not memory).
static void PutLe(uint8_t* p, uint64_t v, int bytes)
{
	for (int i = 0; i < bytes; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t ValueFromHostMemory(const void* p, int bytes)
{
	// What the host CPU reads from memory, assembled numerically for comparison.
	uint8_t b[8];
	memcpy(b, p, bytes);
	uint64_t v = 0;
	uint16_t probe = 1;
	const bool little = *(const uint8_t*)&probe == 1;
	for (int i = 0; i < bytes; ++i)
		v |= (uint64_t)b[little ? i : bytes - 1 - i] << (8 * i);
	return v;
}

static void TestHostMatchesBuild()
{
	uint16_t probe = 1;
	const bool little = *(const uint8_t*)&probe == 1;
	// The build flag (-D__CPU_BIG_ENDIAN__=1 on the PS3) must say what the CPU does.
	CHECK((Endian::kHostIsLittle != 0) == little, "kHostIsLittle=%d, CPU is %s-endian",
		(int)Endian::kHostIsLittle, little ? "little" : "big");
#if defined(__BYTE_ORDER__)
	CHECK(little == (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__), "__BYTE_ORDER__ disagrees");
#endif
	CHECK(sizeof(unsigned short) == 2 && sizeof(unsigned int) == 4 && sizeof(unsigned long long) == 8 && sizeof(float) == 4,
		"type sizes");
}

static void TestSwaps()
{
	CHECK(Endian::Swap16(0x1234) == 0x3412, "0x%04x", Endian::Swap16(0x1234));
	CHECK(Endian::Swap32(0x12345678u) == 0x78563412u, "0x%08x", Endian::Swap32(0x12345678u));
	CHECK(Endian::Swap64(0x0102030405060708ull) == 0x0807060504030201ull, "0x%016llx",
		(unsigned long long)Endian::Swap64(0x0102030405060708ull));
	CHECK(Endian::Swap16(0xff00) == 0x00ff && Endian::Swap32(0x80000000u) == 0x80u, "sign bits");
	for (int i = 0; i < 100000; ++i)
	{
		const uint64_t v = Next();
		CHECK(Endian::Swap16(Endian::Swap16((unsigned short)v)) == (unsigned short)v, "16 round trip");
		CHECK(Endian::Swap32(Endian::Swap32((unsigned int)v)) == (unsigned int)v, "32 round trip");
		CHECK(Endian::Swap64(Endian::Swap64(v)) == v, "64 round trip");
		CHECK(Endian::Le16(Endian::Le16((unsigned short)v)) == (unsigned short)v, "Le16 round trip");
		CHECK(Endian::Le32(Endian::Le32((unsigned int)v)) == (unsigned int)v, "Le32 round trip");
		CHECK(Endian::Le64(Endian::Le64(v)) == v, "Le64 round trip");
		if (s_failures > 20) return;
	}
}

static void TestReads()
{
	// Every alignment: the loaders read fields at any offset inside a file.
	uint8_t buf[32];
	for (int i = 0; i < 20000; ++i)
	{
		const uint64_t v = Next();
		const int at = (int)(Next() % 16);
		memset(buf, 0xcd, sizeof(buf));
		PutLe(buf + at, v, 4);
		CHECK(Endian::ReadLe32(buf + at) == (uint32_t)v, "ReadLe32 at %d: 0x%08x vs 0x%08x", at, Endian::ReadLe32(buf + at), (uint32_t)v);
		CHECK(Endian::ReadLeS32(buf + at) == (int32_t)(uint32_t)v, "ReadLeS32");
		PutLe(buf + at, v, 2);
		CHECK(Endian::ReadLe16(buf + at) == (uint16_t)v, "ReadLe16 at %d", at);
		CHECK(Endian::ReadLeS16(buf + at) == (int16_t)(uint16_t)v, "ReadLeS16 at %d", at);
		// Le32 of what the host loads from little-endian bytes is the value.
		PutLe(buf, v, 4);
		unsigned int raw;
		memcpy(&raw, buf, 4);
		CHECK(Endian::Le32(raw) == (uint32_t)v, "Le32 of loaded bytes");
		PutLe(buf, v, 8);
		unsigned long long raw64;
		memcpy(&raw64, buf, 8);
		CHECK(Endian::Le64(raw64) == v, "Le64 of loaded bytes");
		if (s_failures > 20) return;
	}
	// The bytes around the value are never touched (reads only).
	for (size_t k = 20; k < sizeof(buf); ++k) CHECK(buf[k] == 0xcd, "byte %u changed", (unsigned)k);

	// Floats: 1.5f is 0x3fc00000, stored as 00 00 c0 3f.
	const uint8_t onePointFive[] = { 0x00, 0x00, 0xc0, 0x3f };
	CHECK(Endian::ReadLeF(onePointFive) == 1.5f, "ReadLeF(1.5) = %g", Endian::ReadLeF(onePointFive));
	const uint8_t minusTwo[] = { 0x00, 0x00, 0x00, 0xc0 };
	CHECK(Endian::ReadLeF(minusTwo) == -2.0f, "ReadLeF(-2) = %g", Endian::ReadLeF(minusTwo));
	float f;
	memcpy(&f, onePointFive, 4);
	CHECK(Endian::LeF(f) == 1.5f, "LeF");
	CHECK(Endian::LeF(Endian::LeF(f)) == f || isnan(f), "LeF round trip");
}

template<typename T> static void TestFixOne(uint64_t v)
{
	uint8_t le[8];
	PutLe(le, v, sizeof(T));
	T value;
	memcpy(&value, le, sizeof(T));
	Endian::Fix(value);
	// Now the host's own representation of the little-endian value.
	CHECK(ValueFromHostMemory(&value, sizeof(T)) == (sizeof(T) == 8 ? v : v & ((1ull << (8 * sizeof(T))) - 1)),
		"Fix<%u bytes>", (unsigned)sizeof(T));
	// Fixing it again gives back the file bytes (the swap is its own inverse).
	Endian::Fix(value);
	CHECK(memcmp(&value, le, sizeof(T)) == 0, "Fix twice <%u bytes>", (unsigned)sizeof(T));
}

static void TestFix()
{
	for (int i = 0; i < 20000; ++i)
	{
		const uint64_t v = Next();
		TestFixOne<uint8_t>(v);
		TestFixOne<int16_t>(v);
		TestFixOne<uint16_t>(v);
		TestFixOne<int32_t>(v);
		TestFixOne<uint32_t>(v);
		TestFixOne<uint64_t>(v);
		TestFixOne<int64_t>(v);
		if (s_failures > 20) return;
	}
	// Floats through Fix: the file's 1.5f becomes the host's 1.5f.
	const uint8_t onePointFive[] = { 0x00, 0x00, 0xc0, 0x3f };
	float f;
	memcpy(&f, onePointFive, 4);
	Endian::Fix(f);
	CHECK(f == 1.5f, "Fix(float) = %g", f);
	const uint8_t dbl[] = { 0, 0, 0, 0, 0, 0, 0xf8, 0x3f };	// 1.5 as a double
	double d;
	memcpy(&d, dbl, 8);
	Endian::Fix(d);
	CHECK(d == 1.5, "Fix(double) = %g", d);
}

static void TestArrays()
{
	enum { N = 37 };
	// Canaries either side catch a fixer that runs past its count.
	uint8_t mem[8 + N * 4 + 8];
	for (int pass = 0; pass < 200; ++pass)
	{
		const size_t count = (size_t)(Next() % N);
		uint32_t expect32[N];
		memset(mem, 0xa5, sizeof(mem));
		for (size_t i = 0; i < count; ++i)
		{
			expect32[i] = (uint32_t)Next();
			PutLe(mem + 8 + i * 4, expect32[i], 4);
		}
		Endian::Fix32Array(mem + 8, count);
		for (size_t i = 0; i < count; ++i)
		{
			uint32_t got;
			memcpy(&got, mem + 8 + i * 4, 4);
			CHECK(got == expect32[i], "Fix32Array[%u]", (unsigned)i);
		}
		for (size_t k = 0; k < 8; ++k) CHECK(mem[k] == 0xa5, "Fix32Array wrote before the array");
		for (size_t k = 8 + count * 4; k < sizeof(mem); ++k) CHECK(mem[k] == 0xa5, "Fix32Array wrote past %u words", (unsigned)count);

		uint16_t expect16[N];
		memset(mem, 0xa5, sizeof(mem));
		for (size_t i = 0; i < count; ++i)
		{
			expect16[i] = (uint16_t)Next();
			PutLe(mem + 8 + i * 2, expect16[i], 2);
		}
		Endian::Fix16Array(mem + 8, count);
		for (size_t i = 0; i < count; ++i)
		{
			uint16_t got;
			memcpy(&got, mem + 8 + i * 2, 2);
			CHECK(got == expect16[i], "Fix16Array[%u]", (unsigned)i);
		}
		for (size_t k = 8 + count * 2; k < sizeof(mem); ++k) CHECK(mem[k] == 0xa5, "Fix16Array wrote past %u halfwords", (unsigned)count);

		// A second call converts back to the file's bytes.
		Endian::Fix16Array(mem + 8, count);
		for (size_t i = 0; i < count; ++i)
		{
			uint8_t le[2];
			PutLe(le, expect16[i], 2);
			CHECK(memcmp(mem + 8 + i * 2, le, 2) == 0, "Fix16Array twice");
		}
		if (s_failures > 20) return;
	}
	// A zero count touches nothing.
	memset(mem, 0x5a, sizeof(mem));
	Endian::Fix32Array(mem, 0);
	Endian::Fix16Array(mem, 0);
	for (size_t k = 0; k < sizeof(mem); ++k) CHECK(mem[k] == 0x5a, "zero count wrote");
}

int main()
{
	TestHostMatchesBuild();
	TestSwaps();
	TestReads();
	TestFix();
	TestArrays();
	uint16_t probe = 1;
	printf("endian-test (%s-endian host, %u-bit pointers): %d checks, %d failures -> %s\n",
		*(const uint8_t*)&probe == 1 ? "little" : "big", (unsigned)(sizeof(void*) * 8),
		s_checks, s_failures, s_failures ? "FAIL" : "PASS");
	return s_failures ? 1 : 0;
}
