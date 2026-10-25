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
#endif
#include "usersoundtrack.h"	// stdfs (std::filesystem, or the PS3 subset)

#ifndef __PLAT_PS3__	// unused, and no std::thread on PS3
#include <mutex>
#include <thread>
#endif
#include <functional>
#include <fstream>

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
			std::vector<char> buffer;

			bool paused = true;
			bool playing = true;

			float volume[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

		public:
			virtual ~Streamer() = default;

			virtual void Rewind() = 0;
			virtual size_t Stream(char *p, size_t bytes) = 0;

			size_t Mix(char *out, size_t bytes)
			{
				// If stopped or paused, return 0
				if (!playing || paused)
					return 0;

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
				Rewind();
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
			std::ifstream file;

		public:
#ifdef __PLAT_PS3__
			// The PC data's music and streams are audio-only Bink files, which miniaudio
			// can't decode (Code/Ps3/audio/bink_audio.cpp does).
			BinkAudio *bink = nullptr;
			std::vector<float> bink_buffer;
			double bink_pos = 0.0;		// resampling: position between bink_buffer frames 0 and 1
			bool bink_end = false;

			bool OpenBink()
			{
				char magic[3] = {};
				file.read(magic, 3);
				file.clear();
				file.seekg(0);
				if (magic[0] != 'B' || magic[1] != 'I' || magic[2] != 'K')
					return false;
				bink = new BinkAudio();
				if (!bink->Open(&file))
				{
					printf("[music] unsupported Bink file\n");
					delete bink;
					bink = nullptr;
					file.close();
					return true;
				}
				bink_buffer.clear();
				bink_pos = 0.0;
				bink_end = false;
				return true;
			}

			// Output: 5 channels (FL FR C BL BR) at 48 kHz, like the miniaudio path. Stereo goes
			// to the front pair; mono to every channel, as miniaudio does.
			size_t StreamBink(float *out, size_t frames)
			{
				const int ch = bink->Channels();
				const double step = bink->SampleRate() / 48000.0;
				size_t done = 0;
				while (done < frames)
				{
					// Need frames floor(pos) and floor(pos)+1 buffered.
					size_t have = bink_buffer.size() / ch;
					if ((size_t)bink_pos + 1 >= have)
					{
						if (bink_end)
						{
							if ((size_t)bink_pos >= have)
								break;
						}
						else
						{
							// Keep the frame we're on, decode more after it.
							size_t keep = (size_t)bink_pos;
							bink_buffer.erase(bink_buffer.begin(), bink_buffer.begin() + keep * ch);
							bink_pos -= keep;
							size_t old = bink_buffer.size();
							bink_buffer.resize(old + 2048 * ch);
							size_t got = bink->Read(&bink_buffer[old], 2048);
							bink_buffer.resize(old + got * ch);
							if (got < 2048)
								bink_end = true;
							continue;
						}
					}
					size_t i0 = (size_t)bink_pos;
					size_t i1 = i0 + 1 < bink_buffer.size() / ch ? i0 + 1 : i0;
					float t = (float)(bink_pos - i0);
					float l = bink_buffer[i0 * ch] * (1.0f - t) + bink_buffer[i1 * ch] * t;
					float r = ch > 1 ? bink_buffer[i0 * ch + 1] * (1.0f - t) + bink_buffer[i1 * ch + 1] * t : l;
					if (ch > 1)
					{
						out[0] = l; out[1] = r; out[2] = 0.0f; out[3] = 0.0f; out[4] = 0.0f;
					}
					else
					{
						out[0] = out[1] = out[2] = out[3] = out[4] = l;
					}
					out += 5;
					++done;
					bink_pos += step;
				}
				return done;
			}
#endif

			MusicDecoder(stdfs::path path) : file(path, std::ios::binary)
			{
				if (!file.is_open())
					return;
#ifdef __PLAT_PS3__
				if (OpenBink())
					return;
#endif

				ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 5, 48000);
				ma_decoder_init(
					[](ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead, size_t *pBytesRead) -> ma_result
					{
						MusicDecoder *self = (MusicDecoder*)pDecoder->pUserData;
						self->file.read((char *)pBufferOut, bytesToRead);
						if (pBytesRead != nullptr)
							*pBytesRead = (size_t)self->file.gcount();
						return MA_SUCCESS;
					},
					[](ma_decoder *pDecoder, ma_int64 byteOffset, ma_seek_origin origin) -> ma_result
					{
						MusicDecoder *self = (MusicDecoder*)pDecoder->pUserData;
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
				if (!file.is_open())
					return;
				
				// Close decoder
				ma_decoder_uninit(&decoder);
			}

		private:
			void Rewind() override
			{
				if (!file.is_open())
					return;
#ifdef __PLAT_PS3__
				if (bink != nullptr)
				{
					bink->Rewind();
					bink_buffer.clear();
					bink_pos = 0.0;
					bink_end = false;
					return;
				}
#endif

				ma_decoder_seek_to_pcm_frame(&decoder, 0);
			}

			size_t Stream(char *p, size_t bytes) override
			{
				if (!file.is_open())
					return 0;
#ifdef __PLAT_PS3__
				if (bink != nullptr)
					return StreamBink((float *)p, bytes / (sizeof(float) * 5)) * (sizeof(float) * 5);
#endif

				// Read from decoder
				ma_uint64 frames_read = 0;
				ma_decoder_read_pcm_frames(&decoder, p, bytes / (sizeof(float) * 5), &frames_read);
				return (size_t)(frames_read * (sizeof(float) * 5));
			}
	};

	// Mixing streams
	Streamer *music_stream = nullptr;
	Streamer *stream_stream[NUM_STREAMS] = {};

	// Mixer
	void PCMAudio_Mix(char *buffer, size_t size)
	{
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
				uint32 checksum;
				sscanf(name.c_str(), "%x", &checksum);

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
		music_paths = Index(File::DataPath() / "music");
		stream_paths = Index(File::DataPath() / "streams");
#ifdef __PLAT_PS3__
		printf("[music] %u music files, %u streams\n", (unsigned)music_paths.size(), (unsigned)stream_paths.size());
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

		Audio::Lock();

		if (music_stream != nullptr)
		{
			delete music_stream;
			music_stream = nullptr;
		}

		Audio::Unlock();
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	void PCMAudio_StopStream( int whichStream, bool waitPlease )
	{
		(void)waitPlease;

		Audio::Lock();

		if (stream_stream[whichStream] != nullptr)
		{
			delete stream_stream[whichStream];
			stream_stream[whichStream] = nullptr;
		}

		Audio::Unlock();
	}



	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	void PCMAudio_StopStreams( void )
	{
		Audio::Lock();

		for (auto &i : stream_stream)
		{
			if (i != nullptr)
			{
				delete i;
				i = nullptr;
			}
		}

		Audio::Unlock();
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

		Audio::Lock();

		if (music_stream != nullptr)
			delete music_stream;
		music_stream = new MusicDecoder(File::DataPath() / *it);

		Audio::Unlock();
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
		if (last_slash != nullptr)
			filename = last_slash + 1;

		// Find music path from checksum
		uint32 hash = Crc::GenerateCRCFromString(filename);
		auto it = FindMusic(Crc::GenerateCRCFromString(filename));
		if (it == nullptr)

			return false;

		Audio::Lock();
		
		// Play song
		if (music_stream != nullptr)
			delete music_stream;

		music_stream = new MusicDecoder(File::DataPath() / *it);
		music_stream->Play();

		Audio::Unlock();
		return true;
	}

	/******************************************************************/
	/*                                                                */
	/*                                                                */
	/******************************************************************/
	bool PCMAudio_PlaySoundtrackMusicTrack(size_t soundtrack, size_t track )
	{
		Audio::Lock();

		// Play song
		if (music_stream != nullptr)
			delete music_stream;

		const UserSoundtrack::Soundtracks &soundtracks = UserSoundtrack::GetSoundtracks();
		music_stream = new MusicDecoder(soundtracks.at(soundtrack).tracks.at(track).path);
		music_stream->Play();

		Audio::Unlock();
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
		if (it == stream_paths.end())
			return false;

		Audio::Lock();

		// Play stream
		if (stream_stream[whichStream] != nullptr)
			delete stream_stream[whichStream];

		stream_stream[whichStream] = new MusicDecoder(File::DataPath() / it->second);
		if (!preload)
			stream_stream[whichStream]->Play();

		if (p_volume != nullptr)
			PCMAudio_SetStreamVolume(p_volume, whichStream);

		Audio::Unlock();

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
