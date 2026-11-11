// Knockback arena bot: approximate forward simulation over candidate orders.
#include "include/step_worker.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
using J = mini_json::Json;

namespace {
// ---- constants (overwritten from rules every turn) ----
double RAD = 1.0, HAM_REACH = 3.2, HAM_COS = 0.5, HAM_POW = 0.9, HAM_RECOIL = 0.15;
double ATT_REACH = 2.8, ATT_COS = 0.5, ATT_POW = 0.35, ATT_RECOIL = 0.05;
double FRIC = 0.06, ACCEL = 0.018, REST = 0.9, MIN_SPEED = 0.001, MIN_IMP = 0.03;
double BOMB_R = 0.6, BOMB_M = 0.5, BOMB_F = 0.05, BLAST_R = 3.5, BLAST_P = 1.4, THROW = 0.5, SPAWN_GAP = 0.2;
double WALL_T = 0.5, WALL_REST = 0.8, SPRING_F = 1.5, SPRING_MAX = 0.5, REGEN_PUSH = 0.5;
double DASH_SPEED = 0.9, CHARGE_RATE = 0.04, HAM_CF = 0.3, DASH_CF = 0.2;
int ATT_WINDUP = 9, ATT_RECOVER = 6, HAM_RECOVER = 5, OVER_T = 45;

double num(const J& j, const char* k, double d = 0) {
    if (!j.contains(k)) return d;
    const J& v = j.at(k);
    if (v.is_null()) return d;
    return v.as_double();
}

struct P {
    double x = 0, y = 0, vx = 0, vy = 0, bar = 0, charge = 0;
    int skill = 0;  // 0 none 1 dash 2 hammer
    bool out = true;
    int act = 0;  // 0 none 1 jab 2 jab_recover 3 recover
    int act_ticks = 0;
    double ax = 0, ay = 0;
    int bomb_cd = 0, attack_cd = 0, hammer_cd = 0, held = 0, pause = 0;
};
struct Bm { double x, y, vx, vy; int fuse; int owner; };
struct Wl { double x1, y1, x2, y2; bool spring; int at; };  // at: tick it becomes solid (0 = solid now)

struct World {
    int me = 0, n = 0, tick = 0, turn = 0;
    double half = 16.5;
    bool hasNext = false;
    double nextHalf = 0;
    int nextTurn = 0, nextEnd = 0;
    std::vector<P> ps;
    std::vector<Bm> bombs;
    std::vector<Wl> walls;
    double halfAt(int t) const {  // t = absolute tick number
        if (!hasNext) return half;
        int s0 = (nextTurn - 1) * 6;
        int e0 = s0 + 60;
        if (tick >= e0) return std::max(nextHalf, half);
        if (t <= s0) return half;
        if (tick < s0) {
            double rate = (half - nextHalf) / 60.0;
            return std::max(nextHalf, half - rate * (t - s0));
        }
        double rate = (half - nextHalf) / double(std::max(1, e0 - tick));
        return std::max(nextHalf, half - rate * (t - tick));
    }
};

struct Order {
    bool move = false; double mx = 0, my = 0;
    bool charge = false;
    bool hammer = false; double hx = 0, hy = 0;
    bool jab = false; double jx = 0, jy = 0;
    bool bomb = false; double bx = 0, by = 0; int fuse = 30;
    bool cont = false;
};

double slideTable[2001];
void initSlide() {
    for (int i = 0; i <= 2000; ++i) {
        double v = i * 0.001, d = 0;
        for (int t = 0; t < 400 && v > 0.0005; ++t) {
            v = std::max(0.0, v - ACCEL);
            d += v;
            v *= (1 - FRIC);
        }
        slideTable[i] = d;
    }
}
double slide(double v) {
    if (v <= 0) return 0;
    int i = int(v * 1000);
    if (i >= 2000) return slideTable[2000] + (v - 2.0) * 15.0;
    return slideTable[i];
}

void wallCollide(double& x, double& y, double& vx, double& vy, double r, const Wl& w) {
    double ex = w.x2 - w.x1, ey = w.y2 - w.y1, l = ex * ex + ey * ey;
    double t = l > 0 ? ((x - w.x1) * ex + (y - w.y1) * ey) / l : 0;
    t = std::max(0.0, std::min(1.0, t));
    double cx = w.x1 + t * ex, cy = w.y1 + t * ey;
    double dx = x - cx, dy = y - cy, d = std::sqrt(dx * dx + dy * dy);
    double lim = r + WALL_T / 2;
    if (d >= lim) return;
    double nx, ny;
    if (d < 1e-9) { double ll = std::sqrt(l); nx = -ey / ll; ny = ex / ll; }
    else { nx = dx / d; ny = dy / d; }
    x = cx + nx * lim; y = cy + ny * lim;
    double vn = vx * nx + vy * ny;
    if (vn < 0) {
        double v = -vn;
        double out = w.spring ? std::min(SPRING_F * v, SPRING_MAX) : WALL_REST * v;
        vx += (out - vn) * nx; vy += (out - vn) * ny;
    }
}

struct SimOut {
    bool meOut = false; int meOutT = 999;
    double mx = 0, my = 0;  // my pos at end of turn (t=6)
    double ex = 0, ey = 0;  // my pos at end of horizon
    double minMargin = 99;
    std::vector<double> ox, oy;  // others at t=6
    double mx12 = 0, my12 = 0;
    std::vector<double> ox12, oy12, oxe, oye;
    std::vector<int> fellMine, fell;
    int fellAny = 0;
    struct BX { double mx, my, bx, by; int t; int owner; };
    std::vector<BX> blasts;
};

const int H = 42;

SimOut simulate(const World& w, const Order& o) {
    int n = w.n, me = w.me;
    struct S { double x, y, vx, vy; bool out; int lastMe; int act; int actT; double ax, ay; bool brake; };
    std::vector<S> s(n);
    for (int i = 0; i < n; ++i) {
        const P& p = w.ps[i];
        s[i] = {p.x, p.y, p.vx, p.vy, p.out, 0, p.act, p.act_ticks, p.ax, p.ay, false};
    }
    std::vector<Bm> bombs = w.bombs;
    SimOut r;
    r.ox.assign(n, 0); r.oy.assign(n, 0); r.ox12.assign(n, 0); r.oy12.assign(n, 0); r.fellMine.assign(n, 0); r.fell.assign(n, 0);
    const P& mp = w.ps[me];
    double myCharge = mp.charge;
    int mySkill = mp.skill;
    if (o.charge) { mySkill = 2; myCharge = std::min(1.0, myCharge + std::min(mp.bar, 0.24)); }
    int myAct = 0, myActT = 0;  // 1 jab windup 2 jab recover 3 hammer recover
    double jabx = 0, jaby = 0;
    for (int t = 1; t <= H; ++t) {
        int at = w.tick + t;
        double h = w.halfAt(at);
        // bombs fuse
        for (size_t b = 0; b < bombs.size();) {
            bombs[b].fuse--;
            if (bombs[b].fuse <= 0) {
                Bm bb = bombs[b];
                bombs.erase(bombs.begin() + b);
                if (!s[me].out) r.blasts.push_back({s[me].x, s[me].y, bb.x, bb.y, t, bb.owner});
                for (int i = 0; i < n; ++i) {
                    if (s[i].out) continue;
                    double dx = s[i].x - bb.x, dy = s[i].y - bb.y, d = std::sqrt(dx * dx + dy * dy);
                    if (d > BLAST_R) continue;
                    double ux = 1, uy = 0;
                    if (d > 1e-9) { ux = dx / d; uy = dy / d; }
                    double pw = BLAST_P * (1 - d / BLAST_R);
                    s[i].vx += ux * pw; s[i].vy += uy * pw;
                    s[i].lastMe = (bb.owner == me) ? (i == me ? 0 : 1) : 0;
                    s[i].brake = true;
                }
                continue;
            }
            ++b;
        }
        // regrowing walls
        for (const Wl& wl : w.walls) {
            if (wl.at == at) {
                for (int i = 0; i < n; ++i) {
                    if (s[i].out) continue;
                    double x0 = s[i].x, y0 = s[i].y, vx0 = s[i].vx, vy0 = s[i].vy;
                    Wl tmp = wl; tmp.spring = false;
                    wallCollide(s[i].x, s[i].y, s[i].vx, s[i].vy, RAD, tmp);
                    if (s[i].x != x0 || s[i].y != y0) {
                        double nx = s[i].x - x0, ny = s[i].y - y0, l = std::sqrt(nx * nx + ny * ny);
                        nx /= l; ny /= l;
                        double vn = vx0 * nx + vy0 * ny;
                        s[i].vx = vx0 + (REGEN_PUSH - vn) * nx; s[i].vy = vy0 + (REGEN_PUSH - vn) * ny;
                    }
                }
            }
        }
        // actions: opponents' jab windups
        for (int i = 0; i < n; ++i) {
            if (s[i].out || i == me) continue;
            if (s[i].act == 1 && s[i].actT > 0) {
                s[i].actT--;
                if (s[i].actT == 0) {
                    for (int j = 0; j < n; ++j) {
                        if (j == i || s[j].out) continue;
                        double dx = s[j].x - s[i].x, dy = s[j].y - s[i].y, d = std::sqrt(dx * dx + dy * dy);
                        if (d > ATT_REACH) continue;
                        double ux = 1, uy = 0; if (d > 1e-9) { ux = dx / d; uy = dy / d; }
                        if (d > 1e-9 && ux * s[i].ax + uy * s[i].ay < ATT_COS) continue;
                        s[j].vx += ux * ATT_POW; s[j].vy += uy * ATT_POW;
                        if (j == me) {} else s[j].lastMe = 0;
                        s[j].brake = true;
                    }
                    s[i].act = 2; s[i].actT = ATT_RECOVER;
                }
            } else if (s[i].actT > 0) {
                s[i].actT--;
                if (s[i].actT == 0) s[i].act = 0;
            }
        }
        // my actions
        S& m = s[me];
        bool canMove = true;
        if (myAct != 0) {
            myActT--;
            if (myAct == 1 && myActT == 0) {
                for (int j = 0; j < n; ++j) {
                    if (j == me || s[j].out) continue;
                    double dx = s[j].x - m.x, dy = s[j].y - m.y, d = std::sqrt(dx * dx + dy * dy);
                    if (d > ATT_REACH) continue;
                    double ux = 1, uy = 0; if (d > 1e-9) { ux = dx / d; uy = dy / d; }
                    if (d > 1e-9 && ux * jabx + uy * jaby < ATT_COS) continue;
                    s[j].vx += ux * ATT_POW; s[j].vy += uy * ATT_POW; s[j].lastMe = 1; s[j].brake = true;
                }
                for (auto& b : bombs) {
                    double dx = b.x - m.x, dy = b.y - m.y, d = std::sqrt(dx * dx + dy * dy);
                    if (d > ATT_REACH) continue;
                    double ux = 1, uy = 0; if (d > 1e-9) { ux = dx / d; uy = dy / d; }
                    if (d > 1e-9 && ux * jabx + uy * jaby < ATT_COS) continue;
                    b.vx += ux * ATT_POW / BOMB_M; b.vy += uy * ATT_POW / BOMB_M; b.owner = me;
                }
                m.vx -= jabx * ATT_RECOIL; m.vy -= jaby * ATT_RECOIL;
                myAct = 2; myActT = ATT_RECOVER;
            } else if (myActT <= 0) myAct = 0;
            canMove = false;
        }
        if (t == 1 && myAct == 0) {
            if (o.hammer && mp.skill == 2 && mp.charge > 0) {
                double c = mp.charge;
                for (int j = 0; j < n; ++j) {
                    if (j == me || s[j].out) continue;
                    double dx = s[j].x - m.x, dy = s[j].y - m.y, d = std::sqrt(dx * dx + dy * dy);
                    if (d > HAM_REACH) continue;
                    double ux = 1, uy = 0; if (d > 1e-9) { ux = dx / d; uy = dy / d; }
                    if (d > 1e-9 && ux * o.hx + uy * o.hy < HAM_COS) continue;
                    s[j].vx += ux * HAM_POW * c; s[j].vy += uy * HAM_POW * c; s[j].lastMe = 1; s[j].brake = true;
                }
                for (auto& b : bombs) {
                    double dx = b.x - m.x, dy = b.y - m.y, d = std::sqrt(dx * dx + dy * dy);
                    if (d > HAM_REACH) continue;
                    double ux = 1, uy = 0; if (d > 1e-9) { ux = dx / d; uy = dy / d; }
                    if (d > 1e-9 && ux * o.hx + uy * o.hy < HAM_COS) continue;
                    b.vx += ux * HAM_POW * c / BOMB_M; b.vy += uy * HAM_POW * c / BOMB_M; b.owner = me;
                }
                m.vx -= o.hx * HAM_RECOIL * c; m.vy -= o.hy * HAM_RECOIL * c;
                myCharge = 0; mySkill = 0;
                myAct = 3; myActT = HAM_RECOVER;
                canMove = false;
            } else if (o.jab && mp.attack_cd == 0) {
                myAct = 1; myActT = ATT_WINDUP; jabx = o.jx; jaby = o.jy;
                canMove = false;
            }
        }
        if (canMove) {
            double f = mySkill == 2 && myCharge > 0 ? HAM_CF : (mySkill == 1 && myCharge > 0 ? DASH_CF : 1.0);
            if (t <= 6 || (o.cont && t <= 12)) {
                if (o.move) { m.vx += o.mx * ACCEL * f; m.vy += o.my * ACCEL * f; }
            } else {
                double sp = std::sqrt(m.vx * m.vx + m.vy * m.vy);
                if (sp > 0.03) { m.vx -= m.vx / sp * ACCEL * f; m.vy -= m.vy / sp * ACCEL * f; }
            }
        }
        if (t == 1 && o.bomb && mp.bomb_cd == 0 && myAct != 3 && !(myAct == 1)) {
            double dd = RAD + BOMB_R + SPAWN_GAP;
            bombs.push_back({m.x + o.bx * dd, m.y + o.by * dd, m.vx + o.bx * THROW, m.vy + o.by * THROW, o.fuse, me});
        }
        // others move: brake when fast
        for (int i = 0; i < n; ++i) {
            if (i == me || s[i].out || s[i].act != 0) continue;
            double sp = std::sqrt(s[i].vx * s[i].vx + s[i].vy * s[i].vy);
            const P& p = w.ps[i];
            double f = p.charge > 0 ? (p.skill == 2 ? HAM_CF : DASH_CF) : 1.0;
            if (sp > 0.27 || s[i].brake) {
                if (sp > 0.02) { s[i].vx -= s[i].vx / sp * ACCEL * f; s[i].vy -= s[i].vy / sp * ACCEL * f; }
            }
        }
        // integrate
        for (int i = 0; i < n; ++i) if (!s[i].out) { s[i].x += s[i].vx; s[i].y += s[i].vy; }
        for (auto& b : bombs) { b.x += b.vx; b.y += b.vy; }
        // player-player
        for (int i = 0; i < n; ++i) {
            if (s[i].out) continue;
            for (int j = i + 1; j < n; ++j) {
                if (s[j].out) continue;
                double dx = s[j].x - s[i].x, dy = s[j].y - s[i].y, d2 = dx * dx + dy * dy;
                if (d2 >= 4 * RAD * RAD) continue;
                double d = std::sqrt(d2), nx = 1, ny = 0;
                if (d > 1e-9) { nx = dx / d; ny = dy / d; }
                double ov = 2 * RAD - d;
                s[i].x -= nx * ov / 2; s[i].y -= ny * ov / 2; s[j].x += nx * ov / 2; s[j].y += ny * ov / 2;
                double rel = (s[i].vx - s[j].vx) * nx + (s[i].vy - s[j].vy) * ny;
                if (rel > 0) {
                    double Jm = (1 + REST) * rel / 2;
                    s[i].vx -= Jm * nx; s[i].vy -= Jm * ny; s[j].vx += Jm * nx; s[j].vy += Jm * ny;
                    if (Jm >= MIN_IMP) {
                        if (i == me) s[j].lastMe = 1; else if (j != me) s[j].lastMe = 0;
                        if (j == me) s[i].lastMe = 1; else if (i != me) s[i].lastMe = 0;
                        s[i].brake = s[j].brake = true;
                    }
                }
            }
        }
        // player-bomb
        for (int i = 0; i < n; ++i) {
            if (s[i].out) continue;
            for (auto& b : bombs) {
                double dx = b.x - s[i].x, dy = b.y - s[i].y, d2 = dx * dx + dy * dy, rr = RAD + BOMB_R;
                if (d2 >= rr * rr) continue;
                double d = std::sqrt(d2), nx = 1, ny = 0;
                if (d > 1e-9) { nx = dx / d; ny = dy / d; }
                double ov = rr - d, im = 1.0, ib = 1.0 / BOMB_M, tot = im + ib;
                s[i].x -= nx * ov * im / tot; s[i].y -= ny * ov * im / tot; b.x += nx * ov * ib / tot; b.y += ny * ov * ib / tot;
                double vp = s[i].vx * nx + s[i].vy * ny, vb = b.vx * nx + b.vy * ny, rel = vp - vb;
                if (rel > 0) {
                    double Jm = (1 + REST) * rel / tot;
                    s[i].vx -= Jm * im * nx; s[i].vy -= Jm * im * ny; b.vx += Jm * ib * nx; b.vy += Jm * ib * ny;
                    if (Jm >= MIN_IMP && vp > -vb) b.owner = i;
                }
            }
        }
        // walls
        for (const Wl& wl : w.walls) {
            if (wl.at > at) continue;
            if (std::max(std::fabs(wl.x1), std::fabs(wl.y1)) > h || std::max(std::fabs(wl.x2), std::fabs(wl.y2)) > h) continue;
            for (int i = 0; i < n; ++i) if (!s[i].out) wallCollide(s[i].x, s[i].y, s[i].vx, s[i].vy, RAD, wl);
            for (auto& b : bombs) wallCollide(b.x, b.y, b.vx, b.vy, BOMB_R, wl);
        }
        // friction
        for (int i = 0; i < n; ++i) {
            if (s[i].out) continue;
            s[i].vx *= (1 - FRIC); s[i].vy *= (1 - FRIC);
            if (s[i].vx * s[i].vx + s[i].vy * s[i].vy < MIN_SPEED * MIN_SPEED) s[i].vx = s[i].vy = 0;
        }
        for (auto& b : bombs) { b.vx *= (1 - BOMB_F); b.vy *= (1 - BOMB_F); }
        // falls
        for (int i = 0; i < n; ++i) {
            if (s[i].out) continue;
            if (std::max(std::fabs(s[i].x), std::fabs(s[i].y)) > h) {
                s[i].out = true;
                if (i == me) { r.meOut = true; r.meOutT = t; }
                else { r.fell[i] = 1; r.fellAny++; if (s[i].lastMe) r.fellMine[i] = 1; }
            }
        }
        for (size_t b = 0; b < bombs.size();) {
            if (std::max(std::fabs(bombs[b].x), std::fabs(bombs[b].y)) > h) bombs.erase(bombs.begin() + b); else ++b;
        }
        if (!s[me].out) r.minMargin = std::min(r.minMargin, h - std::max(std::fabs(s[me].x), std::fabs(s[me].y)));
        if (t == 6) {
            r.mx = s[me].x; r.my = s[me].y;
            for (int i = 0; i < n; ++i) { r.ox[i] = s[i].x; r.oy[i] = s[i].y; }
        }
        if (t == 12) {
            r.mx12 = s[me].x; r.my12 = s[me].y;
            for (int i = 0; i < n; ++i) { r.ox12[i] = s[i].x; r.oy12[i] = s[i].y; }
        }
        if (r.meOut) break;
    }
    r.ex = s[me].x; r.ey = s[me].y;
    r.oxe.assign(n, 0); r.oye.assign(n, 0);
    for (int i = 0; i < n; ++i) { r.oxe[i] = s[i].x; r.oye[i] = s[i].y; }
    return r;
}

// ---- wall-aware escape distance to the future safe zone ----
const double GC = 0.5;
const int GN = 68;
float escD[GN][GN];
bool escOn = false;
double escW = 0;
inline int gidx(double v) { return int(std::floor((v + GN * GC / 2) / GC)); }
void buildEscape(const World& w) {
    escOn = false;
    double hT = (w.hasNext ? w.nextHalf : w.half) - 1.0;
    if (hT < 0.3) hT = 0.3;
    static bool blocked[GN][GN];
    std::vector<int> q; q.reserve(GN * GN);
    for (int i = 0; i < GN; ++i) for (int j = 0; j < GN; ++j) {
        double x = -GN * GC / 2 + (i + 0.5) * GC, y = -GN * GC / 2 + (j + 0.5) * GC;
        bool b = std::max(std::fabs(x), std::fabs(y)) > w.half;
        if (!b) for (const Wl& wl : w.walls) {
            if (wl.at > 0 && wl.at - w.tick > 60) continue;
            double ex = wl.x2 - wl.x1, ey = wl.y2 - wl.y1, l = ex * ex + ey * ey;
            double t = l > 0 ? ((x - wl.x1) * ex + (y - wl.y1) * ey) / l : 0;
            t = std::max(0.0, std::min(1.0, t));
            double dx = x - (wl.x1 + t * ex), dy = y - (wl.y1 + t * ey);
            if (dx * dx + dy * dy < (RAD + WALL_T / 2) * (RAD + WALL_T / 2) * 0.95) { b = true; break; }
        }
        blocked[i][j] = b;
        escD[i][j] = 1e9f;
        if (!b && std::max(std::fabs(x), std::fabs(y)) <= hT) { escD[i][j] = 0; q.push_back(i * GN + j); }
    }
    for (size_t h = 0; h < q.size(); ++h) {
        int i = q[h] / GN, j = q[h] % GN;
        static const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            int a = i + di[k], b = j + dj[k];
            if (a < 0 || b < 0 || a >= GN || b >= GN || blocked[a][b]) continue;
            if (escD[a][b] <= escD[i][j] + GC) continue;
            escD[a][b] = escD[i][j] + GC;
            q.push_back(a * GN + b);
        }
    }
    escOn = true;
    // urgency: stronger when a shrink is announced and near
    escW = 25;
    if (w.hasNext) {
        int left = w.nextEnd - w.turn;
        escW = left <= 6 ? 120 : (left <= 12 ? 80 : 50);
    }
}
double escapeDist(double x, double y) {
    if (!escOn) return 0;
    int i = gidx(x), j = gidx(y);
    double best = 1e9;
    for (int a = i - 1; a <= i + 1; ++a) for (int b = j - 1; b <= j + 1; ++b) {
        if (a < 0 || b < 0 || a >= GN || b >= GN) continue;
        best = std::min(best, double(escD[a][b]));
    }
    if (best > 1e8) return 30;
    // straight-line part is already covered by edgePen; only charge the detour beyond it
    return best;
}

int g_alive = 8;
double edgePen(double x, double y, double h) {
    double M = std::max(1.0, std::min(5.5, h * 0.5));
    auto g = [&](double m) {
        if (m < 0) return 3000.0 + 500 * (-m);
        if (m < M) { double d = (M - m) / M; return 320.0 * (1.0 + (g_alive <= 4 ? std::max(0.0, (9.0 - h) / 3.0) : 0.0)) * d * d; }
        return 0.0;
    };
    return g(h - std::fabs(x)) + g(h - std::fabs(y));
}

double landingDanger(double px, double py, double qx, double qy, double k, double h) {
    double dx = px - qx, dy = py - qy, d = std::sqrt(dx * dx + dy * dy);
    double ux, uy;
    if (d < 1e-6) { double l = std::sqrt(px * px + py * py); if (l < 1e-6) { ux = 1; uy = 0; } else { ux = px / l; uy = py / l; } }
    else { ux = dx / d; uy = dy / d; }
    double s = slide(k);
    double lx = px + ux * s, ly = py + uy * s;
    double m = h - std::max(std::fabs(lx), std::fabs(ly));
    if (m < 0) return 1.0 + std::min(1.0, -m / 4);
    return std::max(0.0, 1 - m / 3) * 0.35;
}

double THREAT_W6 = 1.0, THREAT_W12 = 0.7, CHARGE_V = 80, DASH_W = 1000, CENTER_W = 0.12, BOMBHAM_W = 300, JAB_W = 450, BOMB_W = 500, QUICK_W = 0.6, PRESS_W = 70;
double threatAt(const P& p, double mx, double my, double qx, double qy, double h, double appr) {
    double dx = mx - qx, dy = my - qy, d = std::sqrt(dx * dx + dy * dy);
    double T = 0;
    double ch = 0, wgt = 1.0;
    if (p.skill == 2 && p.charge > 0) ch = std::min(1.0, p.charge + std::min(p.bar, 0.12));
    if (ch <= 0 && p.hammer_cd <= 12 && p.bar >= 0.2 && p.act == 0) { ch = std::min(p.bar, 0.24 * appr); wgt = QUICK_W; }
    if (ch > 0) {
        double R = HAM_REACH + 0.3 + appr;
        if (d < R) {
            double prox = d <= HAM_REACH + 0.3 ? 1.0 : (R - d) / (R - HAM_REACH - 0.3);
            T += 900 * wgt * prox * landingDanger(mx, my, qx, qy, HAM_POW * ch, h);
        }
    }
    if (p.skill == 1 && p.charge >= 0.15 && p.act == 0) {
        double ux = 0, uy = 0;
        if (d > 1e-6) { ux = dx / d; uy = dy / d; }
        double c = std::min(1.0, p.charge + std::min(p.bar, 0.12 * appr));
        double v0 = DASH_SPEED * c + std::max(0.0, p.vx * ux + p.vy * uy);
        double va = v0 - 0.064 * std::max(0.0, d - 2.0);
        if (va > 0.1) {
            double k = 0.95 * va;
            double prox = d < 5 ? 1.0 : std::max(0.3, 1.0 - (d - 5) * 0.12);
            T += DASH_W * prox * landingDanger(mx, my, qx, qy, k, h);
        }
    }
    if (p.bomb_cd <= 6 * appr && (p.act == 0 || p.act == 2) && d < 7.0 && d > 1.9) {
        double vb = std::max(0.0, THROW - (d - 1.8) * BOMB_F / (1 - BOMB_F));
        double k = (1 + REST) * vb / (1 + 1 / BOMB_M);
        if (k > 0.08) T += 250 * landingDanger(mx, my, qx, qy, k, h);
    }
    if (p.attack_cd <= 6 && (p.act == 0 || p.act == 2)) {
        double R = ATT_REACH + appr;
        if (d < R) T += JAB_W * ((R - d) / R) * landingDanger(mx, my, qx, qy, ATT_POW, h);
    }
    return T;
}

double evaluate(const World& w, const Order& o, const SimOut& r, double chargeAfter, bool overDue) {
    const P& mp = w.ps[w.me];
    double S = 0;
    if (r.meOut) return -100000 + r.meOutT * 100;
    int endTick = w.tick + 6;
    double h6 = w.halfAt(endTick);
    double hs = h6;
    if (w.hasNext) hs = std::min(h6, w.nextHalf + 1.0);
    S -= edgePen(r.mx, r.my, hs);
    S -= 0.6 * edgePen(r.ex, r.ey, w.halfAt(w.tick + H));
    if (r.minMargin < 1.0) S -= (1.0 - r.minMargin) * 200;
    S -= CENTER_W * (r.mx * r.mx + r.my * r.my) / std::max(1.0, w.half * w.half / 72.25);
    {
        double hT = (w.hasNext ? w.nextHalf : w.half) - 1.0;
        double straight = std::max(0.0, std::max(std::fabs(r.mx), std::fabs(r.my)) - std::max(0.3, hT));
        double det = escapeDist(r.mx, r.my) - straight;
        S -= escW * std::max(0.0, det) + 0.3 * escW * straight;
    }
    int alive = 0;
    for (int i = 0; i < w.n; ++i) if (!w.ps[i].out && i != w.me) alive++;
    for (int i = 0; i < w.n; ++i) {
        if (i == w.me || w.ps[i].out) continue;
        if (r.fell[i]) { S += r.fellMine[i] ? 700 : 150; continue; }
        {
            double hE = w.halfAt(w.tick + H);
            double mE = hE - std::max(std::fabs(r.oxe[i]), std::fabs(r.oye[i]));
            if (mE < 2.5) S += PRESS_W * (2.5 - std::max(0.0, mE)) / 2.5;
        }
        const P& p = w.ps[i];
        double qx = r.ox[i], qy = r.oy[i];
        double dx = r.mx - qx, dy = r.my - qy, d = std::sqrt(dx * dx + dy * dy);
        double qx12 = r.ox12[i], qy12 = r.oy12[i];
        double hl = w.halfAt(w.tick + 36);
        S -= THREAT_W6 * threatAt(p, r.mx, r.my, qx, qy, hl, 1.0);
        S -= THREAT_W12 * threatAt(p, r.mx12, r.my12, qx12, qy12, hl, 1.8);
        // generic crowding
        if (d < 2.5) S -= (2.5 - d) * 8;
        // offense: being in a striking position
        if (chargeAfter >= 0.15 && d < HAM_REACH + 0.4) {
            double ld = landingDanger(qx, qy, r.mx, r.my, HAM_POW * chargeAfter, w.halfAt(w.tick + 36));
            S += 160 * ld;
        }
    }
    for (const Bm& b : w.bombs) {
        if (b.fuse <= 3) continue;
        for (int j = 0; j < w.n; ++j) {
            if (j == w.me || w.ps[j].out) continue;
            const P& q = w.ps[j];
            if (q.skill != 2 || q.charge < 0.3 || q.act != 0) continue;
            if (std::hypot(q.x - b.x, q.y - b.y) > HAM_REACH + 0.5) continue;
            double d = std::hypot(r.mx - b.x, r.my - b.y);
            double va = HAM_POW * q.charge / BOMB_M - 0.0526 * std::max(0.0, d - 1.6);
            if (va < 0.2 || d > 14) continue;
            double k = (1 + REST) * va / (1 + 1 / BOMB_M);
            S -= BOMBHAM_W * landingDanger(r.mx, r.my, b.x, b.y, k, w.halfAt(w.tick + 36));
        }
    }
    for (const auto& bx : r.blasts) {
        double dx = bx.mx - bx.bx, dy = bx.my - bx.by, d = std::sqrt(dx * dx + dy * dy);
        double R = BLAST_R + 1.5;
        if (d >= R) continue;
        double k = BLAST_P * std::max(0.15, 1 - std::max(0.0, d - 1.0) / BLAST_R);
        double prox = d < BLAST_R ? 1.0 : (R - d) / 1.5;
        double unc = std::min(1.0, 0.4 + bx.t / 30.0);
        S -= BOMB_W * prox * unc * landingDanger(bx.mx, bx.my, bx.bx, bx.by, k, w.halfAt(w.tick + bx.t + 30));
    }
    double cv = overDue ? 0 : CHARGE_V;
    S += cv * chargeAfter;
    if (alive == 0) S += 0;
    (void)mp; (void)o;
    return S;
}

void readRules(const J& r) {
    auto g = [&](const char* a, const char* b, double& v) {
        if (r.contains(a) && r.at(a).contains(b) && r.at(a).at(b).is_number()) v = r.at(a).at(b).as_double();
    };
    auto gi = [&](const char* a, const char* b, int& v) {
        if (r.contains(a) && r.at(a).contains(b) && r.at(a).at(b).is_number()) v = int(r.at(a).at(b).as_double());
    };
    g("player", "radius", RAD);
    g("hammer", "reach", HAM_REACH); g("hammer", "cone_cos", HAM_COS); g("hammer", "power", HAM_POW); g("hammer", "recoil", HAM_RECOIL);
    g("hammer", "charging_factor", HAM_CF); gi("hammer", "recover_ticks", HAM_RECOVER);
    g("attack", "reach", ATT_REACH); g("attack", "cone_cos", ATT_COS); g("attack", "power", ATT_POW); g("attack", "recoil", ATT_RECOIL);
    gi("attack", "windup_ticks", ATT_WINDUP); gi("attack", "recover_ticks", ATT_RECOVER);
    g("physics", "friction", FRIC); g("physics", "restitution", REST); g("physics", "min_speed", MIN_SPEED); g("physics", "min_impulse", MIN_IMP);
    g("move", "accel", ACCEL); g("move", "charging_factor", DASH_CF);
    g("bomb", "radius", BOMB_R); g("bomb", "mass", BOMB_M); g("bomb", "friction", BOMB_F); g("bomb", "blast_radius", BLAST_R);
    g("bomb", "blast_power", BLAST_P); g("bomb", "throw_speed", THROW); g("bomb", "spawn_gap", SPAWN_GAP);
    g("walls", "thickness", WALL_T); g("walls", "restitution", WALL_REST); g("walls", "regen_push", REGEN_PUSH);
    g("spring", "factor", SPRING_F); g("spring", "max_speed", SPRING_MAX);
    g("dash", "speed", DASH_SPEED); g("bar", "charge_rate", CHARGE_RATE); gi("bar", "overcharge_ticks", OVER_T);
}

P readP(const J& j) {
    P p;
    p.x = num(j, "x"); p.y = num(j, "y"); p.vx = num(j, "vx"); p.vy = num(j, "vy");
    p.bar = num(j, "bar"); p.charge = num(j, "charge");
    if (j.contains("skill") && j.at("skill").is_string()) { std::string s = j.at("skill").as_string(); p.skill = s == "dash" ? 1 : (s == "hammer" ? 2 : 0); }
    p.out = j.contains("out") && j.at("out").is_bool() && j.at("out").as_bool();
    if (j.contains("act") && j.at("act").is_string()) {
        std::string a = j.at("act").as_string();
        p.act = a == "jab" ? 1 : (a == "jab_recover" ? 2 : (a == "recover" ? 3 : 0));
    }
    p.act_ticks = int(num(j, "act_ticks"));
    if (j.contains("aim") && j.at("aim").is_array() && j.at("aim").size() == 2) { p.ax = j.at("aim").at(0).as_double(); p.ay = j.at("aim").at(1).as_double(); }
    p.bomb_cd = int(num(j, "bomb_cd")); p.attack_cd = int(num(j, "attack_cd")); p.hammer_cd = int(num(j, "hammer_cd"));
    p.held = int(num(j, "held")); p.pause = int(num(j, "pause"));
    return p;
}

bool inited = false;

J vec(double x, double y) { return J::array{x, y}; }

J decide(const J& obs) {
    auto t0 = std::chrono::steady_clock::now();
    if (!inited) { initSlide(); inited = true; }
    if (obs.contains("rules")) readRules(obs.at("rules"));
    World w;
    w.me = int(num(obs, "you")); w.n = int(num(obs, "n")); w.tick = int(num(obs, "tick")); w.turn = int(num(obs, "turn"));
    const J& ar = obs.at("arena");
    w.half = num(ar, "half", 16.5);
    if (ar.contains("next") && ar.at("next").is_object()) {
        w.hasNext = true; const J& nx = ar.at("next");
        w.nextHalf = num(nx, "half"); w.nextTurn = int(num(nx, "turn")); w.nextEnd = int(num(nx, "end_turn"));
    }
    for (const J& p : obs.at("players").as_array()) w.ps.push_back(readP(p));
    w.n = int(w.ps.size());
    if (obs.contains("bombs") && obs.at("bombs").is_array())
        for (const J& b : obs.at("bombs").as_array())
            w.bombs.push_back({num(b, "x"), num(b, "y"), num(b, "vx"), num(b, "vy"), int(num(b, "fuse")), int(num(b, "owner", -1))});
    if (obs.contains("walls") && obs.at("walls").is_array())
        for (const J& x : obs.at("walls").as_array())
            w.walls.push_back({num(x, "x1"), num(x, "y1"), num(x, "x2"), num(x, "y2"), x.contains("kind") && x.at("kind") == J("spring"), 0});
    if (obs.contains("broken") && obs.at("broken").is_array())
        for (const J& x : obs.at("broken").as_array())
            w.walls.push_back({num(x, "x1"), num(x, "y1"), num(x, "x2"), num(x, "y2"), false, int(num(x, "at"))});
    const P& mp = w.ps[w.me];
    if (mp.out) return J::object{};
    if (mp.act != 0) return J::object{};
    g_alive = 0;
    for (const P& q : w.ps) if (!q.out) g_alive++;
    buildEscape(w);

    std::vector<Order> cands;
    bool topUpOk = mp.hammer_cd <= 0 && mp.skill == 2 && mp.charge > 0 && mp.bar > 0.01 && mp.held >= OVER_T - 14;
    bool canCharge = (mp.hammer_cd <= 0 && mp.skill != 1 && mp.bar > 0.01 && mp.charge < 0.70) || topUpOk;
    bool mustTopUp = topUpOk;
    bool overDue = mp.skill == 2 && mp.charge > 0 && mp.held >= OVER_T - 7 && !canCharge;
    const int ND = 16;
    for (int k = -1; k < ND; ++k) {
        Order o;
        if (k >= 0) { o.move = true; o.mx = std::cos(2 * M_PI * k / ND); o.my = std::sin(2 * M_PI * k / ND); }
        if (!mustTopUp) { cands.push_back(o); if (k >= 0) { Order c2 = o; c2.cont = true; cands.push_back(c2); } }
        if (canCharge) { o.charge = true; cands.push_back(o); if (k >= 0) { Order c2 = o; c2.cont = true; cands.push_back(c2); } }
    }
    // hammer releases: toward opponents, bombs, and the nearest edge (recoil inward); with follow-up moves
    if (mp.skill == 2 && mp.charge > 0) {
        std::vector<std::pair<double,double>> dirs;
        for (int j = 0; j < w.n; ++j) {
            if (j == w.me || w.ps[j].out) continue;
            double dx = w.ps[j].x - mp.x, dy = w.ps[j].y - mp.y, d = std::sqrt(dx * dx + dy * dy);
            if (d > HAM_REACH + 0.01 || d < 1e-9) continue;
            dirs.push_back({dx / d, dy / d});
        }
        bool bombNear = false;
        for (const Bm& b : w.bombs) {
            double dx = b.x - mp.x, dy = b.y - mp.y, d = std::sqrt(dx * dx + dy * dy);
            if (d < 7) bombNear = true;
            if (d > HAM_REACH + 0.01 || d < 1e-9) continue;
            dirs.push_back({dx / d, dy / d});
        }
        bool edgeNear = w.half - std::max(std::fabs(mp.x), std::fabs(mp.y)) < 3.0;
        if (bombNear || edgeNear || overDue) {
            double ex = std::fabs(mp.x) > std::fabs(mp.y) ? (mp.x > 0 ? 1 : -1) : 0;
            double ey = ex == 0 ? (mp.y > 0 ? 1 : -1) : 0;
            dirs.push_back({ex, ey});
        }
        for (auto& dv : dirs) {
            for (int k = -1; k < 8; ++k) {
                Order o; o.hammer = true; o.hx = dv.first; o.hy = dv.second; o.cont = true;
                if (k >= 0) { o.move = true; o.mx = std::cos(2 * M_PI * k / 8); o.my = std::sin(2 * M_PI * k / 8); }
                cands.push_back(o);
            }
        }
    }
    // jabs (never while a charged hammer or a strong dash can reach us during the 15 immobile ticks)
    bool jabSafe = true;
    for (int j = 0; j < w.n; ++j) {
        if (j == w.me || w.ps[j].out) continue;
        const P& q = w.ps[j];
        double dj = std::hypot(q.x - mp.x, q.y - mp.y);
        if (q.skill == 2 && q.charge >= 0.15 && dj < HAM_REACH + 1.5) jabSafe = false;
        if (q.skill == 2 && q.charge > 0 && q.bar >= 0.2 && q.hammer_cd <= 12 && dj < HAM_REACH + 0.8) jabSafe = false;
        if (q.skill == 1 && q.charge >= 0.3 && dj < 6.0) jabSafe = false;
    }
    if (mp.attack_cd == 0 && jabSafe) {
        for (int j = 0; j < w.n; ++j) {
            if (j == w.me || w.ps[j].out) continue;
            const P& q = w.ps[j];
            double px = q.x + q.vx * 7, py = q.y + q.vy * 7;
            double dx = px - mp.x, dy = py - mp.y, d = std::sqrt(dx * dx + dy * dy);
            if (d > ATT_REACH + 1.0 || d < 1e-9) continue;
            Order o; o.jab = true; o.jx = dx / d; o.jy = dy / d;
            cands.push_back(o);
        }
    }
    if (mp.attack_cd == 0 && jabSafe) {
        for (const Bm& b : w.bombs) {
            double px = b.x + b.vx * 7, py = b.y + b.vy * 7;
            double dx = px - mp.x, dy = py - mp.y, d = std::sqrt(dx * dx + dy * dy);
            if (d > ATT_REACH + 0.5 || d < 1e-9 || b.fuse < 10) continue;
            Order o; o.jab = true; o.jx = dx / d; o.jy = dy / d;
            cands.push_back(o);
        }
    }
    // bombs
    if (mp.bomb_cd == 0) {
        for (int j = 0; j < w.n; ++j) {
            if (j == w.me || w.ps[j].out) continue;
            const P& q = w.ps[j];
            double dx = q.x - mp.x, dy = q.y - mp.y, d = std::sqrt(dx * dx + dy * dy);
            if (d > 9 || d < 2.5) continue;
            for (int f : {24, 30, 38}) {
                Order o; o.bomb = true; o.bx = dx / d; o.by = dy / d; o.fuse = f;
                cands.push_back(o);
            }
        }
    }
    double best = -1e18; Order bo; double baseNoBomb = -1e18;
    std::vector<double> scores(cands.size());
    for (size_t c = 0; c < cands.size(); ++c) {
        if ((c & 7) == 7) {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el > 0.035) break;
        }
        const Order& o = cands[c];
        SimOut r = simulate(w, o);
        double chargeAfter = mp.skill == 2 ? mp.charge : 0;
        if (o.charge) chargeAfter = std::min(1.0, (mp.skill == 2 ? mp.charge : 0) + std::min(mp.bar, 0.24));
        if (o.hammer) chargeAfter = 0;
        double sc = evaluate(w, o, r, chargeAfter, overDue);
        if (o.bomb) {
            // only count bomb benefit from kills; discount since targets dodge
            int k = 0; for (int i = 0; i < w.n; ++i) k += r.fellMine[i];
            sc -= 20;
            if (k == 0) sc -= 30;
            else sc -= 700 * k * 0.6;
        }
        if (o.jab) {
            sc -= 10;
            double hl = w.halfAt(w.tick + 30);
            for (int i = 0; i < w.n; ++i) {
                if (i == w.me || w.ps[i].out || r.fell[i]) continue;
                const P& q = w.ps[i];
                if (q.attack_cd > 9 || q.act == 3) continue;
                double dx = mp.x - q.x, dy = mp.y - q.y, d = std::sqrt(dx * dx + dy * dy);
                if (d < ATT_REACH + 0.8) sc -= 500 * landingDanger(mp.x, mp.y, q.x, q.y, ATT_POW, hl);
                if (q.skill == 2 && q.charge > 0 && d < HAM_REACH + 1.0) sc -= 700 * landingDanger(mp.x, mp.y, q.x, q.y, HAM_POW * q.charge, hl);
            }
            for (int i = 0; i < w.n; ++i) if (r.fellMine[i]) {
                const P& q = w.ps[i];
                bool slow = q.act != 0 || q.charge > 0;
                if (!slow) sc -= 700 * 0.55;
            }
        }
        scores[c] = sc;
        if (sc > best) { best = sc; bo = o; }
        if (!o.bomb) baseNoBomb = std::max(baseNoBomb, sc);
    }
    J order = J::object{};
    if (bo.move) order["move"] = vec(bo.mx, bo.my);
    if (bo.charge) order["charge"] = "hammer";
    if (bo.hammer) order["hammer"] = vec(bo.hx, bo.hy);
    if (bo.jab) order["attack"] = vec(bo.jx, bo.jy);
    if (bo.bomb) order["bomb"] = J{{"dir", vec(bo.bx, bo.by)}, {"fuse", bo.fuse}};
    std::cerr << "t" << w.turn << " best " << best << " nc " << cands.size();
    return order;
}
}  // namespace

int main() {
    return kb_step_worker::run([](const J& obs, J& memory) {
        J order = decide(obs);
        return std::make_pair(order, memory);
    });
}
