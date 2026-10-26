// THUG PS3: audio output. Replaces Gel/SoundFX/SDL/Mixer.cpp: the game's mixer
// (music streams + sound effects) renders 5-channel float audio at 48 kHz, which
// a PPU thread copies into an 8-channel libaudio port. The PS3 downmixes to
// whatever the audio output setting is (stereo, 5.1, 7.1).

#include <Core/Defines.h>
#include <Gel/SoundFX/SDL/Mixer.h>
#include <Gel/SoundFX/SDL/p_sfx.h>
#include <Gel/Music/SDL/p_music.h>
#include <ps3_platform.h>

#include <sys/mutex.h>
#include <sys/thread.h>

#if !PS3_HEADLESS
#include <audio/audio.h>
#include <sys/event_queue.h>
#endif

namespace Audio
{

static sys_mutex_t s_lock;
static bool s_started;

// Priority inheritance: the game thread (priority 1000) takes this lock to start and stop
// sounds, and the mixer thread (200) waits for it every block. Without inheritance, a thread
// between the two could keep the game thread from releasing it while the mixer starves.
static void CreateLock()
{
	sys_mutex_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO_INHERIT;
	attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strcpy(attr.name, "audmix");
	if (sysMutexCreate(&s_lock, &attr) != 0)
	{
		printf("[audio] no priority-inheriting mutex, using a plain one\n");
		attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
		sysMutexCreate(&s_lock, &attr);
	}
}

void Lock()
{
	if (s_started) sysMutexLock(s_lock, 0);
}

void Unlock()
{
	if (s_started) sysMutexUnlock(s_lock);
}

#if PS3_HEADLESS

void Init()
{
	if (s_started) return;
	CreateLock();
	s_started = true;
	printf("[audio] headless build: no audio output\n");
}

void Shutdown(bool)
{
}

#else

// kAhead: blocks (256 samples, 5.3 ms each) kept queued ahead of playback, out of the port's
// 16. The mixer no longer reads or decodes anything itself (Gel/Music/SDL/p_music.cpp's
// streaming thread does), so this only has to cover scheduling delays.
enum { kInChannels = 5, kOutChannels = 8, kAhead = 8 };

static u32 s_port;
static audioPortConfig s_config;
static sys_event_queue_t s_queue;
static u64 s_key;
static sys_ppu_thread_t s_thread;
static bool s_threadRunning;
static bool s_portOpen;
static volatile bool s_quit;
static float s_mix[AUDIO_BLOCK_SAMPLES * kInChannels];

static float* Block(u32 index)
{
	return (float*)(u64)s_config.audioDataStart + (u64)index * AUDIO_BLOCK_SAMPLES * s_config.channelCount;
}

static void ClearBlock(u32 index)
{
	memset(Block(index), 0, AUDIO_BLOCK_SAMPLES * s_config.channelCount * sizeof(float));
}

static void Fill(float* out)
{
	memset(s_mix, 0, sizeof(s_mix));
	sysMutexLock(s_lock, 0);
	Pcm::PCMAudio_Mix((char*)s_mix, sizeof(s_mix));
	Sfx::MixSoundFX((char*)s_mix, sizeof(s_mix));
	sysMutexUnlock(s_lock);

	// Game order FL FR C BL BR -> PS3 order L R C LFE Ls Rs Lb Rb.
	const float* in = s_mix;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i, in += kInChannels, out += kOutChannels)
	{
		out[0] = in[0];
		out[1] = in[1];
		out[2] = in[2];
		out[3] = 0.0f;
		out[4] = in[3];
		out[5] = in[4];
		out[6] = 0.0f;
		out[7] = 0.0f;
	}
}

// The port plays its ring of blocks round and round whether or not they were refilled, so a
// block that has been played is cleared at once: if the mixer is late, the port plays silence
// instead of the same audio again. If it is so late that the port has gone past the blocks it
// had filled, every block is cleared before it starts again.
static void MixThread(void*)
{
	Ps3Platform::AttachThreadLog();
	u32 blocks = (u32)s_config.numBlocks;
	u32 next = 1;
	u32 last = 0;	// the block that was playing last time
	sys_event_t ev;
	while (!s_quit)
	{
		// One event per block played; the timeout only matters for quitting.
		sysEventQueueReceive(s_queue, &ev, 20 * 1000);
		if (s_quit) break;
		u32 playing = (u32)(*(volatile u64*)(u64)s_config.readIndex % blocks);
		u32 dist = (next + blocks - playing) % blocks;
		if (dist == 0 || dist > kAhead + 1)
		{
			// Fell behind: the port is past what was filled. Clear the whole ring (including
			// the block playing now, which holds old audio) and restart just after the play
			// position.
			for (u32 i = 0; i < blocks; ++i)
				ClearBlock(i);
			next = (playing + 1) % blocks;
			dist = 1;
		}
		else
		{
			for (u32 i = last; i != playing; i = (i + 1) % blocks)
				ClearBlock(i);
		}
		last = playing;
		while (dist <= kAhead)
		{
			Fill(Block(next));
			next = (next + 1) % blocks;
			++dist;
		}
	}
	sysThreadExit(0);
}

void Init()
{
	if (s_started) return;
	CreateLock();
	s_started = true;

	if (audioInit() != 0)
	{
		printf("[audio] audioInit failed: no sound\n");
		return;
	}
	audioPortParam param;
	memset(&param, 0, sizeof(param));
	param.numChannels = AUDIO_PORT_8CH;
	param.numBlocks = AUDIO_BLOCK_16;
	param.attrib = 0;
	param.level = 1.0f;
	if (audioPortOpen(&param, &s_port) != 0)
	{
		printf("[audio] audioPortOpen failed: no sound\n");
		audioQuit();
		return;
	}
	audioGetPortConfig(s_port, &s_config);
	audioCreateNotifyEventQueue(&s_queue, &s_key);
	audioSetNotifyEventQueue(s_key);
	sysEventQueueDrain(s_queue);
	memset((void*)(u64)s_config.audioDataStart, 0, s_config.portSize);
	audioPortStart(s_port);
	s_portOpen = true;

	s_quit = false;
	if (Ps3Platform::NoThreads())
	{
		printf("[audio] nothreads.txt: no mixer thread, so no sound\n");
		return;
	}
	// Above the game thread's priority so audio keeps up while levels load.
	// 256 KB: the mixer runs the game's mixing and the Bink decoder.
	if (sysThreadCreate(&s_thread, MixThread, NULL, 200, 256 * 1024, THREAD_JOINABLE, (char*)"thugaudio") != 0)
		printf("[audio] mixer thread failed to start\n");
	else
	{
		s_threadRunning = true;
		printf("[audio] %u-channel port, %u blocks\n", (unsigned)s_config.channelCount, (unsigned)s_config.numBlocks);
	}
}

void Shutdown(bool wait)
{
	s_quit = true;
	if (s_threadRunning && wait)
	{
		u64 rv;
		sysThreadJoin(s_thread, &rv);	// it wakes at least every 20 ms
		s_threadRunning = false;
	}
	if (s_portOpen)
	{
		s_portOpen = false;
		audioPortStop(s_port);
		if (wait)
		{
			audioPortClose(s_port);
			audioQuit();
		}
	}
}

#endif // PS3_HEADLESS

} // namespace Audio
