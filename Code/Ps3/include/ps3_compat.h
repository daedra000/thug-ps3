// THUG PS3: stand-ins for the Windows / MSVC types and helpers the PC (kisak-thug)
// code paths use. The PS3 build defines __PLAT_WN32__ (to follow the PC data paths)
// and __PLAT_PS3__ (to override anything Windows-specific).
#ifndef PS3_COMPAT_H
#define PS3_COMPAT_H

#ifndef __PLAT_PS3__
#error "ps3_compat.h is PS3-only"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

// ---------------------------------------------------------------------------
// MSVC keywords
// ---------------------------------------------------------------------------
#define __forceinline   inline __attribute__((always_inline))
#define __cdecl
#define __stdcall
#define __fastcall
#define WINAPI
#define CALLBACK
#define __declspec(x)
#ifndef __int64
#define __int64 long long
#endif
#define _int64 long long

// ---------------------------------------------------------------------------
// Windows base types (sizes match Win32, not LP64: LONG/DWORD are 32-bit)
// ---------------------------------------------------------------------------
typedef int32_t         BOOL;
typedef uint8_t         BYTE;
typedef uint16_t        WORD;
typedef uint32_t        DWORD;
typedef int32_t         LONG;
typedef uint32_t        ULONG;
typedef uint32_t        UINT;
typedef int32_t         INT;
typedef int16_t         SHORT;
typedef uint16_t        USHORT;
typedef float           FLOAT;
typedef char            CHAR;
typedef uint8_t         UCHAR;
typedef int32_t         HRESULT;
typedef long long       LONGLONG;
typedef unsigned long long ULONGLONG;
typedef uintptr_t       UINT_PTR;
typedef intptr_t        INT_PTR;
typedef uintptr_t       DWORD_PTR;
typedef uintptr_t       ULONG_PTR;
typedef intptr_t        LONG_PTR;
typedef void            VOID;
typedef void*           PVOID;
typedef void*           LPVOID;
typedef const void*     LPCVOID;
typedef char*           LPSTR;
typedef const char*     LPCSTR;
typedef DWORD*          LPDWORD;
typedef BYTE*           LPBYTE;
typedef void*           HANDLE;
typedef void*           HWND;
typedef void*           HINSTANCE;
typedef void*           HMODULE;
typedef void*           HDC;
typedef void*           HMONITOR;
typedef uintptr_t       WPARAM;
typedef intptr_t        LPARAM;
typedef intptr_t        LRESULT;
typedef UINT            MMRESULT;
typedef uint16_t        WCHAR;		// UTF-16, as on Windows
typedef WCHAR*          LPWSTR;
typedef const WCHAR*    LPCWSTR;
typedef char*           PSTR;

typedef union _LARGE_INTEGER {
	struct { DWORD HighPart; DWORD LowPart; };   // big-endian layout
	LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct tagRECT  { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagSIZE  { LONG cx, cy; } SIZE;
typedef struct _GUID { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID, IID;
#define REFIID const IID&
#define REFGUID const GUID&

#ifndef TRUE
#define FALSE 0
#define TRUE 1
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260

#define S_OK            ((HRESULT)0)
#define S_FALSE         ((HRESULT)1)
#define E_FAIL          ((HRESULT)0x80004005)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000E)
#define E_INVALIDARG    ((HRESULT)0x80070057)
#define E_NOTIMPL       ((HRESULT)0x80004001)
#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)

#define MAKEFOURCC(a,b,c,d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w) ((BYTE)(((DWORD_PTR)(w) >> 8) & 0xff))
#define ZeroMemory(p, n)        memset((p), 0, (n))
#define CopyMemory(d, s, n)     memcpy((d), (s), (n))
#define FillMemory(d, n, v)     memset((d), (v), (n))

// ---------------------------------------------------------------------------
// MSVC CRT names
// ---------------------------------------------------------------------------
#define _stricmp        strcasecmp
#define stricmp         strcasecmp
#define _strnicmp       strncasecmp
#define strnicmp        strncasecmp
#define _snprintf       snprintf
#define _vsnprintf      vsnprintf
#define sprintf_s(buf, n, ...)  snprintf((buf), (n), __VA_ARGS__)
#define _isnan          isnan
#define _finite         isfinite
#define _alloca         __builtin_alloca
#define _countof(a)     (sizeof(a) / sizeof((a)[0]))

static inline char* _strupr(char* s) { for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p); return s; }
static inline char* _strlwr(char* s) { for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p); return s; }
#define strupr _strupr
#define strlwr _strlwr
static inline char* _itoa(int value, char* buf, int radix)
{
	if (radix == 16) sprintf(buf, "%x", value); else sprintf(buf, "%d", value);
	return buf;
}
#define itoa _itoa

// Debug output goes to the PS3 TTY (visible with ps3load / a debug-capable firmware).
static inline void OutputDebugStringA(const char* s) { fputs(s, stderr); }
#define OutputDebugString OutputDebugStringA

// ---------------------------------------------------------------------------
// Byte order. PC game data is little-endian; the PS3 is big-endian.
// ---------------------------------------------------------------------------
#include "ps3_endian.h"

#endif // PS3_COMPAT_H
