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

Work in progress. **It does not run on a console yet.**

- [x] Import kisak-thug; build on GitHub Actions (ps3toolchain + PSL1GHT, cached)
- [x] PS3 platform target (`__PLAT_PS3__` alongside the PC code paths), PSL1GHT Makefile
- [x] Windows-cased and backslashed `#include`s resolved without renaming files
- [x] System layer: start-up, log file, XMB events, case-insensitive paths for the PC data,
      Win32 API stand-ins (threads, timers, files, directory search)
- [x] Input: DualShock 3 (the game's PS2 pad layout maps one to one)
- [x] Audio output: the game's mixer on an 8-channel libaudio port
- [x] D3D9 layer with a headless backend; RSX backend stage 1 (video mode, flip, clears), with
      the command-buffer fix from GridSiege's RSX renderer (no random RSX lock-ups)
- [x] Builds and links with the PPU GCC; both PKGs are produced by CI
- [x] Little-endian PC data on the big-endian PS3: every known loader converts (see
      [docs/ps3-byte-order.md](docs/ps3-byte-order.md)); untested on a console
- [ ] RSX rendering: 2D (menus, loading screens), then 3D meshes and shaders
- [ ] Saves (`USRDIR/Save`), music

## Installing (once it runs)

1. Install `thug-ps3.pkg` on a PS3 with HEN or CFW. It appears in the XMB as
   **Tony Hawk's Underground**.
2. The PKG creates `/dev_hdd0/game/THUG00001/USRDIR/Data`. Copy the contents of the `Data`
   folder from your PC installation of THUG into it (by FTP). Letter case doesn't matter.
3. The log is `USRDIR/thug-ps3.log` (the previous run's is `thug-ps3.old.log`). Please include
   it with any report.

Two PKGs are built:

| PKG | What it is |
|-----|------------|
| `thug-ps3.pkg` | The game. |
| `thug-ps3-headless.pkg` | Test build: loads and runs the game logic without video, audio or pad, writing everything to the log. Useful for checking the data loads before the renderer is finished. |

## Building

Every push builds both PKGs on GitHub Actions (**Actions** tab → latest run → artifacts). The
first run builds the PS3 toolchain (about two hours) and caches it.

Locally, on Linux or WSL:

```sh
# PS3 toolchain + PSL1GHT: https://github.com/ps3dev/ps3toolchain
export PS3DEV=/usr/local/ps3dev PSL1GHT=$PS3DEV
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH

make -f Makefile.ps3 -j$(nproc)              # thug-ps3.pkg
make -f Makefile.ps3 HEADLESS=1 -j$(nproc)   # thug-ps3-headless.pkg
```

`tools/ps3-check-all.sh` type-checks the whole tree with clang and the PSL1GHT headers, without
the PS3 toolchain.

## How the port is put together

- The PS3 build defines `__PLAT_WN32__` **and** `__PLAT_PS3__`: it follows kisak's PC code paths
  (which read the PC data), and `__PLAT_PS3__` branches replace what's Windows-specific.
- `Code/Ps3/include` holds stand-ins for the Windows, D3D9/D3DX and SDL APIs the PC code uses,
  implemented in `Code/Ps3` on PSL1GHT (lv2 threads and files, RSX, libaudio, pads).
- `Code/Core/endian.h` reads the little-endian PC data on either byte order (it compiles to plain
  loads on PC).
