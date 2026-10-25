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

static void CreateLock()
{
	sys_mutex_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strcpy(attr.name, "audmix");
	sysMutexCreate(&s_lock, &attr);
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

#else

enum { kInChannels = 5, kOutChannels = 8, kAhead = 3 };

static u32 s_port;
static audioPortConfig s_config;
static sys_event_queue_t s_queue;
static u64 s_key;
static sys_ppu_thread_t s_thread;
static volatile bool s_quit;
static float s_mix[AUDIO_BLOCK_SAMPLES * kInChannels];

static float* Block(u32 index)
{
	return (float*)(u64)s_config.audioDataStart + (u64)index * AUDIO_BLOCK_SAMPLES * s_config.channelCount;
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

static void MixThread(void*)
{
	u32 blocks = (u32)s_config.numBlocks;
	u32 next = 1;
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
			next = (playing + 1) % blocks;	// fell behind: restart just after the play position
			dist = 1;
		}
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

	s_quit = false;
	// Above the game thread's priority so audio keeps up while levels load.
	if (sysThreadCreate(&s_thread, MixThread, NULL, 200, 64 * 1024, THREAD_JOINABLE, (char*)"thugaudio") != 0)
		printf("[audio] mixer thread failed to start\n");
	else
		printf("[audio] %u-channel port, %u blocks\n", (unsigned)s_config.channelCount, (unsigned)s_config.numBlocks);
}

#endif // PS3_HEADLESS

} // namespace Audio
