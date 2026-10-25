# PC data on the PS3: byte order and 64-bit layouts

The game data is the PC version's, which is **little-endian**. The PS3 is **big-endian**, and the
PS3 build is **LP64** (8-byte pointers and `long`). Every loader of binary data needs one of:

- explicit little-endian reads (`Endian::ReadLe32` etc. in `Code/Core/endian.h`), or
- an in-place conversion of the loaded buffer to host order (`Endian::Fix`, `Fix16Array`,
  `Fix32Array`), done once.

Structs mapped onto file data must also keep their on-disk layout: pointer members that are 4-byte
offsets in the file use `DATA_PTR(T)` (`Code/Core/ptr32.h`), a 4-byte pointer on PS3 (all PS3
addresses are below 4 GB) and a plain `T*` on PC.

Status: ✅ done · ⬜ to do · ➖ already safe (reads bytes explicitly)

## Containers

| Data | Code | Status |
|------|------|--------|
| `.pre` archives (PreMgr) | `Sys/File/PRE.cpp` | ✅ LE reads |
| `.pre` archives (Pip: qb.pre, collision, anims…) | `Sys/File/pip.cpp` `LoadPre` | ✅ headers converted on load |
| LZSS decompression | `Core/compress.cpp` | ➖ |
| Cutscene libraries `.cut` (`SLibHeader`, `SFileInfo`) | `Sys/File/FileLibrary.cpp` | ⬜ |

## Scripts

| Data | Code | Status |
|------|------|--------|
| `.qb` token stream | `Gel/Scripting/parse.cpp` `Read4Bytes`/`Read2Bytes`, `skiptoken.cpp` | ➖ |
| C-function pointers patched into tokens | `parse.cpp` `PreProcessScript`, `script.cpp` | ✅ kept in 32 bits, widened on PS3 |
| CStruct buffers (parks, saves, net) | `Gel/Scripting/utils.cpp` `ReadFromBuffer` | ➖ |

## Collision (used in place)

| Data | Code | Status |
|------|------|--------|
| `.col.xbx` headers, objects, verts, faces, BSP nodes, face indices | `Gel/Collision/CollTriData.cpp` `sFixFileByteOrder`, called from `Gfx/NxScene.cpp`, `Gfx/NxMesh.cpp` | ✅ |
| `CCollObjTriData` / `CCollBSPNode` pointer members | `Gel/Collision/CollTriData.h` | ✅ `DATA_PTR`; size asserts (64 / 8 bytes) |

## Textures and fonts

Texture memory holds texels in **host order**: 32-bit texels and CLUT entries as native `uint32`
ARGB words, 16-bit texels as native `uint16`. Code that writes `D3DCOLOR` values into a locked
texture therefore works unchanged, and the RSX backend reads A8R8G8B8 / A1R5G5B5 as big-endian
words. DXT blocks and 8-bit indices are copied as they are.

| Data | Code | Status |
|------|------|--------|
| `.tex.xbx` dictionaries (headers, palettes, DXT blocks, 16/32-bit texels) | `Gfx/DX9/p_nxtexture.cpp` `LoadTextureFile_Internal` | ✅ |
| `.img.xbx` (`sIMGHeader`, CLUT, texels) | `Gfx/DX9/NX/texture.cpp` `LoadTexture` | ✅ |
| `.fnt.xbx` and the built-in font | `Gfx/DX9/NX/chars.cpp` `LoadFont` | ✅ |
| `DwordizeTexelData` took pointers as `int` | chars.cpp, NX/texture.cpp, p_nxtexture.cpp | ✅ `intptr_t` |

## Geometry

| Data | Code | Status |
|------|------|--------|
| Scene headers, `CHierarchyObject` array | `Gfx/DX9/p_nx.cpp` `s_plat_load_scene(_from_memory)` | ✅ (size assert: 80 bytes) |
| Materials (incl. `uint64` reg_alpha, VC-wibble keyframes) | `Gfx/DX9/NX/material.cpp` (both loaders) | ✅ |
| Sectors: vertex/normal/weight/colour/UV arrays, index arrays | `Gfx/DX9/p_nxsector.cpp` | ✅ (material checksum now read as 4 bytes) |
| CAS tables `.cas.xbx` | `Gfx/DX9/p_NxMesh.cpp` | ✅ |
| Cutscene head weights `.wgt` | `Sk/Objects/cutscenedetails.cpp` | ⬜ |

## Animation

| Data | Code | Status |
|------|------|--------|
| `.ska` headers, frame tables, standard keys (incl. `timestamp:15` bitfield) | `Gfx/BonedAnim.cpp` `s_fix_anim_byte_order`, `Gfx/BonedAnimTypes.h` | ✅ (big-endian bitfield order on PS3) |
| Compressed keyframes | `BonedAnim.cpp` `get_compressed_q/t_frame` | ➖ |
| Q48/T48 tables | `BonedAnim.cpp` `init_48_table` | ✅ |
| Custom keys | `Gfx/CustomAnimKey.cpp` `ReadCustomAnimKey` | ✅ |
| Skeletons `.ske` | `Gfx/Skeleton.cpp` | ✅ |
| Cutscene CIF | `Sk/Objects/cutscenedetails.cpp` `create_objects` | ⬜ |

## Sound

| Data | Code | Status |
|------|------|--------|
| `.snd` RIFF headers / FourCCs | `Gel/SoundFX/SDL/p_sfx.cpp` `CXBSound` | ⬜ |
| IMA ADPCM samples | `Gel/SoundFX/SDL/ima.cpp` | ➖ |
| Music | miniaudio (`Gel/Music/SDL/p_music.cpp`) | ➖ |

## Park editor

| Data | Code | Status |
|------|------|--------|
| Compressed map buffer (packed fields in `uint32` words) | `Sk/ParkEditor2/EdMap.cpp` (NGC has `SwapMapBufferEndianness`) | ⬜ |
