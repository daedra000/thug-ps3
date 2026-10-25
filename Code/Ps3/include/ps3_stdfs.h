// THUG PS3: the subset of std::filesystem the music code uses (Gel/Music/SDL),
// on the PS3 file layer. Paths are resolved case-insensitively (see ps3_fs.h).
#ifndef PS3_STDFS_H
#define PS3_STDFS_H

#include <string>
#include <vector>
#include <string.h>
#include <lv2/sysfs.h>
#include "ps3_fs.h"

namespace ps3stdfs
{
	class path
	{
	public:
		typedef std::string string_type;

		path() {}
		path(const char* s) : m_s(s ? s : "") { normalise(); }
		path(const std::string& s) : m_s(s) { normalise(); }

		path operator/(const path& rhs) const
		{
			if (m_s.empty()) return rhs;
			if (rhs.m_s.empty()) return *this;
			return path(m_s + "/" + rhs.m_s);
		}
		path& operator/=(const path& rhs) { *this = *this / rhs; return *this; }

		const std::string& string() const { return m_s; }
		const std::string& native() const { return m_s; }
		const char* c_str() const { return m_s.c_str(); }
		bool empty() const { return m_s.empty(); }

		path filename() const
		{
			size_t p = m_s.rfind('/');
			return path(p == std::string::npos ? m_s : m_s.substr(p + 1));
		}
		path stem() const
		{
			std::string f = filename().m_s;
			size_t d = f.rfind('.');
			return path((d == std::string::npos || d == 0) ? f : f.substr(0, d));
		}
		path parent_path() const
		{
			size_t p = m_s.rfind('/');
			return path(p == std::string::npos ? std::string() : m_s.substr(0, p));
		}

		// Opening a file (std::ifstream file(path)) gets the real, case-corrected path.
		operator std::string() const { return resolved(); }

		std::string resolved() const
		{
			char buf[1024];
			if (!Ps3Fs::ResolvePath(m_s.c_str(), buf, sizeof(buf)))
				return m_s;
			return std::string(buf);
		}

	private:
		void normalise()
		{
			for (size_t i = 0; i < m_s.size(); ++i)
				if (m_s[i] == '\\') m_s[i] = '/';
			while (m_s.size() > 1 && m_s[m_s.size() - 1] == '/')
				m_s.erase(m_s.size() - 1);
		}
		std::string m_s;
	};

	static inline bool exists(const path& p)
	{
		return Ps3Fs::Exists(p.resolved().c_str());
	}

	static inline bool is_directory(const path& p)
	{
		bool dir = false;
		return Ps3Fs::Exists(p.resolved().c_str(), &dir) && dir;
	}

	// p relative to base, when p is inside base (all this code needs).
	static inline path relative(const path& p, const path& base)
	{
		const std::string& a = p.string();
		const std::string& b = base.string();
		if (a.size() > b.size() && !strncasecmp(a.c_str(), b.c_str(), b.size()) && a[b.size()] == '/')
			return path(a.substr(b.size() + 1));
		return p;
	}

	class directory_entry
	{
	public:
		directory_entry(const path& p, bool dir) : m_path(p), m_dir(dir) {}
		const ps3stdfs::path& path() const { return m_path; }
		bool is_directory() const { return m_dir; }
		bool is_regular_file() const { return !m_dir; }
	private:
		ps3stdfs::path m_path;
		bool m_dir;
	};

	namespace detail
	{
		static inline void list(const path& dir, bool recursive, std::vector<directory_entry>& out, int depth = 0)
		{
			if (depth > 8) return;	// no runaway recursion
			std::string real = dir.resolved();
			std::vector<std::string> names;
			if (!Ps3Fs::ListDirectory(real.c_str(), names))
				return;	// missing (e.g. the PC data has no Data/music): nothing listed

			for (size_t i = 0; i < names.size(); ++i)
			{
				path child = path(real) / path(names[i]);
				bool isDir = false;
				Ps3Fs::Exists(child.string().c_str(), &isDir);
				out.push_back(directory_entry(child, isDir));
				if (recursive && isDir)
					list(child, true, out, depth + 1);
			}
		}
	}

	// Both iterators list everything up front and are used in range-for loops.
	class directory_iterator
	{
	public:
		explicit directory_iterator(const path& p) { detail::list(p, false, m_entries); }
		std::vector<directory_entry>::const_iterator begin() const { return m_entries.begin(); }
		std::vector<directory_entry>::const_iterator end() const { return m_entries.end(); }
	protected:
		directory_iterator() {}
		std::vector<directory_entry> m_entries;
	};

	class recursive_directory_iterator : public directory_iterator
	{
	public:
		explicit recursive_directory_iterator(const path& p) { detail::list(p, true, m_entries); }
	};
}

#endif // PS3_STDFS_H
