// THUG PS3: paths.
//
// The game data is the PC version's Data folder, copied by the user to
//   /dev_hdd0/game/THUG00001/USRDIR/Data
// The PC code builds Windows paths ("...\Data\levels\nj\nj.pre"), and the PC file
// names don't match the letter case the code asks for, while the PS3 file system
// is case-sensitive. ResolvePath turns any such path into the real absolute one.
#ifndef PS3_FS_H
#define PS3_FS_H

#include <stddef.h>
#include <string>
#include <vector>

#define PS3_APP_ID      "THUG00001"
#define PS3_USRDIR      "/dev_hdd0/game/" PS3_APP_ID "/USRDIR"

namespace Ps3Fs
{
	// Remember where the EBOOT runs from (argv[0]); defaults to PS3_USRDIR.
	void        SetAppPath(const char* argv0);
	const char* AppDir();               // e.g. /dev_hdd0/game/THUG00001/USRDIR

	// Absolute, '/'-separated, case-corrected path for `path`. A relative path is
	// taken relative to AppDir(); a Windows drive prefix ("c:\") is dropped.
	// When the file doesn't exist (yet), the existing part of the path is still
	// case-corrected and the rest is kept as given. Returns false only if the
	// result doesn't fit in `out`.
	bool ResolvePath(const char* path, char* out, size_t outSize);

	bool Exists(const char* absPath, bool* isDir = 0, unsigned long long* size = 0);

	// Forget cached directory listings (after creating or deleting files).
	void InvalidateCache();

	// The names in a directory (no "." or ".."). False, with nothing listed, when the
	// directory doesn't exist or can't be opened. Never loops forever: it stops at the
	// first error, empty entry or repeated name, and after 20000 entries.
	bool ListDirectory(const char* absDir, std::vector<std::string>& names);

	// Log every stat and directory read (Ps3Platform::Trace) while on.
	void SetTrace(bool on);
}

#endif // PS3_FS_H
