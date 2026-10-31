// psx.h - internal interfaces between the native runtime modules.
#pragma once
#include "rec.h"
#include <cstdio>
#include <string>
#include <vector>

#ifndef PATH_SEP
#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif
#endif
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

// ------------------------------------------------------------------ logging
void plog(const char* fmt, ...);
void log_once(const char* key, const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);

// ------------------------------------------------------------------ settings (teambuddies.ini)
#include "settings.h"

// ------------------------------------------------------------------ scheduler (CPU-cycle based)
enum EventId { EV_VBLANK, EV_TIMER0, EV_TIMER1, EV_TIMER2, EV_CDROM_CMD, EV_CDROM_READ, EV_CDROM_SEEK, EV_SPU, EV_DMA, EV_CARD, EV_MDEC, EV_SIO, EV_COUNT };
typedef void (*EventFn)(int id);
void sched_init();
void sched_start();
void sched_set(int id, u64 cpu_cycles_from_now, EventFn fn);
void sched_repeat(int id, u64 period_cycles, EventFn fn); // relative to the previous deadline
void sched_cancel(int id);
bool sched_active(int id);
u64 sched_remaining(int id);
u64 cpu_now();             // current time in CPU cycles
void sched_run();

const u64 CPU_HZ = 33868800;
const u64 CYCLES_PER_LINE = 2152;
const u64 LINES_PER_FRAME = 263;
const u64 CYCLES_PER_FRAME = CYCLES_PER_LINE * LINES_PER_FRAME;

// ------------------------------------------------------------------ interrupts
enum { IRQ_VBLANK, IRQ_GPU, IRQ_CDROM, IRQ_DMA, IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_SIO0, IRQ_SIO1, IRQ_SPU, IRQ_PIO };
extern u32 g_istat, g_imask;
void irq_raise(int n);
void irq_update();       // re-evaluates whether an interrupt must be taken
bool irq_take_now(const CPU* c);

// ------------------------------------------------------------------ memory / io
void mem_init();
u32 hw_read(u32 phys, int size);
void hw_write(u32 phys, u32 v, int size);

// ------------------------------------------------------------------ dma
void dma_init();
u32 dma_read(u32 off);
void dma_write(u32 off, u32 v);
void dma_mdec_ready();

// ------------------------------------------------------------------ timers
void timers_init();
u32 timers_read(u32 off);
void timers_write(u32 off, u32 v);
void timers_vblank(bool start);
void timers_hblank_reset();

// ------------------------------------------------------------------ cpu / dispatch
void cpu_init();
void cpu_run(u32 pc);
RecFn rec_lookup(u32 addr);
void interp_call(CPU* c, u32 pc);
extern int g_exception_depth;

// ------------------------------------------------------------------ bios (HLE kernel)
void bios_init();
void bios_boot(const std::vector<u8>& exe);
void bios_irq_exception(CPU* c);
void bios_vblank();
struct ExceptionReturn {};

// ------------------------------------------------------------------ gte
void gte_init();

// ------------------------------------------------------------------ gpu
void gpu_init();
void gpu_reset();
u32 gpu_read(u32 off);
void gpu_write(u32 off, u32 v);
void gpu_gp0(u32 v);
void gpu_gp1(u32 v);
u32 gpu_read_data();
void gpu_dma_write(const u32* words, int n);
void gpu_vblank();
u16* gpu_vram();
struct DisplayInfo {
    int x, y, w, h;      // in VRAM pixels (w is in 24-bit pixels when depth24)
    bool depth24;
    bool enabled;
    bool interlaced;
};
DisplayInfo gpu_display();
extern bool g_gpu_widescreen_hack;
extern int g_gpu_widescreen_q16;

// ------------------------------------------------------------------ renderer backend
struct Vertex {
    s32 x, y;
    u8 r, g, b;
    u8 u, v;
};
struct GpuDrawState {
    u16 texpage;           // GP0(E1) style bits 0-11
    u16 clut;              // x/16 | y<<6
    u8 tex_mask_x, tex_mask_y, tex_off_x, tex_off_y;
    s16 clip_x1, clip_y1, clip_x2, clip_y2;
    bool set_mask, check_mask;
    bool dither;
    s8 icon = -1;          // >= 0: draw from the button-glyph atlas slot instead of VRAM (icons.cpp)
    u8 icon_u = 0, icon_v = 0, icon_w = 0, icon_h = 0; // texel rectangle the glyph replaces
    s16 glyph = -1;        // >= 0: a letter of the game's fonts, drawn from its HD reconstruction (hd_text.cpp)
};
enum PrimFlags { PF_TEXTURED = 1, PF_SEMI = 2, PF_RAW = 4, PF_GOURAUD = 8 };
struct VertexF {             // sub-pixel position, for in-between frames
    float x, y;
    u8 r, g, b;
    u8 u, v;
};
struct Renderer {
    virtual ~Renderer() {}
    virtual void triangle(const Vertex* v, int flags, const GpuDrawState& st) = 0;
    virtual void rect(s32 x, s32 y, s32 w, s32 h, u8 r, u8 g, u8 b, u8 u, u8 v, int flags, const GpuDrawState& st, bool flipx, bool flipy) = 0;
    virtual void line(const Vertex& a, const Vertex& b, int flags, const GpuDrawState& st) = 0;
    virtual void fill(int x, int y, int w, int h, u32 color) = 0;
    virtual void vram_write(int x, int y, int w, int h, const u16* data, const GpuDrawState& st) = 0;
    virtual void vram_copy(int sx, int sy, int dx, int dy, int w, int h, const GpuDrawState& st) = 0;
    virtual void vram_read(int x, int y, int w, int h) = 0;   // make CPU copy (gpu_vram) current
    virtual void present(const DisplayInfo& d) = 0;
    virtual void flush() {}
    // frame interpolation (interp.cpp): redraw a recorded frame into a separate target and show that.
    // VRAM coordinates as usual; nothing here touches emulated VRAM.
    virtual bool interp_supported() const { return false; }
    virtual bool interp_ready(const DisplayInfo& d) { return false; } // target available for this display
    virtual bool interp_begin(const DisplayInfo& d) { return false; } // starts from the displayed buffer's pixels
    virtual void interp_tri(const VertexF* v, int flags, const GpuDrawState& st) {}
    virtual void interp_rect(float x, float y, s32 w, s32 h, u8 r, u8 g, u8 b, u8 u, u8 v, int flags, const GpuDrawState& st, bool flipx, bool flipy) {}
    virtual void interp_line(const VertexF& a, const VertexF& b, int flags, const GpuDrawState& st) {}
    virtual void interp_fill(int x, int y, int w, int h, u32 color) {}
    virtual void interp_present(const DisplayInfo& d, bool pace) {} // pace: then wait for the vblank as present() does
};

// ------------------------------------------------------------------ frame interpolation (interp.cpp)
extern bool g_interp_on;     // recording primitives (frame_rate above the game's own)
extern u32 g_gpu_packet;     // RAM address of the DMA packet being drawn (0 = written directly)
void interp_rec_poly(const Vertex* v, int n, int flags, const GpuDrawState& st);
void interp_rec_rect(s32 x, s32 y, s32 w, s32 h, u32 color, u8 u, u8 v, int flags, const GpuDrawState& st, bool flipx, bool flipy);
void interp_rec_line(const Vertex& a, const Vertex& b, int flags, const GpuDrawState& st);
void interp_rec_fill(int x, int y, int w, int h, u32 color);
void interp_flip(const DisplayInfo& d);    // the game showed a new frame (display start changed)
bool interp_present(const DisplayInfo& d);  // at vblank: true if an in-between frame was shown
Renderer* renderer_create_software(u16* vram);
Renderer* renderer_create_d3d11(u16* vram);   // Windows (gpu_d3d11.cpp)
Renderer* renderer_create_sdlgpu(u16* vram);  // SDL3 GPU: Vulkan on Linux (sdl/gpu_sdlgpu.cpp)
Renderer* renderer_create_hardware(u16* vram); // the platform's GPU renderer when renderer = hardware, else nullptr

// ------------------------------------------------------------------ cdrom
bool cd_open(const std::string& cue);
void cd_init();
u8 cd_read(u32 off);
void cd_write(u32 off, u8 v);
u32 cd_dma_read();
void cd_dma_read_block(u8* dst, u32 bytes);
bool cd_read_raw_sector(u32 lba, u8* out2352);
bool cd_find_file(const char* name, u32& lba, u32& size);
std::vector<u8> cd_read_file(const char* name);
void cd_get_audio(s16* out, int frames);   // CD-DA / XA output at 44.1 kHz, stereo

// ------------------------------------------------------------------ spu
void spu_init();
u16 spu_read(u32 off);
void spu_write(u32 off, u16 v);
void spu_dma_write(const u8* src, u32 bytes);
void spu_dma_read(u8* dst, u32 bytes);
void spu_tick(int samples);

// ------------------------------------------------------------------ mdec
void mdec_init();
u32 mdec_read(u32 off);
void mdec_write(u32 off, u32 v);
void mdec_dma_in(const u32* words, int n);
void mdec_dma_out(u32* words, int n);
bool mdec_out_ready();

// ------------------------------------------------------------------ pads / memory cards
enum PadButton {
    PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008,
    PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
    PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
};
struct PadState {
    bool connected;
    u16 buttons;        // 1 = pressed
    u8 lx, ly, rx, ry;
    u8 motor_small, motor_big; // rumble requested by the game
};
extern PadState g_pads[4];
u32 sio_read(u32 off);
void sio_write(u32 off, u32 v);
void card_init(const std::string& root, const std::string& legacy_dir); // savedata root; folder of old cardN.mcd
void card_vblank(); // writes saves back once the game has stopped writing
void card_flush();  // at exit
bool card_read_frame(int slot, int frame, u8* out128);
bool card_write_frame(int slot, int frame, const u8* in128);
bool card_present(int slot);

// ------------------------------------------------------------------ platform
bool platform_init();
void platform_shutdown();
bool platform_pump();                // false when the user closed the window
int platform_refresh_hz();            // the monitor the window is on
void platform_present_unlocked(int fps, bool (*draw)(double frac)); // frames between vblanks, then pacing (interp.cpp)
void platform_frame_pace();
void platform_audio_push(const s16* stereo, int frames);
int platform_audio_queued();         // frames waiting to be played
void* platform_hwnd();
void platform_get_window_size(int& w, int& h);
void platform_toggle_fullscreen();
void platform_present_image(const u32* rgba, int w, int h, int stride, bool enabled); // a CPU-made picture (software renderer, 24-bit movies)
extern Renderer* g_renderer;
extern std::string g_base_dir;
extern std::string g_ini_path; // teambuddies.ini, or TB_INI (test harness)

// ------------------------------------------------------------------ icons (button prompts follow the input device)
void icons_frame_start(u64 frame);
bool gpu_tracing(); // inside a TB_GPU_TRACE frame
void icons_trace(const char* kind, s32 x, s32 y, s32 w, s32 h, int u0, int v0, int u1, int v1, int flags, const GpuDrawState& st);
enum InputDevice { DEV_PLAYSTATION, DEV_XBOX, DEV_KEYBOARD };
extern InputDevice g_input_device;   // last device the player used (platform layer)
enum IconId { ICON_NONE = -1, ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE, ICON_START, ICON_SELECT,
              ICON_L1, ICON_R1, ICON_L2, ICON_R2, ICON_COUNT };
IconId icons_lookup(const GpuDrawState& st, int u0, int v0, int w, int h);
void icons_vram_changed();
void icon_atlas_build();
const u32* icon_atlas_pixels(int& w, int& h); // premultiplied BGRA, nullptr if unavailable
int icon_slot(InputDevice dev, IconId id);    // atlas slot, -1 = keep the game's own icon
u16 icon_sample_555(int slot, int u, int v, int w, int h);
void icons_apply(GpuDrawState& ds, int u0, int v0, int w, int h); // sets ds.icon when the texels are a prompt
// ------------------------------------------------------------------ HD text (hd_text.cpp)
const int HD_TEXT_K = 12;   // reconstruction pixels per texel
const int HD_TEXT_PAD = 1;  // texels of border around a letter's reconstruction
const int HD_TEXT_MAX_W = 32, HD_TEXT_MAX_H = 24; // largest letter sprite, texels
struct HdGlyph {
    u8 w, h;                // texels
    u8 lo, hi;              // palette entries of the dark rim (coverage 0) and of the letter (coverage 1)
    u8 style;               // 0 anti-aliased letter (an even rim is drawn), 1 / 2 drawn outline 1 / 2 texels wide (kept)
    std::vector<u8> idx;    // w*h palette indices
    u16 pal[16];
};
void hd_text_apply(GpuDrawState& ds, int u0, int v0, int w, int h); // sets ds.glyph when the texels are a letter
void hd_text_vram_changed();
const HdGlyph* hd_text_glyph(int id);
// input of the GPU reconstruction: fw x fh pairs (opacity, coverage 0 rim .. 255 letter), rows pitch pairs apart
void hd_text_source(const HdGlyph& g, u8* rg, int pitch, int& fw, int& fh);
bool gamepad_init();
void gamepad_shutdown();
int gamepad_poll(int first_player); // returns the InputDevice family used this frame, or -1
int gamepad_first_family();
const std::vector<u8>& card_image(int slot);
void card_replace(int slot, const u8* data128k);

// ------------------------------------------------------------------ online play (netplay.cpp)
bool netplay_start(int argc, char** argv, const std::vector<u8>& exe); // false = cancelled, quit
bool netplay_active();
void netplay_sync();      // at every frame boundary, after local input was read into g_pads[0]
void netplay_shutdown();  // tell the peer, restore local settings
int netplay_local_slot(); // player index of this machine (0 when offline)
void platform_set_title(const char* text);
bool platform_pump_messages(); // window messages only; false once the user closed the window
void netplay_dialog();     // F9
