// netplay_ui_sdl.cpp - online play's dialogs in the SDL build (netplay_ui.cpp is Windows'): F9 asks to host or
// join and starts the game again into the session; message boxes; while the two games connect, the window title
// shows how far they are and Esc gives up (the game then runs offline).
#include "sdl_platform.h"
#include "../platform_common.h"
#include <cctype>
#include <cerrno>
#include <initializer_list>
#include <string>
#include <vector>
#ifndef _WIN32
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

static bool s_status_open;

void np_status_open() { s_status_open = true; }
void np_status_text(const char* t) {
    if (s_status_open) platform_set_title((std::string("Team Buddies - ") + t + "  [Esc: play offline]").c_str());
}
bool np_status_cancelled() {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    return plat_focused() && keys && keys[SDL_SCANCODE_ESCAPE];
}
void np_status_close() { s_status_open = false; }

void np_message(const char* text, bool warning) {
    if (hidden_run()) return; // the log has it
    SDL_ShowSimpleMessageBox(warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, "Team Buddies online", text, nullptr);
}

static int choose(const char* title, const std::string& text, std::initializer_list<const char*> buttons) {
    std::vector<SDL_MessageBoxButtonData> b;
    int id = 0;
    for (const char* label : buttons) {
        SDL_MessageBoxButtonData d = {0, id++, label};
        if (d.buttonID == 0) d.flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if (d.buttonID == (int)buttons.size() - 1) d.flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        b.push_back(d);
    }
    const SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, sdl_window(), title, text.c_str(), (int)b.size(), b.data(), nullptr};
    int choice = -1;
    return SDL_ShowMessageBox(&d, &choice) ? choice : -1;
}

enum Ask { ASK_OK, ASK_CANCELLED, ASK_NO_TOOL };

#ifndef _WIN32
// a line of text from the desktop's dialog tool (zenity, else kdialog); the game's window keeps answering its
// compositor meanwhile
static Ask ask_text(const char* title, const char* prompt, std::string& text) {
    const std::vector<std::vector<std::string>> tools = {
        {"zenity", "--entry", std::string("--title=") + title, std::string("--text=") + prompt, "--entry-text=" + text},
        {"kdialog", "--title", title, "--inputbox", prompt, text},
    };
    for (const std::vector<std::string>& args : tools) {
        int fd[2];
        if (pipe(fd) != 0) return ASK_NO_TOOL;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, fd[0]);
        posix_spawn_file_actions_addclose(&fa, fd[1]);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        pid_t pid;
        const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(fd[1]);
        if (rc != 0) { close(fd[0]); continue; } // not installed
        std::string out;
        for (;;) {
            pollfd p = {fd[0], POLLIN, 0};
            const int r = poll(&p, 1, 100);
            if (r == 0 || (r < 0 && errno == EINTR)) { SDL_PumpEvents(); continue; }
            char buf[256];
            const ssize_t n = read(fd[0], buf, sizeof buf);
            if (n <= 0) break;
            if (out.size() < 1024) out.append(buf, (size_t)n);
        }
        close(fd[0]);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) continue; // could not run (some C libraries say so)
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return ASK_CANCELLED;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        text = out;
        return ASK_OK;
    }
    return ASK_NO_TOOL;
}

// this game again with these arguments (this copy then quits)
static bool relaunch(const std::vector<std::string>& args) {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    std::vector<char*> argv = {exe};
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    return posix_spawn(&pid, exe, nullptr, nullptr, argv.data(), environ) == 0;
}
#else
// the SDL build on Windows is a test build: online play there starts from the command line
static Ask ask_text(const char*, const char*, std::string&) { return ASK_NO_TOOL; }
static bool relaunch(const std::vector<std::string>&) { return false; }
#endif

void netplay_dialog() {
    if (hidden_run()) return;
    if (netplay_active()) {
        if (choose("Team Buddies online", "You are playing online. Leave the session and continue offline?", {"Leave", "Keep playing"}) == 0)
            netplay_shutdown();
        return;
    }
    const std::string port = std::to_string(g_settings.online_port);
    const int c = choose("Play online",
                         "Host a game (you are player 1) or join one (player 2): the game restarts and connects. Both players "
                         "need the same disc image and the same version of this port. The host's router opens UDP port " +
                             port + " by itself when it supports UPnP; otherwise forward that port to the host's computer.",
                         {"Host", "Join...", "Cancel"});
    std::vector<std::string> args;
    if (c == 0) {
        args = {"--host", port};
    } else if (c == 1) {
        std::string a = g_settings.online_address;
        const Ask r = ask_text("Join a game", "Host address (name or IP, optionally :port):", a);
        if (r == ASK_CANCELLED) return;
        if (r == ASK_NO_TOOL) {
            if (a.empty()) {
                np_message("No text entry dialog was found (zenity or kdialog). Start the game with --join <address>[:port], "
                           "or put the address in TeamBuddies.ini as online_address.", false);
                return;
            }
            if (choose("Play online", "Join " + a + "?", {"Join", "Cancel"}) != 0) return;
        }
        while (!a.empty() && a.back() == ' ') a.pop_back();
        while (!a.empty() && a.front() == ' ') a.erase(a.begin());
        if (a.empty()) { np_message("Enter the host's address.", true); return; }
        // only plain host names / IPs reach the command line
        for (char ch : a)
            if (!isalnum((u8)ch) && ch != '.' && ch != '-' && ch != ':' && ch != '[' && ch != ']') {
                np_message("The address contains invalid characters.", true);
                return;
            }
        g_settings.online_address = a;
        args = {"--join", a.find(':') == std::string::npos ? a + ":" + port : a};
    } else {
        return;
    }
    if (!relaunch(args)) {
        np_message("Could not restart the game. Start it with --host [port] or --join <address>[:port].", true);
        return;
    }
    platform_request_quit(); // this copy saves its settings and closes
}
