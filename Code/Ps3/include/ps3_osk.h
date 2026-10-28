// THUG PS3: the system on-screen keyboard (sysutil OSK) for the game's text entry.
//
// The PC game's keyboard dialog (create_onscreen_keyboard) has no key grid: it shows the text
// and waits for typed keys, which SIO::KeyboardRead hands to the handle_keyboard_input script.
// On PS3 the dialog's keyboard task opens the system keyboard instead (Poll), and once it
// closes, Read returns the result as typed keys: backspaces for the text the dialog started
// with, the new text, and Enter (which runs the dialog's keyboard_done). Cancelling keeps the
// old text (Enter alone). The approach and the memory handling follow Grid Siege's OSK.
#ifndef PS3_OSK_H
#define PS3_OSK_H

namespace Ps3Osk
{
	// Called every frame by the game's keyboard task while its dialog is up: opens the system
	// keyboard if it isn't open (and wasn't just closed), with `initial` as its starting text.
	void Poll(int maxLength, const char* title, const char* initial);

	// The keys to hand to the game this frame (at most `size`), SIO::vKB_* codes for Enter and
	// Backspace; 0 while the keyboard is open or nothing is waiting.
	int Read(char* makes, int size);

	// The game's dialog went away (StopKeyboardHandler): close the system keyboard if it is open.
	void Close();

	// True while the system keyboard is up: the game gets no pad input meanwhile.
	bool Active();

	// From the sysutil callback (ps3_main.cpp).
	void OnSysutil(unsigned long long status);
}

#endif
