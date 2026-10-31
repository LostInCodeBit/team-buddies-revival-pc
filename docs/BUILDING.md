# Building

Building needs your own disc image of *Team Buddies* (USA, SLUS-00869): the recompiler reads the game's
executables from it. Nothing from the game is in this repository.

Stack: C++17, MSVC (Visual Studio 2022 Build Tools, C++ workload + Windows SDK), Direct3D 11, XAudio2,
Direct2D/DirectWrite (launcher), SDL3 3.4 for controllers (`third_party/SDL3`, zlib licence; `SDL3.dll` is
copied next to the exe).

```
build.bat            :: recompile from "..\Team Buddies.cue" and build everything
build.bat runtime    :: rebuild only the runtime (and the launcher)
build.bat launcher   :: rebuild only the launcher
```

Set `DISC=path\to\game.cue` to use another location for the disc image. The result is in `build\`
(`TeamBuddiesLauncher.exe`, `TeamBuddies.exe`, `SDL3.dll`). `build.bat sdl` makes `build\TeamBuddiesSDL.exe`,
a test build of the Linux code paths (SDL3 platform layer, Vulkan renderer) on Windows; it is not shipped.

**Linux:** GCC or Clang (C++17) and SDL 3.4 (`pkg-config sdl3`, or `SDL3_DIR` = its install prefix, whose
`libSDL3.so.0` is then copied next to the game). The window, sound, controllers and the Vulkan renderer (SDL
GPU) come from SDL3; there is no launcher.

```
./build.sh            # recompile from "../Team Buddies.cue" and build everything
./build.sh runtime    # rebuild only the runtime
```

`DISC`, `OUT` (default `build-linux/`), `CXX` and `JOBS` can be set. The game links its C++ runtime in and
finds `libSDL3.so.0` next to itself, so the build folder runs as it is.

**Shaders:** `runtime/shaders/*.hlsl` are the one source of both renderers. After editing them run
`python tools/build_shaders.py`: it writes the HLSL includes that Direct3D 11 compiles at start and the
SPIR-V for Vulkan (`runtime/sdl/shaders_spirv.inc`), with DXC (SPIR-V support; `DXC` = its path). The outputs
are committed, so building needs no shader compiler.

## Layout

| Folder | What it is |
|---|---|
| `recomp/` | The recompiler: reads the executables from the disc image and writes C++ to `generated/` |
| `runtime/` | Native replacement of the PS1 hardware and kernel, renderers, input, audio; the Win32 / Direct3D 11 platform |
| `runtime/sdl/` | The SDL3 platform (Linux): window, audio, input, Vulkan presenter and renderer, online dialogs, UPnP |
| `runtime/shaders/` | The renderers' HLSL (`gpu.hlsl`, `present.hlsl`) and what `tools/build_shaders.py` makes of it |
| `launcher/` | `TeamBuddiesLauncher.exe`: settings, disc choice, patch notes and Play (Direct2D) |
| `server/` | `tbmatch`, a matchmaking and relay server for online play (build line at the top of `tbmatch.cpp`) |
| `tools/` | Small helpers (disc file lister/extractor, icon, HD text catalogue, release package) |
| `generated/` | Output of the recompiler: the game's code, never committed or published on its own |
| `build/` | Build output |

## How it works

1. `recomp` parses `SYSTEM.CNF` and `OVERLAY.DAT` to find the main executable and its code overlays (SYS,
   ENG, GAME, MNU, ROT, MPLR, TUTO), discovers functions (call targets, function pointers in data, jump
   tables) and emits one C++ function per MIPS function.
2. Overlays that share an address (GAME/MNU/ROT/MPLR) are told apart at run time by checking which one is
   currently in memory.
3. Code the recompiler missed runs in a fallback interpreter and is logged to `misses.txt`; `build.bat`
   feeds `build\misses.txt` (and an optional hand-written `extra_seeds.txt`) back into the recompiler, so a
   rebuild turns them into native code.

## Testing

The game runs unattended with test-only environment variables (listed in `CHANGELOG.md`): `TB_INPUT` plays a
scripted pad (`700=start;712=;...`: pressed at vblank 700, released at 712), `TB_FRAMES` quits after that many
vblanks, `TB_SHOT_AT` / `TB_SHOT_DIR` save screenshots, `TB_HIDDEN` runs without a window or sound, `TB_TURBO`
fast-forwards. With `log_level = 2` the log has a RAM fingerprint per second: emulation must stay deterministic,
so the same input gives the same fingerprints whatever the graphics settings.

## Release package

`tools\package_release.ps1 -Version 0.2.0` takes a fresh `build\` and writes
`dist\TeamBuddies-0.2.0-win.zip` (launcher, game, `SDL3.dll`, `README.txt`, `CHANGELOG.md`, `LICENSE`,
`licenses\`) and `dist\SHA256SUMS.txt`. It copies an explicit list of files, so settings, logs, saves,
debug files and anything from the disc never end up in it. Upload both to a GitHub release.

On Linux, `tools/package_linux.sh 0.2.0 <source url>` does the same from `build-linux/` (or `OUT`):
`dist/TeamBuddies-0.2.0-linux-x86_64.tar.gz` (game, `libSDL3.so.0`, `README.txt`, docs, licences), and its
line in `dist/SHA256SUMS.txt` next to the Windows zip's.
