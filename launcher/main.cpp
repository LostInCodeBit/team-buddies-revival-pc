// main.cpp - TeamBuddiesLauncher.exe: settings for teambuddies.ini, disc choice, patch notes, and Play.
// The launcher only edits the ini (atomically, through runtime/settings.cpp) and starts TeamBuddies.exe;
// it hides while the game runs and comes back, re-reading the ini, when the game exits.
#include "ui.h"
#include "settings.h"
#include <commdlg.h>
#include <objbase.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t* kClass = L"TeamBuddiesLauncher";
static const float kWinW = 1040, kWinH = 680;  // DIPs
static const float kNavW = 248, kFooterH = 80, kTitleH = 36;
enum { WM_GAME_EXITED = WM_APP + 1 };

// ---------------------------------------------------------------- text helpers
static std::wstring widen(const std::string& s, UINT cp = CP_ACP) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
// the game opens files with the ANSI code page; a path it cannot represent is refused
static bool narrow_acp(const std::wstring& w, std::string& out) {
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), nullptr, 0, nullptr, &lossy);
    if (lossy || n <= 0) return false;
    out.assign(n, 0);
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), &out[0], n, nullptr, &lossy);
    return !lossy;
}
static std::wstring file_name(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}
static std::wstring dir_of(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s + 1);
}
static bool exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring ago(FILETIME ft) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG a = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime, b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    long long s = b > a ? (long long)((b - a) / 10000000ULL) : 0;
    wchar_t buf[64];
    if (s < 90) return L"just now";
    if (s < 120) return L"a minute ago";
    if (s < 3600) swprintf(buf, 64, L"%lld minutes ago", s / 60);
    else if (s < 2 * 3600) return L"an hour ago";
    else if (s < 86400) swprintf(buf, 64, L"%lld hours ago", s / 3600);
    else if (s < 2 * 86400) return L"yesterday";
    else if (s < 30 * 86400) swprintf(buf, 64, L"%lld days ago", s / 86400);
    else {
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);
        static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        swprintf(buf, 64, L"on %d %s %d", st.wDay, mon[(st.wMonth + 11) % 12], st.wYear);
    }
    return buf;
}

// ---------------------------------------------------------------- app state
static HWND s_hwnd;
static std::wstring s_dir;         // folder of the launcher (and the game)
static std::string s_ini;          // teambuddies.ini (TB_INI overrides, like the game)
static std::wstring s_saves;       // savedata\SLUS-00869 (TB_CARD_DIR overrides the savedata root, like the game)
static std::wstring s_legacy;      // folder of old cardN.mcd images
static int s_page = 0;             // 0 play, 1 settings, 2 patch notes
static int s_tab = 0;              // settings: graphics, display, game, controls
static HANDLE s_game;              // running game process
static std::wstring s_snack;       // M3 snackbar message
static ULONGLONG s_snack_until;
static float s_notes_scroll;

struct DiscInfo { int state = 0; std::wstring title, detail; } static s_disc; // 0 none, 1 recognised, 2 other game, 3 missing
struct SaveInfo { int count = 0; FILETIME newest = {}; bool legacy = false; } static s_save;
static std::vector<std::wstring> s_pads;

static void snack(const std::wstring& s) {
    s_snack = s;
    s_snack_until = GetTickCount64() + 6000;
    SetTimer(s_hwnd, 2, 6100, nullptr);
    InvalidateRect(s_hwnd, nullptr, FALSE);
}

static void save() {
    if (!settings_save(s_ini)) snack(L"Couldn't save teambuddies.ini. Is the game folder read-only?");
}

// .cue -> first FILE's .bin; Team Buddies (USA) has SLUS_008.69 in SYSTEM.CNF near the start of the data track.
// Cached by path and timestamp: it runs whenever the window is activated.
static void check_disc() {
    static std::string last_path;
    static FILETIME last_time;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    bool have = !g_settings.disc.empty() && GetFileAttributesExW(widen(g_settings.disc).c_str(), GetFileExInfoStandard, &fa);
    if (have && s_disc.state != 0 && last_path == g_settings.disc && CompareFileTime(&last_time, &fa.ftLastWriteTime) == 0) return;
    last_path = g_settings.disc;
    last_time = fa.ftLastWriteTime;
    s_disc = DiscInfo();
    if (g_settings.disc.empty()) return;
    std::wstring path = widen(g_settings.disc);
    if (!exists(path)) {
        s_disc.state = 3;
        s_disc.title = L"Disc image not found";
        s_disc.detail = path;
        return;
    }
    std::wstring bin = path;
    std::wstring low = path;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    if (low.size() > 4 && low.substr(low.size() - 4) == L".cue") {
        std::ifstream f(path);
        std::string line;
        bin.clear();
        while (std::getline(f, line)) {
            size_t p = line.find("FILE");
            if (p == std::string::npos) continue;
            size_t q0 = line.find('"', p), q1 = q0 == std::string::npos ? q0 : line.find('"', q0 + 1);
            if (q1 == std::string::npos) continue;
            std::wstring name = widen(line.substr(q0 + 1, q1 - q0 - 1));
            bin = name.find(L':') != std::wstring::npos ? name : dir_of(path) + name;
            break;
        }
        if (bin.empty() || !exists(bin)) {
            s_disc.state = 3;
            s_disc.title = L"Track file missing";
            s_disc.detail = bin.empty() ? file_name(path) + L" lists no track file" : file_name(bin) + L" is not next to the .cue";
            return;
        }
    }
    bool found = false;
    if (FILE* f = _wfopen(bin.c_str(), L"rb")) {
        static char buf[1 << 16];
        const char key[] = "SLUS_008.69";
        size_t total = 0, n;
        std::string tail;
        while (!found && total < (8u << 20) && (n = fread(buf, 1, sizeof buf, f)) > 0) {
            std::string chunk = tail + std::string(buf, n);
            found = chunk.find(key) != std::string::npos;
            tail = chunk.substr(chunk.size() > 16 ? chunk.size() - 16 : 0);
            total += n;
        }
        fclose(f);
    }
    s_disc.state = found ? 1 : 2;
    s_disc.title = found ? L"Team Buddies (USA)" : L"Not Team Buddies (USA)";
    s_disc.detail = found ? file_name(path) + L" · SLUS-00869" : file_name(path) + L" · the port needs SLUS-00869";
}

// the same search the game does: a .cue next to the exe or up to two folders above
static void find_disc() {
    if (!g_settings.disc.empty()) return;
    std::wstring d = s_dir;
    for (int up = 0; up < 3 && !d.empty(); up++) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"*.cue").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            std::string a;
            if (narrow_acp(d + fd.cFileName, a)) { g_settings.disc = a; save(); }
            return;
        }
        if (d.size() < 4) break;
        size_t s = d.find_last_of(L"\\/", d.size() - 2);
        if (s == std::wstring::npos) break;
        d = d.substr(0, s + 1);
    }
}

// saves are one file each in savedata\SLUS-00869 (card 1) and its card2 folder; an old memcards\card1.mcd
// still counts until the game's next start splits it up
static void check_saves() {
    SaveInfo v;
    for (const wchar_t* sub : {L"", L"\\card2"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((s_saves + sub + L"\\*.bin").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            v.count++;
            if (CompareFileTime(&fd.ftLastWriteTime, &v.newest) > 0) v.newest = fd.ftLastWriteTime;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::wstring mcd = s_legacy + L"\\card1.mcd";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(mcd.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 131072) {
        if (FILE* f = _wfopen(mcd.c_str(), L"rb")) {
            unsigned char dir[128 * 16];
            if (fread(dir, 1, sizeof dir, f) == sizeof dir)
                for (int b = 1; b < 16; b++) if (dir[b * 128] == 0x51) { v.count++; v.legacy = true; } // first block of a save
            fclose(f);
        }
        if (v.legacy && CompareFileTime(&fa.ftLastWriteTime, &v.newest) > 0) v.newest = fa.ftLastWriteTime;
    }
    s_save = v;
}

static void open_saves() {
    std::wstring d = s_saves;
    for (size_t i = 3; i <= d.size(); i++) if (i == d.size() || d[i] == L'\\') CreateDirectoryW(d.substr(0, i).c_str(), nullptr);
    ShellExecuteW(s_hwnd, L"explore", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void check_pads() {
    std::vector<std::wstring> v;
    if (SDL_WasInit(SDL_INIT_GAMEPAD)) {
        SDL_UpdateGamepads();
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            for (int i = 0; i < n; i++) {
                const char* name = SDL_GetGamepadNameForID(ids[i]);
                v.push_back(name ? widen(name, CP_UTF8) : L"Controller");
            }
            SDL_free(ids);
        }
    }
    if (v != s_pads) { s_pads = v; InvalidateRect(s_hwnd, nullptr, FALSE); }
}

static void refresh_all() {
    find_disc();
    check_disc();
    check_saves();
    check_pads();
}

static void choose_disc() {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.hwndOwner = s_hwnd;
    ofn.lpstrFilter = L"PlayStation disc image (*.cue)\0*.cue\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose your Team Buddies disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string a;
    if (!narrow_acp(buf, a)) { snack(L"That folder name has characters the game can't open. Move the disc image to a plain-named folder."); return; }
    g_settings.disc = a;
    save();
    check_disc();
    if (s_disc.state == 2) snack(L"This disc image doesn't contain Team Buddies (USA). The port only runs SLUS-00869.");
}

// ---------------------------------------------------------------- game process
static DWORD WINAPI wait_game(void* h) {
    WaitForSingleObject((HANDLE)h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess((HANDLE)h, &code);
    PostMessageW(s_hwnd, WM_GAME_EXITED, code, 0);
    return 0;
}

static void play() {
    if (s_game) return;
    std::wstring exe = s_dir + L"TeamBuddies.exe";
    if (!exists(exe)) { snack(L"TeamBuddies.exe is missing from this folder."); return; }
    if (s_disc.state == 3 || s_disc.state == 0) { choose_disc(); if (s_disc.state != 1 && s_disc.state != 2) return; }
    save();
    // let the game own the controllers while it runs
    if (SDL_WasInit(SDL_INIT_GAMEPAD)) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, s_dir.c_str(), &si, &pi)) {
        wchar_t msg[160];
        swprintf(msg, 160, L"Couldn't start the game (Windows error %lu).", GetLastError());
        snack(msg);
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        return;
    }
    CloseHandle(pi.hThread);
    s_game = pi.hProcess;
    ui::close_popup();
    ShowWindow(s_hwnd, SW_HIDE);
    if (HANDLE t = CreateThread(nullptr, 0, wait_game, s_game, 0, nullptr)) CloseHandle(t);
}

static void game_exited(DWORD code) {
    CloseHandle(s_game);
    s_game = nullptr;
    settings_load(s_ini); // the game saves hotkey changes (F2-F11) on exit
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    refresh_all();
    ShowWindow(s_hwnd, SW_SHOW);
    SetForegroundWindow(s_hwnd);
    if (code != 0) {
        wchar_t msg[200];
        swprintf(msg, 200, L"The game stopped with an error (code %lu). Details are in teambuddies.log.", code);
        snack(msg);
    }
}

// ---------------------------------------------------------------- patch notes (CHANGELOG.md, embedded)
struct NoteLine { int kind; std::wstring text; }; // 0 version, 1 section, 2 bullet, 3 paragraph
static std::vector<NoteLine> s_notes;

static std::wstring strip_md(std::wstring s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'`') continue;
        if (s[i] == L'*' && i + 1 < s.size() && s[i + 1] == L'*') { i++; continue; }
        o += s[i];
    }
    return o;
}

static void load_notes() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(100), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    if (!g) return;
    std::string md((const char*)LockResource(g), SizeofResource(nullptr, r));
    std::wstring all = widen(md, CP_UTF8);
    size_t p = 0;
    while (p <= all.size()) {
        size_t e = all.find(L'\n', p);
        std::wstring ln = all.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
        p = e == std::wstring::npos ? all.size() + 1 : e + 1;
        while (!ln.empty() && (ln.back() == L'\r' || ln.back() == L' ')) ln.pop_back();
        if (ln.empty() || ln.rfind(L"# ", 0) == 0) continue;
        if (ln.rfind(L"## ", 0) == 0) s_notes.push_back({0, ln.substr(3)});
        else if (ln.rfind(L"### ", 0) == 0) s_notes.push_back({1, ln.substr(4)});
        else if (ln.rfind(L"- ", 0) == 0) s_notes.push_back({2, strip_md(ln.substr(2))});
        else if (ln.rfind(L"  ", 0) == 0 && !s_notes.empty() && s_notes.back().kind == 2) {
            size_t a = ln.find_first_not_of(L' ');
            s_notes.back().text += L" " + strip_md(ln.substr(a));
        } else s_notes.push_back({3, strip_md(ln)});
    }
}

// ---------------------------------------------------------------- drawing: shared pieces
enum Ids { // widget ids (segmented buttons use id*32+segment, keep these below 1000)
    ID_MIN = 1, ID_CLOSE, ID_SAVES, ID_NAV = 10, ID_TABS = 20, ID_RESET = 30, ID_CHOOSE, ID_PLAY, ID_PLAY_BIG, ID_DISC_ROW,
    ID_RENDERER = 40, ID_SCALE, ID_TRUECOLOR, ID_TEXFILTER, ID_VSYNC, ID_FRAMERATE, ID_AA, ID_HDTEXT,
    ID_DISPLAY = 50, ID_WINSIZE, ID_ASPECT, ID_SCALING, ID_WIDESCREEN,
    ID_SKIPINTRO = 60, ID_CPU, ID_VOLUME, ID_MULTITAP,
    ID_ICONS = 70, ID_VIBRATION,
};

static float content_x() { return kNavW + 40; }
static float content_w() { return ui::W - kNavW - 80; }

static void nav() {
    using namespace ui;
    fill({0, 0, kNavW, H - kFooterH}, col::surface_low);
    // wordmark: the game's name, BUDDIES in the crate yellow
    text(L"TEAM", {28, 34, 200, 32}, F_WORDMARK, col::on_surface);
    text(L"BUDDIES", {28, 62, 200, 32}, F_WORDMARK, col::tertiary);
    text(L"Native PC port", {29, 98, 200, 18}, F_BODY_SMALL, col::on_surface_variant);
    struct Item { const wchar_t* label; wchar_t glyph; } items[] = {{L"Play", ic::play}, {L"Settings", ic::settings}, {L"Patch notes", ic::notes}};
    float y = 148;
    float ind = anim(ID_NAV + 9, (float)s_page, 18); // the active indicator slides between items
    fill({16, y + ind * 56, kNavW - 32, 52}, col::secondary_container, 26);
    for (int i = 0; i < 3; i++) {
        Rc r = {16, y + i * 56, kNavW - 32, 52};
        bool sel = s_page == i;
        state_layer(r, ID_NAV + i, col::on_surface, 26);
        u32 fg = sel ? col::on_secondary_container : col::on_surface_variant;
        icon(items[i].glyph, {r.x + 16, r.y, 24, r.h}, fg, F_ICON_LARGE);
        text(items[i].label, {r.x + 56, r.y, r.w - 64, r.h}, F_LABEL, sel ? col::on_secondary_container : col::on_surface);
        if (clicked(ID_NAV + i, r) && s_page != i) { s_page = i; s_notes_scroll = 0; close_popup(); request_frame(); }
    }
    text(L"Built " + widen(__DATE__), {28, H - kFooterH - 40, kNavW - 40, 20}, F_BODY_SMALL, col::alpha(col::on_surface_variant, 0.7f));
}

static void title_bar() {
    using namespace ui;
    if (icon_button(ID_MIN, {W - 96, 0, 48, 40}, ic::minimize, col::surface_highest)) ShowWindow(s_hwnd, SW_MINIMIZE);
    if (icon_button(ID_CLOSE, {W - 48, 0, 48, 40}, ic::close, 0xFFC42B1C)) PostMessageW(s_hwnd, WM_CLOSE, 0, 0);
}

static void footer() {
    using namespace ui;
    Rc f = {0, H - kFooterH, W, kFooterH};
    fill(f, col::surface_container);
    line(0, f.y + 0.5f, W, f.y + 0.5f, col::outline_variant);
    u32 dot = s_disc.state == 1 ? col::success : s_disc.state == 0 ? col::tertiary : col::error;
    circle(36, f.y + f.h / 2, 5, dot);
    std::wstring t = s_disc.state == 0 ? L"No disc image yet" : s_disc.title;
    std::wstring d = s_disc.state == 0 ? L"Choose your Team Buddies (USA) .cue to play" : s_disc.detail;
    float bw = 132, cw = text_width(L"Choose disc", F_LABEL) + 18 + 8 + 48;
    text(t, {52, f.y + 18, W - bw - cw - 120, 22}, F_TITLE, col::on_surface);
    text(d, {52, f.y + 40, W - bw - cw - 120, 20}, F_BODY_SMALL, col::on_surface_variant);
    if (button_outlined(ID_CHOOSE, {W - 28 - bw - 12 - cw, f.y + 18, cw, 44}, L"Choose disc", ic::disc)) choose_disc();
    if (button_filled(ID_PLAY, {W - 28 - bw, f.y + 18, bw, 44}, L"Play", ic::play, s_disc.state == 1 || s_disc.state == 2)) play();
}

static void snackbar() {
    using namespace ui;
    if (s_snack.empty() || GetTickCount64() > s_snack_until) return;
    float w = std::min(text_width(s_snack, F_BODY) + 32, W - kNavW - 48);
    Rc r = {kNavW + (W - kNavW - w) / 2, H - kFooterH - 64, w, 48};
    fill({r.x, r.y + 2, r.w, r.h}, 0x40000000, 6);
    fill(r, col::inverse_surface, 6);
    text(s_snack, r.inset(16, 0), F_BODY, col::inverse_on_surface);
}

static void page_header(const wchar_t* title, const wchar_t* sub) {
    using namespace ui;
    text(title, {content_x(), 36, content_w() - 140, 44}, F_HEADLINE, col::on_surface);
    text(sub, {content_x(), 82, content_w() - 40, 40}, F_BODY, col::on_surface_variant, A_LEFT, true);
}

// one settings row: title + supporting text on the left, the control's rect returned on the right
// refresh rate of the monitor the launcher is on (the game follows the one its own window is on)
static int monitor_hz() {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    return 60;
}

static int s_rows = 6; // rows on the settings tab being drawn
static Rc row(int i, const std::wstring& title, const std::wstring& sub, float control_w, bool enabled = true) {
    using namespace ui;
    // 66 DIPs per row; a tab with more rows than fit above the footer packs them closer
    const float pitch = std::min(66.0f, std::floor((H - kFooterH - 196 - 4) / s_rows)), off = std::floor((pitch - 66) / 2);
    float y = 196 + i * pitch;
    float cx = content_x(), cw = content_w();
    u32 a = enabled ? 0 : 1;
    text(title, {cx, y + 12 + off, cw - control_w - 24, 24}, F_TITLE, a ? col::alpha(col::on_surface, 0.38f) : col::on_surface);
    text(sub, {cx, y + 36 + off, cw - control_w - 24, 22}, F_BODY_SMALL, a ? col::alpha(col::on_surface_variant, 0.38f) : col::on_surface_variant);
    if (i > 0) line(cx, y + 0.5f, cx + cw, y + 0.5f, col::outline_variant);
    return {cx + cw - control_w, y + 12 + off, control_w, 48};
}

// ---------------------------------------------------------------- pages
static void card(Rc r, wchar_t glyph, const std::wstring& label, const std::wstring& value, const std::wstring& detail, u32 accent) {
    using namespace ui;
    fill(r, col::surface_container, 16);
    circle(r.x + 40, r.y + 40, 20, col::secondary_container);
    icon(glyph, {r.x + 20, r.y + 20, 40, 40}, col::on_secondary_container, F_ICON_LARGE);
    text(label, {r.x + 20, r.y + 72, r.w - 40, 18}, F_LABEL_SMALL, accent);
    text(value, {r.x + 20, r.y + 92, r.w - 40, 26}, F_TITLE, col::on_surface);
    text(detail, {r.x + 20, r.y + 118, r.w - 40, 20}, F_BODY_SMALL, col::on_surface_variant);
}

static void keycaps(float x, float y, const std::vector<std::wstring>& keys, const std::wstring& action, float colw) {
    using namespace ui;
    float kx = x;
    for (auto& k : keys) {
        float w = std::max(28.0f, text_width(k, F_LABEL_SMALL) + 16);
        stroke({kx, y, w, 26}, col::outline, 6);
        text(k, {kx, y, w, 26}, F_LABEL_SMALL, col::on_surface, A_CENTER);
        kx += w + 4;
    }
    text(action, {kx + 6, y, x + colw - kx - 10, 26}, F_BODY_SMALL, col::on_surface_variant);
}

static void page_play() {
    using namespace ui;
    float cx = content_x(), cw = content_w();
    text(L"Team Buddies", {cx, 30, cw, 56}, F_DISPLAY, col::on_surface);
    text(L"PlayStation, 2000 · Psygnosis · running natively on Windows", {cx, 86, cw, 22}, F_BODY, col::on_surface_variant);
    float gw = (cw - 32) / 3;
    // disc
    std::wstring dv = s_disc.state == 1 ? L"Ready" : s_disc.state == 0 ? L"Not chosen yet" : s_disc.state == 2 ? L"Different game" : L"Missing";
    card({cx, 132, gw, 152}, ic::disc, L"DISC", dv, s_disc.state ? s_disc.detail : L"Use Choose disc below", s_disc.state == 1 ? col::success : col::tertiary);
    // controllers
    std::wstring pv = s_pads.empty() ? L"Keyboard only" : std::to_wstring(s_pads.size()) + (s_pads.size() == 1 ? L" controller" : L" controllers");
    std::wstring pd;
    for (auto& p : s_pads) pd += (pd.empty() ? L"" : L", ") + p;
    card({cx + gw + 16, 132, gw, 152}, ic::game, L"CONTROLLERS", pv, s_pads.empty() ? L"Plug one in any time" : pd, col::primary);
    // saves: the card opens the savedata folder
    std::wstring mv = s_save.count == 0 ? L"No saves yet" : std::to_wstring(s_save.count) + (s_save.count == 1 ? L" save" : L" saves");
    std::wstring md = s_save.count == 0 ? L"savedata\\SLUS-00869" : L"Last saved " + ago(s_save.newest);
    Rc sc = {cx + 2 * (gw + 16), 132, gw, 152};
    card(sc, ic::save, L"SAVES", mv, md, col::primary);
    state_layer(sc, ID_SAVES, col::on_surface, 16);
    icon(ic::folder, {sc.r() - 48, sc.y + 16, 32, 32}, col::on_surface_variant);
    if (clicked(ID_SAVES, sc)) open_saves();

    text(L"Keyboard", {cx, 308, cw, 22}, F_LABEL, col::primary);
    float colw = cw / 3;
    keycaps(cx, 340, {L"←", L"↑", L"→", L"↓"}, L"D-pad", colw);
    keycaps(cx + colw, 340, {L"Z"}, L"Cross", colw);
    keycaps(cx + 2 * colw, 340, {L"X"}, L"Circle", colw);
    keycaps(cx, 374, {L"A"}, L"Square", colw);
    keycaps(cx + colw, 374, {L"S"}, L"Triangle", colw);
    keycaps(cx + 2 * colw, 374, {L"Enter"}, L"Start", colw);
    keycaps(cx, 408, {L"Q", L"W"}, L"L1 / R1", colw);
    keycaps(cx + colw, 408, {L"E", L"R"}, L"L2 / R2", colw);
    keycaps(cx + 2 * colw, 408, {L"Backspace"}, L"Select", colw);

    text(L"In game", {cx, 456, cw, 22}, F_LABEL, col::primary);
    keycaps(cx, 488, {L"F11"}, L"Full screen", colw);
    keycaps(cx + colw, 488, {L"F12"}, L"Screenshot", colw);
    keycaps(cx + 2 * colw, 488, {L"Tab"}, L"Hold to fast-forward", colw);
    keycaps(cx, 522, {L"F2", L"F3"}, L"Resolution", colw);
    keycaps(cx + colw, 522, {L"F6"}, L"Aspect", colw);
    keycaps(cx + 2 * colw, 522, {L"F4"}, L"Widescreen", colw);
}

static const struct { int w, h; } kSizes[] = {{960, 720}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440},
                                              {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};

static void reset_tab() {
    Settings d;
    Settings& s = g_settings;
    switch (s_tab) {
    case 0: s.renderer = d.renderer; s.scale = d.scale; s.anti_aliasing = d.anti_aliasing; s.frame_rate = d.frame_rate; s.true_color = d.true_color; s.texture_filter = d.texture_filter; s.hd_text = d.hd_text; break;
    case 1: s.display = d.display; s.window_w = d.window_w; s.window_h = d.window_h; s.aspect = d.aspect; s.bilinear = d.bilinear; s.widescreen = d.widescreen; s.vsync = d.vsync; break;
    case 2: s.skip_intro = d.skip_intro; s.cpu_cpi = d.cpu_cpi; s.volume = d.volume; break;
    case 3: s.button_icons = d.button_icons; s.vibration = d.vibration; s.multitap = d.multitap; break;
    }
    save();
    static const wchar_t* names[] = {L"Graphics", L"Display", L"Game", L"Controls"};
    snack(std::wstring(names[s_tab]) + L" settings are back to their defaults.");
}

static void page_settings() {
    using namespace ui;
    Settings& s = g_settings;
    page_header(L"Settings", L"Saved to teambuddies.ini as you change them; the game reads them when it starts.");
    if (button_text(ID_RESET, {content_x() + content_w() - 120, 40, 120, 40}, L"Reset tab", ic::reset)) reset_tab();
    // M3 primary tabs
    const wchar_t* tabs[] = {L"Graphics", L"Display", L"Game", L"Controls"};
    float tx = content_x(), ty = 130;
    float xs[5];
    for (int i = 0; i < 4; i++) {
        float w = text_width(tabs[i], F_LABEL) + 32;
        xs[i] = tx;
        Rc r = {tx, ty, w, 48};
        state_layer(r, ID_TABS + i, col::on_surface, 0);
        text(tabs[i], r, F_LABEL, s_tab == i ? col::primary : col::on_surface_variant, A_CENTER);
        if (clicked(ID_TABS + i, r) && s_tab != i) { s_tab = i; close_popup(); request_frame(); }
        tx += w;
    }
    xs[4] = tx;
    line(content_x(), ty + 48.5f, content_x() + content_w(), ty + 48.5f, col::outline_variant);
    float ix = anim(ID_TABS + 8, xs[s_tab], 18), iw = anim(ID_TABS + 9, xs[s_tab + 1] - xs[s_tab], 18);
    fill({ix + 8, ty + 45, iw - 16, 3}, col::primary, 1.5f);

    bool hw = s.renderer == 1;
    s_rows = s_tab == 0 ? 7 : 6;
    switch (s_tab) {
    case 0: {
        std::vector<std::wstring> ren = {L"Hardware", L"Software"};
        int v = hw ? 0 : 1;
        if (segmented(ID_RENDERER, row(0, L"Renderer", L"Hardware upscales on your GPU; Software draws the original picture", segmented_width(ren)), ren, v)) { s.renderer = v == 0; save(); }
        std::vector<std::wstring> sc;
        for (int k = 1; k <= 8; k++) sc.push_back(std::to_wstring(k) + L"x · " + std::to_wstring(320 * k) + L" × " + std::to_wstring(240 * k) + (k == 1 ? L" (original)" : L""));
        int sv = s.scale - 1;
        if (dropdown(ID_SCALE, row(1, L"Resolution", L"How sharp the 3D picture is drawn (F2/F3 in game)", 240, hw), sc, sv, hw)) { s.scale = sv + 1; save(); }
        // anti-aliasing; the game caps the samples so the multisampled VRAM stays within 512 MB
        std::vector<std::wstring> aa = {L"Off", L"FXAA", L"MSAA 2x", L"MSAA 4x", L"MSAA 8x", L"SSAA 2x", L"SSAA 4x", L"SSAA 8x"};
        int aav = std::clamp(s.anti_aliasing, 0, AA_COUNT - 1);
        int cap = 8;
        while (cap > 1 && cap * s.scale * s.scale > 256) cap /= 2;
        std::wstring capped = aa_samples(aav) > cap ? L" (" + std::to_wstring(cap) + L" samples at this resolution)" : L"";
        std::wstring aasub = !hw ? L"Needs the hardware renderer"
                           : aav == AA_OFF ? L"Smooths jagged edges (F1 in game)"
                           : aav == AA_FXAA ? L"A light filter over the picture; text softens a little"
                           : aa_per_sample(aav) ? L"Smooths edges and textures; the heaviest" + capped
                           : L"Smooths polygon edges; text stays sharp" + capped;
        if (dropdown(ID_AA, row(2, L"Anti-aliasing", aasub, 240, hw), aa, aav, hw)) { s.anti_aliasing = aav; save(); }
        // frame rate: the game's own, fixed rates or the monitor's; a rate typed into the ini keeps an entry
        const int mhz = monitor_hz();
        std::vector<int> rv = {0, 60, 120, 144, 165, 240};
        if (s.frame_rate > 0 && std::find(rv.begin(), rv.end(), s.frame_rate) == rv.end()) { rv.push_back(s.frame_rate); std::sort(rv.begin(), rv.end()); }
        rv.push_back(FRAME_RATE_MONITOR);
        std::vector<std::wstring> fr;
        for (int r : rv) fr.push_back(r == 0 ? L"Original" : r == FRAME_RATE_MONITOR ? L"Monitor · " + std::to_wstring(mhz) + L" Hz" : std::to_wstring(r) + L" fps");
        int fv = (int)(std::find(rv.begin(), rv.end(), s.frame_rate) - rv.begin());
        std::wstring fsub = !hw ? L"Above Original needs the hardware renderer"
                          : s.frame_rate == 0 ? L"The game's own rate: 30 fps in battle"
                          : s.vsync && s.frame_rate > mhz ? L"V-Sync holds it to your monitor's " + std::to_wstring(mhz) + L" Hz"
                          : L"Draws in-between frames; the game keeps its speed (F10)";
        if (dropdown(ID_FRAMERATE, row(3, L"Frame rate", fsub, 240, hw), fr, fv, hw)) { s.frame_rate = rv[fv]; save(); }
        std::vector<std::wstring> tf = {L"Off", L"Smooth", L"xBR"};
        int tv = std::clamp(s.texture_filter, 0, TEX_COUNT - 1);
        static const wchar_t* tf_sub[] = {L"The original's sharp texture pixels (F7)", L"Blends texture pixels; text and small sprites stay sharp (F7)",
                                          L"Redraws pixel art with smooth edges; heavy above 4x resolution (F7)"};
        if (segmented(ID_TEXFILTER, row(4, L"Texture filtering", hw ? tf_sub[tv] : L"Needs the hardware renderer", segmented_width(tf), hw), tf, tv, hw)) {
            s.texture_filter = tv;
            save();
        }
        if (toggle(ID_HDTEXT, row(5, L"HD text", hw ? L"Letters and HUD numbers redrawn smooth and sharp at any resolution" : L"Needs the hardware renderer", 52, hw), s.hd_text, hw)) save();
        if (toggle(ID_TRUECOLOR, row(6, L"True color", L"Smooth gradients without the PlayStation's dithering", 52, hw), s.true_color, hw)) save();
        break;
    }
    case 1: {
        std::vector<std::wstring> dm = {L"Window", L"Borderless", L"Fullscreen"};
        if (segmented(ID_DISPLAY, row(0, L"Display", L"F11 or Alt+Enter switches in game", segmented_width(dm)), dm, s.display)) save();
        std::vector<std::wstring> ws;
        int wv = -1;
        for (int k = 0; k < (int)(sizeof kSizes / sizeof kSizes[0]); k++) {
            ws.push_back(std::to_wstring(kSizes[k].w) + L" × " + std::to_wstring(kSizes[k].h) + (kSizes[k].w * 3 == kSizes[k].h * 4 ? L"  (4:3)" : L"  (16:9)"));
            if (kSizes[k].w == s.window_w && kSizes[k].h == s.window_h) wv = k;
        }
        if (wv < 0) { ws.push_back(std::to_wstring(s.window_w) + L" × " + std::to_wstring(s.window_h) + L"  (your size)"); wv = (int)ws.size() - 1; }
        bool win = s.display == DISPLAY_WINDOW;
        if (dropdown(ID_WINSIZE, row(1, L"Window size", win ? L"Size of the game window when it opens" : L"Only for Display: Window", 240, win), ws, wv, win) &&
            wv < (int)(sizeof kSizes / sizeof kSizes[0])) {
            s.window_w = kSizes[wv].w;
            s.window_h = kSizes[wv].h;
            save();
        }
        std::vector<std::wstring> asp = {L"4:3", L"16:9", L"Wide", L"Stretch", L"Pixels"};
        static const wchar_t* asp_sub[] = {L"The original shape (F6 cycles in game)", L"Always 16:9", L"Fills your screen's shape; pair it with Widescreen",
                                           L"Fills the window, whatever its shape", L"Whole-number scaling: every pixel the same size"};
        int av = s.aspect;
        if (segmented(ID_ASPECT, row(2, L"Aspect", asp_sub[std::clamp(s.aspect, 0, 4)], segmented_width(asp)), asp, av)) { s.aspect = av; save(); }
        std::vector<std::wstring> scl = {L"Sharp", L"Smooth"};
        int bv = s.bilinear ? 1 : 0;
        if (segmented(ID_SCALING, row(3, L"Scaling", L"How the picture is enlarged to fit the window (F5)", segmented_width(scl)), scl, bv)) { s.bilinear = bv == 1; save(); }
        if (toggle(ID_WIDESCREEN, row(4, L"Widescreen", L"Widens the 3D view (F4); the HUD stretches and edges can pop in", 52), s.widescreen)) save();
        if (toggle(ID_VSYNC, row(5, L"V-Sync", L"Waits for the monitor, so the picture never tears", 52), s.vsync)) save();
        break;
    }
    case 2: {
        Rc dr = row(0, L"Disc image", g_settings.disc.empty() ? L"Not chosen yet" : widen(g_settings.disc), 128);
        if (button_tonal(ID_DISC_ROW, {dr.x, dr.y + 4, dr.w, 40}, L"Choose", ic::folder)) choose_disc();
        if (toggle(ID_SKIPINTRO, row(1, L"Skip intro movies", L"Starts at the main menu: logos, movies and the title are skipped", 52), s.skip_intro)) save();
        std::vector<std::wstring> cpu = {L"Original", L"Fast", L"Fastest"};
        static const double cpis[] = {2.0, 1.5, 1.0};
        int cv = s.cpu_cpi >= 1.75 ? 0 : s.cpu_cpi >= 1.25 ? 1 : 2;
        if (segmented(ID_CPU, row(2, L"CPU speed", L"Faster removes slowdown in busy fights; Original matches the console", segmented_width(cpu)), cpu, cv)) { s.cpu_cpi = cpis[cv]; save(); }
        Rc vr = row(3, L"Volume", L"Music, speech and sound effects", 280);
        int vol = s.volume;
        if (slider(ID_VOLUME, {vr.x + 10, vr.y, vr.w - 70, vr.h}, vol, 0, 100)) { s.volume = vol; request_frame(); }
        static int saved_volume = -1;
        if (!held(ID_VOLUME) && saved_volume != s.volume) { if (saved_volume >= 0) save(); saved_volume = s.volume; } // one write per drag
        text(std::to_wstring(s.volume) + L"%", {vr.r() - 48, vr.y, 48, vr.h}, F_LABEL, col::on_surface, A_RIGHT);
        break;
    }
    case 3: {
        std::vector<std::wstring> icons = {L"Auto", L"PlayStation", L"Xbox", L"Keyboard"};
        if (segmented(ID_ICONS, row(0, L"Button prompts", L"Auto follows the device you used last", segmented_width(icons)), icons, s.button_icons)) save();
        if (toggle(ID_VIBRATION, row(1, L"Vibration", L"Controller rumble", 52), s.vibration)) save();
        if (toggle(ID_MULTITAP, row(2, L"Multitap", L"Four players on one PC: controllers 1 to 4 (MULTI games)", 52), s.multitap)) save();
        break;
    }
    }
}

static void page_notes() {
    using namespace ui;
    page_header(L"Patch notes", L"What changed in this port, newest first.");
    float cx = content_x(), cw = content_w() - 24;
    Rc view = {cx, 132, content_w(), H - kFooterH - 132 - 8};
    // measure, then draw with the scroll offset
    float total = 0;
    std::vector<float> hs(s_notes.size());
    for (size_t i = 0; i < s_notes.size(); i++) {
        const NoteLine& n = s_notes[i];
        float h = n.kind == 0 ? 52 : n.kind == 1 ? 34 : text_height(n.text, F_BODY, n.kind == 2 ? cw - 20 : cw) + 10;
        hs[i] = h;
        total += h;
    }
    float maxs = std::max(0.0f, total - view.h + 16);
    if (hovered(view)) s_notes_scroll -= in.wheel * 72;
    s_notes_scroll = std::clamp(s_notes_scroll, 0.0f, maxs);
    float sy = anim(ID_NAV + 30, s_notes_scroll, 20);
    push_clip(view);
    float y = view.y - sy;
    for (size_t i = 0; i < s_notes.size(); i++) {
        const NoteLine& n = s_notes[i];
        if (y + hs[i] >= view.y && y <= view.b()) {
            if (n.kind == 0) text(n.text, {cx, y + 14, cw, 32}, F_TITLE_LARGE, col::on_surface);
            else if (n.kind == 1) text(n.text, {cx, y + 8, cw, 22}, F_LABEL, col::primary);
            else if (n.kind == 2) {
                circle(cx + 6, y + 11, 2.5f, col::on_surface_variant);
                text(n.text, {cx + 20, y, cw - 20, hs[i]}, F_BODY, col::on_surface, A_LEFT, true);
            } else text(n.text, {cx, y, cw, hs[i]}, F_BODY, col::on_surface_variant, A_LEFT, true);
        }
        y += hs[i];
    }
    pop_clip();
    if (maxs > 0) { // scroll position
        float th = std::max(32.0f, view.h * view.h / (total + 16));
        float ty = view.y + (view.h - th) * (sy / maxs);
        fill({view.r() - 4, ty, 4, th}, col::alpha(col::on_surface_variant, 0.38f), 2);
    }
}

static void draw() {
    if (!ui::begin_frame()) return;
    nav();
    if (s_page == 0) page_play();
    else if (s_page == 1) page_settings();
    else page_notes();
    title_bar();
    footer();
    ui::popups();
    snackbar();
    ui::end_frame();
}

// ---------------------------------------------------------------- test harness snapshots
// TB_LAUNCHER_SHOTS=<dir>: render every page/tab (and an open menu) to PNGs, then exit
static void shots(const std::wstring& dir) {
    ui::W = kWinW;
    ui::H = kWinH;
    struct S { int page, tab; const wchar_t* name; } list[] = {
        {0, 0, L"play"}, {1, 0, L"settings_graphics"}, {1, 1, L"settings_display"}, {1, 2, L"settings_game"}, {1, 3, L"settings_controls"}, {2, 0, L"patch_notes"}};
    for (auto& s : list) {
        s_page = s.page;
        s_tab = s.tab;
        ui::snapshot(dir + L"\\launcher_" + s.name + L".png", draw, 2);
    }
    // the Graphics menus open: press and release on the field, then draw the menu
    s_page = 1;
    s_tab = 0;
    static int step, menu_id;
    struct M { int id; const wchar_t* name; } menus[] = {{ID_SCALE, L"menu"}, {ID_AA, L"menu_aa"}, {ID_FRAMERATE, L"menu_framerate"}};
    for (auto& m : menus) {
        ui::close_popup();
        step = 0;
        menu_id = m.id;
        ui::snapshot(dir + L"\\launcher_" + m.name + L".png", [] {
            Rc r = ui::last_rect(menu_id);
            ui::in.mx = r.x + r.w / 2;
            ui::in.my = r.y + r.h / 2;
            ui::in.pressed = step == 1;
            ui::in.down = step == 1;
            ui::in.released = step == 2;
            step++;
            draw();
        }, 4);
    }
}

// ---------------------------------------------------------------- window
static void track_leave() {
    TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, s_hwnd, 0};
    TrackMouseEvent(&t);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float scale = 96.0f / (float)std::max<UINT>(96, GetDpiForWindow(h));
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) return 0; // the whole window is client area; DWM keeps the shadow and rounded corners
        break;
    case WM_NCHITTEST: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        ScreenToClient(h, &p);
        float x = p.x * scale, y = p.y * scale;
        if (y < kTitleH && x < ui::W - 100) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        draw();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: ui::on_resize(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
        ui::set_dpi((float)HIWORD(w));
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSEMOVE:
        ui::in.mx = (short)LOWORD(l) * scale;
        ui::in.my = (short)HIWORD(l) * scale;
        track_leave();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSELEAVE: ui::in.mx = ui::in.my = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        ui::in.mx = (short)LOWORD(l) * scale;
        ui::in.my = (short)HIWORD(l) * scale;
        ui::in.down = ui::in.pressed = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        ui::in.mx = (short)LOWORD(l) * scale;
        ui::in.my = (short)HIWORD(l) * scale;
        ui::in.down = false;
        ui::in.released = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: ui::in.wheel += GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) ui::in.escape = true;
        if (w == VK_RETURN && !ui::popup_open() && (s_disc.state == 1 || s_disc.state == 2)) play();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (w == 1) { check_pads(); check_saves(); }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) { check_disc(); InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_GAME_EXITED: game_exited((DWORD)w); return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const wchar_t* shot_dir = _wgetenv(L"TB_LAUNCHER_SHOTS");
    // one launcher at a time: a second start brings the first one forward
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TeamBuddiesLauncher");
    if (!shot_dir && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kClass, nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    s_dir = dir_of(exe);
    std::string dir_a;
    narrow_acp(s_dir, dir_a);
    s_ini = getenv("TB_INI") ? getenv("TB_INI") : dir_a + "teambuddies.ini";
    const wchar_t* cd = _wgetenv(L"TB_CARD_DIR");
    s_saves = (cd ? std::wstring(cd) : s_dir + L"savedata") + L"\\SLUS-00869";
    s_legacy = cd ? cd : s_dir + L"memcards";
    settings_load(s_ini);
    load_notes();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    // centred on the primary monitor at its DPI
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dpiy = 96;
    typedef HRESULT(WINAPI * GetDpiForMonitor_t)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll"))
        if (auto f = (GetDpiForMonitor_t)GetProcAddress(sh, "GetDpiForMonitor")) f(mon, 0, &dpi, &dpiy);
    int ww = (int)(kWinW * dpi / 96), wh = (int)(kWinH * dpi / 96);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
    s_hwnd = CreateWindowExW(0, kClass, L"Team Buddies", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!s_hwnd) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(s_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    int corners = 2; // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(s_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof corners);
    COLORREF border = RGB(0x45, 0x46, 0x4F);
    DwmSetWindowAttribute(s_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
    SetWindowPos(s_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (!ui::init(s_hwnd)) {
        MessageBoxW(nullptr, L"Direct2D is not available on this PC.", L"Team Buddies", MB_ICONERROR);
        return 1;
    }
    refresh_all();
    if (shot_dir) {
        shots(shot_dir);
        ui::shutdown();
        return 0;
    }
    ShowWindow(s_hwnd, show);
    SetTimer(s_hwnd, 1, 1500, nullptr); // controllers and memory cards
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ui::shutdown();
    SDL_Quit();
    if (mutex) CloseHandle(mutex);
    return 0;
}
