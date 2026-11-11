# SafeHouse

SafeHouse is an offline launcher with native PC ports of the Sly Cooper games, for Linux (including the Steam Deck)
and Windows:

- **Sly Cooper and the Thievius Raccoonus** (Sly 1): USA, Europe, Japan and Korea discs.
- **Sly 2: Band of Thieves**: USA, Europe (v1.00 and v2.01) and Japan discs.
- **Sly 3: Honor Among Thieves**: in development.

SafeHouse contains no game data. You install each game from your own PlayStation 2 disc image (`.iso`).

**This is SafeHouse-Coop** (github.com/retrojunkie1/SafeHouse-Coop), a fork of SafeHouse that adds to Sly 2:
online co-op over Steam, a walkable safe house, and a wardrobe for character colors and cosmetics. Its updates come
from the fork's releases.

## Sly 2 online co-op

1. Both players: in **Settings > Games > Sly 2**, set **Online co-op** to **Steam**.
2. Both players: copy `steam_api64.dll` from the folder of any recent 64-bit Steam game into SafeHouse's `bin`
   folder (SafeHouse does not ship Valve's files). Have Steam running, with **Spacewar** (free, Valve's Steamworks
   sample) in your library: co-op borrows its app id, so Steam shows you playing Spacewar.
3. Both players press **Play**. Each game hosts a friends-only lobby; press **F8** in game to open Steam's invite
   dialog, or your friend right-clicks you in Steam and picks **Join Game** (their game must already be running).

Both players need the same SafeHouse-Coop version and the same disc region. The window's title shows the connection.

## The safe house wardrobe

Walk around the safe house and use the wardrobe to recolor Sly, Bentley and Murray (color, brightness, saturation)
and to wear cosmetics: hats, a bandana, a backpack, canes for Sly, a mask and "No hat" for Bentley, and fist items for
Murray. Your co-op partner sees what you wear. Your own items go in `sly2/Mods/Cosmetics` (see `MODDING.md`).

SafeHouse was called ProjectCane until November 2026. It is not affiliated with or endorsed by Sony Interactive
Entertainment or Sucker Punch Productions.

## Start

| Package | How to start it |
| --- | --- |
| `SafeHouse-Launcher-Linux-x86_64.AppImage` (Linux) | Make it executable and open it. Without FUSE: `--appimage-extract-and-run`. |
| `SafeHouse-Launcher-Linux-x86_64.tar.gz` (Linux) | Extract it and open **SafeHouse**. |
| `SafeHouse-Launcher-Windows-x86_64-Setup.exe` | Installs for your Windows account into `%LOCALAPPDATA%\Programs\SafeHouse`, with a Start Menu entry. |
| `SafeHouse-Launcher-Windows-x86_64-Portable.zip` | Extract it into a writable folder and open **SafeHouse.exe**. Keep `portable.txt` beside `bin`. |
| `SafeHouse-Launcher-macOS-arm64.dmg` / `-x86_64.dmg` (macOS, Apple silicon / Intel; Sly 2 only for now) | Drag **SafeHouse** to Applications, then open it with right-click > **Open** the first time. |

Then pick a game, choose **Install** and select (or drag in) your disc image. The image is checked first: the game,
the region, the free space needed, and whether the image is complete. When the install is done, choose **Play**.

On the Steam Deck, install in Desktop Mode, then use **Add to Steam** on the game's page (exit Steam first, or
restart it afterwards). Run the Linux package natively; do not force Proton.

## Where your files are

| | Installed games | Saves, settings and mods |
| --- | --- | --- |
| Linux | `~/.local/share/projectcane/installations` | `~/.config/projectcane` (Sly 2: `sly2/`) |
| Windows setup | `%LOCALAPPDATA%\ProjectCane\installations` | `%LOCALAPPDATA%\ProjectCane\user` (Sly 2: `sly2\`) |
| Windows portable | `data\installations` beside `bin` | `data\user` beside `bin` |
| macOS | `~/Library/Application Support/SafeHouse/installations` | `~/Library/Application Support/SafeHouse/user` |

The folders keep the old ProjectCane name on purpose, so games, saves and settings from earlier versions carry
over. Re-extracting or uninstalling a game never touches your saves.

## On the game page

- **Region:** with more than one region installed, pick which one **Play** starts. **Settings > Games > Install
  another region** adds one. Saves are shared between a game's regions.
- **Add to Steam** and **Add desktop shortcut:** start the game directly, without the launcher window.
- **Update needed / Re-extract:** after an update that changed the installed files, the game is extracted again from
  your disc image. Your saves are kept.

**Settings** has each game's options: display, audio volumes, controls (button prompts, **Controller vibration**)
and more. **Settings > Advanced > Debug mode** turns on the debug keys and Sly 2's debug menu.

## Updates

When the launcher starts it asks GitHub whether a newer SafeHouse exists (**Settings > Launcher > Check for
updates**, on by default; **Check for updates now** is beside it). Only the launcher's version is sent. When there is
a new version you see what is new and how many days older your build is, with **Update now**, **Later** and **Skip
this version**.

**Update now** downloads the package for your kind of installation, checks it against the SHA-256 published with the
release and restarts SafeHouse as the new version. If a game or an install is running, the restart waits for it.
Nothing else changes: installed games, saves, settings, mods and a portable copy's `data` folder are never touched.

| Installation | How it is updated |
| --- | --- |
| AppImage | The AppImage file is replaced where it is, keeping its name and permissions. |
| Linux archive (`.tar.gz`) | The files in its folder are replaced. |
| Windows setup | The installed folder is updated in place, without running the setup; Apps & features shows the new version and the uninstaller keeps working. |
| Windows portable | The folder is updated in place; `portable.txt` and `data` stay as they are. |
| macOS | `SafeHouse.app` is replaced. Keep it in Applications (or another folder you can write to). |

If a file cannot be replaced (another program holds it open, or the folder is read-only), the old files are put back
and SafeHouse keeps working as before; **Open download page** then leads to the release. Each release lists its
SHA-256 checksums in `SHA256SUMS`.

## Mods

Sly 1 and Sly 2 read a **Mods** folder in your settings folder (`Mods` for Sly 1, `sly2/Mods` for Sly 2) that
replaces textures, music, dialog, text and movies without changing the installed game. The guide is `MODDING.md`
beside this file.

## Problems

Turn on **Save logs** in **Settings > Advanced**, reproduce the problem, then open **Log folder** there and attach
`launcher.log` to your report.

Licences of the included libraries are in `THIRD_PARTY_NOTICES.md` and the `licenses` folder.
