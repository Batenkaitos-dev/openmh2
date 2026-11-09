# Making a translation pack

A translation pack changes the words the game shows: the quests' texts, the townspeople's lines, the menus and the help. The
OpenMH2 ships no translation. It supplies the tools that make a pack, and this guide is for the people who use them. The
choice of words, and the knowledge of the language they are in, are the author's.

A pack is a folder. The game reads it in place of its own text, and nothing in the game's files or in your saves is changed.
A save state made with a pack loads only if the same pack is chosen again (section 8).

## 1. What a pack can change

| The text | Where it lives | How a pack replaces it |
|---|---|---|
| The townspeople's lines (13 dialogue tables), the quests' titles, goals, messages and notices (1,968 quest files) | files of the game's disc | `out`, then `in` (section 3.1) |
| The menus, the help texts, the names of items and monsters that the game's code draws (about 10,900 strings) | the game's code | `strings`, then `table` (section 3.2) |
| The letters a language needs and the game's font lacks (accents, `¿`, `ß`) | the font file | `glyphs`, then `font` (section 3.3) |

What a pack **cannot** change:

- **Text that is an image** (a title drawn as a texture, a sign): use a texture pack for those.
- **Text that players type**: names and chat come from the keyboard, which has no accents.
- **Strings the game fills in with a float or a code of its own** (`%6.1f`, `%a`, `%y`, `%1`): the table cannot take them. The
  `strings` command lists them at the end of its file, in comments.
- **The game's rules.** A pack changes what is drawn. Where the game measures a string to centre it or size a box, it measures
  the original, so a translated title sits as if it were as long as the Japanese one.

A Latin letter is half as wide as a Japanese character, so most translations fit the game's boxes, but the game has no word
wrapping: **the author breaks lines by hand** with `\n`.

## 2. What you need

- **Your own copy of the game**, as the launcher uses it: either the folder that holds `DATA.BIN` (`--disc FOLDER`) or the disc
  archive the launcher made (`--archive FILE`). Nothing of the game is ever written into a pack except the files that `in` and
  `font` make, which hold the game's own data (see section 6 about sharing).
- **Python 3.** The tools are one script and two files beside it, `tools/pc/textpack.py`, `textpack_table.py` and
  `textpack_font.py`; they use the standard library only. A release download carries them in `kit/src/tools/pc` and a Python of its own
  that runs them (`kit/python/bin/python3` on Linux, `kit\python\python.exe` on Windows).
- **For the game's own strings**, the folder of generated modules that the build makes from your copy (`build/gen`, made by
  `content_gen.py`; the player's guide, section 2).
- **For the font**, a TrueType font file you may use. `pc/host/fonts/NotoSans-Regular.ttf` (SIL Open Font License, copied with its
  licence) is in the repository; a release download has no font file, and any `.ttf` with outlines you may use works.
- **A text editor that saves UTF-8** and does not change tabs into spaces.

Every command prints what it wrote. All of them are run from OpenMH2's folder; `PACK` is the folder you are making.

## 3. Making a pack, step by step

### 3.0 Decide the letters

If your language needs letters that are not ASCII, write a **character map**, `charmap.txt`: UTF-8, one character on each line,
at most 63 lines. The first line is the letter the game will draw for the byte 0xA1, the second for 0xA2, and so on; an empty
line leaves a byte alone. A character may appear once.

```
é
è
à
ñ
```

The game draws these bytes with narrow glyphs it reserves for half-width katakana, so **while the pack is on, the half-width
katakana of the on-screen keyboard show as your letters.** Capital and small letters each take a line.

### 3.1 The disc's text (dialogue and quests)

```
python3 tools/pc/textpack.py out --archive ARCHIVE --to work/text --charmap charmap.txt
```

writes `work/text/talk/NNNNN.txt` for each dialogue table and `work/text/quest/NNNNN.txt` for each quest file. A line is

```
KEY<TAB>TEXT
```

and a line that starts with `#` is a comment. The keys are the game's: `script.line` in a dialogue table (the script is the
townsperson's number), and `title`, `goal`, `suba`, `subb`, `clear`, `fail`, `contractor`, `desc` and `notice0`, `notice1`, ...
in a quest file. These names are OpenMH2's. **Change the text after the tab only**, in the files you translate. The files you
leave alone, and the lines you delete, stay as the game has them.

Write the text with these escapes: `\\` a backslash, `\n` a line feed (the game's line break), `\t`, `\r`, and `\xNN` a byte as
it is. The game's own codes stay as they are: `~Cnn` a colour, `~Ann` a number, `~Bnn` an icon, `~%`, `~~`, and the `%`
conversions of printf. Japanese characters you keep are written as they are (UTF-8).

Then make the game's files:

```
python3 tools/pc/textpack.py in --archive ARCHIVE --from work/text --pack PACK --name "My pack" --charmap charmap.txt
```

`in` writes, into `PACK/files/DATA.BIN/...`, a file for each of the game's files whose text you changed, in the game's own
format (the dialogue tables compressed as the disc compresses them), and `PACK/pack.ini`. **A dialogue table may not grow**: the
game's buffer is as big as the original. The tool refuses a longer one and says by how much; shorten some lines.

### 3.2 The game's own strings

In a release, the kit's first build has already written this list for you: core/strings.tsv in your data folder.
From a development tree:

```
python3 tools/pc/textpack.py strings --gen build/gen --to work/strings.tsv
```

writes one line for every string of the game that has Japanese in it (about 10,900; `--all` adds strings with English words):

```
KEY<TAB>SIG<TAB>ORIGINAL<TAB>TEXT
```

- `KEY` is the first 8 bytes of the string's SHA-256, as 16 hex digits. The game finds your replacement by it.
- `SIG` is the string's `%` conversions in order (`%d%-8s`), or `-` for none.
- `ORIGINAL` is the Japanese, to translate from.
- `TEXT` is empty. **Write your translation there** in the strings you translate, and leave the others empty. Do not change the
  first three columns.

Your `TEXT` must have **the same `%` conversions as `SIG`, in the same order**: the game hands the original's numbers and names
to the string. `%d pieces of %-8s` becomes `%d pièces de %-8s`, not `%s pièces`. Colour codes and icons (`~C05`, `~B07`) are yours
to place. Where the game puts a number or a name into a string before drawing it, the string is looked up after that, so
translate those lines in the dialogue table instead.

```
python3 tools/pc/textpack.py table --from work/strings.tsv --pack PACK --charmap charmap.txt
```

reads your file and writes `PACK/text.tsv`, for the strings whose `TEXT` you filled in, checking each as the game checks it. **One
fault and nothing is written**; each fault is listed with its line in `strings.tsv`. A Japanese character that you kept is
written as its bytes (`\x82\xa0`), which the game wants, and your letters stay as they are.

### 3.3 The letters

```
python3 tools/pc/textpack.py glyphs --ttf pc/host/fonts/NotoSans-Regular.ttf --charmap charmap.txt --to PACK/glyphs.txt --preview work/glyphs.png
```

draws each letter of your map in the game's proportions (the game's text is monospaced; a letter's ink is up to 10 pixels wide,
capitals 15 high) and writes them into `PACK/glyphs.txt`: for each letter a header line and 20 rows of 12 characters,

```
glyph 0xA1 U+00E9 é
.....-+-....
```

where `.` is no ink, `-` light, `+` heavy and `#` full. **Open it in an editor and redraw any letter you want to improve.**
`--preview` writes an image of all of them. `--weight` makes the strokes heavier or lighter (the game's are about 1.5).

```
python3 tools/pc/textpack.py font --archive ARCHIVE --pack PACK
```

puts your letter images into the game's own font file and writes `PACK/files/DATA.BIN/00000/05906`.

### 3.4 Check it

```
python3 tools/pc/textpack.py check PACK --archive ARCHIVE
```

applies every rule the game applies to a pack, without starting the game, and says each fault it finds (the game stops at the
first). Without `--archive` or `--disc` it does everything except hold the disc files to the original's size. Section 5 lists the
messages.

### 3.5 Use it

Put the folder in `text_packs` in the user data folder (the player's guide, "Where things are"), and choose it:

- `text_pack = NAME` in `mh2.ini` (`NAME` is the folder's name; a path also works), or
- `--text-pack NAME` on the command line.

The launcher does not list or choose translation packs (its Content page is for texture packs).

`text_pack = off` is the default. A pack that cannot be used stops the game before it starts, with the reason.

## 4. The files of a pack

```
PACK/
  pack.ini        required: the pack's name and settings
  charmap.txt     the character map (or the file pack.ini names)
  text.tsv        the game's own strings (section 3.2)
  glyphs.txt      the images of the letters, which `font` puts into the game's font
  files/          the game's own files, replaced: what `in` and `font` write
    DATA.BIN/00000/000NN            a dialogue table, NN from 44 to 56
    DATA.BIN/00000/06586/NNNNN      a quest file
    DATA.BIN/00000/05906            the font
```

Every file is optional but `pack.ini`. A pack with only a `text.tsv` changes the game's own strings; a pack with only `files/`
changes only what they hold.

**pack.ini** is UTF-8, one `key = value` a line; blanks around both are dropped; a line that starts with `#` or `;` is a comment.

| Key | | |
|---|---|---|
| `name` | required, 1 to 64 bytes | the pack's name in the game's messages about it |
| `language` | up to 16 bytes | a tag for the player (`en`, `pt-BR`) |
| `version` | up to 32 bytes | |
| `charmap` | a file name in the pack's folder | the character map, when it is not `charmap.txt` |

**text.tsv** is UTF-8, a blank line or one that starts with `#` is a comment, and every other line is

```
KEY<TAB>SIG<TAB>TEXT
```

The text is ASCII, the letters of the character map, and the escapes `\\ \n \t \r \xNN`. `\xNN` takes two **lower-case** hex
digits. A `~` must be `~~`, `~%` or `~Cnn`, `~Ann`, `~Bnn` with two digits. The text may not reach 1,024 bytes. The pack is
refused as a whole for one fault.

## 5. What the checker says

`check` prints `error:` for what the game would refuse and `warning:` for what it would accept but you probably did not mean.
The sentences are the game's own where the game has the same rule. The exit status is 1 when there is an error.

### pack.ini

| Message | What it means |
|---|---|
| the folder X doesn't exist or can't be read | the path is not a folder |
| the folder X has no pack.ini that can be read (a translation pack holds one) | add a `pack.ini` with at least `name = ...` |
| the pack.ini of X is over 16 KB | |
| pack.ini, line N: a NUL byte | the file is not text |
| pack.ini, line N: no = (a setting is key = value) | |
| pack.ini, line N: the key is empty or too long | |
| pack.ini, line N: K is not a key of a pack.ini (name, language, version, charmap) | a misspelt key |
| pack.ini, line N: K is given twice | |
| pack.ini, line N: K has no value | |
| pack.ini, line N: K is longer than M bytes | name 64, language 16, version 32, charmap 64 |
| pack.ini, line N: K holds a control character | |
| pack.ini, line N: K is not UTF-8 | save the file as UTF-8 |
| pack.ini, line N: charmap is a file name in the pack's folder (no folder, no leading dot) | |
| pack.ini has no name | |

### The character map

| Message | What it means |
|---|---|
| the character map F that pack.ini names is not in X | the file is missing |
| the character map F is over 8 KB | |
| the character map F has more than 63 lines (the bytes 0xA1 to 0xDF) | there are 63 bytes to use |
| the character map F, line N: one UTF-8 character a line | two characters on a line, or bytes that are not UTF-8 |
| the character map F, line N: the character is already on line M | |

### text.tsv

Each begins `text.tsv, line N:`.

| Message | What it means |
|---|---|
| the line is not KEY (16 hex digits), a tab, SIG, a tab, and the text | a missing tab, or a key of the wrong length |
| the key is not 16 lower-case hex digits | |
| the key is given twice | |
| a backslash ends the text | write `\\` for a backslash |
| \x00 would end the string | |
| an escape that is not \\, \n, \t, \r or \xNN (two lower-case hex digits) | `\xA0` is `\xa0` |
| a control character (write \n or \xNN) | |
| bytes that are not UTF-8 | |
| the character U+XXXX is not in the character map (a Japanese character is written as its Shift-JIS bytes, \xNN) | add the letter to the map, or write the Japanese character's bytes |
| a ~ that is not ~~, ~%, ~Cnn, ~Ann or ~Bnn (nn two digits) | |
| a % that is not a conversion (diouxXcsp, or %%) | write `%%` for a percent sign |
| too many conversions | |
| the replacement's conversions are "...", and the original's (SIG) are not the same | the numbers and names the game hands over must be there, in order |
| the text is N bytes; the game's text buffer holds 1023 | shorten it |

### Files

| Message | What it means |
|---|---|
| the file X can't be read | |
| files/X: not in the game's compressed format | a dialogue table must be compressed; `in` does it |
| files/X: not a dialogue table: why | the table's layout is wrong |
| files/X: the table is N bytes, M over the limit of K (the original's size) | a dialogue table may not grow |
| files/X: not a quest file: why | |
| files/X: the quest file is N bytes, M over the limit of 32768 (the stage buffer) | |
| files/X: the quest archive has no entry N | |
| files/X: the font is N bytes, and must be 780800 (7,808 glyphs of 100 bytes) | |

Warnings: a file the game would not find (a name that is not upper case, 95 characters or more); an empty file (ignored); a file
the checker has no rule for; a dialogue table whose limit was not checked (no `--archive` or `--disc`); a letter of the map with no
glyph in `glyphs.txt`; letters with no font file.

## 6. Sharing a pack

`PACK/files/` holds the game's own data: every line you did not change of the tables you changed, and the glyphs of the font that you did not draw.
**Do not publish it, and do not publish the files of `out`**, which hold the game's text. What is yours is

- `pack.ini`, `charmap.txt`, `text.tsv` and `glyphs.txt`, which hold only your work; and
- the lines you changed in the dialogue and quest files, which `changes` copies:

```
python3 tools/pc/textpack.py changes --archive ARCHIVE --from work/text --to SOURCE/ --charmap charmap.txt
```

Put those four files in `SOURCE/` beside the `talk/` and `quest/` folders that `changes` made, and publish that folder.
Whoever has the game makes the pack from it:

```
python3 tools/pc/textpack.py build --archive ARCHIVE --from SOURCE --pack PACK
```

`build` copies the files that are yours, makes `files/` from the game's own, and runs the checker.

## 7. The limits

- A dialogue table's size may not grow; a quest file may not pass 32,768 bytes.
- A string is 1,023 bytes or less.
- A character map has 63 letters. The letters share the bytes of the half-width katakana.
- The fixed boxes of the game are the original's: a Latin letter is half a Japanese character wide, so the text of a box often
  fits, but there is no wrapping. Where a title is centred, it is centred as the original.
- The game's fonts are monospaced; a letter is 10 pixels wide at most.
- Strings that contain a float or one of the game's own codes cannot be replaced.
- Names and chat typed on the game's keyboard stay ASCII and katakana.
- The tools read the text of the game version this port runs.

## 8. Save states

A save state made with a pack remembers it by its hash, which covers every file of the folder, so **changing any file changes
the pack**: the state then loads only with the same bytes. A state made without a pack loads only without one. This keeps a
loaded state exact; it is not a restriction on play.
