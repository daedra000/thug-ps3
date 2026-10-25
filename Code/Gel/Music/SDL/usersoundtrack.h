#pragma once

#include <Core/Defines.h>

#include <string>
#include <vector>
#ifdef __PLAT_PS3__
#include <ps3_stdfs.h>		// PS3: no std::filesystem in newlib's libstdc++
namespace stdfs = ps3stdfs;
#else
#include <filesystem>
namespace stdfs = std::filesystem;
#endif

namespace UserSoundtrack
{
	// Track definition
	struct Track
	{
		stdfs::path path;
	};

	struct Soundtrack
	{
		std::string name;
		std::vector<Track> tracks;
	};

	typedef std::vector<Soundtrack> Soundtracks;

	// Get soundtracks
	const Soundtracks &GetSoundtracks();
}
