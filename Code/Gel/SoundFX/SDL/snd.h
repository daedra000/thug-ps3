#pragma once

#include "ima.h"

#include <memory>

namespace Snd
{
	class Sound
	{
		private:
			// IMA state
			IMA::IMA ima;

			// Sound state
			bool playing = false;
			bool paused = false;
			bool looping = false;

			unsigned long frequency = 0;
			unsigned long coord = 0, step = 0;
			signed short sample_a = 0, sample_b = 0;

			float Fetch()
			{
				// Check if stopped or paused
				if (!playing || paused)
					return 0.0f;

				// Increment coordinate
				coord += step;
				while (coord >= 0x10000)
				{
					// Check if at end
					if (ima.AtEnd())
					{
						if (looping)
						{
							// Rewind stream
							ima.Rewind();
						}
						else
						{
							playing = false;
							return 0.0f;
						}
					}

					// Fetch new sample
					sample_a = sample_b;
					sample_b = ima.Fetch();

					// Update coordinate
					coord -= 0x10000;
				}

				// Interpolate
				// Signed 64-bit maths: with the unsigned version a falling waveform gave a huge
				// value where long is 64 bits (PS3), heard as static. (It only worked on PC because
				// long is 32 bits there and the result was cut back down.)
				const int frac = (int)(coord & 0xFFFF);
				const int sample = sample_a + (int)(((long long)frac * (sample_b - sample_a)) >> 16);
				return (float)sample / (float)0x8000;
			}

			float volume[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

		public:
			// Constructor
			Sound() {}
			Sound(unsigned char *_data, size_t _size, unsigned long _frequency)
			{
				// Setup stream
				SetPointer(_data, _size, _frequency);
			}

			void SetPointer(unsigned char *_data, size_t _size, unsigned long _frequency, bool _pcm16 = false, size_t _blockAlign = 0)
			{
				// Setup IMA stream (or plain 16-bit PCM, or Xbox ADPCM blocks)
				ima.SetPointer(_data, _size, _pcm16, _blockAlign);

				// Setup sound stream
				coord = 0;
				sample_a = ima.Fetch();
				sample_b = ima.Fetch();

				frequency = _frequency;

				// Setup sound state
				playing = false;
				paused = false;
				looping = false;
			}

			void Play()
			{
				playing = true;
				paused = false;
			}

			void Pause()
			{
				paused = true;
			}

			void Stop()
			{
				playing = false;
			}

			void SetLooping(bool loops)
			{
				looping = loops;
			}

			bool IsPlaying()
			{
				return playing;
			}

			bool IsPaused()
			{
				return paused;
			}

			bool IsLooping()
			{
				return looping;
			}

			void SetVolume(float *coef)
			{
				volume[0] = coef[0];
				volume[1] = coef[1];
				volume[2] = coef[2];
				volume[3] = coef[3];
				volume[4] = coef[4];
			}

			void SetPitch(float pitch)
			{
				step = (unsigned long)((pitch / 100.0f) * (float)frequency / (float)48000 * (float)0x10000);
			}

			void Mix(char *buffer, size_t bytes)
			{
				// Check if playing
				if (!playing || paused)
					return;

				// Mix sound
				float *out = (float*)buffer;
				for (size_t i = 0; i < bytes / (sizeof(float) * 5); i++)
				{
					float sample = Fetch();
					for (size_t c = 0; c < 5; c++)
						*out++ += sample * volume[c];
				}
			}
	};
}
