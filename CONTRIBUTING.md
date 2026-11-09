# Contributing to OpenMH2

OpenMH2 is a native PC port of Monster Hunter 2 (Dos). Thank you for wanting to help. This page tells you what the
project takes, where things are, how to change the game's C code (for a fix, a new option or a mod), what the build
checks, and what you may and may not put in a contribution. The README says what the project is and how to build it.
`docs/guide.md` is for people who play.

## What the project takes

- **The platform layer** (`pc/core`): OpenMH2's own versions of what the PS2 provided, written from scratch against
  the interfaces the game calls (no Sony or CRI code).
- **The host** (`pc/host`, `pc/ipc`, `pc/gs`, `pc/movie`): the window, the input, the sound device, the graphics,
  the movies, the launcher and the settings.
- **The build and its tools** (`tools/`, `pc/CMakeLists.txt`, the toolchain files), and the documents.
- **Changes to the game's C** (`src/`, `include/`) in the forms below: a PC form, a fix for something the PC
  version gets wrong, an opt-in option, or a mod of your own.

The matching decompilation of the PS2 executables is a separate public project, mh2-decomp, which stays PS2-only. It
is the C that, built with Capcom's compiler (Metrowerks CodeWarrior for the PS2, MWCCPS2), gives the original bytes.
Work on matching functions belongs there.

## The tree

| Path | What |
|---|---|
| `src/`, `include/` | the game's C, with the PC forms under `#if PLATFORM_PC`. The PC-only headers (`pal.h`, `pal_core.h`, `pal_movie.h`, `platform.h`) are in `include/` |
| `config/` | what the split of the game and the build read about it; `config/pc/layout/` is the layout manifest (names, offsets and sizes: no game bytes) |
| `pc/core` | OpenMH2's own versions, written from scratch, of the PS2's kernel (threads, semaphores, interrupts), its C library, its libraries' behaviour (`api/`, `io/` for controllers, memory cards and the disc, `gfx/` for the graphics chip's commands, `snd/` for the sound chip), and the C runtime with the PS2's arithmetic (`crt/`). With the game's code they make `mh2-core`, 32-bit x86 |
| `pc/host`, `pc/ipc` | `mh2`, 64-bit: window, input, sound, display, the launcher and settings, and the channel to the game |
| `pc/gs`, `pc/movie` | `mh2gs`, the graphics on paraLLEl-GS, and `mh2movie`, the movies on FFmpeg: both loaded by `mh2` at run time |
| `pc/tests`, `pc/host/tests`, `pc/core/ipc/tests` | the build's tests, run by `build.py --check` (ctest) |
| `pc/game_flags.txt`, `pc/known_broken.txt` | the compiler flags the game's files are built with; the files that do not build yet, by name |
| `tools/pc` | the product's build (`build.py`), its checks (`census.py`, `layout.py`, `bpcheck.py`) and the wrappers the compiler runs through (`stackalign.py`, `literals.py`, `ps2fp/`) |
| `tools/port/make_archive.py` | makes the game archive from your disc |
| `tools/setup.sh`, `tools/setup.ps1`, `tools/versions.txt` | the pinned tools, with the version and SHA-256 of each download |

There is one root. The C is in this repository, not in a submodule, and every tool reads `src/`, `include/` and
`config/` from it.

## Setting up

Follow the README: run `tools/setup.sh` (Linux) or `tools\setup.ps1` (Windows, no WSL), run `source tools/env.sh` (or
`. .\tools\env.ps1`) in every new shell, set `MH2_ORIG_DIR` to your copy of the game, run the split, then run
`python tools/pc/build.py --target core` and `--target host`. Both builds use the clang version that
`tools/versions.txt` pins, and the build stops if it finds another one.

There are two programs and two builds. `mh2-core` is 32-bit x86 so that the game keeps the PS2's data layouts (a
pointer is 4 bytes, a `long` is 4, and so on). `mh2` is 64-bit. Game code is compiled with clang's `regcall` calling
convention (`pc/game_flags.txt`), because the default 32-bit convention returns floats through the x87 stack and
quiets signalling NaNs, which the PS2 does not. `pc/CMakeLists.txt` and the toolchain files set this up. If you add a
file that includes system headers, look at how the `host_*.c` files are kept apart from the game-facing ones.

## The rules

1. **The game behaves as it did on the PS2.** A change to a game function must not change what the C means on the
   PS2. The port aims at the same state, frame by frame, for the same inputs.
2. **A PC form lives under `PLATFORM_PC`.** The decompiled C stays as it is for the PS2. What the PC needs
   differently goes in `#if PLATFORM_PC` blocks (or in the platform layer), not in a restructured function.
3. **The game reaches the platform through the interfaces it already calls** (the library functions and the
   hardware macros), or through a hook declared in `include/pal.h` (the calls the PC forms make into the platform
   layer, under `PLATFORM_PC`). It never calls a host API. The platform layer touches the game's memory only
   through what a call hands it.
4. **New options are opt-in.** Each one is off by default, is off when you run the checks, and is documented in
   `docs/guide.md`. The default image and behaviour are the PS2's.
5. **Nothing the game can observe comes from the host's clock or randomness.** Time, input, files and thread timing
   enter through the platform layer.
6. **No warnings**, on Linux and on Windows. The platform layer, the C runtime and the host are built with `-Wall
   -Wextra -Werror`. The game's own files are built with `-w`, since the decompiled C has warnings of its own:
   `tools/pc/census.py` counts them against `config/pc/census_baseline.txt`, and a change must not add one.
7. **The PS2's arithmetic.** Floating-point and integer helpers get the PS2 processor's results from the compiler
   pass in `tools/pc/ps2fp` and the runtime in `pc/core/crt`. Do not edit an expression by hand to make the numbers
   come out. If a function's arithmetic seems wrong, open an issue with the inputs and the result you expected.

## Changing the game's C

You change the game by editing `src/` and `include/` in this repository, then building again.

1. Find the function. Its name and address are in `config/symbols/`, and its C is in `src/<module>/` (the main
   program or one of the overlays the game loads).
2. Edit it. For a fix or an option that is not the PS2's behaviour, put the new code under `#if PLATFORM_PC` and,
   if it is a new option, behind a setting. `pc/host/settings.c` is the table that the launcher, `mh2.ini` and
   `--help` all read, and the host tells the game through the channel in `pc/ipc`. If the change is yours alone, a
   mod, keep it as a patch on a branch of your own. Keep it small, and keep `PLATFORM_PC` around it so that the
   unchanged game is one setting away.
3. Data the game loads from your disc is read into the C structures as they are laid out on the PS2. Do not move a
   member of a type in `include/`, change its size, or add a member in the middle of one that is loaded: the file
   would be read wrongly. A new member at the end of a type the game never loads is safe.
4. Build the game: `python tools/pc/build.py --target core`. When you change a data type or a table, make the
   generated sources again with `python tools/content_gen.py`. Then run the game from the host's build folder.
5. Run the checks (next section).

A mod that changes the game's behaviour is a different game from the PS2's. That is fine for a mod, but say so in your
change, and keep it out of the default settings.

## Content

The repository holds none of the game's data values and none of its text. The build writes them from your copy.

- **Data:** an object keeps its declaration, and its initializer is an include of a file that `tools/content_gen.py`
  writes from your copy, `build/gen/MODULE/ADDR.inc` (ADDR is the object's address; `build/gen` is never committed):

      s16 gSelHeaderLayout[16][2] = {
      #include "select/006CAD00.inc"
      };

  The generator reads the type from clang's parse of the file and the bytes from your copy. Tables that hold only
  references (functions, data, zeros, offsets from names) keep their C: they are structure, not content.
- **Text in code:** `TXT(0xADDR)` (`include/common.h`) stands for a literal of the game's text, with a short English
  comment that says what it is, never the text itself. Short functional ASCII strings (formats, file and device names,
  assert messages) stay literals.
- **Comments** say what a text is, in English. They never quote the game's text.
- **The check:** `python tools/content_scan.py` fails on non-ASCII text in `src/`, `include/`, `config/`, `tools/`,
  `CONTRIBUTING.md`, `README.md` and `docs/guide.md`, and on data initializers with values in `src/`.
  `config/content_exceptions.txt` lists the reviewed exceptions, each with its reason: things that are not the game's
  (zlib's tables, the PNG format's constants).

## Described assets

A data file the game loads (`lbguild.bin`: 328 guild quests and their texts) is described, not committed. What is
committed is the types (`include/lbguild.h`) and the layout, `config/assets/MODULE/FILE.txt`: the header, the range
the C owns, the roots (an object at a known address) and what each pointer type points to. It holds no values. The
build writes the C from your copy with `tools/asset_c.py` (`build/assets/lbguild/lbguild.c`, the third command of the
README's build), and stops at anything the description does not explain: an object that no pointer reaches, or a type
without a `target` line.

## Functions with switch tables or strings

The compiler puts a `switch`'s jump table and every string literal in the C file's `.rodata`, and initialized
variables in its `.data`. A C file that uses such data owns its range of it: the data is defined in the file, in the
order the functions use it, which is what lets the decompilation match the PS2's bytes. `tools/gen_data.py` writes
the C definitions for a range of a module's `.data` or `.bss`, from your copy, when a file takes the range over.
This is decompilation work, which belongs to mh2-decomp. Here you only meet it as the definitions inside a game
file, which you leave as they are.

## The checks

`python tools/pc/build.py --check` (add `--target host` for the host) builds and then runs the unit tests (ctest). For
the game it also checks:

- `pc/known_broken.txt` names no file that compiles by now (a file that builds must leave the list);
- `gen_stubs.py --self-test` passes;
- `bpcheck.py`: no function addresses its frame through a base pointer, because `regcall` passes arguments in the
  register that clang would use for it.

What the tests say when they fail:

- A **compile error** in game code, or a **warning** in the platform layer or the host: your change breaks the PS2's
  meaning, or a PC form is missing. Fix the code; do not turn the warning off. (A warning in game code does not
  stop the build: `census.py` reports it, below.)
- **The calling-convention tests** (`cc_game_nan`, `cc_control_x87_quiets_snan`, `cc_game_no_x87`, built from
  `mh2_cc_test`): a signalling NaN changed through a call, or something bypassed `regcall` (a function declared
  without a prototype, a system header in a game file).
- **`layout_probe`**: the compiler laid out a structure or a bit-field differently from the PS2. On Windows the flag
  `-mno-ms-bitfields` matters.
- **The host's tests**, whose names start with `host_`: `host_settings` and `host_settings_writer` (the table, and the
  lines written back to `mh2.ini`), `host_help` (`--help`), `host_launcher_wanted` and `host_launcher_guard` (when the
  launcher opens), `host_card_lock` (one card, one program), and `host_packs` (the texture packs' list). If one fails,
  the setting you changed no longer behaves as the test says. Read the test before you change the test, and explain
  a changed expected value in your pull request. The tests of the launcher's pages and of the archive maker run in
  the maintainers' own build, not here.

`python tools/pc/census.py` compiles every game file whole with clang and compares the warnings and errors with
`config/pc/census_baseline.txt`. It needs the generated sources (`build/gen`, from `content_gen.py`) and runs here. A
change to `src/` or `include/` must leave it equal to the baseline, or say in the pull request why not.
`tools/pc/layout.py` compares every struct's member offsets with the PS2's, from `config/pc/layout_baseline.txt`, by
compiling them with Capcom's compiler, which this repository does not carry. The maintainers run it on a pull request
that touches `src/` or `include/`.

Tell us in the pull request what you ran and on which system. A change that is only for one system says so.

## The clean-room rule for the platform layer

The PS2's libraries (Sony's, CRI's, the network libraries) are not in this repository, and OpenMH2 does not rewrite
them from their code. A version of one of their functions in `pc/core` is written from:

- public documentation;
- the game's own calls (how Capcom's code uses the function, what it passes, and what it does with the result);
- observation of a running copy of the game, treated as a black box;
- the behaviour written down in the comments of `pc/core`, which say what each library function does at its
  interface.

Never write one from a library's disassembly, never with tables or constants copied out of its binary, and never
from leaked SDK material. Never read or copy GPL or AGPL code (PCSX2, libgcc and similar) to write the port's: you may
cite it for documented behaviour, never as code. When you add a library function, say in the pull request what you
wrote it from.

You may use the sources that `THIRD_PARTY.txt` and the README's Licences list, with their notices kept: newlib and
fdlibm, SDL3, paraLLEl-GS, FFmpeg (as shared libraries), stb, zlib, Noto Sans, and PCSX2's two PS2 math routines.
Those two routines are the one exception to the rule above: they are PCSX2's own code (GPL-3.0-or-later), kept apart
in `pc/core/third_party/pcsx2_ps2float` with their licence. Change them only there, and never copy them or any other
PCSX2 code into the port's own files.

## What may never be committed

**Nothing of the game's.** That means no ISO or extracted file, no `mh2-disc.arc`, no `mh2-core` or any program made
from your copy (it contains the game's code and data), no save state, memory card, texture dump or screenshot of the
game, no sound or video file, and no hash list of the game's data beyond what the repository already has. Everything
the port produces from the game stays in `build/`, in your user data folder, or in your own scratch space. The same
goes for a texture pack made from a dump: it is your work, not something this project carries.

`build/`, `asm/` and `assets/` are ignored by git. `python tools/content_scan.py` checks the tree for anything that
looks like the game's data: run it before you send a change.

Do not download game data, a disc image or a BIOS for any reason related to this project. Tools and libraries are
fine: record each one's source, version and SHA-256 in `tools/versions.txt`.

## Your contribution and its licence

The project's own work is dedicated to the public domain under **CC0 1.0** (`LICENSE`). By sending a contribution,
you agree that it is yours to give, and that you give it under CC0 1.0. Do not send code under another licence, and
do not send a change that takes code from a project whose licence does not allow it. A file that must keep its own
notice (a third-party library) goes in with the notice, and into `THIRD_PARTY.txt`.

The game's code, data and text are Capcom's. The project claims no rights in them, and a contribution does not add
any.

## Pull requests

- Send one change per pull request, with a message that says what it does and why.
- Say what you ran (`build.py --check` for the core and the host, on which systems) and what you did not run.
- A change to a setting goes together with: the table in `pc/host/settings.c` (the launcher, `mh2.ini` and `--help`
  all read it), the launcher's page if it has one, `docs/guide.md`, and the host's tests that count the table's rows
  (`host_settings`, `host_help`). The maintainers' build also checks that the pages, rows and `mh2.ini` keys the
  guide names exist in the launcher and in the table, so when you rename something, rename it in the guide too.
- Use the names the launcher shows, in text that players read (the guide, the launcher's strings, messages). For
  example, say "frames per second", "memory card" and "graphics card".
- Keep generated files, build output and anything from your copy of the game out of the commit (run `git status`
  before you add, and `git diff --stat` after).
- The README says how the project was made, including the help of an AI model. Say the same if you used one: the
  person who commits is responsible for what they commit.
