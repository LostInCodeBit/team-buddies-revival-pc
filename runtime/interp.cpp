// interp.cpp - frame interpolation (frame_rate = 60). In gameplay the game draws a new frame every second
// vblank and its logic takes one fixed step per frame, so running it faster would speed the game up.
// Instead each frame's primitives are recorded (between two buffer flips), the newest frame's primitives are
// matched to the previous frame's, and on the vblank in between the newest frame is redrawn with every matched
// primitive halfway between its two positions. Presentation only: emulated state is never touched, so it is
// also safe online. Cost: the newest frame reaches the screen one vblank (16.7 ms) later.
#include "psx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

bool g_interp_on = false;
u32 g_gpu_packet = 0;
extern u64 g_dbg_frames; // sched.cpp

namespace {
enum Kind : u8 { K_POLY, K_RECT, K_LINE, K_FILL };

struct Prim {
    Kind kind;
    u8 n;                 // vertices: polygon 3 or 4, line 2, rectangle and fill 1 (plus w, h)
    bool fx, fy;          // rectangle texture flips
    int flags;
    s32 w, h;             // rectangle / fill size
    u32 color;            // rectangle / fill colour
    GpuDrawState st;
    Vertex v[4];
    u64 order;            // the order the game built it: packet address, then position in the stream
    u64 key;              // what it is: texture page, palette, texture coordinates, viewport...
    float cx, cy;         // centre, relative to the frame's display origin
    float bw, bh;         // bounding box size
    int match;            // the same primitive in the previous frame, -1 = none
    bool inherit;         // unmatched, but moves with a neighbour of the same object by (ix, iy)
    u8 how;               // how it was matched: 0 not, 1 anchor, 2 build order, 3 nearest, 4 moves with a neighbour
    float ix, iy;
    float dx[4], dy[4];   // per vertex: where it was in the previous frame, relative (in-between frames move it back
                          // by that times 1 - t)
    bool on_screen;       // draws into the displayed buffer
};

struct Frame {
    std::vector<Prim> prims;
    int ox = 0, oy = 0, dw = 640, dh = 240; // display rectangle this frame was shown in
    long long c = 0;                        // vblank from which the original game shows it
    float matched = 0;                      // share of its on-screen primitives found in the frame before it
};

const size_t MAX_PRIMS = 60000;  // per frame; a runaway frame just stops being recorded
const float MAX_MOVE = 64;       // native pixels per game frame; anything faster is a cut, not motion
const int CELL = 32;             // motion field resolution (native pixels)

// recording, finished at the last flip, newest shown, the one before, and the one before that (pictures
// shown a refresh late, with vsync, can still fall between the older two)
Frame s_rec, s_done, s_cur, s_prev, s_prev2;
bool s_flip_pending, s_have_cur, s_have_prev, s_have_prev2;
long long s_vcount;  // vblanks seen while recording
int s_period = 1;    // vblanks the newest frame took
float s_matched;     // share of the newest frame's on-screen primitives found in the previous frame
int s_stat_presents, s_stat_shown, s_stat_frames;
double s_stat_matched, s_stat_match_ms;

u64 fnv(u64 h, u64 v) {
    for (int i = 0; i < 8; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 0x100000001B3ull; }
    return h;
}

void reset() {
    s_rec.prims.clear();
    s_done.prims.clear();
    s_cur.prims.clear();
    s_prev.prims.clear();
    s_prev2.prims.clear();
    s_flip_pending = s_have_cur = s_have_prev = s_have_prev2 = false;
    s_vcount = 0;
    s_period = 1;
    s_matched = 0;
}

Prim* push(Kind k) {
    if (s_rec.prims.size() >= MAX_PRIMS) return nullptr;
    s_rec.prims.emplace_back();
    Prim& p = s_rec.prims.back();
    p.kind = k;
    p.n = 1;
    p.fx = p.fy = false;
    p.flags = 0;
    p.w = p.h = 0;
    p.color = 0;
    p.match = -1;
    p.inherit = false;
    p.how = 0;
    for (int k = 0; k < 4; k++) p.dx[k] = p.dy[k] = 0;
    p.order = (u64)g_gpu_packet << 24 | (s_rec.prims.size() - 1);
    return &p;
}

// keys and positions, once the frame's display rectangle is known
void finalize(Frame& f) {
    for (Prim& p : f.prims) {
        float x0, y0, x1, y1;
        if (p.kind == K_RECT || p.kind == K_FILL) {
            x0 = (float)p.v[0].x; y0 = (float)p.v[0].y; x1 = x0 + p.w; y1 = y0 + p.h;
        } else {
            x0 = y0 = 1e9f; x1 = y1 = -1e9f;
            for (int k = 0; k < p.n; k++) {
                x0 = std::min(x0, (float)p.v[k].x); x1 = std::max(x1, (float)p.v[k].x);
                y0 = std::min(y0, (float)p.v[k].y); y1 = std::max(y1, (float)p.v[k].y);
            }
        }
        p.cx = (x0 + x1) / 2 - f.ox;
        p.cy = (y0 + y1) / 2 - f.oy;
        p.bw = x1 - x0;
        p.bh = y1 - y0;
        int c1x = p.st.clip_x1, c1y = p.st.clip_y1, c2x = p.st.clip_x2, c2y = p.st.clip_y2;
        if (p.kind == K_FILL) { c1x = p.v[0].x; c1y = p.v[0].y; c2x = c1x + p.w - 1; c2y = c1y + p.h - 1; }
        p.on_screen = c2x >= f.ox && c1x < f.ox + f.dw && c2y >= f.oy && c1y < f.oy + f.dh;
        u64 h = 0xCBF29CE484222325ull;
        h = fnv(h, (u64)p.kind | (u64)p.n << 8 | (u64)(p.flags & 15) << 16 | (u64)((p.fx ? 1 : 0) | (p.fy ? 2 : 0)) << 24);
        if (p.flags & PF_TEXTURED) {
            h = fnv(h, (u64)p.st.texpage | (u64)p.st.clut << 16 | (u64)p.st.tex_mask_x << 32 | (u64)p.st.tex_mask_y << 40 |
                           (u64)p.st.tex_off_x << 48 | (u64)p.st.tex_off_y << 56);
            for (int k = 0; k < p.n; k++) h = fnv(h, (u64)p.v[k].u | (u64)p.v[k].v << 8);
        } else if (p.flags & PF_SEMI) {
            h = fnv(h, (p.st.texpage >> 5) & 3); // blend mode
        }
        if (p.kind == K_RECT || p.kind == K_FILL) h = fnv(h, (u64)(u32)p.w | (u64)(u32)p.h << 32);
        if (!(p.flags & PF_TEXTURED) && p.kind != K_POLY) h = fnv(h, p.color);
        h = fnv(h, (u64)(u16)(c1x - f.ox) | (u64)(u16)(c1y - f.oy) << 16 | (u64)(u16)(c2x - f.ox) << 32 | (u64)(u16)(c2y - f.oy) << 48);
        h = fnv(h, (u8)p.st.icon);
        p.key = h;
    }
}

bool similar(const Prim& a, const Prim& b) {
    auto ok = [](float x, float y) { return std::fabs(x - y) <= 4 + 0.5f * std::max(x, y); };
    return ok(a.bw, b.bw) && ok(a.bh, b.bh);
}

float median(std::vector<float>& v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Where each vertex of B was in A: a matched face's own corners, else its inherited move. Worked out once per game
// frame; every in-between picture moves the vertices back by that times 1 - t.
// (Welding the corners neighbouring faces share was tried against the hairline seams some in-between frames
// show: it did not remove them, since they are gaps in the game's own models, also seen as dots at 4x without
// interpolation, and it bent the buddies' feet.)
void displacements(const Frame& A, Frame& B) {
    const float sx = (float)(B.ox - A.ox), sy = (float)(B.oy - A.oy); // A's buffer -> B's buffer
    for (Prim& b : B.prims) {
        const Prim* a = b.match >= 0 ? &A.prims[b.match] : nullptr;
        const int n = b.kind == K_POLY || b.kind == K_LINE ? b.n : 1;
        for (int k = 0; k < n; k++) {
            if (a) { b.dx[k] = a->v[k].x + sx - b.v[k].x; b.dy[k] = a->v[k].y + sy - b.v[k].y; }
            else if (b.inherit) { b.dx[k] = -b.ix; b.dy[k] = -b.iy; }
            else b.dx[k] = b.dy[k] = 0;
        }
    }
}

// newest frame -> previous frame, in three passes:
//  1. anchors: primitives that occur exactly once in both frames. Certain matches, and samples of how each
//     part of the screen moved.
//  2. between consecutive anchors, in the order the game built its packets (its model order, the same from
//     frame to frame), equal primitives pair up in sequence. This tells apart parts that share a texture,
//     like a buddy's two feet.
//  3. what is left goes to the nearest equal primitive around where the local motion says it came from, so a
//     scrolling grid of identical terrain tiles does not latch onto the neighbouring tile.
void match() {
    std::vector<Prim>& A = s_prev.prims;
    std::vector<Prim>& B = s_cur.prims;
    auto matchable = [](const Prim& p) { return p.on_screen && p.kind != K_FILL; };
    static std::vector<int> oa, ob, rank_a;
    oa.clear();
    ob.clear();
    for (int i = 0; i < (int)A.size(); i++) if (matchable(A[i])) oa.push_back(i);
    for (int j = 0; j < (int)B.size(); j++) { B[j].match = -1; if (matchable(B[j])) ob.push_back(j); }
    std::sort(oa.begin(), oa.end(), [&](int x, int y) { return A[x].order < A[y].order; });
    std::sort(ob.begin(), ob.end(), [&](int x, int y) { return B[x].order < B[y].order; });
    rank_a.assign(A.size(), -1);
    for (int r = 0; r < (int)oa.size(); r++) rank_a[oa[r]] = r;

    static std::unordered_map<u64, std::vector<int>> groups;
    static std::unordered_map<u64, int> count_b;
    for (auto& g : groups) g.second.clear();
    if (groups.size() > 8192) groups.clear();
    count_b.clear();
    for (int i : oa) groups[A[i].key].push_back(i);
    for (int j : ob) count_b[B[j].key]++;
    static std::vector<char> taken;
    taken.assign(A.size(), 0);

    const int gw = std::max(1, (s_cur.dw + CELL - 1) / CELL), gh = std::max(1, (s_cur.dh + CELL - 1) / CELL);
    static std::vector<float> fdx, fdy, fn;
    fdx.assign(gw * gh, 0); fdy.assign(gw * gh, 0); fn.assign(gw * gh, 0);
    auto cell = [&](float x, float y) {
        int cx = std::clamp((int)std::floor(x / CELL), 0, gw - 1), cy = std::clamp((int)std::floor(y / CELL), 0, gh - 1);
        return cy * gw + cx;
    };
    int matched = 0;

    // 1. anchors
    static std::vector<float> all_dx, all_dy;
    static std::vector<std::pair<int, int>> anchors; // (rank in the newest frame's order, rank in the previous)
    all_dx.clear(); all_dy.clear(); anchors.clear();
    for (int rb = 0; rb < (int)ob.size(); rb++) {
        Prim& b = B[ob[rb]];
        auto g = groups.find(b.key);
        if (g == groups.end() || g->second.size() != 1 || count_b[b.key] != 1) continue;
        int i = g->second[0];
        const Prim& a = A[i];
        float dx = b.cx - a.cx, dy = b.cy - a.cy;
        if (std::fabs(dx) > MAX_MOVE || std::fabs(dy) > MAX_MOVE || !similar(a, b)) continue;
        b.match = i;
        b.how = 1;
        taken[i] = 1;
        matched++;
        int c = cell(b.cx, b.cy);
        fdx[c] += dx; fdy[c] += dy; fn[c] += 1;
        all_dx.push_back(dx); all_dy.push_back(dy);
        anchors.push_back({rb, rank_a[i]});
    }
    // motion per cell: its own anchors, else the neighbours' (up to two cells away), else the median
    float mdx = median(all_dx), mdy = median(all_dy);
    static std::vector<float> mx, my;
    mx.assign(gw * gh, mdx); my.assign(gw * gh, mdy);
    for (int cy = 0; cy < gh; cy++)
        for (int cx = 0; cx < gw; cx++) {
            float sx = 0, sy = 0, n = 0;
            for (int r = 0; r <= 2 && n == 0; r++)
                for (int y = std::max(0, cy - r); y <= std::min(gh - 1, cy + r); y++)
                    for (int x = std::max(0, cx - r); x <= std::min(gw - 1, cx + r); x++) {
                        int c = y * gw + x;
                        sx += fdx[c]; sy += fdy[c]; n += fn[c];
                    }
            if (n > 0) { mx[cy * gw + cx] = sx / n; my[cy * gw + cx] = sy / n; }
        }
    // equal size, believable move, close to how its part of the screen moved, and (when the key alone cannot
    // tell, as with untextured faces) the same shape and shading: a blend of two different faces is a sliver,
    // while an unmatched face still moves with its object (pass 4) and stays whole
    auto plausible = [&](const Prim& a, const Prim& b) {
        float dx = b.cx - a.cx, dy = b.cy - a.cy;
        if (std::fabs(dx) > MAX_MOVE || std::fabs(dy) > MAX_MOVE || !similar(a, b)) return false;
        int c = cell(b.cx, b.cy);
        float tol = std::max(16.0f, std::max(b.bw, b.bh));
        if (std::fabs(dx - mx[c]) > tol || std::fabs(dy - my[c]) > tol) return false;
        if (b.kind != K_POLY && b.kind != K_LINE) return true;
        float ax = 0, ay = 0, bx = 0, by = 0;
        for (int k = 0; k < b.n; k++) { ax += a.v[k].x; ay += a.v[k].y; bx += b.v[k].x; by += b.v[k].y; }
        ax /= b.n; ay /= b.n; bx /= b.n; by /= b.n;
        float shape = 0, shade = 0;
        for (int k = 0; k < b.n; k++) {
            float ex = (b.v[k].x - bx) - (a.v[k].x - ax), ey = (b.v[k].y - by) - (a.v[k].y - ay);
            shape += ex * ex + ey * ey;
            shade += std::abs(b.v[k].r - a.v[k].r) + std::abs(b.v[k].g - a.v[k].g) + std::abs(b.v[k].b - a.v[k].b);
        }
        shape /= b.n;
        shade /= 3.0f * b.n;
        if (shape > 4 + 0.04f * (b.bw * b.bw + b.bh * b.bh)) return false;
        return (b.flags & PF_TEXTURED) || shade <= 40;
    };
    // how far its vertices are from where the local motion puts them (picks the right one of equal faces)
    auto cost = [&](const Prim& a, const Prim& b) {
        int c = cell(b.cx, b.cy), n = b.kind == K_RECT ? 1 : b.n;
        float s = 0;
        for (int k = 0; k < n; k++) {
            float dx = (b.v[k].x - s_cur.ox) - (a.v[k].x - s_prev.ox) - mx[c];
            float dy = (b.v[k].y - s_cur.oy) - (a.v[k].y - s_prev.oy) - my[c];
            s += dx * dx + dy * dy;
        }
        return s / n;
    };

    // 2. build order between anchors that keep their order in both frames (longest increasing run)
    static std::vector<int> tails, tails_k, prev_k;
    static std::vector<std::pair<int, int>> chain;
    tails.clear(); tails_k.clear(); chain.clear();
    prev_k.assign(anchors.size(), -1);
    for (int k = 0; k < (int)anchors.size(); k++) {
        int v = anchors[k].second;
        int pos = (int)(std::lower_bound(tails.begin(), tails.end(), v) - tails.begin());
        if (pos == (int)tails.size()) { tails.push_back(v); tails_k.push_back(k); }
        else { tails[pos] = v; tails_k[pos] = k; }
        prev_k[k] = pos > 0 ? tails_k[pos - 1] : -1;
    }
    chain.push_back({(int)ob.size(), (int)oa.size()}); // end sentinel, the chain is built backwards
    for (int k = tails_k.empty() ? -1 : tails_k.back(); k >= 0; k = prev_k[k]) chain.push_back(anchors[k]);
    chain.push_back({-1, -1});
    std::reverse(chain.begin(), chain.end());
    // within a window ahead in build order, the equal primitive whose vertices fit best
    const int WINDOW = 48;
    for (size_t g = 0; g + 1 < chain.size(); g++) {
        int b0 = chain[g].first + 1, b1 = chain[g + 1].first;
        int a0 = chain[g].second + 1, a1 = chain[g + 1].second;
        int p = a0;
        for (int rb = b0; rb < b1; rb++) {
            Prim& b = B[ob[rb]];
            if (b.match >= 0) continue;
            int best = -1, best_ra = -1;
            float best_c = 1e30f;
            for (int ra = p; ra < a1 && ra < p + WINDOW; ra++) {
                int i = oa[ra];
                if (taken[i] || A[i].key != b.key || !plausible(A[i], b)) continue;
                float c = cost(A[i], b);
                if (c < best_c) { best_c = c; best = i; best_ra = ra; }
            }
            if (best < 0) continue;
            b.match = best;
            b.how = 2;
            taken[best] = 1;
            matched++;
            p = best_ra + 1;
        }
    }

    // 3. nearest equal primitive around where it should have been
    for (int j : ob) {
        Prim& b = B[j];
        if (b.match >= 0) continue;
        auto g = groups.find(b.key);
        if (g == groups.end()) continue;
        float r = std::clamp(0.45f * std::max(b.bw, b.bh), 4.0f, 24.0f);
        float best_c = r * r;
        int best = -1;
        for (int i : g->second) {
            if (taken[i] || !plausible(A[i], b)) continue;
            float c = cost(A[i], b);
            if (c < best_c) { best_c = c; best = i; }
        }
        if (best >= 0) { b.match = best; b.how = 3; taken[best] = 1; matched++; }
    }

    // 4. the rest (a face hidden in the previous frame, a part that changed shape) moves like the matched
    //    primitives next to it in build order and on screen: most likely the same object. A weighted average,
    //    so neighbouring leftovers move together and the object stays in one piece.
    for (int rb = 0; rb < (int)ob.size(); rb++) {
        Prim& b = B[ob[rb]];
        b.inherit = false;
        if (b.match < 0) b.how = 0;
        if (b.match >= 0) continue;
        float sx = 0, sy = 0, sw = 0;
        for (int k = 1; k <= 8; k++)
            for (int side = -1; side <= 1; side += 2) {
                int r = rb + side * k;
                if (r < 0 || r >= (int)ob.size()) continue;
                const Prim& n = B[ob[r]];
                if (n.match < 0) continue;
                float dx = n.cx - b.cx, dy = n.cy - b.cy, d = dx * dx + dy * dy;
                if (d >= 32 * 32) continue;
                float w = 1.0f / (4.0f + d);
                sx += w * (n.cx - A[n.match].cx);
                sy += w * (n.cy - A[n.match].cy);
                sw += w;
            }
        if (sw > 0) {
            b.ix = sx / sw;
            b.iy = sy / sw;
            b.inherit = true;
            b.how = 4;
        }
    }
    s_matched = ob.empty() ? 0 : (float)matched / ob.size();
    displacements(s_prev, s_cur);

    // test harness: TB_INTERP_DUMP="vblank,x0,y0,x1,y1" logs the matches inside that display rectangle once
    static const char* dump = getenv("TB_INTERP_DUMP");
    static bool dumped = false;
    int dv = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (dump && !dumped && sscanf(dump, "%d,%d,%d,%d,%d", &dv, &x0, &y0, &x1, &y1) == 5 && g_dbg_frames >= (u64)dv) {
        dumped = true;
        static const char* how[] = {"none", "anchor", "order", "near", "inherit"};
        plog("interp dump: vblank %llu, origin %d,%d (prev %d,%d)\n", g_dbg_frames, s_cur.ox, s_cur.oy, s_prev.ox, s_prev.oy);
        for (int rb = 0; rb < (int)ob.size(); rb++) {
            const Prim& b = B[ob[rb]];
            if (b.cx < x0 || b.cx > x1 || b.cy < y0 || b.cy > y1) continue;
            char line[512];
            int o = snprintf(line, sizeof line, "  #%d %s k%d n%d f%X tp%04X cl%04X %s", rb, how[b.how], b.kind, b.n, b.flags, b.st.texpage, b.st.clut,
                             b.inherit ? "" : "");
            for (int k = 0; k < (b.kind == K_RECT ? 1 : b.n); k++)
                o += snprintf(line + o, sizeof line - o, " (%d,%d uv%d,%d)", b.v[k].x - s_cur.ox, b.v[k].y - s_cur.oy, b.v[k].u, b.v[k].v);
            if (b.match >= 0) {
                const Prim& a = A[b.match];
                o += snprintf(line + o, sizeof line - o, " <- #%d", rank_a[b.match]);
                for (int k = 0; k < (a.kind == K_RECT ? 1 : a.n); k++) o += snprintf(line + o, sizeof line - o, " (%d,%d)", a.v[k].x - s_prev.ox, a.v[k].y - s_prev.oy);
            } else if (b.inherit) {
                o += snprintf(line + o, sizeof line - o, " moves %.1f,%.1f", b.ix, b.iy);
            }
            plog("%s\n", line);
        }
    }
}

bool too_big(const Vertex& a, const Vertex& b, const Vertex& c) { // the GPU drops these (gpu.cpp does the same)
    s32 x0 = std::min(a.x, std::min(b.x, c.x)), x1 = std::max(a.x, std::max(b.x, c.x));
    s32 y0 = std::min(a.y, std::min(b.y, c.y)), y1 = std::max(a.y, std::max(b.y, c.y));
    return x1 - x0 >= 1024 || y1 - y0 >= 512;
}

// frame B, each vertex moved back towards where it was in A (the frame before B) by (1 - t) (see displacements),
// drawn into the display area of the newest frame
void draw(const Frame& A, const Frame& B, float t) {
    const int dx = s_cur.ox - B.ox, dy = s_cur.oy - B.oy; // B's buffer -> the one on screen
    const float back = 1 - t;
    for (const Prim& b : B.prims) {
        if (!b.on_screen) continue;
        const Prim* a = b.match >= 0 ? &A.prims[b.match] : nullptr; // matched: the colours blend too
        auto vert = [&](int k) {
            const Vertex& p = b.v[k];
            VertexF o{p.x + b.dx[k] * back + dx, p.y + b.dy[k] * back + dy, p.r, p.g, p.b, p.u, p.v};
            if (a) {
                o.r = (u8)std::lround(a->v[k].r + (p.r - a->v[k].r) * t);
                o.g = (u8)std::lround(a->v[k].g + (p.g - a->v[k].g) * t);
                o.b = (u8)std::lround(a->v[k].b + (p.b - a->v[k].b) * t);
            }
            return o;
        };
        GpuDrawState st = b.st; // the clip rectangle moves along to the buffer on screen
        st.clip_x1 = (s16)(st.clip_x1 + dx); st.clip_x2 = (s16)(st.clip_x2 + dx);
        st.clip_y1 = (s16)(st.clip_y1 + dy); st.clip_y2 = (s16)(st.clip_y2 + dy);
        switch (b.kind) {
        case K_POLY: {
            VertexF v[4];
            for (int k = 0; k < b.n; k++) v[k] = vert(k);
            if (!too_big(b.v[0], b.v[1], b.v[2])) g_renderer->interp_tri(v, b.flags, st);
            if (b.n == 4 && !too_big(b.v[1], b.v[2], b.v[3])) {
                VertexF q[3] = {v[1], v[2], v[3]};
                g_renderer->interp_tri(q, b.flags, st);
            }
            break;
        }
        case K_RECT:
            g_renderer->interp_rect(b.v[0].x + b.dx[0] * back + dx, b.v[0].y + b.dy[0] * back + dy, b.w, b.h, (u8)b.color,
                                    (u8)(b.color >> 8), (u8)(b.color >> 16), b.v[0].u, b.v[0].v, b.flags, st, b.fx, b.fy);
            break;
        case K_LINE:
            g_renderer->interp_line(vert(0), vert(1), b.flags, st);
            break;
        case K_FILL:
            g_renderer->interp_fill(b.v[0].x + dx, b.v[0].y + dy, b.w, b.h, b.color);
            break;
        }
    }
}
} // namespace

// ---------------------------------------------------------------- recording (gpu.cpp, only while g_interp_on)
void interp_rec_poly(const Vertex* v, int n, int flags, const GpuDrawState& st) {
    Prim* p = push(K_POLY);
    if (!p) return;
    p->n = (u8)n;
    p->flags = flags;
    p->st = st;
    for (int k = 0; k < n; k++) p->v[k] = v[k];
}

void interp_rec_rect(s32 x, s32 y, s32 w, s32 h, u32 color, u8 u, u8 v, int flags, const GpuDrawState& st, bool flipx, bool flipy) {
    Prim* p = push(K_RECT);
    if (!p) return;
    p->flags = flags;
    p->st = st;
    p->v[0] = Vertex{x, y, 0, 0, 0, u, v};
    p->w = w;
    p->h = h;
    p->color = color;
    p->fx = flipx;
    p->fy = flipy;
}

void interp_rec_line(const Vertex& a, const Vertex& b, int flags, const GpuDrawState& st) {
    Prim* p = push(K_LINE);
    if (!p) return;
    p->n = 2;
    p->flags = flags;
    p->st = st;
    p->v[0] = a;
    p->v[1] = b;
}

void interp_rec_fill(int x, int y, int w, int h, u32 color) {
    Prim* p = push(K_FILL);
    if (!p) return;
    p->v[0] = Vertex{x, y, 0, 0, 0, 0, 0};
    p->w = w;
    p->h = h;
    p->color = color;
}

void interp_flip(const DisplayInfo& d) {
    if (!g_interp_on) return;
    s_rec.ox = d.x;
    s_rec.oy = d.y;
    s_rec.dw = d.w;
    s_rec.dh = d.h;
    s_rec.c = s_vcount + 1; // the original shows it from the next vblank on
    finalize(s_rec);
    std::swap(s_rec, s_done); // a second flip before the next vblank just replaces the finished frame
    s_rec.prims.clear();
    s_flip_pending = true;
}

// ---------------------------------------------------------------- presenting (every vblank)
namespace {
DisplayInfo s_disp;

// frames per second to show: 0 = the game's own, <= 60 = one per vblank, above = unlocked
int target_fps() {
    int fr = g_settings.frame_rate;
    if (fr == 0) return 0;
    int hz = platform_refresh_hz();
    int fps = fr == FRAME_RATE_MONITOR ? hz : fr;
    if (g_settings.vsync) fps = std::min(fps, hz);
    return std::max(fps, 60);
}

// The picture for the moment `frac` of the way from the previous vblank to this one (below 0 when it is shown
// a refresh late: up to a refresh before the previous vblank). The picture runs (period - 1) vblanks behind the
// game: one vblank in gameplay, none in 60 fps menus. The game's next frame is computed before its vblank is due
// (the emulator runs ahead and then waits), so that delay is enough to always have a frame on each side.
// older = false: the newest frame blended from the one before it; true: that one from its own predecessor.
// t = 1 is the frame itself.
struct Blend { bool older; double t; };
Blend pick(double frac) {
    double x = (double)(s_vcount - 1) + frac - (s_period - 1);
    auto t_of = [&](const Frame& A, const Frame& B, bool have) {
        long long span = B.c - A.c;
        if (!have || span < 1 || span > 4 || B.matched < 0.5f) return 1.0;
        return std::clamp((x - (double)A.c) / (double)span, 0.0, 1.0);
    };
    if (!s_have_prev || x >= (double)s_prev.c) return {false, t_of(s_prev, s_cur, s_have_prev)};
    return {true, t_of(s_prev2, s_prev, s_have_prev2)};
}

// unlocked: one picture for platform_present_unlocked (the real frame when the blend reaches it)
bool draw_at(double frac) {
    Blend b = pick(frac);
    if (!g_renderer->interp_begin(s_disp)) return false;
    if (b.older) draw(s_prev2, s_prev, (float)b.t);
    else if (b.t < 0.999) draw(s_prev, s_cur, (float)b.t);
    g_renderer->interp_present(s_disp, false);
    s_stat_shown++;
    return true;
}
} // namespace

bool interp_present(const DisplayInfo& d) {
    const int fps = target_fps();
    const bool want = fps > 0 && g_renderer && g_renderer->interp_supported();
    if (want != g_interp_on) { reset(); g_interp_on = want; }
    if (!want) return false;
    s_vcount++;
    if (s_flip_pending) {
        s_flip_pending = false;
        std::swap(s_prev2, s_prev); // prev2 <- prev <- cur <- done (keeps each frame's matches to the one before)
        std::swap(s_prev, s_cur);
        std::swap(s_cur, s_done);
        s_done.prims.clear();
        s_have_prev2 = s_have_prev;
        s_have_prev = s_have_cur;
        s_have_cur = true;
        s_period = s_have_prev ? (int)std::clamp<long long>(s_cur.c - s_prev.c, 1, 1000) : 1;
        s_matched = 0;
        if (s_have_prev && s_period <= 4 && (s_period >= 2 || fps > 60)) { // 60 fps frames need no blend at 60
            auto t0 = std::chrono::steady_clock::now();
            match();
            s_stat_match_ms = std::max(s_stat_match_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            s_stat_frames++;
            s_stat_matched += s_matched;
        }
        s_cur.matched = s_matched;
    }
    if (g_settings.log_level >= 2 && ++s_stat_presents >= 60) {
        if (s_stat_frames)
            plog("    interp: %d frames shown, %.0f%% of primitives matched (match up to %.1f ms, %d primitives)\n", s_stat_shown,
                 100.0 * s_stat_matched / s_stat_frames, s_stat_match_ms, (int)s_cur.prims.size());
        s_stat_presents = s_stat_shown = s_stat_frames = 0;
        s_stat_matched = s_stat_match_ms = 0;
    }
    // movies, a blank screen or a buffer we did not record: the renderer shows it as usual
    if (d.depth24 || !d.enabled || !s_have_cur || d.x != s_cur.ox || d.y != s_cur.oy || !g_renderer->interp_ready(d)) return false;
    s_disp = d;
    if (fps > 60) {
        platform_present_unlocked(fps, draw_at);
        return true;
    }
    // one picture per vblank: halfway in gameplay, the real frame on the next vblank
    Blend b = pick(1.0);
    if (b.older || b.t >= 0.999 || !g_renderer->interp_begin(d)) return false;
    draw(s_prev, s_cur, (float)b.t);
    g_renderer->interp_present(d, true);
    s_stat_shown++;
    return true;
}
