# OpenMH2

**OpenMH2 lets you play Monster Hunter 2 (Dos) on your PC, on Windows or Linux.** Dos is the PlayStation 2 game that never left Japan.

**A fully native port, built from the ground up.** OpenMH2 is built on [mh2-decomp](https://github.com/Batenkaitos-dev/mh2-decomp), which turns the game's code back into readable C source that, compiled for the PS2, rebuilds the original game byte for byte. OpenMH2 compiles that same source into a native PC program, with its own new code in place of the PS2's system software, so no PS2 BIOS is needed. It is not an emulator and not a recompilation: unlike a "recomp", it does not translate the PS2's machine code, and nothing emulates the PS2's processor.

It adds options the PS2 never had: widescreen, sharper graphics, texture packs, save states and more. Every option is off by default, so the game plays as it did on the PS2 until you turn something on.

**This repository holds no game data.** You need your own copy of the game, and OpenMH2 builds everything it needs
from it, on your PC. Nothing made from your copy ever leaves your PC, and you should not share it.

## What you need

- **Your own copy of Monster Hunter 2 (Dos)**: the Japanese PS2 game, NTSC-J, version 1.04 (disc code
  `SLPM-66280`, boot file `SLPM_662.80`). Use the disc image (`.iso`) or a folder with the disc's files (the one
  with `SYSTEM.CNF` in it). Other versions and regions do not work.
- **A PC** with Windows 11 (64-bit) or Linux (64-bit; Ubuntu 24.04 is the tested system), a graphics card and driver
  with Vulkan 1.2, 8 GB of memory, and about 5 GB of free disk space (about 10 GB to build from source).

## Install and start

1. Download the release for your system (Windows or Linux) from this repository's Releases page, and unpack it
   anywhere you like. You do not need Git, Python or any other program.
2. Run `mh2` (`mh2.exe` on Windows). The **launcher** opens.
3. On the first start, the launcher walks you through the setup. You choose your disc image or folder, the launcher
   makes the game archive (a copy of your disc in one file, 2.7 GB), builds the game on your PC (5 to 15 minutes),
   and sets up your memory card. It does this once, and builds the game again after an update.
4. Press **Play**. Next time, press **Play** straight away, or turn on **Skip this screen next time** to start the
   game at once.

If you want to build OpenMH2 from the source code instead, see "Building from source" below.

## Where your saves go

The game saves to memory cards, as on the PS2. They are normal PCSX2-format memory card files (8 MB,
`Mcd001.ps2`) in the `cards` folder of the user data folder:

- Windows: `%APPDATA%\mh2-decomp\mh2` (paste it into Explorer's address bar)
- Linux: `~/.local/share/mh2-decomp/mh2`

The launcher's **Folders and logs** page opens that folder. Your settings (`mh2.ini`), save states, textures,
photos and the log of your last sessions are kept there too. To bring in a card from PCSX2 or another emulator, run
`mh2 --import-card FILE`. Back up the `cards` folder from time to time.

## Where to get help

- Read the **guide**, `docs/guide.md`. It covers the first start, the launcher, the controls, every setting, and
  what to do when something goes wrong.
- If the game does not start or behaves badly, look at the log first: the launcher's **Folders and logs** page
  shows it. Then open an issue in this repository. The guide's last sections say what to include.
- To change the code, read `CONTRIBUTING.md`.

## What it can do

All of these are options. The guide explains each one.

- **Widescreen**: the game in 16:9, with a wider camera view. Movies stay 4:3.
- **Anti-aliasing** (2x to 16x) and **high resolution** (twice the original, 1280x896) for a sharper image.
- **Texture packs**: replace the game's textures with your own PNG files, at the original resolution or higher (2x, 3x, etc.), and dump the game's textures to start from.
- **Translation packs**: replace the game's text with a translation. OpenMH2 provides the tools to make one: see [Making a translation pack](#making-a-translation-pack).
- **Save states**: save and load the whole game at any moment, with ten slots.
- **Fast loading, fast-forward and pause**, with a frame advance key.
- **Free camera and photo mode**: fly the camera through the village or a hunt, and take photos.
- **Camera on the right stick**, so a gamepad can turn the game's camera.
- **Keyboard typing**: type names and chat with your keyboard, including a Japanese keyboard's extra keys.
- **Offline Dundorma**: visit Dundorma, the online town, without an internet connection. You can take its online
  quests and earn Hunter Rank, awards and rewards, saved to your memory card as usual.

**PS2-accurate math, built in.** A PC does some of the game's math (positions, physics, the camera, damage)
slightly differently from the PS2, and that can change small things in play, such as whether a hit bounces off a
monster. OpenMH2 does this math the way the PS2 does. Before, this fix had to be applied externally; in OpenMH2 it
is a native fix, always on.
Credit to PCSX2 for providing a solution for better emulating the PS2's math on PCs.

## Not in this release

- **Higher frame rates.** The game runs at 30 frames per second, as it did on the PS2. Higher frame rates are currently being researched. They are difficult, because everything in the game is tied to its original frame rate.
- **An English translation.** OpenMH2 does not come with an English translation, out of respect for the hard work of translators like the BREAK ARTS team. If you want to, you can make your own translation pack with the tools OpenMH2 provides: "Making a translation pack", below, says where they are and how to use them.
- **Online play.** OpenMH2 has no online play, and it sends nothing anywhere. It is not set up to connect to the MHOldschool servers, out of respect for the MHOldschool team.

If either team would like to give their confirmation for packaging the BREAK ARTS translation or for allowing connections to the MHOldschool servers, they can contact the project's author at batenkaitos.dev@gmail.com.

## Making a translation pack

A translation pack is a folder that replaces the game's text with your own. Nothing in the game's files or in your saves is changed. OpenMH2 gives you the tools to make one. The words are yours, and so is the choice of which text to translate. This is the short version. The full guide is `docs/translation-packs.md`.

**Where the tools are.** In `tools/pc`: `textpack.py`, with `textpack_table.py` and `textpack_font.py` beside it. They need Python 3 and nothing else (they use only Python's own library). In the download they are in `kit/src/tools/pc`, and the download's own Python runs them, so you need no other Python: `kit\python\python.exe kit\src\tools\pc\textpack.py` on Windows, `kit/python/bin/python3 kit/src/tools/pc/textpack.py` on Linux. Below, the commands start with `python3 tools/pc/textpack.py`: in the download, start them with that instead.

**What you need.** Your game archive, `mh2-disc.arc`, which the launcher made from your disc: it is in the user data folder (see "Where your saves go"), unless you chose another place for it on the launcher's **Game archive** page. Below, `ARCHIVE` stands for its full path. You also need a text editor that saves UTF-8 and keeps tabs as tabs.

**The dialogue and the quests**, which are most of the text:

1. Write the game's text into files you can edit:

   ```
   python3 tools/pc/textpack.py out --archive ARCHIVE --to work/text
   ```

   This makes a text file for each of the 13 dialogue tables (`work/text/talk`) and each of the 1,968 quest files (`work/text/quest`). Each line is a key, a tab, and the game's Japanese text.
2. Open the files you want to translate, and change the text after the tab. Leave the keys and the tab as they are. Keep the game's own codes (`~C05`, `%d`) where they belong, and write `\n` for a new line: the game does not wrap lines, so you break them yourself. Lines and files you leave alone stay as the game has them.
3. Make the pack from your files:

   ```
   python3 tools/pc/textpack.py in --archive ARCHIVE --from work/text --pack PACK --name "My pack"
   ```

   `PACK` is the new folder for your pack. A dialogue table cannot grow longer than the original; the tool says so, and by how much.
4. Check it. This applies the game's own tests to the pack without starting the game, and lists every problem it finds:

   ```
   python3 tools/pc/textpack.py check PACK --archive ARCHIVE
   ```
5. Put the `PACK` folder in the `text_packs` folder of the user data folder (make the `text_packs` folder if it is not there). To use it, add `text_pack = NAME` to `mh2.ini`, where `NAME` is the pack folder's name. `mh2 --text-pack NAME` does the same for one start. `text_pack = off` turns it off. (The launcher does not list translation packs. The launcher's **Folders and logs** page shows where `mh2.ini` is.)

A save state made with a pack loads only with the same pack.

**The rest of the game's text** is in the game's own code: the menus, the help and the names of items and monsters (about 10,900 strings). `textpack.py strings` lists them and `textpack.py table` turns your translation of them into part of the pack. The launcher writes the list for you when it builds the game: it is `core/strings.tsv` in the user data folder. Copy it (the launcher writes it again when it builds the game again), write your translation in the fourth column of each line you want to change, and keep every `%` code of the original, in the same order. Then run (`STRINGS` is the path of your copy):

```
python3 tools/pc/textpack.py table --from STRINGS --pack PACK --name "My pack"
```

**Letters the game's font does not have**, such as accents, go in a character map (`charmap.txt`, one letter a line). Plain English needs no character map and no `--charmap`. For a language that does, add `--charmap charmap.txt` to `out`, `in` and `table`, then draw the letters with `textpack.py glyphs --ttf FONT.ttf --charmap charmap.txt --to PACK/glyphs.txt` (`FONT.ttf` is a TrueType font you may use: the download has none) and put them in the game's font with `textpack.py font --archive ARCHIVE --pack PACK`.

**Sharing a pack.** Publish only your own work, never the game's text: not the `files` folder of the pack and not the files `out` made. `changes` copies just the lines you changed into a folder, and whoever has the game makes the pack from that folder with `build`:

```
python3 tools/pc/textpack.py changes --archive ARCHIVE --from work/text --to SOURCE
python3 tools/pc/textpack.py build --archive ARCHIVE --from SOURCE --pack PACK
```

Copy your `pack.ini` into `SOURCE` first (and `charmap.txt`, `text.tsv` and `glyphs.txt` if you made them). Section 6 of `docs/translation-packs.md` has the details.

## Building from source

Most players do not need this. Building from source is for modders and for anyone who wants to check the code. It
takes about 15 minutes after the downloads (the setup downloads about 2 GB of tools).

On Linux:

```sh
tools/setup.sh                    # apt packages (sudo), then the pinned tools into tools/deps (--no-apt: skip apt)
source tools/env.sh               # in each new shell
export MH2_ORIG_DIR=/path/to/your/game/files     # the folder with the .iso, or with SYSTEM.CNF

python tools/configure.py --split-only
python tools/content_gen.py --jobs 8
python tools/asset_c.py config/assets/lbguild/lbguild.txt build/assets/lbguild/lbguild.c
python tools/pc/build.py --target core       # mh2-core, 32-bit: build/pc/linux-i686/core/mh2-core
python tools/pc/build.py --target host       # mh2, mh2gs, mh2movie: build/pc/linux-x86_64
python tools/port/make_archive.py            # the game archive: build/port/mh2-disc.arc (2.7 GB)
python tools/pc/movie_table.py --lib build/pc/linux-x86_64
```

On Windows (PowerShell, no WSL needed):

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

Then run `build/pc/linux-x86_64/mh2` (on Windows, `mh2.exe` in `build\pc\windows-x86_64`). The launcher can also make
the game archive and the movie table for you, so the last two commands are optional. `MH2_ORIG_DIR` is only read from,
never written to. Both systems need Git. Neither needs `make`, except `--build-ffmpeg` and `-BuildFFmpeg`, which build
FFmpeg's libraries from source instead of installing the prebuilt ones. `build.py --check` also runs the unit tests.
The guide's section 2 explains the build in more detail.

## How it works

It is not an emulator and not a recompilation.
The game's code is compiled from source, not translated from the PS2's machine code.

- **The game's code** is the C source in `src/` and `include/`. It is a decompilation of the game: compiled for
  the PS2 with the compiler the game was built with (Metrowerks CodeWarrior), it matches the original executables
  byte for byte. For the PC, clang compiles the same source into a native x86 program. A special compiler step keeps
  the PS2's arithmetic (floating point, shifts and division) exact, so the game reaches the same state, frame by
  frame, for the same inputs. Where the PC needs a different form of the code, it sits under `#if PLATFORM_PC`, next
  to the PS2's.
- **None of the console's software is included.** The game used Sony's SDK (the kernel's services, and the
  libraries for graphics, controllers, memory cards, the disc and sound) and CRI's middleware (ADX audio, its file
  system and Sofdec movies). Their code is not in this repository or in the program, and no PS2 BIOS is used.
  `pc/core` has OpenMH2's own version of each function the game calls, written from scratch for the PC from public
  documentation, from how the game uses each function, and from observing how the original behaves. The game's
  threads run on fibers with the PS2 kernel's scheduling, the disc is read from the game archive, the memory cards
  are files in a folder, and the sound is mixed the way the PS2's sound chip mixes it.
- **The PS2's other processors.** Capcom's sound driver, which ran on the PS2's I/O processor, is rewritten in C
  from Capcom's module and its observed behaviour. The vector unit's microcode, the small programs that transform
  the game's 3D geometry, was written in that unit's own assembly language. The decompilation keeps it as assembly
  source (`src/sub_main/vu1_microcode.vsm`), and the build compiles it into C. The graphics chip's work is done by
  paraLLEl-GS on Vulkan, in `mh2gs`. The movies are decoded by FFmpeg, in `mh2movie`.
- **`pc/host`** is the program you run, `mh2`: the window, the input, the sound device, the launcher and the
  display.

| Path | What |
|---|---|
| `src/`, `include/` | the game's C (the decompilation), with the PC forms under `#if PLATFORM_PC` |
| `config/` | what the split and the build read about the game; `config/pc/layout/` is the layout manifest |
| `pc/core` | OpenMH2's own versions of the PS2's kernel, libraries and hardware, written from scratch (no Sony or CRI code); with the game's code they make `mh2-core` (32-bit x86) |
| `pc/host`, `pc/ipc` | `mh2`: window, input, sound device, display, and its channel to `mh2-core` |
| `pc/gs`, `pc/movie` | `mh2gs` (the graphics, on paraLLEl-GS) and `mh2movie` (the movies, on FFmpeg) |
| `tools/` | setup, the split, the generators and the build; `tools/port/make_archive.py` makes the game archive |

## Licences

OpenMH2's own work in this repository is dedicated to the public domain under **CC0 1.0** (`LICENSE`): the
platform layer, the host and its launcher, the graphics and movie bridges, the tools, the build, the documentation,
and what the project added to the decompiled code (its names, types and comments).

**Monster Hunter 2 is not covered.** The game's code, which the decompiled C reproduces, and its data and text
are Capcom's. The project claims no rights in them, and CC0 does not apply to them, to anyone's trademarks, or
to anything built or extracted from a player's copy of the game.

Files that carry their own notices keep them (newlib, fdlibm and zlib in `pc/core/crt`, `src/lobby` and
`pc/core/third_party/zlib`, and PCSX2's two math routines in `pc/core/third_party/pcsx2_ps2float`). The build
links or loads:

- **SDL3** (zlib licence), built from the pinned source.
- **paraLLEl-GS**, Granite and volk (LGPL-3.0-or-later; MIT), built into `mh2gs`, which `mh2` loads at run time, so
  that it can be replaced. Its source is the pinned commits in `tools/versions.txt` plus the patches in
  `pc/gs/patches`.
- **stb_image** 2.30 and **stb_image_write** 1.16 (public domain or MIT, at your choice), unmodified, in
  `pc/gs/third_party/stb`; the graphics code uses them for the textures it dumps and loads.
- **stb_truetype** 1.26 (public domain or MIT, at your choice), unmodified, in `pc/host/third_party/stb`; the launcher
  draws its text with it.
- **Noto Sans** Regular, release NotoSans-v2.015 (SIL Open Font License 1.1), unmodified, in `pc/host/fonts` with its
  licence text; the launcher embeds it in `mh2`. A path with letters it lacks is drawn with the system's font.
- **zlib** 1.3.1 (zlib licence), unmodified, in `pc/core/third_party/zlib`; `mh2-core` uses it to compress save
  states.
- **PCSX2's PS2 math routines** (GPL-3.0-or-later): the multiply and the add of PCSX2's pull request 12001,
  adapted to C, in `pc/core/third_party/pcsx2_ps2float` with their licence text (`COPYING`). `mh2-core` compiles
  them in, so a `mh2-core` built from this source is GPL-3.0-or-later as a whole. You build it on your own PC, and
  it is never distributed.
- **FFmpeg** 8.1.3 (LGPL-2.1-or-later, the project's own build, shared libraries only), which `mh2movie` loads at run
  time. The source is ffmpeg.org's release tarball, unchanged; `tools/pc/build_ffmpeg.sh` builds it, with only what
  the game's movies use (the MPEG program stream demuxer and the MPEG-2 video and ADX decoders) and no external
  library. `tools/setup.sh` and `tools/setup.ps1` install the libraries prebuilt (`mh2-ffmpeg-8.1.3-linux-x64.tar.xz`,
  `mh2-ffmpeg-8.1.3-windows-x64.zip`: release files of this repository, tag `ffmpeg-8.1.3`, checked against the SHA-256
  in `tools/versions.txt`, like every download), so they need no `make` and no WSL. With `--build-ffmpeg` (Linux) or
  `-BuildFFmpeg` (Windows), or when the download fails or does not match, they check the tarball against its SHA-256
  (and take it from the same release files if ffmpeg.org's download is gone) and run the script. On Linux that needs
  `make`. On Windows it runs in WSL (with make, curl and xz-utils) and cross-builds with llvm-mingw. The libraries
  are the same either way. The release files also carry the tarball, the configure line and the script.

`THIRD_PARTY.txt` lists the components and their licences. Monster Hunter is a trademark of Capcom. This project
is not affiliated with or endorsed by Capcom, and it distributes none of Capcom's data.

## How this was made

The decompilation and this port were written with the help of Claude (Anthropic's AI model), working under the
direction of the repository's owner, who is responsible for what is committed.
