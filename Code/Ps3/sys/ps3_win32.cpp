#include <ps3_platform.h>
// THUG PS3: Win32 API stand-ins on lv2 (see ps3_win32.h).

#include <ps3_compat.h>
#include <ps3_fs.h>

#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <lv2/sysfs.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/systime.h>
#include <lv2/systime.h>
#include <net/net.h>

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
namespace
{
	enum HandleKind { kMutex = 0x4d555458, kEvent = 0x45564e54, kThread = 0x54485244, kFile = 0x46494c45 };

	struct Handle
	{
		uint32_t kind;
	};

	struct MutexHandle : Handle
	{
		sys_mutex_t mutex;
	};

	struct EventHandle : Handle
	{
		sys_mutex_t mutex;
		sys_cond_t cond;
		bool manualReset;
		volatile bool signaled;
	};

	struct ThreadHandle : Handle
	{
		sys_ppu_thread_t id;
		LPTHREAD_START_ROUTINE start;
		LPVOID param;
		volatile bool done;
		DWORD exitCode;
		bool joined;
		bool closed;		// CloseHandle ran while the thread was still running: it frees itself
	};

	// Guards ThreadHandle::done/closed between a finishing thread and CloseHandle.
	sys_mutex_t s_threadLock;
	bool s_threadLockReady;

	struct FileHandle : Handle
	{
		s32 fd;
		bool writable;
	};

	__thread DWORD s_lastError = 0;	// per thread, as on Win32

	void MakeMutex(sys_mutex_t* m, bool recursive, const char* name)
	{
		sys_mutex_attr_t attr;
		memset(&attr, 0, sizeof(attr));
		attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
		attr.attr_recursive = recursive ? SYS_MUTEX_ATTR_RECURSIVE : SYS_MUTEX_ATTR_NOT_RECURSIVE;
		attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
		attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
		strncpy(attr.name, name, sizeof(attr.name) - 1);
		if (sysMutexCreate(m, &attr) != 0)
			printf("[win32] couldn't create mutex '%s'\n", name);
	}

	// Win32 milliseconds -> lv2 microseconds (0 = wait forever on lv2).
	u64 TimeoutUs(DWORD ms)
	{
		if (ms == INFINITE) return 0;
		return (u64)ms * 1000 + 1;
	}
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
void Sleep(DWORD ms)
{
	if (ms == 0)
		sysThreadYield();
	else
		sysUsleep((u64)ms * 1000);	// 64-bit: Sleep(INFINITE) mustn't wrap
}

DWORD GetTickCount()
{
	return (DWORD)(sysGetSystemTime() / 1000);
}

DWORD timeGetTime()
{
	return GetTickCount();
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* count)
{
	count->QuadPart = (LONGLONG)sysGetSystemTime();
	return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq)
{
	freq->QuadPart = 1000000;	// sysGetSystemTime() is in microseconds
	return TRUE;
}

// ---------------------------------------------------------------------------
// Mutexes, events, threads
// ---------------------------------------------------------------------------
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL initialOwner, LPCSTR)
{
	MutexHandle* h = new MutexHandle;
	h->kind = kMutex;
	MakeMutex(&h->mutex, true, "w32mtx");	// Win32 mutexes are recursive
	if (initialOwner)
		sysMutexLock(h->mutex, 0);
	return h;
}

BOOL ReleaseMutex(HANDLE hh)
{
	MutexHandle* h = (MutexHandle*)hh;
	if (!h || h->kind != kMutex) return FALSE;
	return sysMutexUnlock(h->mutex) == 0;
}

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manualReset, BOOL initialState, LPCSTR)
{
	EventHandle* h = new EventHandle;
	h->kind = kEvent;
	h->manualReset = manualReset != 0;
	h->signaled = initialState != 0;
	MakeMutex(&h->mutex, false, "w32evt");
	sys_cond_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.attr_pshared = SYS_COND_ATTR_PSHARED;
	strcpy(attr.name, "w32evt");
	if (sysCondCreate(&h->cond, h->mutex, &attr) != 0)
		printf("[win32] couldn't create an event\n");
	return h;
}

BOOL SetEvent(HANDLE hh)
{
	EventHandle* h = (EventHandle*)hh;
	if (!h || h->kind != kEvent) return FALSE;
	sysMutexLock(h->mutex, 0);
	h->signaled = true;
	sysCondBroadcast(h->cond);
	sysMutexUnlock(h->mutex);
	return TRUE;
}

BOOL ResetEvent(HANDLE hh)
{
	EventHandle* h = (EventHandle*)hh;
	if (!h || h->kind != kEvent) return FALSE;
	sysMutexLock(h->mutex, 0);
	h->signaled = false;
	sysMutexUnlock(h->mutex);
	return TRUE;
}

static void ThreadTrampoline(void* arg)
{
	ThreadHandle* h = (ThreadHandle*)arg;
	Ps3Platform::AttachThreadLog();
	DWORD code = h->start(h->param);
	sysMutexLock(s_threadLock, 0);
	h->exitCode = code;
	h->done = true;
	const bool closed = h->closed;
	sysMutexUnlock(s_threadLock);
	if (closed) delete h;	// nobody holds the handle any more (and the thread was detached)
	sysThreadExit(0);
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, size_t stackSize, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD, LPDWORD threadId)
{
	if (Ps3Platform::NoThreads())
	{
		printf("[diag] CreateThread refused (nothreads.txt), entry %p\n", (void*)start);
		return NULL;
	}
	if (!s_threadLockReady)
	{
		MakeMutex(&s_threadLock, false, "w32thr");
		s_threadLockReady = true;
	}
	ThreadHandle* h = new ThreadHandle;
	h->kind = kThread;
	h->closed = false;
	h->start = start;
	h->param = param;
	h->done = false;
	h->exitCode = 0;
	h->joined = false;
	if (stackSize < 64 * 1024) stackSize = 64 * 1024;
	if (sysThreadCreate(&h->id, ThreadTrampoline, h, 1000, stackSize, THREAD_JOINABLE, (char*)"w32thread") != 0)
	{
		delete h;
		return NULL;
	}
	if (threadId) *threadId = (DWORD)h->id;
	return h;
}

DWORD GetCurrentThreadId()
{
	sys_ppu_thread_t id;
	sysThreadGetId(&id);
	return (DWORD)id;
}

DWORD WaitForSingleObject(HANDLE hh, DWORD ms)
{
	Handle* base = (Handle*)hh;
	if (!base) return WAIT_FAILED;

	switch (base->kind)
	{
	case kMutex:
	{
		MutexHandle* h = (MutexHandle*)base;
		s32 r = (ms == 0) ? sysMutexTryLock(h->mutex) : sysMutexLock(h->mutex, TimeoutUs(ms));
		return r == 0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
	}
	case kEvent:
	{
		EventHandle* h = (EventHandle*)base;
		sysMutexLock(h->mutex, 0);
		u64 start = sysGetSystemTime();
		while (!h->signaled)
		{
			if (ms == 0) break;
			u64 wait = 0;
			if (ms != INFINITE)
			{
				u64 elapsed = sysGetSystemTime() - start;
				u64 total = (u64)ms * 1000;
				if (elapsed >= total) break;
				wait = total - elapsed;
			}
			sysCondWait(h->cond, wait);
		}
		bool got = h->signaled;
		if (got && !h->manualReset)
			h->signaled = false;
		sysMutexUnlock(h->mutex);
		return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
	}
	case kThread:
	{
		ThreadHandle* h = (ThreadHandle*)base;
		if (ms == INFINITE)
		{
			if (!h->joined) { u64 rv; sysThreadJoin(h->id, &rv); h->joined = true; }
			return WAIT_OBJECT_0;
		}
		u64 end = sysGetSystemTime() + (u64)ms * 1000;
		while (!h->done && sysGetSystemTime() < end)
			sysUsleep(1000);
		return h->done ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
	}
	default:
		return WAIT_FAILED;
	}
}

BOOL CloseHandle(HANDLE hh)
{
	Handle* base = (Handle*)hh;
	if (!base || hh == INVALID_HANDLE_VALUE) return FALSE;
	switch (base->kind)
	{
	case kMutex:
		sysMutexDestroy(((MutexHandle*)base)->mutex);
		delete (MutexHandle*)base;
		return TRUE;
	case kEvent:
		sysCondDestroy(((EventHandle*)base)->cond);
		sysMutexDestroy(((EventHandle*)base)->mutex);
		delete (EventHandle*)base;
		return TRUE;
	case kThread:
	{
		ThreadHandle* h = (ThreadHandle*)base;
		// Win32 just drops the handle; the thread runs on. A finished thread is joined (lv2
		// needs joinable threads joined); a running one is detached and frees its handle.
		sysMutexLock(s_threadLock, 0);
		if (h->done)
		{
			sysMutexUnlock(s_threadLock);
			if (!h->joined) { u64 rv; sysThreadJoin(h->id, &rv); }
			delete h;
		}
		else
		{
			const sys_ppu_thread_t id = h->id;
			h->closed = true;
			sysMutexUnlock(s_threadLock);
			sysThreadDetach(id);
		}
		return TRUE;
	}
	case kFile:
		sysFsClose(((FileHandle*)base)->fd);
		delete (FileHandle*)base;
		return TRUE;
	}
	return FALSE;
}

void InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
	sys_mutex_t* m = new sys_mutex_t;
	MakeMutex(m, true, "w32cs");
	cs->impl = m;
}

void DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
	if (!cs->impl) return;
	sysMutexDestroy(*(sys_mutex_t*)cs->impl);
	delete (sys_mutex_t*)cs->impl;
	cs->impl = NULL;
}

void EnterCriticalSection(LPCRITICAL_SECTION cs) { sysMutexLock(*(sys_mutex_t*)cs->impl, 0); }
void LeaveCriticalSection(LPCRITICAL_SECTION cs) { sysMutexUnlock(*(sys_mutex_t*)cs->impl); }

// ---------------------------------------------------------------------------
// Multimedia timers
// ---------------------------------------------------------------------------
namespace
{
	struct Timer
	{
		UINT id;
		UINT delayMs;
		LPTIMECALLBACK cb;
		DWORD_PTR user;
		bool periodic;
		volatile bool stop;
		sys_ppu_thread_t thread;
	};

	enum { kMaxTimers = 8 };
	Timer* s_timers[kMaxTimers];
	UINT s_nextTimerId = 1;
	sys_mutex_t s_timerLock;
	bool s_timerLockReady;

	void TimerLock()
	{
		if (!s_timerLockReady)
		{
			sys_mutex_attr_t attr;
			memset(&attr, 0, sizeof(attr));
			attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
			attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
			attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
			attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
			strcpy(attr.name, "w32timer");
			sysMutexCreate(&s_timerLock, &attr);
			s_timerLockReady = true;
		}
		sysMutexLock(s_timerLock, 0);
	}

	void TimerUnlock()
	{
		sysMutexUnlock(s_timerLock);
	}

	void TimerThread(void* arg)
	{
		Ps3Platform::AttachThreadLog();
		Timer* t = (Timer*)arg;
		u64 next = sysGetSystemTime() + (u64)t->delayMs * 1000;
		while (!t->stop)
		{
			u64 now = sysGetSystemTime();
			if (now < next)
			{
				u64 wait = next - now;
				sysUsleep(wait > 2000 ? 2000 : (u32)wait);	// wake often so a kill is quick
				continue;
			}
			t->cb(t->id, 0, t->user, 0, 0);
			if (!t->periodic) break;
			next += (u64)t->delayMs * 1000;
			if (next < sysGetSystemTime()) next = sysGetSystemTime() + (u64)t->delayMs * 1000;
		}
		// The thread owns its Timer: take it out of the table (if timeKillEvent hasn't) and free it.
		TimerLock();
		for (int i = 0; i < kMaxTimers; ++i)
			if (s_timers[i] == t) s_timers[i] = NULL;
		TimerUnlock();
		delete t;
		sysThreadExit(0);
	}
}

MMRESULT timeSetEvent(UINT delayMs, UINT, LPTIMECALLBACK cb, DWORD_PTR user, UINT flags)
{
	if (Ps3Platform::NoThreads())
	{
		printf("[diag] timeSetEvent refused (nothreads.txt)\n");
		return 0;
	}
	TimerLock();
	for (int i = 0; i < kMaxTimers; ++i)
	{
		if (s_timers[i]) continue;
		Timer* t = new Timer;
		t->id = s_nextTimerId++;
		t->delayMs = delayMs ? delayMs : 1;
		t->cb = cb;
		t->user = user;
		t->periodic = (flags & TIME_PERIODIC) != 0;
		t->stop = false;
		UINT id = t->id;
		s_timers[i] = t;
		// Not joinable: timeKillEvent doesn't wait for it (as on Win32), since a running
		// callback may be blocked on a lock the killer holds (the loading screen does this).
		// 256 KB: the loading-screen callback draws.
		if (sysThreadCreate(&t->thread, TimerThread, t, 1000, 256 * 1024, 0, (char*)"w32timer") != 0)
		{
			s_timers[i] = NULL;
			delete t;
			id = 0;
		}
		TimerUnlock();
		return id;
	}
	TimerUnlock();
	return 0;
}

MMRESULT timeKillEvent(UINT id)
{
	TimerLock();
	for (int i = 0; i < kMaxTimers; ++i)
	{
		Timer* t = s_timers[i];
		if (!t || t->id != id) continue;
		t->stop = true;		// the thread sees it, finishes any callback, and frees t
		s_timers[i] = NULL;
		TimerUnlock();
		return TIMERR_NOERROR;
	}
	TimerUnlock();
	return 1;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE)
{
	char path[1024];
	if (!Ps3Fs::ResolvePath(name, path, sizeof(path)))
	{
		s_lastError = ERROR_FILE_NOT_FOUND;
		return INVALID_HANDLE_VALUE;
	}

	s32 flags;
	bool write = (access & GENERIC_WRITE) != 0;
	if (write)
		flags = (access & GENERIC_READ) ? SYS_O_RDWR : SYS_O_WRONLY;
	else
		flags = SYS_O_RDONLY;
	switch (disposition)
	{
	case CREATE_NEW:
	case CREATE_ALWAYS:     flags |= SYS_O_CREAT | SYS_O_TRUNC; break;
	case OPEN_ALWAYS:       flags |= SYS_O_CREAT; break;
	case TRUNCATE_EXISTING: flags |= SYS_O_TRUNC; break;
	default: break;
	}

	s32 fd;
	if (sysFsOpen(path, flags, &fd, NULL, 0) != 0)
	{
		s_lastError = ERROR_FILE_NOT_FOUND;
		return INVALID_HANDLE_VALUE;
	}
	if (flags & SYS_O_CREAT)
		Ps3Fs::InvalidateParent(path);

	FileHandle* h = new FileHandle;
	h->kind = kFile;
	h->fd = fd;
	h->writable = write;
	s_lastError = ERROR_SUCCESS;
	return h;
}

static bool SeekForOverlapped(FileHandle* h, LPOVERLAPPED ov)
{
	if (!ov) return true;
	u64 pos;
	s64 offset = ((s64)ov->OffsetHigh << 32) | ov->Offset;
	return sysFsLseek(h->fd, offset, SEEK_SET, &pos) == 0;
}

BOOL ReadFile(HANDLE hh, LPVOID buf, DWORD toRead, LPDWORD read, LPOVERLAPPED ov)
{
	FileHandle* h = (FileHandle*)hh;
	if (read) *read = 0;
	if (!h || hh == INVALID_HANDLE_VALUE || h->kind != kFile) { s_lastError = 6; return FALSE; }
	if (!SeekForOverlapped(h, ov)) { s_lastError = ERROR_HANDLE_EOF; return FALSE; }

	u64 got = 0;
	if (toRead && sysFsRead(h->fd, buf, toRead, &got) != 0)
	{
		s_lastError = 30;	// ERROR_READ_FAULT
		return FALSE;
	}
	if (read) *read = (DWORD)got;
	if (ov)
	{
		ov->InternalHigh = got;
		ov->Internal = 0;
		if (ov->hEvent) SetEvent(ov->hEvent);
		if (got == 0 && toRead) { s_lastError = ERROR_HANDLE_EOF; return FALSE; }
	}
	s_lastError = ERROR_SUCCESS;
	return TRUE;
}

BOOL WriteFile(HANDLE hh, LPCVOID buf, DWORD toWrite, LPDWORD written, LPOVERLAPPED ov)
{
	FileHandle* h = (FileHandle*)hh;
	if (written) *written = 0;
	if (!h || hh == INVALID_HANDLE_VALUE || h->kind != kFile) return FALSE;
	if (!SeekForOverlapped(h, ov)) return FALSE;
	u64 put = 0;
	if (sysFsWrite(h->fd, buf, toWrite, &put) != 0) return FALSE;
	if (written) *written = (DWORD)put;
	if (ov) { ov->InternalHigh = put; if (ov->hEvent) SetEvent(ov->hEvent); }
	return TRUE;
}

DWORD SetFilePointer(HANDLE hh, LONG distance, LONG* distanceHigh, DWORD method)
{
	FileHandle* h = (FileHandle*)hh;
	if (!h || hh == INVALID_HANDLE_VALUE || h->kind != kFile) return INVALID_SET_FILE_POINTER;
	s64 offset = distanceHigh ? (((s64)*distanceHigh << 32) | (uint32_t)distance) : (s64)distance;
	s32 whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
	u64 pos;
	if (sysFsLseek(h->fd, offset, whence, &pos) != 0) return INVALID_SET_FILE_POINTER;
	if (distanceHigh) *distanceHigh = (LONG)(pos >> 32);
	return (DWORD)pos;
}

BOOL GetFileSizeEx(HANDLE hh, LARGE_INTEGER* size)
{
	FileHandle* h = (FileHandle*)hh;
	if (!h || hh == INVALID_HANDLE_VALUE || h->kind != kFile) return FALSE;
	sysFSStat st;
	if (sysFsFstat(h->fd, &st) != 0) return FALSE;
	size->QuadPart = (LONGLONG)st.st_size;
	return TRUE;
}

DWORD GetFileSize(HANDLE hh, LPDWORD sizeHigh)
{
	LARGE_INTEGER li;
	if (!GetFileSizeEx(hh, &li)) return INVALID_FILE_SIZE;
	if (sizeHigh) *sizeHigh = li.HighPart;
	return li.LowPart;
}

BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED ov, LPDWORD transferred, BOOL)
{
	// Reads complete synchronously, so the result is always ready.
	if (transferred) *transferred = ov ? (DWORD)ov->InternalHigh : 0;
	return TRUE;
}

DWORD GetLastError() { return s_lastError; }
void SetLastError(DWORD e) { s_lastError = e; }

DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD size)
{
	int n = snprintf(buf, size, "%s/EBOOT.BIN", Ps3Fs::AppDir());
	return n < 0 ? 0 : (DWORD)n;
}

BOOL PathRemoveFileSpecA(LPSTR path)
{
	char* a = strrchr(path, '/');
	char* b = strrchr(path, '\\');
	char* cut = a > b ? a : b;
	if (!cut) return FALSE;
	*cut = '\0';
	return TRUE;
}


// ---------------------------------------------------------------------------
// Directories, attributes, times
// ---------------------------------------------------------------------------
namespace
{
	// Windows FILETIME: 100 ns ticks since 1601-01-01.
	void UnixToFileTime(time_t t, FILETIME* ft)
	{
		ULONGLONG v = ((ULONGLONG)t + 11644473600ULL) * 10000000ULL;
		ft->dwLowDateTime = (DWORD)v;
		ft->dwHighDateTime = (DWORD)(v >> 32);
	}

	bool WildMatch(const char* pat, const char* s)
	{
		// Case-insensitive * and ? (Windows also lets "*.*" match names without dots).
		if (!strcmp(pat, "*.*")) return true;
		while (*pat)
		{
			if (*pat == '*')
			{
				while (*pat == '*') ++pat;
				if (!*pat) return true;
				for (; *s; ++s)
					if (WildMatch(pat, s)) return true;
				return false;
			}
			if (!*s) return false;
			if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return false;
			++pat; ++s;
		}
		return *s == 0;
	}

	struct FindHandle
	{
		uint32_t kind;
		std::string dir;
		std::vector<std::string> names;
		size_t next;
	};
	const uint32_t kFind = 0x46494e44;

	bool FillFindData(FindHandle* f, LPWIN32_FIND_DATAA data)
	{
		if (f->next >= f->names.size()) return false;
		const std::string& name = f->names[f->next++];
		memset(data, 0, sizeof(*data));
		snprintf(data->cFileName, sizeof(data->cFileName), "%s", name.c_str());
		std::string full = f->dir + "/" + name;
		sysFSStat st;
		if (sysFsStat(full.c_str(), &st) == 0)
		{
			data->dwFileAttributes = ((st.st_mode & 0170000) == 0040000) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
			data->nFileSizeLow = (DWORD)st.st_size;
			data->nFileSizeHigh = (DWORD)(st.st_size >> 32);
			UnixToFileTime(st.st_mtime, &data->ftLastWriteTime);
			UnixToFileTime(st.st_ctime, &data->ftCreationTime);
			UnixToFileTime(st.st_atime, &data->ftLastAccessTime);
		}
		return true;
	}
}

BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS, LPVOID info)
{
	char path[1024];
	sysFSStat st;
	if (!Ps3Fs::ResolvePath(name, path, sizeof(path)) || sysFsStat(path, &st) != 0)
	{
		s_lastError = ERROR_FILE_NOT_FOUND;
		return FALSE;
	}
	WIN32_FILE_ATTRIBUTE_DATA* d = (WIN32_FILE_ATTRIBUTE_DATA*)info;
	memset(d, 0, sizeof(*d));
	d->dwFileAttributes = ((st.st_mode & 0170000) == 0040000) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	d->nFileSizeLow = (DWORD)st.st_size;
	d->nFileSizeHigh = (DWORD)(st.st_size >> 32);
	UnixToFileTime(st.st_mtime, &d->ftLastWriteTime);
	UnixToFileTime(st.st_ctime, &d->ftCreationTime);
	UnixToFileTime(st.st_atime, &d->ftLastAccessTime);
	return TRUE;
}

DWORD GetFileAttributesA(LPCSTR name)
{
	WIN32_FILE_ATTRIBUTE_DATA d;
	return GetFileAttributesExA(name, GetFileExInfoStandard, &d) ? d.dwFileAttributes : INVALID_FILE_ATTRIBUTES;
}

BOOL GetDiskFreeSpaceExA(LPCSTR, ULARGE_INTEGER* freeToCaller, ULARGE_INTEGER* total, ULARGE_INTEGER* totalFree)
{
	u32 blockSize = 0;
	u64 freeBlocks = 0;
	if (sysFsGetFreeSize("/dev_hdd0", &blockSize, &freeBlocks) != 0)
		return FALSE;
	ULONGLONG bytes = (ULONGLONG)blockSize * freeBlocks;
	if (freeToCaller) freeToCaller->QuadPart = bytes;
	if (totalFree) totalFree->QuadPart = bytes;
	if (total) total->QuadPart = bytes;	// lv2 doesn't report the disk size here; the game only needs free space
	return TRUE;
}

HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
	// Split "dir\mask" and list dir.
	std::string p(pattern);
	for (size_t i = 0; i < p.size(); ++i) if (p[i] == '\\') p[i] = '/';
	size_t slash = p.rfind('/');
	std::string dir = slash == std::string::npos ? std::string(".") : p.substr(0, slash);
	std::string mask = slash == std::string::npos ? p : p.substr(slash + 1);

	char real[1024];
	if (!Ps3Fs::ResolvePath(dir.c_str(), real, sizeof(real)))
		return INVALID_HANDLE_VALUE;

	// Unlike Win32, "." and ".." aren't returned: the game's only caller (the save list,
	// Sys/MemCard/Win32/p_McMan.cpp) would show them as saves.
	FindHandle* f = new FindHandle;
	f->kind = kFind;
	f->dir = real;
	f->next = 0;
	std::vector<std::string> all;
	Ps3Fs::ListDirectory(real, all);	// empty when the folder doesn't exist
	for (size_t i = 0; i < all.size(); ++i)
		if (WildMatch(mask.c_str(), all[i].c_str()))
			f->names.push_back(all[i]);
	if (!FillFindData(f, data))
	{
		delete f;
		s_lastError = ERROR_FILE_NOT_FOUND;
		return INVALID_HANDLE_VALUE;
	}
	return f;
}

BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data)
{
	FindHandle* f = (FindHandle*)h;
	if (!f || h == INVALID_HANDLE_VALUE || f->kind != kFind) return FALSE;
	if (!FillFindData(f, data)) { s_lastError = ERROR_NO_MORE_FILES; return FALSE; }
	return TRUE;
}

BOOL FindClose(HANDLE h)
{
	FindHandle* f = (FindHandle*)h;
	if (!f || h == INVALID_HANDLE_VALUE || f->kind != kFind) return FALSE;
	delete f;
	return TRUE;
}

BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES)
{
	char path[1024];
	if (!Ps3Fs::ResolvePath(name, path, sizeof(path))) return FALSE;
	BOOL ok = sysFsMkdir(path, 0755) == 0;
	Ps3Fs::InvalidateParent(path);
	return ok;
}

BOOL DeleteFileA(LPCSTR name)
{
	char path[1024];
	if (!Ps3Fs::ResolvePath(name, path, sizeof(path))) return FALSE;
	BOOL ok = sysFsUnlink(path) == 0;
	Ps3Fs::InvalidateParent(path);
	return ok;
}

BOOL FlushFileBuffers(HANDLE)
{
	return TRUE;	// lv2 file writes aren't cached by us
}

BOOL FileTimeToLocalFileTime(const FILETIME* in, LPFILETIME out)
{
	*out = *in;	// the console's clock is already local time
	return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME* in, LPSYSTEMTIME out)
{
	ULONGLONG v = ((ULONGLONG)in->dwHighDateTime << 32) | in->dwLowDateTime;
	time_t t = (time_t)(v / 10000000ULL - 11644473600ULL);
	struct tm tmv;
	gmtime_r(&t, &tmv);
	out->wYear = (WORD)(tmv.tm_year + 1900);
	out->wMonth = (WORD)(tmv.tm_mon + 1);
	out->wDayOfWeek = (WORD)tmv.tm_wday;
	out->wDay = (WORD)tmv.tm_mday;
	out->wHour = (WORD)tmv.tm_hour;
	out->wMinute = (WORD)tmv.tm_min;
	out->wSecond = (WORD)tmv.tm_sec;
	out->wMilliseconds = (WORD)((v / 10000ULL) % 1000ULL);
	return TRUE;
}

// ---------------------------------------------------------------------------
// Winsock
// ---------------------------------------------------------------------------
int WSAStartup(WORD version, LPWSADATA data)
{
	// Report the version asked for: Net::Manager checks for 2.2 and, if it doesn't match,
	// returns from its constructor before setting the bandwidth, connection type and app
	// count (a bandwidth of 0 held every message back, so the front end's skater was never made).
	if (data)
	{
		memset(data, 0, sizeof(*data));
		data->wVersion = version;
		data->wHighVersion = version;
	}
	return 0;	// the network module is started by the platform layer
}

int WSACleanup() { return 0; }

int WSAGetLastError() { return net_errno; }
