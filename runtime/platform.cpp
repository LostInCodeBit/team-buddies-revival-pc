// platform.cpp - Win32 window, Direct3D 11 presentation, keyboard (+ SDL3 controllers, gamepad.cpp), XAudio2.
// What the Linux (SDL3) platform layer shares with this one is in platform_common.cpp: logging, screenshots,
// frame pacing, input and the hotkeys.
#include "platform_d3d.h"
#include <dxgi1_6.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <atomic>
#include <timeapi.h>
#include "platform_common.h"
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

// ---------------------------------------------------------------- window
static HWND s_hwnd;
static bool s_fullscreen;
static RECT s_saved_rect;
static bool s_focus = true;

void* platform_hwnd() { return s_hwnd; }

// ---------------------------------------------------------------- hooks for platform_common.cpp
long long plat_ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
long long plat_tick_hz() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }
void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }
void plat_yield() { YieldProcessor(); }
// only for a debugger: with another program listening for debug output (seen while Valorant ran) each call
// can block for seconds
void plat_debug_out(const char* s) { if (IsDebuggerPresent()) OutputDebugStringA(s); }
void plat_error_box(const char* s) { MessageBoxA(nullptr, s, "Team Buddies", MB_OK | MB_ICONERROR); }
void plat_exit(int code) { ExitProcess(code); }
bool plat_focused() { return s_focus; }
bool plat_key(PlatKey k) {
    static const int vk[PK_COUNT] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', 'A', 'S', 'Q', 'W', 'E', 'R',
                                     VK_RETURN, VK_BACK, VK_RSHIFT, VK_TAB};
    return (GetAsyncKeyState(vk[k]) & 0x8000) != 0;
}

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

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: platform_request_quit(); return 0;
    case WM_SIZE: resize_swapchain(); return 0;
    case WM_ACTIVATE:
        s_focus = LOWORD(w) != WA_INACTIVE;
        if (s_focus && s_exclusive) s_reenter_exclusive = true; // handled on the next present, outside the message
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) { platform_toggle_fullscreen(); return 0; }
        if (w == VK_F10) { // F10 arrives as a system key (it would open the window menu)
            if (!(l & (1 << 30))) hotkey(10);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (!(l & (1 << 30)) && w >= VK_F1 && w <= VK_F12) hotkey((int)(w - VK_F1) + 1);
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

static const char* k_present_hlsl =
#include "shaders/present_hlsl.inc"
    ;

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
            shot_save((const u8*)m.pData, td.Width, td.Height, m.RowPitch);
            s_ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}

void d3d_present_texture(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1, int src_w, int src_h, int native_h, bool enabled) {
    g_sync_interval = 1;
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
    if (shot_due()) capture_backbuffer();
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT pr = s_swap->Present(g_settings.vsync ? g_sync_interval : 0, 0);
    QueryPerformanceCounter(&p1);
    g_present_wait = p1.QuadPart - p0.QuadPart;
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

bool platform_pump_messages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return !platform_quit_requested();
}

void platform_set_title(const char* text) { if (s_hwnd) SetWindowTextA(s_hwnd, text); }

bool platform_pump() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    poll_input();
    platform_quit_if_requested();
    return true;
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
            if (vs.BuffersQueued < 8 && !platform_fast()) {
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
    timeBeginPeriod(1); // 1 ms Sleep for the frame pacing
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
