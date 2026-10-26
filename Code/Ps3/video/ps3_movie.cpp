// THUG PS3: full-screen movie playback (the PlayMovie script function).
//
// The PC data's movies are Bink 1 files in Data/movies/bik. Like the Xbox version's
// PMovies_PlayMovie, this plays one to the end (or until Start or X is pressed) before
// returning: the video is decoded with BinkVideo (bink_video.cpp), converted to RGB on the
// PPU and drawn as a full-screen textured quad; the sound track plays on the music channel
// through the Bink audio decoder. Frames are timed against the clock from the moment the
// sound starts, and a frame that comes up late is decoded but not drawn, so the picture
// keeps up with the sound.

#include <core/defines.h>
#include <core/macros.h>
#include <core/singleton.h>
#include <gfx/DX9/NX/nx_init.h>
#include <gfx/DX9/NX/render.h>
#include <gel/music/music.h>
#include <gel/soundfx/soundfx.h>
#include <Gel/Music/SDL/p_music.h>
#include <sys/sioman.h>
#include <sys/siodev.h>

#include <ps3_platform.h>
#include <ps3_fs.h>
#include "bink_video.h"

#include <fstream>
#include <string.h>
#include <stdio.h>
#include <sys/systime.h>
#include <unistd.h>

namespace Ps3Movie
{

namespace
{
	// BT.601 studio-swing YUV to RGB, in 16.16 fixed point.
	int s_yTab[256], s_rv[256], s_gu[256], s_gv[256], s_bu[256];
	bool s_tables;

	void InitTables()
	{
		if (s_tables) return;
		for (int i = 0; i < 256; ++i)
		{
			s_yTab[i] = (int)(1.164383 * 65536.0 * (i - 16));
			s_rv[i] = (int)(1.596027 * 65536.0 * (i - 128));
			s_gu[i] = (int)(0.391762 * 65536.0 * (i - 128));
			s_gv[i] = (int)(0.812968 * 65536.0 * (i - 128));
			s_bu[i] = (int)(2.017232 * 65536.0 * (i - 128));
		}
		s_tables = true;
	}

	inline u32 Clamp8(int v)
	{
		v >>= 16;
		return v < 0 ? 0 : v > 255 ? 255 : (u32)v;
	}

	// One frame into an A8R8G8B8 texture (host-order words, as the D3D9 layer expects).
	void ConvertFrame(const BinkVideo& video, u8* dst, int dstPitch)
	{
		const int w = video.Width(), h = video.Height();
		const u8* yp = video.Plane(0);
		const u8* up = video.Plane(1);
		const u8* vp = video.Plane(2);
		const int ys = video.Stride(0), us = video.Stride(1), vs = video.Stride(2);
		for (int y = 0; y < h; ++y)
		{
			u32* out = (u32*)(dst + y * dstPitch);
			const u8* yr = yp + y * ys;
			const u8* ur = up + (y >> 1) * us;
			const u8* vr = vp + (y >> 1) * vs;
			for (int x = 0; x < w; x += 2)
			{
				const int u = ur[x >> 1], v = vr[x >> 1];
				const int rv = s_rv[v], g = s_gu[u] + s_gv[v], bu = s_bu[u];
				int l = s_yTab[yr[x]];
				out[x] = 0xFF000000u | (Clamp8(l + rv) << 16) | (Clamp8(l - g) << 8) | Clamp8(l + bu);
				if (x + 1 < w)
				{
					l = s_yTab[yr[x + 1]];
					out[x + 1] = 0xFF000000u | (Clamp8(l + rv) << 16) | (Clamp8(l - g) << 8) | Clamp8(l + bu);
				}
			}
		}
	}

	struct MovieVert
	{
		float x, y, z, rhw;
		float u, v;
	};

	// The frame, scaled to the screen's height and centred (black bars at the sides of a
	// 4:3 movie on a 16:9 screen).
	void DrawFrame(IDirect3DTexture9* tex, int w, int h)
	{
		const float sw = (float)NxXbox::EngineGlobals.backbuffer_width;
		const float sh = (float)NxXbox::EngineGlobals.backbuffer_height;
		float dw = sh * (float)w / (float)h, dh = sh;
		if (dw > sw)
		{
			dw = sw;
			dh = sw * (float)h / (float)w;
		}
		const float x0 = (sw - dw) * 0.5f, y0 = (sh - dh) * 0.5f;

		// The loading bar draws from a timer thread under this lock.
		WaitForSingleObject(NxXbox::EngineGlobals.g_asyncRenderMutex, 0xFFFFFFFF);
		D3DDevice_BeginScene();
		D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);

		D3DDevice_SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		D3DDevice_SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		D3DDevice_SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		D3DDevice_SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		D3DDevice_SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		D3DDevice_SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		D3DDevice_SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		D3DDevice_SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		D3DDevice_SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		D3DDevice_SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		D3DDevice_SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		D3DDevice_SetRenderState(D3DRS_LIGHTING, FALSE);
		D3DDevice_SetRenderState(D3DRS_ZENABLE, FALSE);
		D3DDevice_SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		D3DDevice_SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		D3DDevice_SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		D3DDevice_SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

		NxXbox::set_pixel_shader(NULL);
		NxXbox::set_vertex_shader(D3DFVF_TEX1 | D3DFVF_XYZRHW);
		NxXbox::set_blend_mode(0);
		NxXbox::set_texture(0, tex);

		const MovieVert quad[4] = {
			{ x0,      y0,      0.0f, 1.0f, 0.0f, 0.0f },
			{ x0,      y0 + dh, 0.0f, 1.0f, 0.0f, 1.0f },
			{ x0 + dw, y0,      0.0f, 1.0f, 1.0f, 0.0f },
			{ x0 + dw, y0 + dh, 0.0f, 1.0f, 1.0f, 1.0f },
		};
		D3DDevice_DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(MovieVert));
		NxXbox::set_texture(0, NULL);

		D3DDevice_EndScene();
		D3DDevice_Present(NULL, NULL, NULL, NULL);
		ReleaseMutex(NxXbox::EngineGlobals.g_asyncRenderMutex);
	}

	// Start or X, newly pressed (the press that started the movie must be let go first).
	bool SkipPressed(bool& armed)
	{
		Spt::SingletonPtr<SIO::Manager> sio_manager;
		sio_manager->ProcessDevices();
		bool held = false;
		for (int d = 0; d < 4; ++d)
		{
			SIO::Device* dev = sio_manager->GetDeviceByIndex(d);
			if (!dev) continue;
			const unsigned char* p = dev->GetControlData();
			if (!p) continue;
			// PS2 pad layout, active low: byte 2 bit 3 Start, byte 3 bit 6 Cross.
			if (!(p[2] & (1 << 3)) || !(p[3] & (1 << 6)))
				held = true;
		}
		if (!held)
		{
			armed = true;
			return false;
		}
		return armed;
	}
}

// pName is as the scripts give it, e.g. "movies\intro": the file is Data/movies/bik/intro.bik.
void Play(const char* pName)
{
	const char* base = pName;
	for (const char* p = pName; *p; ++p)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	char path[512];
	snprintf(path, sizeof(path), "%s/Data/movies/bik/%s.bik", Ps3Fs::AppDir(), base);

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		printf("[movie] %s: not found\n", path);
		return;
	}
	BinkVideo video;
	if (!video.Open(&file))
	{
		printf("[movie] %s: not a Bink file this player handles\n", path);
		return;
	}
	const double fps = video.Fps() > 1.0 ? video.Fps() : 29.97;
	printf("[movie] %s: %dx%d, %u frames at %.2f fps\n", path, video.Width(), video.Height(), video.FrameCount(), fps);

	// Two textures, written in turn, so a frame isn't rewritten while the RSX may still be
	// drawing it.
	IDirect3DTexture9* tex[2] = { NULL, NULL };
	for (int i = 0; i < 2; ++i)
	{
		if (D3D_OK != D3DDevice_CreateTexture(video.Width(), video.Height(), 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[i], NULL))
		{
			printf("[movie] couldn't create the frame texture\n");
			if (tex[0]) tex[0]->Release();
			return;
		}
	}
	InitTables();

	// Sound: at the louder of the music and effects volumes, like the Xbox player.
	Spt::SingletonPtr<Sfx::CSfxManager> sfx_manager;
	const float music_vol = Pcm::GetVolume();
	const float sfx_vol = sfx_manager->GetMainVolume();
	const float vol = (music_vol > sfx_vol ? music_vol : sfx_vol) * 0.01f;
	Pcm::StopMusic();
	Pcm::StopStreams();
	const bool sound = Pcm::PCMAudio_PlayMovieAudio(path, vol);

	const u64 start = sysGetSystemTime();
	unsigned frame = 0, shown = 0, dropped = 0, cur = 0;
	bool armed = false, skipped = false;
	while (video.DecodeFrame())
	{
		const u64 due = start + (u64)((double)frame * 1000000.0 / fps);
		const u64 now = sysGetSystemTime();
		const bool late = now > due + (u64)(1000000.0 / fps) && frame + 1 < video.FrameCount();
		if (!late)
		{
			D3DLOCKED_RECT lr;
			if (D3D_OK == tex[cur]->LockRect(0, &lr, NULL, 0))
			{
				ConvertFrame(video, (u8*)lr.pBits, lr.Pitch);
				tex[cur]->UnlockRect(0);
			}
			if (now < due)
				usleep((useconds_t)(due - now));
			DrawFrame(tex[cur], video.Width(), video.Height());
			cur ^= 1;
			++shown;
		}
		else
			++dropped;
		++frame;

		Ps3Platform::Pump();
		if (Ps3Platform::QuitRequested() || SkipPressed(armed))
		{
			skipped = true;
			break;
		}
	}

	if (sound)
		Pcm::PCMAudio_StopMovieAudio();
	tex[0]->Release();
	tex[1]->Release();
	const double secs = (double)(sysGetSystemTime() - start) / 1000000.0;
	printf("[movie] %s after %u frames in %.1f s (%u shown, %u dropped)\n", skipped ? "skipped" : "finished", frame, secs, shown, dropped);
}

} // namespace Ps3Movie
