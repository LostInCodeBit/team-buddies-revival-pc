// gpu_d3d11.cpp - hardware renderer: draws PS1 primitives with Direct3D 11 into an upscaled VRAM (the batching
// and the CPU copy of VRAM: gpu_hw.h).
#include "platform_d3d.h"
#include "gpu_hw.h"

static const char* k_hlsl =
#include "shaders/gpu_hlsl.inc"
    ;

struct D3DRenderer : HwRenderer {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    // rt: the upscaled VRAM the game draws into (multisampled with MSAA / SSAA); copy: its single-sample copy,
    // kept current per tile, that textures, readbacks and (multisampled) the screen read from
    ID3D11Texture2D* rt = nullptr, * copy = nullptr;
    ID3D11RenderTargetView* rtv = nullptr, * copy_rtv = nullptr;
    ID3D11ShaderResourceView* rt_srv = nullptr, * copy_srv = nullptr;
    ID3D11Texture2D* up = nullptr;
    ID3D11ShaderResourceView* up_srv = nullptr;
    ID3D11ShaderResourceView* icon_srv = nullptr;
    ID3D11SamplerState* icon_smp = nullptr;
    ID3D11Texture2D* glyph_tex = nullptr;
    ID3D11ShaderResourceView* glyph_srv = nullptr;
    ID3D11RenderTargetView* glyph_rtv = nullptr; // its top mip level, drawn into by ps_glyph
    ID3D11Texture2D* glyph_src = nullptr;        // ps_glyph's input: one letter's texels (hd_text_source)
    ID3D11ShaderResourceView* glyph_src_srv = nullptr;
    ID3D11PixelShader* ps_glyph = nullptr;
    ID3D11Texture2D* down_rt = nullptr, * down_stage = nullptr;
    ID3D11RenderTargetView* down_rtv = nullptr;
    ID3D11VertexShader* vs = nullptr, * vs_rect = nullptr, * vs_full = nullptr;
    ID3D11PixelShader* ps = nullptr, * ps_upload = nullptr, * ps_fill = nullptr, * ps_down = nullptr;
    ID3D11PixelShader* ps_sample = nullptr; // ps_main shaded per sample (SSAA)
    ID3D11PixelShader* ps_copy = nullptr, * ps_resolve = nullptr, * ps_copy_ms = nullptr, * ps_fxaa = nullptr;
    ID3D11SamplerState* lin_smp = nullptr;
    // FXAA output: the displayed area at the internal resolution
    ID3D11Texture2D* post = nullptr;
    ID3D11RenderTargetView* post_rtv = nullptr;
    ID3D11ShaderResourceView* post_srv = nullptr;
    int post_w = 0, post_h = 0;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr, * cb = nullptr;
    ID3D11BlendState* bs_opaque = nullptr, * bs_mode[4] = {};
    ID3D11RasterizerState* rs = nullptr;
    static const int VB_SIZE = 1 << 17;
    // in-between frames (interp.cpp) are drawn into irt, multisampled like rt; irt_res is then its resolved picture
    ID3D11Texture2D* irt = nullptr, * irt_res = nullptr;
    ID3D11RenderTargetView* irtv = nullptr;
    ID3D11ShaderResourceView* irt_srv = nullptr, * irt_res_srv = nullptr;
    int irt_w = 0, irt_h = 0, irt_ms = 0;

    explicit D3DRenderer(u16* v) : HwRenderer(v) {
        dev = d3d_device();
        ctx = d3d_context();
        ID3DBlob* b;
        b = d3d_compile(k_hlsl, "vs_main", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
        D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"ATTR", 0, DXGI_FORMAT_R32G32_UINT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BOX", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        dev->CreateInputLayout(il, 5, b->GetBufferPointer(), b->GetBufferSize(), &layout);
        b->Release();
        b = d3d_compile(k_hlsl, "vs_rect", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_rect);
        b->Release();
        b = d3d_compile(k_hlsl, "vs_full", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_full);
        b->Release();
        auto mkps = [&](const char* src, const char* e, ID3D11PixelShader** out) {
            ID3DBlob* x = d3d_compile(src, e, "ps_5_0");
            dev->CreatePixelShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, out);
            x->Release();
        };
        mkps(k_hlsl, "ps_main", &ps);
        mkps(k_hlsl, "ps_upload", &ps_upload);
        mkps(k_hlsl, "ps_fill", &ps_fill);
        mkps(k_hlsl, "ps_down", &ps_down);
        mkps(k_hlsl, "ps_copy", &ps_copy);
        mkps(k_hlsl, "ps_resolve", &ps_resolve);
        mkps(k_hlsl, "ps_copy_ms", &ps_copy_ms);
        mkps(k_hlsl, "ps_fxaa", &ps_fxaa);
        mkps(k_hlsl, "ps_glyph", &ps_glyph);
        mkps((std::string("#define PER_SAMPLE 1\n") + k_hlsl).c_str(), "ps_main", &ps_sample);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &lin_smp);

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = VB_SIZE * sizeof(HwVertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &vb);
        bd.ByteWidth = sizeof(DrawCB);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = 0;
        dev->CreateBuffer(&bd, nullptr, &cb);

        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bl, &bs_opaque);
        for (int m = 0; m < 4; m++) {
            D3D11_RENDER_TARGET_BLEND_DESC& r = bl.RenderTarget[0];
            r.BlendEnable = TRUE;
            r.SrcBlend = D3D11_BLEND_ONE;
            r.DestBlend = D3D11_BLEND_BLEND_FACTOR;
            r.BlendOp = m == 2 ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_ADD;
            r.SrcBlendAlpha = D3D11_BLEND_ONE;
            r.DestBlendAlpha = D3D11_BLEND_ZERO;
            r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            dev->CreateBlendState(&bl, &bs_mode[m]);
        }
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        rd.MultisampleEnable = TRUE; // per-sample coverage on multisampled targets (no effect on the others)
        dev->CreateRasterizerState(&rd, &rs);

        // native-resolution helpers
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1024;
        td.Height = 512;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &up);
        dev->CreateShaderResourceView(up, nullptr, &up_srv);
        create_icon_atlas();
        create_glyph_atlas();
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        dev->CreateTexture2D(&td, nullptr, &down_rt);
        dev->CreateRenderTargetView(down_rt, nullptr, &down_rtv);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&td, nullptr, &down_stage);

        create_targets(g_settings.scale, aa_samples(g_settings.anti_aliasing));
        upload_rect(0, 0, 1024, 512);
    }

    template <typename T> static void release(T*& p) { if (p) p->Release(); p = nullptr; }

    void create_targets(int scale, int samples) override {
        release(rtv); release(rt_srv); release(rt);
        release(copy_rtv); release(copy_srv); release(copy);
        release(irtv); release(irt_srv); release(irt); release(irt_res_srv); release(irt_res);
        irt_w = irt_h = irt_ms = 0;
        S = scale;
        want_ms = samples;
        // samples: as many as asked that the GPU supports, keeping the render target within 512 MB (8x
        // resolution: 4 samples at most)
        const double per_sample = 1024.0 * S * 512.0 * S * 4;
        MS = samples;
        while (MS > 1) {
            UINT q = 0;
            if (per_sample * MS <= 512.0 * 1024 * 1024 &&
                SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, MS, &q)) && q > 0) break;
            MS /= 2;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1024 * S;
        td.Height = 512 * S;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = MS;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        while (FAILED(dev->CreateTexture2D(&td, nullptr, &rt))) { // out of video memory: fewer samples
            if (td.SampleDesc.Count == 1) fatal("cannot create %dx VRAM render target", S);
            td.SampleDesc.Count /= 2;
        }
        MS = td.SampleDesc.Count;
        dev->CreateRenderTargetView(rt, nullptr, &rtv);
        dev->CreateShaderResourceView(rt, nullptr, &rt_srv);
        td.SampleDesc.Count = 1;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &copy))) fatal("cannot create %dx VRAM copy", S);
        dev->CreateRenderTargetView(copy, nullptr, &copy_rtv);
        dev->CreateShaderResourceView(copy, nullptr, &copy_srv);
        float black[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(rtv, black);
        if (MS > 1) plog("hardware renderer: %dx internal resolution (%dx%d VRAM), %d samples%s\n", S, 1024 * S, 512 * S, MS,
                         MS < samples ? " (fewer than asked: GPU or memory)" : "");
        else plog("hardware renderer: %dx internal resolution (%dx%d VRAM)\n", S, 1024 * S, 512 * S);
    }

    void set_cb(int pass, float factor, const float* rect = nullptr, const float* fillc = nullptr) {
        DrawCB c = make_cb(pass, factor, rect, fillc);
        ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);
    }

    void bind_target() {
        ctx->OMSetRenderTargets(1, redirect ? &irtv : &rtv, nullptr);
        D3D11_VIEWPORT vp = {(float)-tox, (float)-toy, 1024.0f * S, 512.0f * S, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);
    }

    void sync_copy() override {
        if (!any_dirty) return;
        if (MS > 1) { // multisampled: average each pixel's samples into the copy, a draw per run of tiles
            ctx->OMSetRenderTargets(1, &copy_rtv, nullptr);
            D3D11_VIEWPORT vp = {0, 0, 1024.0f * S, 512.0f * S, 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rs);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->VSSetShader(vs_rect, nullptr, 0);
            ctx->PSSetShader(ps_resolve, nullptr, 0);
            ctx->PSSetShaderResources(3, 1, &rt_srv);
        }
        for (int ty = 0; ty < 16; ty++)
            for (int tx = 0; tx < 32;) {
                if (!dirty[ty][tx]) { tx++; continue; }
                int t0 = tx;
                while (tx < 32 && dirty[ty][tx]) { dirty[ty][tx] = false; tx++; }
                D3D11_BOX box = {(UINT)(t0 * 32 * S), (UINT)(ty * 32 * S), 0, (UINT)(tx * 32 * S), (UINT)((ty + 1) * 32 * S), 1};
                if (MS > 1) {
                    float r[4] = {(float)box.left, (float)box.top, (float)box.right, (float)box.bottom};
                    set_cb(0, 1.0f, r);
                    D3D11_RECT sc = {(LONG)box.left, (LONG)box.top, (LONG)box.right, (LONG)box.bottom};
                    ctx->RSSetScissorRects(1, &sc);
                    ctx->Draw(4, 0);
                } else {
                    ctx->CopySubresourceRegion(copy, 0, box.left, box.top, 0, rt, 0, &box);
                }
            }
        if (MS > 1) {
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(3, 1, &none);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        any_dirty = false;
    }

    // ------------------------------------------------------------ batching
    // button glyph atlas with a mip chain, so the glyphs stay clean at every internal resolution
    void create_icon_atlas() {
        int aw, ah;
        const u32* px = icon_atlas_pixels(aw, ah);
        if (!px) return;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = aw;
        td.Height = ah;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { plog("icons: cannot create glyph atlas\n"); return; }
        ctx->UpdateSubresource(tex, 0, nullptr, px, aw * 4, 0);
        dev->CreateShaderResourceView(tex, nullptr, &icon_srv);
        ctx->GenerateMips(icon_srv);
        tex->Release();
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &icon_smp);
    }

    void create_glyph_atlas() {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = GLYPH_ATLAS;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &glyph_tex))) { plog("hd text: cannot create the glyph atlas\n"); return; }
        dev->CreateShaderResourceView(glyph_tex, nullptr, &glyph_srv);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = td.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(glyph_tex, &rd, &glyph_rtv);
        td.Width = GLYPH_SRC_W;
        td.Height = GLYPH_SRC_H;
        td.MipLevels = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &glyph_src))) {
            plog("hd text: cannot create the glyph source texture\n");
            glyph_srv->Release();
            glyph_srv = nullptr; // glyph_place then always declines
            return;
        }
        dev->CreateShaderResourceView(glyph_src, nullptr, &glyph_src_srv);
        glyph_ok = true;
        glyph_reset();
    }

    // reconstruct letter g into the atlas cell at x, y (w x h pixels) on the GPU
    void glyph_draw(const HdGlyph& g, int x, int y, int w, int h) override {
        u8 src[GLYPH_SRC_W * GLYPH_SRC_H * 2];
        int fw, fh;
        hd_text_source(g, src, GLYPH_SRC_W, fw, fh);
        const D3D11_BOX sb = {0, 0, 0, (UINT)fw, (UINT)fh, 1};
        ctx->UpdateSubresource(glyph_src, 0, &sb, src, GLYPH_SRC_W * 2, 0);
        const float r[4] = {(float)x, (float)y, (float)g.style, 0};
        set_cb(0, 1.0f, r);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(5, 1, &none); // the atlas, about to be drawn into
        ctx->OMSetRenderTargets(1, &glyph_rtv, nullptr);
        const D3D11_VIEWPORT vp = {(float)x, (float)y, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        const D3D11_RECT sc = {x, y, x + w, y + h};
        ctx->RSSetScissorRects(1, &sc);
        ctx->RSSetState(rs);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_full, nullptr, 0);
        ctx->PSSetShader(ps_glyph, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetShaderResources(6, 1, &glyph_src_srv);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(6, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr); // the next pass binds its own target
    }

    void glyph_clear() override {
        static const float zero[4] = {};
        if (glyph_rtv) ctx->ClearRenderTargetView(glyph_rtv, zero);
    }

    void flush() override {
        if (batch.empty()) return;
        if (glyph_mips && glyph_srv) { ctx->GenerateMips(glyph_srv); glyph_mips = false; }
        bind_target();
        const D3D11_RECT bsc = {batch_scissor.x0, batch_scissor.y0, batch_scissor.x1, batch_scissor.y1};
        ctx->RSSetScissorRects(1, &bsc);
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(MS > 1 && aa_per_sample(g_settings.anti_aliasing) ? ps_sample : ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &copy_srv);
        ctx->PSSetShaderResources(2, 1, &icon_srv);
        ctx->PSSetShaderResources(5, 1, &glyph_srv);
        ctx->PSSetSamplers(0, 1, &icon_smp);
        ctx->PSSetSamplers(1, 1, &lin_smp);
        UINT stride = sizeof(HwVertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        size_t done = 0;
        while (done < batch.size()) {
            size_t n = std::min(batch.size() - done, (size_t)(VB_SIZE / 6 * 6)); // whole triangles
            D3D11_MAPPED_SUBRESOURCE m;
            ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            memcpy(m.pData, batch.data() + done, n * sizeof(HwVertex));
            ctx->Unmap(vb, 0);
            static const float factors[4] = {0.5f, 1.0f, 1.0f, 0.25f};
            float bf[4] = {batch_mode == 0 ? 0.5f : 1.0f, batch_mode == 0 ? 0.5f : 1.0f, batch_mode == 0 ? 0.5f : 1.0f, 1.0f};
            if (batch_kind == 0) {
                set_cb(0, 1.0f);
                ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
            } else if (batch_kind == 1) {
                set_cb(0, factors[batch_mode]);
                ctx->OMSetBlendState(bs_mode[batch_mode], bf, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
            } else {
                set_cb(1, 1.0f);
                ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
                set_cb(2, factors[batch_mode]);
                ctx->OMSetBlendState(bs_mode[batch_mode], bf, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
            }
            done += n;
        }
        batch.clear();
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }

    // ------------------------------------------------------------ vram operations
    void draw_rect_pass(ID3D11PixelShader* p, ID3D11ShaderResourceView* srv, int x, int y, int w, int h, const float* col) {
        bind_target();
        float r[4] = {(float)(x * S), (float)(y * S), (float)((x + w) * S), (float)((y + h) * S)};
        set_cb(0, 1.0f, r, col);
        D3D11_RECT sc = {x * S - tox, y * S - toy, (x + w) * S - tox, (y + h) * S - toy};
        ctx->RSSetScissorRects(1, &sc);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs_rect, nullptr, 0);
        ctx->PSSetShader(p, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {nullptr, srv};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        ctx->Draw(4, 0);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, none);
        if (!redirect) mark_dirty(x, y, x + w - 1, y + h - 1);
    }

    void upload_pixels(int x, int y, int w, int h, const u32* rgba) override {
        D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
        ctx->UpdateSubresource(up, 0, &box, rgba, w * 4, 0);
        draw_rect_pass(ps_upload, up_srv, x, y, w, h, nullptr);
    }

    void fill_rect(int x, int y, int w, int h, const float* col) override { draw_rect_pass(ps_fill, nullptr, x, y, w, h, col); }

    void copy_rect(int sx, int sy, int dx, int dy, int w, int h) override {
        if (MS > 1) { // a multisampled texture takes no region copies: draw the source over the destination
            copy_off[0] = (sx - dx) * S;
            copy_off[1] = (sy - dy) * S;
            draw_rect_pass(ps_copy, copy_srv, dx, dy, w, h, nullptr);
            copy_off[0] = copy_off[1] = 0;
            return;
        }
        D3D11_BOX box = {(UINT)(sx * S), (UINT)(sy * S), 0, (UINT)((sx + w) * S), (UINT)((sy + h) * S), 1};
        ctx->CopySubresourceRegion(rt, 0, dx * S, dy * S, 0, copy, 0, &box);
        mark_dirty(dx, dy, dx + w - 1, dy + h - 1);
    }

    void download(int x, int y, int w, int h) override {
        // downsample the upscaled VRAM (its single-sample copy, brought up to date), then read it back
        sync_copy();
        bind_target();
        ctx->OMSetRenderTargets(1, &down_rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, 1024, 512, 0, 1};
        ctx->RSSetViewports(1, &vp);
        float r[4] = {(float)(x * S), (float)(y * S), (float)((x + w) * S), (float)((y + h) * S)};
        set_cb(0, 1.0f, r, nullptr);
        D3D11_RECT sc = {x, y, x + w, y + h};
        ctx->RSSetScissorRects(1, &sc);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs_rect, nullptr, 0);
        ctx->PSSetShader(ps_down, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &copy_srv);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        // vs_rect divides by the scale, so feed it native coordinates scaled by S on a 1024x512 target
        ctx->Draw(4, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
        ctx->CopySubresourceRegion(down_stage, 0, x, y, 0, down_rt, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(down_stage, 0, D3D11_MAP_READ, 0, &m))) {
            for (int yy = 0; yy < h; yy++) {
                const u32* row = (const u32*)((const u8*)m.pData + (y + yy) * m.RowPitch);
                for (int xx = 0; xx < w; xx++) shadow[(y + yy) * 1024 + x + xx] = rgba_to_555(row[x + xx]);
            }
            ctx->Unmap(down_stage, 0);
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
            release(irtv); release(irt_srv); release(irt); release(irt_res_srv); release(irt_res);
            irt_w = irt_h = irt_ms = 0;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = MS;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &irt))) return false;
            dev->CreateRenderTargetView(irt, nullptr, &irtv);
            dev->CreateShaderResourceView(irt, nullptr, &irt_srv);
            if (MS > 1) {
                td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(dev->CreateTexture2D(&td, nullptr, &irt_res))) { release(irtv); release(irt_srv); release(irt); return false; }
                dev->CreateShaderResourceView(irt_res, nullptr, &irt_res_srv);
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
        if (MS > 1) { // sample by sample (no region copies between multisampled textures)
            ctx->OMSetRenderTargets(1, &irtv, nullptr);
            D3D11_VIEWPORT vp = {0, 0, (float)irt_w, (float)irt_h, 0, 1};
            ctx->RSSetViewports(1, &vp);
            D3D11_RECT sc = {0, 0, irt_w, irt_h};
            ctx->RSSetScissorRects(1, &sc);
            ctx->RSSetState(rs);
            ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
            copy_off[0] = d.x * S;
            copy_off[1] = d.y * S;
            set_cb(0, 1.0f);
            copy_off[0] = copy_off[1] = 0;
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs_full, nullptr, 0);
            ctx->PSSetShader(ps_copy_ms, nullptr, 0);
            ctx->PSSetShaderResources(3, 1, &rt_srv);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(3, 1, &none);
        } else {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            D3D11_BOX box = {(UINT)(d.x * S), (UINT)(d.y * S), 0, (UINT)(d.x * S + irt_w), (UINT)(d.y * S + irt_h), 1};
            ctx->CopySubresourceRegion(irt, 0, 0, 0, 0, rt, 0, &box);
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
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (MS > 1) ctx->ResolveSubresource(irt_res, 0, irt, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        show(MS > 1 ? irt_res_srv : irt_srv, 0, 0, irt_w, irt_h, irt_w, irt_h, d, pace);
    }

    // ------------------------------------------------------------ the picture on screen
    // FXAA over w x h pixels at (x, y) of src (tw x th) into `post`; false if it could not
    bool fxaa(ID3D11ShaderResourceView* src, int x, int y, int w, int h, int tw, int th) {
        w = std::min(w, tw - x);
        h = std::min(h, th - y);
        if (w <= 0 || h <= 0) return false;
        if (w != post_w || h != post_h) {
            release(post_rtv); release(post_srv); release(post);
            post_w = post_h = 0;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &post))) return false;
            dev->CreateRenderTargetView(post, nullptr, &post_rtv);
            dev->CreateShaderResourceView(post, nullptr, &post_srv);
            post_w = w;
            post_h = h;
        }
        ctx->OMSetRenderTargets(1, &post_rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc = {0, 0, w, h};
        ctx->RSSetScissorRects(1, &sc);
        ctx->RSSetState(rs);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        float r[4] = {(float)x, (float)y, 1.0f / tw, 1.0f / th};
        float lim[4] = {(x + 0.5f) / tw, (y + 0.5f) / th, (x + w - 0.5f) / tw, (y + h - 0.5f) / th};
        set_cb(0, 1.0f, r, lim);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_full, nullptr, 0);
        ctx->PSSetShader(ps_fxaa, nullptr, 0);
        ctx->PSSetShaderResources(4, 1, &src);
        ctx->PSSetSamplers(1, 1, &lin_smp);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(4, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }

    // show w x h pixels at (x, y) of src (tw x th), through FXAA when it is on; pace = wait for the next vblank
    void show(ID3D11ShaderResourceView* src, int x, int y, int w, int h, int tw, int th, const DisplayInfo& d, bool pace) {
        ID3D11ShaderResourceView* srv = src;
        float u0 = (float)x / tw, v0 = (float)y / th, u1 = (float)(x + w) / tw, v1 = (float)(y + h) / th;
        if (g_settings.anti_aliasing == AA_FXAA && fxaa(src, x, y, w, h, tw, th)) {
            srv = post_srv;
            u0 = v0 = 0;
            u1 = v1 = 1;
            w = post_w;
            h = post_h;
        }
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (pace) d3d_present_texture(srv, u0, v0, u1, v1, w, h, d.h, d.enabled);
        else d3d_show(srv, u0, v0, u1, v1, w, h, d.h, d.enabled);
    }

    void show_vram(const DisplayInfo& d) override {
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (MS > 1) sync_copy(); // the screen shows the resolved copy
        show(MS > 1 ? copy_srv : rt_srv, d.x * S, d.y * S, d.w * S, d.h * S, 1024 * S, 512 * S, d, true);
    }
};

Renderer* renderer_create_d3d11(u16* vram) {
    if (!d3d_device()) return nullptr;
    return new D3DRenderer(vram);
}

Renderer* renderer_create_hardware(u16* vram) { return g_settings.renderer == 1 ? renderer_create_d3d11(vram) : nullptr; }
