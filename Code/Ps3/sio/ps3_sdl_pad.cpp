// THUG PS3: SDL game-controller calls on the PS3 pad library (see Ps3/include/SDL.h).

#include <SDL.h>
#include <ps3_platform.h>

#include <string.h>
#include <stdio.h>
#include <io/pad.h>

namespace
{
	enum { kMaxPads = 7 };

	bool s_init;
	padInfo s_info;
	padData s_data[kMaxPads];	// last data received (ioPadGetData only returns changes)
	bool s_open[kMaxPads];

	// Handles are port + 1, so port 0 isn't a NULL pointer.
	int Port(SDL_GameController* c)
	{
		long p = (long)(uintptr_t)c - 1;
		return (p >= 0 && p < kMaxPads) ? (int)p : -1;
	}

	bool Connected(int port)
	{
		return port >= 0 && s_info.status[port] != 0;
	}
}

int SDL_Init(Uint32 flags)
{
	if ((flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) && !s_init)
	{
		if (ioPadInit(kMaxPads) != 0)
		{
			printf("[pad] ioPadInit failed\n");
			return -1;
		}
		memset(s_data, 0, sizeof(s_data));
		memset(&s_info, 0, sizeof(s_info));
		s_init = true;
		ioPadGetInfo(&s_info);
		printf("[pad] pad library started: %u of %u pads connected\n", (unsigned)s_info.connected, (unsigned)s_info.max);
	}
	return 0;
}

void SDL_Quit(void)
{
	if (s_init)
	{
		ioPadEnd();
		s_init = false;
	}
}

const char* SDL_GetError(void)
{
	return "";
}

int SDL_PollEvent(SDL_Event*)
{
	if (!s_init)
		return 0;

	// Keep the XMB happy and pick up quit requests even while only input is polled.
	Ps3Platform::Pump();

	ioPadGetInfo(&s_info);
	for (int i = 0; i < kMaxPads; ++i)
	{
		if (!s_info.status[i])
		{
			memset(&s_data[i], 0, sizeof(s_data[i]));
			continue;
		}
		padData d;
		if (ioPadGetData(i, &d) == 0 && d.len > 0)
			s_data[i] = d;
	}
	// The game would ignore events anyway; it reads state directly.
	return 0;
}

int SDL_NumJoysticks(void)
{
	// The game opens controllers by port number, so report the ports up to the
	// highest one with a pad (a pad in port 2 alone still shows up as index 1).
	if (!s_init) return 0;
	ioPadGetInfo(&s_info);
	int n = 0;
	for (int i = 0; i < kMaxPads; ++i)
		if (s_info.status[i]) n = i + 1;
	return n;
}

SDL_bool SDL_IsGameController(int index)
{
	return (index >= 0 && index < kMaxPads && s_info.status[index]) ? 1 : 0;
}

const char* SDL_GameControllerNameForIndex(int)
{
	return "DUALSHOCK 3";
}

SDL_GameController* SDL_GameControllerOpen(int index)
{
	if (index < 0 || index >= kMaxPads) return NULL;
	s_open[index] = true;
	ioPadSetPressMode(index, PAD_PRESS_MODE_ON);	// analog button pressures
	return (SDL_GameController*)(uintptr_t)(index + 1);
}

void SDL_GameControllerClose(SDL_GameController* c)
{
	int p = Port(c);
	if (p >= 0) s_open[p] = false;
}

Uint8 SDL_GameControllerGetButton(SDL_GameController* c, SDL_GameControllerButton b)
{
	int p = Port(c);
	if (!Connected(p)) return 0;
	const padData& d = s_data[p];
	switch (b)
	{
	case SDL_CONTROLLER_BUTTON_A:             return d.BTN_CROSS;
	case SDL_CONTROLLER_BUTTON_B:             return d.BTN_CIRCLE;
	case SDL_CONTROLLER_BUTTON_X:             return d.BTN_SQUARE;
	case SDL_CONTROLLER_BUTTON_Y:             return d.BTN_TRIANGLE;
	case SDL_CONTROLLER_BUTTON_BACK:          return d.BTN_SELECT;
	case SDL_CONTROLLER_BUTTON_START:         return d.BTN_START;
	case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return d.BTN_L3;
	case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return d.BTN_R3;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return d.BTN_L1;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return d.BTN_R1;
	case SDL_CONTROLLER_BUTTON_DPAD_UP:       return d.BTN_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return d.BTN_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return d.BTN_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return d.BTN_RIGHT;
	default:                                  return 0;
	}
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController* c, SDL_GameControllerAxis a)
{
	int p = Port(c);
	if (!Connected(p)) return 0;
	const padData& d = s_data[p];
	// Sticks: 0..255 with 128 centred, down/right positive (as SDL).
	switch (a)
	{
	case SDL_CONTROLLER_AXIS_LEFTX:  return (Sint16)(((int)d.ANA_L_H - 128) * 256);
	case SDL_CONTROLLER_AXIS_LEFTY:  return (Sint16)(((int)d.ANA_L_V - 128) * 256);
	case SDL_CONTROLLER_AXIS_RIGHTX: return (Sint16)(((int)d.ANA_R_H - 128) * 256);
	case SDL_CONTROLLER_AXIS_RIGHTY: return (Sint16)(((int)d.ANA_R_V - 128) * 256);
	// Triggers: L2/R2 are pressure-sensitive buttons; 0..32767 like SDL.
	case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
		return d.BTN_L2 ? (Sint16)(d.PRE_L2 ? d.PRE_L2 * 128 + 127 : 32767) : 0;
	case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
		return d.BTN_R2 ? (Sint16)(d.PRE_R2 ? d.PRE_R2 * 128 + 127 : 32767) : 0;
	default: return 0;
	}
}

Uint8 PS3_GameControllerGetPressure(SDL_GameController* c, SDL_GameControllerButton b)
{
	int p = Port(c);
	if (!Connected(p)) return 0;
	const padData& d = s_data[p];
	switch (b)
	{
	case SDL_CONTROLLER_BUTTON_A:             return (Uint8)d.PRE_CROSS;
	case SDL_CONTROLLER_BUTTON_B:             return (Uint8)d.PRE_CIRCLE;
	case SDL_CONTROLLER_BUTTON_X:             return (Uint8)d.PRE_SQUARE;
	case SDL_CONTROLLER_BUTTON_Y:             return (Uint8)d.PRE_TRIANGLE;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return (Uint8)d.PRE_L1;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return (Uint8)d.PRE_R1;
	case SDL_CONTROLLER_BUTTON_DPAD_UP:       return (Uint8)d.PRE_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return (Uint8)d.PRE_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return (Uint8)d.PRE_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return (Uint8)d.PRE_RIGHT;
	default:                                  return 0;
	}
}

int SDL_GameControllerRumble(SDL_GameController* c, uint16_t low, uint16_t high, Uint32)
{
	int p = Port(c);
	if (!Connected(p)) return -1;
	padActParam act;
	memset(&act, 0, sizeof(act));
	act.small_motor = high ? 1 : 0;			// the DS3's small motor is on/off
	act.large_motor = (u8)(low >> 8);
	ioPadSetActDirect(p, &act);
	return 0;
}
