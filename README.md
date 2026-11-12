<div align="center">

# ChampionsOfNorrath_Recomp

**A native Linux and Windows port of *Champions of Norrath: Realms of EverQuest* (NTSC-U, `SLUS-20565`), made by statically recompiling the game's PlayStation 2 code to C++.**<br>
Not an emulator: the game's own MIPS R5900 code, rebuilt as a native program, drawing and playing sound natively.

![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-Clang-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-BozoEngine-17a35b)
![Status](https://img.shields.io/badge/status-bring--up-c0392b)

</div>

> [!IMPORTANT]
> No game files are included: no ELF, IRX modules, level archives, or code generated from them.
> You need **your own copy** of the North American release, PS2 ID **`SLUS-20565`**. The port is built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SLUS_205.65` | `89fc400cf533d3eacca4264c8c315ad30d9fd735` |

## Status

**Bring-up. It runs headless, and it draws nothing yet.** With a scripted pad, `con-headless` goes through the start-up
screens and into levels for 6,000 frames with no fault, which the game's own log shows (`to level intro`, `to level
kelsel`). There is no window and no picture, and the sound has not been listened to.

Everything runs native, with no emulated GS, VU or IOP hardware and nothing read from GS, GIF or VIF packets.

| Layer | State |
|---|---|
| Disc extraction, both layers | ✅ |
| EE code recompiled | ✅ |
| Start-up, memory card, multitap, the front-end, into a hub level | ✅ |
| VU0 programs as native C++ | ✅ (no reference run to compare against) |
| The disc's sound modules as native code over native libsd | 🔧 the mixer makes sound from about 18 s in a run; nobody has listened to it |
| A window held to real time | 🔧 runs at 60 frames a second, into the hub level Kelethin |
| Frame loop on the PC clock, no vblank pacing | 🔧 frames held to the PC clock; the game's vblank waits are still there |
| Textures: native decoder reading the disc image's `.TEX` files at run time | 🔧 all 650,820 decode; drawn on the level's geometry |
| 3D: native meshes | 🔧 level geometry, scenery models and lit, skinned characters with their equipment and particle sprites (torch fire, embers, glows) draw with the game's camera, spark lines included; water and shadows don't |
| 2D, menus and HUD | 🔧 the game's sprite and font calls drawn natively, menus, dialogs and screen fades show; untextured 2D isn't drawn |
| Movies | ⏳ |
| Window | ⏳ |
| Windows build | ⏳ not tried |

✅ works · 🔧 in progress · ⏳ not reached yet

## Goal

A fully playable *Champions of Norrath* as a native executable, single player and local co-op (multitap), from the
opening movie to the end of the campaign, at the original frame rate. Accuracy first; once it is solid, widescreen and
higher internal resolution. Online play is out of scope.

## How it works

```
disc ELF ──► bozo-recomp (the ELF's own symbol table) ──► C++ in the build tree
disc IRX ──► BozoEngine tools/native_irx ─────────────► C++ in the build tree
                                                              │
   BozoEngine: native kernel · native Sony libraries · native libsd ─┼─► con-headless
   runtime/:   CoN's overrides, native VU0 programs, sound wiring ───┘
```

- The ELF keeps its symbol table (about 21,000 symbols), so the functions are named from the start.
- `bozo-recomp` turns the game's code into C++. BozoEngine answers Sony's libraries natively by name.
- Instructions that only reach graphics hardware are listed in `runtime/con_graphics.cpp` and replaced before they
  run; retired graphics registers fault if anything else reaches them.
- SDRDRV, LIBSD and HELLO are translated from your disc at build time and run on BozoEngine's compiled-library host.

The engine is the `BozoEngine/` git submodule: [TheBozos/BozoEngine](https://github.com/TheBozos/BozoEngine). Fixes made
for this port that are not specific to the game go there.

## Requirements

- Linux (developed on Arch). Windows is the goal, but not tried yet.
- CMake 3.21+, Ninja, Clang or GCC with C++20, Python 3
- FFmpeg development libraries for BozoEngine's movie decoder (`BOZO_FFMPEG_ROOT` names a folder of them)
- Your own copy of `SLUS-20565` as a disc image

## Build and run

```bash
git clone --recurse-submodules https://github.com/TheBozos/ChampionsOfNorrath_Recomp.git
cd ChampionsOfNorrath_Recomp
scripts/extract_iso.sh "/path/to/Champions of Norrath (USA).iso"   # disc files, both layers -> game/
cmake -S . -B build/bozo -G Ninja -DCON_ELF="$PWD/game/SLUS_205.65" -DBOZO_BUILD_TOOLS=OFF
cmake --build build/bozo --target con-headless
build/bozo/con-headless --elf game/SLUS_205.65 --iso game/disc.iso --frames 3000 --print
```

For the window, configure with `-DCON_WINDOW=ON` and build the `con` target. It takes `--iso`, or `disc.iso` beside the ELF.

The run prints what the game logs and why it stopped. Without input it stays in the start-up screens; `--pad-script`
takes a frame-keyed script (`frame:BUTTON/frames`, with upper-case button names, such as `300:START/8`).

`scripts/remote_bozo_build.sh` builds on another x86-64 Linux machine over ssh (`REMOTE_HOST`, default `stallman`) and
copies the program back.

## Disc facts

| | |
|---|---|
| Boot ELF | `SLUS_205.65`, entry `0x100008`, with its symbol table; code also in a `.srs` segment at `0x1000000` |
| Video mode | NTSC (`SYSTEM.CNF` `VER = 1.02`) |
| Disc | DVD9: the second layer is a volume of its own, 103 of the 744 files (levels in `BG/DATA`) are only there |
| IOP modules | `MODULES/*.IRX` and `BG/0MOD/*.IRX`: LIBSD, SDRDRV, SNDDRV, PADMAN, MTAPMAN, MCMAN, MCSERV, SIO2MAN, CDVDSTM, and the network and USB stacks |
| IOP image | `BG/0MOD/IOPRP280.IMG` |
| Data | `BG/DATA/*.GOB`, `.TEX`, `.VAT`, `.LMP` (levels), `DATA/`, `DATA2/` (UI, fonts, strings); `NTGUI2.ELF` is the network setup program |

## Repository layout

| Path | Contents |
|---|---|
| `BozoEngine/` | The recompiler and runtime (git submodule, TheBozos/BozoEngine) |
| `CMakeLists.txt`, `cmake/bozo.cmake` | Generate the game's code and link it with `runtime/` |
| `bozo/` | The runners (`con_headless.cpp`, `con_window.cpp`) and the texture check (`con_tex_check.cpp`) |
| `recomp/` | Functions left as stubs for the overrides (`con_stubs.txt`) |
| `runtime/` | The game's overrides, native graphics list, VU0 programs, sound wiring, texture decoder, native 2D and world drawing |
| `tests/` | The texture decoder on synthetic cells, no game data (`con-textures-tests`) |
| `tools/` | Override list and IOP module image generators, the Python texture decoder (`con_tex.py`) |
| `scripts/` | Disc extraction and the remote build |
| `game/`, `build/` | Created by the steps above, ignored by git: disc files, generated C++, builds |

Everything derived from the game (disc files, generated code, builds, logs) stays outside this repository.
Never commit or redistribute it.

## Credits

- **The Bozos**: boubou666 and Arzaroth ([RecompWatcher](https://thebozos.github.io/RecompWatcher/) shows our projects).
- [BozoEngine](https://github.com/TheBozos/BozoEngine), The Bozos' MIT recompiler and native runtime, this port's engine.
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPL-3.0), on which the port's first bring-up was built.
- The [Return to Arms research](https://github.com/Open-Champions/champions-return-to-arms-re) (MIT) for the engine's file formats.
- *Champions of Norrath* © Sony Online Entertainment, developed by Snowblind Studios. This is an unofficial fan project, not affiliated with or endorsed by them.
