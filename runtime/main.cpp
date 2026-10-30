// main.cpp - Team Buddies native port entry point.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include "psx.h"
#include <fstream>
#include <sstream>
#include <map>
#include <cstdlib>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------- disc discovery
static bool file_exists(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static std::string find_cue(std::string dir) {
    for (int up = 0; up < 3; up++) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "*.cue").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            return dir + fd.cFileName;
        }
        if (dir.size() < 4) break;
        size_t s = dir.find_last_of("\\/", dir.size() - 2);
        if (s == std::string::npos) break;
        dir = dir.substr(0, s + 1);
    }
    return "";
}

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

static void ev_spu(int id) {
    sched_repeat(id, 768 * 32, ev_spu);
    spu_tick(32);
}

// hybrid-graphics drivers (NVIDIA Optimus, AMD PowerXpress) run exes exporting these on the discrete GPU
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char** argv) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    g_base_dir = exe_path;
    g_base_dir = g_base_dir.substr(0, g_base_dir.find_last_of("\\/") + 1);
    g_ini_path = getenv("TB_INI") ? std::string(getenv("TB_INI")) : g_base_dir + "teambuddies.ini";
    const std::string& ini = g_ini_path;
    settings_load(ini);
    // release builds have no console; log_level >= 2 opens one for the live log
    if (g_settings.log_level >= 2 && !getenv("TB_HIDDEN") && AllocConsole()) { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); }
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
    if (g_settings.renderer == 1) g_renderer = renderer_create_d3d11(gpu_vram());
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
