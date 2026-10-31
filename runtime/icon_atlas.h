// icon_atlas.h - the atlas of keyboard and Xbox button glyphs that replace the game's PlayStation prompts
// (icons.cpp): its layout, painted once at startup by icon_atlas_build (icon_atlas.cpp: GDI+ on Windows;
// sdl/icon_atlas_draw.cpp: drawn by hand in the SDL build) and kept by icon_atlas_common.cpp.
#pragma once
#include "psx.h"
#include <vector>

const int ICON_CELL = 256, ICON_ROWS = 2; // row 0 = Xbox, row 1 = keyboard, one cell per IconId

// on-screen width/height of the game's sprite for each icon (32x16 texels in 640-wide modes = square)
inline float icon_aspect(int id) { return id == ICON_START || id == ICON_SELECT ? 1.5f : 1.0f; }

// the painted atlas: premultiplied BGRA, (ICON_COUNT * ICON_CELL) x (ICON_ROWS * ICON_CELL); has = which cells
// hold a glyph
void icon_atlas_set(std::vector<u32>&& pixels, const bool (&has)[ICON_ROWS][ICON_COUNT]);
