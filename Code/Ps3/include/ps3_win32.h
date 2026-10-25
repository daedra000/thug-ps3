// THUG PS3: the handful of Win32 calls the PC code paths make (threads, mutexes,
// events, multimedia timers, file handles, Winsock bits), implemented on lv2 in
// Code/Ps3/sys/ps3_win32.cpp.
#ifndef PS3_WIN32_H
#define PS3_WIN32_H

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
void  Sleep(DWORD ms);
DWORD GetTickCount();
DWORD timeGetTime();
BOOL  QueryPerformanceCounter(LARGE_INTEGER* count);
BOOL  QueryPerformanceFrequency(LARGE_INTEGER* freq);
static inline MMRESULT timeBeginPeriod(UINT) { return 0; }
static inline MMRESULT timeEndPeriod(UINT) { return 0; }

// Multimedia timers: a periodic or one-shot callback on its own thread.
typedef void (CALLBACK *LPTIMECALLBACK)(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2);
#define TIME_ONESHOT          0x0000
#define TIME_PERIODIC         0x0001
#define TIME_CALLBACK_FUNCTION 0x0000
#define TIMERR_NOERROR        0
MMRESULT timeSetEvent(UINT delayMs, UINT resolution, LPTIMECALLBACK cb, DWORD_PTR user, UINT flags);
MMRESULT timeKillEvent(UINT id);

// ---------------------------------------------------------------------------
// Synchronisation / threads. HANDLEs point at a small object owned by the shim.
// ---------------------------------------------------------------------------
#define INFINITE        0xFFFFFFFF
#define WAIT_OBJECT_0   0x00000000L
#define WAIT_TIMEOUT    0x00000102L
#define WAIT_FAILED     0xFFFFFFFFL
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL initialOwner, LPCSTR name);
BOOL   ReleaseMutex(HANDLE h);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manualReset, BOOL initialState, LPCSTR name);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES, size_t stackSize, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD threadId);
DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
BOOL   CloseHandle(HANDLE h);
DWORD  GetCurrentThreadId();
#define CreateMutex CreateMutexA
#define CreateEvent CreateEventA

typedef struct _CRITICAL_SECTION { void* impl; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);

static inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p, 1); }
static inline LONG InterlockedDecrement(volatile LONG* p) { return __sync_sub_and_fetch(p, 1); }
static inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __sync_lock_test_and_set(p, v); }

// ---------------------------------------------------------------------------
// Files (synchronous; OVERLAPPED is accepted and treated as a plain offset)
// ---------------------------------------------------------------------------
typedef struct _OVERLAPPED {
	ULONG_PTR Internal, InternalHigh;
	union { struct { DWORD OffsetHigh; DWORD Offset; }; PVOID Pointer; };	// big-endian order
	HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

#define GENERIC_READ            0x80000000L
#define GENERIC_WRITE           0x40000000L
#define FILE_SHARE_READ         0x00000001
#define FILE_SHARE_WRITE        0x00000002
#define CREATE_NEW              1
#define CREATE_ALWAYS           2
#define OPEN_EXISTING           3
#define OPEN_ALWAYS             4
#define TRUNCATE_EXISTING       5
#define FILE_ATTRIBUTE_NORMAL   0x00000080
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_FLAG_OVERLAPPED    0x40000000
#define FILE_FLAG_NO_BUFFERING  0x20000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_BEGIN              0
#define FILE_CURRENT            1
#define FILE_END                2
#define INVALID_FILE_SIZE       ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define ERROR_SUCCESS           0L
#define ERROR_FILE_NOT_FOUND    2L
#define ERROR_HANDLE_EOF        38L
#define ERROR_IO_PENDING        997L
#define ERROR_IO_INCOMPLETE     996L

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags, HANDLE templ);
BOOL   ReadFile(HANDLE h, LPVOID buf, DWORD toRead, LPDWORD read, LPOVERLAPPED ov);
BOOL   WriteFile(HANDLE h, LPCVOID buf, DWORD toWrite, LPDWORD written, LPOVERLAPPED ov);
DWORD  SetFilePointer(HANDLE h, LONG distance, LONG* distanceHigh, DWORD method);
BOOL   GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD transferred, BOOL wait);
BOOL   GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
#define HasOverlappedIoCompleted(ov) (TRUE)	// reads complete synchronously
DWORD  GetFileSize(HANDLE h, LPDWORD sizeHigh);
static inline char* StrStrIA(const char* hay, const char* needle) { return (char*)strcasestr(hay, needle); }
#define CreateFile CreateFileA

typedef union _ULARGE_INTEGER {
	struct { DWORD HighPart; DWORD LowPart; };	// big-endian layout
	ULONGLONG QuadPart;
} ULARGE_INTEGER;
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
	DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
	DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef struct _WIN32_FIND_DATAA {
	DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
	DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
	CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, _WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
#define ERROR_DISK_FULL         112L
#define ERROR_NO_MORE_FILES     18L
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

BOOL   GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS, LPVOID info);
DWORD  GetFileAttributesA(LPCSTR name);
BOOL   GetDiskFreeSpaceExA(LPCSTR dir, ULARGE_INTEGER* freeToCaller, ULARGE_INTEGER* total, ULARGE_INTEGER* totalFree);
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
BOOL   FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data);
BOOL   FindClose(HANDLE h);
BOOL   CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES);
BOOL   DeleteFileA(LPCSTR name);
BOOL   FlushFileBuffers(HANDLE h);
BOOL   FileTimeToLocalFileTime(const FILETIME* in, LPFILETIME out);
BOOL   FileTimeToSystemTime(const FILETIME* in, LPSYSTEMTIME out);
#define GetFileAttributesEx GetFileAttributesExA
#define GetFileAttributes   GetFileAttributesA
#define GetDiskFreeSpaceEx  GetDiskFreeSpaceExA
#define FindFirstFile       FindFirstFileA
#define FindNextFile        FindNextFileA
#define CreateDirectory     CreateDirectoryA
#define DeleteFile          DeleteFileA
#define WIN32_FIND_DATA     WIN32_FIND_DATAA

DWORD GetLastError();
void  SetLastError(DWORD e);

// Path of the running EBOOT (USRDIR/EBOOT.BIN); the game data sits next to it.
DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD size);
BOOL  PathRemoveFileSpecA(LPSTR path);
#define GetModuleFileName GetModuleFileNameA
#define PathRemoveFileSpec PathRemoveFileSpecA

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------
#define wsprintfA sprintf
#define wsprintf  sprintf

#define SPI_GETSCREENSAVETIMEOUT 0x000E
#define SPI_SETSCREENSAVETIMEOUT 0x000F
#define SPI_GETSCREENSAVEACTIVE  0x0010
#define SPI_SETSCREENSAVEACTIVE  0x0011
#define SPIF_SENDCHANGE          0x0002
static inline BOOL SystemParametersInfoA(UINT, UINT, PVOID p, UINT) { if (p) *(UINT*)p = 0; return TRUE; }
#define SystemParametersInfo SystemParametersInfoA

#define ES_SYSTEM_REQUIRED  0x00000001
#define ES_DISPLAY_REQUIRED 0x00000002
#define ES_CONTINUOUS       0x80000000
static inline DWORD SetThreadExecutionState(DWORD) { return 0; }
static inline int ShowCursor(BOOL) { return 0; }
#define MB_OK 0
static inline int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT) { fprintf(stderr, "%s: %s\n", caption ? caption : "", text ? text : ""); return 1; }
#define MessageBox MessageBoxA

// ---------------------------------------------------------------------------
// Winsock leftovers (the socket calls themselves are BSD, see gel/net/net.h)
// ---------------------------------------------------------------------------
typedef struct WSAData { WORD wVersion, wHighVersion; char szDescription[257]; char szSystemStatus[129]; } WSADATA, *LPWSADATA;
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
int WSAStartup(WORD version, LPWSADATA data);
int WSACleanup();
int WSAGetLastError();
#define WSAEWOULDBLOCK  35		// EWOULDBLOCK on lv2
#define WSAEINPROGRESS  36
#define WSAEADDRINUSE   48
#define WSAECONNRESET   54
#define WSAEMSGSIZE     40
#define SOCKET_ERROR    (-1)
#define INVALID_SOCKET  (-1)

#endif // PS3_WIN32_H
