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

namespace Ps3Fs
{

static char s_appDir[512] = PS3_USRDIR;

// Directory listings, keyed by absolute directory path. Game data never changes
// while the game runs, so a listing is read once.
static std::map<std::string, std::vector<std::string> >* s_dirCache;
static sys_mutex_t s_mutex;
static bool s_mutexReady;

static void Lock()
{
	if (!s_mutexReady)
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
	if (!argv0 || argv0[0] != '/')
		return;
	snprintf(s_appDir, sizeof(s_appDir), "%s", argv0);
	char* slash = strrchr(s_appDir, '/');
	if (slash && slash != s_appDir)
		*slash = '\0';
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
	if (s_dirCache)
		s_dirCache->clear();
	Unlock();
}

static const std::vector<std::string>& Listing(const std::string& dir)
{
	if (!s_dirCache)
		s_dirCache = new std::map<std::string, std::vector<std::string> >();

	std::map<std::string, std::vector<std::string> >::iterator it = s_dirCache->find(dir);
	if (it != s_dirCache->end())
		return it->second;

	std::vector<std::string>& names = (*s_dirCache)[dir];
	ListDirectory(dir.c_str(), names);
	return names;
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

	// Walk the components, fixing the case of each one that doesn't exist as given.
	std::string cur;
	bool missing = false;
	Lock();
	for (size_t n = 0; n < parts.size(); ++n)
	{
		std::string next = cur + "/" + parts[n];
		// Mount points (/dev_hdd0, /dev_usb000...) are exact; so is anything once
		// a component is missing.
		if (!missing && n > 0 && !Exists(next.c_str()))
		{
			const std::vector<std::string>& names = Listing(cur);
			bool found = false;
			for (size_t k = 0; k < names.size(); ++k)
			{
				if (!strcasecmp(names[k].c_str(), parts[n].c_str()))
				{
					next = cur + "/" + names[k];
					found = true;
					break;
				}
			}
			if (!found)
				missing = true;
		}
		cur = next;
	}
	Unlock();

	if (cur.empty())
		cur = "/";
	if (cur.size() + 1 > outSize)
		return false;
	memcpy(out, cur.c_str(), cur.size() + 1);
	return true;
}

} // namespace Ps3Fs
