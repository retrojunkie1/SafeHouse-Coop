# Modding Sly 1 and Sly 2 in SafeHouse

SafeHouse installs each game from your own disc into a folder of ordinary files. This guide covers where those files
are, how to replace textures, music, dialog, text, movies and levels, which tools come with SafeHouse, and what is
safe to change.

Both games have a **Mods folder**: you drop replacement files into it, and the game uses them instead of the
originals. The installation itself is never touched, so removing a file from Mods brings the original back.

---

## Sly 2: Band of Thieves

### Where things are

| What | Linux | Windows | Portable SafeHouse |
|---|---|---|---|
| The installation (read only for mods) | `~/.local/share/projectcane/installations/sly2` | `%LOCALAPPDATA%\ProjectCane\installations\sly2` | `data/installations/sly2` |
| **Your Mods folder** | `~/.config/projectcane/sly2/Mods` | `%LOCALAPPDATA%\ProjectCane\user\sly2\Mods` | `data/user/sly2/Mods` |

- A second region, such as Europe installed beside USA, is in `installations/sly2-europe`. The Mods folder is shared
  by both.
- SafeHouse's **Open game folder** button opens the installation.
- Create the `Mods` folder if it doesn't exist.
- When the game starts with mods, its log prints `mods: N files from ...`.

The installation looks like this:

```text
sly2/
  extraction_manifest.json        the index: every file, what it is, and which levels use it
  Worlds/<N> <Episode>/<Level>/
      <Level>.brx                 the level
      <Level>.bnk                 its sound effects' definitions
      Sounds/Sound <n>.wav        its sound effects, one WAV per sample
      Ambience.wav                its background ambience
      Screens/Screen <id>.brx     a menu or HUD screen only this level uses
  Worlds/Front End/Front End.brx  the title screen and menus
  Screens/Screen <id>.brx         HUD screens every level uses
  Music/<id> <name>.wav           the music, for example "28 Paris.wav"
  Dialog/<Language>/<Level>/<line>.wav   a voice line ("Shared" holds lines every level uses)
  Movies/<id> <name>.m2v          a movie's picture, for example "2009 Ep1 Intro.m2v"
  Movies/<id> <name>.wav          its sound
```

The audio is ordinary WAV (16-bit PCM), which any audio editor opens. On the European discs, with 11 languages, the
voice lines and the movies' sound are **FLAC** instead (lossless, about 60 % of the size; Audacity and most editors
open it): `Dialog/French/Paris/1259.flac`, `Movies/French/2062 Ep1 Intro.flac`. The game plays exactly the sound
of the disc from these files.

Examples: `Worlds/1 The Black Chateau/Paris/Paris.brx`, `Music/18 The Jungle.wav`,
`Movies/2013 Ep2 Operation Hippo Drop.m2v`.

**The rule: a file in Mods at the same path as a file of the installation replaces it.** For example,
`Mods/Music/28 Paris.wav` replaces `Music/28 Paris.wav`.

Music and movies also match by their **number** alone:

- `Mods/Music/28.wav` or `Mods/Music/28 My Theme.wav` replaces track 28.
- `Mods/Movies/2009.m2v` replaces movie 2009's picture, `Mods/Movies/2009.wav` its sound.

The PS2 formats of older SafeHouse versions still work in Mods: a `.vpk` replaces a music track, a `.vag` a voice
line, a `.pss` a whole movie (picture and sound).

Use the number when you share a mod: names can change between SafeHouse versions, numbers don't. Movie numbers
differ between the USA and European discs: the European Ep 1 intro is 2062. Music numbers are the same on both.

### The tools

The tools are options of the game program **Sly2-Release** (`Sly2-Release.exe` on Windows), which is in SafeHouse's
program folder. Run them from a terminal.

**The audio tools need no game data:**

```sh
Sly2-Release --export-wav "<installation>/Music/28 Paris.wav" paris-normal.wav --stem 1
Sly2-Release --make-music "Mods/Music/28 Paris.wav" mytrack.wav --like "<installation>/Music/28 Paris.wav"
```

Voice lines, movie sound and sound effects need no tool: put a WAV in Mods.

**The dumps need the installation** (`--disc` takes the installation folder):

```sh
Sly2-Release --disc "<installation>" --dump-text text
Sly2-Release --disc "<installation>" --dump-textures textures 2
```

`--dump-textures` takes a level by its record number, as listed in `extraction_manifest.json` under `worlds`:
2 is Paris, `boot` is the museum prologue, `frontend` is the menus.

To try a different Mods folder, use `--mods DIR`. An empty value (`--mods ""`) starts the game with no mods.

### Textures

1. Find the texture. **The installation already has every texture:** the `Textures` folder in the game folder (the
   launcher's **Open game folder**) holds all 7051 of them as `<W>x<H>_<hash>.png` (about 80 MB). The name
   identifies the texture by its contents.

   - Installed before 2026-11-04 and no `Textures` folder? Press **Export textures** on the launcher's Sly 2 page
     (about 10 seconds), or run `Sly2-Release --data-dir "<installation>" --export-textures`. No re-extract needed.
   - Only one level's textures: `Sly2-Release --disc "<installation>" --dump-textures dump 2` writes what Paris
     draws to `dump/`.

2. **Copy** the PNG into `Mods/Textures/` under the **same file name**, and edit the copy. (Leave `Textures`
   itself alone: it is the reference, and the game never reads it.)

The texture is then replaced wherever the game draws it: in every level, on characters, on HUD screens, in
animated (multi-frame) textures and in particles.

- **Higher resolution:** a replacement is used **at its own resolution, not stretched or scaled back down**. The
  game's texture coordinates run 0..1 across the texture, so a 4x PNG (a 64x64 texture as 256x256) maps exactly like
  the original and shows four times the detail. The width and height don't need to be the original's multiple or
  aspect, but keeping them a whole multiple (2x, 4x) keeps every detail where the original had it.
- **Filtering:** each replacement gets mipmaps, so a large texture doesn't shimmer in the distance. Textures the
  game draws smooth are drawn smooth (trilinear, with anisotropic filtering), and textures it draws with sharp
  pixels stay sharp.
- **Recommended limits:** 4x the original (most textures become 128x128 to 1024x1024; the largest, 512x128, becomes
  2048x512) and at most 4096 pixels a side. A whole level at 4x uses about 300 to 550 MB of video memory in place of
  about 25 MB, and loads about half a second slower. Only the current level's textures are kept. On the Steam Deck
  or a 2 GB card, use 2x for whole-level packs and save 4x for close-up details. 8x and above costs 4x as much again,
  for detail the camera rarely gets close enough to see. Replacements over 16384 pixels or over the GPU's limit are
  ignored.
- **Alpha:** 255 is fully opaque. The game's own range is half that, and SafeHouse converts it both ways, so a
  dumped PNG put back unchanged looks identical. Upscaled edges with in-between alpha are fine.
- **Shared textures:** a texture used by several levels has the same name in each, so one PNG covers all of them.
- **Animated textures and particles:** each frame is its own PNG. Give all the frames of one animation the same
  size (particle frames of another size are scaled to the first replaced frame's size).
- Start the game with `SLY2N_MODSTATS=1` set to have its log list each replacement it uploads and the video memory
  they use.
- **Converting by hand:** `Sly2-Release --disc "Sly 2.iso" --convert sly2-files` writes the `Textures` folder too;
  `--no-export-textures` leaves it out.

### The PS3 HD textures (The Sly Collection)

If you own **The Sly Collection** for PS3, SafeHouse can take its redrawn textures (up to 4x sharper) and use them
over Sly 2's: **Settings > Games > Sly 2: Band of Thieves > Import PS3 HD textures**. Choose the PS3 disc image
(`.iso`), a `.zip` of it (read as it is, nothing is unpacked), or the extracted disc (the folder with `PS3_GAME`).
It takes about 6 minutes and writes about 200 MB (5989 textures) to `PS3 HD/Textures` in your Sly 2 profile folder
(`~/.config/projectcane/sly2/PS3 HD`, `%LOCALAPPDATA%\ProjectCane\user\sly2\PS3 HD`).

- The pack is a texture pack like any other: the same `<W>x<H>_<hash>.png` names. Your own `Mods/Textures` win over
  it, so you can still change single textures.
- **Use the PS3 HD textures** turns it on or off; **Remove** deletes it. The game itself never reads the PS3 disc:
  levels, scripts and gameplay stay the PS2's.
- Without the launcher: `Sly2-Release --data-dir "<installation>" --ps3-hd-textures "<PS3 disc, .iso or .zip>" OUT`
  builds the pack, and `--texture-pack OUT` uses it.
- Only the textures are taken: the PS3's cutscenes, music and voices are the same quality as the PS2's.

### Music

Each music track is one WAV. Most free-roam tracks have three **stems** the game cross-fades:

- **sneak** (mono), when Sly is sneaking;
- **normal** (stereo);
- **alert** (stereo), when guards spot you.

The WAV has a channel for each: Paris's five channels are sneak, normal left, normal right, alert left, alert right.
Battles, chases and menus have one plain stereo stem. The WAV also says where the loop starts (its `smpl` chunk,
which samplers and many editors show as a loop marker) and which channel is which stem (a `stem` chunk).

**The simplest replacement** is any WAV, for example a stereo song at `Mods/Music/28.wav`. It plays the same whether
you sneak or get spotted, and loops whole.

**With stems and a loop point**, build the track with `--make-music`:

```sh
Sly2-Release --make-music "Mods/Music/28 Paris.wav" normal.wav \
    --like "<installation>/Music/28 Paris.wav" --sneak sneak.wav --alert alert.wav --loop 14.9
```

- **`--like`** copies the original's stem layout, so the game's mixing keeps working. Always use it with the
  track you replace. Without `--like` you get one plain stereo stem.
- **Stems:** `--sneak` and `--alert` are optional. Without them, the normal WAV is used for every stem, which
  means your track plays the same whether you sneak or get spotted.
- **`--loop SECONDS`** is where the music jumps back to after the end. The default, 0, loops the whole file.
  `--loop none` plays the track once. The loop is sample-exact.
- **WAV formats:** 16-bit or 24-bit PCM or 32-bit float, mono or stereo. 44100 Hz is what the game uses. Other rates
  play too.
- **Getting the original:** `--export-wav` gives one stem of the original as a WAV, with its loop point: `--stem 1`
  the normal one (the default), `--stem 0` the sneak one, `--stem 2` the alert one.
- An edited copy of an installed track that lost its `stem` chunk (some editors drop unknown chunks) still mixes
  right when it keeps the original's channel count: the game takes the stems from the installed track.

Each level's background ambience (`Ambience.wav`) works the same way.

Track numbers and where they play are in the table below.

| # | Track | # | Track |
|---:|---|---:|---|
| 36 | Museum (prologue) | 34, 35 | Episode menus |
| 28 | Paris (Ep 1 hub) | 29 | Wine Cellar (Ep 1 interiors) |
| 23 | India (Ep 2 hub) | 24 | Guest Rooms (Ep 2 interiors) |
| 18 | The Jungle (Ep 3 hub) | 19 | Spice Factory |
| 14 | Prague (Ep 4 hub) | 15 | Prison |
| 10 | Contessa's Castle (Ep 5 hub) | 11 | The Crypt (Ep 5 interiors) |
| 7 | Train Station (Ep 6 hub) | 8 | Cabins (the train) |
| 3, 4 | Sawmill hub, Sawmill interiors (Ep 7) | 6 | Jean Bison Battle |
| 0 | Arpeggio's Airship (Ep 8 hub) | 1 | Clockwerk Assembly (blimp interiors) |
| 2 | Clock-La Battle | | others: jobs, chases and bosses, named by place |

### Dialog

Voice lines are mono WAV files (22050 Hz, 16-bit):

- `Dialog/<Language>/<Level>/<line>.wav`: a line one level uses;
- `Dialog/<Language>/Shared/<line>.wav`: a line every level uses.

To replace one:

1. Find the line: listen to the WAVs. The manifest lists every line with its level and length.
2. Record your line and save it as a WAV, ideally 22050 Hz mono, like the game's. Any rate, mono or stereo, plays.
3. Put it at the same path in Mods: `Mods/Dialog/English/Paris/1259.wav`.

The subtitle's timing and the lip sync come from the level, not the audio, so keep your line about as long as the
original. The manifest's `seconds` field gives the original length.

On European installations every line exists once per language, as FLAC: `Dialog/French/Paris/1259.flac`. A WAV or a
FLAC at that path (`Mods/Dialog/French/Paris/1259.wav` works too) replaces it.

### Text and subtitles

All on-screen text (menus, HUD, job names, hints, subtitles) can be replaced per language with
`Mods/Text/<Language>.json`:

1. Dump the current strings. This writes one file per language on your disc (`English.json`; on European discs also
   `French.json`, `German.json` ...):

   ```sh
   Sly2-Release --disc "<installation>" --dump-text text
   ```

2. Copy the strings you want to change into your own file:

   ```json
   {
     "strings": {
       "5": "Press START button for a New Heist",
       "305": "Press &2X&. to play as Sly."
     },
     "worlds": {
       "Paris": { "306": "This line only changes in Paris." }
     }
   }
   ```

3. Save it as `Mods/Text/English.json` (or `French.json`, ...).

How the file works:

- **`strings`** applies everywhere; **`worlds`**, keyed by the level's name as in the installation's folders, applies
  only in that level and wins over `strings`.
- **Keys** are the string numbers from the dump. You only need the strings you change. You can't add new numbers.
- **Encoding:** write the file as UTF-8. The USA and European fonts have the Western European (Latin-1)
  characters, such as é, ß, ñ and å; other characters show as `?`. On the Japanese disc (`Japanese.json`), only
  characters the game's Japanese font has can show.
- **Button icons:** sequences like `&2X&.` (the X button), `&2T&.` (Triangle) and `&2S&.` (Square) draw the game's
  button icons. Keep them.
- **Comments:** `//` comment lines are allowed in the file.

A translation is a full `<Language>.json` with every string changed.

### Movies

Each movie is two files, like Sly 1's cutscenes: `Movies/<number> <name>.m2v`, the picture (MPEG-2 video, 640x480,
29.97 fps), and `Movies/<number> <name>.wav`, its sound (16-bit stereo). On European discs the sound is one FLAC per
language: `Movies/French/2062 Ep1 Intro.flac`.

Put your versions in Mods under the movie's number, `Mods/Movies/2009.m2v` and `Mods/Movies/2009.wav` (European
sound: `Mods/Movies/French/2062.wav`). Each replaces its own part, so you can change only the sound. The picture is
timed by the sound: keep the two the same length. With ffmpeg, for example:

```sh
ffmpeg -i myvideo.mp4 -an -c:v mpeg2video -q:v 2 -s 640x480 -r 30000/1001 -f mpeg2video Mods/Movies/2009.m2v
ffmpeg -i myvideo.mp4 -vn -c:a pcm_s16le -ar 48000 -ac 2 Mods/Movies/2009.wav
```

A Sony PSS file (the PS2 format) at `Mods/Movies/2009.pss` still replaces the whole movie.

The movie names say what each one is:

| Name | What it is |
|---|---|
| `Prologue` | the game's opening |
| `Ep<N> Intro` | the cartoon opening of each episode |
| `Ep<N> Briefing 1`, `2` | Bentley's chalk-talk plans during the episode |
| `Ep<N> Operation ...` | the briefing for the episode's big operation |
| `Ep<N> Ending` | the episode's closing cartoon |
| `Credits` | the end credits |
| `Extra ...`, `Logo ...` | the extras and the boot logos |

### Sound effects

Every sound effect sample is a WAV in the level's `Sounds` folder: `Worlds/1 The Black Chateau/Paris/Sounds/Sound
021.wav`. A WAV at the same path in Mods replaces that sample. The samples are named after the first sound of the
level that plays them (`Sound 021.wav`; a sound with several samples has `Sound 021-2.wav`, `-3` ...).
`extraction_manifest.json` lists, for each bank, every sample with the sounds that use it: one sample often serves
several sounds (footsteps on different floors, say), and each level has its own copy.

- **Format:** the originals are mono, 48000 Hz. Any rate plays; the game converts it to 48000 Hz, and a sample plays
  at its own speed at the sound's normal pitch (the game raises or lowers some sounds' pitch, as it does the
  original's). Stereo is mixed to mono.
- **Loops:** sounds that loop (engines, alarms) have a loop marker (a `smpl` chunk). Your WAV loops only if it has
  one too; a WAV without one plays once.
- The volume, pitch and timing of each sound come from the level's bank (`<Level>.bnk`), not the WAV.

A whole bank in the 989snd format at `Mods/Worlds/.../<Level>.bnk` replaces the level's definitions and plays its own
samples. SafeHouse has no tool for banks, and every sound the level asks for must still be in the bank.

### Levels

A level file (`<Level>.brx`) is Sly 2's own level format, uncompressed. A modified one in
`Mods/Worlds/<N> <Episode>/<Level>/<Level>.brx` loads instead of the original.

This is the advanced end of modding:

- the format is only partly documented;
- the game expects every object it refers to by number to still exist;
- saves and jobs refer to the level's objects and tasks.

Start from small changes, and test with a fresh save.

### What is safe to change

| Change | Safe? |
|---|---|
| Textures, any size | Yes |
| Music, any length (keep the stem layout with `--make-music --like`) | Yes |
| Dialog audio | Yes. The timing of subtitles and lip sync stays the original's. |
| Text and subtitles | Yes. Very long strings may not fit their box. |
| Movies, any length | Yes: an M2V and a WAV of the same length |
| Sound effect samples | Yes |
| Sound effect banks | With care: every sound the level uses must be present |
| Level files | Advanced. Can break jobs and saves. |
| The installation folder itself | No. Re-extracting or reinstalling replaces it, and Verify reports changed files. Use Mods. |

**Saves:**

- Saves don't store anything from mods, so adding or removing a mod never harms a save. Level mods are the
  exception if they change jobs or tasks.
- Saves are shared between regions, so the same Mods folder and saves work with USA and Europe.

---

## Sly 1: Sly Cooper and the Thievius Raccoonus

### Where things are

| What | Linux | Windows | Portable SafeHouse |
|---|---|---|---|
| The installation (read only for mods) | `~/.local/share/projectcane/installations/sly1` | `%LOCALAPPDATA%\ProjectCane\installations\sly1` | `data/installations/sly1` |
| **Your Mods folder** | `~/.config/projectcane/Mods` | `%LOCALAPPDATA%\ProjectCane\user\Mods` | `data/user/Mods` |

- A European, Japanese or Korean installation is in `installations/sly1-europe` (`-japan`, `-korea`). The Mods
  folder is shared by all of them.
- Sly 1's Mods folder sits directly in your SafeHouse profile, not in a `sly1` subfolder: Sly 1 keeps its saves
  there too.
- Create the `Mods` folder if it doesn't exist.
- When the game starts, its log prints `mods: N files, N textures, N text files from ...`. Each replaced file is
  printed once, the first time the game uses it, as `mods: using ...`.

The installation looks like this:

```text
sly1/
  extraction_manifest.json                  region, languages, level format (PCBRX 19)
  Worlds/<Episode>/<Level>/
      <Level>.brx                           the level (PCBRX: SafeHouse's rebuilt level format)
      Dialog/VAG_<id>.wav                   its voice lines (USA)
      <Language>/Dialogs/VAG_<id>_<n>.wav   its voice lines (Europe, Japan, Korea: one folder per language)
      <Language>/Commentary/...             the bonus commentary (Europe, Japan, Korea)
  Worlds/Paris, Worlds/The Hideout, Worlds/Splash   the levels outside the episodes
  Worlds/Music/MUSID_<id>.mus               the music; MusicMap.txt says which level plays which
  Worlds/Sound Banks/<World>.bnk            the sound effects
  Worlds/Sounds/                            commentary, Jo ambience and prompt sounds (USA)
  Cutscenes/Cutscene_<NN>.m2v               the cutscenes' video
  Cutscenes/Cutscene_<NN>.wav               their sound (Europe: Cutscenes/<Language>/Cutscene_<NN>.wav)
```

The episode folders are `Tide of Terror`, `Sunset Snake Eyes`, `Vicious Voodoo`, `Fire in the sky` and
`The Cold Heart of Hate`. Example: `Worlds/Tide of Terror/A Stealthy Approach/A Stealthy Approach.brx`.

**The rule: a file in Mods at the same path as a file of the installation replaces it.** For example,
`Mods/Worlds/Music/MUSID_51.mus` replaces `Worlds/Music/MUSID_51.mus`, the music of A Stealthy Approach. Upper and
lower case don't matter. A file can only replace one the installation has; Mods can't add new ones.

### The tools

The tools are options of the game program **Sly1-Release** (`Sly1-Release.exe` on Windows), which is in
SafeHouse's program folder. Run them from a terminal.

**The music tools need no game data:**

```sh
Sly1-Release --make-mus "Mods/Worlds/Music/MUSID_51.mus" mytrack.wav --loop 14.9
Sly1-Release --export-wav "<installation>/Worlds/Music/MUSID_51.mus" original.wav --seconds 120
```

**The dumps need the installation** (`--data-dir` takes the installation folder):

```sh
Sly1-Release --data-dir "<installation>" --dump-text text
Sly1-Release --data-dir "<installation>" --dump-textures textures "A Stealthy Approach"
```

To try a different Mods folder, use `--mods DIR`, or set the environment variable `SLY1_MODS`. An empty value
(`--mods ""`) starts the game with no mods.

### Textures

1. Dump a level's textures:

   ```sh
   Sly1-Release --data-dir "<installation>" --dump-textures dump "A Stealthy Approach"
   ```

   The game starts, goes to that level, writes every texture it loaded as `dump/<W>x<H>_<hash>.png`, and quits.
   The name identifies the texture by its contents. The level name is the level's folder name.

   Without a level name, the game runs normally and writes every texture it loads while you play, so you can
   collect the textures of a whole run. Files already in the folder are kept.

2. Edit the PNG. It can be any size: a 64x64 texture can come back as 256x256.
3. Put it in `Mods/Textures/` under the **same file name**.

The texture is then replaced wherever the game draws it, in every level and on the HUD.

- **Higher resolution:** as in Sly 2, a replacement is used **at its own resolution, not stretched**: the texture
  coordinates are normalized, so a 4x PNG maps like the original and shows 4x the detail. World textures get
  mipmaps with trilinear filtering (and the anisotropic filtering setting); fonts and HUD pieces, which have no
  mipmaps in the game either, stay single-level. Same recommended limits as Sly 2: 4x, at most 4096 pixels a side,
  16384 at the most.
- **No dump at install:** unlike Sly 2, Sly 1's textures are inside its rebuilt levels and only decoded by the game
  as a level loads, so the installation has no `Textures` folder. Dump the levels you want to change as above.

- **Shared textures:** a texture used by several levels has the same name in each, so one PNG covers all of them.
  The dump of a level also holds the title screen's and Paris's textures, which load on the way there.
- **Three versions of a surface:** many walls and floors come as up to three textures with the same drawing in
  different colours: the surface in shadow, lit, and highlighted. Change all of them, or the surface changes
  colour as the light on it changes.
- **Alpha:** 255 is fully opaque, as in any PNG.
- **Unchanged textures:** a PNG that is the dump unchanged is ignored, so the texture looks exactly as before.
  This makes it safe to keep a whole dump in Mods and edit a few files.
- The game's log prints, for every level, how many textures it replaced.

### The PS3 HD textures (The Sly Collection)

As for Sly 2, **Settings > Games > Sly Cooper and the Thievius Raccoonus > Import PS3 HD textures** takes the redrawn
textures of **The Sly Collection** (PS3: the disc image, a `.zip` of it, or the extracted disc) and uses them over
Sly 1's. It takes about 2 minutes and writes about 530 MB (9222 textures, 45 of the 46 levels; the start-up splash
has none) to `PS3 HD/Textures` in your profile folder (`~/.config/projectcane/PS3 HD`,
`%LOCALAPPDATA%\ProjectCane\user\PS3 HD`).

- The pack uses the names of `--dump-textures`, so your own `Mods/Textures` win over it.
- **Use the PS3 HD textures** turns it on or off; **Remove** deletes it. The levels stay the installation's.
- Without the launcher: `Sly1-Release --data-dir "<installation>" --ps3-hd-textures "<PS3 disc, .iso or .zip>" OUT`,
  then `--texture-pack OUT`.

### Text

All on-screen text that the game takes from its string table (menus, the pause menu, prompts, level and world
names, power-up names, cutscene titles, the title screen) can be replaced per language with
`Mods/Text/<Language>.json`:

1. Dump the current strings. This writes one file per language of your disc: `English.json` (USA), `English.json`,
   `French.json`, `German.json`, `Spanish.json` and `Italian.json` (Europe), `Japanese.json` (Japan) or
   `Korean.json` (Korea):

   ```sh
   Sly1-Release --data-dir "<installation>" --dump-text text
   ```

2. Copy the strings you want to change into your own file:

   ```json
   {
     "strings": {
       "28": "A Very Sneaky Approach",
       "83": "Taking a Break",
       "120": "Back to Thieving"
     },
     "levels": {
       "A Stealthy Approach": { "79": "Look around" }
     }
   }
   ```

3. Save it as `Mods/Text/English.json` (or `French.json`, ...).

How the file works:

- **`strings`** applies everywhere; **`levels`**, keyed by the level's folder name, applies only in that level and
  wins over `strings`.
- **Keys** are the string numbers from the dump. You only need the strings you change. You can't add new numbers.
  The numbers are the same on every disc (the European discs' string table); the USA dump lists the USA game's own
  English text under them (152 strings: it shows fewer than the European game).
- Some examples: 0 and 1 are the title screen's "Press START" lines, 25 to 70 the level names, 83 the pause menu
  title, 117 to 149 the menu choices, 162 to 168 the power-up names, 173 to 188 the cutscene titles in the
  Thievius Raccoonus.
- **Encoding:** write the file as UTF-8. Characters the game's font lacks don't show. The European fonts have the
  Western European letters (é, ß, ñ ...).
- **Button icons:** sequences like `&2X&.` (the X button) and `&2T&.` (Triangle) draw the game's button icons.
  Keep them.
- **Comments:** `//` comment lines are allowed in the file.
- Not covered: the PC-only options (Video, Sound, Controls rows), which are not in the string table.

### Music

Sly 1's music files (`Worlds/Music/MUSID_<id>.mus`) are not recordings: each is a sound bank of instrument
samples and a sequence that plays them, and the game changes the arrangement as you sneak or get spotted.
`MusicMap.txt` beside them says which level plays which file.

To replace a track with your own recording:

```sh
Sly1-Release --make-mus "Mods/Worlds/Music/MUSID_51.mus" mytrack.wav --loop 14.9
```

- **`--loop SECONDS`** is where the music jumps back to after the end. The default, 0, loops the whole file.
  `--loop none` plays the track once. The loop is seamless: the tool moves the loop start by under a millisecond
  and changes the loop's length by under 14 samples (both inaudible) to fit the game's sample blocks.
- **`--volume 0..127`** sets the level. The default, 80, puts a normally mastered track about as loud as the
  original music.
- **WAV formats:** 16-bit or 24-bit PCM or 32-bit float, mono or stereo, any sample rate.
- Your track plays the same whether you sneak or get spotted.
- A track is stored at 48 kHz in the game's 4-bit format: a 3-minute stereo track makes a file of about 10 MB.

To get an original track as a WAV, for listening or as a reference:

```sh
Sly1-Release --export-wav "<installation>/Worlds/Music/MUSID_51.mus" original.wav --seconds 120
```

This plays the track through the game's own music engine for that many seconds and records it, so it takes that
long. It records the track as it starts in its level. On Windows it needs an audio output device (the recording
itself is silent).

### Dialog and sounds

Voice lines, commentary, the cutscenes' sound and the `Worlds/Sounds` files are ordinary WAV files. Replace one
by putting a WAV at the same path in Mods, for example `Mods/Worlds/Paris/Dialog/VAG_006B.wav`.

- **Format:** any PCM WAV plays. The originals are 16-bit mono at 22050 Hz (voice lines) or 16-bit stereo at
  48000 Hz (cutscenes).
- **Which line is which:** `Dialog/DialogMap.txt` beside the lines lists each line's id.
- **Languages:** on European, Japanese and Korean installations every line exists once per language, in
  `<Level>/<Language>/Dialogs/`. Replace it in each language you want to change.
- The subtitles and the lip sync stay the original's, so keep a line about as long as the original.

### Cutscenes

Each cutscene is two files: `Cutscenes/Cutscene_NN.m2v` (the video) and `Cutscenes/Cutscene_NN.wav` (its sound;
on European discs, one per language in `Cutscenes/<Language>/`). Put your versions at the same paths in Mods.
Replace both together. With ffmpeg, for example:

```sh
ffmpeg -i myvideo.mp4 -an -c:v mpeg2video -q:v 2 -s 640x480 -r 30000/1001 -f mpeg2video Mods/Cutscenes/Cutscene_00.m2v
ffmpeg -i myvideo.mp4 -vn -c:a pcm_s16le -ar 48000 -ac 2 Mods/Cutscenes/Cutscene_00.wav
```

The video and the sound can be longer or shorter than the original; keep them the same length as each other.
`Cutscene_17` is the start-up splash.

### Sound effects

Each world's sound effects are one 989snd bank (`Worlds/Sound Banks/<World>.bnk`). You can replace it with a bank
in the same format at the same path in Mods. SafeHouse has no tool for these, and every sound the game asks for
must still be in the bank.

### Levels

Levels are PCBRX, SafeHouse's rebuilt level format (see `PCBRX-v19.md`). A level in Mods at the same path replaces
the installed one:

- **The whole level:** `Mods/Worlds/<Episode>/<Level>/<Level>.brx`, a PCBRX file (v19 with all of the disc's
  languages, or a single-language v17).
- **One language only:** on European, Japanese and Korean installations a level holds one version per language.
  `Mods/Worlds/<Episode>/<Level>/<Language>/<Level>.brx`, a single-language PCBRX v17 file, replaces only that
  language's version. On the Japanese disc, the language folders are the voice languages, `Japanese` and
  `English`.

The `Sly1BrxRebuilder` tool, next to SafeHouse's extractor, rebuilds a decompressed retail-format level into
PCBRX v17:

```sh
Sly1BrxRebuilder MyLevel.brx ntsc
```

The tool prints the path of the rebuilt file. Editing levels needs community tools for the retail Sly 1 format.
This is the advanced end of modding: the game expects every object it refers to to still exist, and saves refer
to the level's progress. Start from small changes, and test with a fresh save.

### What is safe to change

| Change | Safe? |
|---|---|
| Textures, any size | Yes |
| Text | Yes. Very long strings may not fit their box. |
| Music, any length | Yes. It no longer changes when you are spotted. |
| Dialog and sounds | Yes. Subtitle and lip-sync timing stay the original's. |
| Cutscenes, any length | Yes, as MPEG-2 video and a WAV |
| Sound effect banks | With care: every sound the game uses must be present |
| Level files | Advanced. Can break the level and saves. |
| The installation folder itself | No. Re-extracting or reinstalling replaces it, and Verify reports changed files. Use Mods. |

Saves don't store anything from mods, so adding or removing a mod never harms a save; level mods are the
exception.


---

## Sharing a mod

- **Sly 2:** zip your Mods folder's contents. A player unzips them into their own Mods folder. Name music and movies
  by number (`Music/28.vpk`), so the mod works with any SafeHouse version.
- **Sly 1:** zip your Mods folder's contents the same way. Players unzip them into their own Sly 1 Mods folder.
- **Never share game files you did not make.** That includes original textures and dumps: share only your own work.

## Sly 2 cosmetics (SafeHouse-Coop)

The safe house wardrobe lists worn items from `share/safehouse/sly2/Cosmetics` (the starter items) and from your
`sly2/Mods/Cosmetics`. Each item is a folder with an `item.ini` and a Wavefront `.obj` (with its `.mtl` colors or a
`map_Kd` texture). A folder in your Mods with the same name as a starter item replaces it.

```
name=Aviator cap
model=aviator_cap.obj
slot=head            ; head, face, back, cane (Sly), fists (Murray: one right fist, mirrored for the left)
characters=all       ; sly, bentley, murray (any of them) or all
hides=hat            ; optional: hat (Bentley's own hat) and/or cane (Sly's cane)
offset=0 0 0         ; optional, in the slot's frame
rotate=0 0 0         ; optional, degrees about X, Y, Z
scale=1              ; optional
```

Frames (game units, about a centimetre): head and face items have their origin on top of the head, X forward and Z
up; back items the middle of the back; a cane is in the cane's own frame (the shaft along X from -40 to 54, the hook
curling in +Y); a fists item is a right fist centred on the origin, X the punch's direction, Z the back of the hand.
An `item.ini` with `hides=` and no `model` only hides (the starter "No hat"). In co-op, your partner sees an item
only if they have it too (the starter items always match). `tools/make_cosmetics.py` in the source makes the
starter items and shows how they are built.
