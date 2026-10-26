#include <Core/macros.h>
#include <Core/Defines.h>
#include <Core/allmath.h>
#include <Core/crc.h>

#include "p_music.h"

#include "usersoundtrack.h"

#include <Sys/Config/config.h>
#include <Gel/SoundFX/soundfx.h>
#include <Gel/Scripting/checksum.h>

#include <Gel/SoundFX/SDL/Mixer.h>

#include <Sys/File/filesys.h>
#ifdef __PLAT_PS3__
#include <ps3_platform.h>
#include <bink_audio.h>
#include "stream_ring.h"
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/thread.h>
#endif
#include "usersoundtrack.h"	// stdfs (std::filesystem, or the PS3 subset)

#ifndef __PLAT_PS3__	// unused, and no std::thread on PS3
#include <mutex>
#include <thread>
#endif
#include <functional>
#include <fstream>
#ifdef __PLAT_PS3__
#include <sstream>
#endif

#ifndef __PLAT_PS3__
#include <Shlwapi.h>
#endif

namespace File
{
static stdfs::path DataPath()
{
	char		nameConversionBuffer[256];
	GetModuleFileNameA(NULL, nameConversionBuffer, 255);
	PathRemoveFileSpecA(nameConversionBuffer);
	strcat(nameConversionBuffer, "\\Data");

	return nameConversionBuffer;
}
static stdfs::path ModulePath()
{
	char		nameConversionBuffer[256];
	GetModuleFileNameA(NULL, nameConversionBuffer, 255);
	PathRemoveFileSpecA(nameConversionBuffer);

	return nameConversionBuffer;
}
}

namespace Pcm
{
	// Volume constants
	static constexpr float MUSIC_VOLUME = 1.0f;
	static constexpr float STREAM_VOLUME = 0.5f; // Streams are too loud, ripped from PS2

	// Music decoder using miniaudio
	class Streamer
	{
		private:
#ifndef __PLAT_PS3__
			std::vector<char> buffer;
#endif

			bool paused = true;
			bool playing = true;

			float volume[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

		public:
			virtual ~Streamer() = default;

			virtual void Rewind() = 0;
#ifdef __PLAT_PS3__
			// The PS3 decodes on the streaming thread (below), ahead of the mixer, into ring.
			// Decode writes up to `frames` frames of ring.Channels() floats each and returns how
			// many it wrote; fewer means the end of the stream. Rewind goes back to the start.
			// Both are only called by the thread filling the ring.
			StreamRing ring;
			virtual size_t Decode(float *out, size_t frames) = 0;

			// Decodes one piece into the ring if it has room for one (or a rewind was asked
			// for). Returns true if it did anything.
			bool Fill()
			{
				if (!ring.Valid())
					return false;
				bool did = false;
				if (ring.RestartPending())
				{
					Rewind();
					ring.Restart();
					did = true;
				}
				if (ring.AtEnd() || ring.FreeFrames() < kFillMin)
					return did;
				uint32_t n = 0;
				float *at = ring.WriteSpace(StreamRing::kChunk, &n);
				if (n == 0)
					return did;
				size_t got = Decode(at, n);
				ring.Commit((uint32_t)got);
				if (got < n)
					ring.SetEnd();
				return true;
			}

			// Ring size in frames: about 0.37 s at 44.1 kHz, so a slow disk read on the
			// streaming thread doesn't starve the mixer. The thread decodes into it again once
			// kFillMin frames are free.
			enum { kRingFrames = 16384, kFillMin = 1024 };
#else
			virtual size_t Stream(char *p, size_t bytes) = 0;
#endif

			size_t Mix(char *out, size_t bytes)
			{
				// If stopped or paused, return 0
				if (!playing || paused)
					return 0;

#ifdef __PLAT_PS3__
				// Only reads what the streaming thread has decoded: never waits for it.
				if (!ring.Valid())
				{
					playing = false;	// nothing to play
					return 0;
				}
				bool ended = false;
				size_t frames = ring.MixInto((float *)out, bytes / (sizeof(float) * 5), volume, &ended);
				if (ended)
					playing = false;
				return frames * (sizeof(float) * 5);
#else
				// Read in buffer
				if (buffer.size() < bytes)
					buffer.resize(bytes);

				size_t read = Stream(buffer.data(), bytes);
				if (read < bytes)
					playing = false;

				// Accumulate
				float *a = (float*)out;
				float *b = (float*)buffer.data();
				for (size_t i = 0; i < read / sizeof(float) / 5; i++)
				{
					for (int c = 0; c < 5; c++)
						*a++ += *b++ * volume[c];
				}

				return read;
#endif
			}

			void Play()
			{
				paused = false;
			}

			void Pause()
			{
				paused = true;
			}

			void Stop()
			{
#ifdef __PLAT_PS3__
				ring.RequestRestart();	// the decoder belongs to the streaming thread
#else
				Rewind();
#endif
				paused = true;
				playing = true;
			}

			bool IsPaused() const { return paused; }
			bool IsPlaying() const { return playing; }

			void SetVolume(float *coefs)
			{
				for (int i = 0; i < 5; i++)
					volume[i] = coefs[i];
			}
	};

	class MusicDecoder : public Streamer
	{
		private:
			ma_decoder decoder = {};
			bool decoder_ok = false;	// ma_decoder_init succeeded (only then Stream/Rewind/uninit)
			std::ifstream file;

		public:
#ifdef __PLAT_PS3__
			// The PC data's music and streams are audio-only Bink files, which miniaudio
			// can't decode (Code/Ps3/audio/bink_audio.cpp does).
			BinkAudio *bink = nullptr;

			// A stream held in memory (one entry of streams/pcm/pcm.wad), instead of a file.
			std::istringstream mem;
			bool from_mem = false;

			bool OpenBink(std::istream &in)
			{
				char magic[3] = {};
				in.read(magic, 3);
				in.clear();
				in.seekg(0);
				if (magic[0] != 'B' || magic[1] != 'I' || magic[2] != 'K')
					return false;
				bink = new BinkAudio();
				if (!bink->Open(&in))
				{
					printf("[music] unsupported Bink file\n");
					delete bink;
					bink = nullptr;
					file.close();
					from_mem = false;
					return true;
				}
				if (!ring.Init(bink->Channels(), bink->SampleRate(), kRingFrames))
					printf("[music] no memory for a stream buffer\n");
				return true;
			}
#endif

#ifdef __PLAT_PS3__
			MusicDecoder(const std::string &bytes) : mem(bytes)
			{
				from_mem = true;
				if (!OpenBink(mem))
					from_mem = false;	// not Bink: nothing to play
			}

			bool IsOpen() const { return file.is_open() || from_mem; }

			// unbuffered: for a movie's sound track, whose frames are mostly video. The decoder
			// reads only each frame's audio packet, and a buffered stream would read on into
			// the video data around it.
			MusicDecoder(stdfs::path path, bool unbuffered = false)
			{
				if (unbuffered)
					file.rdbuf()->pubsetbuf(nullptr, 0);
				file.open(path, std::ios::binary);
#else
			bool IsOpen() const { return file.is_open(); }

			MusicDecoder(stdfs::path path) : file(path, std::ios::binary)
			{
#endif
				if (!file.is_open())
					return;
#ifdef __PLAT_PS3__
				if (OpenBink(file))
					return;
#endif

				ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 5, 48000);
				decoder_ok = MA_SUCCESS == ma_decoder_init(
					[](ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead, size_t *pBytesRead) -> ma_result
					{
						MusicDecoder *self = (MusicDecoder*)pDecoder->pUserData;
						self->file.read((char *)pBufferOut, bytesToRead);
						size_t got = (size_t)self->file.gcount();
						if (pBytesRead != nullptr)
							*pBytesRead = got;
						return got == 0 && bytesToRead > 0 ? MA_AT_END : MA_SUCCESS;
					},
					[](ma_decoder *pDecoder, ma_int64 byteOffset, ma_seek_origin origin) -> ma_result
					{
						MusicDecoder *self = (MusicDecoder*)pDecoder->pUserData;
						self->file.clear();	// a read that hit the end sets failbit, which blocks seekg
						switch (origin)
						{
							case ma_seek_origin_start:
								self->file.seekg(byteOffset, std::ios::beg);
								break;
							case ma_seek_origin_current:
								self->file.seekg(byteOffset, std::ios::cur);
								break;
							case ma_seek_origin_end:
								self->file.seekg(byteOffset, std::ios::end);
								break;
						}
						return MA_SUCCESS;
					},
					this,
					&config,
					&decoder
				);
#ifdef __PLAT_PS3__
				if (decoder_ok && !ring.Init(5, 48000, kRingFrames))
					printf("[music] no memory for a stream buffer\n");
#endif
			}

			~MusicDecoder()
			{
#ifdef __PLAT_PS3__
				if (bink != nullptr)
				{
					delete bink;
					return;
				}
#endif
				if (!IsOpen() || !decoder_ok)
					return;
				
				// Close decoder
				ma_decoder_uninit(&decoder);
			}

		private:
			void Rewind() override
			{
				if (!IsOpen())
					return;
#ifdef __PLAT_PS3__
				if (bink != nullptr)
				{
					bink->Rewind();
					return;
				}
#endif
				if (!decoder_ok)
					return;

				ma_decoder_seek_to_pcm_frame(&decoder, 0);
			}

#ifdef __PLAT_PS3__
			size_t Decode(float *out, size_t frames) override
			{
				if (!IsOpen())
					return 0;
				if (bink != nullptr)
					return bink->Read(out, frames);
				if (!decoder_ok)
					return 0;
				ma_uint64 frames_read = 0;
				ma_decoder_read_pcm_frames(&decoder, out, frames, &frames_read);
				return (size_t)frames_read;
			}
#else
			size_t Stream(char *p, size_t bytes) override
			{
				if (!IsOpen())
					return 0;
				if (!decoder_ok)
					return 0;

				// Read from decoder
				ma_uint64 frames_read = 0;
				ma_decoder_read_pcm_frames(&decoder, p, bytes / (sizeof(float) * 5), &frames_read);
				return (size_t)(frames_read * (sizeof(float) * 5));
			}
#endif
	};

	// Mixing streams
	Streamer *music_stream = nullptr;
	Streamer *stream_stream[NUM_STREAMS] = {};

#ifdef __PLAT_PS3__
	// The streaming thread: decodes every playing stream ahead into its ring, so the mixer
	// (which runs under the mixer lock, at a higher priority) never waits on a disk read or
	// a Bink decode.
	//
	// Ownership: the slots above are swapped under both the mixer lock (for the mixer) and
	// s_workLock (for this thread). This thread takes a stream from its slot under
	// s_workLock and marks it in s_working while it decodes; ReplaceStream waits until a
	// stream it took out of its slot isn't s_working before deleting it, so this thread
	// never touches a deleted stream.
	enum { kSlots = 1 + NUM_STREAMS };
	static sys_mutex_t s_workLock;
	static sys_cond_t s_workWake;		// a stream was added: fill it now
	static sys_cond_t s_workIdle;		// s_working changed and someone is waiting for that
	static Streamer *s_working = nullptr;
	static int s_workWaiters = 0;
	static bool s_worker = false;		// the thread is running (otherwise the mixer decodes)

	static Streamer *&Slot(int i)
	{
		return i == 0 ? music_stream : stream_stream[i - 1];
	}

#if !PS3_HEADLESS	// (no mixer thread, so no streaming thread either)
	static void StreamThread(void *)
	{
		Ps3Platform::AttachThreadLog();
		for (;;)
		{
			bool did = false;
			for (int i = 0; i < kSlots; i++)
			{
				sysMutexLock(s_workLock, 0);
				Streamer *stream = Slot(i);
				s_working = stream;
				sysMutexUnlock(s_workLock);
				if (stream == nullptr)
					continue;

				if (stream->Fill())
					did = true;

				sysMutexLock(s_workLock, 0);
				s_working = nullptr;
				if (s_workWaiters > 0)
					sysCondSignal(s_workIdle);
				sysMutexUnlock(s_workLock);
			}
			if (!did)
			{
				// Every ring is full or ended: look again in 5 ms (a ring holds about 340 ms),
				// or as soon as a new stream starts.
				sysMutexLock(s_workLock, 0);
				sysCondWait(s_workWake, 5000);
				sysMutexUnlock(s_workLock);
			}
		}
	}

	static bool MakeLock(sys_mutex_t *lock, const char *name)
	{
		sys_mutex_attr_t attr;
		memset(&attr, 0, sizeof(attr));
		attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO_INHERIT;
		attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
		attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
		attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
		strncpy(attr.name, name, sizeof(attr.name) - 1);
		if (sysMutexCreate(lock, &attr) == 0)
			return true;
		attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
		return sysMutexCreate(lock, &attr) == 0;
	}

	static bool MakeCond(sys_cond_t *cond, sys_mutex_t lock, const char *name)
	{
		sys_cond_attr_t attr;
		memset(&attr, 0, sizeof(attr));
		attr.attr_pshared = SYS_COND_ATTR_PSHARED;
		strncpy(attr.name, name, sizeof(attr.name) - 1);
		return sysCondCreate(cond, lock, &attr) == 0;
	}
#endif

	static void StartStreamThread()
	{
#if !PS3_HEADLESS
		if (Ps3Platform::NoThreads())
			return;
		if (!MakeLock(&s_workLock, "audstrm") || !MakeCond(&s_workWake, s_workLock, "audwake") ||
		    !MakeCond(&s_workIdle, s_workLock, "audidle"))
		{
			printf("[music] streaming thread: no lock, the mixer decodes\n");
			return;
		}
		// Below the mixer (200), above the game thread (1000) and the sysutil thread (500).
		// 256 KB like the mixer's: the Bink decoder keeps a 16 KB block on the stack, and
		// miniaudio's decoders run here too.
		sys_ppu_thread_t id;
		if (sysThreadCreate(&id, StreamThread, NULL, 300, 256 * 1024, 0, (char*)"thugstream") != 0)
		{
			printf("[music] streaming thread failed to start, the mixer decodes\n");
			return;
		}
		s_worker = true;
#endif
	}
#endif

	// Mixer
	void PCMAudio_Mix(char *buffer, size_t size)
	{
#ifdef __PLAT_PS3__
		// Without the streaming thread, decode here as before (under the mixer lock).
		if (!s_worker)
		{
			for (int i = 0; i < kSlots; i++)
				if (Slot(i) != nullptr)
					Slot(i)->Fill();
		}
#endif

		// Mix music
		if (music_stream != nullptr)
			music_stream->Mix(buffer, size);

		// Mix streams
		for (auto &i : stream_stream)
			if (i != nullptr)
				i->Mix(buffer, size);
	}

	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	static std::unordered_map<uint32, stdfs::path> music_paths;
	static std::unordered_map<uint32, stdfs::path> stream_paths;

#ifdef __PLAT_PS3__
	// The level's ambient streams are entries in streams/pcm/pcm.wad (each one an audio-only
	// Bink file), indexed by streams/pcm/pcm.dat: a count, then checksum, offset, size per
	// entry, little-endian (as the Xbox and Win32 code reads it).
	struct WadEntry { uint32 offset, size; };
	static std::unordered_map<uint32, WadEntry> wad_entries;

	static void IndexWad()
	{
		std::ifstream dat(File::DataPath() / "streams" / "pcm" / "pcm.dat", std::ios::binary);
		if (!dat.is_open())
			return;
		std::string bytes((std::istreambuf_iterator<char>(dat)), std::istreambuf_iterator<char>());
		const unsigned char *b = (const unsigned char *)bytes.data();
		auto le32 = [&](size_t at) { return (uint32)(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | ((uint32)b[at + 3] << 24)); };
		if (bytes.size() < 4)
			return;
		uint32 n = le32(0);
		for (uint32 i = 0; i < n && 4 + i * 12 + 12 <= bytes.size(); ++i)
			wad_entries[le32(4 + i * 12)] = WadEntry{ le32(8 + i * 12), le32(12 + i * 12) };
		printf("[music] pcm.wad: %u ambient streams\n", (unsigned)wad_entries.size());
	}

	static bool ReadWadEntry(uint32 checksum, std::string &out)
	{
		auto it = wad_entries.find(checksum);
		if (it == wad_entries.end())
			return false;
		std::ifstream wad(File::DataPath() / "streams" / "pcm" / "pcm.wad", std::ios::binary);
		if (!wad.is_open())
			return false;
		out.resize(it->second.size);
		wad.seekg(it->second.offset);
		wad.read(&out[0], it->second.size);
		return (size_t)wad.gcount() == it->second.size;
	}
#endif

	// Opening a decoder reads the file (path lookups, the Bink frame index) and closing one
	// frees it, so neither happens under the mixer's lock: only the pointer swap does.
	static void ReplaceStream(Streamer *&slot, Streamer *replacement)
	{
		Audio::Lock();
#ifdef __PLAT_PS3__
		if (s_worker)
			sysMutexLock(s_workLock, 0);
#endif
		Streamer *old = slot;
		slot = replacement;
#ifdef __PLAT_PS3__
		if (s_worker)
			sysMutexUnlock(s_workLock);
#endif
		Audio::Unlock();
#ifdef __PLAT_PS3__
		// The streaming thread may still be decoding the old stream (it took it before the
		// swap): wait for it to put it down. A new stream gets its first decode right away.
		if (s_worker)
		{
			sysMutexLock(s_workLock, 0);
			if (replacement != nullptr)
				sysCondSignal(s_workWake);
			while (old != nullptr && s_working == old)
			{
				++s_workWaiters;
				sysCondWait(s_workIdle, 20 * 1000);
				--s_workWaiters;
			}
			sysMutexUnlock(s_workLock);
		}
#endif
		delete old;
	}

	// The PC data has no Data/music: its music is with the other streams, in
	// Data/streams/music (Bink files named by checksum).
	static const stdfs::path *FindMusic(uint32 checksum)
	{
		auto it = music_paths.find(checksum);
		if (it != music_paths.end())
			return &it->second;
		it = stream_paths.find(checksum);
		if (it != stream_paths.end())
			return &it->second;
		return nullptr;
	}

	static std::unordered_map<uint32, stdfs::path> Index(stdfs::path path)
	{
#ifdef __PLAT_PS3__
		Ps3Platform::Trace("[music] index %s", path.c_str());
#endif
		if (!stdfs::exists(path))
			return {};

		std::unordered_map<uint32, stdfs::path> paths;
		for (auto &i : stdfs::recursive_directory_iterator(path))
		{
			if (i.is_regular_file())
			{
				std::string name = i.path().stem().string();
				//uint32 checksum = Crc::GenerateCRCFromString(name.c_str());
				uint32 checksum = 0;
				if (sscanf(name.c_str(), "%x", &checksum) != 1)
					continue;	// not named by checksum (a readme, desktop.ini...)

				//uint32 checksum = std::stoi(name.c_str(), 0, 16);
				paths[checksum] = stdfs::relative(i.path(), File::DataPath());
			}
		}
		return paths;
	}

	void PCMAudio_Init( void )
	{
		// Initialize mixer
		Audio::Init();

		// Index music and streams
#ifdef __PLAT_PS3__
		Ps3Platform::Step("indexing music and streams");
#endif
#ifdef __PLAT_PS3__
		StartStreamThread();
#endif
		music_paths = Index(File::DataPath() / "music");
		stream_paths = Index(File::DataPath() / "streams");
#ifdef __PLAT_PS3__
		printf("[music] %u music files, %u streams\n", (unsigned)music_paths.size(), (unsigned)stream_paths.size());
		IndexWad();
#endif
	}



	/******************************************************************/
	/*                                                                */
	/* Call every frame to make sure music and stream buffers are	  */
	/* loaded and current status is checked each frame...			  */
	/*                                                                */
	/******************************************************************/
	int PCMAudio_Update( void )
	{
		return 0;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	void PCMAudio_StopMusic( bool waitPlease )
	{
		(void)waitPlease;

		ReplaceStream(music_stream, nullptr);
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	void PCMAudio_StopStream( int whichStream, bool waitPlease )
	{
		(void)waitPlease;

		ReplaceStream(stream_stream[whichStream], nullptr);
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	void PCMAudio_StopStreams( void )
	{
		for (auto &i : stream_stream)
			ReplaceStream(i, nullptr);
	}


	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/

	// This is temp code for the preload streams.  It just calls the normal one.

	// static uint32 sPreLoadChecksum[NUM_STREAMS];
	// static uint32 sPreLoadMusicChecksum;

	/******************************************************************/
	/*                                                                */
	/* Get a stream loaded into a buffer, but don't play yet		  */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PreLoadStream( uint32 checksum, int whichStream )
	{
		PCMAudio_PlayStream(checksum, whichStream, nullptr, 0.0f, true);
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/* Returns true if preload done. Assumes that caller is calling	  */
	/* this on a preloaded, but not yet played, stream. The results	  */
	/* are meaningless otherwise.									  */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PreLoadStreamDone( int whichStream )
	{
		(void)whichStream;
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/* Tells a preloaded stream to start playing.					  */
	/* Must call PCMAudio_PreLoadStreamDone() first to guarantee that */
	/* it starts immediately.										  */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_StartPreLoadedStream( int whichStream, Sfx::sVolume *p_volume, float pitch )
	{
		(void)whichStream;
		(void)p_volume;
		(void)pitch;

		PCMAudio_SetStreamVolume(p_volume, whichStream);

		Audio::Lock();

		if (stream_stream[whichStream] != nullptr)
			stream_stream[whichStream]->Play();

		Audio::Unlock();
		
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PreLoadMusicStream( uint32 checksum )
	{
		auto it = FindMusic(checksum);
		if (it == nullptr)
			return false;

		ReplaceStream(music_stream, new MusicDecoder(File::DataPath() / *it));
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PreLoadMusicStreamDone( void )
	{
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_StartPreLoadedMusicStream( void )
	{
		Audio::Lock();
		if (music_stream != nullptr)
			music_stream->Play();
		Audio::Unlock();
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	int PCMAudio_GetMusicStatus( void )
	{
		Audio::Lock();

		int status;
		if (music_stream == nullptr)
			status = PCM_STATUS_FREE;
		else if (music_stream->IsPlaying())
			status = PCM_STATUS_PLAYING;
		else
			status = PCM_STATUS_FREE;

		Audio::Unlock();
		return status;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	int PCMAudio_GetStreamStatus( int whichStream )
	{
		Audio::Lock();

		int status;
		Streamer *stream = stream_stream[whichStream];
		if (stream == nullptr)
			status = PCM_STATUS_FREE;
		else if (stream->IsPlaying())
			status = PCM_STATUS_PLAYING;
		else
			status = PCM_STATUS_FREE;
		
		Audio::Unlock();
		return status;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	void PCMAudio_Pause( bool pause, int ch )
	{
		(void)pause;
		(void)ch;

		Audio::Lock();

		if (ch == MUSIC_CHANNEL)
		{
			if (music_stream != nullptr)
			{
				if (pause)
					music_stream->Pause();
				else
					music_stream->Play();
			}
		}
		else
		{
			for (auto &i : stream_stream)
			{
				if (i != nullptr)
				{
					if (pause)
						i->Pause();
					else
						i->Play();
				}
			}
		}

		Audio::Unlock();
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_TrackExists( const char *pTrackName, int ch )
	{
		(void)pTrackName;
		(void)ch;

		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_LoadMusicHeader( const char *nameOfFile )
	{
		(void)nameOfFile;
		// Legacy call left over from PS2 code.
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_LoadStreamHeader( const char *nameOfFile )
	{
		(void)nameOfFile;
		// Legacy call left over from PS2 code.
		return true;
	}


	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	uint32 PCMAudio_FindNameFromChecksum( uint32 checksum, int ch )
	{
		if (ch != EXTRA_CHANNEL)
			return 0;

		auto it = stream_paths.find(checksum);
		if (it == stream_paths.end())
			return 0;

		return checksum;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_SetStreamVolume( Sfx::sVolume *p_volume, int whichStream )
	{
		Audio::Lock();

		if (stream_stream[whichStream] != nullptr)
		{
			Spt::SingletonPtr< Sfx::CSfxManager > sfx_manager;

			float coef[5] = {};

			switch (p_volume->GetVolumeType())
			{
				case Sfx::VOLUME_TYPE_5_CHANNEL_DOLBY5_1:
				{
					// NOTE: sVolume is in the order of FL, C, FR, BL, BR
					// So we need to swap C and FR to match what the mixer expects
					coef[0] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(0)) / 100.0f * STREAM_VOLUME;
					coef[1] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(1)) / 100.0f * STREAM_VOLUME;
					coef[2] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(4)) / 100.0f * STREAM_VOLUME;
					coef[3] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(2)) / 100.0f * STREAM_VOLUME;
					coef[4] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(3)) / 100.0f * STREAM_VOLUME;
					break;
				}
				case Sfx::VOLUME_TYPE_2_CHANNEL_DOLBYII:
				{
					coef[0] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(0)) / 100.0f * STREAM_VOLUME;
					coef[1] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(1)) / 100.0f * STREAM_VOLUME;
					break;
				}
				case Sfx::VOLUME_TYPE_BASIC_2_CHANNEL:
				{
					coef[0] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(0)) / 100.0f * STREAM_VOLUME;
					coef[1] = PERCENT(sfx_manager->GetMainVolume(), p_volume->GetChannelVolume(1)) / 100.0f * STREAM_VOLUME;
					break;
				}
			}

			stream_stream[whichStream]->SetVolume(coef);
		}

		Audio::Unlock();
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	int PCMAudio_SetMusicVolume( float volume )
	{
		(void)volume;

		Audio::Lock();

		if (music_stream != nullptr)
		{
			float coef[5] = {};
			coef[0] = volume / 100.0f;
			coef[1] = volume / 100.0f;
			coef[2] = volume / 100.0f;
			coef[3] = volume / 100.0f;
			coef[4] = volume / 100.0f;
			music_stream->SetVolume(coef);
		}

		Audio::Unlock();
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_SetStreamPitch( float fPitch, int whichStream )
	{
		(void)fPitch;
		(void)whichStream;
		return true;
	}

	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PlayMusicTrack( uint32 checksum )
	{
		(void)checksum;
		Dbg_Assert(0);
		return false;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PlayMusicTrack( const char *filename )
	{
		// Remove path from filename
		const char *last_slash = strrchr(filename, '\\');
		const char *last_fwd = strrchr(filename, '/');
		if (last_fwd != nullptr && (last_slash == nullptr || last_fwd > last_slash))
			last_slash = last_fwd;
		if (last_slash != nullptr)
			filename = last_slash + 1;

		// Find music path from checksum
		uint32 hash = Crc::GenerateCRCFromString(filename);
		auto it = FindMusic(Crc::GenerateCRCFromString(filename));
		if (it == nullptr)

			return false;

		// Play song
		Streamer *song = new MusicDecoder(File::DataPath() / *it);
		song->Play();
		ReplaceStream(music_stream, song);
		return true;
	}

#ifdef __PLAT_PS3__
	bool PCMAudio_PlayMovieAudio(const char *path, float volume)
	{
		MusicDecoder *track = new MusicDecoder(stdfs::path(path), true);
		if (track->bink == nullptr)
		{
			delete track;
			return false;
		}
		float coef[5] = { volume, volume, volume, volume, volume };
		track->SetVolume(coef);
		track->Play();
		ReplaceStream(music_stream, track);
		return true;
	}

	void PCMAudio_StopMovieAudio()
	{
		ReplaceStream(music_stream, nullptr);
	}
#endif

	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PlaySoundtrackMusicTrack(size_t soundtrack, size_t track )
	{
		// Play song
		const UserSoundtrack::Soundtracks &soundtracks = UserSoundtrack::GetSoundtracks();
		Streamer *song = new MusicDecoder(soundtracks.at(soundtrack).tracks.at(track).path);
		song->Play();
		ReplaceStream(music_stream, song);
		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_StartStream( int whichStream )
	{
		Audio::Lock();

		if (stream_stream[whichStream] != nullptr)
			stream_stream[whichStream]->Play();

		Audio::Unlock();

		return true;
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PlayStream( uint32 checksum, int whichStream, Sfx::sVolume *p_volume, float fPitch, bool preload )
	{
		(void)fPitch;

		// Find stream path from checksum
		auto it = stream_paths.find(checksum);
		Streamer *stream = nullptr;
		if (it == stream_paths.end())
		{
#ifdef __PLAT_PS3__
			std::string bytes;
			if (!ReadWadEntry(checksum, bytes))
				return false;
			stream = new MusicDecoder(bytes);
#else
			return false;
#endif
		}
		else
		{
			// Play stream
			stream = new MusicDecoder(File::DataPath() / it->second);
		}
		if (!preload)
			stream->Play();
		ReplaceStream(stream_stream[whichStream], stream);

		if (p_volume != nullptr)
			PCMAudio_SetStreamVolume(p_volume, whichStream);

		return true;
	}

	// User soundtracks
	size_t GetNumSoundtracks()
	{
		const UserSoundtrack::Soundtracks &soundtracks = UserSoundtrack::GetSoundtracks();
		return soundtracks.size();
	}

	size_t GetSoundtrackNumSongs(size_t i)
	{
		const UserSoundtrack::Soundtracks &soundtracks = UserSoundtrack::GetSoundtracks();
		if(soundtracks.empty())
			return 0;

		return soundtracks.at(i).tracks.size();
	}

	const char *GetSoundtrackName(size_t i)
	{
		const UserSoundtrack::Soundtracks &soundtracks = UserSoundtrack::GetSoundtracks();
		if (soundtracks.empty())
			return "";

		return soundtracks.at(i).name.c_str();
	}

} // namespace PCM
