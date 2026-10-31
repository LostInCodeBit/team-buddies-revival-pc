// main.cpp - Team Buddies native port entry point.
#include "files.h"
#ifdef _WIN32
#include <commdlg.h>
#else
#include <sys/resource.h>
#endif
#include "psx.h"
#include <fstream>
#include <sstream>
#include <map>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#endif

// ---------------------------------------------------------------- disc discovery
static std::string find_cue(std::string dir) {
    for (int up = 0; up < 3; up++) {
        std::vector<std::string> cues = list_files(dir.size() > 1 ? dir.substr(0, dir.size() - 1) : dir, ".cue");
        if (!cues.empty()) return dir + cues.front();
        if (dir.size() < 4) break;
        size_t s = dir.find_last_of("\\/", dir.size() - 2);
        if (s == std::string::npos) break;
        dir = dir.substr(0, s + 1);
    }
    return "";
}

#ifdef _WIN32
static std::string ask_cue() {
    char buf[MAX_PATH] = "";
    OPENFILENAMEA ofn = {sizeof ofn};
    ofn.lpstrFilter = "PlayStation disc image (*.cue)\0*.cue\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Select your Team Buddies (USA, SLUS-00869) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) return buf;
    return "";
}

static std::string exe_dir() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string d = exe_path;
    return d.substr(0, d.find_last_of("\\/") + 1);
}

// hybrid-graphics drivers (NVIDIA Optimus, AMD PowerXpress) run exes exporting these on the discrete GPU
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#else
std::string platform_ask_disc(); // the desktop's file chooser (platform_sdl.cpp)
static std::string ask_cue() { return getenv("TB_HIDDEN") ? "" : platform_ask_disc(); }

static std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "./";
    std::string d(buf, (size_t)n);
    return d.substr(0, d.find_last_of('/') + 1);
}

// the recompiled code nests calls deeply (the Windows exe reserves a 32 MB stack); the main thread's stack
// grows up to this limit, which is 8 MB by default
static void raise_stack_limit() {
    const rlim_t want = (rlim_t)64 << 20;
    rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= want) return;
    rl.rlim_cur = rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want ? rl.rlim_max : want;
    setrlimit(RLIMIT_STACK, &rl);
}
#endif

static void ev_spu(int id) {
    sched_repeat(id, 768 * 32, ev_spu);
    spu_tick(32);
}

int main(int argc, char** argv) {
#ifndef _WIN32
    raise_stack_limit();
#endif
    g_base_dir = exe_dir();
    g_ini_path = getenv("TB_INI") ? std::string(getenv("TB_INI")) : g_base_dir + "teambuddies.ini";
    const std::string& ini = g_ini_path;
    settings_load(ini);
#ifdef _WIN32
    // release builds have no console; log_level >= 2 opens one for the live log
    if (g_settings.log_level >= 2 && !getenv("TB_HIDDEN") && AllocConsole()) { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); }
#endif
    if (argc > 1 && strncmp(argv[1], "--", 2) != 0) g_settings.disc = argv[1];
    if (g_settings.disc.empty() || !file_exists(g_settings.disc)) g_settings.disc = find_cue(g_base_dir);
    if (g_settings.disc.empty() || !file_exists(g_settings.disc)) g_settings.disc = ask_cue();
    if (g_settings.disc.empty()) return 0;
    settings_save(ini);

    mem_init();
    cpu_init();
    sched_init();
    timers_init();
    dma_init();
    gte_init();
    gpu_init();
    spu_init();
    mdec_init();
    cd_init();
    bios_init();
    // saves: savedata/SLUS-00869/*.bin; an old memcards/cardN.mcd is split up once (TB_CARD_DIR: test harness, both)
    if (const char* cd = getenv("TB_CARD_DIR")) card_init(cd, cd);
    else card_init(g_base_dir + "savedata", g_base_dir + "memcards");

    if (!cd_open(g_settings.disc)) fatal("Cannot open the disc image:\n%s", g_settings.disc.c_str());
    std::vector<u8> cnf = cd_read_file("SYSTEM.CNF");
    std::string cs(cnf.begin(), cnf.end());
    if (cs.find("SLUS_008.69") == std::string::npos)
        fatal("This disc is not Team Buddies (USA, SLUS-00869).\nPlease select the correct disc image.");
    std::vector<u8> exe = cd_read_file("SLUS_008.69");
    if (exe.size() < 0x800) fatal("Cannot read SLUS_008.69 from the disc");
    // the recompiled code must match the executable on the disc
    {
        const RecImage& im = g_rec_images[0];
        u32 base = *(u32*)&exe[0x18];
        int checked = 0, bad = 0;
        for (int i = 0; i < g_rec_func_count && checked < 64; i++) {
            const RecFunc& f = g_rec_funcs[i];
            if (f.img != 0 || f.addr < im.base || f.addr + 8 > im.end) continue;
            u32 off = f.addr - base + 0x800;
            if (off + 8 > exe.size()) continue;
            checked++;
            if (*(u32*)&exe[off] != f.sig0 || *(u32*)&exe[off + 4] != f.sig1) bad++;
        }
        if (bad) fatal("This disc image does not match the recompiled game (%d/%d functions differ).", bad, checked);
    }

    if (!platform_init()) fatal("Cannot create the game window");
    g_renderer = nullptr;
    icon_atlas_build(); // keyboard / Xbox button glyphs
    g_renderer = renderer_create_hardware(gpu_vram());
    if (!g_renderer) g_renderer = renderer_create_software(gpu_vram());
    if (!netplay_start(argc, argv, exe)) { platform_shutdown(); return 0; } // --host / --join

    bios_boot(exe);
    sched_start();
    { void platform_start_watchdog(); platform_start_watchdog(); }
    sched_set(EV_SPU, 768 * 32, ev_spu);
    plog("starting game\n");
    cpu_run(g_cpu.pc);
    card_flush();
    netplay_shutdown();
    settings_save(ini);
    platform_shutdown();
    return 0;
}
