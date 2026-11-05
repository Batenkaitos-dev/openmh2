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
    X(T_ARCHIVE_H, "The game's disc as one file (default: mh2-disc.arc next to mh2, or in the user data folder).") \
    X(T_MEMCARD, "Card of an earlier setup") \
    X(T_MEMCARD_H, "A memory card file of the old build, offered for import at the first start.") \
    X(T_CARDS, "Cards folder") \
    X(T_CARDS_H, "Where mh2 keeps its memory cards (default: cards in the user data folder).") \
    X(T_CARD1, "Card in port 1") \
    X(T_CARD1_H, "The card in the cards folder for port 1 (default Mcd001.ps2).") \
    X(T_CARD2, "Card in port 2") \
    X(T_CARD2_H, "The card in the cards folder for port 2 (default none).") \
    X(T_FULLSCREEN, "Fullscreen") \
    X(T_FULLSCREEN_H, "Start in a full screen. F11 or Alt+Enter switches while you play.") \
    X(T_MUTE, "Mute") \
    X(T_MUTE_H, "No sound.") \
    X(T_VSYNC, "Wait for the screen") \
    X(T_VSYNC_H, "auto: on in a full screen only. on: no tearing. off: show each picture as soon as it is drawn.") \
    X(T_SSAA, "Smoother edges") \
    X(T_SSAA_H, "Draws each pixel with 2, 4, 8 or 16 samples. Changes how the game is drawn, never how it plays.") \
    X(T_SSAA_SHOWN, "off|2x|4x|8x|16x") \
    X(T_SSAA_RULE, "1 (off), 2, 4, 8 or 16") \
    X(T_HIRES, "Sharper picture") \
    X(T_HIRES_H, "A picture of twice the width and height (1280x896), made from at least 4 samples.") \
    X(T_WIDESCREEN, "Widescreen") \
    X(T_WIDESCREEN_H, "The camera sees 4/3 wider; the HUD, menus and text keep their own shape. Movies stay 4:3.") \
    X(T_TEXDUMP, "Write out the textures") \
    X(T_TEXDUMP_H, "Every texture the game draws is written once as a PNG to the dump folder.") \
    X(T_TEXPACKS, "Use texture packs") \
    X(T_TEXPACKS_H, "A texture that has a PNG of its name in the packs folder is drawn from that PNG.") \
    X(T_TEXTURES, "Textures folder") \
    X(T_TEXTURES_H, "Holds the dump and packs folders (default: textures in the user data folder).") \
    X(T_USBKB, "PC keyboard in the game") \
    X(T_USBKB_H, "The keyboard is the game's USB keyboard, for the places that take typing.") \
    X(T_TOGGLE, "Typing key") \
    X(T_TOGGLE_H, "Switches between typing and the pad's keyboard keys (none: you stay in the mode you start in).") \
    X(T_KEY_YEN, "Yen key") \
    X(T_KEY_RO, "Ro key") \
    X(T_KEY_HENKAN, "Henkan key") \
    X(T_KEY_MUHENKAN, "Muhenkan key") \
    X(T_KEY_KANA, "Kana key") \
    X(T_KEY_ZENKAKU, "Zenkaku key") \
    X(T_KEY_H, "Which of your keys stands for a key of the Japanese keyboard (none: no key).")     X(T_SKEY_SAVE, "Save state key")     X(T_SKEY_SAVE_H, "Saves the game to the chosen slot (none: no key). Not while a movie plays or the keyboard types.")     X(T_SKEY_LOAD, "Load state key")     X(T_SKEY_LOAD_H, "Loads the chosen slot (none: no key). A state made by another build or with other settings is refused.")     X(T_SKEY_NEXT, "Next slot key")     X(T_SKEY_NEXT_H, "Chooses the next of the ten slots (none: no key).")     X(T_SKEY_PREV, "Previous slot key")     X(T_SKEY_PREV_H, "Chooses the previous of the ten slots (none: no key).")     X(T_STATES, "States folder")     X(T_STATES_H, "Where the ten slots are kept (default: states in the user data folder). A state holds data from your disc: do not share it.") \
    X(T_LAUNCHER, "Show this screen at every start") \
    X(T_LAUNCHER_H, "off: mh2 starts the game at once. Hold Shift at the start, or use --launcher, to show it again.") \
    X(T_BIND, "Key and pad bindings") \
    X(T_BIND_H, "key.cross = Space, pad.l2 = lefttrigger: a key or a pad button for a button of the pad (none unbinds).") \
    X(T_OLD_ORIG, "(the original files) is the original's: mh2 reads the disc archive only") \
    X(T_OLD_ANYCORE, "is the original's: mh2 doesn't place its threads") \
    X(T_DEFAULT, "(default)") \
    /* the launcher's pages */ \
    X(T_TITLE, "OpenMH2") \
    X(T_TITLE_SUB, "for Monster Hunter 2 (Dos)") \
    X(T_PLAY, "Play") \
    X(T_PLAY_H, "Starts the game with these settings.") \
    X(T_PICTURE, "Picture") \
    X(T_PICTURE_H, "How the game is drawn. These never change how it plays.") \
    X(T_HEAD_PICTURE, "PICTURE") \
    X(T_GAME, "Game") \
    X(T_GAME_H, "What the game computes, and the keyboard. They apply from the next start of the game.") \
    X(T_HEAD_GAME, "GAME") \
    X(T_FOLDERS, "Folders and logs") \
    X(T_FOLDERS_H, "Where the game keeps its files and the log of the last session.") \
    X(T_HEAD_FOLDERS, "FOLDERS AND LOGS") \
    X(T_QUIT, "Quit") \
    X(T_QUIT_H, "Closes this window without starting the game.") \
    X(T_BACK, "Back") \
    X(T_BACK_H, "Goes back.") \
    X(T_DEFAULTS, "Defaults") \
    X(T_DEFAULTS_H, "Puts the settings of this page back to their defaults.") \
    X(T_SKIP, "Skip this screen next time") \
    X(T_SKIP_H, "on: the game starts at once. Hold Shift at the start, or use --launcher, to show this screen again.") \
    X(T_CMDLINE, "(command line)") \
    X(T_FREECAM, "Freecam") \
    X(T_FREECAM_H, "A free camera: fly through the village or a hunt while the game goes on. Its key, what the game is given while it has control, and its speed.") \
    X(T_HEAD_FCKEYS, "FREECAM") \
    X(T_FCK_TOGGLE, "Freecam key") \
    X(T_FCK_TOGGLE_H, "Turns the free camera on and off (none: no key). W S A D move, Q E go down and up, the mouse looks, the wheel sets the speed, Shift is fast, Ctrl slow, Z X zoom, R resets, Tab hands the pad back, Space pauses, Period steps, P takes a photo, H hides the HUD, Escape leaves.") \
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
    X(T_FCK_PAUSE_H, "Pauses the game and back while the free camera is on: the world stands still and the camera moves. It waits in menus, while a movie plays and while the memory card is in use.") \
    X(T_FCK_PHOTO, "Photo key") \
    X(T_FCK_PHOTO_H, "Takes a picture of the game at the size of the picture you play at, into the pictures folder, while the free camera is on (none: no key).") \
    X(T_FCK_HIDE, "Hide the HUD key") \
    X(T_FCK_HIDE_H, "Hides the gauges, the map and the texts while the free camera has control, and shows them again. The fades stay. With the game paused too, it is photo mode.") \
    X(T_PICS, "Pictures folder") \
    X(T_PICS_H, "Where photo mode keeps its pictures, named MH2-date-time.png (default: pictures in the game's folder).") \
    X(T_FCK_STEP, "Step key") \
    X(T_FCK_STEP_H, "While paused, lets the game run one frame for each press (none: no key).") \
    X(T_FCK_H, "Which of your keys does this for the free camera (none: no key). Change it in mh2.ini.") \
    X(T_FCIN, "Game input in the freecam") \
    X(T_FCIN_H, "neutral: the game is given a pad with nothing pressed while the free camera has control, and the keyboard and the gamepad move the camera. pad: the gamepad still plays and the keyboard and the mouse move the camera.") \
    X(T_FCSPEED, "Freecam speed") \
    X(T_FCSPEED_H, "How fast the free camera moves, in game units a second (default 400). The wheel changes it while the camera is on. Change it in mh2.ini, for example freecam_speed = 1500.") \
    X(T_FCPAD, "Gamepad: both sticks") \
    X(T_FCPAD_H, "on: pressing the gamepad's two sticks in together (L3 and R3) turns the free camera on and off. The game never sees the two together; a stick press waits a moment (60 ms) to see if the other joins. off: the sticks are the game's.") \
    X(T_JKEYS, "Japanese keyboard keys") \
    X(T_JKEYS_H, "The keys that stand for the extra keys of a Japanese keyboard. Change them in mh2.ini, for example usb_key_yen = F1.") \
    X(T_HEAD_JKEYS, "JAPANESE KEYBOARD KEYS") \
    X(T_OPEN_DATA, "Open the game's folder") \
    X(T_OPEN_DATA_H, "Opens the folder with the memory cards, the settings and the log in your file manager.") \
    X(T_OPEN_LOGS, "Open the logs folder") \
    X(T_OPEN_LOGS_V, "(the log and crash dumps)") \
    X(T_OPEN_LOGS_H, "If the game stops, its log and the crash dump are here. Nothing is sent anywhere.") \
    X(T_SHOW_LOG, "Show the last log") \
    X(T_SHOW_LOG_H, "The last lines of the log of the session before this one.") \
    X(T_LOG_LINES, "mh2.log, %d lines") \
    X(T_NO_LOG, "(no log yet)") \
    X(T_SETTINGS_FILE, "Settings file") \
    X(T_SETTINGS_FILE_H, "The file the changes are saved in. Your own lines in it are kept.") \
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
    X(T_SUM_SSAA, "smoother edges %s") \
    X(T_SUM_HIRES, "sharper picture") \
    X(T_SUM_FULLSCREEN, "fullscreen") \
    X(T_SUM_WIDE_ON, "widescreen on") \
    X(T_SUM_WIDE_OFF, "widescreen off") \
    X(T_SUM_KEYBOARD, "PC keyboard on") \
    X(T_SUM_SEE, "see the page") \
    /* the disc and archive */ \
    X(T_DISC, "Disc and archive") \
    X(T_DISC_H, "The game's disc as one file, made from your own copy of the game (NTSC-J disc SLPM-66280). Nothing is sent anywhere.") \
    X(T_HEAD_DISC, "DISC AND ARCHIVE") \
    X(T_CARDS_PAGE_H, "Which memory card is in each port, a new card, and the old version's cards.") \
    X(T_FREE, "Free space") \
    X(T_FREE_H, "The archive needs about 2.7 GB where it is written, the file shown above.") \
    X(T_ARC, "Archive") \
    X(T_ARC_READY, "ready") \
    X(T_ARC_MISSING, "missing") \
    X(T_ARC_BAD, "not an archive of this version") \
    X(T_ARC_NOTABLE, "the movie table is missing") \
    X(T_ARC_SUM_READY, "ready (%d files)") \
    X(T_ARC_FILE, "File") \
    X(T_ARC_SIZE, "Size, checked") \
    X(T_ARC_SIZE_V, "%s, %d files") \
    X(T_ARC_SIZE_H, "The archive's header and its size agree. Making it checked each file against the known disc.") \
    X(T_ARC_MISSING_H, "mh2 needs the archive to start the game. Make it from your own copy of the game: a disc image, or a folder with the disc's files.") \
    X(T_ARC_BAD_H, "This file is not a disc archive of this version of mh2. Make it again from your own copy of the game.") \
    X(T_ARC_NOTABLE_H, "The table of the movies' timing, next to the archive, is missing. Making the table takes about a minute.") \
    X(T_MAKE_ISO, "Make it from a disc image...") \
    X(T_MAKE_ISO_AGAIN, "Make it again from a disc image...") \
    X(T_MAKE_DIR, "Make it from a folder...") \
    X(T_MAKE_DIR_AGAIN, "Make it again from a folder...") \
    X(T_MAKE_ISO_H, "Choose the disc image (.iso) of your copy of the game. The archive is written to the file shown above.") \
    X(T_MAKE_DIR_H, "Choose the folder with the disc's files (the one with SYSTEM.CNF in it). The archive is written to the file shown above.") \
    X(T_MAKE_TABLE, "Make the movie table") \
    X(T_MAKE_TABLE_H, "Times the game's movies once, from the archive. About a minute.") \
    X(T_CHOOSE_ARC, "Choose another archive...") \
    X(T_CHOOSE_ARC_H, "Use an archive that is somewhere else. It is saved as the setting archive in mh2.ini.") \
    X(T_PLAY_NOARC, "The game cannot start: the disc archive is missing. Choose Disc and archive on the Home page.") \
    X(T_BANNER, "The disc archive is missing") \
    X(T_BANNER_H, "mh2 cannot start the game without it. Choose Disc and archive to make it from your own copy of the game.") \
    X(T_ARC_BADFILE, "That file is not a disc archive of this version of mh2.") \
    X(T_MAKING, "MAKING THE ARCHIVE") \
    X(T_MK_READING, "Reading   %s") \
    X(T_MK_STAGE_SIZE, "Getting ready...") \
    X(T_MK_FILE_OF, "File %d of %d   %s") \
    X(T_MK_CHECKING, "Checking each file against the known disc.") \
    X(T_MK_MOVIE_OF, "Timing the movies: %d of %d") \
    X(T_MK_MOVIES_NOTE, "The game's movies are timed once, from the archive.") \
    X(T_MK_STEP, "Step %d of 2") \
    X(T_MK_CANCEL, "Cancel") \
    X(T_MK_CANCEL_H, "Stops it and removes the half-written file.") \
    X(T_MK_DONE, "The archive is made") \
    X(T_MK_DONE_V, "%s, %d files, checked") \
    X(T_MK_CONTINUE, "Continue") \
    X(T_MK_CONTINUE_H, "Goes on.") \
    X(T_MK_FAILED, "The archive could not be made") \
    X(T_MK_BADSUM, "%s is not the file of the NTSC-J disc (the check value differs): this is another version or a damaged copy") \
    X(T_MK_SHORT, "%s is shorter than the file of the NTSC-J disc: this is a damaged or cut-off copy") \
    X(T_MK_MISSING, "%s is missing from %s: that folder does not hold the files of the NTSC-J disc") \
    X(T_MK_NODISC, "%.*s holds no disc image (*.iso) and no disc files (SYSTEM.CNF)") \
    X(T_MK_NOTDISC, "%.*s is not a disc image or a folder") \
    X(T_MK_SIZE, "%s is not the size of the NTSC-J disc's file: this is another version or a damaged copy") \
    X(T_MK_NOTISO, "%s is not a disc image: it is not in the ISO 9660 form with 2048-byte sectors, which is how a PS2 disc is copied to an .iso file") \
    X(T_MK_NOTONDISC, "%s is not on that disc image: this is not the NTSC-J disc") \
    X(T_MK_LAYOUT, "%s is not where the NTSC-J disc has it, or not its size: this is another version of the game") \
    X(T_MK_TAIL, "%s ends differently from the file of the NTSC-J disc: this is another version or a damaged copy") \
    X(T_MK_READ, "%s could not be read (%s)") \
    X(T_MK_WRITE, "The archive could not be written in %s (%s). If the disk is full, free some space.") \
    X(T_MK_OPEN, "%s could not be opened (%s)") \
    X(T_MK_OTHER, "The archive could not be made: %s") \
    X(T_MK_NOTABLE, "The archive is made, but its movie table is not: %s") \
    X(T_MK_SPACE, "Not enough free space: the archive needs %s, and the folder %s has %s free. Free some space, or choose another place with archive = FILE in mh2.ini.") \
    X(T_MK_NOSPACE_FOLDER, "There is no folder to put the archive in.") \
    /* the memory cards */ \
    X(T_CARDS_PAGE, "Memory cards") \
    X(T_HEAD_CARDS, "MEMORY CARDS") \
    X(T_PORT1, "Port 1") \
    X(T_PORT2, "Port 2") \
    X(T_PORT1_H, "The card in port 1. Left and right choose among the cards in the cards folder.") \
    X(T_PORT2_H, "The card in port 2 (none by default). Left and right choose among the cards in the cards folder.") \
    X(T_CARD_NONE, "none") \
    X(T_CARD_NEW, "%s (new)") \
    X(T_CARD_INFO, "%s, %s") \
    X(T_CARD_INUSE, "in use") \
    X(T_CARD_ROW_H, "A memory card in the cards folder. mh2 does not open its contents.") \
    X(T_CARD_MORE, "(%d more in the folder)") \
    X(T_NEWCARD, "New card...") \
    X(T_NEWCARD_H, "Makes a card for a port. It is made empty when the game first starts with it.") \
    X(T_IMPORT, "Import the old version's cards...") \
    X(T_IMPORT_H, "Copies the old version's cards into mh2's cards folder. It never overwrites a card, and the old files stay as they are.") \
    X(T_IMPORT_NONE, "(none found)") \
    X(T_IMPORT_NONE_H, "No card of the old version was found (its memcards folder in the user data folder, or memcard1 and memcard2 in mh2.ini).") \
    X(T_IMPORT_DONE, "Copied %d card(s) into the cards folder; %d already there and left alone.") \
    X(T_IMPORT_FAIL, "A card could not be copied: see the log.") \
    X(T_OPEN_CARDS, "Open the cards folder") \
    X(T_OPEN_CARDS_H, "Opens the folder with the memory cards in your file manager.") \
    X(T_NO_CARDS_DIR, "(no folder)") \
    X(T_HEAD_NEWCARD, "NEW MEMORY CARD") \
    X(T_NC_NAME, "Name") \
    X(T_NC_NAME_H, "Type the card's file name. .ps2 is added if you leave it out. Enter goes on.") \
    X(T_NC_PORT, "For port") \
    X(T_NC_PORT_H, "The port the new card goes in.") \
    X(T_NC_MAKE, "Make the card") \
    X(T_NC_MAKE_H, "Puts the card in the port. The empty card is made when the game starts.") \
    X(T_NC_BAD, "%s is not a name for a card: no folders, and not one of the core's own files (.journal, .tmp, .bak, .lock).") \
    X(T_NC_EXISTS, "%s is in the cards folder already. Choose it in the port rows instead.") \
    X(T_NC_FIXED, "Port %d is set on the command line.") \
    /* the first run */ \
    X(T_HEAD_FIRST, "FIRST RUN") \
    X(T_FIRST_STEP, "step %d of 3: %s") \
    X(T_FIRST_1, "Your game files") \
    X(T_FIRST_2, "Making the archive") \
    X(T_FIRST_3, "Your save") \
    X(T_FIRST_1_H, "OpenMH2 needs your own copy of Monster Hunter 2 (Dos), the Japanese PS2 disc (SLPM-66280): its disc image, or the folder with its files.") \
    X(T_FIRST_USE, "Use the %s") \
    X(T_FIRST_USE_ISO, "disc image") \
    X(T_FIRST_USE_DIR, "folder") \
    X(T_FIRST_USE_H, "Found on this computer. One press makes the archive from it.") \
    X(T_FIRST_LATER, "Set up later") \
    X(T_FIRST_LATER_H, "Goes to the Home page. This screen comes back at the next start until the game is set up.") \
    X(T_FIRST_3_H, "Your saved games are kept on a memory card of the game's own. Start a new card, or bring the old version's cards.") \
    X(T_FIRST_NEWCARD, "Start a new memory card") \
    X(T_FIRST_NEWCARD_H, "The game makes an empty card, Mcd001.ps2, in the cards folder at its first start.") \
    X(T_FIRST_IMPORT, "Import your old version's cards") \
    X(T_FIRST_FINISH, "Go to the Home page") \
    /* choices */ \
    X(T_CH_FLAG, "off|on") \
    X(T_CH_VSYNC, "auto|on|off") \
    /* options that are not settings: help */ \
    X(T_O_CONFIG, "Read and edit this settings file, not the usual mh2.ini.") \
    X(T_O_SET, "A settings line over the file's for this start, for example --set ssaa=4 (up to 32).") \
    X(T_O_CORE, "The game's program (default: mh2-core beside mh2).") \
    X(T_O_NOWINDOW, "No window and no launcher (tests, servers).") \
    X(T_O_GSOFF, "No renderer: nothing is drawn.") \
    X(T_O_FAST, "The game runs as fast as it can, not in real time.") \
    X(T_O_NOCARD, "No memory card in port 1.") \
    X(T_O_QUITAFTER, "End after N frames (tests).") \
    X(T_O_IMPORTCARD, "Copy a memory card file into mh2's cards folder, then end.") \
    X(T_O_LOGFILE, "Write the log to this file.") \
    X(T_O_LAUNCHER, "Show the launcher, whatever mh2.ini says.") \
    X(T_O_NOLAUNCHER, "Do not show the launcher.") \
    X(T_O_HELP, "This text.") \
    X(T_O_REST, "Everything after -- goes to the game's program.") \
    X(T_O_MEMCARD1, "A memory card file for port 1 (the core's --memcard1).") \
    X(T_O_MEMCARD2, "A memory card file for port 2 (the core's --memcard2).") \
    X(T_O_ARCHIVE, "The game's disc archive.")


enum {
#define X(id, s) id,
    LTEXTS(X)
#undef X
    T_COUNT
};

extern const char *const g_text[T_COUNT];
#define TX(id) g_text[id]

#endif
