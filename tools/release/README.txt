Team Buddies - native PC port {VERSION}
=======================================

A native 64-bit Windows port of Team Buddies (PlayStation, 2000), USA release (SLUS-00869).
Bring your own disc: this package contains no game data.

START
  1. Unzip this package into a folder you can write to (not Program Files).
  2. Run TeamBuddiesLauncher.exe.
  3. Choose the .cue of your USA disc image (SLUS-00869) and press Play.
  Needs Windows 10/11 64-bit and a Direct3D 11 graphics card.
  The programs aren't code-signed, so Windows may show "Windows protected your PC":
  choose More info, then Run anyway.

FILES
  TeamBuddiesLauncher.exe   settings, disc choice, patch notes and Play
  TeamBuddies.exe           the game (also runs on its own)
  SDL3.dll                  controllers
  PLAYING.md                settings, controls, hotkeys, online play
  CHANGELOG.md              what changed in each version
  LICENSE, licenses\        the licences of this port and of the code it uses

  The game writes teambuddies.ini (settings), teambuddies.log, savedata\ (saves) and screenshots\
  into this folder. To update, unzip a new version over this one: settings and saves are kept.
  If the game crashes or stalls, the reason is in teambuddies.log; attach it to a bug report.

LEGAL
  This is a fan project. It is not made, endorsed or supported by the game's developer or
  publishers, or anyone who holds rights to the game. It contains no game data: no disc image, no
  files from the disc, no graphics, sound, music or movies. TeamBuddies.exe holds the game's
  program translated to native code only; at every start it reads the game's data from your disc
  image. You need your own copy of the USA disc. The project sells nothing.

  This port is free software under the GNU General Public License, version 3 or later (LICENSE).
  Source code: {SOURCE}
  SDL3: zlib licence (licenses\SDL3.txt). xBR-lv2 by Hyllian: MIT licence (licenses\xBR-lv2.txt).
  Team Buddies and its data, names and trademarks belong to their owners.
