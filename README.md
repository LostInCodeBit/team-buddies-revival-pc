# Team Buddies — native PC port

A native 64-bit Windows and Linux port of *Team Buddies* (PlayStation, 2000), USA release (SLUS-00869). The game's
program is statically recompiled to C++; the PlayStation hardware (GPU, sound, CD-ROM, BIOS) is replaced by
native code. No emulator and no BIOS file. Bring your own disc.

**[Download](../../releases/latest)** · **[Players' guide](docs/PLAYING.md)** (settings, controls, online)
· **[Building](docs/BUILDING.md)** · **[Changes](CHANGELOG.md)**

## Features

- A launcher (Windows) with the settings, the disc check, saves and the patch notes.
- Hardware renderer, Direct3D 11 on Windows and Vulkan on Linux: 1x–8x resolution, widescreen,
  FXAA/MSAA/SSAA, xBR texture filtering, HD text
  and HUD numbers, 60 fps or more (up to 500, or the monitor's rate) with in-between frames while the game
  keeps its speed.
- Online play for 2 players (lockstep, UPnP): experimental, still a work in progress.
- Controllers through SDL3 (Xbox, DualSense, DualShock 4, Switch Pro and more) with rumble; button prompts
  follow the device you use. Full keyboard play.
- Saves as files, with the previous version of each kept.

## Getting started

1. Download `TeamBuddies-<version>-win.zip` from [Releases](../../releases/latest) and unzip it into a folder
   you can write to (not Program Files).
2. Run `TeamBuddiesLauncher.exe`, choose the `.cue` of your **USA disc (SLUS-00869)** and press **Play**.

Needs Windows 10/11 64-bit and a Direct3D 11 graphics card. The programs aren't code-signed, so Windows may
show "Windows protected your PC": choose **More info → Run anyway**.

**Linux:** unpack `TeamBuddies-<version>-linux-x86_64.tar.gz` into a folder you can write to and run
`./TeamBuddies`; it asks for the `.cue` (or takes it as an argument). Needs x86-64 with glibc 2.35 or newer
(Ubuntu 22.04, Debian 12, Fedora 36 or later), X11 or Wayland, and Vulkan drivers for the hardware renderer
(without them the game uses its software renderer). There is no launcher: settings are in `teambuddies.ini`
and on the hotkeys.

## Legal notice

- This is a fan project. It is not made, endorsed or supported by the game's developer or publishers, or
  anyone who holds rights to the game.
- This repository and its releases contain **no game data**: no disc image, no files from the disc, no
  graphics, sound, music or movies. `TeamBuddies.exe` holds the game's program translated to native code
  only; at every start it reads the game's data from your disc image.
- To play you need your own copy of the USA disc. Other regions are not supported.
- The project sells nothing.

## Building

C++17: Visual Studio 2022 Build Tools with `build.bat` on Windows, GCC or Clang and SDL 3.4 with `build.sh` on
Linux. Both recompile the game from your own disc image, so nothing builds without it. Details, layout and
tests: [docs/BUILDING.md](docs/BUILDING.md).

## Credits and licence

SDL3 (zlib), Hyllian's xBR-lv2 (MIT) and others: [docs/CREDITS.md](docs/CREDITS.md).

[GPL-3.0-or-later](LICENSE). The original game, its data, names and trademarks belong to their owners and are
not covered by this licence. Code adapted from other projects keeps its own notice (`licenses/`).
