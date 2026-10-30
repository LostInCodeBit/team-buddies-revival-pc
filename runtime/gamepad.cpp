// gamepad.cpp - controllers through SDL3: Xbox/XInput, DualShock 4, DualSense, Switch Pro and other
// SDL-known pads, with hot-plugging, rumble and the controller family (for the button prompts).
#include <SDL3/SDL.h>
#include "psx.h"

static bool s_ok = false;
static std::vector<SDL_Gamepad*> s_pads; // in connection order = player order

bool gamepad_init() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // same as XInput: read even without focus
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        plog("gamepad: SDL3 init failed (%s), keyboard only\n", SDL_GetError());
        return false;
    }
    s_ok = true;
    return true;
}

void gamepad_shutdown() {
    for (SDL_Gamepad* g : s_pads) SDL_CloseGamepad(g);
    s_pads.clear();
    if (s_ok) SDL_Quit();
    s_ok = false;
}

static InputDevice family_of(SDL_Gamepad* g) {
    switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return DEV_PLAYSTATION;
    default: return DEV_XBOX; // Xbox and everything else: positional A/B/X/Y glyphs
    }
}

static void handle_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
            if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) {
                s_pads.push_back(g);
                plog("gamepad: connected %s (player %zu)\n", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "?", s_pads.size());
            }
        } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
            for (size_t i = 0; i < s_pads.size(); i++)
                if (SDL_GetGamepadID(s_pads[i]) == e.gdevice.which) {
                    plog("gamepad: player %zu disconnected\n", i + 1);
                    SDL_CloseGamepad(s_pads[i]);
                    s_pads.erase(s_pads.begin() + i);
                    break;
                }
        }
    }
}

// Fills g_pads[first..] from the connected controllers; returns the family of a controller that was
// used this frame (new button press or stick push), or -1.
int gamepad_poll(int first) {
    if (!s_ok) return -1;
    handle_events();
    int used = -1;
    static u16 prev[8];
    static u8 rumble[8][2];
    static Uint64 rumble_at[8];
    for (size_t i = 0; i < s_pads.size() && first + (int)i < 4; i++) {
        SDL_Gamepad* g = s_pads[i];
        PadState& p = g_pads[first + i];
        static const struct { SDL_GamepadButton s; u16 b; } map[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT}, {SDL_GAMEPAD_BUTTON_START, PAD_START},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_R3},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
        };
        u16 b = 0;
        for (auto& m : map) if (SDL_GetGamepadButton(g, m.s)) b |= m.b;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= PAD_L2;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= PAD_R2;
        int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        int rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
        const int dz = 12000; // the game reads the d-pad in menus: the left stick doubles as one
        if (lx < -dz) b |= PAD_LEFT;
        if (lx > dz) b |= PAD_RIGHT;
        if (ly < -dz) b |= PAD_UP;
        if (ly > dz) b |= PAD_DOWN;
        if ((b & ~prev[i]) != 0) used = family_of(g);
        prev[i] = b;
        p.connected = true;
        p.buttons |= b;
        p.lx = (u8)((lx >> 8) + 128);
        p.ly = (u8)((ly >> 8) + 128);
        p.rx = (u8)((rx >> 8) + 128);
        p.ry = (u8)((ry >> 8) + 128);
        // DualShock rumble -> controller motors (big = low frequency); refreshed before SDL's duration runs out
        // online, this machine's first controller belongs to its own player slot
        const PadState& mp = (first + i == 0) ? g_pads[netplay_local_slot()] : p;
        const u8 big = g_settings.vibration ? mp.motor_big : 0, small = g_settings.vibration ? mp.motor_small : 0;
        Uint64 now = SDL_GetTicks();
        if (big != rumble[i][0] || small != rumble[i][1] || ((big || small) && now - rumble_at[i] > 400)) {
            SDL_RumbleGamepad(g, (Uint16)(big * 257), (Uint16)(small * 257), 1000);
            rumble[i][0] = big;
            rumble[i][1] = small;
            rumble_at[i] = now;
        }
    }
    return used;
}

// family of the first connected controller, -1 = none (initial button prompts)
int gamepad_first_family() {
    if (!s_ok) return -1;
    handle_events();
    return s_pads.empty() ? -1 : family_of(s_pads[0]);
}
