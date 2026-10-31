#!/bin/sh
# Builds the Team Buddies native port on Linux (GCC or Clang, SDL3 3.4).
#   ./build.sh            recompile the game from the disc image and build everything
#   ./build.sh runtime    rebuild only the runtime (reuses the recompiled game code)
# The disc image is looked up next to this folder ("../Team Buddies.cue") unless DISC is set. SDL3 comes from
# SDL3_DIR (an install prefix; its libSDL3.so.0 is copied next to the game) or else from pkg-config.
set -eu
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$ROOT/build-linux}
DISC=${DISC:-"$ROOT/../Team Buddies.cue"}
CXX=${CXX:-g++}
JOBS=${JOBS:-$(nproc)}

SDL_CFLAGS="" SDL_LIBS=""
if [ -n "${SDL3_DIR:-}" ]; then
    : # its include and lib folders are passed quoted below (paths may have spaces)
elif pkg-config --exists sdl3 2>/dev/null; then
    SDL_CFLAGS=$(pkg-config --cflags sdl3)
    SDL_LIBS=$(pkg-config --libs sdl3)
else
    echo "SDL3 not found: install SDL 3.4 or newer, or set SDL3_DIR to where it is installed"
    exit 1
fi

# -fwrapv / -fno-strict-aliasing: the recompiled MIPS code relies on wrapping arithmetic and on reading memory
# through other types, as it does when MSVC builds it
CFLAGS="-std=c++17 -O2 -g0 -fwrapv -fno-strict-aliasing -DNDEBUG -Wall -Wno-unknown-pragmas -Wno-unused-function \
-Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-sign-compare -Wno-format-truncation -Wno-vexing-parse"
mkdir -p "$OUT/obj" "$OUT/gen" "$ROOT/generated"

# compile <folder> <extra flags> <files...>: one object each, in parallel; stops at the first error
compile() {
    DEST=$1 EXTRA=$2 SDL_INC=${SDL3_DIR:+$SDL3_DIR/include}
    shift 2
    export CXX CFLAGS ROOT DEST EXTRA SDL_INC
    printf '%s\n' "$@" | xargs -d '\n' -P "$JOBS" -I{} sh -c \
        'f="$1"; o="$DEST/$(basename "${f%.cpp}").o"
         $CXX $CFLAGS $EXTRA ${SDL_INC:+"-I$SDL_INC"} -I"$ROOT/runtime" -I"$ROOT/generated" -c "$f" -o "$o" || exit 255' _ {}
}

if [ "${1:-}" != runtime ]; then
    echo "=== building recompiler"
    $CXX $CFLAGS -w "$ROOT/recomp/recomp.cpp" -o "$OUT/recomp"
    echo "=== recompiling game code from \"$DISC\""
    rm -f "$ROOT/generated/"*.cpp
    set --
    [ -f "$ROOT/extra_seeds.txt" ] && set -- "$@" "$ROOT/extra_seeds.txt"
    [ -f "$OUT/misses.txt" ] && set -- "$@" "$OUT/misses.txt"
    "$OUT/recomp" "$DISC" "$ROOT/generated" "$@"
    echo "=== compiling recompiled code (this takes a while)"
    rm -f "$OUT/gen/"*.o
    compile "$OUT/gen" "-w" "$ROOT/generated/"*.cpp
fi

echo "=== compiling runtime"
rm -f "$OUT/obj/"*.o
# the Windows-only parts (Win32 window and Direct3D 11, GDI+ prompts, the Win32 online dialogs) are replaced by runtime/sdl
set --
for f in "$ROOT/runtime/"*.cpp "$ROOT/runtime/sdl/"*.cpp; do
    case "$(basename "$f")" in
    platform.cpp | gpu_d3d11.cpp | icon_atlas.cpp | netplay_ui.cpp) ;;
    *) set -- "$@" "$f" ;;
    esac
done
compile "$OUT/obj" "$SDL_CFLAGS" "$@"

echo "=== linking"
# -rdynamic: crash and hang reports name the recompiled functions; $ORIGIN: libSDL3.so.0 next to the game;
# the C++ runtime linked in, so the binary runs on distributions with an older one
$CXX -o "$OUT/TeamBuddies" "$OUT/obj/"*.o "$OUT/gen/"*.o ${SDL3_DIR:+"-L$SDL3_DIR/lib"} ${SDL3_DIR:+-lSDL3} $SDL_LIBS \
    -rdynamic -Wl,-rpath,'$ORIGIN' -static-libstdc++ -static-libgcc -lpthread -ldl
if [ -n "${SDL3_DIR:-}" ]; then cp -L "$SDL3_DIR/lib/libSDL3.so.0" "$OUT/"; fi
echo "=== done: $OUT/TeamBuddies"
