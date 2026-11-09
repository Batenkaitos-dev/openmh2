/*
 * Every string the launcher and --help show, in one table (the launcher design, decision 8): English in v1, the table
 * a Japanese version replaces. The settings table (settings.c) names a setting's label, help and choices by these ids.
 * Texts of the development build's options are listed apart, so that the release holds none of them.
 */
#ifndef MH2_LAUNCHER_TEXT_H
#define MH2_LAUNCHER_TEXT_H

#define LTEXTS(X) \
    X(T_NONE, "") \
    /* settings: label, help */ \
    X(T_ARCHIVE, "Game archive") \
    X(T_ARCHIVE_H, "Your game's disc in one file (default: mh2-disc.arc next to mh2, or in the user data folder).") \
    X(T_MEMCARD, "Card from an earlier version") \
    X(T_MEMCARD_H, "A memory card file from an earlier version, offered for import at the first start.") \
    X(T_CARDS, "Memory cards folder") \
    X(T_CARDS_H, "Where OpenMH2 keeps its memory cards (default: cards in the user data folder).") \
    X(T_CARD1, "Memory card, slot 1") \
    X(T_CARD1_H, "The card from the memory cards folder to use in slot 1 (default Mcd001.ps2).") \
    X(T_CARD2, "Memory card, slot 2") \
    X(T_CARD2_H, "The card from the memory cards folder to use in slot 2 (default none).") \
    X(T_FULLSCREEN, "Fullscreen") \
    X(T_FULLSCREEN_H, "Starts the game in fullscreen. F11 or Alt+Enter switches while you play.") \
    X(T_MUTE, "Mute") \
    X(T_MUTE_H, "Turns all sound off.") \
    X(T_VSYNC, "V-Sync") \
    X(T_VSYNC_H, "Prevents screen tearing. Auto: on in fullscreen only. Off: shows each frame as soon as it is drawn.") \
    X(T_SSAA, "Anti-aliasing") \
    X(T_SSAA_H, "Smooths jagged edges using 2, 4, 8 or 16 samples per pixel. Higher values need a faster graphics card.") \
    X(T_SSAA_SHOWN, "off|2x|4x|8x|16x") \
    X(T_SSAA_RULE, "1 (off), 2, 4, 8 or 16") \
    X(T_HIRES, "High resolution") \
    X(T_HIRES_H, "Draws the game at twice its original resolution (1280x896). Uses at least 4x anti-aliasing.") \
    X(T_WIDESCREEN, "Widescreen") \
    X(T_WIDESCREEN_H, "Shows the game in 16:9, with a camera view 33% wider. The HUD, menus and text keep their shape. Movies stay 4:3.") \
    X(T_OFFLINE_HUB, "Offline Dundorma") \
    X(T_OFFLINE_HUB_H, "Visit Dundorma, the online town, with no internet connection: the village gate leads there. Take its online quests and earn Hunter Rank, awards and rewards, kept on your memory card.") \
    X(T_TEXDUMP, "Dump textures") \
    X(T_TEXDUMP_H, "Saves every texture the game draws, once, as a PNG file in the dump folder. Use it to make texture packs.") \
    X(T_TEXPACKS, "Use texture packs") \
    X(T_TEXPACKS_H, "Replaces a texture with the PNG file of the same name from your packs folder. The file can be the original size or larger (2x, 3x, etc.).") \
    X(T_TEXMB, "Texture cache (MB)") \
    X(T_TEXMB_H, "Memory limit for texture packs, in your computer's RAM and again on the graphics card (16 to 16384, default 1024). The RAM part never goes above half of your free memory.") \
    X(T_TEXPRE, "Preload texture packs") \
    X(T_TEXPRE_H, "Loads the packs that are on before the game starts, as far as the cache above allows, and logs its progress. Off: each texture loads when it is first drawn, and the original shows until then.") \
    X(T_TEXTPACK, "Translation pack") \
    X(T_TEXTPACK_H, "off, or the name of a folder in text_packs (in the user data folder) with a translation's files and text. A save state made with a pack loads only with that pack.") \
    X(T_TEXTURES, "Textures folder") \
    X(T_TEXTURES_H, "Holds the dump and packs folders (default: textures in the user data folder).") \
    X(T_USBKB, "Keyboard typing") \
    X(T_USBKB_H, "Lets you type with your keyboard where the game asks for text. It works as the game's USB keyboard.") \
    X(T_TOGGLE, "Typing key") \
    X(T_TOGGLE_H, "Switches between typing and using the keyboard as the controller. Set it to none to stay in the mode you start in.") \
    X(T_KEY_YEN, "Yen key") \
    X(T_KEY_RO, "Ro key") \
    X(T_KEY_HENKAN, "Henkan key") \
    X(T_KEY_MUHENKAN, "Muhenkan key") \
    X(T_KEY_KANA, "Kana key") \
    X(T_KEY_ZENKAKU, "Zenkaku key") \
    X(T_KEY_H, "Which of your keys stands in for this key of a Japanese keyboard. Set it to none for no key.")     X(T_SKEY_SAVE, "Save state key")     X(T_SKEY_SAVE_H, "Saves the game to the selected slot. Set it to none for no key. It does not work during movies or while you type.")     X(T_SKEY_LOAD, "Load state key")     X(T_SKEY_LOAD_H, "Loads the selected slot. Set it to none for no key. A save state made by another version or with other settings can't be loaded.")     X(T_SKEY_NEXT, "Next slot key")     X(T_SKEY_NEXT_H, "Selects the next of the ten slots. Set it to none for no key.")     X(T_SKEY_PREV, "Previous slot key")     X(T_SKEY_PREV_H, "Selects the previous of the ten slots. Set it to none for no key.")     X(T_STATES, "Save states folder")     X(T_STATES_H, "Where the ten save state slots are kept (default: states in the user data folder). A save state contains data from your game: don't share it.") \
    X(T_LAUNCHER, "Show the launcher at every start") \
    X(T_LAUNCHER_H, "Off: the game starts right away, unless the game archive needs fixing. To show the launcher again, run mh2 with --launcher (on Windows, hold Shift).") \
    X(T_BIND, "Key and button bindings") \
    X(T_BIND_H, "key.cross = Space or pad.l2 = lefttrigger: a keyboard key or a gamepad button for a controller button (none removes the binding).") \
    X(T_OLD_ORIG, "is a setting of an earlier version and is not used now: OpenMH2 reads the game archive only") \
    X(T_OLD_ANYCORE, "is a setting of an earlier version and is not used now") \
    X(T_DEFAULT, "(default)") \
    /* the launcher's pages */ \
    X(T_TITLE, "OpenMH2") \
    X(T_TITLE_SUB, "for Monster Hunter 2 (Dos)") \
    X(T_PLAY, "Play") \
    X(T_PLAY_H, "Starts the game with these settings.") \
    X(T_PICTURE, "Graphics") \
    X(T_PICTURE_H, "How the game looks and sounds on your PC. None of these change how the game plays.") \
    X(T_HEAD_PICTURE, "GRAPHICS") \
    X(T_GAME, "Game") \
    X(T_GAME_H, "Widescreen, Offline Dundorma, fast loading, typing, cameras and hotkeys. Changes apply the next time the game starts.") \
    X(T_HEAD_GAME, "GAME") \
    X(T_FOLDERS, "Folders and logs") \
    X(T_FOLDERS_H, "Where OpenMH2 keeps its files, and the log of the last session.") \
    X(T_HEAD_FOLDERS, "FOLDERS AND LOGS") \
    X(T_QUIT, "Quit") \
    X(T_QUIT_H, "Closes the launcher without starting the game.") \
    X(T_BACK, "Back") \
    X(T_BACK_H, "Goes back.") \
    X(T_DEFAULTS, "Defaults") \
    X(T_DEFAULTS_H, "Resets the settings on this page to their defaults.") \
    X(T_SKIP, "Skip this screen next time") \
    X(T_SKIP_H, "On: the game starts right away next time, unless the game archive needs fixing. To show the launcher again, run mh2 with --launcher (on Windows, hold Shift).") \
    X(T_CMDLINE, "(command line)") \
    X(T_FASTLOAD, "Fast loading") \
    X(T_FASTLOAD_H, "Skips the wait on loading screens, so going from the village to a hunt takes under 2 seconds instead of about 6. Movies, music and voices keep their normal timing. Your saves are not affected.") \
    X(T_PZKEYS, "Save state, pause and speed keys") \
    X(T_PZKEYS_H, "Keys to save and load a save state, pause the game, advance it one frame and fast-forward it, and the fast-forward speed.") \
    X(T_HEAD_PZKEYS, "SAVE STATE, PAUSE AND SPEED KEYS") \
    X(T_PZ_PAUSE, "Pause key") \
    X(T_PZ_PAUSE_H, "Pauses the game and keeps the screen as it is; press it again to resume. With the free camera on, it pauses the free camera instead. Change it in mh2.ini.") \
    X(T_PZ_STEP, "Frame advance key") \
    X(T_PZ_STEP_H, "While paused, runs the game for one frame each time you press it, and repeatedly while you hold it. Set it to none for no key.") \
    X(T_PZ_FAST, "Fast-forward key") \
    X(T_PZ_FAST_H, "Hold it to run the game faster, without sound. It does not work while paused, while you type or during movies. Set it to none for no key.") \
    X(T_PZ_CAP, "Fast-forward speed") \
    X(T_PZ_CAP_H, "The most times faster than normal that fast-forward goes (2, 3, 4, 6, 8 or none: as fast as your PC can). Change it in mh2.ini, for example fast_forward_cap = 8.") \
    X(T_FREECAM, "Cameras") \
    X(T_FREECAM_H, "A free camera to fly through the village or a hunt while the game goes on (its key, what the game receives while you control it, and its speed), and the game's camera on the right stick.") \
    X(T_HEAD_FCKEYS, "CAMERAS") \
    X(T_FCK_TOGGLE, "Freecam key") \
    X(T_FCK_TOGGLE_H, "Turns the free camera on and off. W A S D move, Q E go down and up, the mouse looks, the wheel sets the speed, Shift is faster, Ctrl slower, Z X zoom, R resets, Tab gives the controller back, Space pauses, Period advances a frame, P takes a photo, Esc leaves.") \
    X(T_FCK_FWD, "Move forward") \
    X(T_FCK_BACK, "Move back") \
    X(T_FCK_LEFT, "Move left") \
    X(T_FCK_RIGHT, "Move right") \
    X(T_FCK_DOWN, "Move down") \
    X(T_FCK_UP, "Move up") \
    X(T_FCK_RESET, "Back to the game's camera") \
    X(T_FCK_NARROW, "Narrower view") \
    X(T_FCK_WIDE, "Wider view") \
    X(T_FCK_HOLD, "Hold") \
    X(T_FCK_PAUSE, "Pause key") \
    X(T_FCK_PAUSE_H, "Pauses and resumes the game while the free camera is on: the world stands still and the camera keeps moving. A pause waits until menus, movies and memory card access are over.") \
    X(T_FCK_PHOTO, "Photo key") \
    X(T_FCK_PHOTO_H, "Saves a photo of the game, at the size you play at, to the photos folder. It works while the free camera is on. Set it to none for no key.") \
    X(T_FCK_HIDE, "Hide the HUD key") \
    X(T_FCK_HIDE_H, "Does nothing yet: hiding the HUD is not available. The key is reserved for later.") \
    X(T_PICS, "Photos folder") \
    X(T_PICS_H, "Where photo mode saves its photos, named MH2-date-time.png (default: pictures in the user data folder).") \
    X(T_FCK_STEP, "Frame advance key") \
    X(T_FCK_STEP_H, "While the game is paused, runs it for one frame each time you press it. Set it to none for no key.") \
    X(T_FCK_H, "Which of your keys does this for the free camera. Set it to none for no key. Change it in mh2.ini.") \
    X(T_FCOTHER, "Movement keys") \
    X(T_FCOTHER_H, "W A S D move, Q E go down and up, Shift is faster, Ctrl slower, Z X zoom, R resets, Tab gives the controller back to the game, Esc leaves, the mouse looks and its wheel sets the speed. Change the keys in mh2.ini, for example freecam_key_forward = I.") \
    /* the content page */ \
    X(T_CONTENT, "Content") \
    X(T_CONTENT_H, "Texture packs that replace the game's textures.") \
    X(T_HEAD_CONTENT, "CONTENT") \
    X(T_CONTENT_SUM_OFF, "texture packs off") \
    X(T_CONTENT_SUM_NONE, "no texture packs") \
    X(T_CONTENT_SUM, "%d of %d texture packs on") \
    X(T_PACKS_NONE, "No texture packs") \
    X(T_PACKS_NONE_H, "A pack is a folder in the packs folder that holds the PNG files of its textures. Open the packs folder below to find it.") \
    X(T_PACK_FILE1, "1 file") \
    X(T_PACK_FILES, "%s files") \
    X(T_PACK_NOFILES, "no files") \
    X(T_PACK_BY, "by %s") \
    X(T_PACK_MISSING, "folder missing") \
    X(T_PACK_NOKEY, "rename the folder") \
    X(T_PACK_H, "Left and Right turn the pack on or off. Enter or a click opens a small menu: turn it on or off, or move it up or down the list. When two packs have the same texture, the pack higher in the list wins.") \
    X(T_PACK_MISSING_H, "mh2.ini has a line for this pack, but its folder is not in the packs folder. The line is kept in case the folder comes back. Press Enter to forget it.") \
    X(T_PACK_NOKEY_H, "This folder's name can't be written in mh2.ini (it has an =, # or ; in it, starts with a [, or has spaces at either end), so the pack is always on. Rename the folder to turn it off.") \
    X(T_PACK_MORE, "%d more packs, not shown") \
    X(T_PACK_MORE_H, "The page lists the first 40 packs. The others stay as they are in mh2.ini.") \
    X(T_PACK_ON, "Turn on") \
    X(T_PACK_OFF, "Turn off") \
    X(T_PACK_UP, "Move up") \
    X(T_PACK_DOWN, "Move down") \
    X(T_PACK_FORGET, "Forget it") \
    X(T_CONTENT_PACKS, "Content packs") \
    X(T_CONTENT_NONE, "none installed") \
    X(T_CONTENT_PACKS_H, "Packs that add new content to the game. None are available yet.") \
    X(T_OPEN_PACKS, "Open the packs folder") \
    X(T_OPEN_PACKS_H, "Opens the folder where texture packs go, in your file manager. It is created if it does not exist.") \
    X(T_PACKS_NOFOLDER, "(no folder)") \
    X(T_FCIN, "Game input in the freecam") \
    X(T_FCIN_H, "neutral: while you control the free camera, the game receives a controller with nothing pressed, and the keyboard and gamepad move the camera. pad: the gamepad keeps playing the game, and the keyboard and mouse move the camera.") \
    X(T_FCSPEED, "Freecam speed") \
    X(T_FCSPEED_H, "How fast the free camera moves, in game units per second (default 400). The mouse wheel changes it while the camera is on. Change it in mh2.ini, for example freecam_speed = 1500.") \
    X(T_FCPAD, "Gamepad: both sticks") \
    X(T_FCPAD_H, "off (default): the sticks work as in the game. on: pressing both sticks in together (L3 and R3) turns the free camera on and off. The game never sees both presses at once, and a single stick press reaches it 60 ms late, in case the other stick joins.") \
    X(T_CAMSTICK, "Camera on the right stick") \
    X(T_CAMSTICK_H, "The right stick turns and tilts the game's camera, as the D-pad does. The game uses that stick to attack, so hold: it is the camera only while you hold L3, and on: it is the camera always. More keys in mh2.ini: camera_stick_button, _which, _invert, _speed.") \
    X(T_CAMSTICK_BTN, "Camera stick button") \
    X(T_CAMSTICK_BTN_H, "The button you hold in hold mode (l3, select, l2 or r2). The game does not receive it. L3 is a button the game does not use while you play. Change it in mh2.ini.") \
    X(T_CAMSTICK_WHICH, "Camera stick") \
    X(T_CAMSTICK_WHICH_H, "Which stick is the camera: right (default) or left. Change it in mh2.ini.") \
    X(T_CAMSTICK_INV, "Camera stick, invert") \
    X(T_CAMSTICK_INV_H, "Reverses an axis: x (left and right), y (up and down), xy or none. The game's camera option (Options, Camera operation) reverses the D-pad as well, so leave it at NORMAL and invert here.") \
    X(T_CAMSTICK_SPEED, "Camera stick, speed") \
    X(T_CAMSTICK_SPEED_H, "How fast the camera turns at full tilt, as a percentage of the D-pad's turning speed (100 is the same speed). A lighter tilt turns more slowly.") \
    X(T_JKEYS, "Japanese keyboard keys") \
    X(T_JKEYS_H, "The keys that stand in for the extra keys of a Japanese keyboard. Change them in mh2.ini, for example usb_key_yen = F1.") \
    X(T_HEAD_JKEYS, "JAPANESE KEYBOARD KEYS") \
    X(T_OPEN_DATA, "Open the user data folder") \
    X(T_OPEN_DATA_H, "Opens the folder with your memory cards, settings and log in your file manager.") \
    X(T_OPEN_LOGS, "Open the logs folder") \
    X(T_OPEN_LOGS_V, "(the log and crash dumps)") \
    X(T_OPEN_LOGS_H, "If the game stops, its log and the crash dump are here. Nothing is sent anywhere.") \
    X(T_SHOW_LOG, "Show the last log") \
    X(T_SHOW_LOG_H, "The last lines of the log from your previous session.") \
    X(T_LOG_LINES, "mh2.log, %d lines") \
    X(T_NO_LOG, "(no log yet)") \
    X(T_SETTINGS_FILE, "Settings file") \
    X(T_SETTINGS_FILE_H, "The file your changes are saved in. Lines you wrote yourself are kept.") \
    X(T_NONE_SET, "(none)") \
    X(T_HEAD_LOG, "THE LAST LOG") \
    X(T_LOG_KEYS, "Up, Down, PageUp and PageDown scroll. Cancel goes back.") \
    X(T_SAVE_Q, "Save the changes?") \
    X(T_YES, "Yes") \
    X(T_NO, "No") \
    X(T_OK, "OK") \
    X(T_SAVE_FAIL, "mh2.ini could not be saved") \
    X(T_PLAY_ANYWAY, "Play anyway") \
    X(T_OPEN_FAIL, "The folder could not be opened.") \
    X(T_SUM_DEFAULTS, "defaults") \
    X(T_SUM_SSAA, "anti-aliasing %s") \
    X(T_SUM_HIRES, "high resolution") \
    X(T_SUM_FULLSCREEN, "fullscreen") \
    X(T_SUM_WIDE_ON, "widescreen on") \
    X(T_SUM_WIDE_OFF, "widescreen off") \
    X(T_SUM_KEYBOARD, "keyboard typing on") \
    X(T_SUM_SEE, "see the page") \
    X(T_SUM_OFFLINE_HUB, "Offline Dundorma") \
    /* the game archive */ \
    X(T_DISC, "Game archive") \
    X(T_DISC_H, "Your game's disc in one file, made from your own copy of the game (NTSC-J disc SLPM-66280). Nothing is sent anywhere.") \
    X(T_HEAD_DISC, "GAME ARCHIVE") \
    X(T_CARDS_PAGE_H, "Which memory card is in each slot, a new card, and cards from an earlier version.") \
    X(T_FREE, "Free space") \
    X(T_FREE_H, "The game archive needs about 2.7 GB of free space where the file shown above is written.") \
    X(T_ARC, "Status") \
    X(T_ARC_READY, "ready") \
    X(T_ARC_MISSING, "missing") \
    X(T_ARC_BAD, "not valid for this version") \
    X(T_ARC_NOTABLE, "the movie table is missing") \
    X(T_ARC_SUM_READY, "ready (%d files)") \
    X(T_ARC_FILE, "File") \
    X(T_ARC_SIZE, "Size (checked)") \
    X(T_ARC_SIZE_V, "%s, %d files") \
    X(T_ARC_SIZE_H, "The archive's header and its size agree. Each file was checked against the known disc when the archive was made.") \
    X(T_ARC_MISSING_H, "OpenMH2 needs the game archive to start the game. Make it from your own copy of the game: a disc image, or a folder with the disc's files.") \
    X(T_ARC_BAD_H, "This file is not a game archive for this version of OpenMH2. Make the archive again from your own copy of the game.") \
    X(T_ARC_NOTABLE_H, "The movie table, a small file next to the archive with the timing of the movies, is missing. Making it takes about a minute.") \
    X(T_MAKE_ISO, "Make the archive from a disc image...") \
    X(T_MAKE_ISO_AGAIN, "Make the archive again from a disc image...") \
    X(T_MAKE_DIR, "Make the archive from a folder...") \
    X(T_MAKE_DIR_AGAIN, "Make the archive again from a folder...") \
    X(T_MAKE_ISO_H, "Choose the disc image (.iso) of your copy of the game. The archive is written to the file shown above.") \
    X(T_MAKE_DIR_H, "Choose the folder with the disc's files (the one with SYSTEM.CNF in it). The archive is written to the file shown above.") \
    X(T_MAKE_TABLE, "Make the movie table") \
    X(T_MAKE_TABLE_H, "Measures the timing of the game's movies once, from the archive. Takes about a minute.") \
    X(T_CHOOSE_ARC, "Choose another archive...") \
    X(T_CHOOSE_ARC_H, "Use a game archive stored somewhere else. It is saved as the archive setting in mh2.ini.") \
    X(T_PLAY_NOARC, "The game can't start because the game archive is missing. Choose Game archive on the Home page.") \
    X(T_PLAY_NOARC_BAD, "The game can't start because the game archive is not valid for this version. Choose Game archive on the Home page.") \
    X(T_PLAY_NOARC_NOTABLE, "The game can't start because the movie table is missing. Choose Game archive on the Home page.") \
    X(T_BANNER, "The game archive is missing. Choose Game archive.") \
    X(T_BANNER_BAD, "The game archive is not valid for this version. Choose Game archive.") \
    X(T_BANNER_NOTABLE, "The movie table is missing. Choose Game archive.") \
    X(T_ARC_BADFILE, "That file is not a game archive for this version of OpenMH2.") \
    X(T_MAKING, "MAKING THE GAME ARCHIVE") \
    X(T_MK_READING, "Reading   %s") \
    X(T_MK_STAGE_SIZE, "Getting ready...") \
    X(T_MK_FILE_OF, "File %d of %d   %s") \
    X(T_MK_CHECKING, "Checking each file against the known disc.") \
    X(T_MK_MOVIE_OF, "Timing the movies: %d of %d") \
    X(T_MK_MOVIES_NOTE, "The game's movies are timed once, from the archive.") \
    X(T_MK_STEP, "Step %d of 2") \
    X(T_MK_CANCEL, "Cancel") \
    X(T_MK_CANCEL_H, "Stops and removes the half-written file.") \
    X(T_MK_DONE, "The game archive is made") \
    X(T_MK_DONE_V, "%s, %d files, checked") \
    X(T_MK_CONTINUE, "Continue") \
    X(T_MK_CONTINUE_H, "Goes on.") \
    X(T_MK_FAILED, "The game archive could not be made") \
    X(T_MK_BADSUM, "%s does not match the NTSC-J disc (its check value differs). It is another version of the game, or the copy is damaged.") \
    X(T_MK_SHORT, "%s is shorter than the same file on the NTSC-J disc. The copy is damaged or incomplete.") \
    X(T_MK_MISSING, "%s is missing from %s. That folder does not contain the files of the NTSC-J disc.") \
    X(T_MK_NODISC, "%.*s contains no disc image (*.iso) and no disc files (SYSTEM.CNF).") \
    X(T_MK_NOTDISC, "%.*s is not a disc image or a folder.") \
    X(T_MK_SIZE, "%s is not the size it has on the NTSC-J disc. It is another version of the game, or the copy is damaged.") \
    X(T_MK_NOTISO, "%s is not a usable disc image. It must be an .iso file in the usual format for a PS2 disc (ISO 9660 with 2048-byte sectors).") \
    X(T_MK_NOTONDISC, "%s is not on that disc image, so this is not the NTSC-J disc.") \
    X(T_MK_LAYOUT, "%s is not in the place, or not the size, it has on the NTSC-J disc. This is another version of the game.") \
    X(T_MK_TAIL, "%s ends differently from the same file on the NTSC-J disc. It is another version of the game, or the copy is damaged.") \
    X(T_MK_READ, "%s could not be read (%s)") \
    X(T_MK_WRITE, "The game archive could not be written in %s (%s). If the disk is full, free some space.") \
    X(T_MK_OPEN, "%s could not be opened (%s)") \
    X(T_MK_OTHER, "The game archive could not be made: %s") \
    X(T_MK_NOTABLE, "The game archive is made, but its movie table is not: %s") \
    X(T_MK_SPACE, "Not enough free space. The game archive needs %s, but the folder %s has only %s free. Free some space, or choose another place with archive = FILE in mh2.ini.") \
    X(T_MK_NOSPACE_FOLDER, "There is no folder to put the game archive in.") \
    /* the memory cards */ \
    X(T_CARDS_PAGE, "Memory cards") \
    X(T_HEAD_CARDS, "MEMORY CARDS") \
    X(T_PORT1, "Slot 1") \
    X(T_PORT2, "Slot 2") \
    X(T_PORT1_H, "The memory card in slot 1. Left and Right choose from the cards in the memory cards folder.") \
    X(T_PORT2_H, "The memory card in slot 2 (none by default). Left and Right choose from the cards in the memory cards folder.") \
    X(T_CARD_NONE, "none") \
    X(T_CARD_NEW, "%s (new)") \
    X(T_CARD_INFO, "%s, %s") \
    X(T_CARD_INUSE, "in use") \
    X(T_CARD_ROW_H, "A memory card in the memory cards folder. OpenMH2 does not look inside it.") \
    X(T_CARD_MORE, "(%d more in the folder)") \
    X(T_NEWCARD, "New card...") \
    X(T_NEWCARD_H, "Sets up a new, empty memory card for a slot. The file is created when the game first starts with it.") \
    X(T_IMPORT, "Import cards from an earlier version...") \
    X(T_IMPORT_H, "Copies the memory cards of an earlier version of OpenMH2 into the memory cards folder. It never overwrites a card, and the old files are left as they are.") \
    X(T_IMPORT_NONE, "(none found)") \
    X(T_IMPORT_NONE_H, "No cards from an earlier version were found (looked for the memcards folder in the user data folder, and for memcard1 and memcard2 in mh2.ini).") \
    X(T_IMPORT_DONE, "Copied %d card(s) into the memory cards folder. %d were already there and were left alone.") \
    X(T_IMPORT_FAIL, "A card could not be copied. See the log.") \
    X(T_OPEN_CARDS, "Open the memory cards folder") \
    X(T_OPEN_CARDS_H, "Opens the folder with your memory cards in your file manager.") \
    X(T_NO_CARDS_DIR, "(no folder)") \
    X(T_HEAD_NEWCARD, "NEW MEMORY CARD") \
    X(T_NC_NAME, "Name") \
    X(T_NC_NAME_H, "Type the file name for the card. .ps2 is added if you leave it out. Press Enter to go on.") \
    X(T_NC_PORT, "For slot") \
    X(T_NC_PORT_H, "The slot the new card goes in.") \
    X(T_NC_MAKE, "Make the card") \
    X(T_NC_MAKE_H, "Puts the card in the slot. The empty card is created when the game starts.") \
    X(T_NC_BAD, "%s can't be used as a card name. Use a plain file name, with no folder and not ending in .journal, .tmp, .bak or .lock.") \
    X(T_NC_EXISTS, "%s is already in the memory cards folder. Choose it in the slot rows instead.") \
    X(T_NC_FIXED, "Slot %d is set on the command line.") \
    /* the game, built from the player's game archive with the kit */ \
    X(T_BUILD, "BUILDING THE GAME") \
    X(T_BD_INTRO, "The game is built on this computer from your game archive, using the tools in the kit folder. This happens once, and again after an update. You don't need your disc.") \
    X(T_BD_STEP, "Step %d of %d: %s") \
    X(T_BD_START, "Starting") \
    X(T_BD_NOTE, "This takes 5 to 15 minutes. The window can stay open in the background.") \
    X(T_BD_CANCEL, "Cancel") \
    X(T_BD_CANCEL_H, "Stops the build and removes what it made. You can build the game later from the Home page.") \
    X(T_BD_STOPPING, "Stopping...") \
    X(T_BD_DONE, "The game is built") \
    X(T_BD_DONE_V, "It is saved in the user data folder, and the temporary build files are removed") \
    X(T_BD_FAILED, "The game could not be built") \
    X(T_BD_AGAIN, "Try again") \
    X(T_BD_LOGS, "Open the logs folder") \
    X(T_BD_LATER, "Later") \
    X(T_GAMEPROG, "Build the game") \
    X(T_GAMEPROG_H, "Builds the game on this computer from your game archive, using the tools in the kit folder. This happens once, and again after an update. You don't need your disc.") \
    X(T_GP_READY, "built") \
    X(T_GP_NOT, "not built yet") \
    X(T_PLAY_NOCORE, "The game is not built yet. Choose \"Build the game\" first.") \
    /* the first start */ \
    X(T_HEAD_FIRST, "FIRST-TIME SETUP") \
    X(T_FIRST_STEP, "Step %d of %d: %s") \
    X(T_FIRST_BUILD, "Building the game") \
    X(T_FIRST_1, "Your game files") \
    X(T_FIRST_2, "Making the game archive") \
    X(T_FIRST_3, "Your save") \
    X(T_FIRST_1_H, "OpenMH2 needs your own copy of Monster Hunter 2 (Dos), the Japanese PS2 game (disc SLPM-66280). Use its disc image (.iso) or a folder with its files.") \
    X(T_FIRST_USE, "Use the %s") \
    X(T_FIRST_USE_ISO, "disc image") \
    X(T_FIRST_USE_DIR, "folder") \
    X(T_FIRST_USE_H, "Found on this computer. Press once to make the game archive from it.") \
    X(T_FIRST_LATER, "Set up later") \
    X(T_FIRST_LATER_H, "Goes to the Home page. This setup comes back at the next start until the game is set up.") \
    X(T_FIRST_3_H, "Your saved games are kept on a memory card file. Start a new card, or bring your cards from an earlier version.") \
    X(T_FIRST_NEWCARD, "Start a new memory card") \
    X(T_FIRST_NEWCARD_H, "The game creates an empty card, Mcd001.ps2, in the memory cards folder the first time it starts.") \
    X(T_FIRST_IMPORT, "Import cards from an earlier version") \
    X(T_FIRST_FINISH, "Go to the Home page") \
    /* choices */ \
    X(T_CH_FLAG, "off|on") \
    X(T_CH_VSYNC, "auto|on|off") \
    /* options that are not settings: help */ \
    X(T_O_CONFIG, "Use this settings file instead of the usual mh2.ini.") \
    X(T_O_SET, "Changes one setting for this start only, for example --set ssaa=4 (up to 32 times).") \
    X(T_O_CORE, "Use this game program (default: mh2-core next to mh2).") \
    X(T_O_NOWINDOW, "Runs without a window or launcher (for tests and automation).") \
    X(T_O_GSOFF, "Turns graphics off: nothing is drawn (for tests).") \
    X(T_O_FAST, "Runs the game as fast as it can instead of at normal speed (for tests).") \
    X(T_O_NOCARD, "No memory card in slot 1.") \
    X(T_O_QUITAFTER, "Quits after N display refreshes (two for each game frame, about 60 a second), for tests.") \
    X(T_O_IMPORTCARD, "Copies a memory card file into the memory cards folder, then quits.") \
    X(T_O_LOGFILE, "Writes the log to this file.") \
    X(T_O_LAUNCHER, "Shows the launcher, even if mh2.ini turns it off.") \
    X(T_O_NOLAUNCHER, "Starts the game without showing the launcher.") \
    X(T_O_HELP, "Shows this help.") \
    X(T_O_REST, "Everything after -- is passed to the game program (mh2-core).") \
    X(T_O_MEMCARD1, "Use this memory card file in slot 1 (an option of mh2-core).") \
    X(T_O_MEMCARD2, "Use this memory card file in slot 2 (an option of mh2-core).") \
    X(T_O_ARCHIVE, "The game archive file.")


enum {
#define X(id, s) id,
    LTEXTS(X)
#undef X
    T_COUNT
};

extern const char *const g_text[T_COUNT];
#define TX(id) g_text[id]

#endif
