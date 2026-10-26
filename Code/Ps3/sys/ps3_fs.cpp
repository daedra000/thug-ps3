// THUG PS3: path resolution (see ps3_fs.h).

#include <ps3_fs.h>
#include <ps3_platform.h>

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <map>
#include <string>
#include <vector>

#include <lv2/sysfs.h>
#include <sys/mutex.h>
#include <sys/systime.h>

namespace Ps3Fs
{

static char s_appDir[512] = PS3_USRDIR;

// Directory listings, keyed by absolute directory path, read once. A name that isn't in a
// listing more than 2 s old re-reads it (the game, or newlib's fopen, may have created it).
struct CachedListing
{
	std::vector<std::string> names;
	u64 time;
};
static std::map<std::string, CachedListing>* s_dirCache;
// Directories already resolved: lower-case path (real parent + name as asked) -> real path.
// Saves a stat per path component on every open.
static std::map<std::string, std::string>* s_resolvedDirs;
// Names known to be missing: real parent directory + "/" + lower-case name -> when that was
// found out (sysGetSystemTime). Without it every lookup of a missing file (the game probes
// for optional files all the time) stat'ed it and, every 2 s, re-read the whole directory.
// Cleared with the listings by InvalidateParent / InvalidateCache, which the shim calls
// whenever it creates or deletes a file or folder; the entries also expire after 60 s, for
// files that appear some other way. (A stale entry is mostly harmless anyway: the path then
// comes back with the case it was asked with, which is how such a file was created.)
static std::map<std::string, u64>* s_missing;
static const u64 kMissingLifetime = 60 * 1000000ull;
static const size_t kMaxMissing = 4096;
// Bumped by every invalidation: a listing or a "missing" read without the lock is only
// stored if nothing was invalidated meanwhile (it may predate the change).
static u64 s_generation;
// Guards the maps above. The file system calls (stat, directory reads) are made without it,
// so one slow lookup doesn't hold up every other thread's file opens.
static sys_mutex_t s_mutex;
static volatile bool s_mutexReady;

static void Lock()
{
	if (!s_mutexReady)	// normally created by SetAppPath, before any thread
	{
		sys_mutex_attr_t attr;
		memset(&attr, 0, sizeof(attr));
		attr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
		attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
		attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
		attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
		strcpy(attr.name, "ps3fs");
		sysMutexCreate(&s_mutex, &attr);
		s_mutexReady = true;
	}
	sysMutexLock(s_mutex, 0);
}

static void Unlock()
{
	sysMutexUnlock(s_mutex);
}

void SetAppPath(const char* argv0)
{
	Lock();		// creates the lock here, on the main thread, before any other thread
	Unlock();
	if (!argv0 || argv0[0] != '/')
		return;
	snprintf(s_appDir, sizeof(s_appDir), "%s", argv0);
	char* slash = strrchr(s_appDir, '/');
	if (slash && slash != s_appDir)
		*slash = '\0';

	// Started from somewhere else (ps3load / PS3LoadX runs the EBOOT from its own folder):
	// the PC data and the log live in the installed game's USRDIR.
	char data[600];
	snprintf(data, sizeof(data), "%s/Data", s_appDir);
	bool isDir = false;
	if (!(Exists(data, &isDir) && isDir) && strcmp(s_appDir, PS3_USRDIR) != 0)
	{
		bool installed = false;
		if (Exists(PS3_USRDIR "/Data", &installed) && installed)
			snprintf(s_appDir, sizeof(s_appDir), "%s", PS3_USRDIR);
	}
}

const char* AppDir()
{
	return s_appDir;
}

static volatile bool s_trace;

void SetTrace(bool on)
{
	s_trace = on;
}

bool Exists(const char* absPath, bool* isDir, unsigned long long* size)
{
	sysFSStat st;
	if (s_trace) Ps3Platform::Trace("[fs] stat %s", absPath);
	s32 r = sysFsStat(absPath, &st);
	if (s_trace) Ps3Platform::Trace("[fs]   -> %d, mode %o", (int)r, r == 0 ? (unsigned)st.st_mode : 0u);
	if (r != 0)
		return false;
	if (isDir) *isDir = (st.st_mode & 0170000) == 0040000;
	if (size) *size = st.st_size;
	return true;
}

void InvalidateCache()
{
	Lock();
	++s_generation;
	if (s_dirCache)
		s_dirCache->clear();
	if (s_resolvedDirs)
		s_resolvedDirs->clear();
	if (s_missing)
		s_missing->clear();
	Unlock();
}

void InvalidateParent(const char* absPath)
{
	if (!absPath) { InvalidateCache(); return; }
	std::string dir(absPath);
	size_t slash = dir.rfind('/');
	if (slash == std::string::npos) return;
	dir.erase(slash ? slash : 1);
	Lock();
	++s_generation;
	if (s_dirCache)
		s_dirCache->erase(dir);
	if (s_missing)
	{
		// Every "missing" entry under dir: the keys from dir + "/" up to dir + "0" ('0'
		// follows '/'), which also takes those of its subdirectories (harmless).
		std::string from = dir + "/", to = dir + "0";
		s_missing->erase(s_missing->lower_bound(from), s_missing->lower_bound(to));
	}
	Unlock();
}

static std::string Lower(const std::string& s)
{
	std::string r(s);
	for (size_t i = 0; i < r.size(); ++i)
		if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
	return r;
}

static bool FindName(const std::vector<std::string>& names, const std::string& dir, const std::string& name, std::string& real)
{
	for (size_t k = 0; k < names.size(); ++k)
	{
		if (!strcasecmp(names[k].c_str(), name.c_str()))
		{
			real = dir + "/" + names[k];
			return true;
		}
	}
	return false;
}

// Looks name up in dir's cached listing, reading the listing (without the lock) when there
// is none yet, or when refreshIfOld and it is over 2 s old.
static bool FindInListing(const std::string& dir, const std::string& name, std::string& real, bool refreshIfOld)
{
	Lock();
	if (!s_dirCache)
		s_dirCache = new std::map<std::string, CachedListing>();
	const u64 now = sysGetSystemTime();
	std::map<std::string, CachedListing>::iterator it = s_dirCache->find(dir);
	if (it != s_dirCache->end() && (!refreshIfOld || now - it->second.time < 2000000))
	{
		const bool found = FindName(it->second.names, dir, name, real);
		Unlock();
		return found;
	}
	const u64 generation = s_generation;
	Unlock();

	std::vector<std::string> names;
	ListDirectory(dir.c_str(), names);
	const bool found = FindName(names, dir, name, real);

	Lock();
	if (s_generation == generation)
	{
		CachedListing& l = (*s_dirCache)[dir];
		l.names.swap(names);
		l.time = now;
	}
	Unlock();
	return found;
}

// True when name was found missing from dir less than kMissingLifetime ago.
static bool KnownMissing(const std::string& key, u64 now)
{
	Lock();
	bool known = false;
	if (s_missing)
	{
		std::map<std::string, u64>::iterator it = s_missing->find(key);
		if (it != s_missing->end())
		{
			if (now - it->second < kMissingLifetime)
				known = true;
			else
				s_missing->erase(it);
		}
	}
	Unlock();
	return known;
}

static void NoteMissing(const std::string& key, u64 now, u64 generation)
{
	Lock();
	if (s_generation == generation)
	{
		if (!s_missing)
			s_missing = new std::map<std::string, u64>();
		if (s_missing->size() >= kMaxMissing)
			s_missing->clear();
		(*s_missing)[key] = now;
	}
	Unlock();
}

bool ListDirectory(const char* absDir, std::vector<std::string>& names)
{
	names.clear();
	bool isDir = false;
	if (!Exists(absDir, &isDir) || !isDir)
		return false;
	s32 fd;
	if (s_trace) Ps3Platform::Trace("[fs] opendir %s", absDir);
	s32 r = sysFsOpendir(absDir, &fd);
	if (s_trace) Ps3Platform::Trace("[fs]   -> %d, fd %d", (int)r, (int)fd);
	if (r != 0)
		return false;
	sysFSDirent ent;
	std::string last;
	int repeats = 0;
	for (int n = 0; n < 20000; ++n)
	{
		u64 read = 0;
		memset(&ent, 0, sizeof(ent));
		if (s_trace && n < 4) Ps3Platform::Trace("[fs] readdir %d", n);
		r = sysFsReaddir(fd, &ent, &read);
		if (s_trace && (n < 4 || r != 0 || read == 0))
			Ps3Platform::Trace("[fs]   -> %d, read %u, type %u, namlen %u, '%.40s'", (int)r, (unsigned)read, (unsigned)ent.d_type, (unsigned)ent.d_namlen, ent.d_name);
		if (r != 0 || read == 0 || ent.d_namlen == 0 || ent.d_name[0] == 0)
			break;
		std::string name(ent.d_name, ent.d_namlen);
		if (name == last && ++repeats > 2)
			break;	// the same entry over and over: the end, whatever readdir says
		if (name != last) repeats = 0;
		last = name;
		if (name != "." && name != "..")
			names.push_back(name);
	}
	if (s_trace) Ps3Platform::Trace("[fs] closedir after %u names", (unsigned)names.size());
	sysFsClosedir(fd);
	return true;
}

bool ResolvePath(const char* path, char* out, size_t outSize)
{
	// Split into components, dropping drive letters, "." and "..", empty parts
	// and trailing spaces (all of which the PC paths contain).
	std::vector<std::string> parts;
	std::string base;

	const char* p = path;
	if (p[0] && p[1] == ':')
	{
		// "c:\..." / "x:\...": a Windows drive means the game's own folder here.
		p += 2;
		while (*p == '/' || *p == '\\') ++p;
		base = s_appDir;
	}
	else if (*p == '/' || *p == '\\')
		base = "";							// absolute
	else
		base = s_appDir;

	std::string all = base + "/" + p;
	size_t i = 0;
	while (i < all.size())
	{
		while (i < all.size() && (all[i] == '/' || all[i] == '\\')) ++i;
		size_t start = i;
		while (i < all.size() && all[i] != '/' && all[i] != '\\') ++i;
		std::string part = all.substr(start, i - start);
		while (!part.empty() && part[part.size() - 1] == ' ')
			part.erase(part.size() - 1);
		if (part.empty() || part == ".")
			continue;
		if (part == "..")
		{
			if (!parts.empty()) parts.pop_back();
			continue;
		}
		parts.push_back(part);
	}

	// Walk the components, fixing the case of each one that doesn't exist as given. The lock
	// is only held for the cache lookups; stat and directory reads run without it.
	std::string cur;
	bool missing = false;
	for (size_t n = 0; n < parts.size(); ++n)
	{
		std::string next = cur + "/" + parts[n];
		// Mount points (/dev_hdd0, /dev_usb000...) are exact; so is anything once
		// a component is missing.
		if (!missing && n > 0)
		{
			const bool last = n + 1 == parts.size();
			std::string key;
			if (!last)
			{
				key = Lower(next);
				bool hitFound = false;
				Lock();
				if (!s_resolvedDirs) s_resolvedDirs = new std::map<std::string, std::string>();
				std::map<std::string, std::string>::iterator hit = s_resolvedDirs->find(key);
				if (hit != s_resolvedDirs->end())
				{
					cur = hit->second;
					hitFound = true;
				}
				Unlock();
				if (hitFound)
					continue;
			}
			const std::string missingKey = cur + "/" + Lower(parts[n]);
			const u64 now = sysGetSystemTime();
			bool found = false;
			if (!KnownMissing(missingKey, now))
			{
				Lock();
				const u64 generation = s_generation;
				Unlock();
				found = Exists(next.c_str());
				if (!found)
				{
					std::string real;
					// Cached listing first; if it's old and lacks the name, read it again.
					found = FindInListing(cur, parts[n], real, false) || FindInListing(cur, parts[n], real, true);
					if (found)
						next = real;
					else
						NoteMissing(missingKey, now, generation);
				}
			}
			if (!found)
				missing = true;
			else if (!last)
			{
				Lock();
				(*s_resolvedDirs)[key] = next;
				Unlock();
			}
		}
		cur = next;
	}

	if (cur.empty())
		cur = "/";
	if (cur.size() + 1 > outSize)
		return false;
	memcpy(out, cur.c_str(), cur.size() + 1);
	return true;
}

} // namespace Ps3Fs
