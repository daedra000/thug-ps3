# THUG PS3

A homebrew PlayStation 3 port of **Tony Hawk's Underground** (2003), for consoles running HEN or CFW.

Built on [kisak-thug](https://github.com/SwagSoftware/kisak-thug) (the PC/DX9 port, imported at
`98b4e24`), which is built on the [thug1src](https://github.com/thug1src/thug) source release.
Many PS3 lessons come from [re3-PS3](https://github.com/stingziz0u/re3-PS3) and
[reVC-PS3](https://github.com/stingziz0u/reVC-PS3). kisak-thug's original README is kept in
[README-kisak-thug.md](README-kisak-thug.md).

**No game data is included.** You supply the `Data` folder from your own legitimate copy of the
**PC version** of THUG.

## Status

**Playable on a console** (HEN): menus, levels, skating, story mode start, movies, music and
sound. Still being tested; please send `USRDIR/thug-ps3.log` with any report.

- [x] Import kisak-thug; build on GitHub Actions (ps3toolchain + PSL1GHT, cached)
- [x] PS3 platform target (`__PLAT_PS3__` alongside the PC code paths), PSL1GHT Makefile
- [x] System layer: start-up, log file, XMB events, case-insensitive paths for the PC data,
      Win32 API stand-ins (threads, timers, files, directory search)
- [x] Little-endian PC data on the big-endian PS3 (see [docs/ps3-byte-order.md](docs/ps3-byte-order.md))
- [x] RSX renderer: textures, buffers, programs generated from the fixed-function state and the
      game's ps_1_1 shaders, drawing into textures; 720p, the game's widescreen mode on 16:9
- [x] Input: DualShock 3 (the game's PS2 pad layout maps one to one), vibration, and the PS2
      button glyphs in the menus and tips
- [x] Text entry (skater names and the like) with the PS3's on-screen keyboard
- [x] Audio: music, level ambience (`streams/pcm/pcm.wad`) and sound effects (16-bit PCM and
      Xbox ADPCM)
- [x] Movies: the PC data's Bink 1 movies (`Code/Ps3/video/bink_video.cpp`, LGPL 2.1+, derived
      from FFmpeg; checked against ffmpeg with `tools/bink-video-test`); Start or X skips
- [x] Crash log: a PPU exception handler (liblv2dbg) logs pc, registers and a backtrace;
      `thug-ps3.map` from the CI artifacts names the addresses. A hang watchdog, a frame
      profiler (`[prof]`, `[hitch]`) and memory reports (`[mem]`) also go to the log
- [x] Renderer: tiled display and depth buffers, ZCULL, triple buffering; a 4 MB command
      buffer so the CPU and RSX work in parallel
- [x] Surfaces flashing dark or pale as the camera moved: fixed (patched fragment programs
      get a fresh copy for every draw, and the RSX finishes drawing into a texture, such as a
      shadow, before anything reads it)
- [x] Frame pacing: frames that reach the screen wait only for the vsync (the TV's 59.94 Hz),
      not also for a 60 Hz timer of the game's own; that cost a whole extra vsync on many frames
- [ ] Performance: 60 fps in most places; busy views (over 1,000 level pieces on screen)
      still drop to 30 fps, as drawing them takes the CPU 17–20 ms. Being worked on: a native
      PS3 path for drawing the level, in place of the D3D9 layer
- [ ] Story mode cutscenes past the first (being checked)
- [ ] Saves (`USRDIR/Save`): written to a `.tmp` file and swapped in when complete; untested

## Installing

1. Install `thug-ps3.pkg` on a PS3 with HEN or CFW. It appears in the XMB as
   **Tony Hawk's Underground**.
2. The PKG creates `/dev_hdd0/game/THUG00001/USRDIR/Data`. Copy the contents of the `Data`
   folder from your PC installation of THUG into it (by FTP). Letter case doesn't matter. The
   movies in `Data/movies/bik` must be the full files (some downloads ship them empty).
3. The log is `USRDIR/thug-ps3.log` (the previous run's is `thug-ps3.old.log`), with a copy on
   a USB drive if one is plugged in. Please include it with any report.
4. For finding rendering bugs, an empty `USRDIR/rsx_capture.txt` makes the game save a
   screenshot about every 30 seconds as `USRDIR/thug-frame-NNN.bmp` and log every draw of that
   frame. Each capture pauses the game for several seconds; it is off by default.

These PKGs are built (each CI run keeps them for 7 days):

| PKG | What it is |
|-----|------------|
| `thug-ps3.pkg` | The game. |
| `thug-ps3-asserts.pkg` | The game with its asserts on, logging to `thug-ps3.log`. For tracking down crashes. |
| `thug-ps3-headless.pkg` | Test build: loads and runs the game logic without video, audio or pad, writing everything to the log. |
| `thug-ps3-profile.pkg` | Branches other than `main` only: the game with the renderer's functions timed. Every 10 s the log gets the 40 that take the most CPU time (`[fprof]` lines). Runs slower than the game. |

## Building

Every push builds the PKGs on GitHub Actions (**Actions** tab → latest run → artifacts). The
first run builds the PS3 toolchain (about two hours) and caches it.

Locally, on Linux or WSL:

```sh
# PS3 toolchain + PSL1GHT: https://github.com/ps3dev/ps3toolchain
export PS3DEV=/usr/local/ps3dev PSL1GHT=$PS3DEV
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH

make -f Makefile.ps3 -j$(nproc)              # thug-ps3.pkg
make -f Makefile.ps3 HEADLESS=1 -j$(nproc)   # thug-ps3-headless.pkg
make -f Makefile.ps3 ASSERTS=1 -j$(nproc)    # thug-ps3-asserts.pkg
make -f Makefile.ps3 PROFILE=1 -j$(nproc)    # thug-ps3-profile.pkg
```

`tools/ps3-check-all.sh` type-checks the whole tree with clang and the PSL1GHT headers, without
the PS3 toolchain.

## Testing switches

Empty files in `USRDIR` change how the port works, to help narrow down problems. None is
needed to play; leave them all out unless asked for one.

| File | Effect |
|------|--------|
| `rsx_capture.txt` | Save a screenshot and log every draw about every 30 s (see Installing) |
| `rsx_linear_textures.txt` | No texture swizzling (and so no mipmaps) |
| `rsx_flip_cull.txt` | Cull the other triangle winding (the old mapping) |
| `rsx_no_draw.txt` | Clear and flip only, draw nothing |
| `rsx_depth_clamp.txt` | Depth clamp instead of near/far clipping |
| `rsx_double_buffer.txt` | Two display buffers instead of three |
| `rsx_notiles.txt` | No tiled display or depth buffers |
| `rsx_no_zcull.txt` | ZCULL off |
| `rsx_keep_texture_data.txt`, `rsx_keep_buffer_data.txt` | Keep the main-memory copies of uploaded textures / vertex buffers |
| `rsx_no_target_wait.txt` | Don't make the RSX finish drawing into a texture before going on (brings back the shadow flicker) |
| `rsx_vs_always.txt` | Work out the vertex constants for every draw, even when nothing they come from changed |
| `rsx_fresh_label.txt` | Read the RSX's frame counter on every check instead of a copy refreshed every 32 draws |
| `rsx_slow_draws.txt` | Wait 2.5 µs in every draw (timing tests) |
| `draw_distance_75.txt`, `draw_distance_50.txt` | Draw distance 48000 or 32000 (the Xbox's) instead of the PC's 64000 |
| `draw_distance_fog.txt` | With either of those, fog over the last fifth of the distance to hide the cut |
| `no_frustum_cull.txt` | Draw every world mesh (no sphere test, visibility mask or occluders) |
| `no_frustum_test.txt`, `no_vis_mask.txt`, `no_occlusion.txt` | Turn off one of those three |
| `no_transparent_sort.txt` | Draw the sorted see-through meshes in scene order |
| `sort_front_to_back.txt` | Draw the opaque world in the original two loops |
| `no_stream_thread.txt` | Decode music and ambience on the mixer thread |
| `no_atomic_saves.txt` | Write saves in place instead of through a `.tmp` file |
| `no_device_lock.txt` | Don't serialise the renderer between threads |
| `no_crash_handler.txt` | Don't install the crash handler |
| `no_mallopt.txt` | Leave newlib's heap trimming on |
| `fs_no_missing_cache.txt` | Don't cache file names found missing |

For start-up problems: `nothreads.txt` (none of the port's threads; the game's
`CreateThread` calls are refused) and `memcap.txt` (allocations over 10 MB fail during
start-up). `thug-ps3-boot.txt` (in `USRDIR` and `/dev_hdd0/tmp`) is written from the first
line of `main()` and lists every allocation of 1 MB or more during start-up; the log has a
`[step]` line per start-up phase and an `[alive]` line every 5 seconds.
With a USB drive plugged in, both are also written to its root (`thug-ps3.log`,
`thug-ps3-boot.txt`), so they can be read on a PC without FTP.

## How the port is put together

- The PS3 build defines `__PLAT_WN32__` **and** `__PLAT_PS3__`: it follows kisak's PC code paths
  (which read the PC data), and `__PLAT_PS3__` branches replace what's Windows-specific.
- `Code/Ps3/include` holds stand-ins for the Windows, D3D9/D3DX and SDL APIs the PC code uses,
  implemented in `Code/Ps3` on PSL1GHT (lv2 threads and files, RSX, libaudio, pads).
- `Code/Core/endian.h` reads the little-endian PC data on either byte order (it compiles to plain
  loads on PC).
