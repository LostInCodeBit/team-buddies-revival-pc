// platform.cpp - Win32 window, Direct3D 11 presentation, keyboard (+ SDL3 controllers, gamepad.cpp), XAudio2, frame pacing.
#include "platform_d3d.h"
#include <dxgi1_6.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <atomic>
#include <timeapi.h>
#include "psx.h"
#include <cmath>
#include <cstdarg>
#include <unordered_set>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

std::string g_base_dir;
std::string g_ini_path;

// ---------------------------------------------------------------- logging
static FILE* s_log;
void plog(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    static size_t written = 0;
    const size_t limit = 32u << 20; // keep the log file bounded
    if (!s_log) s_log = fopen(getenv("TB_LOG") ? getenv("TB_LOG") : (g_base_dir + "teambuddies.log").c_str(), "w"); // TB_LOG: test harness
    if (s_log && written < limit) {
        written += strlen(buf);
        fputs(written < limit ? buf : "log size limit reached, further output dropped\n", s_log);
        fflush(s_log);
    }
    // only for a debugger: with another program listening for debug output (seen while Valorant ran) each call
    // can block for seconds
    if (IsDebuggerPresent()) OutputDebugStringA(buf);
    fputs(buf, stdout);
}
void log_once(const char* key, const char* fmt, ...) {
    static std::unordered_set<std::string> seen;
    if (!seen.insert(key).second) return;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plog("%s", buf);
}
// test harness: TB_HIDDEN runs show nothing on screen, not even an error box (the log has it)
static bool hidden_run() {
    static const bool h = getenv("TB_HIDDEN") != nullptr;
    return h;
}

void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plog("FATAL: %s\n", buf);
    if (!hidden_run()) MessageBoxA(nullptr, buf, "Team Buddies", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

// ---------------------------------------------------------------- window
static HWND s_hwnd;
static bool s_quit;
static bool s_fullscreen;
static RECT s_saved_rect;
static bool s_focus = true;

void* platform_hwnd() { return s_hwnd; }
bool platform_key(int vk) { return s_focus && (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void resize_swapchain();
static void set_exclusive(bool on);
static bool s_exclusive;          // DXGI exclusive fullscreen requested (display = fullscreen)
static bool s_reenter_exclusive;  // regained focus: DXGI left exclusive mode on alt-tab
static int s_full_mode = DISPLAY_BORDERLESS; // what F11 switches to: borderless or exclusive

static void set_fullscreen(bool on) {
    if (on == s_fullscreen) return;
    if (!on && s_exclusive) set_exclusive(false);
    s_fullscreen = on;
    if (on) {
        GetWindowRect(s_hwnd, &s_saved_rect);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfo(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtr(s_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(s_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (s_full_mode == DISPLAY_FULLSCREEN) set_exclusive(true);
    } else {
        SetWindowLongPtr(s_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(s_hwnd, nullptr, s_saved_rect.left, s_saved_rect.top, s_saved_rect.right - s_saved_rect.left,
                     s_saved_rect.bottom - s_saved_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

void platform_toggle_fullscreen() {
    set_fullscreen(!s_fullscreen);
    g_settings.display = s_fullscreen ? s_full_mode : DISPLAY_WINDOW;
}

void platform_get_window_size(int& w, int& h) {
    RECT r;
    GetClientRect(s_hwnd, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
}

static void hotkey(WPARAM k);

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: s_quit = true; return 0;
    case WM_SIZE: resize_swapchain(); return 0;
    case WM_ACTIVATE:
        s_focus = LOWORD(w) != WA_INACTIVE;
        if (s_focus && s_exclusive) s_reenter_exclusive = true; // handled on the next present, outside the message
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) { platform_toggle_fullscreen(); return 0; }
        if (w == VK_F10) { // F10 arrives as a system key (it would open the window menu)
            if (!(l & (1 << 30))) { // original <-> the rate chosen in the ini (60 if none)
                static int last_on = 60;
                if (g_settings.frame_rate != 0) { last_on = g_settings.frame_rate; g_settings.frame_rate = 0; }
                else g_settings.frame_rate = last_on;
                plog("frame rate: %d (0 original, -1 monitor)\n", g_settings.frame_rate);
            }
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (!(l & (1 << 30))) hotkey(w);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ---------------------------------------------------------------- d3d11
static ID3D11Device* s_dev;
static ID3D11DeviceContext* s_ctx;
static IDXGISwapChain1* s_swap;
static ID3D11RenderTargetView* s_rtv;
static ID3D11VertexShader* s_vs;
static ID3D11PixelShader* s_ps;
static ID3D11SamplerState* s_point, * s_linear;
static ID3D11Buffer* s_cb;
static ID3D11Texture2D* s_sw_tex;
static ID3D11ShaderResourceView* s_sw_srv;
static int s_sw_w, s_sw_h;

ID3D11Device* d3d_device() { return s_dev; }
ID3D11DeviceContext* d3d_context() { return s_ctx; }

ID3DBlob* d3d_compile(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr, * err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) fatal("shader compile failed (%s): %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
    if (err) err->Release();
    return code;
}

static const char* k_present_hlsl = R"(
cbuffer C : register(b0) { float4 uvrect; };
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs_main(uint id : SV_VertexID) {
    VO o;
    float2 p = float2(id & 1, id >> 1);
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = lerp(uvrect.xy, uvrect.zw, p);
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps_main(VO i) : SV_Target {
    float w, h;
    tex.GetDimensions(w, h);
    float2 size = float2(w, h);
    float2 fp = float2(abs(ddx(i.uv.x)), abs(ddy(i.uv.y))) * size; // source texels per screen pixel
    if (max(fp.x, fp.y) <= 1.0) return float4(tex.SampleLevel(smp, i.uv, 0).rgb, 1); // one mip level only
    // drawn smaller than the source (an internal resolution above the window's): average the texels each pixel
    // covers instead of picking one, so the extra resolution (and MSAA's blended edges) smooth the picture
    int2 n = (int2)min(ceil(fp), 4.0);
    float3 acc = 0;
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++)
            acc += tex.SampleLevel(smp, i.uv + (float2(x, y) + 0.5 - n * 0.5) * fp / n / size, 0).rgb; // no gradients in a loop
    return float4(acc / (n.x * n.y), 1);
}
)";

static void create_rtv() {
    ID3D11Texture2D* bb = nullptr;
    s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    s_dev->CreateRenderTargetView(bb, nullptr, &s_rtv);
    bb->Release();
}

static void resize_swapchain() {
    if (!s_swap) return;
    if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
    s_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    s_swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    create_rtv();
}

// exclusive fullscreen at the desktop's own mode; falls back to borderless if DXGI refuses
static void set_exclusive(bool on) {
    if (!s_swap) return;
    HRESULT hr = s_swap->SetFullscreenState(on ? TRUE : FALSE, nullptr);
    if (FAILED(hr) && on) { plog("display: exclusive fullscreen refused (%08lX), staying borderless\n", (unsigned long)hr); on = false; }
    s_exclusive = on;
    resize_swapchain();
}

static bool d3d_init() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
    // laptops with two GPUs: ask for the high-performance one (falls back to the default adapter)
    IDXGIAdapter1* pick = nullptr;
    IDXGIFactory6* f6 = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)&pick);
        f6->Release();
    }
    HRESULT hr = D3D11CreateDevice(pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1,
                                   D3D11_SDK_VERSION, &s_dev, nullptr, &s_ctx);
    if (FAILED(hr) && pick) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &s_dev, nullptr, &s_ctx);
    if (pick) pick->Release();
    if (FAILED(hr)) return false;
    IDXGIDevice* dxdev;
    IDXGIAdapter* adapter;
    IDXGIFactory2* factory;
    s_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    // one frame in flight: a present shows up at the next refresh, so in-between frames land when they were meant to
    IDXGIDevice1* dxdev1 = nullptr;
    if (SUCCEEDED(dxdev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev1))) { dxdev1->SetMaximumFrameLatency(1); dxdev1->Release(); }
    dxdev->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad;
    if (SUCCEEDED(adapter->GetDesc(&ad))) plog("gpu: %ls\n", ad.Description);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // three: with one frame waiting for the next refresh (vsync), the GPU can still draw the next one; with two
    // it would wait for the flip, and the emulator's own GPU work behind it (unlocked frame rate)
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(s_dev, s_hwnd, &sd, nullptr, nullptr, &s_swap);
    factory->MakeWindowAssociation(s_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    adapter->Release();
    dxdev->Release();
    if (FAILED(hr)) return false;
    create_rtv();
    ID3DBlob* vs = d3d_compile(k_present_hlsl, "vs_main", "vs_5_0");
    ID3DBlob* ps = d3d_compile(k_present_hlsl, "ps_main", "ps_5_0");
    s_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_vs);
    s_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_ps);
    vs->Release();
    ps->Release();
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    s_dev->CreateSamplerState(&smp, &s_point);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s_dev->CreateSamplerState(&smp, &s_linear);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    s_dev->CreateBuffer(&bd, nullptr, &s_cb);
    return true;
}

// ---------------------------------------------------------------- screenshots (uncompressed PNG)
static u32 crc32_png(const u8* d, size_t n, u32 c = 0xFFFFFFFF) {
    static u32 t[256];
    if (!t[1]) for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = (x >> 1) ^ (0xEDB88320 & (0 - (x & 1))); t[i] = x; }
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void write_png(const std::string& path, const u8* rgba, int w, int h, int pitch) {
    std::vector<u8> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        const u8* row = rgba + (size_t)y * pitch;
        for (int x = 0; x < w; x++) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
    }
    // zlib stream made of stored blocks
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw.size(); p += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - p);
        z.push_back(p + n >= raw.size() ? 1 : 0);
        z.push_back((u8)n); z.push_back((u8)(n >> 8)); z.push_back((u8)~n); z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
    }
    u32 ad = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((u8)(ad >> s));
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto be32 = [&](u32 v) { u8 x[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; fwrite(x, 1, 4, f); };
    auto chunk = [&](const char* type, const std::vector<u8>& data) {
        be32((u32)data.size());
        std::vector<u8> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        fwrite(td.data(), 1, td.size(), f);
        be32(~crc32_png(td.data(), td.size()));
    };
    const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<u8> ihdr = {(u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

static bool s_shot_request;
static int s_shot_counter;

static void capture_backbuffer() {
    ID3D11Texture2D* bb = nullptr;
    s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(s_dev->CreateTexture2D(&td, nullptr, &st))) {
        s_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(s_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            // TB_SHOT_DIR redirects screenshots (test harness)
            std::string dir = getenv("TB_SHOT_DIR") ? std::string(getenv("TB_SHOT_DIR")) : g_base_dir + "screenshots";
            CreateDirectoryA(dir.c_str(), nullptr);
            char name[64];
            snprintf(name, sizeof name, "\\shot_%04d.png", s_shot_counter++);
            write_png(dir + name, (const u8*)m.pData, td.Width, td.Height, m.RowPitch);
            s_ctx->Unmap(st, 0);
            plog("saved %s%s\n", dir.c_str(), name);
        }
        st->Release();
    }
    bb->Release();
}

// size of the picture inside a ww x wh window for the aspect setting; native_h = the game's own line count
static void picture_size(int ww, int wh, int native_h, double& vw, double& vh) {
    vw = ww;
    vh = wh;
    const double base = g_settings.widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
    double a = base;
    switch (g_settings.aspect) {
    case ASPECT_STRETCH: return;
    case ASPECT_WIDE: if (!netplay_active()) return; a = 16.0 / 9.0; break; // online the 3D view is fixed at 16:9
    case ASPECT_16_9: a = 16.0 / 9.0; break;
    case ASPECT_PIXELS: {
        // whole-number multiples of the game's own lines: every source pixel the same size
        double bh = native_h > 0 ? native_h : 240, bw = bh * base;
        int k = (int)std::min(ww / bw, wh / bh);
        if (k >= 1) { vw = bw * k; vh = bh * k; return; }
        break;
    }
    }
    if (vw / vh > a) vw = vh * a;
    else vh = vw / a;
}

// widescreen hack strength: 3/4 for 16:9; "wide" follows the window's shape (offline only, it changes the 3D maths)
static void update_widescreen_factor(int ww, int wh) {
    int q = 49152;
    if (g_settings.aspect == ASPECT_WIDE && !netplay_active() && wh > 0)
        q = std::clamp((int)(65536.0 * (4.0 / 3.0) / ((double)ww / wh)), 16384, 65536);
    g_gpu_widescreen_q16 = q;
}

static int s_sync_interval = 1; // refreshes per present with vsync (the unlocked frame rate caps with it)
static LONGLONG s_present_wait;  // QPC ticks the last Present call took (it waits while the frame before is queued)

void d3d_present_texture(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1, int src_w, int src_h, int native_h, bool enabled) {
    s_sync_interval = 1;
    d3d_show(srv, u0, v0, u1, v1, src_w, src_h, native_h, enabled);
    platform_frame_pace();
    platform_pump();
}

bool d3d_show(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1, int src_w, int src_h, int native_h, bool enabled) {
    if (s_reenter_exclusive && s_focus) {
        s_reenter_exclusive = false;
        BOOL fs = FALSE;
        if (SUCCEEDED(s_swap->GetFullscreenState(&fs, nullptr)) && !fs) set_exclusive(true);
    }
    int ww, wh;
    platform_get_window_size(ww, wh);
    if (ww <= 0 || wh <= 0) return false; // minimized: nothing to draw
    update_widescreen_factor(ww, wh);
    float black[4] = {0, 0, 0, 1};
    s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
    s_ctx->ClearRenderTargetView(s_rtv, black);
    if (enabled && srv && src_w > 0 && src_h > 0) {
        double vw, vh;
        picture_size(ww, wh, native_h, vw, vh);
        D3D11_VIEWPORT vp = {(float)std::floor((ww - vw) / 2), (float)std::floor((wh - vh) / 2), (float)vw, (float)vh, 0, 1};
        s_ctx->RSSetViewports(1, &vp);
        float rect[4] = {u0, v0, u1, v1};
        s_ctx->UpdateSubresource(s_cb, 0, nullptr, rect, 0, 0);
        s_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        s_ctx->IASetInputLayout(nullptr);
        s_ctx->VSSetShader(s_vs, nullptr, 0);
        s_ctx->VSSetConstantBuffers(0, 1, &s_cb);
        s_ctx->PSSetShader(s_ps, nullptr, 0);
        s_ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* smp = g_settings.bilinear ? s_linear : s_point;
        s_ctx->PSSetSamplers(0, 1, &smp);
        s_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        s_ctx->OMSetDepthStencilState(nullptr, 0);
        s_ctx->RSSetState(nullptr);
        s_ctx->Draw(4, 0);
        ID3D11ShaderResourceView* none = nullptr;
        s_ctx->PSSetShaderResources(0, 1, &none);
    }
    // F12, or every N frames when TB_SHOT_EVERY is set (debugging aid)
    static int every = getenv("TB_SHOT_EVERY") ? atoi(getenv("TB_SHOT_EVERY")) : 0;
    static int frame = 0;
    static std::vector<int> at; // TB_SHOT_AT="1650,2000,4250": screenshots at those frames
    static bool at_parsed = false;
    if (!at_parsed) {
        at_parsed = true;
        if (const char* s = getenv("TB_SHOT_AT")) for (const char* p = s; *p; p += *p == ',') at.push_back((int)strtol(p, (char**)&p, 10));
    }
    ++frame;
    bool shot_at = std::find(at.begin(), at.end(), frame) != at.end();
    // TB_SHOT_VBLANK="7000:16": the first 16 presents from vblank 7000 on (in-between frames included)
    static long long sv_from = -1, sv_left = 0;
    if (sv_from < 0) { const char* e = getenv("TB_SHOT_VBLANK"); sv_from = 0; if (e) sscanf(e, "%lld:%lld", &sv_from, &sv_left); }
    extern u64 g_dbg_frames;
    if (sv_left > 0 && (long long)g_dbg_frames >= sv_from) { shot_at = true; sv_left--; }
    if (s_shot_request || shot_at || (every > 0 && frame % every == 0)) { s_shot_request = false; capture_backbuffer(); }
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT pr = s_swap->Present(g_settings.vsync ? s_sync_interval : 0, 0);
    QueryPerformanceCounter(&p1);
    s_present_wait = p1.QuadPart - p0.QuadPart;
    if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) // driver crash/update: tell the user instead of freezing
        fatal("The graphics device was lost (driver reset or GPU removed, reason %08lX).\nPlease restart the game.",
              (unsigned long)s_dev->GetDeviceRemovedReason());
    return true;
}

void platform_present_image(const u32* rgba, int w, int h, int stride, bool enabled) {
    if (w != s_sw_w || h != s_sw_h || !s_sw_tex) {
        if (s_sw_srv) s_sw_srv->Release();
        if (s_sw_tex) s_sw_tex->Release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        s_dev->CreateTexture2D(&td, nullptr, &s_sw_tex);
        s_dev->CreateShaderResourceView(s_sw_tex, nullptr, &s_sw_srv);
        s_sw_w = w;
        s_sw_h = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(s_ctx->Map(s_sw_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int y = 0; y < h; y++) memcpy((u8*)m.pData + y * m.RowPitch, rgba + y * stride, w * 4);
        s_ctx->Unmap(s_sw_tex, 0);
    }
    d3d_present_texture(s_sw_srv, 0, 0, 1, 1, w, h, h, enabled);
}

// ---------------------------------------------------------------- pacing
static LARGE_INTEGER s_freq, s_next;
static bool s_fast;

void platform_frame_pace() {
    if (!s_freq.QuadPart) { QueryPerformanceFrequency(&s_freq); QueryPerformanceCounter(&s_next); timeBeginPeriod(1); }
    if (s_fast) { QueryPerformanceCounter(&s_next); return; }
    double hz = (double)CPU_HZ / (double)CYCLES_PER_FRAME;
    s_next.QuadPart += (LONGLONG)(s_freq.QuadPart / hz);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart > s_next.QuadPart + s_freq.QuadPart / 10) { s_next = now; return; } // fell behind
    while (now.QuadPart < s_next.QuadPart) {
        LONGLONG left = (s_next.QuadPart - now.QuadPart) * 1000 / s_freq.QuadPart;
        if (left > 2) Sleep((DWORD)(left - 1));
        else YieldProcessor();
        QueryPerformanceCounter(&now);
    }
}

// refresh rate of the monitor the window is on (checked at most once a second)
int platform_refresh_hz() {
    static int hz = 60;
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 1000) return hz;
    last = now;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = (int)dm.dmDisplayFrequency;
    return hz;
}

// Unlocked frame rate: between this vblank and its deadline, show as many frames as the cap (fps, or the
// monitor's rate when fps < 0) allows. draw(frac) draws and presents the picture for the moment `frac` (0..1]
// of the way from the previous vblank to this one; it returns false if it could not. With vsync the monitor
// paces the presents (every Nth refresh for a cap that divides its rate); any other cap is kept by spacing the
// presents in time, across vblanks, so 144 or 165 average out right although they don't divide 60.
void platform_present_unlocked(int fps, bool (*draw)(double frac)) {
    if (!s_freq.QuadPart) { QueryPerformanceFrequency(&s_freq); QueryPerformanceCounter(&s_next); timeBeginPeriod(1); }
    if (s_fast) {
        // fast-forward and the test harness: no waiting; TB_UNLOCKED_STEPS=n shows n evenly spaced moments
        static int steps = getenv("TB_UNLOCKED_STEPS") ? std::clamp(atoi(getenv("TB_UNLOCKED_STEPS")), 1, 16) : 1;
        for (int i = 1; i <= steps; i++) if (!draw((double)i / steps)) break;
        QueryPerformanceCounter(&s_next);
        platform_pump();
        return;
    }
    const double vhz = (double)CPU_HZ / (double)CYCLES_PER_FRAME;
    const LONGLONG period = (LONGLONG)(s_freq.QuadPart / vhz);
    LONGLONG start = s_next.QuadPart; // the previous deadline: frac 0
    s_next.QuadPart += period;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart > s_next.QuadPart + s_freq.QuadPart / 10) { s_next = now; start = now.QuadPart - period; } // fell behind
    const int hz = platform_refresh_hz();
    const bool vs = g_settings.vsync;
    const int cap = fps > 0 ? fps : hz;
    const double per = (double)hz / cap; // refreshes per frame
    const int whole = std::clamp((int)std::lround(per), 1, 4);
    s_sync_interval = vs && std::fabs(per - whole) < 0.05 ? whole : 1;
    const bool timed = !vs || s_sync_interval * cap < hz * 0.95; // vsync alone would show more than the cap
    const LONGLONG refresh = s_freq.QuadPart * s_sync_interval / hz; // between shown frames, with vsync
    const LONGLONG gap = s_freq.QuadPart / cap;
    static LONGLONG next_free = 0;  // earliest time for the next present (timed); carries over between vblanks
    static LONGLONG prev_shown = 0; // with vsync: the refresh that puts up the previous frame
    for (int shown_n = 0;;) {
        QueryPerformanceCounter(&now);
        const LONGLONG at = timed ? std::max(next_free, now.QuadPart) : now.QuadPart;
        // with vsync a present waits until the frame before it is up. One that would still be waiting (or not yet
        // due) at the next vblank belongs to that vblank, but every vblank shows at least one.
        const bool late = (vs ? std::max(at, prev_shown) : at) >= s_next.QuadPart;
        if (late && shown_n) break;
        if (!late && at > now.QuadPart) {
            LONGLONG ms = (at - now.QuadPart) * 1000 / s_freq.QuadPart;
            if (ms > 1) Sleep((DWORD)(ms - 1));
            else YieldProcessor();
            continue;
        }
        // the moment it reaches the screen: right away without vsync; with it, the first refresh after both now
        // and the previous frame's (counting refreshes from that one)
        LONGLONG shown = now.QuadPart;
        if (vs && prev_shown > 0) shown = prev_shown + std::max<LONGLONG>(1, (now.QuadPart - prev_shown + refresh - 1) / refresh) * refresh;
        // with vsync the picture runs a refresh behind: one put up as late as a refresh after the next vblank
        // still has its frame (frac can then go below 0, into the previous vblank)
        double frac = std::clamp((double)(shown - (vs ? refresh : 0) - start) / (double)period, -1.0, 1.0);
        const LONGLONG t_draw = now.QuadPart;
        if (!draw(frac)) break;
        shown_n++;
        QueryPerformanceCounter(&now);
        // a present that had to wait returned as the previous frame went up: anchor on that refresh
        const bool waited = s_present_wait > s_freq.QuadPart / 1000;
        if (vs) prev_shown = waited ? now.QuadPart + refresh : shown;
        static int pace_log = getenv("TB_PACE_LOG") ? atoi(getenv("TB_PACE_LOG")) : 0; // debug: the first n presents
        if (pace_log > 0) {
            pace_log--;
            auto ms = [&](LONGLONG t) { return (double)t * 1000.0 / (double)s_freq.QuadPart; };
            plog("pace: at %.2f ms, +%.2f ms in vblank, draw %.2f + present %.2f ms, %.2f ms left, shown at %.2f -> %.2f, frac %.3f, interval %d\n",
                 ms(now.QuadPart), ms(t_draw - start), ms(now.QuadPart - t_draw - s_present_wait), ms(s_present_wait),
                 ms(s_next.QuadPart - now.QuadPart), ms(shown), ms(vs ? prev_shown : shown), frac, s_sync_interval);
        }
        next_free = std::max(next_free + gap, now.QuadPart);
    }
    while (now.QuadPart < s_next.QuadPart) {
        LONGLONG ms = (s_next.QuadPart - now.QuadPart) * 1000 / s_freq.QuadPart;
        if (ms > 2) Sleep((DWORD)(ms - 1));
        else YieldProcessor();
        QueryPerformanceCounter(&now);
    }
    platform_pump();
}

// ---------------------------------------------------------------- input
static void hotkey(WPARAM k);

static void poll_input() {
    for (int p = 0; p < 4; p++) { g_pads[p].buttons = 0; g_pads[p].connected = false; g_pads[p].lx = g_pads[p].ly = g_pads[p].rx = g_pads[p].ry = 0x80; }
    g_pads[0].connected = true; // keyboard is always player 1
    if (s_focus) {
        struct { int vk; u16 b; } km[] = {
            {VK_UP, PAD_UP}, {VK_DOWN, PAD_DOWN}, {VK_LEFT, PAD_LEFT}, {VK_RIGHT, PAD_RIGHT},
            {'Z', PAD_CROSS}, {'X', PAD_CIRCLE}, {'A', PAD_SQUARE}, {'S', PAD_TRIANGLE},
            {'Q', PAD_L1}, {'W', PAD_R1}, {'E', PAD_L2}, {'R', PAD_R2},
            {VK_RETURN, PAD_START}, {VK_BACK, PAD_SELECT}, {VK_RSHIFT, PAD_SELECT},
        };
        for (auto& k : km) if (GetAsyncKeyState(k.vk) & 0x8000) g_pads[0].buttons |= k.b;
    }
    // button prompts follow whichever device was used last (or the ini choice)
    static u16 prev_kb = 0;
    const u16 kb = g_pads[0].buttons; // keyboard only, controllers are added below
    bool kb_used = (kb & ~prev_kb) != 0;
    prev_kb = kb;
    // controllers (SDL3); the first one shares player 1 with the keyboard
    int pad_family = gamepad_poll(0);
    switch (g_settings.button_icons) {
    case 1: g_input_device = DEV_PLAYSTATION; break;
    case 2: g_input_device = DEV_XBOX; break;
    case 3: g_input_device = DEV_KEYBOARD; break;
    default: {
        static bool first = true; // before any input: the controller if one is plugged in, else the keyboard
        if (first) { first = false; int f = gamepad_first_family(); g_input_device = f >= 0 ? (InputDevice)f : DEV_KEYBOARD; }
        if (kb_used) g_input_device = DEV_KEYBOARD;
        else if (pad_family >= 0) g_input_device = (InputDevice)pad_family;
    }
    }
    // test harness: run unthrottled; TB_TURBO=n (n > 1) only until vblank n, then at real speed
    static long long turbo_until = getenv("TB_TURBO") ? std::max(1LL, atoll(getenv("TB_TURBO"))) : 0;
    extern u64 g_dbg_frames;
    const bool turbo = turbo_until == 1 || (long long)g_dbg_frames < turbo_until;
    s_fast = !netplay_active() && (turbo || (s_focus && (GetAsyncKeyState(VK_TAB) & 0x8000))); // online: both run at real speed

    // scripted input for automated testing: TB_INPUT="600=start;640=;900=cross" (pad 1), TB_INPUT2 (pad 2)
    static std::vector<std::pair<int, u16>> script[2];
    static bool parsed = false;
    static int frame = 0;
    if (!parsed) {
        parsed = true;
        static const struct { const char* n; u16 b; } bn[] = {
            {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT}, {"cross", PAD_CROSS},
            {"circle", PAD_CIRCLE}, {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE}, {"start", PAD_START},
            {"select", PAD_SELECT}, {"l1", PAD_L1}, {"r1", PAD_R1}, {"l2", PAD_L2}, {"r2", PAD_R2}};
        const char* vars[2] = {"TB_INPUT", "TB_INPUT2"};
        for (int pad = 0; pad < 2; pad++) {
            const char* s = getenv(vars[pad]);
            if (!s) continue;
            std::string all = s;
            size_t p = 0;
            while (p < all.size()) {
                size_t e = all.find(';', p);
                std::string item = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
                p = e == std::string::npos ? all.size() : e + 1;
                size_t eq = item.find('=');
                if (eq == std::string::npos) continue;
                u16 b = 0;
                std::string names = item.substr(eq + 1) + "+";
                for (size_t q = 0, r; (r = names.find('+', q)) != std::string::npos; q = r + 1)
                    for (auto& x : bn) if (names.substr(q, r - q) == x.n) b |= x.b;
                script[pad].push_back({atoi(item.c_str()), b});
            }
        }
    }
    frame++;
    static int quit_at = getenv("TB_FRAMES") ? atoi(getenv("TB_FRAMES")) : 0;
    if (quit_at > 0 && frame >= quit_at) s_quit = true;
    // TB_KEYS="6400=F1;6500=F3": function keys pressed at those frames (test harness)
    static std::vector<std::pair<int, int>> keys;
    static bool keys_parsed = false;
    if (!keys_parsed) {
        keys_parsed = true;
        if (const char* e = getenv("TB_KEYS"))
            for (const char* p = e; *p;) {
                int f = 0, n = 0;
                if (sscanf(p, "%d=F%d", &f, &n) == 2 && n >= 1 && n <= 12) keys.push_back({f, n});
                p = strchr(p, ';');
                if (!p) break;
                p++;
            }
    }
    for (auto& k : keys) if (k.first == frame) hotkey(VK_F1 + k.second - 1);
    for (int pad = 0; pad < 2; pad++) {
        if (script[pad].empty()) continue;
        u16 sb = 0;
        for (auto& e : script[pad]) if (frame >= e.first) sb = e.second;
        // the script replaces the real devices: a plugged-in controller (stick drift) must not change a test run
        g_pads[pad].connected = true;
        g_pads[pad].buttons = sb;
        g_pads[pad].lx = g_pads[pad].ly = g_pads[pad].rx = g_pads[pad].ry = 0x80;
    }
    // skip_intro: tap Start while the boot logos and intro movies (24-bit MDEC frames) play. Ends once a
    // non-movie screen has stayed up for 7 s after a movie (the title), or 60 s after boot. Online the taps
    // are this player's own input, so both games see them.
    static int intro_phase = g_settings.skip_intro ? 0 : 2, intro_quiet = 0, intro_tick = 0; // 0 before, 1 in movies, 2 done
    if (intro_phase < 2) {
        DisplayInfo d = gpu_display();
        if (d.depth24 && d.enabled) {
            intro_phase = 1;
            intro_quiet = 0;
            if (intro_tick++ % 16 < 6) g_pads[0].buttons |= PAD_START;
        } else if ((intro_phase == 1 && ++intro_quiet > 420) || frame > 3600) {
            intro_phase = 2;
        }
    }
    netplay_sync(); // online: replaces the pads with both players' input for this frame
}

static void hotkey(WPARAM k) {
    switch (k) {
    case VK_F11: platform_toggle_fullscreen(); break;
    case VK_F12: s_shot_request = true; break;
    case VK_F9: netplay_dialog(); break;
    case VK_F2: if (g_settings.scale > 1) g_settings.scale--; plog("internal resolution x%d\n", g_settings.scale); break;
    case VK_F3: if (g_settings.scale < 8) g_settings.scale++; plog("internal resolution x%d\n", g_settings.scale); break;
    case VK_F4: if (netplay_active()) break; // changes the 3D maths: both players must match
        g_settings.widescreen = !g_settings.widescreen; g_gpu_widescreen_hack = g_settings.widescreen; break;
    case VK_F5: g_settings.bilinear = !g_settings.bilinear; break;
    case VK_F6: {
        static const char* names[ASPECT_COUNT] = {"4:3", "16:9", "stretch", "wide", "pixels"};
        g_settings.aspect = (g_settings.aspect + 1) % ASPECT_COUNT;
        plog("aspect %s\n", names[g_settings.aspect]);
        break;
    }
    case VK_F1:
        g_settings.anti_aliasing = (g_settings.anti_aliasing + 1) % AA_COUNT;
        plog("anti-aliasing %s\n", k_aa_names[g_settings.anti_aliasing]);
        break;
    case VK_F7:
        g_settings.texture_filter = (g_settings.texture_filter + 1) % TEX_COUNT;
        plog("texture filtering %s\n", k_tex_filter_names[g_settings.texture_filter]);
        break;
    case VK_F8: g_settings.true_color = !g_settings.true_color; break;
    }
}

bool platform_pump_messages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return !s_quit;
}

void platform_set_title(const char* text) { if (s_hwnd) SetWindowTextA(s_hwnd, text); }
void platform_request_quit() { s_quit = true; }

bool platform_pump() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    poll_input();
    if (s_quit) {
        netplay_shutdown();
        settings_save(g_ini_path);
        gamepad_shutdown(); // stops rumble
        plog("quit\n");
        ExitProcess(0);
    }
    return !s_quit;
}

// ---------------------------------------------------------------- audio
static IXAudio2* s_xa;
static IXAudio2MasteringVoice* s_master;
static IXAudio2SourceVoice* s_src;
static const int AUDIO_BUFS = 32, AUDIO_FRAMES = 512;
static s16 s_abuf[AUDIO_BUFS][AUDIO_FRAMES * 2];
static int s_abuf_i, s_afill;

// Audio survives device loss and default-device switches (headset plugged/unplugged): XAudio2 reports
// a critical error, the MMDevice notifier reports a new default device; both just flag a rebuild that
// platform_audio_push() performs on the emulation thread.
static std::atomic<bool> s_audio_reset{false};

struct AudioEngineCallback : IXAudio2EngineCallback {
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) override { s_audio_reset = true; } // XAudio2 thread: flag only
};
static AudioEngineCallback s_audio_cb;

struct DeviceNotifier : IMMNotificationClient {
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) s_audio_reset = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
static DeviceNotifier s_dev_notifier;

static void audio_shutdown() {
    if (s_src) { s_src->DestroyVoice(); s_src = nullptr; }
    if (s_master) { s_master->DestroyVoice(); s_master = nullptr; }
    if (s_xa) { s_xa->UnregisterForCallbacks(&s_audio_cb); s_xa->Release(); s_xa = nullptr; }
}

static bool audio_init() {
    if (hidden_run()) return false; // silent
    static bool notifier = false;
    if (!notifier) {
        notifier = true;
        IMMDeviceEnumerator* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
            en->RegisterEndpointNotificationCallback(&s_dev_notifier); // kept for the process lifetime
    }
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { s_xa = nullptr; return false; }
    s_xa->RegisterForCallbacks(&s_audio_cb);
    if (FAILED(s_xa->CreateMasteringVoice(&s_master))) { s_master = nullptr; audio_shutdown(); return false; }
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 44100 * 4;
    if (FAILED(s_xa->CreateSourceVoice(&s_src, &wf))) { s_src = nullptr; audio_shutdown(); return false; }
    s_src->Start(0);
    return true;
}

// rebuild after a device change; with no device, retry every ~2 s so plugging one in works
static void audio_recover() {
    static ULONGLONG next_try = 0;
    if (s_audio_reset.exchange(false)) { plog("audio: device changed or lost, reopening\n"); audio_shutdown(); next_try = 0; }
    ULONGLONG now = GetTickCount64();
    if (s_src || now < next_try) return;
    next_try = now + 2000;
    if (audio_init()) plog("audio: output opened\n");
}

int platform_audio_queued() {
    if (!s_src) return 0;
    XAUDIO2_VOICE_STATE vs;
    s_src->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)vs.BuffersQueued * AUDIO_FRAMES;
}

int g_dbg_audio_peak;
void platform_audio_push(const s16* stereo, int frames) {
    for (int i = 0; i < frames * 2; i++) { int a = stereo[i] < 0 ? -stereo[i] : stereo[i]; if (a > g_dbg_audio_peak) g_dbg_audio_peak = a; }
    if (!s_src || s_audio_reset) audio_recover();
    if (!s_src) return;
    for (int i = 0; i < frames; i++) {
        s_abuf[s_abuf_i][s_afill * 2] = stereo[i * 2];
        s_abuf[s_abuf_i][s_afill * 2 + 1] = stereo[i * 2 + 1];
        if (++s_afill == AUDIO_FRAMES) {
            s_afill = 0;
            XAUDIO2_VOICE_STATE vs;
            s_src->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (vs.BuffersQueued < 8 && !s_fast) {
                XAUDIO2_BUFFER b = {};
                b.AudioBytes = AUDIO_FRAMES * 4;
                b.pAudioData = (const BYTE*)s_abuf[s_abuf_i];
                s_src->SubmitSourceBuffer(&b);
                s_abuf_i = (s_abuf_i + 1) % AUDIO_BUFS;
            }
        }
    }
}

// ---------------------------------------------------------------- hang watchdog (debugging aid)
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

int platform_private_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc);
    return (int)(pmc.PrivateUsage >> 20);
}
extern u64 g_dbg_frames, g_dbg_exceptions;
static HANDLE s_main_thread;

// symbolized stack of a thread (from its context) into the log
static void log_stack(HANDLE thread, CONTEXT ctx) {
    static bool sym = false;
    HANDLE proc = GetCurrentProcess();
    if (!sym) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(proc, nullptr, TRUE); sym = true; }
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, si)) plog("  %s+0x%llX\n", si->Name, disp);
        else plog("  %016llX\n", sf.AddrPC.Offset);
    }
    plog("  cpu: ra=%08X sp=%08X v0=%08X a0=%08X a1=%08X sr=%08X istat=%03X imask=%03X frame=%llu\n", g_cpu.r[31], g_cpu.r[29],
         g_cpu.r[2], g_cpu.r[4], g_cpu.r[5], g_cpu.cop0[12], g_istat, g_imask, g_dbg_frames);
}

// debugging aid: where the emulation thread is right now (native frames name the recompiled functions)
void platform_log_stack_here() {
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    log_stack(GetCurrentThread(), ctx);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    plog("CRASH: exception %08lX at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    log_stack(GetCurrentThread(), *ep->ContextRecord);
    if (!hidden_run()) MessageBoxA(nullptr, "Team Buddies crashed. Details were written to teambuddies.log.", "Team Buddies", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI watchdog(void*) {
    u64 last = 0;
    int stuck = 0;
    bool dumped = false;
    for (;;) {
        Sleep(1000);
        // progress = frames advance and interrupts keep being taken
        static u64 last_frames;
        bool moving = g_dbg_frames != last_frames && g_dbg_exceptions != last;
        last_frames = g_dbg_frames;
        last = g_dbg_exceptions;
        if (moving) { stuck = 0; dumped = false; continue; }
        if (++stuck < 3 || dumped) continue;
        dumped = true;
        SuspendThread(s_main_thread);
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        GetThreadContext(s_main_thread, &ctx);
        plog("watchdog: no frame for 3 s, main thread stack:\n");
        log_stack(s_main_thread, ctx);
        ResumeThread(s_main_thread);
    }
}

u32 g_watch_lo, g_watch_len;

void watch_hit(u32 a, u32 v, int size) {
    static int hits = 0;
    if (++hits > 20) return;
    static bool sym = false;
    HANDLE proc = GetCurrentProcess();
    if (!sym) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(proc, nullptr, TRUE); sym = true; }
    plog("watch: write%d %08X = %08X (frame %llu, ra=%08X sp=%08X)\n", size * 8, a, v, g_dbg_frames, g_cpu.r[31], g_cpu.r[29]);
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT i = 1; i < n; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, (DWORD64)frames[i], &disp, si)) plog("    %s+0x%llX\n", si->Name, disp);
    }
}

void platform_start_watchdog() {
    if (const char* w = getenv("TB_WATCH")) {
        unsigned lo = 0, len = 4;
        sscanf(w, "%x:%x", &lo, &len);
        g_watch_lo = lo & 0x1FFFFF;
        g_watch_len = len;
        plog("watching writes to %06X..%06X\n", g_watch_lo, g_watch_lo + g_watch_len);
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    SetUnhandledExceptionFilter(crash_filter);
}

// ---------------------------------------------------------------- init
bool platform_init() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    gamepad_init();
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
    wc.lpszClassName = "TeamBuddiesWnd";
    RegisterClassA(&wc);
    RECT r = {0, 0, g_settings.window_w, g_settings.window_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowA("TeamBuddiesWnd", "Team Buddies", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s_hwnd) return false;
    // test harness: TB_HIDDEN runs without showing the window (no focus taken), without sound and at low CPU
    // and GPU priority, so a test doesn't disturb whoever is using the PC
    const bool hidden = hidden_run();
    if (hidden) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    else ShowWindow(s_hwnd, SW_SHOW);
    if (!d3d_init()) fatal("Direct3D 11 is not available");
    if (hidden) {
        IDXGIDevice* dx = nullptr;
        if (SUCCEEDED(s_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx))) { dx->SetGPUThreadPriority(-7); dx->Release(); }
        plog("hidden test run: no window, no sound, low priority\n");
    } else if (g_settings.display != DISPLAY_WINDOW) {
        s_full_mode = g_settings.display;
        set_fullscreen(true);
    }
    if (!hidden && !audio_init()) plog("audio: XAudio2 unavailable, running without sound\n");
    g_gpu_widescreen_hack = g_settings.widescreen;
    return true;
}

void platform_shutdown() {
    if (s_exclusive && s_swap) s_swap->SetFullscreenState(FALSE, nullptr); // DXGI requires leaving exclusive mode first
    if (s_src) s_src->DestroyVoice();
    if (s_master) s_master->DestroyVoice();
    if (s_xa) s_xa->Release();
}
