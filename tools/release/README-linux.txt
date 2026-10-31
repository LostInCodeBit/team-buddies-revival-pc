Team Buddies - native PC port {VERSION} for Linux
=================================================

A native 64-bit Linux port of Team Buddies (PlayStation, 2000), USA release (SLUS-00869).
Bring your own disc: this package contains no game data.

START
  1. Unpack this package into a folder you can write to:
       tar xzf TeamBuddies-{VERSION}-linux-x86_64.tar.gz
  2. Run ./TeamBuddies from that folder. It finds the .cue of your USA disc image (SLUS-00869) next to it,
     or asks for it, and remembers the path; ./TeamBuddies "path/to/game.cue" also works.
  3. Settings: teambuddies.ini in the same folder (written at the first start) and the hotkeys (PLAYING.md).
  Needs x86-64 Linux with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 or later) on X11 or
  Wayland. The hardware renderer draws with Vulkan (Mesa, NVIDIA or AMD drivers); without Vulkan the game
  uses its original 1x software renderer.

FILES
  TeamBuddies       the game
  libSDL3.so.0      window, sound, controllers and Vulkan (SDL 3)
  PLAYING.md        settings, controls, hotkeys, online play
  CHANGELOG.md      what changed in each version
  LICENSE, licenses/  the licences of this port and of the code it uses

  The game writes teambuddies.ini (settings), teambuddies.log, savedata/ (saves) and screenshots/ into this
  folder. To update, unpack a new version over this one: settings and saves are kept.
  If the game crashes or stalls, the reason is in teambuddies.log; attach it to a bug report.

LEGAL
  This is a fan project. It is not made, endorsed or supported by the game's developer or
  publishers, or anyone who holds rights to the game. It contains no game data: no disc image, no
  files from the disc, no graphics, sound, music or movies. TeamBuddies holds the game's program
  translated to native code only; at every start it reads the game's data from your disc image.
  You need your own copy of the USA disc. The project sells nothing.

  This port is free software under the GNU General Public License, version 3 or later (LICENSE).
  Source code: {SOURCE}
  SDL3: zlib licence (licenses/SDL3.txt). xBR-lv2 by Hyllian: MIT licence (licenses/xBR-lv2.txt).
  Team Buddies and its data, names and trademarks belong to their owners.
