#include "usersoundtrack.h"

#include <Sys/File/filesys.h>

#include <Shlwapi.h>

//#include <filesystem>

namespace UserSoundtrack
{
	static stdfs::path ModulePath()
	{
		char		nameConversionBuffer[256];
		GetModuleFileNameA(NULL, nameConversionBuffer, 255);
		PathRemoveFileSpecA(nameConversionBuffer);

		return nameConversionBuffer;
	}

	// User soundtrack indexer
	Soundtracks IndexSoundtracks()
	{
		// Get user soundtrack path
		stdfs::path usersoundtrack_path = ModulePath() / "Soundtracks";
		if (!stdfs::exists(usersoundtrack_path))
			return Soundtracks();

		// Iterate over folders in user soundtrack path
		Soundtracks soundtracks;
		for (const auto &soundtrack_entry : stdfs::directory_iterator(usersoundtrack_path))
		{
			// Check if entry is a directory
			if (soundtrack_entry.is_directory())
			{
				// Create soundtrack
				Soundtrack soundtrack;
				stdfs::path::string_type name = soundtrack_entry.path().filename().native();
				
				soundtrack.name.reserve(name.size());
				for (auto &c : name)
				{
					if (c >= 0x20 && c < 0x80)
						soundtrack.name.push_back((char)c);
					else
						soundtrack.name.push_back('?');
				}

				// Iterate over files in soundtrack path
				for (const auto &song_entry : stdfs::recursive_directory_iterator(soundtrack_entry.path()))
				{
					// Check if entry is a file
					if (song_entry.is_regular_file())
					{
						// Create track
						Track track;
						track.path = song_entry.path();

						// Add track to soundtrack
						soundtrack.tracks.push_back(track);
					}
				}

				// Add soundtrack to soundtracks
				soundtracks.push_back(soundtrack);
			}
		}

		return soundtracks;
	}

	// Get soundtracks
	const Soundtracks &GetSoundtracks()
	{
		static Soundtracks soundtracks(IndexSoundtracks());
		return soundtracks;
	}
}