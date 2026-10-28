// THUG PS3: the system on-screen keyboard for the game's text entry (see ps3_osk.h).
#include <ps3_osk.h>
#include <sysutil/osk.h>
#include <sysutil/sysutil.h>
#include <sys/memory.h>
#include <sys/systime.h>
#include <stdio.h>
#include <string.h>

namespace SIO { enum { vKB_BACKSPACE_ = 24, vKB_ENTER_ = 25 }; }	// sys/sio/keyboard.h: vKB_LEFT = 20 ... BACKSPACE, ENTER

namespace Ps3Osk
{
namespace
{
	// 0 idle, 1 open, 2 closed with a result waiting for Read.
	volatile int s_state = 0;
	volatile bool s_loaded = false, s_unloading = false, s_cancelled = false;
	oskCallbackReturnParam s_out;
	oskInputFieldInfo s_field;
	oskParam s_param;
	uint16_t s_title[128], s_init[256], s_text[256];
	sys_mem_container_t s_mem;
	bool s_memOk = false;
	int s_memMb = 0;
	int s_initialLength = 0;	// characters the game's dialog had when the keyboard opened
	int s_cooldown = 0;			// frames before Poll opens it again (after a result or a failure)
	int s_wait = 0;				// frames since it opened without showing up
	volatile unsigned long long s_closedAt = 0;	// when it closed: the pad stays blocked a moment after

	void ToUtf16(const char* s, uint16_t* d, int cap)
	{
		int n = 0;
		for (; *s && n < cap - 1; ++s) d[n++] = (unsigned char)*s;	// the game's text is ASCII
		d[n] = 0;
	}

	// The keyboard's memory comes from outside the game's heap and is held only while it is
	// open; some firmware wants more than 4 MB, so the largest size that fits is taken.
	bool Reserve()
	{
		static const int kSizes[] = { 8, 7, 6, 5, 4, 3 };
		s32 r = 0;
		for (unsigned i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i)
		{
			r = sysMemContainerCreate(&s_mem, (u32)kSizes[i] * 1024 * 1024);
			if (r == 0) { s_memOk = true; s_memMb = kSizes[i]; return true; }
		}
		printf("[osk] no memory for the system keyboard (0x%08x)\n", (unsigned)r);
		return false;
	}

	void Release()
	{
		if (s_memOk) { sysMemContainerDestroy(s_mem); s_memOk = false; }
	}
}

void OnSysutil(unsigned long long status)
{
	if (status != SYSUTIL_OSK_LOADED && status != SYSUTIL_OSK_DONE && status != SYSUTIL_OSK_INPUT_CANCELED &&
		status != SYSUTIL_OSK_UNLOADED)
		return;
	if (s_state != 1)
	{
		// A late one after it was given up on: just close it again.
		if (status == SYSUTIL_OSK_LOADED || status == SYSUTIL_OSK_DONE || status == SYSUTIL_OSK_INPUT_CANCELED)
		{
			oskAbort();
			oskUnloadAsync(&s_out);
		}
		if (status == SYSUTIL_OSK_UNLOADED) Release();
		return;
	}
	if (status == SYSUTIL_OSK_LOADED) s_loaded = true;
	if (status == SYSUTIL_OSK_INPUT_CANCELED)
	{
		oskAbort();
		if (!s_unloading) { s_unloading = true; oskUnloadAsync(&s_out); }
		s_cancelled = true;
	}
	if (status == SYSUTIL_OSK_DONE && !s_unloading) { s_unloading = true; oskUnloadAsync(&s_out); }
	if (status == SYSUTIL_OSK_UNLOADED)
	{
		Release();
		s_closedAt = sysGetSystemTime();
		s_state = 2;
	}
}

void Poll(int maxLength, const char* title, const char* initial)
{
	if (s_state == 1)
	{
		// Opened but never showed up (5 s): give up; the dialog can still be cancelled.
		if (!s_loaded && ++s_wait > 5 * 60)
		{
			printf("[osk] the system keyboard did not appear\n");
			oskAbort();
			oskUnloadAsync(&s_out);
			s_state = 0;
			s_cooldown = 10 * 60;
		}
		return;
	}
	if (s_state != 0) return;
	if (s_cooldown > 0) { --s_cooldown; return; }

	if (maxLength <= 0 || maxLength > 250) maxLength = 250;
	ToUtf16(title && *title ? title : "Enter text", s_title, 128);
	ToUtf16(initial ? initial : "", s_init, 256);
	s_initialLength = initial ? (int)strlen(initial) : 0;
	memset(s_text, 0, sizeof(s_text));
	s_field.message = s_title;
	s_field.startText = s_init;
	s_field.maxLength = maxLength;
	s_param.allowedPanels = OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL;
	s_param.firstViewPanel = OSK_PANEL_TYPE_ALPHABET;
	s_param.controlPoint.x = 0;
	s_param.controlPoint.y = 0;
	s_param.prohibitFlags = OSK_PROHIBIT_RETURN;
	s_out.res = OSK_OK;
	s_out.len = maxLength;
	s_out.str = s_text;
	if (!s_memOk && !Reserve()) { s_cooldown = 10 * 60; return; }
	oskSetInitialInputDevice(OSK_DEVICE_PAD);
	oskSetKeyLayoutOption(OSK_FULLKEY_PANEL);
	oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER | OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER);
	s_state = 1;	// first: the events may come at once
	s_loaded = s_unloading = s_cancelled = false;
	s_wait = 0;
	s32 r = oskLoadAsync(s_mem, &s_param, &s_field);
	if (r != 0)
	{
		s_state = 0;
		Release();
		s_cooldown = 10 * 60;
		printf("[osk] the system keyboard did not open (0x%08x, %d MB)\n", (unsigned)r, s_memMb);
		return;
	}
	printf("[osk] system keyboard opening (%d MB, up to %d characters, starting with \"%s\")\n", s_memMb, maxLength, initial ? initial : "");
}

int Read(char* makes, int size)
{
	if (s_state != 2) return 0;
	s_state = 0;
	s_cooldown = 30;	// if the dialog stays (too short a name), open the keyboard again after this
	if (s_cancelled || s_out.res != OSK_OK)
	{
		printf("[osk] cancelled (result %d)\n", (int)s_out.res);
		return -1;
	}
	char text[256];
	int n = 0;
	for (int i = 0; i < 255 && s_text[i]; ++i)
	{
		const uint16_t c = s_text[i];
		if (c >= 32 && c <= 126) text[n++] = (char)c;	// the game's fonts are ASCII
	}
	while (n > 0 && text[n - 1] == ' ') --n;
	text[n] = 0;
	int k = 0;
	for (int i = 0; i < s_initialLength && k < size - 1; ++i) makes[k++] = SIO::vKB_BACKSPACE_;
	for (int i = 0; i < n && k < size - 1; ++i) makes[k++] = text[i];
	makes[k++] = SIO::vKB_ENTER_;
	printf("[osk] entered \"%s\"\n", text);
	return k;
}

void Close()
{
	if (s_state == 1)
	{
		oskAbort();
		oskUnloadAsync(&s_out);
		s_state = 0;
	}
	else if (s_state == 2)
		s_state = 0;
	s_cooldown = 0;
}

bool Active()
{
	// Also for 0.4 s after it closes: the button that confirmed it may still be held, and would
	// otherwise reach the game's next menu.
	return s_state == 1 || (s_closedAt && sysGetSystemTime() - s_closedAt < 400000);
}

}	// namespace Ps3Osk
