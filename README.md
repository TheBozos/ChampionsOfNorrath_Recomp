# Champions of Norrath Recomp

A static recompilation of **Champions of Norrath: Realms of EverQuest** (PlayStation 2, USA, SLUS-20565) to native PC
code for Linux and Windows, built on [PS2Recomp](https://github.com/TheBozos/PS2Recomp) and its native renderer.

No game data is in this repository: you need your own disc image. Everything derived from it (the recompiled code,
converted textures and models) is generated in your build tree and never committed.

## Status

Bring-up. The target is a fully native port: no emulated GS, VU or IOP, nothing drawn from GS packets.

| Layer | State |
|---|---|
| Disc (dual-layer DVD9, both layers) | done |
| EE code recompiled (16k functions) | generated, first build |
| Boot | not yet |
| IOP modules as native code | not yet |
| Sound (native mixer) | not yet |
| 3D (native meshes and textures from the disc) | not yet |
| 2D, menus, HUD | not yet |
| Movies | not yet |
| Windows build | not yet |

## Build and run

Requires CMake 3.21+, Ninja, Clang or GCC with C++20, Python 3, and FFmpeg development files.

```sh
git clone --recursive https://github.com/TheBozos/ChampionsOfNorrath_Recomp.git
cd ChampionsOfNorrath_Recomp
scripts/extract_iso.sh "/path/to/Champions of Norrath (USA).iso"   # into game/
scripts/build_tools.sh                                              # ps2_recomp, ps2_analyzer
scripts/recomp.sh                                                   # generated/
scripts/build_runtime.sh                                            # build/game
scripts/run.sh
```

`scripts/remote_tools.sh` and `scripts/remote_build.sh` do the two build steps on another Linux machine over ssh.

### Windows

Build from an MSYS2 UCRT64 shell with its GCC, CMake, Ninja and FFmpeg packages; `scripts/build_runtime.sh` copies
the DLLs the runner needs next to it (`scripts/stage_dlls.sh`).

## Saves, settings and logs

The runner keeps its memory cards, settings and log in a per-user folder: `%APPDATA%\ChampionsOfNorrath_Recomp` on
Windows, `$XDG_DATA_HOME/championsofnorrath_recomp` (`~/.local/share/championsofnorrath_recomp`) on Linux.

## Credits

The Bozos (boubou666 and Arzaroth). PS2Recomp by ran-j and contributors.
