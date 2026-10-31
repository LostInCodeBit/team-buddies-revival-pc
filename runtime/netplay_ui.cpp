// netplay_ui.cpp - the small native windows of online play: the "Online" dialog (F9) that restarts
// the game as host or client, and the status popup shown while the two games connect.
#define NOMINMAX
#include <windows.h>
#include <string>
#include "psx.h"
// modern control look (Common Controls 6) for the dialogs
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static HWND game_hwnd() { return (HWND)platform_hwnd(); }
void platform_request_quit();

static HFONT ui_font() {
    static HFONT f = nullptr;
    if (!f) {
        NONCLIENTMETRICSA m = {sizeof m};
        SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0);
        f = CreateFontIndirectA(&m.lfMessageFont);
    }
    return f;
}

static HWND child(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(cls[0] == 'E' ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)ui_font(), TRUE);
    return c;
}

static void center_on_game(HWND w, int cw, int ch) {
    RECT r;
    HWND g = game_hwnd();
    if (!g || !GetWindowRect(g, &r)) SystemParametersInfoA(SPI_GETWORKAREA, 0, &r, 0);
    SetWindowPos(w, HWND_TOPMOST, (r.left + r.right - cw) / 2, (r.top + r.bottom - ch) / 2, cw, ch, SWP_SHOWWINDOW);
}

// ---------------------------------------------------------------- status popup while connecting
static HWND s_status, s_status_text;
static bool s_cancelled;

static LRESULT CALLBACK status_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_COMMAND && LOWORD(w) == IDCANCEL) || m == WM_CLOSE) { s_cancelled = true; return 0; }
    return DefWindowProcA(h, m, w, l);
}

void np_status_open() {
    static bool reg = false;
    if (!reg) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = status_proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "TBOnlineStatus";
        RegisterClassA(&wc);
        reg = true;
    }
    s_cancelled = false;
    s_status = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "TBOnlineStatus", "Team Buddies online", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               0, 0, 460, 150, game_hwnd(), nullptr, GetModuleHandle(nullptr), nullptr);
    if (!s_status) { plog("netplay: status window failed (%lu)\n", GetLastError()); return; }
    s_status_text = child(s_status, "STATIC", "", SS_LEFT, 16, 16, 420, 60, 0);
    child(s_status, "BUTTON", "Cancel (play offline)", BS_PUSHBUTTON, 266, 80, 170, 28, IDCANCEL);
    center_on_game(s_status, 460, 150);
}

void np_status_text(const char* t) { if (s_status_text) SetWindowTextA(s_status_text, t); }
bool np_status_cancelled() { return s_cancelled; }
void np_status_close() {
    if (s_status) DestroyWindow(s_status);
    s_status = s_status_text = nullptr;
}

void np_message(const char* text, bool warning) {
    MessageBoxA(nullptr, text, "Team Buddies online", MB_OK | (warning ? MB_ICONWARNING : MB_ICONINFORMATION));
}

// ---------------------------------------------------------------- F9: Online dialog
enum { ID_HOST = 101, ID_JOIN, ID_ADDR, ID_PORT, ID_START };
static HWND s_dlg;
static bool s_dlg_done, s_dlg_start;

static LRESULT CALLBACK dialog_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_HOST: case ID_JOIN:
            EnableWindow(GetDlgItem(h, ID_ADDR), IsDlgButtonChecked(h, ID_JOIN) == BST_CHECKED);
            return 0;
        case ID_START: case IDOK: s_dlg_start = true; s_dlg_done = true; return 0;
        case IDCANCEL: s_dlg_done = true; return 0;
        }
        break;
    case WM_CLOSE: s_dlg_done = true; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

void netplay_dialog() {
    if (netplay_active()) {
        if (MessageBoxA(game_hwnd(), "You are playing online. Leave the session and continue offline?", "Team Buddies online",
                        MB_YESNO | MB_ICONQUESTION) == IDYES)
            netplay_shutdown();
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = dialog_proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "TBOnlineDialog";
        RegisterClassA(&wc);
        reg = true;
    }
    const int W = 470, H = 300;
    s_dlg = CreateWindowExA(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, "TBOnlineDialog", "Play online", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            0, 0, W, H, game_hwnd(), nullptr, GetModuleHandle(nullptr), nullptr);
    child(s_dlg, "BUTTON", "Host a game (you are player 1)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 16, 14, 420, 22, ID_HOST);
    child(s_dlg, "BUTTON", "Join a game (you are player 2)", BS_AUTORADIOBUTTON | WS_TABSTOP, 16, 40, 420, 22, ID_JOIN);
    child(s_dlg, "STATIC", "Host address:", SS_LEFT, 36, 74, 100, 20, 0);
    child(s_dlg, "EDIT", g_settings.online_address.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 140, 70, 296, 24, ID_ADDR);
    child(s_dlg, "STATIC", "Port (UDP):", SS_LEFT, 36, 106, 100, 20, 0);
    child(s_dlg, "EDIT", std::to_string(g_settings.online_port).c_str(), ES_NUMBER | WS_TABSTOP, 140, 102, 80, 24, ID_PORT);
    child(s_dlg, "STATIC",
          "The game restarts and connects. Both players need the same disc image and the same version of "
          "this port. The host's router port is opened automatically when it supports UPnP; otherwise forward "
          "the UDP port to the host's PC.",
          SS_LEFT, 16, 136, 420, 70, 0);
    child(s_dlg, "BUTTON", "Start", BS_DEFPUSHBUTTON | WS_TABSTOP, 236, 216, 96, 30, ID_START);
    child(s_dlg, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 340, 216, 96, 30, IDCANCEL);
    bool join = !g_settings.online_address.empty();
    CheckRadioButton(s_dlg, ID_HOST, ID_JOIN, join ? ID_JOIN : ID_HOST);
    EnableWindow(GetDlgItem(s_dlg, ID_ADDR), join);
    center_on_game(s_dlg, W, H);
    SetFocus(GetDlgItem(s_dlg, join ? ID_ADDR : ID_HOST));

    // modal loop (the game is paused meanwhile)
    s_dlg_done = s_dlg_start = false;
    EnableWindow(game_hwnd(), FALSE);
    MSG msg;
    while (!s_dlg_done && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageA(s_dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    char addr[256] = {}, port[16] = {};
    GetWindowTextA(GetDlgItem(s_dlg, ID_ADDR), addr, sizeof addr);
    GetWindowTextA(GetDlgItem(s_dlg, ID_PORT), port, sizeof port);
    bool host = IsDlgButtonChecked(s_dlg, ID_HOST) == BST_CHECKED;
    EnableWindow(game_hwnd(), TRUE);
    DestroyWindow(s_dlg);
    s_dlg = nullptr;
    SetForegroundWindow(game_hwnd());
    if (!s_dlg_start) return;

    int p = atoi(port);
    if (p < 1 || p > 65535) p = 7845;
    std::string a = addr;
    while (!a.empty() && a.back() == ' ') a.pop_back();
    while (!a.empty() && a.front() == ' ') a.erase(a.begin());
    if (!host && a.empty()) { MessageBoxA(game_hwnd(), "Enter the host's address.", "Play online", MB_OK | MB_ICONWARNING); return; }
    g_settings.online_port = p;
    if (!host) g_settings.online_address = a;
    // only plain host names / IPs reach the command line
    for (char c : a) if (!isalnum((u8)c) && c != '.' && c != '-' && c != ':' && c != '[' && c != ']') {
        MessageBoxA(game_hwnd(), "The address contains invalid characters.", "Play online", MB_OK | MB_ICONWARNING);
        return;
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" " + (host ? "--host " + std::to_string(p) : "--join " + a + ":" + std::to_string(p));
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_base_dir.c_str(), &si, &pi)) {
        MessageBoxA(game_hwnd(), "Could not restart the game.", "Play online", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    platform_request_quit(); // this copy saves its settings and closes
}
