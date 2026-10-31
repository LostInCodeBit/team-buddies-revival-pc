// sdl_platform.h - between the SDL3 platform layer (platform_sdl.cpp), its GPU presenter (sdl_present.cpp)
// and the SDL GPU renderer (gpu_sdlgpu.cpp).
#pragma once
#include <SDL3/SDL.h>
#include "../psx.h"

SDL_Window* sdl_window();   // nullptr in hidden test runs
SDL_GPUDevice* sdl_gpu();   // nullptr when no GPU device could be made (frames are then only screenshots)
void sdl_present_init();
void sdl_present_shutdown();
// a CPU-made picture to the window (software renderer, 24-bit movies); false without a GPU device
bool sdl_present_image(const u32* rgba, int w, int h, int stride, bool enabled);
// draw a region of a texture to the window with the configured aspect ratio and filtering, take a screenshot
// if one is due and present; false when minimized. sdl_present_texture also waits for the next vblank (frame
// pacing, input).
bool sdl_show(SDL_GPUTexture* tex, int tex_w, int tex_h, float u0, float v0, float u1, float v1, int native_h, bool enabled);
void sdl_present_texture(SDL_GPUTexture* tex, int tex_w, int tex_h, float u0, float v0, float u1, float v1, int native_h, bool enabled);
// the command buffer of the frame being built (acquired on first use): the renderer records into it, the
// presenter submits it. sdl_submit submits it early, wait = until the GPU has run it (readbacks).
SDL_GPUCommandBuffer* sdl_cmd();
void sdl_submit(bool wait);
// a shader from shaders_spirv.inc by name (nullptr, logged, if it cannot be made); the caller releases it
SDL_GPUShader* sdl_shader(const char* name);
