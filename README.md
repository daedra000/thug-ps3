# THUG PS3

A homebrew PlayStation 3 port of **Tony Hawk's Underground** (2003), for consoles running HEN or CFW.

Built on [kisak-thug](https://github.com/SwagSoftware/kisak-thug) (the PC/DX9 port, imported at
`98b4e24`), which is built on the [thug1src](https://github.com/thug1src/thug) source release.
kisak-thug's original README is kept in [README-kisak-thug.md](README-kisak-thug.md).

**No game data is included.** You supply the `Data` folder from your own legitimate copy of the
**PC version** of THUG.

## Status

Work in progress. Nothing runs on a console yet.

- [x] Import kisak-thug
- [x] CI: build and cache the PS3 toolchain (ps3toolchain + PSL1GHT) on GitHub Actions
- [ ] `__PLAT_PS3__` platform target and PSL1GHT Makefile
- [ ] Big-endian / 64-bit pointer fixes in the asset loaders (PC data is little-endian)
- [ ] System layer: files, timer, memory, threads
- [ ] Input (pad), audio
- [ ] RSX renderer (replacing the DX9 path)
- [ ] PKG packaging

## Building

Push to GitHub and the **PS3 build** workflow does everything. The first run builds the toolchain
(roughly two hours) and caches it; later runs reuse the cache.
