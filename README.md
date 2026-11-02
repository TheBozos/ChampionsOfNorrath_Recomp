<div align="center">

# ChampionsOfNorrath_Recomp

**A native Linux and Windows port of *Champions of Norrath: Realms of EverQuest* (NTSC-U, `SLUS-20565`), made by statically recompiling the game's PlayStation 2 code to C++.**<br>
Not an emulator: the game's own MIPS R5900 code, rebuilt as a native program, drawing and playing sound natively.

![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-Clang-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-PS2Recomp-17a35b)
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

**Bring-up, started 2026-11-02.** The disc extracts (both layers of the DVD9), the game's code recompiles to about
16,100 C++ functions and the first runner is being built. It does not boot yet.

The port targets PS2Recomp's native route from the first day, like *Twisted Metal: Black*: no emulated GS, VU or
IOP hardware, nothing drawn from GS, GIF or VIF packets. Textures and models that the disc only holds as ready-made
GS and VIF data are converted once from your own disc into native packs in the build tree, never committed.

| Layer | State |
|---|---|
| Disc extraction, both layers | ✅ |
| EE code recompiled | 🔧 generated, first build |
| Boot to the title screen | ⏳ |
| IOP modules as native code (`tools/iop_recomp.py`) | ⏳ |
| Sound: native mixer at libsd's API, no SPU2 | ⏳ |
| Frame loop on the PC clock, no vblank pacing | ⏳ |
| 3D: native meshes and textures from packs converted from the disc | ⏳ |
| 2D, menus and HUD: the game's builders in C++ | ⏳ |
| Movies | ⏳ |
| No GS front end | ⏳ |
| Windows build | ⏳ |

✅ works · 🔧 in progress · ⏳ not reached yet

## Goal

A fully playable *Champions of Norrath* as a native executable, single player and local co-op (multitap), from the
opening movie to the end of the campaign, at the original frame rate. Accuracy first; once it is solid, widescreen and
higher internal resolution. Online play is out of scope.

## How it works

```
disc files ──► ps2_analyzer (the ELF's own symbol table) ──► recomp/con.toml
                                           │
                                ps2_recomp ──► C++ (one file per function)
                                           │
   PS2Recomp runtime: EE kernel HLE · native renderer · native mixer ─┼─► ps2EntryRunner
   PS2Recomp IOP:     the game's own IRX, recompiled (generated/iop/)
```

- The ELF keeps its symbol table (about 21,000 symbols), so the functions are named from the start.
- PS2Recomp turns every function into C++. The game's draw and sound paths are then taken over by native code fed
  from the game's own state, in `runtime/`.
- `runtime/con_overrides.cpp` holds the game-specific bindings.

The runtime is the `PS2Recomp/` git submodule: [TheBozos/PS2Recomp](https://github.com/TheBozos/PS2Recomp), branch
`con/ps2-disc-dual-layer` (`native/renderer` plus the dual-layer disc reader), the fork shared with the *Twisted
Metal: Black*, *Soulcalibur III*, *Valkyrie Profile 2: Silmeria*, *The Lord of the Rings: The Third Age*, *Dark Cloud*
and *Drakengard* recompilations. The top-level `CMakeLists.txt` builds its runtime, swaps its placeholder function
table for the generated one and links the generated code, `runtime/` and the recompiled IOP modules into
`ps2EntryRunner`. Every fix made for this port that is not specific to the game goes to the shared fork.

## Requirements

- Linux (developed on Arch) or Windows 10/11 with [MSYS2](https://www.msys2.org/) (UCRT64, see [Windows](#windows))
- CMake 3.21+, Ninja, Clang with C++20 (or MSYS2's GCC on Windows), rsync, Python 3
- FFmpeg development libraries (`libavcodec`, `libavformat`, `libavutil`, `libswresample`, `libswscale`)
- X11/Wayland and OpenGL development headers (raylib is fetched and built by the runtime)
- An OpenGL 4.5 driver (any recent NVIDIA, AMD or Intel driver)
- Your own copy of `SLUS-20565` as a disc image (the extracted ELF is checked against the hash above)

## Build and run

```bash
git clone --recurse-submodules https://github.com/TheBozos/ChampionsOfNorrath_Recomp.git
cd ChampionsOfNorrath_Recomp
scripts/extract_iso.sh "/path/to/Champions of Norrath (USA).iso"   # disc files, both layers -> game/
scripts/build_tools.sh                                              # ps2_recomp, ps2_analyzer -> build/tools
scripts/recomp.sh                                                   # SLUS_205.65 + recomp/con.toml -> generated/
scripts/build_runtime.sh                                            # runtime + generated code -> build/game (RelWithDebInfo)
scripts/run.sh                                                      # ps2EntryRunner game/SLUS_205.65
```

The extracted disc folder is the game's `cdrom0:`. `scripts/analyze.sh` regenerates `recomp/con.toml` from the ELF
(it overwrites hand edits: diff before committing). `JOBS=<n>` sets the build parallelism of every script (default:
all cores). `scripts/build_runtime.sh Release` or `Debug` picks another build type, and `CXX` another compiler.

`scripts/remote_tools.sh` and `scripts/remote_build.sh` build the tools and the runner on another x86-64 Linux
machine over ssh (`REMOTE_HOST`, default `stallman`) and copy them back to `build/`; the game still runs locally.
The remote host needs rsync, mise and the X11 and OpenGL development headers; the local FFmpeg headers and libraries
are copied over once, so the runner links against the FFmpeg it runs with.

### Saves, settings and logs

| | Linux | Windows |
|---|---|---|
| User folder | `$XDG_DATA_HOME/championsofnorrath_recomp` (`~/.local/share/championsofnorrath_recomp`) | `%APPDATA%\ChampionsOfNorrath_Recomp` |

It holds the memory cards (`mc0/`, `mc1/`), the controls (`input.ini`), the video settings (`video.ini`) and the log
of the last runs (`logs/`). `PS2X_USER_DIR=<dir>` uses another folder, `PS2X_LOG_FILE=<file>` writes the log there
(`0`: no log file), and `PS2X_NO_DIALOGS=1` keeps the message boxes in the log only, for unattended runs.

### Windows

The same scripts run in an MSYS2 **UCRT64** shell, with MSYS2's GCC (MSVC is not supported):

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,ffmpeg,python} git rsync
git clone --recurse-submodules https://github.com/TheBozos/ChampionsOfNorrath_Recomp.git
```

Then follow the steps above. `scripts/build_runtime.sh` ends with `scripts/stage_dlls.sh`, which copies the DLLs the
runner needs (compiler runtime, FFmpeg and their dependencies) from the MSYS2 prefix next to `ps2EntryRunner.exe`,
so it also starts outside the MSYS2 shell. `scripts/remote_tools.sh` and `scripts/remote_build.sh` are Linux only.

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
| `PS2Recomp/` | The recompiler and runtime (git submodule, TheBozos/PS2Recomp branch `con/ps2-disc-dual-layer`) |
| `CMakeLists.txt` | Builds the runtime with the generated code and the game overrides linked in |
| `recomp/` | Recompiler config (`con.toml`) |
| `runtime/` | Game override module (`con_overrides.cpp`) |
| `scripts/` | Extract, analyze, recompile, build (local and remote) and run scripts |
| `game/`, `generated/`, `build/` | Created by the scripts, ignored by git: disc files, generated C++, builds |

Everything derived from the game (disc files, generated code, builds, logs) stays outside this repository.
Never commit or redistribute it.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPL), the recompiler and runtime this port is built on.
- [TheBozos/PS2Recomp](https://github.com/TheBozos/PS2Recomp) and the *Twisted Metal: Black*, *Soulcalibur III*, *Valkyrie Profile 2: Silmeria*, *The Lord of the Rings: The Third Age*, *Dark Cloud* and *Drakengard* recompilations developed on it; runtime fixes are shared both ways.
- *Champions of Norrath* © Sony Online Entertainment, developed by Snowblind Studios. This is an unofficial fan project, not affiliated with or endorsed by them.
