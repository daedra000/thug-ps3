#pragma once

#include <Core/Defines.h>

#include "miniaudio.h"

namespace Audio
{
	// Mixer API
	void Init();

	void Lock();
	void Unlock();
#ifdef __PLAT_PS3__
	void Shutdown(bool wait);	// stop the mixer thread and the audio port
#endif
}
