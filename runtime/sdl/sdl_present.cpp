// sdl_present.cpp - SDL GPU presenter: the GPU device (Vulkan), the window's swapchain and the last draw of a
// frame (present.hlsl: a region of the picture into the window's viewport). The renderer records into the frame's
// command buffer (sdl_cmd), which is submitted here. Hidden test runs have no window: the frame is then drawn
// only when a screenshot is due, into a texture that is read back.
#include "sdl_platform.h"
#include "../platform_common.h"
#include "shaders_spirv.inc"
#include <cmath>
#include <cstring>

static SDL_GPUDevice* s_gpu;
static bool s_claimed;                 // the window's swapchain belongs to the device
static SDL_GPUCommandBuffer* s_cmd;    // the frame being built
static SDL_GPUGraphicsPipeline* s_pipe_window, * s_pipe_shot; // into the swapchain / into s_shot (RGBA)
static SDL_GPUSampler* s_point, * s_linear;
static int s_vsync = -1;               // the present mode set for g_settings.vsync (-1: not yet)
static SDL_GPUTexture* s_shot;         // screenshots: the frame drawn again at the window's size, read back
static SDL_GPUTransferBuffer* s_shot_dl;
static int s_shot_w, s_shot_h;
static SDL_GPUTexture* s_image;        // CPU-made pictures (software renderer, 24-bit movies)
static SDL_GPUTransferBuffer* s_image_up;
static int s_image_w, s_image_h;

SDL_GPUDevice* sdl_gpu() { return s_gpu; }

SDL_GPUCommandBuffer* sdl_cmd() {
    if (!s_cmd && s_gpu) {
        s_cmd = SDL_AcquireGPUCommandBuffer(s_gpu);
        if (!s_cmd) fatal("The graphics device stopped working (%s).\nPlease restart the game.", SDL_GetError());
    }
    return s_cmd;
}

void sdl_submit(bool wait) {
    if (!s_cmd) return;
    SDL_GPUCommandBuffer* cmd = s_cmd;
    s_cmd = nullptr;
    if (!wait) {
        if (!SDL_SubmitGPUCommandBuffer(cmd)) log_once("gpu submit", "gpu: submit failed (%s)\n", SDL_GetError());
        return;
    }
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!f) { log_once("gpu submit", "gpu: submit failed (%s)\n", SDL_GetError()); return; }
    SDL_WaitForGPUFences(s_gpu, true, &f, 1);
    SDL_ReleaseGPUFence(s_gpu, f);
}

SDL_GPUShader* sdl_shader(const char* name) {
    for (const SpirvShader& s : k_spirv) {
        if (strcmp(s.name, name)) continue;
        SDL_GPUShaderCreateInfo ci = {};
        ci.code_size = s.size;
        ci.code = (const Uint8*)s.code;
        ci.entrypoint = s.entry;
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.stage = s.vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
        ci.num_samplers = (Uint32)s.samplers;
        ci.num_uniform_buffers = (Uint32)s.uniforms;
        SDL_GPUShader* sh = SDL_CreateGPUShader(s_gpu, &ci);
        if (!sh) plog("gpu: shader %s: %s\n", name, SDL_GetError());
        return sh;
    }
    plog("gpu: no shader %s\n", name);
    return nullptr;
}

static SDL_GPUGraphicsPipeline* present_pipeline(SDL_GPUShader* vs, SDL_GPUShader* ps, SDL_GPUTextureFormat fmt) {
    SDL_GPUColorTargetDescription ct = {};
    ct.format = fmt;
    SDL_GPUGraphicsPipelineCreateInfo pi = {};
    pi.vertex_shader = vs;
    pi.fragment_shader = ps;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(s_gpu, &pi);
    if (!p) plog("gpu: present pipeline: %s\n", SDL_GetError());
    return p;
}

static SDL_GPUSampler* make_sampler(SDL_GPUFilter filter) {
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = si.mag_filter = filter;
    si.mipmap_mode = filter == SDL_GPU_FILTER_LINEAR ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.max_lod = 1000;
    return SDL_CreateGPUSampler(s_gpu, &si);
}

// vsync on: every frame shown, at a refresh. Off: the newest frame at each refresh without tearing (as DXGI's
// flip model in a window does), else right away.
static void apply_present_mode() {
    const int want = g_settings.vsync ? 1 : 0;
    if (!s_claimed || want == s_vsync) return;
    s_vsync = want;
    SDL_Window* w = sdl_window();
    SDL_GPUPresentMode m = SDL_GPU_PRESENTMODE_VSYNC;
    if (!want) {
        if (SDL_WindowSupportsGPUPresentMode(s_gpu, w, SDL_GPU_PRESENTMODE_MAILBOX)) m = SDL_GPU_PRESENTMODE_MAILBOX;
        else if (SDL_WindowSupportsGPUPresentMode(s_gpu, w, SDL_GPU_PRESENTMODE_IMMEDIATE)) m = SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    if (SDL_SetGPUSwapchainParameters(s_gpu, w, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, m))
        plog("display: present mode %s\n", m == SDL_GPU_PRESENTMODE_VSYNC ? "vsync" : m == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "immediate");
    else
        plog("display: present mode not set (%s)\n", SDL_GetError());
}

void sdl_present_shutdown() {
    if (!s_gpu) return;
    sdl_submit(true);
    SDL_WaitForGPUIdle(s_gpu);
    if (s_shot) SDL_ReleaseGPUTexture(s_gpu, s_shot);
    if (s_shot_dl) SDL_ReleaseGPUTransferBuffer(s_gpu, s_shot_dl);
    if (s_image) SDL_ReleaseGPUTexture(s_gpu, s_image);
    if (s_image_up) SDL_ReleaseGPUTransferBuffer(s_gpu, s_image_up);
    if (s_pipe_window) SDL_ReleaseGPUGraphicsPipeline(s_gpu, s_pipe_window);
    if (s_pipe_shot) SDL_ReleaseGPUGraphicsPipeline(s_gpu, s_pipe_shot);
    if (s_point) SDL_ReleaseGPUSampler(s_gpu, s_point);
    if (s_linear) SDL_ReleaseGPUSampler(s_gpu, s_linear);
    if (s_claimed) SDL_ReleaseWindowFromGPUDevice(s_gpu, sdl_window());
    SDL_DestroyGPUDevice(s_gpu);
    s_gpu = nullptr;
    s_claimed = false;
    s_shot = s_image = nullptr;
    s_shot_dl = s_image_up = nullptr;
    s_pipe_window = s_pipe_shot = nullptr;
    s_point = s_linear = nullptr;
    s_shot_w = s_shot_h = s_image_w = s_image_h = 0;
    s_vsync = -1;
}

void sdl_present_init() {
    g_sync_interval_max = 1; // a swapchain presents at the next refresh at most: no "every Nth refresh"
    s_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!s_gpu) { plog("gpu: no SDL GPU device (%s)\n", SDL_GetError()); return; }
    SDL_PropertiesID props = SDL_GetGPUDeviceProperties(s_gpu);
    plog("gpu: %s (%s, %s)\n", SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "?"), SDL_GetGPUDeviceDriver(s_gpu),
         SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "?")));
    SDL_Window* w = sdl_window();
    if (w) {
        if (!SDL_ClaimWindowForGPUDevice(s_gpu, w)) {
            plog("gpu: the window has no swapchain (%s)\n", SDL_GetError());
            sdl_present_shutdown();
            return;
        }
        s_claimed = true;
        // one frame in flight, as with DXGI: the picture shown is the newest one drawn
        SDL_SetGPUAllowedFramesInFlight(s_gpu, 1);
        apply_present_mode();
    }
    SDL_GPUShader* vs = sdl_shader("vs_present");
    SDL_GPUShader* ps = sdl_shader("ps_present");
    if (vs && ps) {
        if (s_claimed) s_pipe_window = present_pipeline(vs, ps, SDL_GetGPUSwapchainTextureFormat(s_gpu, w));
        s_pipe_shot = present_pipeline(vs, ps, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    }
    if (vs) SDL_ReleaseGPUShader(s_gpu, vs);
    if (ps) SDL_ReleaseGPUShader(s_gpu, ps);
    s_point = make_sampler(SDL_GPU_FILTER_NEAREST);
    s_linear = make_sampler(SDL_GPU_FILTER_LINEAR);
    if (!s_pipe_shot || (s_claimed && !s_pipe_window) || !s_point || !s_linear) {
        plog("gpu: presenter not available (%s)\n", SDL_GetError());
        sdl_present_shutdown();
    }
}

static bool shot_target(int w, int h) {
    if (s_shot && s_shot_w == w && s_shot_h == h) return true;
    if (s_shot) SDL_ReleaseGPUTexture(s_gpu, s_shot);
    if (s_shot_dl) SDL_ReleaseGPUTransferBuffer(s_gpu, s_shot_dl);
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    s_shot = SDL_CreateGPUTexture(s_gpu, &ti);
    SDL_GPUTransferBufferCreateInfo bi = {};
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    bi.size = (Uint32)w * (Uint32)h * 4;
    s_shot_dl = SDL_CreateGPUTransferBuffer(s_gpu, &bi);
    s_shot_w = w;
    s_shot_h = h;
    if (s_shot && s_shot_dl) return true;
    plog("gpu: no screenshot buffer %dx%d (%s)\n", w, h, SDL_GetError());
    if (s_shot) SDL_ReleaseGPUTexture(s_gpu, s_shot);
    if (s_shot_dl) SDL_ReleaseGPUTransferBuffer(s_gpu, s_shot_dl);
    s_shot = nullptr;
    s_shot_dl = nullptr;
    s_shot_w = s_shot_h = 0;
    return false;
}

static void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, int tw, int th, SDL_GPUGraphicsPipeline* pipe,
                 SDL_GPUTexture* tex, float u0, float v0, float u1, float v1, int native_h, bool enabled) {
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = target;
    ct.clear_color = {0, 0, 0, 1};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (!rp) return;
    if (enabled && tex) {
        double vw, vh;
        picture_size(tw, th, native_h, vw, vh);
        SDL_GPUViewport vp = {(float)std::floor((tw - vw) / 2), (float)std::floor((th - vh) / 2), (float)vw, (float)vh, 0, 1};
        SDL_SetGPUViewport(rp, &vp);
        SDL_BindGPUGraphicsPipeline(rp, pipe);
        const float rect[4] = {u0, v0, u1, v1};
        SDL_PushGPUVertexUniformData(cmd, 0, rect, sizeof rect);
        SDL_GPUTextureSamplerBinding b = {tex, g_settings.bilinear ? s_linear : s_point};
        SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
        SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
}

bool sdl_show(SDL_GPUTexture* tex, int tex_w, int tex_h, float u0, float v0, float u1, float v1, int native_h, bool enabled) {
    if (!s_gpu) return false;
    int ww, wh;
    platform_get_window_size(ww, wh);
    g_present_wait = 0;
    if (ww <= 0 || wh <= 0) { sdl_submit(false); return false; } // minimized: nothing to draw
    update_widescreen_factor(ww, wh);
    apply_present_mode();
    if (tex_w <= 0 || tex_h <= 0) tex = nullptr;
    SDL_GPUCommandBuffer* cmd = sdl_cmd();
    const bool shot = shot_due() && shot_target(ww, wh);
    if (shot) {
        draw(cmd, s_shot, ww, wh, s_pipe_shot, tex, u0, v0, u1, v1, native_h, enabled);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src = {};
        src.texture = s_shot;
        src.w = (Uint32)ww;
        src.h = (Uint32)wh;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst = {};
        dst.transfer_buffer = s_shot_dl;
        dst.pixels_per_row = (Uint32)ww;
        dst.rows_per_layer = (Uint32)wh;
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
    }
    bool shown = !s_claimed; // hidden runs show nothing, but the frame counts as presented
    if (s_claimed) {
        SDL_GPUTexture* swap = nullptr;
        Uint32 sw = 0, sh = 0;
        const long long t0 = plat_ticks();
        // waits while the frame before is still queued (vsync), as DXGI's Present does
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, sdl_window(), &swap, &sw, &sh))
            log_once("gpu acquire", "gpu: no swapchain texture (%s)\n", SDL_GetError());
        g_present_wait = plat_ticks() - t0;
        if (swap) {
            draw(cmd, swap, (int)sw, (int)sh, s_pipe_window, tex, u0, v0, u1, v1, native_h, enabled);
            shown = true;
        }
    }
    sdl_submit(shot);
    if (shot) {
        if (const u8* p = (const u8*)SDL_MapGPUTransferBuffer(s_gpu, s_shot_dl, false)) {
            shot_save(p, ww, wh, ww * 4);
            SDL_UnmapGPUTransferBuffer(s_gpu, s_shot_dl);
        }
    }
    return shown;
}

void sdl_present_texture(SDL_GPUTexture* tex, int tex_w, int tex_h, float u0, float v0, float u1, float v1, int native_h, bool enabled) {
    g_sync_interval = 1;
    sdl_show(tex, tex_w, tex_h, u0, v0, u1, v1, native_h, enabled);
    platform_frame_pace();
    platform_pump();
}

static bool image_target(int w, int h) {
    if (s_image && s_image_w == w && s_image_h == h) return true;
    if (s_image) SDL_ReleaseGPUTexture(s_gpu, s_image);
    if (s_image_up) SDL_ReleaseGPUTransferBuffer(s_gpu, s_image_up);
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    s_image = SDL_CreateGPUTexture(s_gpu, &ti);
    SDL_GPUTransferBufferCreateInfo bi = {};
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = (Uint32)w * (Uint32)h * 4;
    s_image_up = SDL_CreateGPUTransferBuffer(s_gpu, &bi);
    s_image_w = w;
    s_image_h = h;
    if (s_image && s_image_up) return true;
    plog("gpu: no picture texture %dx%d (%s)\n", w, h, SDL_GetError());
    if (s_image) SDL_ReleaseGPUTexture(s_gpu, s_image);
    if (s_image_up) SDL_ReleaseGPUTransferBuffer(s_gpu, s_image_up);
    s_image = nullptr;
    s_image_up = nullptr;
    s_image_w = s_image_h = 0;
    return false;
}

bool sdl_present_image(const u32* rgba, int w, int h, int stride, bool enabled) {
    if (!s_gpu) return false;
    g_sync_interval = 1;
    if (w > 0 && h > 0 && image_target(w, h)) {
        // cycle: the previous picture's upload may still be in flight
        if (u8* p = (u8*)SDL_MapGPUTransferBuffer(s_gpu, s_image_up, true)) {
            for (int y = 0; y < h; y++) memcpy(p + (size_t)y * w * 4, rgba + (size_t)y * stride, (size_t)w * 4);
            SDL_UnmapGPUTransferBuffer(s_gpu, s_image_up);
            SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(sdl_cmd());
            SDL_GPUTextureTransferInfo src = {};
            src.transfer_buffer = s_image_up;
            src.pixels_per_row = (Uint32)w;
            src.rows_per_layer = (Uint32)h;
            SDL_GPUTextureRegion dst = {};
            dst.texture = s_image;
            dst.w = (Uint32)w;
            dst.h = (Uint32)h;
            dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, true);
            SDL_EndGPUCopyPass(cp);
        }
    }
    sdl_show(s_image, s_image_w, s_image_h, 0, 0, 1, 1, h, enabled);
    return true;
}
