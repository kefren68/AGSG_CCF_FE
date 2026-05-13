
  AGSG CCF FE — User Guide
  Installation, Folder Structure, Features & Controls
==========================================

  Last updated: May 2026


WHAT IS AGSG CCF FE?

AGSG CCF FE (Custom Classic Frontend) is a graphical game launcher for the
Atari Gamestation Go. It displays your ROM library organized by system, with
boxart, video previews, wheel logos, background music, and a theming system.



1. REQUIREMENTS

- Gamestation Go (GSG) handheld console
- SD card with ROMs already configured
- Launcher folder inside games folder
- Empty Launcher.zip file inside Launcher folder
- A VEEERY big capacity sd card if you think to add a lot of boxarts/videos/3dboxes/screenshots



2. INSTALLATION

Copy the entire AGSG_CCF_FE/ folder to the SD card inside bin folder

  /sdcard/bin/AGSG_CCF_FE/

Create a new Launcher folder inside Games folder

 /sdcard/Games/Launcher

Create an empty Launcher.zip file inside Launcher folder

 /sdcard/Games/Launcher/Launcher.zip

That's it. No additional setup is needed — the launcher auto-detects all
content on first run.


3. FOLDER STRUCTURE

/sdcard/
├── Games/                              (ROMs root - can be lowercase)
│   └── <System>/                       (One folder per system, e.g. Amiga, SNES)
│       ├── *.rom files                 (ROM files with proper extensions)
│       ├── boxart/                     (Optional: per-game artwork)
│       │   └── <game_name>.png         (Same name as ROM without extension)
│       ├── screenshots/                (Optional: per-game screenshots)
│       │   └── <game_name>.png         (Used as fallback when no boxart is found)
│       ├── videos/                     (Optional: per-game video previews)
│       │   └── <game_name>.mp4         (Supports .mp4, .avi, .webm, .mkv)
│       ├── marquees/                   (Optional: per-game marquee)
│       │   └── <game_name>.png         (wheel logos   (<gamename>.png)
│       ├── 3dboxes/                    (Optional: per-game 3dbox)
│       │   └── <game_name>.png         (3dbox   (<gamename>.png)
│       └── gamelist.xml                (Optional: game metadata)
│ 
└── bin/
    └── AGSG_CCF_FE/                    (Launcher application root)
        │
        ├── launcher                  ← executable
        ├── extensions_cfg.txt        ← system/ROM extension mapping (required)
        ├── SuperFolders.txt          ← optional system grouping (see §15)
        ├── favorites.txt             ← saved favorites (auto-created)
        ├── theme_active.txt          ← active theme name (auto-created)
        ├── display_settings.txt      ← HUD visibility toggles (auto-created)
        │
        │
        └── themes/
            ├── default/              ← fallback theme (required)
            └── <ThemeName>/          ← additional themes
                ├── theme.cfg         ← layout & color settings
                ├── systems_desc.xml  ← system descriptions (carousel)
                ├── music_last.txt    ← last played track (auto-created per theme)
                ├── bg/               ← backgrounds  (<system>.png/jpg, default.png)
                │   └── list_bg.png   ← optional separate game-list background
                ├── systems/          ← console images (<system>.png, default.png)
                ├── controllers/      ← controller images (<system>.png, default.png)
                ├── logos/            ← system logos for the game list
                ├── images/           ← HUD icons (WiFi, Battery, Favorite, etc.)
                │   └── crt.png       ← optional CRT scanline overlay (see §17)
                ├── sounds/           ← UI sounds (click.wav, enter.wav, back.wav, fav.wav)
                ├── music/            ← background music (.mp3 .ogg .wav .flac .m4a)
                └── fonts/            ← font file (.ttf or .otf)

Notes:
  • System folder names inside Games/ become the system names shown in the UI.
  • Image/video filenames must match the ROM filename (without extension),
    case-insensitive.
  • The "default" theme is used as a fallback for any missing asset.


4. FIRST RUN

1. Make sure Games/ has at least one system folder with ROMs inside.
2. Make sure extensions_cfg.txt lists the file extensions for each system.
3. Make sure a "default" theme folder exists in themes/.
4. Insert sd card inside the GSG
5. Select SD Card on the popup menu
6. Navigate to Launcher and press A button
7. Launcher.zip must be displayed, press A button to launch it

The launcher will:
  • Scan Games/ and show all systems in the carousel.
  • Show "FAVORITES" as the first carousel entry (starts empty).
  • Start playing background music if music files are present in the theme.


5. CONTROLS — CAROUSEL (system selection screen)

  ←  /  →          Previous / Next system
                    (↑ / ↓ in vertical carousel mode)

  L1  /  R1         Jump ±5 systems

  A                 Enter the game list for the selected system

  SELECT            Open the Theme / Display settings menu

  HOME              Depends on home_mode setting (see §10)

  1  /  3           Previous / Next music track
  (Keypad 1 / 3)

  Held direction    After 2 seconds, activates fast scroll
                    (auto-advances every ~80 ms)



6. CONTROLS — GAME LIST

  ↑  /  ↓           Previous / Next game

  L1  /  R1          Jump ±10 games

  ←  /  →           Switch to previous / next system
                     (stays in list view; FAVORITES is system 0)

  A                  Launch the selected game (or open subfolder)

  B                  Go back (subfolder → root → carousel)

  Y                  Add / Remove game from Favorites

  HOME               Depends on home_mode setting (see §10)

  Held ↑ / ↓        Fast scroll after 2 seconds

In the game list, the right side of the screen shows:
  • Boxart cover (Games/<system>/boxart/<game>.png/jpg)
  • Video preview after 2 seconds (Games/<system>/videos/<game>.mp4 etc.)
  • Wheel logo / marquee over the video (Games/<system>/marquees/<game>.png)
  • Game name, description, and metadata from gamelist.xml (if available)

Media fallback chain (in order of priority):
  1. Boxart   (Games/<system>/boxart/<game>.png/.jpg)
  2. Screenshot (Games/<system>/screenshots/<game>.png/.jpg)  ← used if no boxart
  3. Video    (Games/<system>/videos/<game>.mp4 etc.)
     - If a screenshot is available it is shown first; video starts after
       video_delay_ms (default 2000 ms) with a fade-in effect.
     - If no screenshot is available, the video starts immediately.
  4. no_art.mp4 / no_art.png  (themes/<ThemeName>/images/ or default theme)
     Shown when neither boxart, screenshot, nor video is found.


7. FAVORITES

  • "FAVORITES" is always the first entry in the carousel.
  • Press Y on any game in any system to add it to favorites.
  • Press Y again to remove it.
  • Favorite games are shown with a star icon and highlighted in a
    different color.
  • Entering FAVORITES shows all favorited games in a flat list, each
    prefixed with [SYSTEMNAME].
  • Press Y inside the FAVORITES list to remove a game directly.
  • Favorites are saved to AGSG_CCF_FE/favorites.txt automatically.


8. GAMELIST.XML — GAME METADATA

Optionally place a gamelist.xml file in each system's Games/ folder.
It provides display names, descriptions, genre, developer, rating, etc.

Minimal format:
  <gameList>
    <game>
      <path>./game_filename.nes</path>
      <name>My Game Title</name>
      <desc>Short description of the game.</desc>
      <releasedate>19850101T000000</releasedate>
      <developer>Studio Name</developer>
      <publisher>Publisher Name</publisher>
      <genre>Action</genre>
      <rating>0.8</rating>
    </game>
  </gameList>

To show game names from gamelist.xml in the list (instead of filenames),
add this line to extensions_cfg.txt:
  _show_gamelist_names:1

The AGSG CCF FE Scraper tool can generate this file and download all media
automatically from ScreenScraper.fr.


9. THEMES

Changing theme:
  1. In the carousel, press SELECT.
  2. The THEME / DISPLAY popup opens.
  3. In the THEME tab, scroll through available themes and press A to apply.
  4. The theme is saved and restored on next launch.

Display settings:
  In the DISPLAY tab of the same popup, you can toggle visibility of:
    WiFi icon, Battery icon, System Name, Help Bar, System Logo, Music.

Adding a new theme:
  1. Create a new folder in themes/<YourThemeName>/.
  2. At minimum, add a theme.cfg and a bg/default.png.
  3. Any missing asset automatically falls back to the "default" theme.

Editing theme.cfg:
  The file uses a simple INI format with sections like [carousel], [list],
  [colors], [fonts], [video], [gamelist_3dbox], [crt_overlay], etc.

Notable theme.cfg options:

  [carousel]
    vertical=1              Scroll vertically (↑/↓) instead of horizontally.
    show_controller=1       Show a controller image for the current system.
    show_desc=1             Show a system description text below the console image.
    device_shadow_alpha=N   Drop shadow under the console images (0 = off).

  [gamelist_3dbox]          See §16 — 3D Box View.

  [crt_overlay]             See §17 — CRT Overlay.

  [boxart_overlay]          See §19 — Boxart Overlay.

  [helpbar]                 Help bar position and scale (shared defaults).
    scale=0.8               Scale factor for the help bar icons/text.
    bottom_margin=5         Distance from the bottom edge of the screen.
    x=-1                    X position (-1 = auto-center).

  [helpbar_game]            Overrides for the help bar shown in the game list.
    scale=N                 Scale (default: inherits from [helpbar]).
    bottom_margin=N         Bottom margin (default: inherits from [helpbar]).
    x=N                     X position (default: inherits from [helpbar]).

  [helpbar_menu]            Overrides for the help bar shown inside the
                            SELECT popup menu.
    scale=N                 Scale (default: inherits from [helpbar]).
    bottom_margin=N         Bottom margin (default: inherits from [helpbar]).
    x=N                     X position (default: inherits from [helpbar]).

  [misc]
    shadows=1               Enable/disable drop shadows on text and buttons.
    shadow_alpha=150        Alpha of shadows (0–255).
    shadow_offset_x=2       Horizontal shadow offset in pixels.
    shadow_offset_y=2       Vertical shadow offset in pixels.
    shadow_color=0,0,0,150  Shadow color (R,G,B,A).
    fast_scroll_interval=80 Milliseconds between auto-steps during fast scroll
                            (when a direction button is held for > 2 seconds).

  bg/list_bg.png (or .jpg): Optional separate background displayed when inside
    a game list, instead of the system carousel background.

  controllers/<system>.png: Controller image shown in the carousel for each
    system. Falls back to default.png if the system file is missing.

  systems_desc.xml: Provides the system description text shown in the carousel
    when show_desc=1. Uses the same format as Emulation Station descriptions.


10. BACKGROUND MUSIC

  • Music plays automatically when the carousel is shown.
  • Music stops when you enter a game list.
  • Supported formats: .mp3  .ogg  .wav  .flac  .m4a
  • Place music files in:  themes/<ThemeName>/music/
  • If a theme has no music, the default theme's music folder is used.

Controls (carousel only):
  1 / Keypad 1    Previous track
  3 / Keypad 3    Next track

  The track name appears on screen for 3 seconds when it changes.
  The last played track per theme is remembered across sessions.

  Music can be toggled from SELECT → DISPLAY → "Music in carousel".


11. VOLUME CONTROL

  Volume Up    Joystick btn 16  (Keypad 9)
  Volume Down  Joystick btn 14  (Keypad 7)

  On the GSG, hardware volume keys also work.
  Volume has 11 levels (0% to 100% in 10% steps).
  A volume icon appears on screen for 2 seconds after each change.


12. EXTENSIONS_CFG.TXT

This file tells the launcher which file extensions belong to each system
and controls some global options.

Location:  AGSG_CCF_FE/extensions_cfg.txt

Format:
  <SystemFolderName>:<.ext1>,<.ext2>,...

Example:
  NES:.nes,.unf
  SNES:.sfc,.smc,.fig
  Mega Drive:.md,.bin,.gen
  Game Boy:.gb
  GBA:.gba

Special options (one per line):
  _hide_dirs:<folder1>,<folder2>,...
      Folders to hide from game lists (e.g. boxart, videos, marquees).
      Example:  _hide_dirs:boxart,videos,marquees,.metadata

  _home_mode:N
      Controls what the HOME button does (see §13).
      Example:  _home_mode:2

  _show_gamelist_names:1
      Show game names from gamelist.xml instead of filenames.


13. HOME BUTTON MODES

Set with _home_mode:N in extensions_cfg.txt.

  0   HOME does nothing
  1   HOME exits the launcher  (default)
  2   HOME returns to the carousel from the game list
  3   Like 2, but also highlights FAVORITES when returning to the carousel


14. HDMI OUTPUT

The launcher automatically detects whether an HDMI cable is connected at
startup. When HDMI is detected:

  • The UI scales automatically to the TV's resolution (logical size 1024×600).
  • Only systems that are supported in HDMI mode are shown in the carousel.
    Systems not on the list are hidden to avoid launching incompatible emulators
    on TV output.

Supported systems in HDMI mode as far (folder names, case-insensitive):
  Atari2600, Atari2600Paddle, Atari5200, Atari7800, AtariLynx,
  DOOM, GameAndWatch, GameBoy, GameBoyAdvance, GameBoyColor,
  Intellivision, MAME, NeoGeo, NeoGeoPocketColor, NES,
  Odyssey2, PCEngine, PCEngineCD, PlayStation, ScummVM,
  Sega32X, SegaCD, SegaGameGear, SegaGenesis, SegaMasterSystem,
  SNES, Vectrex, WonderSwanColor, Pico8.

No configuration is needed — HDMI detection is automatic.


15. SUPERFOLDERS

SuperFolders let you group multiple systems under virtual folder entries in
the carousel (e.g., "Atari", "Sega", "Nintendo").

Setup:
  1. Create the file  AGSG_CCF_FE/SuperFolders.txt
  2. Define each group with a [FolderName] header followed by system folder
     names, one per line.

Format:
  [FolderName]
  SystemFolderName
  SubGroup/SystemFolderName   ← nested sub-folder with "/" separator

Example:
  [Nintendo]
  NES
  SNES
  GameBoy
  Handhelds/GameBoyColor
  Handhelds/GameBoyAdvance

  [Sega]
  SegaGenesis
  SegaMasterSystem
  SegaCD

Rules:
  • SuperFolder entries appear before regular (unassigned) systems in the carousel.
  • Systems listed inside a SuperFolder are removed from the main carousel.
  • Navigating into a SuperFolder shows its contents (sub-folders and systems).
  • Pressing B from inside a SuperFolder returns to the carousel.
  • Systems not mentioned in SuperFolders.txt continue to appear normally.
  • Lines starting with # are treated as comments and ignored.

A SuperFolders.txt by OrangeKryptonite file is already inside AGSG_CCF_FE with commented names(#).
You need to just uncomment the name if you want to use it.


16. 3D BOX VIEW (GAME LIST)

When enabled, the game list replaces the standard boxart panel with an
animated 3D box-art carousel, showing the current and adjacent box covers
with a perspective effect.

To enable, add to theme.cfg:

  [gamelist_3dbox]
  enabled=1

Available parameters (with defaults):

  center_x=512          Horizontal center of the 3D box carousel area.
  center_y=280          Vertical center of the center box.
  box_w=200             Width of the center box in pixels.
  box_h=280             Height of the center box in pixels.
  side_scale=0.65       Scale factor for adjacent (non-selected) boxes.
  side_alpha=0.55       Transparency of adjacent boxes (0.0–1.0).
  visible_sides=2       Number of boxes visible on each side of the center.
  spacing=20            Gap between boxes in pixels.
  slide_speed=3600      Animation speed (pixels/second).
  highlight_alpha=80    Alpha of the highlight rectangle on the selected box.
  show_name=1           Show the game name below the center box.
  name_y=-1             Y position of the name label (-1 = auto).
  name_color=255,255,255,255   Name text color (R,G,B,A).

Note: boxart images in Games/<system>/boxart/ are used as the box faces.
The 3D box view requires boxart to be present; if a game has no boxart
a placeholder folder icon is shown instead.

In 3D box mode the navigation directions are swapped compared to the
standard game list:
  ← / →   Navigate games (instead of switching system)
  ↑ / ↓   Switch to previous / next system


17. CRT OVERLAY

A CRT scanline effect can be rendered over the boxart/video area to simulate
the look of a cathode-ray tube monitor.

Setup:
  1. Place a scanline PNG image at:
       themes/<ThemeName>/images/crt.png
     (Falls back to the "default" theme's crt.png if not found.)
  2. Optionally configure the overlay area in theme.cfg:

  [crt_overlay]
  x=-1    Left edge of the overlay (-1 = auto-match the media rect).
  y=-1    Top edge of the overlay  (-1 = auto-match the media rect).
  w=-1    Width of the overlay      (-1 = auto-match the media rect).
  h=-1    Height of the overlay     (-1 = auto-match the media rect).

When all four values are -1 (the default), the overlay is stretched to
exactly cover the currently displayed boxart or video. Set explicit values
to pin the overlay to a fixed screen region regardless of media size.

The overlay image is blended on top of the media using SDL alpha blending,
so a semi-transparent PNG with scanline rows produces the best effect.


18. BOXART OVERLAY

When a video is playing (or when the 3D box view is enabled), the standard
boxart panel is replaced by the video/3D view. The boxart overlay lets you
display a small boxart thumbnail on top of the video or 3D box area.

To enable, add to theme.cfg:

  [boxart_overlay]
  enabled=1

Available parameters (with defaults):

  x=645           X position of the overlay thumbnail.
  y=310           Y position of the overlay thumbnail.
  max_w=120       Maximum width in pixels.
  max_h=90        Maximum height in pixels.
  alpha=220       Opacity of the thumbnail (0–255).
  shadow=1        Draw a drop shadow behind the thumbnail.
  shadow_alpha=120  Alpha of the drop shadow.

The overlay is only shown when video is active or in 3D box mode, and only
for game entries (not folder entries). It falls back gracefully if boxart
is not available for the selected game.


19. LAUNCHER.SH

You will find a launcher_b.sh inside the AGSG_CCF_FE folder.
This launcher from Buckysrevenge let you return to the LOCAL STORAGE / MICRO SD CARD menu when you press HOME button.
To apply this function, the HOME function in extensions.cfg must be set to 1 (exit launcher)
To use the launcher_b.sh, simply rename it in launcher.sh (you have to delete or rename your existing launcher.sh)


APPENDIX — QUICK CONTROLS REFERENCE

CAROUSEL
  ← →           Previous / next system  (↑ ↓ in vertical carousel mode)
  L1 / R1       Jump ±5 systems
  A             Enter game list (or open SuperFolder)
  B             Back (from inside a SuperFolder)
  SELECT        Theme & display menu
  1 / 3         Prev / next music track
  HOME          See §13

GAME LIST
  ↑ ↓           Previous / next game (← → in 3dbox game list mode)
  L1 / R1       Jump ±10 games
  ← →           Switch system (↑ ↓ in 3dbox game list mode)
  A             Launch / enter folder
  B             Back
  Y             Add / remove favorite
  HOME          See §13

VOLUME (anywhere)
  Btn 14 / Kp7  Volume down
  Btn 16 / Kp9  Volume up

DESKTOP / NATIVE BUILD ONLY
  F1            Toggle keyboard shortcut help overlay
  F5            Reload theme.cfg (hot-reload)
  F11           Toggle fullscreen

THEME MENU (SELECT)
  ↑ ↓           Navigate themes / options
  A             Apply theme / toggle option
  B             Close menu


