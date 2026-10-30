// sio.cpp - serial port 0: DualShock controllers (digital/analog, config mode, rumble).
// Memory cards are handled by the HLE kernel, so card addresses get no answer here.
#include "psx.h"

static u16 s_ctrl, s_mode, s_baud;
static u32 s_stat = 0x5;          // TX ready 1 + TX finished
static u8 s_rx;
static bool s_rx_full;
static bool s_ack_pending;

struct DualShock {
    bool config = false;
    bool analog = false;
    bool locked = false;
    u8 map[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // rumble mapping set with command 4Dh
};
static DualShock s_ds[2];

static bool s_active;             // a controller is answering the current packet
static int s_pos;                 // byte index within the packet (0 = address)
static u8 s_cmd;
static u8 s_data[8];              // response bytes after 0x5A
static u8 s_param[8];             // bytes received after 0x5A
static int s_len;                 // number of data bytes

static int slot() { return (s_ctrl >> 13) & 1; }
static bool selected() { return (s_ctrl & 2) != 0; }
static int player() { return slot(); }

static void buttons(int pl, u8* out) {
    u16 b = (u16)~g_pads[pl].buttons;
    out[0] = b & 0xFF;
    out[1] = b >> 8;
    out[2] = g_pads[pl].rx;
    out[3] = g_pads[pl].ry;
    out[4] = g_pads[pl].lx;
    out[5] = g_pads[pl].ly;
}

// response to the command byte; returns the id byte
static u8 begin_command(u8 cmd) {
    DualShock& d = s_ds[slot()];
    s_cmd = cmd;
    memset(s_data, 0, sizeof s_data);
    memset(s_param, 0, sizeof s_param);
    if (!d.config) {
        // normal mode: every command reads the buttons (43h may switch to config mode afterwards)
        buttons(player(), s_data);
        s_len = d.analog ? 6 : 2;
        return d.analog ? 0x73 : 0x41;
    }
    s_len = 6;
    switch (cmd) {
    case 0x42: buttons(player(), s_data); break;
    case 0x45: s_data[0] = 0x01; s_data[1] = 0x02; s_data[2] = d.analog ? 1 : 0; s_data[3] = 0x02; s_data[4] = 0x01; s_data[5] = 0x00; break;
    case 0x47: s_data[2] = 0x02; s_data[4] = 0x01; break;
    case 0x4D: memcpy(s_data, d.map, 6); break;
    default: break; // 43h, 44h, 46h, 4Ch: zeros until the parameter arrives
    }
    return 0xF3;
}

static void param_received(int i, u8 v) {
    s_param[i] = v;
    DualShock& d = s_ds[slot()];
    if (!d.config || i != 0) return;
    if (s_cmd == 0x46) {
        static const u8 r0[4] = {0x01, 0x02, 0x00, 0x0A}, r1[4] = {0x01, 0x01, 0x01, 0x14};
        memcpy(s_data + 2, v ? r1 : r0, 4);
    } else if (s_cmd == 0x4C) {
        s_data[3] = v ? 0x07 : 0x04;
    }
}

static void end_packet() {
    DualShock& d = s_ds[slot()];
    int pl = player();
    switch (s_cmd) {
    case 0x42:
        if (!d.config) {
            u8 small = 0, big = 0;
            for (int i = 0; i < 6; i++) {
                if (d.map[i] == 0x00) small = s_param[i] ? 0xFF : 0;
                if (d.map[i] == 0x01) big = s_param[i];
            }
            g_pads[pl].motor_small = small;
            g_pads[pl].motor_big = big;
        }
        break;
    case 0x43: d.config = s_param[0] == 0x01; break;
    case 0x44:
        if (d.config) { d.analog = s_param[0] == 0x01; d.locked = s_param[1] == 0x03; }
        break;
    case 0x4D:
        if (d.config) memcpy(d.map, s_param, 6);
        break;
    }
}

static void ev_sio(int) {
    // byte transfer finished: data is in RX, ACK follows shortly when the device answers
    s_rx_full = true;
    s_stat |= 0x2 | 0x4;
    if (s_ack_pending) {
        s_ack_pending = false;
        s_stat |= 0x80 | 0x200; // /ACK low + interrupt request
        if (s_ctrl & 0x1000) irq_raise(IRQ_SIO0);
    }
}

static void transfer(u8 tx) {
    u8 rx = 0xFF;
    bool ack = false;
    if (!selected()) {
        s_active = false;
        s_pos = 0;
    } else if (s_pos == 0) {
        // address byte: 01h = controller, 81h = memory card (not answered here)
        bool present = g_pads[player()].connected;
        if (g_settings.log_level >= 2 && tx == 0x01) {
            static bool seen[2][2];
            if (!seen[slot()][present]) { seen[slot()][present] = true; plog("sio: port %d polled, controller %s\n", slot() + 1, present ? "present" : "absent"); }
        }
        s_active = tx == 0x01 && present;
        ack = s_active;
        s_pos = 1;
    } else if (s_active) {
        if (s_pos == 1) {
            rx = begin_command(tx);
            ack = true;
        } else if (s_pos == 2) {
            rx = 0x5A; // (the byte sent here would select a multitap slot)
            ack = true;
        } else {
            int i = s_pos - 3;
            if (i < 8) param_received(i, tx);
            rx = i < s_len ? s_data[i] : 0xFF;
            ack = i + 1 < s_len;
            if (!ack) { end_packet(); s_active = false; }
        }
        s_pos++;
    }
    if (g_settings.log_level >= 4)
        plog("sio tx %02X rx %02X ack %d pos %d ctrl %04X\n", tx, rx, ack, s_pos, s_ctrl);
    s_rx = rx;
    s_ack_pending = ack;
    s_stat &= ~0x6u;
    s_rx_full = false;
    // ~8 bits at the programmed baud rate, then the ACK pulse
    u64 cyc = (u64)(s_baud ? s_baud : 0x88) * 8 + (ack ? 120 : 0);
    sched_set(EV_SIO, cyc, ev_sio);
}

static u32 sio_read_impl(u32 off) {
    switch (off) {
    case 0x0: {
        u8 v = s_rx_full ? s_rx : 0xFF;
        s_rx_full = false;
        s_stat &= ~0x2u;
        return v | (v << 8) | (v << 16) | ((u32)v << 24);
    }
    case 0x4: {
        u32 v = s_stat | 0x1;
        if (s_rx_full) v |= 0x2;
        s_stat &= ~0x80u; // the ACK input goes high again after it was seen
        return v;
    }
    case 0x8: return s_mode;
    case 0xA: return s_ctrl;
    case 0xE: return s_baud;
    }
    return 0;
}

u32 sio_read(u32 off) {
    u32 v = sio_read_impl(off);
    if (g_settings.log_level >= 5) plog("sio r%X = %X\n", off, v);
    return v;
}

void sio_write(u32 off, u32 v) {
    if (g_settings.log_level >= 5) plog("sio w%X = %X\n", off, v);
    switch (off) {
    case 0x0: transfer((u8)v); break;
    case 0x8: s_mode = (u16)v; break;
    case 0xA:
        if (v & 0x40) { // reset
            s_ctrl = 0;
            s_stat = 0x5;
            s_rx_full = false;
            s_active = false;
            s_pos = 0;
            sched_cancel(EV_SIO);
            return;
        }
        if (v & 0x10) s_stat &= ~0x208u; // acknowledge IRQ and errors
        if (!(v & 2) || ((v ^ s_ctrl) & 0x2000)) { s_active = false; s_pos = 0; }
        s_ctrl = (u16)(v & ~0x50u);
        break;
    case 0xE: s_baud = (u16)v; break;
    }
}
