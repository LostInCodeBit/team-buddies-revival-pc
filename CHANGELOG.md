# Changelog

## 0.1.0 - 2026-10-30

First public release. It contains no game data: you need your own USA disc (SLUS-00869). Everything below is
in it; *Changed* and *Fixed* record the development before it.

### Added
- Static recompiler for the main executable and all code overlays (SYS, ENG, GAME, MNU, ROT, MPLR, TUTO).
- Native runtime: HLE kernel (events, interrupt chains, memory card files), GTE, GPU, MDEC, SPU
  (ADPCM, ADSR, reverb), CD-ROM (CUE/BIN, XA-ADPCM, CD-DA), DMA, timers.
- Direct3D 11 renderer with 1x–8x internal resolution, widescreen hack, true color, texture
  filtering; software renderer fallback.
- Win32 window, XAudio2 audio, keyboard input, `teambuddies.ini` settings, hotkeys, screenshots.
- Crash handler: unhandled exceptions write a symbolized stack to `teambuddies.log` and show a message.
- Watchdog: a 3 s stall writes the main thread's stack to the log.
- `log_level` 2: per-second status (speed, audio levels, interrupt state) and a console window.
  Default builds are windowed-only (no console); the log file is capped at 32 MB.
- Ready for a public repository and release: `LICENSE` (GPL-3.0-or-later), `licenses/` (SDL3 zlib, Hyllian's
  xBR-lv2 MIT), `.gitignore` (no build output, recompiled code, disc files or images, local tooling) and
  `.gitattributes`. `tools\package_release.ps1 -Version x.y.z -SourceUrl <repo>` builds the portable
  `dist\TeamBuddies-x.y.z-win.zip` from an explicit file list (launcher, game, `SDL3.dll`, `README.txt` with a
  quick start and the legal notice, `PLAYING.md`, `CHANGELOG.md`, licences) plus `SHA256SUMS.txt`; it refuses
  a build older than its sources. Tested by unzipping it alone and running the game from there.
  `extra_seeds.txt` holds the two functions found at run time (`build\misses.txt`), so a fresh checkout
  recompiles the same code.
- `frame_rate = 60` (F10, launcher Graphics tab): gameplay, which the game itself runs at 30 fps with one fixed
  logic step per frame, is shown at 60 fps by drawing an in-between frame on every other vblank. Each frame's
  primitives are recorded between buffer flips and matched to the previous frame's: unique ones first (which
  also give the local motion), then equal ones in the order the game built its packets, then the nearest fit;
  untextured faces must keep their shape and shading, and leftovers move with their neighbours so objects stay
  whole. Matched primitives are redrawn halfway, at sub-pixel positions, into a separate target. Presentation
  only: RAM fingerprints are identical with it on or off (safe online). Costs one vblank (16.7 ms) of delay;
  hardware renderer only; 8x at 1080p still runs at 170+ fps unthrottled. Fast-moving small parts (feet) can
  show small slivers on in-between frames.
- Unlocked frame rate: `frame_rate = 120`, `144`, `165`, `240` (any 60–500) or `monitor`; the launcher's
  Frame rate menu lists them with the monitor's refresh rate. After each vblank the emulator waits, and
  in that time as many pictures are shown as the cap allows, each blended for the moment it reaches the
  screen. Without V-Sync presents are spaced in time across vblanks, so rates that don't divide 60 average out;
  with V-Sync the monitor paces them, at most its refresh rate. With V-Sync the picture runs one refresh later
  and the frame before the previous one is kept, so a picture shown just after a vblank still has frames on
  both sides. Measured in real time on a 144 Hz laptop (GTX 1650): `monitor` + V-Sync 143–145 pictures/s,
  1383 of 1440 exactly one refresh apart; 120 + V-Sync 120; 144 and 240 without V-Sync 144 and 240. The game
  keeps its speed (59.8–60 vblanks/s) and RAM fingerprints match the original frame rate exactly.
  Hairline seams in the game's own models (dots at 4x even without it) can show as a faint line on some
  in-between pictures.
- Swap chain has three buffers: with one picture waiting for the next refresh the GPU no longer stalls the
  emulator's own drawing.
- Anti-aliasing (`anti_aliasing`, F1, launcher Graphics tab): FXAA over the picture at the internal
  resolution; MSAA 2x/4x/8x renders the upscaled VRAM multisampled (polygon edges, HUD and text stay sharp);
  SSAA 2x/4x/8x shades every sample, so textures and see-through edges are smoothed too. The multisampled VRAM
  is averaged per 32x32 tile into the copy that textures, readbacks and the screen use; VRAM copies into it
  are drawn, and in-between frames copy and resolve it sample by sample. Texture coordinates are taken at the
  covered samples' centroid, which also removes the specks the original shows along some polygon seams.
  Samples are capped so the VRAM stays within 512 MB (8x resolution: 4) and drop on GPUs that offer fewer.
  RAM fingerprints are identical in every mode. Borderless 1080p, `monitor` + V-Sync on a GTX 1650 laptop:
  4x resolution 141–144 pictures/s without AA, 139–143 with MSAA 4x, 118–123 with SSAA 8x; 8x resolution with
  MSAA 123–128; the game keeps 60 vblanks/s in all of them.
- Texture filtering `xbr` (`texture_filtering = off | bilinear | xbr`, F7 cycles, launcher Graphics tab;
  `true`/`false` from older ini files still read): Hyllian's xBR-lv2 at any magnification redraws textures,
  sprites and letters with smooth, hard edges instead of blocks or blur. Each primitive carries the box of
  texels it uses, so neither filter reads past it (no bleeding from the next letter or picture in a page).
  Texels whose side neighbours all match are returned as they are after 5 reads instead of 21: same picture,
  1.5x faster. GTX 1650 laptop, 1080p, MSAA 4x, 240 fps cap: 190–197 pictures/s at 4x resolution, ~80 at 6x;
  8x is more than that GPU can draw (the game slows down).
- HD text (`hd_text`, on by default; launcher Graphics tab, which now packs seven rows): the game's letters and
  the battle HUD's numbers (countdown, ammo, health) are redrawn from smooth reconstructions at 12x instead of
  upscaled blocks, blur or xBR stairs. Six fonts (menu and briefing text 8, 12 and 16 texels high, the park
  map's outlined labels, the HUD's 18- and 10-texel digits) are recognised per sprite by a hash of their palette
  indices (`runtime/glyph_hashes.inc`, hashes only), wherever they sit in VRAM and in any palette that shades
  between two colours; other letters, such as the park map's red-to-yellow titles, keep the texture filter. The
  GPU reconstructs a letter the first time it is drawn (no CPU cost or stutter): coverage from the palette's
  brightness, Catmull-Rom interpolation, the letter's edge at half coverage and an even dark rim; outlined
  letters and digits get their outline as the smooth letter grown by a disc of the original width (1 or 2
  texels), counters still see-through. Cells go into a 4096x4096 atlas with mip levels (43 MB). The original
  palette and vertex colour still colour it, so fades and tints work. In-between frames draw the same letters;
  RAM fingerprints are identical with it on or off (menus to battle).
- An internal resolution above the window's is averaged down (area filter) instead of sampled, so it works
  as supersampling; previously every 2nd or 3rd pixel was skipped. V-Sync moved to the launcher's Display tab.
- `TeamBuddiesLauncher.exe`: Material 3 launcher (Direct2D/DirectWrite, no new dependencies) with Play,
  Settings (Graphics, Display, Game, Controls tabs, reset per tab) and Patch notes (this file, embedded).
  The Play page shows the disc (checked for SLUS-00869), connected controllers and saves (opens the
  savedata folder), plus the keyboard and in-game keys. Settings are written atomically as they change;
  the launcher hides while the game runs and returns, re-reading the ini, when it exits.
- Display modes: window, borderless, exclusive fullscreen (`display`; the old `fullscreen` key is still read).
- Aspect `wide` (fills the window's shape; the widescreen hack follows it, offline) and `pixels`
  (whole-number scaling); F6 cycles all five.
- `skip_intro`: Start is tapped through the logos, intro movies and title, so the game opens at the main menu.
- `vibration` switch for controller rumble.
- Saves as files: `savedata\SLUS-00869\<save name>.bin` holds each save's raw blocks (card 2 in `card2\`).
  The game sees a memory card assembled from them; changed saves are written back half a second after the
  game stops writing and at exit, the previous version kept in `backup\`. Old `.mcd` cards are split up on
  first start and kept as `.mcd.migrated`; one that cannot be read is used as before.
- App icon (a crate) for the game and the launcher; `tools\make_icon.ps1` draws it.
- More test-only switches for unattended runs: `TB_TURBO=n` fast-forwards to vblank n and then runs in real
  time, `TB_SHOT_VBLANK` captures consecutive presents (in-between frames too), `TB_PACE_LOG` logs present
  timing. Scripted input replaces a plugged-in controller instead of adding to it (stick drift made runs
  differ). `TB_HIDDEN` runs without a window, console, sound or error boxes at low CPU/GPU priority, so tests
  don't disturb someone using the PC; `TB_KEYS="6400=F1"` presses hotkeys at given frames. `TB_RAM_DUMP` writes
  main RAM and raw VRAM at a vblank; `tools/glyph_hashes.py` builds the HD text catalogue from four such dumps.
  `TB_GPU_TRACE` logs call stacks at buffer flips and vblanks; `TB_POKE8` forces a RAM byte (used to measure
  frame pacing: gameplay is a fixed 30 Hz step).
- Online play for two players (experimental, work in progress): delay-based lockstep over UDP, peer to peer (`--host` / `--join`, or the
  F9 dialog that restarts the game connected). Handshake checks protocol, build and disc; the host's
  settings that change emulation and its memory cards are sent to the client (used in memory only);
  input delay from the measured ping; redundant input packets tolerate loss; RAM fingerprint every
  120 frames detects desyncs; UPnP port forward on the host; status popup with Cancel while connecting;
  a dropped peer ends the session and the game continues offline. Tested with two local instances:
  identical RAM and pixel-identical frames through a 2-player split-screen match, also with 20% loss.
- Emulation determinism verified: identical RAM fingerprints across runs at different resolutions.
- Button prompts follow the input device: menus show PlayStation, Xbox (A/B/X/Y, Menu) or keyboard
  (Z/X/A/S, Enter) glyphs for the device used last; `button_icons` in the ini can pin one style.
  The game's icons are recognised by a hash of their texels and redrawn from a high-resolution glyph
  atlas (GDI+ at startup, mip-mapped), so they stay sharp at every internal resolution.
- `teambuddies.ini`: `button_icons`, `online_address`, `online_port`.
- Controllers through SDL3 3.4: DualShock 4, DualSense, Switch Pro and others besides Xbox pads,
  with hot-plugging and rumble. Replaces the XInput-only input.
- Test-only environment variables for unattended runs: `TB_INPUT`, `TB_INPUT2` (scripted pads), `TB_FRAMES`,
  `TB_SHOT_EVERY`, `TB_SHOT_AT`, `TB_SHOT_DIR`, `TB_INI`, `TB_CARD_DIR`, `TB_TURBO`, `TB_WATCH`, `TB_LOG`,
  `TB_NET_LOSS`, `TB_NET_DESYNC`, `TB_VRAM_DUMP`, `TB_GPU_TRACE` (also logs untextured primitives, fills, VRAM
  copies/transfers, display start and mask state).

### Changed
- `README.md` is a short front page (download, features, getting started, legal notice, licence); settings,
  controls and online play moved to `docs/PLAYING.md`, building and tests to `docs/BUILDING.md`, credits to
  `docs/CREDITS.md`.
- `teambuddies.ini` is written to a temp file and swapped in, so it cannot be left half-written.
- Settings code moved to `runtime/settings.cpp`, shared by the game and the launcher; values are clamped on load.
- Saves moved from `memcards\cardN.mcd` to `savedata\SLUS-00869\` (see Added).

### Fixed
- Texture filtering smeared the game's text: its letters and digits are 8x8 sprites, blown up 8x and blended.
  Sprites under 32 texels wide or high are no longer filtered, so text and HUD numbers stay sharp; 3D textures
  and larger 2D art (the main menu's 192x80 pictures) are still smoothed.
- `build.bat` no longer fails after linking when `SDL3.dll` is in use (launcher or game open): it is copied
  only when newer.
- The log no longer calls `OutputDebugString` without a debugger: with another program listening for debug
  output (seen while Valorant ran) every log line could stall the game for seconds.
- Hardware renderer: menus drawn over the previous screen (e.g. OPTIONS over the main menu) and
  missing flat-shaded shapes such as the volume bars. The GPU reset zeroed the draw state, which
  marked every untextured primitive (including the game's full-screen clear) as a button glyph that
  the shader then discarded. Regression from the button-prompt replacement.
- Freeze/garbage after the mission briefing ("GET READY") when entering a level: hand-written
  assembly in the 3D renderer branches into its caller's epilogue and returns two levels at once.
  The recompiler now detects functions that restore `ra` without saving it, flags such non-local
  returns, and every call site unwinds to the matching level (interpreter fallback too).
- Memory card saves not recognised: non-blocking (`0x8000`) card `read`/`write` now return 0 like
  the real kernel; the game looped on the byte count and overwrote the start of the loaded data.
- Audio stopping for good when the output device is unplugged or the default device changes;
  output is reopened automatically (also when no device was present at start).
- Graphics device loss (driver reset) now shows a message instead of a frozen window.
- Hybrid-graphics laptops: the discrete GPU is preferred (DXGI high-performance adapter, Optimus/PowerXpress exports).
- Window minimized: the game no longer runs unthrottled with a frozen message loop.
- Hang after the LOADING screen: CD sector interrupts are now buffered (up to 8 sectors) and CD
  DMA never copies past the data actually waiting in the sector buffer.
- "Insert controller into controller port 1": full DualShock emulation on SIO0 (digital/analog,
  config mode, rumble to XInput).
- CD-ROM interrupts not enabled at boot; CD data FIFO reset by a repeated read request.
- Library code calling the kernel through patched A0/B0/C0 tables.
