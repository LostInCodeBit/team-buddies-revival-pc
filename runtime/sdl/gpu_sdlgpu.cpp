// gpu_sdlgpu.cpp - hardware renderer on the SDL GPU API (Vulkan): the backend of gpu_hw.h, as gpu_d3d11.cpp is
// for Direct3D 11, with the same shaders (gpu.hlsl, as SPIR-V). SDL GPU works in passes: each draw or copy here
// records a pass of its own into the frame's command buffer (sdl_cmd). A multisampled target cannot be sampled or
// copied, so it is resolved into its single-sample copy by an empty render pass.
#include "sdl_platform.h"
#include "../gpu_hw.h"
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

namespace {

enum Prog { P_MAIN, P_SAMPLE, P_UPLOAD, P_FILL, P_COPY, P_DOWN, P_GLYPH, P_FXAA, P_COPY_FULL, P_COUNT };
struct ProgDesc { const char* vs; const char* ps; SDL_GPUPrimitiveType prim; bool vertices; };
const ProgDesc k_progs[P_COUNT] = {
    {"vs_main", "ps_main", SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, true},
    {"vs_main", "ps_sample", SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, true}, // ps_main shaded per sample (SSAA)
    {"vs_rect", "ps_upload", SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP, false},
    {"vs_rect", "ps_fill", SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP, false},
    {"vs_rect", "ps_copy", SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP, false},
    {"vs_rect", "ps_down", SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP, false},
    {"vs_full", "ps_glyph", SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, false},
    {"vs_full", "ps_fxaa", SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, false},
    {"vs_full", "ps_copy", SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, false},  // a whole target from copy (in-between frames)
};
const int SLOTS = 7;                     // gpu.hlsl's texture slots: every one a shader declares must be bound
const Uint32 RING = 8u << 20;            // bytes in each upload ring
const int VB_CHUNK = (1 << 17) / 6 * 6;  // vertices per draw: whole triangles (and rectangles)
const SDL_GPUTextureFormat RGBA8 = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

SDL_GPUSampleCount sample_count(int ms) {
    return ms >= 8 ? SDL_GPU_SAMPLECOUNT_8 : ms >= 4 ? SDL_GPU_SAMPLECOUNT_4 : ms >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
}

// the part of a rectangle (x1 / y1 exclusive) inside a tw x th target; false if none
bool clip(int x0, int y0, int x1, int y1, int tw, int th, SDL_Rect& r) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, tw); y1 = std::min(y1, th);
    if (x1 <= x0 || y1 <= y0) return false;
    r = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

} // namespace

struct SdlGpuRenderer : HwRenderer {
    SDL_GPUDevice* dev;
    bool ok = false;                     // everything a frame needs was made (else: the software renderer)
    std::map<std::string, SDL_GPUShader*> shaders;
    std::map<u32, SDL_GPUGraphicsPipeline*> pipes; // made when first needed, per target format and sample count
    SDL_GPUSampler* lin = nullptr;
    // rt: the upscaled VRAM the game draws into (multisampled with MSAA / SSAA); copy: its single-sample copy,
    // kept current per tile (multisampled: resolved whole), that textures, readbacks and the screen read from
    SDL_GPUTexture* rt = nullptr, * copy = nullptr;
    SDL_GPUTexture* up = nullptr;        // native pixels on their way into rt (ps_upload)
    SDL_GPUTexture* down_rt = nullptr;   // rt downsampled to native size for readbacks (ps_down)
    SDL_GPUTexture* dummy = nullptr;     // for the texture slots a draw does not use
    SDL_GPUTexture* icon = nullptr;      // button glyph atlas (icon_atlas.cpp), with a mip chain
    SDL_GPUTexture* glyph_tex = nullptr; // HD letters: the atlas (mip chain) and one letter's texels (ps_glyph's input)
    SDL_GPUTexture* glyph_src = nullptr;
    SDL_GPUTexture* post = nullptr;      // FXAA output: the displayed area at the internal resolution
    int post_w = 0, post_h = 0;
    // in-between frames (interp.cpp) are drawn into irt, multisampled like rt; irt_res is then its resolved picture
    SDL_GPUTexture* irt = nullptr, * irt_res = nullptr;
    int irt_w = 0, irt_h = 0, irt_ms = 0;
    // uploads: rings of transfer buffers, written at increasing offsets, cycled (a fresh buffer while the GPU may
    // still read the old one) when full; the vertex buffer follows its ring offset for offset
    struct Ring { SDL_GPUTransferBuffer* tb = nullptr; Uint32 cur = 0; };
    Ring vring, uring;
    SDL_GPUBuffer* vbuf = nullptr;
    SDL_GPUTransferBuffer* dl = nullptr; // readbacks

    explicit SdlGpuRenderer(u16* v) : HwRenderer(v), dev(sdl_gpu()) {
        for (const ProgDesc& p : k_progs)
            for (const char* name : {p.vs, p.ps})
                if (!shaders.count(name) && !(shaders[name] = sdl_shader(name))) return;
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.max_lod = 1000;
        lin = SDL_CreateGPUSampler(dev, &si);
        SDL_GPUTransferBufferCreateInfo ti = {};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = RING;
        vring.tb = SDL_CreateGPUTransferBuffer(dev, &ti);
        uring.tb = SDL_CreateGPUTransferBuffer(dev, &ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = 1024 * 512 * 4;
        dl = SDL_CreateGPUTransferBuffer(dev, &ti);
        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = RING;
        vbuf = SDL_CreateGPUBuffer(dev, &bi);
        up = make_texture(1024, 512, RGBA8, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        down_rt = make_texture(1024, 512, RGBA8, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        dummy = make_texture(1, 1, RGBA8, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        if (!lin || !vring.tb || !uring.tb || !dl || !vbuf || !up || !down_rt || !dummy) {
            plog("gpu: renderer resources: %s\n", SDL_GetError());
            return;
        }
        clear(dummy);
        // the pipelines of every frame now, so a driver that cannot make them is caught before the game starts
        if (!pipe(P_MAIN, 0, RGBA8, 1) || !pipe(P_UPLOAD, 0, RGBA8, 1) || !pipe(P_FILL, 0, RGBA8, 1) || !pipe(P_DOWN, 0, RGBA8, 1)) return;
        create_icon_atlas();
        create_glyph_atlas();
        create_targets(g_settings.scale, aa_samples(g_settings.anti_aliasing));
        upload_rect(0, 0, 1024, 512);
        ok = true;
    }

    ~SdlGpuRenderer() override {
        for (auto& p : pipes) if (p.second) SDL_ReleaseGPUGraphicsPipeline(dev, p.second);
        for (auto& s : shaders) if (s.second) SDL_ReleaseGPUShader(dev, s.second);
        for (SDL_GPUTexture* t : {rt, copy, up, down_rt, dummy, icon, glyph_tex, glyph_src, post, irt, irt_res})
            if (t) SDL_ReleaseGPUTexture(dev, t);
        for (SDL_GPUTransferBuffer* b : {vring.tb, uring.tb, dl}) if (b) SDL_ReleaseGPUTransferBuffer(dev, b);
        if (vbuf) SDL_ReleaseGPUBuffer(dev, vbuf);
        if (lin) SDL_ReleaseGPUSampler(dev, lin);
    }

    // ------------------------------------------------------------ SDL GPU helpers
    SDL_GPUTexture* make_texture(int w, int h, SDL_GPUTextureFormat fmt, int ms, SDL_GPUTextureUsageFlags usage, int levels = 1) {
        SDL_GPUTextureCreateInfo ti = {};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = fmt;
        ti.usage = usage;
        ti.width = (Uint32)w;
        ti.height = (Uint32)h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = (Uint32)levels;
        ti.sample_count = sample_count(ms);
        return SDL_CreateGPUTexture(dev, &ti);
    }

    static void release(SDL_GPUDevice* d, SDL_GPUTexture*& t) { if (t) SDL_ReleaseGPUTexture(d, t); t = nullptr; }

    SDL_GPUGraphicsPipeline* pipe(int prog, int blend, SDL_GPUTextureFormat fmt, int ms) {
        const u32 key = (u32)prog | (u32)blend << 4 | (u32)fmt << 8 | (u32)ms << 24;
        auto it = pipes.find(key);
        if (it != pipes.end()) return it->second;
        const ProgDesc& p = k_progs[prog];
        SDL_GPUColorTargetDescription ct = {};
        ct.format = fmt;
        if (blend > 0) { // blend - 1 = the PS1 mode: B/2 + F/2, B + F, B - F, B + F/4 (the shader scales F)
            ct.blend_state.enable_blend = true;
            ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
            ct.blend_state.color_blend_op = blend - 1 == 2 ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
            ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        const SDL_GPUVertexBufferDescription vbd = {0, sizeof(HwVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const SDL_GPUVertexAttribute va[] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},       // POSITION
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 8},  // COLOR
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12},      // TEXCOORD
            {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, 20},       // ATTR
            {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, 28},      // BOX
        };
        SDL_GPUGraphicsPipelineCreateInfo pi = {};
        pi.vertex_shader = shaders[p.vs];
        pi.fragment_shader = shaders[p.ps];
        if (p.vertices) {
            pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
            pi.vertex_input_state.num_vertex_buffers = 1;
            pi.vertex_input_state.vertex_attributes = va;
            pi.vertex_input_state.num_vertex_attributes = 5;
        }
        pi.primitive_type = p.prim;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.multisample_state.sample_count = sample_count(ms);
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* g = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!g) plog("gpu: pipeline %s + %s, %d samples: %s\n", p.vs, p.ps, ms, SDL_GetError());
        pipes[key] = g; // a failure too: not tried again at every draw
        return g;
    }

    SDL_GPURenderPass* begin(SDL_GPUTexture* target, SDL_GPULoadOp load = SDL_GPU_LOADOP_LOAD) {
        SDL_GPUColorTargetInfo ct = {};
        ct.texture = target;
        ct.load_op = load;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        return SDL_BeginGPURenderPass(sdl_cmd(), &ct, 1, nullptr);
    }

    void clear(SDL_GPUTexture* t) {
        if (SDL_GPURenderPass* rp = begin(t, SDL_GPU_LOADOP_CLEAR)) SDL_EndGPURenderPass(rp); // clear_color 0, 0, 0, 0
    }

    // a multisampled texture's average per pixel into a single-sample one (all of it)
    void resolve(SDL_GPUTexture* ms_tex, SDL_GPUTexture* into) {
        SDL_GPUColorTargetInfo ct = {};
        ct.texture = ms_tex;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
        ct.resolve_texture = into;
        if (SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(sdl_cmd(), &ct, 1, nullptr)) SDL_EndGPURenderPass(rp);
    }

    // texture slots: the given ones, the others a dummy (a target being drawn into is never bound)
    void bind(SDL_GPURenderPass* rp, std::initializer_list<std::pair<int, SDL_GPUTexture*>> set) {
        SDL_GPUTextureSamplerBinding b[SLOTS];
        for (SDL_GPUTextureSamplerBinding& x : b) x = {dummy, lin};
        for (const auto& s : set) if (s.second) b[s.first].texture = s.second;
        SDL_BindGPUFragmentSamplers(rp, 0, b, SLOTS);
    }

    void push(const DrawCB& c) {
        SDL_GPUCommandBuffer* cmd = sdl_cmd();
        SDL_PushGPUVertexUniformData(cmd, 0, &c, sizeof c);
        SDL_PushGPUFragmentUniformData(cmd, 0, &c, sizeof c);
    }

    // n bytes into a ring, 16-byte aligned; cycled = it started over in a fresh buffer
    bool stage(Ring& r, const void* data, Uint32 n, Uint32& off, bool& cycled) {
        if (n == 0 || n > RING) return false;
        cycled = r.cur + n > RING;
        if (cycled) r.cur = 0;
        u8* p = (u8*)SDL_MapGPUTransferBuffer(dev, r.tb, cycled);
        if (!p) { log_once("gpu map", "gpu: transfer buffer not mapped (%s)\n", SDL_GetError()); return false; }
        memcpy(p + r.cur, data, n);
        SDL_UnmapGPUTransferBuffer(dev, r.tb);
        off = r.cur;
        r.cur = (r.cur + n + 15) & ~15u;
        return true;
    }

    // native pixels (pixels_per_row wide in memory) into a region of a texture
    void upload(SDL_GPUTexture* t, int x, int y, int w, int h, const void* px, int pixels_per_row, int bpp) {
        Uint32 off;
        bool cycled;
        if (!stage(uring, px, (Uint32)(pixels_per_row * (h - 1) + w) * bpp, off, cycled)) return;
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(sdl_cmd());
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = uring.tb;
        src.offset = off;
        src.pixels_per_row = (Uint32)pixels_per_row;
        src.rows_per_layer = (Uint32)h;
        SDL_GPUTextureRegion dst = {};
        dst.texture = t;
        dst.x = (Uint32)x;
        dst.y = (Uint32)y;
        dst.w = (Uint32)w;
        dst.h = (Uint32)h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
    }

    SDL_GPUTexture* target() const { return redirect ? irt : rt; }
    void target_size(int& w, int& h) const { w = redirect ? irt_w : 1024 * S; h = redirect ? irt_h : 512 * S; }

    // into rt (or irt): native VRAM coordinates through the shifted viewport, the scissor in target pixels
    bool target_view(SDL_GPURenderPass* rp, int x0, int y0, int x1, int y1) {
        int tw, th;
        target_size(tw, th);
        SDL_Rect sc;
        if (!clip(x0, y0, x1, y1, tw, th, sc)) return false;
        const SDL_GPUViewport vp = {(float)-tox, (float)-toy, 1024.0f * S, 512.0f * S, 0, 1};
        SDL_SetGPUViewport(rp, &vp);
        SDL_SetGPUScissor(rp, &sc);
        return true;
    }

    // ------------------------------------------------------------ targets
    void create_targets(int scale, int samples) override {
        release(dev, rt); release(dev, copy); release(dev, irt); release(dev, irt_res);
        irt_w = irt_h = irt_ms = 0;
        S = scale;
        want_ms = samples;
        // samples: as many as asked that the GPU supports, keeping the render target within 512 MB (8x
        // resolution: 4 samples at most)
        const double per_sample = 1024.0 * S * 512.0 * S * 4;
        MS = samples;
        while (MS > 1 && (per_sample * MS > 512.0 * 1024 * 1024 || !SDL_GPUTextureSupportsSampleCount(dev, RGBA8, sample_count(MS)))) MS /= 2;
        // multisampled: SDL samples only its resolved copy
        auto make_rt = [&] { return make_texture(1024 * S, 512 * S, RGBA8, MS, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (MS == 1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0)); };
        while (!(rt = make_rt())) { // out of video memory: fewer samples
            if (MS == 1) fatal("cannot create %dx VRAM render target", S);
            MS /= 2;
        }
        copy = make_texture(1024 * S, 512 * S, RGBA8, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (!copy) fatal("cannot create %dx VRAM copy", S);
        clear(rt);
        if (MS > 1) plog("hardware renderer: %dx internal resolution (%dx%d VRAM), %d samples%s\n", S, 1024 * S, 512 * S, MS,
                         MS < samples ? " (fewer than asked: GPU or memory)" : "");
        else plog("hardware renderer: %dx internal resolution (%dx%d VRAM)\n", S, 1024 * S, 512 * S);
    }

    void sync_copy() override {
        if (!any_dirty) return;
        if (MS > 1) { // the whole target: SDL resolves render passes, not regions
            resolve(rt, copy);
            memset(dirty, 0, sizeof dirty);
            any_dirty = false;
            return;
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(sdl_cmd());
        for (int ty = 0; ty < 16; ty++)
            for (int tx = 0; tx < 32;) {
                if (!dirty[ty][tx]) { tx++; continue; }
                int t0 = tx;
                while (tx < 32 && dirty[ty][tx]) { dirty[ty][tx] = false; tx++; }
                const Uint32 x = (Uint32)(t0 * 32 * S), y = (Uint32)(ty * 32 * S);
                const SDL_GPUTextureLocation src = {rt, 0, 0, x, y, 0}, dst = {copy, 0, 0, x, y, 0};
                SDL_CopyGPUTextureToTexture(cp, &src, &dst, (Uint32)((tx - t0) * 32 * S), (Uint32)(32 * S), 1, false);
            }
        SDL_EndGPUCopyPass(cp);
        any_dirty = false;
    }

    // ------------------------------------------------------------ atlases
    // button glyph atlas with a mip chain, so the glyphs stay clean at every internal resolution
    void create_icon_atlas() {
        int aw, ah;
        const u32* px = icon_atlas_pixels(aw, ah);
        if (!px) return;
        int levels = 1;
        while ((std::max(aw, ah) >> levels) > 0) levels++;
        icon = make_texture(aw, ah, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, levels);
        if (!icon) { plog("icons: cannot create glyph atlas\n"); return; }
        upload(icon, 0, 0, aw, ah, px, aw, 4);
        SDL_GenerateMipmapsForGPUTexture(sdl_cmd(), icon);
    }

    void create_glyph_atlas() {
        const SDL_GPUTextureFormat f = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
        if (!SDL_GPUTextureSupportsFormat(dev, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) {
            plog("hd text: the GPU cannot draw into two-channel textures\n");
            return;
        }
        int levels = 1;
        while ((GLYPH_ATLAS >> levels) > 0) levels++;
        glyph_tex = make_texture(GLYPH_ATLAS, GLYPH_ATLAS, f, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, levels);
        if (!glyph_tex) { plog("hd text: cannot create the glyph atlas\n"); return; }
        glyph_src = make_texture(GLYPH_SRC_W, GLYPH_SRC_H, f, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (!glyph_src) {
            plog("hd text: cannot create the glyph source texture\n");
            release(dev, glyph_tex); // glyph_place then always declines
            return;
        }
        glyph_ok = true;
        glyph_reset();
    }

    // reconstruct letter g into the atlas cell at x, y (w x h pixels) on the GPU
    void glyph_draw(const HdGlyph& g, int x, int y, int w, int h) override {
        u8 src[GLYPH_SRC_W * GLYPH_SRC_H * 2];
        int fw, fh;
        hd_text_source(g, src, GLYPH_SRC_W, fw, fh);
        if (fw > 0 && fh > 0) upload(glyph_src, 0, 0, fw, fh, src, GLYPH_SRC_W, 2);
        SDL_GPUGraphicsPipeline* p = pipe(P_GLYPH, 0, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, 1);
        SDL_GPURenderPass* rp = p ? begin(glyph_tex) : nullptr;
        if (!rp) return;
        const SDL_GPUViewport vp = {(float)x, (float)y, (float)w, (float)h, 0, 1};
        SDL_SetGPUViewport(rp, &vp);
        const SDL_Rect sc = {x, y, w, h};
        SDL_SetGPUScissor(rp, &sc);
        SDL_BindGPUGraphicsPipeline(rp, p);
        const float r[4] = {(float)x, (float)y, (float)g.style, 0};
        push(make_cb(0, 1.0f, r));
        bind(rp, {{6, glyph_src}}); // slot 5 (the atlas) stays a dummy: it is the target
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
    }

    void glyph_clear() override { if (glyph_tex) clear(glyph_tex); }

    // ------------------------------------------------------------ batching
    void flush() override {
        if (batch.empty()) return;
        if (glyph_mips && glyph_ok) { SDL_GenerateMipmapsForGPUTexture(sdl_cmd(), glyph_tex); glyph_mips = false; }
        // SSAA shades every sample (a GPU without sample-rate shading gets MSAA instead)
        const int prog = MS > 1 && aa_per_sample(g_settings.anti_aliasing) && pipe(P_SAMPLE, 0, RGBA8, MS) ? P_SAMPLE : P_MAIN;
        SDL_GPUGraphicsPipeline* opaque = pipe(prog, 0, RGBA8, MS);
        SDL_GPUGraphicsPipeline* blend = batch_kind != 0 ? pipe(prog, 1 + batch_mode, RGBA8, MS) : nullptr;
        static const float factors[4] = {0.5f, 1.0f, 1.0f, 0.25f};
        const float bfv = batch_mode == 0 ? 0.5f : 1.0f;
        const SDL_FColor bf = {bfv, bfv, bfv, 1.0f};
        for (size_t done = 0; done < batch.size();) {
            const size_t n = std::min(batch.size() - done, (size_t)VB_CHUNK);
            Uint32 off;
            bool cycled;
            if (!stage(vring, batch.data() + done, (Uint32)(n * sizeof(HwVertex)), off, cycled)) break;
            SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(sdl_cmd());
            const SDL_GPUTransferBufferLocation src = {vring.tb, off};
            const SDL_GPUBufferRegion dst = {vbuf, off, (Uint32)(n * sizeof(HwVertex))};
            SDL_UploadToGPUBuffer(cp, &src, &dst, cycled);
            SDL_EndGPUCopyPass(cp);
            done += n;
            if (!opaque || (batch_kind != 0 && !blend)) continue;
            SDL_GPURenderPass* rp = begin(target());
            if (!rp) continue;
            if (target_view(rp, batch_scissor.x0, batch_scissor.y0, batch_scissor.x1, batch_scissor.y1)) {
                const SDL_GPUBufferBinding vb = {vbuf, off};
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                if (batch_kind == 0) {
                    SDL_BindGPUGraphicsPipeline(rp, opaque);
                    bind(rp, {{0, copy}, {2, icon}, {5, glyph_tex}});
                    push(make_cb(0, 1.0f));
                    SDL_DrawGPUPrimitives(rp, (Uint32)n, 1, 0, 0);
                } else if (batch_kind == 1) {
                    SDL_BindGPUGraphicsPipeline(rp, blend);
                    bind(rp, {{0, copy}, {2, icon}, {5, glyph_tex}});
                    SDL_SetGPUBlendConstants(rp, bf);
                    push(make_cb(0, factors[batch_mode]));
                    SDL_DrawGPUPrimitives(rp, (Uint32)n, 1, 0, 0);
                } else {
                    SDL_BindGPUGraphicsPipeline(rp, opaque);
                    bind(rp, {{0, copy}, {2, icon}, {5, glyph_tex}});
                    push(make_cb(1, 1.0f));
                    SDL_DrawGPUPrimitives(rp, (Uint32)n, 1, 0, 0);
                    SDL_BindGPUGraphicsPipeline(rp, blend);
                    bind(rp, {{0, copy}, {2, icon}, {5, glyph_tex}});
                    SDL_SetGPUBlendConstants(rp, bf);
                    push(make_cb(2, factors[batch_mode]));
                    SDL_DrawGPUPrimitives(rp, (Uint32)n, 1, 0, 0);
                }
            }
            SDL_EndGPURenderPass(rp);
        }
        batch.clear();
    }

    // ------------------------------------------------------------ vram operations
    void draw_rect_pass(int prog, SDL_GPUTexture* tex, int x, int y, int w, int h, const float* col) {
        SDL_GPUGraphicsPipeline* p = pipe(prog, 0, RGBA8, MS);
        if (SDL_GPURenderPass* rp = p ? begin(target()) : nullptr) {
            if (target_view(rp, x * S - tox, y * S - toy, (x + w) * S - tox, (y + h) * S - toy)) {
                SDL_BindGPUGraphicsPipeline(rp, p);
                const float r[4] = {(float)(x * S), (float)(y * S), (float)((x + w) * S), (float)((y + h) * S)};
                push(make_cb(0, 1.0f, r, col));
                bind(rp, {{1, tex}});
                SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
            }
            SDL_EndGPURenderPass(rp);
        }
        if (!redirect) mark_dirty(x, y, x + w - 1, y + h - 1);
    }

    void upload_pixels(int x, int y, int w, int h, const u32* rgba) override {
        upload(up, x, y, w, h, rgba, w, 4);
        draw_rect_pass(P_UPLOAD, up, x, y, w, h, nullptr);
    }

    void fill_rect(int x, int y, int w, int h, const float* col) override { draw_rect_pass(P_FILL, nullptr, x, y, w, h, col); }

    void copy_rect(int sx, int sy, int dx, int dy, int w, int h) override {
        if (MS > 1) { // a multisampled texture takes no region copies: draw the source over the destination
            copy_off[0] = (sx - dx) * S;
            copy_off[1] = (sy - dy) * S;
            draw_rect_pass(P_COPY, copy, dx, dy, w, h, nullptr);
            copy_off[0] = copy_off[1] = 0;
            return;
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(sdl_cmd());
        const SDL_GPUTextureLocation src = {copy, 0, 0, (Uint32)(sx * S), (Uint32)(sy * S), 0};
        const SDL_GPUTextureLocation dst = {rt, 0, 0, (Uint32)(dx * S), (Uint32)(dy * S), 0};
        SDL_CopyGPUTextureToTexture(cp, &src, &dst, (Uint32)(w * S), (Uint32)(h * S), 1, false);
        SDL_EndGPUCopyPass(cp);
        mark_dirty(dx, dy, dx + w - 1, dy + h - 1);
    }

    void download(int x, int y, int w, int h) override {
        // downsample the upscaled VRAM (its single-sample copy, brought up to date), then read it back
        sync_copy();
        SDL_GPUGraphicsPipeline* p = pipe(P_DOWN, 0, RGBA8, 1);
        if (SDL_GPURenderPass* rp = p ? begin(down_rt) : nullptr) {
            const SDL_GPUViewport vp = {0, 0, 1024, 512, 0, 1};
            SDL_SetGPUViewport(rp, &vp);
            const SDL_Rect sc = {x, y, w, h};
            SDL_SetGPUScissor(rp, &sc);
            SDL_BindGPUGraphicsPipeline(rp, p);
            // vs_rect divides by the scale, so feed it native coordinates scaled by S on a 1024x512 target
            const float r[4] = {(float)(x * S), (float)(y * S), (float)((x + w) * S), (float)((y + h) * S)};
            push(make_cb(0, 1.0f, r));
            bind(rp, {{0, copy}});
            SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(sdl_cmd());
        SDL_GPUTextureRegion src = {};
        src.texture = down_rt;
        src.x = (Uint32)x;
        src.y = (Uint32)y;
        src.w = (Uint32)w;
        src.h = (Uint32)h;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst = {};
        dst.transfer_buffer = dl;
        dst.pixels_per_row = (Uint32)w;
        dst.rows_per_layer = (Uint32)h;
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        sdl_submit(true);
        if (const u8* m = (const u8*)SDL_MapGPUTransferBuffer(dev, dl, false)) {
            for (int yy = 0; yy < h; yy++) {
                const u32* row = (const u32*)(m + (size_t)yy * w * 4);
                for (int xx = 0; xx < w; xx++) shadow[(y + yy) * 1024 + x + xx] = rgba_to_555(row[xx]);
            }
            SDL_UnmapGPUTransferBuffer(dev, dl);
        }
    }

    // ------------------------------------------------------------ in-between frames (interp.cpp)
    bool interp_ready(const DisplayInfo& d) override {
        if (d.depth24) return false;
        flush();
        if (targets_stale()) rescale(); // F1/F2/F3: the new resolution or sample count first
        int w = std::min(d.w, 1024 - d.x) * S, h = std::min(d.h, 512 - d.y) * S;
        if (w <= 0 || h <= 0) return false;
        if (w != irt_w || h != irt_h || MS != irt_ms) {
            release(dev, irt); release(dev, irt_res);
            irt_w = irt_h = irt_ms = 0;
            irt = make_texture(w, h, RGBA8, MS, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (MS == 1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0));
            if (!irt) return false;
            if (MS > 1 && !(irt_res = make_texture(w, h, RGBA8, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))) {
                release(dev, irt);
                return false;
            }
            irt_w = w;
            irt_h = h;
            irt_ms = MS;
        }
        return true;
    }

    bool interp_begin(const DisplayInfo& d) override {
        if (!interp_ready(d)) return false;
        // start from the buffer on screen: whatever the frame does not redraw stays as it is
        if (MS > 1) {
            // SDL reads no multisampled texture: from the resolved picture instead (every sample of a pixel its
            // average, which resolves to the same picture)
            sync_copy();
            SDL_GPUGraphicsPipeline* p = pipe(P_COPY_FULL, 0, RGBA8, MS);
            if (SDL_GPURenderPass* rp = p ? begin(irt) : nullptr) {
                const SDL_GPUViewport vp = {0, 0, (float)irt_w, (float)irt_h, 0, 1};
                SDL_SetGPUViewport(rp, &vp);
                const SDL_Rect sc = {0, 0, irt_w, irt_h};
                SDL_SetGPUScissor(rp, &sc);
                SDL_BindGPUGraphicsPipeline(rp, p);
                copy_off[0] = d.x * S;
                copy_off[1] = d.y * S;
                push(make_cb(0, 1.0f));
                copy_off[0] = copy_off[1] = 0;
                bind(rp, {{1, copy}});
                SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
                SDL_EndGPURenderPass(rp);
            }
        } else {
            SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(sdl_cmd());
            const SDL_GPUTextureLocation src = {rt, 0, 0, (Uint32)(d.x * S), (Uint32)(d.y * S), 0}, dst = {irt, 0, 0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(cp, &src, &dst, (Uint32)irt_w, (Uint32)irt_h, 1, false);
            SDL_EndGPUCopyPass(cp);
        }
        redirect = true;
        tox = d.x * S;
        toy = d.y * S;
        batch_kind = -1; // new batch: different target and scissor offset
        return true;
    }

    void interp_present(const DisplayInfo& d, bool pace) override {
        flush();
        redirect = false;
        tox = toy = 0;
        batch_kind = -1;
        if (MS > 1) resolve(irt, irt_res);
        show(MS > 1 ? irt_res : irt, 0, 0, irt_w, irt_h, irt_w, irt_h, d, pace);
    }

    // ------------------------------------------------------------ the picture on screen
    // FXAA over w x h pixels at (x, y) of src (tw x th) into `post`; false if it could not
    bool fxaa(SDL_GPUTexture* src, int x, int y, int w, int h, int tw, int th) {
        w = std::min(w, tw - x);
        h = std::min(h, th - y);
        if (w <= 0 || h <= 0) return false;
        if (w != post_w || h != post_h) {
            release(dev, post);
            post_w = post_h = 0;
            if (!(post = make_texture(w, h, RGBA8, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))) return false;
            post_w = w;
            post_h = h;
        }
        SDL_GPUGraphicsPipeline* p = pipe(P_FXAA, 0, RGBA8, 1);
        SDL_GPURenderPass* rp = p ? begin(post, SDL_GPU_LOADOP_DONT_CARE) : nullptr;
        if (!rp) return false;
        const SDL_GPUViewport vp = {0, 0, (float)w, (float)h, 0, 1};
        SDL_SetGPUViewport(rp, &vp);
        const SDL_Rect sc = {0, 0, w, h};
        SDL_SetGPUScissor(rp, &sc);
        SDL_BindGPUGraphicsPipeline(rp, p);
        const float r[4] = {(float)x, (float)y, 1.0f / tw, 1.0f / th};
        const float lim[4] = {(x + 0.5f) / tw, (y + 0.5f) / th, (x + w - 0.5f) / tw, (y + h - 0.5f) / th};
        push(make_cb(0, 1.0f, r, lim));
        bind(rp, {{4, src}});
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        return true;
    }

    // show w x h pixels at (x, y) of src (tw x th), through FXAA when it is on; pace = wait for the next vblank
    void show(SDL_GPUTexture* src, int x, int y, int w, int h, int tw, int th, const DisplayInfo& d, bool pace) {
        SDL_GPUTexture* tex = src;
        float u0 = (float)x / tw, v0 = (float)y / th, u1 = (float)(x + w) / tw, v1 = (float)(y + h) / th;
        if (g_settings.anti_aliasing == AA_FXAA && fxaa(src, x, y, w, h, tw, th)) {
            tex = post;
            u0 = v0 = 0;
            u1 = v1 = 1;
            w = post_w;
            h = post_h;
        }
        if (pace) sdl_present_texture(tex, w, h, u0, v0, u1, v1, d.h, d.enabled);
        else sdl_show(tex, w, h, u0, v0, u1, v1, d.h, d.enabled);
    }

    void show_vram(const DisplayInfo& d) override {
        if (MS > 1) sync_copy(); // the screen shows the resolved copy
        show(MS > 1 ? copy : rt, d.x * S, d.y * S, d.w * S, d.h * S, 1024 * S, 512 * S, d, true);
    }
};

Renderer* renderer_create_sdlgpu(u16* vram) {
    if (!sdl_gpu()) return nullptr;
    SdlGpuRenderer* r = new SdlGpuRenderer(vram);
    if (r->ok) return r;
    plog("gpu: no hardware renderer on this GPU, using the software renderer\n");
    delete r;
    return nullptr;
}

Renderer* renderer_create_hardware(u16* vram) { return g_settings.renderer == 1 ? renderer_create_sdlgpu(vram) : nullptr; }
