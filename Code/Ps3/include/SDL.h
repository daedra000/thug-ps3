// THUG PS3: the slice of SDL2's game-controller API that kisak-thug's input code
// (Sys/SIO/Win32/p_siodev.cpp, p_sioman.cpp) uses, implemented on the PS3 pad
// library in Code/Ps3/sio/ps3_sdl_pad.cpp. Button names follow SDL's controller
// layout: A = Cross, B = Circle, X = Square, Y = Triangle, BACK = Select.
#ifndef PS3_SDL_SHIM_H
#define PS3_SDL_SHIM_H

#include <stdint.h>

typedef uint8_t  Uint8;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int      SDL_bool;

#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u

typedef enum SDL_GameControllerButton
{
	SDL_CONTROLLER_BUTTON_INVALID = -1,
	SDL_CONTROLLER_BUTTON_A,
	SDL_CONTROLLER_BUTTON_B,
	SDL_CONTROLLER_BUTTON_X,
	SDL_CONTROLLER_BUTTON_Y,
	SDL_CONTROLLER_BUTTON_BACK,
	SDL_CONTROLLER_BUTTON_GUIDE,
	SDL_CONTROLLER_BUTTON_START,
	SDL_CONTROLLER_BUTTON_LEFTSTICK,
	SDL_CONTROLLER_BUTTON_RIGHTSTICK,
	SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
	SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
	SDL_CONTROLLER_BUTTON_DPAD_UP,
	SDL_CONTROLLER_BUTTON_DPAD_DOWN,
	SDL_CONTROLLER_BUTTON_DPAD_LEFT,
	SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
	SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

typedef enum SDL_GameControllerAxis
{
	SDL_CONTROLLER_AXIS_INVALID = -1,
	SDL_CONTROLLER_AXIS_LEFTX,
	SDL_CONTROLLER_AXIS_LEFTY,
	SDL_CONTROLLER_AXIS_RIGHTX,
	SDL_CONTROLLER_AXIS_RIGHTY,
	SDL_CONTROLLER_AXIS_TRIGGERLEFT,
	SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
	SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef struct SDL_GameController SDL_GameController;	// opaque: the pad port
typedef union SDL_Event { Uint32 type; Uint8 padding[56]; } SDL_Event;

int  SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char* SDL_GetError(void);

// Reads the pads (once per call): the game calls this every frame.
int SDL_PollEvent(SDL_Event* e);

int  SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int index);
const char* SDL_GameControllerNameForIndex(int index);
SDL_GameController* SDL_GameControllerOpen(int index);
void SDL_GameControllerClose(SDL_GameController* c);
Uint8 SDL_GameControllerGetButton(SDL_GameController* c, SDL_GameControllerButton b);
Sint16 SDL_GameControllerGetAxis(SDL_GameController* c, SDL_GameControllerAxis a);

// PS3 extras for the port: the pad's pressure values (0-255) and vibration.
Uint8 PS3_GameControllerGetPressure(SDL_GameController* c, SDL_GameControllerButton b);
int SDL_GameControllerRumble(SDL_GameController* c, uint16_t low, uint16_t high, Uint32 ms);

#endif // PS3_SDL_SHIM_H
