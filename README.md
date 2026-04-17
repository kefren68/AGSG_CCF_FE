# AGSG CCF FE — Installation Guide
**Custom Frontend Launcher for the Atari Gamestation Go**
*by Kefren68 — Revision 1.2*

---

This is a custom launcher for the **Atari Gamestation Go (AGSG)** handheld console, written in C++ with SDL2.
It is not a fork of EmulationStation, Batocera, or any other existing frontend — it was built from scratch.

If you find bugs or have suggestions, please report them. Any feedback is welcome!

---

## 1. Requirements

- Atari Gamestation Go with custom firmware installed
- SD card with ROMs already organized in folders
- A file manager or SSH access to the device

---

## 2. Folder Structure

Copy the `AGSG_CCF_FE` folder to `/sdcard/bin/` on your device.
The final structure should look like this:

```
/sdcard/
├── Games/                              ← ROMs root folder
│   └── <SystemName>/                   ← One folder per system (e.g. SNES, Amiga)
│       ├── game.rom                    ← ROM files
│       ├── boxart/                     ← Optional: per-game cover art (PNG/JPG)
│       │   └── GameName.png
│       ├── videos/                     ← Optional: per-game video previews
│       │   └── GameName.mp4
│       └── gamelist.xml                ← Optional: metadata (EmulationStation format)
│
└── bin/
    └── AGSG_CCF_FE/                    ← Launcher root (THIS FOLDER)
        ├── launcher                    ← ARM binary
        ├── launcher.sh                 ← Startup wrapper
        ├── extensions_cfg.txt          ← System/extension configuration
        ├── favorites.txt               ← Auto-created at runtime
        │
        ├── images/                     ← UI icons and backgrounds (all PNG)
        ├── sounds/                     ← Sound effects (WAV)
        ├── fonts/                      ← Font files (TTF/OTF)
        ├── libs/                       ← Shared ARM libraries
        │
        └── themes/
            └── default/
                ├── bg/                 ← Carousel and list backgrounds
                ├── systems/            ← Console/device images
                └── logos/              ← System logos
```

---

## 3. Installation Steps

### Step 1 — Copy the launcher folder

Copy the entire `AGSG_CCF_FE` folder to:
```
/sdcard/bin/AGSG_CCF_FE/
```

### Step 2 — Create the Launcher entry in the game list

The launcher is launched from within the Gamestation Go's original menu by selecting a dummy "game".

1. Create a folder named **`Launcher`** inside `/sdcard/Games/`
2. Create an empty file named **`Launcher.zip`** inside that folder

```
/sdcard/Games/Launcher/Launcher.zip     ← empty file, just needs to exist
```

### Step 3 — Edit `start_local_sd.sh`

Open `/sdcard/bin/start_local_sd.sh` in a text editor and add the following block **after the Pico-8 section** (after the two `fi` that close the Pico-8 block):


if [ "$4" == "/sdcard/Games/Launcher/Launcher.zip" ]; then
    echo 0   > /sys/class/leds/key_led1/brightness
    echo 0   > /sys/class/leds/key_led3/brightness
    echo 0   > /sys/class/leds/key_led4/brightness
    echo 0   > /sys/class/leds/key_led5/brightness
    echo 255 > /sys/class/leds/key_led15/brightness
    echo 255 > /sys/class/leds/key_led16/brightness
    echo 255 > /sys/class/leds/key_led17/brightness
    echo 255 > /sys/class/leds/key_led18/brightness
    echo 255 > /sys/class/leds/key_led19/brightness
    echo 255 > /sys/class/leds/key_led20/brightness
    echo 255 > /sys/class/leds/key_led22/brightness
    sh /sdcard/bin/AGSG_CCF_FE/launcher.sh
    exit 0
fi


### Step 4 — Launch

In the Gamestation Go's original menu, navigate to the **Launcher** system and select **Launcher.zip**.
The custom frontend will start.

---

## 4. Controls

| Button | Action |
|--------|--------|
| **D-Left / D-Right** | Navigate between systems in carousel |
| **D-Up / D-Down** | Navigate game list |
| **A** | Enter system / Launch game / Enter folder |
| **B** | Go back |
| **Y** | Toggle favorite (game title turns orange) |
| **L1 / R1** | Jump ±5 in carousel, ±10 in game list |
| **HOME** | Exit launcher |
| **SELECT** | Open theme selector (from carousel) |

---

## 5. Configuration

### `extensions_cfg.txt`

This file tells the launcher which file extensions to show for each system, and which folders to hide.

```
Amiga:adf,adz,dms,ipf,hdf,hdz,lha,zip
GameBoy:gb,gbc,zip
SNES:smc,sfc,zip
PlayStation:bin,cue,img,iso,pbp,chd
_hide_dirs:boxart,videos,gamelist.xml
```

- Add a new line for each system folder.
- `_hide_dirs` lists subfolder names that should **not** appear in the game list.
- If a system is not listed, all files are shown (no extension filter).

### `gamelist.xml` (Optional, per system)

Place a `gamelist.xml` file inside any system folder to provide custom display names and descriptions.
Uses the standard EmulationStation format:

```xml
<?xml version="1.0"?>
<gameList>
  <game>
    <path>./RomName.zip</path>
    <name>Display Name</name>
    <desc>Short description of the game.</desc>
  </game>
</gameList>
```

---

## 6. Themes

The launcher supports multiple visual themes. Select a theme by pressing **SELECT** from the carousel.

Themes are stored in:
```
/sdcard/bin/AGSG_CCF_FE/themes/<theme_name>/
```

Each theme folder contains:
```
themes/<theme_name>/
├── bg/
│   ├── default.png         ← Default carousel background
│   ├── list_bg.png         ← Game list background
│   └── <SystemName>.png    ← Per-system carousel background (optional)
├── systems/
│   ├── default.png         ← Default device image
│   └── <SystemName>.png    ← Per-system device image (optional)
└── logos/
    └── <SystemName>.png    ← System logo shown in game list header (optional)
```

- Image names must match the system folder names (case-insensitive but "space.sensitive").
- Recommended resolution: **1024×600** (native GSG screen). 1280×720 also works.
- If a system-specific image is missing, `default.png` is used as fallback.

---

## 7. Boxart and Video Previews

Place media files inside each system's ROM folder:

```
/sdcard/Games/SNES/boxart/SuperMario.png
/sdcard/Games/SNES/videos/SuperMario.mp4
```

- **Boxart**: filename must match the ROM name without extension. PNG or JPG.
- **Videos**: filename must match the ROM name. Supported: MP4, AVI, WebM, MKV.
- Folder names are case-insensitive (`Boxart`, `BOXART`, `boxart` are all found).
- Video preview plays automatically after hovering a game for ~0.5 seconds.

---

## 8. Favorites

- Press **Y** on any game to add or remove it from favorites.
- **FAVORITES** appears as the first system in the carousel.
- Favorite titles show a **[SYSTEM]** prefix, e.g. `[AMIGA] Turrican`.
- Boxart is loaded from the original system's folder.
- Favorites are saved automatically to `favorites.txt`.

---

## 9. Custom Images Reference

| File | Description |
|------|-------------|
| `images/no_art.png` | Placeholder when boxart is missing |
| `images/icons.png` | Folder and controller icons (56×56 px each) |
| `images/Favorite.png` | Favorite star icon |
| `images/Battery 01–05.png` | Battery level icons |
| `images/Wifi_logo0–3.png` | WiFi signal icons |
| `images/help.png` | Help bar for game list (optional) |
| `images/help_menu.png` | Help bar for carousel (optional) |
| `images/up/down/left/right.png` | D-pad arrow icons, 20×20 px (optional) |

---

## 10. System Sensors

The launcher reads device status automatically:

| Sensor | Source |
|--------|--------|
| Battery level | `/sys/class/power_supply/battery/capacity` |
| Charging status | `/sys/class/power_supply/battery/status` |
| WiFi signal | `/proc/net/wireless` |

Battery is refreshed every 30 seconds. WiFi every 5 seconds.

---

## 11. Troubleshooting

**The launcher doesn't appear in the game list**
→ Make sure the `Launcher` folder and `Launcher.zip` file exist in `/sdcard/Games/`.

**The launcher starts but shows no systems**
→ Check that `/sdcard/Games/` contains system subfolders with ROM files matching the extensions in `extensions_cfg.txt`.

**A system shows no games**
→ Verify the extensions in `extensions_cfg.txt` match your ROM file types.

**Boxart is not showing**
→ The image filename must exactly match the ROM name (without extension). Extension is case-insensitive.

**Video preview doesn't play**
→ Make sure the file is in a supported format (MP4 recommended) and the filename matches the ROM name.

**The launcher crashes at startup**
→ Check that all required files are present in `images/`, `sounds/`, `fonts/`, and `libs/`.

---

*Thank you to the entire AGSG custom firmware community — you're fantastic.*
*And a special thanks to everyone who tested and provided feedback, expecially to Buckysrevenge and UncleTed for their support*

Have fun with your Gamestation Go!
— **Kefren68**



