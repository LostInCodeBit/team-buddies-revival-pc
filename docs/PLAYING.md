# Players' guide

## Starting

Run `TeamBuddiesLauncher.exe` to change settings, choose the disc image and play; it hides while the game
runs. `TeamBuddies.exe` also runs on its own: it finds the `.cue` next to it (or in parent folders) or asks for
it, then remembers the path in `teambuddies.ini`. Only the USA disc (SLUS-00869) is supported.

On Linux (experimental) there is no launcher: run `./TeamBuddies` (optionally with the `.cue` as argument); settings are in
`teambuddies.ini` and on the hotkeys. Without Vulkan drivers the game falls back to the software renderer.

Everything the game writes stays in its folder: `teambuddies.ini`, `teambuddies.log`, `savedata\`,
`screenshots\`. To update, unzip the new version over the old one; settings and saves are kept.

## Saves

One file per save in `savedata\SLUS-00869\` (card 2: `savedata\SLUS-00869\card2\`), named after the save's
PlayStation file name and holding its raw 8 KB blocks. The game sees an ordinary memory card built from those
files; the previous version of a save it overwrites or deletes goes to `backup\`. Old `memcards\cardN.mcd`
images are split into files on the first start and kept as `cardN.mcd.migrated`.

## Settings (`teambuddies.ini`)

The launcher's Settings page (Windows) writes these; the file can also be edited by hand.

| Key | Values |
|---|---|
| `renderer` | `hardware` (upscaled: Direct3D 11 on Windows, Vulkan on Linux) or `software` (original 1x) |
| `resolution_scale` | 1–8 (internal resolution multiplier; above the window's size it is averaged down, like supersampling) |
| `anti_aliasing` | `off`, `fxaa`, `msaa2x`/`4x`/`8x` (polygon edges), `ssaa2x`/`4x`/`8x` (edges and textures) (hardware renderer, F1) |
| `display` | `window`, `borderless`, `fullscreen` (exclusive) |
| `window_width`, `window_height` | window size for `display = window` |
| `vsync` | `true` / `false` |
| `frame_rate` | `original` (gameplay runs at 30), a rate such as `60`, `120`, `144`, `240` (up to 500), or `monitor` (its refresh rate): in-between frames are drawn, the game keeps its speed; with `vsync` at most the monitor's rate (hardware renderer, F10) |
| `aspect` | `4:3`, `16:9`, `wide` (the window's shape), `stretch`, `pixels` (whole-number scaling) |
| `widescreen_hack` | widens the 3D view (to 16:9, or to the window's shape with `aspect = wide`) |
| `texture_filtering` | `off` (original texels), `bilinear` (small sprites such as text stay sharp), `xbr` (pixel art redrawn with smooth edges; heavy above 4x resolution) (F7) |
| `hd_text` | the game's letters and HUD numbers redrawn from smooth high-resolution reconstructions (default `true`; hardware renderer) |
| `true_color` | render without the PS1's 15-bit dithering |
| `smooth_output` | bilinear filtering of the final image |
| `cpu_cycles_per_instruction` | 2.0 = original speed, lower = faster CPU (less slowdown) |
| `multitap` | emulate a multitap on port 1 (4 players) |
| `volume` | 0–100 |
| `vibration` | controller rumble |
| `skip_intro` | tap Start through the logos, intro movies and title at boot |
| `button_icons` | `auto` (prompts follow the device used last), `playstation`, `xbox`, `keyboard` |
| `online_address`, `online_port` | last host joined and UDP port (F9 dialog) |
| `log_level` | 0 = errors only, 1 = warnings (default), 2 = live status log + console window |

## Controls

Keyboard (player 1): arrows = D-pad, Z = ✕, X = ○, A = □, S = △, Q/W = L1/R1, E/R = L2/R2, Enter = Start,
Backspace = Select. Controllers (Xbox, DualShock 4, DualSense, Switch Pro and other SDL-supported pads,
hot-pluggable) are players 1–4; the first shares player 1 with the keyboard. Button prompts in menus switch
between PlayStation, Xbox and keyboard glyphs to match the device you last used.

Hotkeys: F1 anti-aliasing, F2/F3 internal resolution, F4 widescreen hack, F5 smooth output, F6 aspect ratio,
F7 texture filtering, F8 true color, F9 online play, F10 frame rate (original / the configured rate), F11 or
Alt+Enter fullscreen, F12 screenshot (`screenshots\`), hold Tab to fast-forward.

## Online play (2 players, work in progress)

Online play is experimental and still being worked on: expect connection problems or desyncs, and report them
with both players' `teambuddies.log`.

Press F9, choose *Host* or *Join* (enter the host's address), and the game restarts connected; or start it
with `TeamBuddies.exe --host [port]` / `--join <address>[:port]` (`--delay N` overrides the input delay).
On Linux the address is asked with `zenity` or `kdialog` (without either, use `--join` or `online_address`),
the window title shows the connection's progress and Esc gives up. Both players need the same disc image and
the same version. The host is player 1, the client player 2 (pick *MULTI*, player 2 joins with ○). Default port: UDP 7845; the host's router is asked to forward
it via UPnP, otherwise forward it manually and allow the game through the firewall.
Both PCs run the whole game in lockstep and exchange only pad input, so play feels like local split-screen
with a small input delay chosen from the ping; a RAM fingerprint every 2 s detects desyncs.

## Problems

If the game crashes or stalls, the reason and a stack trace are written to `teambuddies.log`; attach it (and
`teambuddies.ini`) to a bug report.
