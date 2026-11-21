# Installation

## Requirements

1. A Wii, or a Wii U in vWii mode.
2. The Homebrew Channel installed on the console.
3. An SD card or a USB drive, formatted as FAT32.

## Build

Build the app with one of these commands:

```
make PLATFORM=WII dist
tools/docker-make.sh PLATFORM=WII dist
tools/docker-make.ps1 PLATFORM=WII dist
```

The `docker-make` scripts build in the devkitPPC Docker image.

The output is in `dist/apps/wiipeout/`.

There are two builds:

- Release (default): does not write a log file.
- Debug: add `WII_DEBUG=1` to the command. It writes `wiipeout.log`. The output is in `dist-debug/apps/wiipeout/`.

## Install

1. Copy the `apps/wiipeout/` folder to the root of the SD card or USB drive.
2. Check the result. The file `sd:/apps/wiipeout/boot.dol` (or `usb:/apps/wiipeout/boot.dol`) must exist.

## Add the game data

The game data is not included. It is copyrighted. You must supply it.

1. Copy the PSX NTSC wipEout data to `apps/wiipeout/wipeout/`.
2. Copy the `common/` menu models from the PC version to `apps/wiipeout/wipeout/common/`.
3. Optional: add music in QOA format. Name the files `wipeout/music/track01.qoa` to `wipeout/music/track11.qoa`.
4. Optional: add the intro video in MPEG1 format as `wipeout/intro.mpeg`.

The final tree looks like this:

```
apps/wiipeout/
  boot.dol
  meta.xml
  icon.png
  wipeout/
    common/
    textures/
    track01/ ... track14/
    music/track01.qoa ... track11.qoa   (optional)
    intro.mpeg                          (optional)
```

## Data search order

The game looks for the data folder in this order:

1. `wipeout/` in the same folder as `boot.dol`. You can give the app folder any name, for example `apps/MyWipeout/`.
2. `sd:/apps/wiipeout/wipeout`
3. `usb:/apps/wiipeout/wipeout`

Step 1 needs a loader that gives the DOL path, such as the Homebrew Channel.

If no folder exists, the game shows "Data directory not found". Then HOME exits the game.

## Save files and logs

The game writes these files to the folder that contains the `wipeout/` data folder.

- `save.json`: best times and settings.
- `wiipeout.log`: log file. Debug build only, SD card only.

## Launch the game

1. Insert the SD card or USB drive.
2. Start the Homebrew Channel.
3. Select Wiipeout.
4. Press HOME to return to the Homebrew Channel.

## Controls

See the "Wii controls" section in [README.md](README.md#wii-controls).
