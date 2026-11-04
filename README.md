# OpenMH2

OpenMH2 is a native PC port of **Monster Hunter 2 (Dos)**, the PlayStation 2 game, in its NTSC-J v1.04 release
(boot executable `SLPM_662.80`). It is built from source, on your machine, around **your own copy of the game**.
This repository holds no game data: no disc image, no extracted file, nothing derived from them. The program
that contains the game's code and data (`mh2-core`) is made on your machine from your disc and never leaves it.

The game's own code is the decompilation in `src/` and `include/` (the C that, compiled for the PS2, matches the
original executables byte for byte), compiled for the PC with the PS2's behaviour kept: the same state, frame by
frame, for the same inputs. What the PS2 provided (its kernel, its libraries, its graphics synthesizer, its sound
chip, its memory cards) is written in `pc/core` against the interfaces the game calls. `pc/host` is the program
you run: the window, the input, the sound device, the renderer and the movies.

## What you need

- A copy of Monster Hunter 2 (Dos), NTSC-J v1.04: the disc image (`.iso`), or the disc's files extracted into a
  folder (`SYSTEM.CNF` at its top). Its location is given to the build in `MH2_ORIG_DIR` (never written to).
- A GPU and driver with Vulkan 1.2, 8 GB of memory, and about 10 GB of disk (the tools, the build, and the
  game's archive of 2.7 GB).
- Linux x86-64 (Ubuntu 24.04 is the tested system), or Windows 11 x64. Windows needs no WSL.
- Git. Python and the other tools are installed for you, at pinned versions, in `tools/deps`.

## Building on Linux

```sh
tools/setup.sh                    # apt packages (sudo), then the pinned tools into tools/deps (--no-apt: skip apt)
source tools/env.sh               # in each new shell
export MH2_ORIG_DIR=/path/to/your/game/files     # the folder with the .iso, or with SYSTEM.CNF

python tools/configure.py --split-only
python tools/content_gen.py --jobs 8
python tools/asset_c.py config/assets/lbguild/lbguild.txt build/assets/lbguild/lbguild.c
python tools/pc/build.py --target core       # mh2-core, 32-bit: build/pc/linux-i686/core/mh2-core
python tools/pc/build.py --target host       # mh2, mh2gs, mh2movie: build/pc/linux-x86_64
python tools/port/make_archive.py            # the game's files as one archive: build/port/mh2-disc.arc (2.7 GB)
python tools/pc/movie_table.py --lib build/pc/linux-x86_64
```

The first three steps read your copy and write `build/` (the split and the sources generated from the game's
data); this takes a few minutes. `build.py --check` also runs the unit tests (`ctest`) after a build.

## Building on Windows (PowerShell, no WSL)

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup.ps1      # the pinned tools into tools\deps
. .\tools\env.ps1                                              # in each new shell
$env:MH2_ORIG_DIR = 'C:\path\to\your\game\files'

python tools\configure.py --split-only
python tools\content_gen.py --jobs 8
python tools\asset_c.py config\assets\lbguild\lbguild.txt build\assets\lbguild\lbguild.c
python tools\pc\build.py --target core
python tools\pc\build.py --target host
python tools\port\make_archive.py
python tools\pc\movie_table.py --lib build\pc\windows-x86_64
```

The split of the game needs only the files of your copy and the layout manifest in `config/pc/layout/`
(names, addresses and sizes: no game bytes), so there is nothing to run under WSL.

## Running

The host looks for `mh2-disc.arc` and `mh2-movies.txt` beside `mh2` (`build/pc/<system>-x86_64`), in the user
data folder and in `build/port`. Copy the two files next to `mh2`, or name the archive in a settings file.

```sh
build/pc/linux-x86_64/mh2
```

**Memory cards.** The game saves to PCSX2-format memory cards (8 MB, `Mcd001.ps2`). OpenMH2 keeps its cards in the
`memcards` folder of the user data folder. To bring a card from PCSX2 (or from another emulator), use
`mh2 --import-card FILE`; the card is copied, never changed.

**The user data folder** is `%APPDATA%\mh2-decomp\mh2` on Windows and SDL's preference path for `mh2-decomp`/`mh2`
on Linux (`~/.local/share/mh2-decomp/mh2`). It holds the cards, the log of the session (`mh2.log`, the last five
sessions kept as `mh2.1.log` ...), the optional `mh2.ini`, and, on Windows, a crash dump if the program crashes.
The log and the dumps stay on your machine: nothing is sent anywhere. The folder keeps this name so that cards
made by earlier versions are found.

**Settings.** `mh2.ini` (beside `mh2`, or in the user data folder, or `--config FILE`) holds `key = value` lines;
`--set KEY=VALUE` overrides it. The options of `mh2`:

| Option | Meaning |
|---|---|
| `--config FILE` | the settings file to read |
| `--set KEY=VALUE` | one setting, applied after the file (`archive`, `cards`, `card1`, `card2`, `fullscreen`, `mute`, `vsync`, `ssaa`, `hires`, `widescreen`, `key.NAME`, `pad.NAME`) |
| `--fullscreen` | start full screen |
| `--mute` | no sound |
| `--vsync auto\|on\|off` | presentation (auto: on in full screen only) |
| `--import-card FILE` | copy a memory card into the cards folder, then start |
| `--ssaa 1\|2\|4\|8\|16` | the renderer's super-sampling (1: the PS2's resolution) |
| `--hires` | the high-resolution output (implies `--ssaa 4`) |
| `--widescreen` | the wider view (`widescreen = on` in `mh2.ini`): the game's camera sees 4/3 wider, the HUD, the menus and the text keep their own shape, and the picture is shown at 16:9; the movies stay 4:3 |

The enhancements (`ssaa`, `hires`, `widescreen`) are off by default: the default picture is the PS2's. The keyboard layout
follows PCSX2's default (arrows, `I J K L` for the face buttons, `Q`/`E` for L1/R1, `Enter` for Start, `Backspace` for
Select, `W A S D` and `T F G H` for the sticks); `key.NAME` and `pad.NAME` rebind a button, and a gamepad works
as it comes.

Runs for tests and automation (`--archive`, `--core`, `--no-window`, `--gs-off`, `--quit-after N`, `--no-card`,
`--fast`, `--memcard1/2`, `--log-file`) do not touch the user data folder: they need `--set cards=DIR` to name a
folder of their own.

## What is in the repository

| Path | What |
|---|---|
| `src/`, `include/` | the game's C (the decompilation), with the PC forms under `#if PLATFORM_PC` |
| `config/` | what the split and the build read about the game; `config/pc/layout/` is the layout manifest |
| `pc/core` | `mh2-core`: the PS2's kernel, libraries, graphics, sound and cards for the PC (32-bit x86) |
| `pc/host`, `pc/ipc` | `mh2`: window, input, sound device, presentation, and its channel to the core |
| `pc/gs`, `pc/movie` | `mh2gs` (the renderer, on paraLLEl-GS) and `mh2movie` (the movies, on FFmpeg) |
| `tools/` | setup, the split, the generators and the build; `tools/port/make_archive.py` makes the archive |

## Licences

The project's own work in this repository is dedicated to the public domain under **CC0 1.0** (`LICENSE`): the
platform layer, the host and its launcher, the renderer's and the movies' bridges, the tools, the build, the
documentation, and what the project added to the decompiled code (its names, types and comments).

**Monster Hunter 2 is not covered.** The game's code, which the decompiled C reproduces, and its data and text
are Capcom's. The project claims no rights in them, and CC0 does not apply to them, to anyone's trademarks, or
to anything built or extracted from a player's copy of the game.

Files that carry their own notices keep them (newlib, fdlibm and zlib in `pc/core/crt` and `src/lobby`).
The build links or loads:

- **SDL3** (zlib licence), built from the pinned source.
- **paraLLEl-GS**, Granite and volk (LGPL-3.0-or-later; MIT), built into `mh2gs`, which `mh2` loads at run time, so
  that it can be replaced. Its source is the pinned commits in `tools/versions.txt` plus the patches in
  `pc/gs/patches`.
- **stb_image** 2.30 and **stb_image_write** 1.16 (public domain or MIT, at your choice), unmodified, in
  `pc/gs/third_party/stb`; the renderer uses them for the textures it dumps and loads.
- **FFmpeg** (LGPL-2.1-or-later build, shared libraries only), which `mh2movie` loads at run time. The pinned
  builds are BtbN's; `tools/setup.sh` and `tools/setup.ps1` check each download against the SHA-256 in
  `tools/versions.txt`, and take it from the mirror among this repository's release files if BtbN's download is gone.
  The release files also carry FFmpeg's source at the pinned commit.

`THIRD_PARTY.txt` lists the components and their licences. Monster Hunter is a trademark of Capcom; this project
is not affiliated with or endorsed by Capcom, and it distributes none of Capcom's data.

## How this was made

The decompilation and this port were written with the help of Claude (Anthropic's AI model), working under the
direction of the repository's owner, who is responsible for what is committed.
