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

**Making the archive from the launcher.** Without Python or the source tree, `mh2` makes both files itself: when it finds no
archive, its launcher opens on the first run's steps (or, from the Disc and archive page, **Make it from a disc image** or
**from a folder**). Choose your copy of the game, the `.iso` or the folder with the disc's files; mh2 checks each file against
the known NTSC-J disc, writes the 2.7 GB archive (it needs that much free disk and says so first), and times the movies with
`mh2movie`, which sits beside `mh2`. **Cancel** removes the half-written file. The archive is a copy: afterwards the game runs without
your original files. `tools/port/make_archive.py` and `tools/pc/movie_table.py` write the same two files, byte for byte, in a build
from source.

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
| `--set KEY=VALUE` | one setting, applied after the file (`archive`, `cards`, `card1`, `card2`, `fullscreen`, `mute`, `vsync`, `ssaa`, `hires`, `widescreen`, `texture_dump`, `texture_packs`, `textures`, `usb_keyboard`, `usb_keyboard_toggle`, `usb_key_NAME`, `states`, `state_key_save`, `state_key_load`, `state_key_next`, `state_key_prev`, `freecam_key`, `freecam_game_input`, `freecam_speed`, `freecam_pad_toggle`, `pictures`, `key.NAME`, `pad.NAME`) |
| `--launcher`, `--no-launcher` | show the launcher window, or never show it (below) |
| `--help` | every option, from the table the launcher and `mh2.ini` read |
| `--fullscreen` | start full screen |
| `--mute` | no sound |
| `--vsync auto\|on\|off` | presentation (auto: on in full screen only) |
| `--import-card FILE` | copy a memory card into the cards folder, then start |
| `--ssaa 1\|2\|4\|8\|16` | the renderer's super-sampling (1: the PS2's resolution) |
| `--hires` | the high-resolution output (implies `--ssaa 4`) |
| `--texture-dump` | write every texture the game draws, once, as a PNG named by what it is, to the `dump` folder (`texture_dump = on`) |
| `--texture-packs` | draw a texture from the PNG of its name in the `packs` folder, when there is one (`texture_packs = on`) |
| `--textures FOLDER` | the folder that holds `dump` and `packs` (`textures = FOLDER`; default: `textures` in the user data folder) |
| `--usb-keyboard` | connect the PC keyboard to the game as its USB keyboard, for the name and chat entry (`usb_keyboard = on`); `Scroll Lock` (`usb_keyboard_toggle`) switches the keyboard between typing and the pad's keys, and the Japanese keyboard's extra keys have stand-ins you can rebind (`usb_key_yen`, `usb_key_henkan`, `usb_key_muhenkan`, `usb_key_kana`, `usb_key_ro`, `usb_key_zenkaku`) |
| `--widescreen` | the wider view (`widescreen = on` in `mh2.ini`): the game's camera sees 4/3 wider, the HUD, the menus and the text keep their own shape, and the picture is shown at 16:9; the movies stay 4:3 |

**The launcher.** Started by itself, `mh2` opens a window before the game: the Picture and Game pages have the settings of the
table above (`ssaa`, `hires`, `fullscreen`, `vsync`, `mute`, the texture options, `widescreen`, `usb_keyboard`), Folders and logs
opens the user data folder and shows the last log, and **Play** starts the game. Arrow keys or a pad's D-pad choose and change a
value, `Enter` (or `L`, Circle) confirms, `Esc` (or `K`, Cross) goes back, the mouse works as well, and `F11` or `Alt+Enter` toggles full
screen. The changes are saved to `mh2.ini` when you press Play or answer Yes to "Save the changes?", and only the lines that
changed are touched: your comments and your own lines stay, and a setting given on the command line is used for that start and not
saved over. "Skip this screen next time" (`launcher = off`) starts the game at once; hold Shift at the start, or use `--launcher`, to show
the window again. The launcher never opens for the runs of tests and automation described below; it always opens when the archive is not found (it makes it, above).
It draws with Noto Sans (see Licences).

The enhancements (`ssaa`, `hires`, `widescreen`) and the texture options are off by default: the default picture is the PS2's. A
PNG in `packs` may be any whole multiple of its texture's size (2x, 4x) and is drawn in the same place; a file that cannot be used
is named once in the log, and the game's own texture is drawn. The dump is the game's own art: keep it to yourself. The keyboard layout
follows PCSX2's default (arrows, `I J K L` for the face buttons, `Q`/`E` for L1/R1, `Enter` for Start, `Backspace` for
Select, `W A S D` and `T F G H` for the sticks); `key.NAME` and `pad.NAME` rebind a button, and a gamepad works
as it comes.

**Save states.** `Home` saves the whole game to the chosen slot and `End` loads it; `Page Down` and `Page Up` choose one of the ten
slots (`state_key_save`, `state_key_load`, `state_key_next`, `state_key_prev` rebind the keys, `states = FOLDER` names the folder,
`states` in the user data folder by default). The game goes on from the very frame you saved, and a line on the picture says what
happened. Your memory cards are not rolled back by a load. Nothing is saved while a movie plays or the keyboard types, and a state is
refused by another build or other settings (another widescreen setting, the keyboard connected the other way, another renderer
build, `ssaa`, `hires`, texture packs) with the reason on the picture. **A state holds data from your disc: do not share it.**

**The freecam.** `F9` in the village or a hunt hands you the camera, starting where the game's camera is: `W A S D` move, `Q E` go down
and up, the mouse looks, the wheel changes the speed, `Shift` is fast and `Ctrl` slow, `Z X` change the field of view, `R` goes back
onto the game's camera, `Tab` hands the pad back to the game and takes it again, `Space` pauses the game and resumes it (the world stands still
and the camera still flies), `.` steps one frame while paused, `P` takes a photo (hiding the HUD with `H` is not ready yet), `Escape` or `F9` leaves. A gamepad: the left stick
moves, the right stick looks, `L2` and `R2` go down and up, `L1` and `R1` change the speed. By default the game is given a pad with
nothing pressed while the freecam has control (`freecam_game_input = neutral`; `pad` leaves the gamepad to the game), and the speed is
`freecam_speed` game units a second (400). Pressing both sticks in together (L3 and R3) turns the freecam on and off
(`freecam_pad_toggle = off` turns that off). In a menu or any scene that is not the village or a hunt the game has its pad back by itself.
`freecam_key` and `freecam_key_forward`, `_back`, `_left`, `_right`, `_down`, `_up`,
`_reset`, `_fov_narrow`, `_fov_wide`, `_hold`, `_pause`, `_step`, `_photo` and `_hide` rebind the keys. **Photo mode:** `H` hides the gauges, the
map and the texts (the fades stay), `P` writes `MH2-YYYYMMDD-HHMMSS.png` to the pictures folder (`pictures`, default `pictures` in the mh2
folder; the launcher's Folders page shows it) at the size of the picture you play at. A pause waits in menus, while a movie plays and while
the memory card is in use; the development build does not allow one while a recording is made. It changes only what is drawn: the game plays as it would
without it. It cannot show what the game builds around its own camera (the area's haze and dust sit where the game's camera is;
transparent things more than 1,600 units behind the game's camera are not drawn).

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

Files that carry their own notices keep them (newlib, fdlibm and zlib in `pc/core/crt`, `src/lobby` and
`pc/core/third_party/zlib`). The build links or loads:

- **SDL3** (zlib licence), built from the pinned source.
- **paraLLEl-GS**, Granite and volk (LGPL-3.0-or-later; MIT), built into `mh2gs`, which `mh2` loads at run time, so
  that it can be replaced. Its source is the pinned commits in `tools/versions.txt` plus the patches in
  `pc/gs/patches`.
- **stb_image** 2.30 and **stb_image_write** 1.16 (public domain or MIT, at your choice), unmodified, in
  `pc/gs/third_party/stb`; the renderer uses them for the textures it dumps and loads.
- **stb_truetype** 1.26 (public domain or MIT, at your choice), unmodified, in `pc/host/third_party/stb`; the launcher
  draws its text with it.
- **Noto Sans** Regular, release NotoSans-v2.015 (SIL Open Font License 1.1), unmodified, in `pc/host/fonts` with its
  licence text; the launcher embeds it in `mh2`. A path with letters it lacks is drawn with the system's font.
- **zlib** 1.3.1 (zlib licence), unmodified, in `pc/core/third_party/zlib`; `mh2-core` uses it to compress save
  states.
- **FFmpeg** (LGPL-2.1-or-later build, shared libraries only), which `mh2movie` loads at run time. The pinned
  builds are BtbN's; `tools/setup.sh` and `tools/setup.ps1` check each download against the SHA-256 in
  `tools/versions.txt`, and take it from the mirror among this repository's release files if BtbN's download is gone.
  The release files also carry FFmpeg's source at the pinned commit.

`THIRD_PARTY.txt` lists the components and their licences. Monster Hunter is a trademark of Capcom; this project
is not affiliated with or endorsed by Capcom, and it distributes none of Capcom's data.

## How this was made

The decompilation and this port were written with the help of Claude (Anthropic's AI model), working under the
direction of the repository's owner, who is responsible for what is committed.
