OpenMH2: Monster Hunter 2 (Dos) on your PC
==========================================

OpenMH2 is a fully native port of the PlayStation 2 game, built from the ground up: the game's own source code is compiled
for your PC, and nothing emulates a PS2. This folder holds the program only, and none of the game: you need your own copy
of Monster Hunter 2 (Dos), the Japanese PS2 game (disc SLPM-66280), as a disc image (.iso) or as a folder with its files.
Nothing is downloaded, and nothing is sent anywhere.

What you need
  - Windows 10 or 11, or 64-bit Linux (glibc 2.35 or newer, libstdc++, zlib and zstd, and support for 32-bit programs,
    which every ordinary distribution has).
  - A graphics card with Vulkan 1.2 (or newer) drivers.
  - About 3 GB of free disk space for the game's files, and 2.5 GB more while the first setup runs.
  - No Git, no Python, no compiler: this folder has what it needs (the folder "kit").

First steps
  1. Unpack this folder anywhere you can write to (your Documents folder, not Program Files), and run mh2 (mh2.exe on
     Windows).
  2. The launcher opens on the first-time setup. Step 1: choose your disc image or folder.
  3. Step 2: the launcher makes the game archive (your game disc in one file, 2.7 GB, about a minute).
  4. Step 3: it builds the game from the game archive (5 to 15 minutes, depending on your PC) and shows its progress.
     Cancel stops it and leaves nothing behind. If it fails, the launcher says why and offers to try again.
  5. Step 4: start a new memory card, or bring your memory cards from an earlier version.
  Then press Play. The setup happens once, and you don't need your disc afterwards.

Where your files go
  The game archive, the game, your memory cards, save states, photos and logs are in the user data folder:
  %APPDATA%\mh2-decomp\mh2 on Windows, ~/.local/share/mh2-decomp/mh2 on Linux. The launcher's "Folders and logs" page opens
  it. OpenMH2 never writes to this folder (the one you unpacked). To remove everything, delete it and the user data folder.

If something goes wrong
  The launcher's pages say what happened. The build keeps its log in the user data folder (build.log), and the game's log
  is mh2.log there. On Windows, a virus scanner that blocks the build's tools is the usual reason a build stops at once:
  allow this folder and try again.

Updates
  A new version of this folder brings a new kit. The launcher notices that the game was built by an older one and builds it
  again from the game archive, which is kept: it doesn't ask for your disc.

Making a translation
  The kit's Python runs the translation pack tools (the folder kit/src/tools/pc, textpack*.py): kit/python/bin/python3
  kit/src/tools/pc/textpack.py on Linux, kit\python\python.exe kit\src\tools\pc\textpack.py on Windows. They read the game's
  text from your own copy: "--archive" names the game archive in the user data folder (mh2-disc.arc). When the game is
  built, the launcher also lists the game's own strings in core/strings.tsv in the user data folder (a work file, one string
  a line, for the translator to fill in). docs/translation-packs.md in the project's repository is the author's guide: the
  tools' commands, the pack's files, every message the checker prints, and the limits.

Licences
  The project's own work is public domain (CC0 1.0). The folder "licenses" holds the notices and the source offers of the
  libraries it carries (licenses/SOURCE_OFFER.txt; the graphics and the movie player are LGPL libraries, and the kit carries
  the compiler, Python, CMake and Ninja and, on Linux, glibc, each under its own licence; licenses/COMPONENTS.txt lists
  them). Monster Hunter 2 and its data belong to Capcom.
