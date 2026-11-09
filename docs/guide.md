# OpenMH2: a guide for players

OpenMH2 lets you play **Monster Hunter 2 (Dos)** on a PC, from your own copy of the game. This guide takes you from
nothing to playing: what you need, how to install it, the first start, the launcher, the controls, your saves, the
settings and extras, and what to do when something goes wrong. The README has the short version. To change the
code, see `CONTRIBUTING.md`.

## 1. What it is, and what you need

OpenMH2 is not an emulator. It runs the game's own code, the C of the decompilation in `src/`, natively on your PC.
It has its own versions, written from scratch, of what the PlayStation 2 did for the game: the system software, the
libraries, the graphics chip, the sound chip and the memory cards. None of Sony's or CRI's code is in it. The
program you run, `mh2`, takes care of the window, the keyboard and gamepad, the sound and the movies. It starts the
game itself, `mh2-core`, next to it. With the extras below turned off, the game plays as it did on the PS2: the same
inputs give the same game, frame by frame. That includes the game's math: a PC does some of it (positions,
physics, the camera, damage) slightly differently from the PS2, so OpenMH2 does it the way the PS2 does, always.
Before, this fix had to be applied externally; in OpenMH2 it is a native fix.

**It contains no part of the game's data.** The game's files come from your copy of the disc. Everything made from
them (the game archive, `mh2-core`, your textures and save states) stays on your PC. Do not share those.

You need:

- **A copy of the game:** Monster Hunter 2 (Dos), PS2, NTSC-J, version 1.04 (boot file `SLPM_662.80`), as a disc
  image (`.iso`) or as the disc's files in a folder (`SYSTEM.CNF` at its top). Other versions and regions do not
  work. OpenMH2 tells you which file does not match.
- **A PC:** Windows 11 (64-bit), or Linux (64-bit; Ubuntu 24.04 is the tested system); a graphics card and driver
  with Vulkan 1.2; 8 GB of memory; and about 5 GB of free disk space (about 10 GB to build from source). The game
  archive alone is 2.7 GB.
- **Git**, only if you build from source. The setup script installs Python and the other tools for you, at
  pinned versions.

## 2. Installing

### From a release (most players)

1. Download the release for your system, Windows or Linux, and unpack it anywhere you like. It holds `mh2`, the
   graphics and movie libraries, and a `kit` folder. The kit has the tools that build the game on your PC.
2. Run `mh2` (`mh2.exe` on Windows). The launcher opens and walks you through the first start (section 3).

You do not need to install anything else. To move OpenMH2 somewhere else, move the whole folder. Never delete the
`kit` folder: the launcher needs it again after an update.

### Building from source

Building from source is for modders and for anyone who wants to check the code. You build OpenMH2 once. Plan for
about 15 minutes after the downloads (the setup downloads about 2 GB of tools, mostly the compiler).

On Linux:

```sh
tools/setup.sh                    # apt packages (asks for sudo; --no-apt skips them), then the pinned tools into tools/deps
source tools/env.sh               # in every new shell
export MH2_ORIG_DIR=/path/to/your/game/files     # the folder with the .iso, or the one with SYSTEM.CNF

python tools/configure.py --split-only
python tools/content_gen.py --jobs 8
python tools/asset_c.py config/assets/lbguild/lbguild.txt build/assets/lbguild/lbguild.c
python tools/pc/build.py --target core      # mh2-core, 32-bit
python tools/pc/build.py --target host      # mh2, mh2gs and mh2movie in build/pc/linux-x86_64
```

`tools/setup.sh` installs its packages with `apt`. On another Linux distribution it prints the list of packages it
needs (it also does with `--no-apt`). Install their equivalents yourself, then run `tools/setup.sh --no-apt`.
FFmpeg's libraries (for the movies) are installed prebuilt and checked against their SHA-256, like every download.
`tools/setup.sh --build-ffmpeg` builds them from FFmpeg's source instead (it needs `make`), and so does the setup
automatically if the download fails.

On Windows (PowerShell, no WSL needed):

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup.ps1
. .\tools\env.ps1                                              # in every new shell
$env:MH2_ORIG_DIR = 'C:\path\to\your\game\files'

python tools\configure.py --split-only
python tools\content_gen.py --jobs 8
python tools\asset_c.py config\assets\lbguild\lbguild.txt build\assets\lbguild\lbguild.c
python tools\pc\build.py --target core
python tools\pc\build.py --target host
```

`setup.ps1` installs FFmpeg's libraries prebuilt and checks them against their SHA-256. `setup.ps1 -BuildFFmpeg`
builds them from FFmpeg's source instead. That is the one step that needs WSL (with make, curl and xz-utils), and
the setup does it automatically if the download fails.

`MH2_ORIG_DIR` is only read from, never written to. The first three commands read your copy and write `build/`. They
take a few minutes. The result is in `build/pc/<system>-x86_64/` (the program `mh2`, the graphics library `mh2gs`,
the movie library `mh2movie` and the libraries they need: SDL3 and FFmpeg's) and in
`build/pc/<system>-i686/core/` (the game, `mh2-core`). `mh2` finds the game next to itself or in that tree. It looks
for the game archive (below) in this order: the `archive` setting, `mh2-disc.arc` next to `mh2`, the user data folder
(section 7), and `build/port` of the tree.

If you move the program, move these together: `mh2`, `mh2-core`, `mh2gs`, `mh2movie`, SDL3's library and FFmpeg's
five libraries (on Windows their `.dll` files; on Linux `mh2` finds the libraries next to itself). `mh2-core` is made
from your copy and holds the game's code and data: **never give it to anyone.**

**The game archive.** The game reads its disc from one file, `mh2-disc.arc` (2.7 GB). A small file next to it,
`mh2-movies.txt`, holds the timing of the movies. The launcher makes both (section 3). If you prefer a command:

```sh
python tools/port/make_archive.py                       # build/port/mh2-disc.arc
python tools/pc/movie_table.py --lib build/pc/linux-x86_64
```

(On Windows: `build\pc\windows-x86_64`.) Either way you get the same two files, byte for byte.

## 3. The first start

Run `mh2` (`mh2.exe` on Windows; from a source build, `build/pc/linux-x86_64/mh2` or `mh2.exe` in
`build\pc\windows-x86_64`). A window opens: the **launcher**. The first time, when there is no `mh2.ini` and no game
archive yet, it opens on a short setup. You can leave any step (**Set up later**) and come back to it at the next
start.

1. **Your game files.** The launcher looks next to `mh2`, in the folder above it and in the folder `MH2_ORIG_DIR`
   names, and offers what it finds. Or choose a disc image or a folder with the file dialog.
2. **Making the game archive.** The launcher shows the free space first (it needs 2.7 GB and does not start without
   it). Then it checks each of the disc's files against the known NTSC-J disc (size and SHA-1), names any file that
   does not match, and writes the archive. **Cancel** stops the work and removes the half-written file. Afterwards it
   measures the timing of the movies once and writes `mh2-movies.txt` next to the archive. The archive is a copy:
   from now on the game runs without your original files, and you can put them away.
3. **Building the game** (releases only). The launcher builds the game on your PC from the archive, with the tools in
   the `kit` folder. This takes 5 to 15 minutes and needs about 2.5 GB of free disk space while it runs. You can leave
   the window open in the background, and **Cancel** stops it and cleans up. After an update, the launcher builds
   the game again from the archive. It does not ask for your disc again.
4. **Your save.** A new memory card, or the cards of an earlier version (section 5). This step comes only when
   there is no card yet.

If there is an `mh2.ini` but no game archive (it was moved or deleted), the launcher opens on Home with a banner,
and the Game archive page makes it again. The archive and the movie table go where you set `archive = FILE`, or else
into the user data folder (section 7).

## 4. The launcher

When you start `mh2` by itself, it opens the launcher before the game. The arrow keys or the gamepad's D-pad choose a
row and change its value. **Enter** (or `L`, or the gamepad's Circle) opens or confirms. **Esc** (or `K`, or the
gamepad's Cross) goes back: the first press moves to the last row (Back, or Quit on Home), and the second takes it.
The mouse works too (hover, click, the wheel to change a value, right click as Esc). **F11** or **Alt+Enter**
switches between a window and fullscreen. A page with more rows than fit scrolls with the selection: Page Up and Page
Down move a page, and the wheel scrolls without changing a value. A line at the bottom explains the row you are on.

| Page | What it has |
|---|---|
| **Home** | **Play**; the pages below, each with a summary; **Skip this screen next time** (turn it on to start the game at once); **Quit**. A banner replaces the subtitle when the game archive is missing, is not valid for this version, or has no movie table. In a release, it also has **Build the game**, which shows whether the game is built |
| **Game archive** | the archive's status, file, size and free space; **Make the archive from a disc image...** or **from a folder...** (a file or folder dialog, then a progress screen with Cancel); choose another archive; **Make the movie table** (shown only when the table is missing) |
| **Memory cards** | the card in each slot, the cards in the folder (marked in use when another copy of OpenMH2 has one open), **New card...**, **Import cards from an earlier version...**, open the memory cards folder |
| **Graphics** | anti-aliasing, high resolution, fullscreen, V-Sync, mute, dump textures, use texture packs; **Defaults** shows while something differs |
| **Game** | widescreen, Offline Dundorma, fast loading, keyboard typing and its typing key, the save states folder, and three pages: the Japanese keyboard's keys, **Cameras** (the free camera's keys and the camera on the right stick), and the save state, pause and speed keys |
| **Content** | texture packs: whether to use them, the memory they may use, and the list of packs in order, each with its author (the `author = NAME` line of the pack's `pack.txt`) and file count. A translation is a different kind of pack, made with the tools that [the translation pack guide](translation-packs.md) describes |
| **Folders and logs** | open the user data folder and the logs folder, show the last log, the photos folder (a setting), and the settings file's path. It has no summary |

Your changes are written to `mh2.ini` when you press **Play**, or when you answer **Yes** to "Save the changes?" as
you quit. Only the lines that changed are touched (a changed value in place, a new setting in a `[launcher]`
section at the end), so your comments and your own lines stay. A setting given on the command line (`--ssaa 8`) is
shown with "(command line)": you can change it for this start, and the file keeps what it had.

To skip the launcher, turn on **Skip this screen next time** on the Home page (`launcher = off`). To see it again,
start `mh2` with `--launcher` (on Windows you can also hold **Shift** while it starts). It opens anyway, whatever
this setting says, when the game cannot start without it: the game archive is missing, is not valid for this version, or has no
movie table. Your `launcher = off` stays in `mh2.ini`, so the game starts right away again once the archive is good. `--no-launcher`
never shows it.

## 5. Playing

**The window.** The game's image keeps the PS2's shape (4:3) unless you turn widescreen on. **F11** or
**Alt+Enter** switches fullscreen. Closing the window quits: save in the game first, as on the PS2.

**The controls.** A gamepad works as it comes. The first gamepad is used, by button position: the bottom button is
Cross, right is Circle, left is Square, top is Triangle, and the triggers are L2 and R2. The keyboard follows the
usual PS2 emulator layout:

| Controller | Keyboard |
|---|---|
| D-pad | arrow keys |
| Cross, Circle, Square, Triangle | `K`, `L`, `J`, `I` |
| L1, R1 | `Q`, `E` |
| L2, R2 | `1`, `3` |
| L3, R3 | `2`, `4` |
| Start, Select | `Enter`, `Backspace` |
| left stick | `W A S D` |
| right stick | `T F G H` |

To change a binding, add a line to `mh2.ini`: `key.cross = Space` or `pad.l2 = lefttrigger` (`none` removes the
binding). Your own `key.` and `pad.` lines also set which keys mean Circle and Cross in the launcher.

**Your saves: memory cards.** The game saves to PCSX2-format memory cards (8 MB, `Mcd001.ps2`). OpenMH2 keeps its
cards in the `cards` folder of the user data folder, and writes them safely: if something stops in the middle of a
save, the card is not damaged.

- A new card is made on the first start, or from the launcher's Memory cards page (**New card...**).
- To bring in a card from PCSX2 or another emulator, run `mh2 --import-card FILE`. The card is copied into the
  cards folder under its own name. The original is never changed, and an existing card of that name is never
  overwritten (it refuses, and says so in the log). It does not choose the card, and `mh2` quits once the card is
  copied. Set `card1 = NAME`, or choose the card on the Memory cards page, then start the game. The page's **Import
  cards from an earlier version...** does the same for the cards of an earlier version of OpenMH2, and never
  overwrites a card.
- `card1` and `card2` in `mh2.ini` (or the same rows on the page) say which card is in each slot. `cards = FOLDER`
  names another folder.
- Two copies of OpenMH2 cannot use one card at the same time: the second one says so and stops.

**Save states.** A save state is a snapshot of the whole game, in a hunt, in a menu or in the middle of a fight, that
you can come back to. It is not the game's own save. `Home` saves to the selected slot, `End` loads it, and `Page
Down` and `Page Up` choose one of the ten slots. A line on the screen says what happened. A load does not roll back
your memory cards. You cannot save while a movie plays or while the keyboard is typing. A save state is refused when
it was made by another version or with other settings (another widescreen setting, Offline Dundorma, the keyboard
connected the other way, another graphics library version, `ssaa`, `hires`, texture packs). The line on the screen
gives the reason. Save states are kept in the `states` folder of the user data folder (`states = FOLDER` changes it).
**A save state holds data from your disc: do not share it.**

## 6. Settings and extras

### mh2.ini

Settings live in `mh2.ini`: lines of `key = value`. A `;` or `#` starts a comment, anywhere in a line. `mh2` reads the
`mh2.ini` next to the program first; it wins over the one in the user data folder. `--config FILE` replaces that
search. `--set KEY=VALUE` gives one setting for a single start, and `mh2 --help` lists every option and every key
(except `pack.NAME`). The launcher edits the same file.

| Key | What |
|---|---|
| `archive = FILE` | the game archive (keep `mh2-movies.txt` next to it) |
| `cards`, `card1`, `card2` | the memory cards (section 5) |
| `launcher = off` | start the game without the launcher |
| `fullscreen`, `vsync = auto/on/off`, `mute` | the window and the sound (vsync auto: on in fullscreen only) |
| `ssaa`, `hires`, `widescreen`, `offline_hub` | graphics and game options (below) |
| `texture_dump`, `texture_packs`, `textures`, `texture_cache_mb`, `texture_preload`, `pack.NAME` | textures (below) |
| `text_pack = off` | a translation: the name of a folder in `text_packs` in the user data folder, with the translation's files and text (a save state made with a pack loads only with that pack). [The translation pack guide](translation-packs.md) is for making one: the tools' commands, the pack's files, every message the checker prints, and the limits |
| `usb_keyboard`, `usb_keyboard_toggle`, `usb_key_*` | keyboard typing (below) |
| `states`, `state_key_*` | save states |
| `fast_loading`, `fast_forward_cap`, `pace_key_*` | speed and pause (below) |
| `freecam_*`, `pictures` | the free camera and photo mode (below) |
| `key.NAME`, `pad.NAME` | the controller bindings |

### The extras

**Every extra is off by default.** With none of them on, the image and the behaviour are the PS2's. The free camera
is on a key (`F9`) and does nothing until you press it.

**Anti-aliasing and high resolution.** These change how the game looks, never how it plays. `ssaa = N` smooths
jagged edges by drawing each pixel with N samples (2, 4, 8 or 16; 1 is off). The image keeps its size. `hires = on`
draws the game at twice its original width and height (1280x896), which looks sharper in a large window. It uses at
least 4 samples (`ssaa = 4`). Both ask more of your graphics card: 4 samples suits most cards, and 16 samples or
`hires` want a recent one. If the card cannot make the samples you asked for, OpenMH2 tries again with one sample
and says so in the log. Movies are not affected. The Graphics page has both.

**Widescreen** (`widescreen = on`, the Game page). The game is shown in 16:9, and the camera sees 33% more to the
sides. The HUD, menus and text keep their shape (the 2D image is drawn 3/4 as wide about its centre, and the display
stretches it back). Whatever fills the screen's whole width, such as fades and backgrounds, stays full width. Movies
stay 4:3 with black bars. Unlike the graphics options, widescreen changes what the game computes (the camera's view
of the world, and where a fixed camera stops following you), so a save state made with another widescreen setting
is refused.

**Offline Dundorma** (`offline_hub = on`, or `--offline-hub`; the Game page; off by default). Visit Dundorma, the
online town, without an internet connection. You can take its online quests and earn Hunter Rank, awards and
rewards, saved to your memory card as usual. There is no online play, and nothing is sent anywhere.

- **Getting there.** The gate at the end of the village leads to Dundorma instead of the online lobby. There is no
  network, no room and no party. Your hunter must have been saved in the village: if you saved while away from it,
  the gate stays shut, so go back to the village first. The town's exit leads back to the village.
- **In the town.** You will find the tavern, the workshop, the arena, the elders' hall and your own house. The elders' hall opens
  from Hunter Rank 31, once you have cleared the second gate quest. The doors are the game's own: a prompt, then
  the area.
- **Quests.** The tavern's quest counter lists the online quests of the open maps for your Hunter Rank (from Hunter
  Rank 1; the snowy mountains from rank 5, the volcano from 11, the towers from 22). Choose a quest, pay the fee, and
  get the stamp. The official desks give the events and the gate quests (the hunting trials at Hunter Rank 11, 17,
  31, 41 and 51), as the online game does. Their rows show only what your rank and your cleared quests allow.
- **Setting out.** Use the gate (the tavern's stair) with a quest taken. If the season or time of day has changed,
  the game refuses with its own message, and the quest stays taken: cancel it at the counter and take a new one.
  Without a quest taken, the gate does not open.
- **The quest.** It is played alone, with the offline rules. When it ends, the results work as in the online game: Hunter
  Rank points (each faint takes 20 points off), awards and rewards. You come back to the area of the counter you used.
- **Saving.** After you return, the game asks whether to save and writes your memory card. The card keeps your Hunter
  Rank and points, cleared quests, flags, awards, money and items. If you close the game in the town, it starts in
  the village at your last save.
- **Good to know.** Offline Dundorma changes what the game computes, so a save state made with it loads only
  with it on. The elders' forecast is rolled on your PC instead of by a server, so it can list a forecast quest on a
  visit where the online game would not, and the other way round.

**Textures.** `texture_dump = on` writes every texture the game draws, once, as a PNG named after what it is, into
the `dump` folder of the textures folder (`textures` in the user data folder, or `textures = FOLDER`).
`texture_packs = on` draws a texture from a PNG of the same name found in a pack. A pack is a folder in the `packs`
folder of the textures folder, with files named like the dumped ones, at any folder depth. Open the packs folder from
the Content page.

- A texture's name says what the texture is, not where it was used. For example,
  `T8_256x256_5f3a9c1d2b7e4a60_91c2d3e4f5a6b7c8.png` is a 256x256 texture of 8-bit indices, followed by a hash of
  its texels and a hash of the palette it was drawn with. A palettised texture can also be named without the
  palette part, which then covers every palette of it; the full name wins.
- A replacement can be any whole multiple of the original's size (2x, 3x, etc.) in the same shape: 512x512, 768x768
  or 1024x1024 for a 256x256 texture. Alpha 255 in a PNG stands for the game's full alpha.
- A `.dds` file (BC7, 2D, sizes a multiple of 4, so 768x768 works as well as 1024x1024) is used as it is, uploaded
  compressed, and loads faster. Its alpha uses the game's scale: 128 stands for full alpha.
- A file that cannot be used is named once in the log, and the game's own texture is drawn.
- When two packs have a file for one texture, the pack that comes first wins. The order is the order of the
  `pack.NAME` lines in `mh2.ini` (`on` or `off`). The Content page lists the packs. **Enter** on one opens a small
  menu (Turn on or off, Move up, Move down), and Left and Right turn it on or off. A pack with no line is on and
  follows the others by name.
- `texture_preload` (on by default) loads the packs that are on before the game starts, up to `texture_cache_mb` (16
  to 16384 MB; 1,024 by default, and never more than half of your free memory). What does not fit loads when its
  texture is first drawn, and the original shows until then. With it off, every file loads that way. The same number
  of MB limits what the graphics card keeps. The Content page has both rows.
- Not every texture can be dumped or replaced (the ones the graphics card draws itself, or those with several detail
  levels). The log's summary says how many.
- Do not share your dump: it is the game's own art. A pack you make from it is your own work.

**Keyboard typing** (`usb_keyboard = on`, the Game page). The PS2 game takes text from a USB keyboard where it asks
for a name or a chat line: romaji becomes kana, Space and the arrow keys convert, and Return accepts. This option
connects your keyboard to the game as that USB keyboard, for the whole session. Your keyboard is also the game
controller, so the two share it by mode. It starts as the controller, and the **typing key** (`usb_keyboard_toggle`,
`ScrollLock` by default; on a laptop without it, choose another such as `F10`) switches to typing and back. While you
type, none of its keys presses a controller button. The window's title and a line at the top right say "USB
keyboard: typing". The game was made for a Japanese keyboard, and six of its keys have no place on other layouts.
Stand-ins are set: `usb_key_yen` is `Backslash`, `usb_key_henkan` is `Pause`, `usb_key_muhenkan` is the menu key,
`usb_key_kana` is `Right Alt`, `usb_key_zenkaku` is the grave accent, and `usb_key_ro` has none. The Game page's
Japanese keyboard keys page shows all six.

**Fast loading, fast-forward and pause.** These change how long the game takes, never what it does.

- `fast_loading = on` (the Game page): the game does not wait for the clock while its loading screen is open, so
  going from the village to a hunt takes under 2 seconds instead of about 6. Movies, music, voices and fades keep their
  normal timing.
- Hold the **fast-forward key** (`pace_key_fast`, the grave accent `` ` ``) and the game runs without waiting for the
  clock, up to `fast_forward_cap` times normal speed (2, 3, 4, 6, 8 or `none`; 4 by default). The sound is off while
  you hold it. It does nothing during a pause, while the keyboard is typing, or in a movie.
- **`Space`** (`pace_key_pause`) pauses the whole game and keeps the screen as it is (a line says PAUSED). While
  paused, `Period` (`pace_key_step`, the frame advance key) runs one frame for each press, and repeats while you hold
  it. `Space` resumes. The three keys and the cap are on the Game page's Save state, pause and speed keys page.

**The camera on the right stick.** The game turns its camera with the D-pad: left and right turn it, up and down move
it between four heights, and L1 swings it behind the hunter. With `camera_stick`, a gamepad's stick does the same.
It is off by default. The game already uses the right stick to attack, so the camera has to take the stick from the
attacks. `camera_stick = hold` is for playing: hold the button that `camera_stick_button` names (L3, the left stick's
click) and push the stick, and the camera turns. Let go, and a flick of the stick is an attack again. A stick that is
still pushed when you let go is held back from the game until it comes to rest. `camera_stick = on` makes the stick
the camera all the time, for places where you do not attack. `camera_stick_which` chooses the stick (`right` or
`left`). `camera_stick_invert` reverses an axis (`x`, `y`, `xy`, `none`). `camera_stick_speed` is how fast the camera
turns at full tilt, as a share of the D-pad's turning speed (25, 50, 75 or 100). The mode, the invert and the speed
are on the Cameras page; the other two are in `mh2.ini`. The game and your saves are the same as before. The
camera moves only in hunts and the dojo, because the village's camera does not turn with the D-pad either.

**The free camera and photo mode.** Press `F9` in the village or in a hunt, and the camera is yours. It starts
where the game's camera is, and the game goes on. The keys:

- `W A S D` move, and `Q E` go down and up.
- The mouse looks, and the wheel changes the speed. `Shift` is faster and `Ctrl` is slower.
- `Z X` change the field of view, and `R` goes back onto the game's camera.
- `Tab` gives the controller back to the game (and takes it again).
- `Space` pauses the game while the camera still flies, and `.` runs one frame while paused.
- `P` takes a photo, and `Escape` or `F9` leaves.

With a gamepad, the left stick moves, the right stick looks, L2 and R2 go down and up, and L1 and R1 change the
speed. Pressing both sticks in together (L3 and R3) turns the free camera on and off only if you set
`freecam_pad_toggle = on`. By default it is off, and the game gets your gamepad as it is. With it on, the game never
sees the two sticks pressed together, and a stick press reaches it a moment (60 ms) late, in case the other stick
joins.

By default the game receives a controller with nothing pressed while you control the free camera
(`freecam_game_input = neutral`; `pad` leaves the gamepad to the game). The speed is `freecam_speed` game units per
second (400). Most keys have a setting named `freecam_key_...` (`mh2 --help` lists them; the Game page's Cameras page
shows the main ones and says where the rest are). `Escape`, `Shift`, `Ctrl`, the mouse and the wheel are fixed. A
photo is a PNG written to the `pictures` folder as `MH2-YYYYMMDD-HHMMSS.png`, at the size you play at. Hiding the
HUD with `H` does nothing yet.

The free camera changes what is drawn and, by default, the controller the game sees while you fly. The game goes on as
it would otherwise. (A pause that starts during a disc load can change the game's timing a little afterwards, for
example the quest clock by a fraction of a second.) It works where the world is drawn from one camera, the village and
hunts. In menus the game gets its controller back by itself. What the game builds around its own camera (the haze and
dust of an area, and transparent things more than 1,600 units behind its camera) stays where the game's camera is and
cannot be shown from elsewhere.

## 7. Where things are

**The user data folder** is `%APPDATA%\mh2-decomp\mh2` on Windows (paste it into Explorer's address bar) and
`~/.local/share/mh2-decomp/mh2` on Linux. It holds:

- the memory cards (`cards`), the save states, the textures and the photos (`pictures`);
- the log of the session (`mh2.log`; the last five are kept as `mh2.1.log` and so on);
- the optional `mh2.ini`;
- the game archive (`mh2-disc.arc`, 2.7 GB) and the movie table (`mh2-movies.txt`) that the launcher makes, unless you
  set `archive = FILE`;
- in a release, the built game (`core`);
- on Windows, a crash dump if the program crashes.

The launcher's **Folders and logs** page opens it. The log and the dumps stay on your PC: nothing is sent anywhere.
(The folder keeps the name of the project's first form, so that cards made by earlier versions are found.)

**Disk.** The game archive is 2.7 GB, a save state is 6 to 16 MB a slot, a memory card is 8 MB, and a texture dump is
about 100 KB per texture and stops at 20,000 files per session.

## 8. When something goes wrong

1. **Read the log.** The launcher's Folders and logs page shows the last log, and `mh2.log` is in the user data
   folder. It says what OpenMH2 started with (the settings, the graphics card, the game archive) and why it stopped.
2. **The launcher says the game archive is missing, or not valid for this version.** Use the Game archive page, then
   **Make the archive from a disc image** or **from a folder**. If it names a file as not matching, your copy is
   another version or is damaged: only the NTSC-J v1.04 disc works. If there is not enough free space, it says how much
   it needs and how much is free. If it says the movie table is missing, see item 5.
3. **"The game is not built yet", or the build fails** (releases). Choose **Build the game** on the Home page. If it
   fails, use **Try again** or **Open the logs folder**. The usual causes are too little free disk space (it needs
   about 2.5 GB while it runs) and a virus scanner that blocks the programs in the `kit` folder. Allow the folder and
   try again.
4. **"mh2-core could not be started".** `mh2-core` is not next to `mh2` or in the build tree. In a release, build the
   game from the launcher. From a source build, build the core (section 2) and put `mh2-core` next to `mh2`.
   (`--core PATH` names it elsewhere, but it makes the run an automated one: no launcher, and it stops unless you
   also name a cards folder, as in `--core PATH --set cards=FOLDER`.)
5. **The movies do not play, or the movie table is missing.** `mh2movie` and FFmpeg's libraries must be next to
   `mh2`. Then use **Make the movie table** on the Game archive page.
6. **A black window, or the graphics ask too much.** Lower `ssaa`, turn `hires` off, and update your graphics driver
   (Vulkan 1.2 is needed). If the card cannot make the samples you asked for, the log says it tried again with one.
   If the graphics cannot start at all, the log says "running without graphics". On a PC with no working graphics
   driver but a software Vulkan device (Mesa's lavapipe, for instance), the environment variable
   `MH2GS_ALLOW_CPU_DEVICE` (set to any value) lets OpenMH2 use it. It is slow, but the image shows.
7. **No sound, or the sound stutters.** Check that `mute` is off and that no other program is using your sound
   output device. The log says "no sound device" when the sound device could not be opened, and it counts the sound's
   underruns at the end of a session. (`mh2` does not start at all without SDL3's library next to it.)
8. **A save state is refused.** The line on the screen gives the reason (another version or another setting). Put the
   setting back, or make a new save state.
9. **The launcher or a setting does nothing.** A setting given on the command line wins over the file for that
   start. Check `mh2 --help` for the option, and check that the line in `mh2.ini` has no typo (the log names the
   lines it did not use).
10. **Starting over.** Copy your memory cards (the `cards` folder) and `mh2.ini` somewhere safe first. Then move the
    user data folder aside. The next start is a first start, and the launcher makes the game archive again from the
    disc. To save the 2.7 GB, keep `mh2-disc.arc` and `mh2-movies.txt` in place.

**Reporting a problem.** Open an issue with: what you did and what you expected; your system, graphics card and
driver; the `mh2.log` of the session (it holds paths on your PC, so read it before you post); and your settings
(`mh2.ini`). Never attach the game archive, `mh2-core`, a memory card, a save state or a texture dump.

## 9. Licences and credits

OpenMH2's own work is CC0 1.0. The game's code, data and text are Capcom's, and the project claims no rights in
them. The program uses SDL3 (zlib licence), paraLLEl-GS (LGPL-3.0-or-later; it is loaded at run time so that you
can replace it), FFmpeg (LGPL, shared libraries), stb (public domain or MIT), zlib, newlib and fdlibm (their own
notices), two of PCSX2's PS2 math routines (GPL-3.0-or-later; they are built into `mh2-core`, which you build on
your PC) and the Noto Sans font (SIL Open Font License 1.1). `THIRD_PARTY.txt` lists them with their licences.
Credit to PCSX2 for providing a solution for better emulating the PS2's math on PCs. The README says how the project was made, including the help of an AI model. Monster Hunter is a trademark of Capcom.
This project is not affiliated with or endorsed by Capcom.
