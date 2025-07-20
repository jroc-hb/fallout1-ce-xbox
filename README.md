# Fallout Community Edition XBOX v0.1.0-alpha

An early port of Fallout 1 for the Original Xbox made using the NXDK. Very much a work in progress with performance issue and very crude controller code but it boots and can be played!

# Features
- Working FMVs (Thanks to glebm for his PR in the source repo)
- Working save system in UDATA that appears in MS Dashboard
- Functioning (crude) mouse emulation on the controller with hot keys mapped to buttons
- Right-stick camera control
- 480p, 720p and 1080i support via ini file in UDATA (only 480p runs well)
- Default "Vault Dweller" name on create character screen due to lack of keyboard
- Fully "playable" in that you can probably beat the game without it crashing

# TO DO
- Custom Xbox control scheme potentially with direct character movement similar to Planescape PS4/DevilutionX
- Fix graphical problems with talking head and conversation overlay screens
- Fix audio crackling
- Improve performance and optimize code (NXDK SDL2 is software rendered)
- Custom Xbox UI graphics showing the hotkey buttons
- Reconfigure save slots directory logic so that the individual slots appear separately in the MS Dashboard
- USB Mouse and Keyboard support
- Figure out if the NXDK issues I ran into are actual NXDK bugs or if I'm doing things wrong (SDL_SetRelativeMouseMode crashes the Xbox for instance)
- General code cleanup since I've never ported a game before and am rusty when it comes to C/C++

# Controls

- Very WIP and will be remappable in the future

| Xbox Controller Input      | Fallout Function          | Keyboard Equivalent |
| -------------------------- | ------------------------- | ------------------- |
| **Left Analog Stick**      | Mouse Movement            | N/A                 |
| **Right Analog Stick**     | Camera Movement           | N/A                 |
| **A**                      | Left Mouse Click          | N/A                 |
| **B**                      | Right Mouse Click         | N/A                 |
| **X**                      | Inventory                 | `I`                 |
| **Y**                      | Pip-Boy                   | `P`                 |
| **Back**                   | Character Sheet           | `C`                 |
| **Start**                  | Options Menu              | `Esc`               |
| **D-Pad Up**               | Menu Up / Navigate Up     | `↑`                 |
| **D-Pad Down**             | Menu Down / Navigate Down | `↓`                 |
| **D-Pad Left**             | Open Skilldex             | `S`                 |
| **D-Pad Right**            | Quick Save Menu           | `F6`                |
| **White**                  | End Turn                  | `SPACE`             |
| **Black**                  | Exit Combat               | `ENTER`             |
| **Left Stick Click**       | Center Camera on Player   | `Home`              |
| **Right Stick Click**      | Enter Combat Mode         | `A`                 |

# Installation Instructions
- The FalloutCE folder contains `default.xbe`, `f1_res.ini` and `fallout.cfg`
- You must supply the following files from your Fallout 1 install:
- - `DATA` folder
- - `CRITTER.DAT`
- - `MASTER.DAT`
- Once the FalloutCE folder has been prepared, FTP it to your Xbox HDD and launch default.xbe
- The game should automatically create a `UDATA/FALLOUT1` folder and install relevant files
- Saves will be stored in `UDATA/FALLOUT1` along with the `f1_res.ini` used to change the resolution and `fallout.cfg` used to change various options

# Build Instructions
- Install and set up NXDK https://github.com/XboxDev/nxdk
- I personally use WSL Ubuntu 22.04.5 on Windows 11 for NXDK stuff
- Clone this repo
- Copy the following files from your Fallout 1 install to the fallout-ce folder in this repo
- - `DATA` folder
- - `CRITTER.DAT`
- - `MASTER.DAT`
- Open a terminal and run the NXDK activation script (`./nxdk/bin/activate`)
- Run the following commands:
- - `mkdir build`
- - `cd build`
- - `nxdk-cmake -DCMAKE_BUILD_TYPE=Release ..`
- - `make` or `make -jX` where X is the number of CPU threads you want to use
- You should end up with `fallout-ce.iso` in the root folder and `default.xbe` in your `fallout-ce` folder
- Either burn `fallout-ce.iso` to a DVD or copy the `fallout-ce` folder to your Xbox HDD

# NOTES ON XEMU
- FMV scenes cause XEMU to crash, so if you want to mess around on XEMU you will need to add this to your `fallout.cfg` file:
- - 
```[debug]
disable_fmv=1
```
- - If you've already launched the game before then you will need to go to `UDATA/FALLOUT1` on your XEMU HDD and edit the `fallout.cfg` that is stored there
- If you want to debug in XEMU you can build the project with `nxdk-cmake -DCMAKE_BUILD_TYPE=Debug` and launch XEMU with `./xemu.exe -device lpc47m157 -serial stdio` to activate the simulated serial output window

# Special Thanks
- Alexander Batalov for creating the Fallout1-ce project https://github.com/alexbatalov/fallout1-ce
- The NXDK folks for creating a great SDK and answering my questions https://github.com/XboxDev/nxdk
- Gleb Mazovetskiy (glebm) for his PR in the main Fallout1-ce repo that helped me fix the FMVs
- THE XBOX SCENE at large for being awesome 

-----------------------------------------------

# ORIGINAL README BELOW #

-----------------------------------------------

# Fallout Community Edition

Fallout Community Edition is a fully working re-implementation of Fallout, with the same original gameplay, engine bugfixes, and some quality of life improvements, that works (mostly) hassle-free on multiple platforms.

There is also [Fallout 2 Community Edition](https://github.com/alexbatalov/fallout2-ce).

## Installation

You must own the game to play. Purchase your copy on [GOG](https://www.gog.com/game/fallout) or [Steam](https://store.steampowered.com/app/38400). Download latest [release](https://github.com/alexbatalov/fallout1-ce/releases) or build from source. You can also check latest [debug](https://github.com/alexbatalov/fallout1-ce/actions) build intended for testers.

### XBOX

Release: nxdk-cmake -DCMAKE_BUILD_TYPE=Release 
Debug: nxdk-cmake -DCMAKE_BUILD_TYPE=Debug

### Windows

Download and copy `fallout-ce.exe` to your `Fallout` folder. It serves as a drop-in replacement for `falloutw.exe`.

### Linux

- Use Windows installation as a base - it contains data assets needed to play. Copy `Fallout` folder somewhere, for example `/home/john/Desktop/Fallout`.

- Alternatively you can extract the needed files from the GoG installer:

```console
$ sudo apt install innoextract
$ innoextract ~/Downloads/setup_fallout_2.1.0.18.exe -I app
$ mv app Fallout
```

- Download and copy `fallout-ce` to this folder.

- Install [SDL2](https://libsdl.org/download-2.0.php):

```console
$ sudo apt install libsdl2-2.0-0
```

- Run `./fallout-ce`.

### macOS

> **NOTE**: macOS 10.11 (El Capitan) or higher is required. Runs natively on Intel-based Macs and Apple Silicon.

- Use Windows installation as a base - it contains data assets needed to play. Copy `Fallout` folder somewhere, for example `/Applications/Fallout`.

- Alternatively you can use Fallout from MacPlay/The Omni Group as a base - you need to extract game assets from the original bundle. Mount CD/DMG, right click `Fallout` -> `Show Package Contents`, navigate to `Contents/Resources`. Copy `GameData` folder somewhere, for example `/Applications/Fallout`.

- Or if you're a Terminal user and have Homebrew installed you can extract the needed files from the GoG installer:

```console
$ brew install innoextract
$ innoextract ~/Downloads/setup_fallout_2.1.0.18.exe -I app
$ mv app /Applications/Fallout
```

- Download and copy `fallout-ce.app` to this folder.

- Run `fallout-ce.app`.

### Android

> **NOTE**: Fallout was designed with mouse in mind. There are many controls that require precise cursor positioning, which is not possible with fingers. Current control scheme resembles trackpad usage:
> - One finger moves mouse cursor around.
> - Tap one finger for left mouse click.
> - Tap two fingers for right mouse click (switches mouse cursor mode).
> - Move two fingers to scroll current view (map view, worldmap view, inventory scrollers).

> **NOTE**: From Android standpoint release and debug builds are different apps. Both apps require their own copy of game assets and have their own savegames. This is intentional. As a gamer just stick with release version and check for updates.

- Use Windows installation as a base - it contains data assets needed to play. Copy `Fallout` folder to your device, for example to `Downloads`. You need `master.dat`, `critter.dat`, and `data` folder. Watch for file names - keep (or make) them lowercased (see [Configuration](#configuration)).

- Download `fallout-ce.apk` and copy it to your device. Open it with file explorer, follow instructions (install from unknown source).

- When you run the game for the first time it will immediately present file picker. Select the folder from the first step. Wait until this data is copied. A loading dialog will appear, just wait for about 30 seconds. The game will start automatically.

### iOS

> **NOTE**: See Android note on controls.

- Download `fallout-ce.ipa`. Use sideloading applications ([AltStore](https://altstore.io/) or [Sideloadly](https://sideloadly.io/)) to install it to your device. Alternatively you can always build from source with your own signing certificate.

- Run the game once. You'll see error message saying "Could not find the master datafile...". This step is needed for iOS to expose the game via File Sharing feature.

- Use Finder (macOS Catalina and later) or iTunes (Windows and macOS Mojave or earlier) to copy `master.dat`, `critter.dat`, and `data` folder to "Fallout" app ([how-to](https://support.apple.com/HT210598)). Watch for file names - keep (or make) them lowercased (see [Configuration](#configuration)).

## Configuration

The main configuration file is `fallout.cfg`. There are several important settings you might need to adjust for your installation. Depending on your Fallout distribution main game assets `master.dat`, `critter.dat`, and `data` folder might be either all lowercased, or all uppercased. You can either update `master_dat`, `critter_dat`, `master_patches` and `critter_patches` settings to match your file names, or rename files to match entries in your `fallout.cfg`.

The `sound` folder (with `music` folder inside) might be located either in `data` folder, or be in the Fallout folder. Update `music_path1` setting to match your hierarchy, usually it's `data/sound/music/` or `sound/music/`. Make sure it match your path exactly (so it might be `SOUND/MUSIC/` if you've installed Fallout from CD). Music files themselves (with `ACM` extension) should be all uppercased, regardless of `sound` and `music` folders.

The second configuration file is `f1_res.ini`. Use it to change game window size and enable/disable fullscreen mode.

```ini
[MAIN]
SCR_WIDTH=1280
SCR_HEIGHT=720
WINDOWED=1
```

Recommendations:
- **Desktops**: Use any size you see fit.
- **Tablets**: Set these values to logical resolution of your device, for example iPad Pro 11 is 1668x2388 (pixels), but it's logical resolution is 834x1194 (points).
- **Mobile phones**: Set height to 480, calculate width according to your device screen (aspect) ratio, for example Samsung S21 is 20:9 device, so the width should be 480 * 20 / 9 = 1067.

In time this stuff will receive in-game interface, right now you have to do it manually.

## Contributing

Here is a couple of current goals. Open up an issue if you have suggestion or feature request.

- **Update to v1.2**. This project is based on Reference Edition which implements v1.1 released in November 1997. There is a newer v1.2 released in March 1998 which at least contains important multilingual support.

- **Backport some Fallout 2 features**. Fallout 2 (with some Sfall additions) added many great improvements and quality of life enhancements to the original Fallout engine. Many deserve to be backported to Fallout 1. Keep in mind this is a different game, with slightly different gameplay balance (which is a fragile thing on its own).

## License

The source code is this repository is available under the [Sustainable Use License](LICENSE.md).
