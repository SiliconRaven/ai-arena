#pragma once
// Fast struct-based mirror of knockback::Engine (native/include/engine.hpp) for forward simulation.
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace kbs {
using J = mini_json::Json;

struct Phase { int warn, start, end; double from, to; };
struct Rules {
    bool loaded = false;
    int D = 6, max_turns = 200;
    double half0 = 16.5, rad = 1.0, fr = 0.06, rest = 0.9, minspeed = 0.001, minimp = 0.03;
    double accel = 0.018, cf = 0.2, crate = 0.04, refill = 0.02;
    int pause_t = 30, overch = 45;
    double dashs = 0.9;
    double hcf = 0.3, hreach = 3.2, hcos = 0.5, hpow = 0.9, hrec = 0.15;
    int hrect = 5, hcd = 60;
    int awind = 9;
    double areach = 2.8, acos_ = 0.5, apow = 0.35, arec = 0.05;
    int arect = 6, acd = 15;
    double brad = 0.6, bmass = 0.5, bfr = 0.05, bthrow = 0.5, bgap = 0.2;
    int bfmin = 24, bfmax = 90, bcd = 90;
    double bblast = 3.5, bpow = 1.4;
    double wth = 0.5, wrest = 0.8, wpush = 0.5, sfac = 1.5, smax = 0.5;
    int regen = 300, wwarn = 90, kcred = 90;
    std::vector<Phase> ph;
};
inline Rules R;

inline double num(const J& j, const char* k) { return j[k].as_double(); }
inline void loadRules(const J& r) {
    R.D = (int)r["decision_ticks"].as_int();
    R.max_turns = (int)r["max_turns"].as_int();
    R.half0 = num(r["arena"], "half");
    R.rad = num(r["player"], "radius");
    R.fr = num(r["physics"], "friction"); R.rest = num(r["physics"], "restitution");
    R.minspeed = num(r["physics"], "min_speed"); R.minimp = num(r["physics"], "min_impulse");
    R.accel = num(r["move"], "accel"); R.cf = num(r["move"], "charging_factor");
    R.crate = num(r["bar"], "charge_rate"); R.refill = num(r["bar"], "refill_rate");
    R.pause_t = (int)r["bar"]["pause_ticks"].as_int(); R.overch = (int)r["bar"]["overcharge_ticks"].as_int();
    R.dashs = num(r["dash"], "speed");
    R.hcf = num(r["hammer"], "charging_factor"); R.hreach = num(r["hammer"], "reach"); R.hcos = num(r["hammer"], "cone_cos");
    R.hpow = num(r["hammer"], "power"); R.hrec = num(r["hammer"], "recoil");
    R.hrect = (int)r["hammer"]["recover_ticks"].as_int(); R.hcd = (int)r["hammer"]["cooldown_ticks"].as_int();
    R.awind = (int)r["attack"]["windup_ticks"].as_int();
    R.areach = num(r["attack"], "reach"); R.acos_ = num(r["attack"], "cone_cos"); R.apow = num(r["attack"], "power"); R.arec = num(r["attack"], "recoil");
    R.arect = (int)r["attack"]["recover_ticks"].as_int(); R.acd = (int)r["attack"]["cooldown_ticks"].as_int();
    R.brad = num(r["bomb"], "radius"); R.bmass = num(r["bomb"], "mass"); R.bfr = num(r["bomb"], "friction");
    R.bthrow = num(r["bomb"], "throw_speed"); R.bgap = num(r["bomb"], "spawn_gap");
    R.bfmin = (int)r["bomb"]["fuse_min"].as_int(); R.bfmax = (int)r["bomb"]["fuse_max"].as_int(); R.bcd = (int)r["bomb"]["cooldown_ticks"].as_int();
    R.bblast = num(r["bomb"], "blast_radius"); R.bpow = num(r["bomb"], "blast_power");
    R.wth = num(r["walls"], "thickness"); R.wrest = num(r["walls"], "restitution");
    R.regen = (int)r["walls"]["regen_ticks"].as_int(); R.wwarn = (int)r["walls"]["warn_ticks"].as_int(); R.wpush = num(r["walls"], "regen_push");
    R.sfac = num(r["spring"], "factor"); R.smax = num(r["spring"], "max_speed");
    R.kcred = (int)r["kill_credit_ticks"].as_int();
    // shrink phases (mirror of setup_phases)
    R.ph.clear();
    const J& s = r["shrink"];
    double half = R.half0, minh = num(s, "min_half"), step = num(s, "step");
    int first = (int)s["first_turn"].as_int(), every = (int)s["every"].as_int(), warn = (int)s["warn_turns"].as_int(), sturns = (int)s["shrink_turns"].as_int();
    int k = 0;
    while (half > minh) {
        int start = first + k * every;
        if (start > R.max_turns) break;
        double target = std::max(minh, R.half0 - (k + 1) * step);
        R.ph.push_back({std::max(1, start - warn), start, start + sturns - 1, half, target});
        half = target; ++k;
    }
    R.loaded = true;
}
inline double halfAt(int t) {  // arena half after tick t has been resolved
    double h = R.half0;
    for (auto& p : R.ph) {
        int st = (p.start - 1) * R.D + 1, en = p.end * R.D;
        if (t >= en) h = p.to;
        else if (t >= st) { h = p.from + (p.to - p.from) * (t - st + 1) / (double)(en - st + 1); break; }
        else break;
    }
    return h;
}

inline double rnd4(double v) { return std::nearbyint(v * 10000.0) / 10000.0; }
inline bool unitv(double dx, double dy, double& ux, double& uy) {
    double big = std::max(std::fabs(dx), std::fabs(dy));
    if (big == 0) return false;
    dx /= big; dy /= big;
    double l = std::sqrt(dx * dx + dy * dy);
    ux = dx / l; uy = dy / l; return true;
}

enum Skill { SK_NONE = 0, SK_DASH = 1, SK_HAMMER = 2 };
enum Act { ACT_NONE = 0, ACT_JAB = 1, ACT_JABREC = 2, ACT_REC = 3 };

struct Pl {
    double x = 0, y = 0, vx = 0, vy = 0, bar = 1, charge = 0;
    int skill = 0, pause = 0, act = 0, act_ticks = 0;
    double ax = 0, ay = 0;
    int bomb_cd = 0, attack_cd = 0, hammer_cd = 0, held = 0;
    bool out = false;
    int lh_by = -1, lh_tick = 0;
    int out_tick = -1, out_by = -1;  // out_by: killer seat or -1
    int kills = 0;
};
struct Bm { double x, y, vx, vy; int fuse, owner; };
struct Wl { double x1, y1, x2, y2; bool spring = false; bool present = false; bool up = false; int back_at = -1; };
struct Ord {
    bool mv = false; double mx = 0, my = 0;
    int ch = 0;
    bool hd = false; double dx = 0, dy = 0;
    bool hh = false; double hx = 0, hy = 0;
    bool bo = false; double bx = 0, by = 0; int fuse = 30;
    bool at = false; double ax = 0, ay = 0;
};
inline void setMove(Ord& o, double dx, double dy) { double ux, uy; if (unitv(dx, dy, ux, uy)) { o.mv = true; o.mx = ux; o.my = uy; } else o.mv = false; }
inline void setDash(Ord& o, double dx, double dy) { double ux, uy; if (unitv(dx, dy, ux, uy)) { o.hd = true; o.dx = ux; o.dy = uy; } }
inline void setHammer(Ord& o, double dx, double dy) { double ux, uy; if (unitv(dx, dy, ux, uy)) { o.hh = true; o.hx = ux; o.hy = uy; } }
inline void setAttack(Ord& o, double dx, double dy) { double ux, uy; if (unitv(dx, dy, ux, uy)) { o.at = true; o.ax = ux; o.ay = uy; } }
inline void setBomb(Ord& o, double dx, double dy, int fuse) { double ux, uy; if (unitv(dx, dy, ux, uy)) { o.bo = true; o.bx = ux; o.by = uy; o.fuse = fuse; } }

struct St {
    int n = 0, tick = 0;
    double half = 16.5;
    Pl p[8];
    int nb = 0;
    Bm b[40];
    Wl w[16];
    int nw = 0;
};

inline double pairc(double& ax, double& ay, double& avx, double& avy, double& bx, double& by, double& bvx, double& bvy,
                    double reach, double ma, double mb, double e) {
    double dx = bx - ax, dy = by - ay;
    double d2 = dx * dx + dy * dy;
    if (d2 >= reach * reach) return 0.0;
    double d = std::sqrt(d2);
    double nx = d > 0 ? dx / d : 1.0, ny = d > 0 ? dy / d : 0.0;
    double share = mb / (ma + mb);
    ax -= nx * (reach - d) * share; ay -= ny * (reach - d) * share;
    bx += nx * (reach - d) * (1 - share); by += ny * (reach - d) * (1 - share);
    double approach = (avx - bvx) * nx + (avy - bvy) * ny;
    if (approach <= 0) return 0.0;
    double imp = (1 + e) * approach / (1 / ma + 1 / mb);
    avx -= imp / ma * nx; avy -= imp / ma * ny; bvx += imp / mb * nx; bvy += imp / mb * ny;
    return imp;
}
inline void nearestPt(double px, double py, const Wl& w, double& qx, double& qy) {
    double ax = w.x1, ay = w.y1, ex = w.x2 - ax, ey = w.y2 - ay, l = ex * ex + ey * ey;
    double t = l == 0 ? 0 : std::max(0., std::min(1., ((px - ax) * ex + (py - ay) * ey) / l));
    qx = ax + t * ex; qy = ay + t * ey;
}
inline void bounceWall(double& x, double& y, double& vx, double& vy, double radius, const Wl& w) {
    double reach = radius + R.wth / 2;
    double mnx = std::min(w.x1, w.x2) - reach - 1e-9, mxx = std::max(w.x1, w.x2) + reach + 1e-9;
    double mny = std::min(w.y1, w.y2) - reach - 1e-9, mxy = std::max(w.y1, w.y2) + reach + 1e-9;
    if (x < mnx || x > mxx || y < mny || y > mxy) return;
    double qx, qy; nearestPt(x, y, w, qx, qy);
    double dx = x - qx, dy = y - qy, d2 = dx * dx + dy * dy;
    if (d2 >= reach * reach) return;
    double nx, ny;
    if (d2 > 0) { double d = std::sqrt(d2); nx = dx / d; ny = dy / d; }
    else { if (!unitv(-(w.y2 - w.y1), w.x2 - w.x1, nx, ny)) { nx = 1; ny = 0; } }
    x = qx + nx * reach; y = qy + ny * reach;
    double inward = vx * nx + vy * ny;
    if (inward >= 0) return;
    double speed = w.spring ? std::min(R.sfac * -inward, R.smax) : R.wrest * -inward;
    vx += (speed - inward) * nx; vy += (speed - inward) * ny;
}

inline void setOut(St& s, int seat, int tick, bool kill_ok) {
    Pl& p = s.p[seat];
    p.out = true; p.vx = 0; p.vy = 0; p.act = 0; p.act_ticks = 0; p.out_tick = tick;
    if (kill_ok && p.lh_by >= 0 && p.lh_by != seat && tick - p.lh_tick <= R.kcred) {
        p.out_by = p.lh_by; s.p[p.lh_by].kills++;
    } else p.out_by = -1;
}

inline void swing(St& s, int seat, int tick, const int* a, int na, bool hammer, double ax, double ay, double scale) {
    Pl& p = s.p[seat];
    double reach = hammer ? R.hreach : R.areach, gain = hammer ? R.hpow : R.apow, cs = hammer ? R.hcos : R.acos_;
    double recoil = hammer ? R.hrec : R.arec;
    for (int k = 0; k < na; ++k) {
        int i = a[k]; if (i == seat || s.p[i].out) continue;
        Pl& q = s.p[i];
        double dx = q.x - p.x, dy = q.y - p.y;
        if (dx * dx + dy * dy > reach * reach) continue;
        double ux, uy; if (!unitv(dx, dy, ux, uy)) { ux = ax; uy = ay; }
        if (ux * ax + uy * ay < cs) continue;
        q.vx += ux * gain * scale; q.vy += uy * gain * scale;
        q.lh_by = seat; q.lh_tick = tick;
    }
    for (int k = 0; k < s.nb; ++k) {
        Bm& q = s.b[k];
        double dx = q.x - p.x, dy = q.y - p.y;
        if (dx * dx + dy * dy > reach * reach) continue;
        double ux, uy; if (!unitv(dx, dy, ux, uy)) { ux = ax; uy = ay; }
        if (ux * ax + uy * ay < cs) continue;
        q.vx += ux * gain * scale / R.bmass; q.vy += uy * gain * scale / R.bmass;
        q.owner = seat;
    }
    p.vx -= ax * recoil * scale; p.vy -= ay * recoil * scale;
}

inline void actTick(St& s, int seat, const Ord& o, bool first, int tick, const int* a, int na) {
    Pl& p = s.p[seat];
    p.attack_cd = std::max(0, p.attack_cd - 1); p.hammer_cd = std::max(0, p.hammer_cd - 1);
    bool busy = p.act != 0;
    if (busy) {
        p.act_ticks -= 1;
        if (p.act_ticks <= 0) {
            if (p.act == ACT_JAB) {
                swing(s, seat, tick, a, na, false, p.ax, p.ay, 1.0);
                p.act = ACT_JABREC; p.act_ticks = R.arect;
            }
            if ((p.act == ACT_REC || p.act == ACT_JABREC) && p.act_ticks <= 0) { p.act = 0; p.act_ticks = 0; }
        }
    }
    int want = o.ch, skill = p.skill;
    if (want == SK_HAMMER && p.hammer_cd > 0) want = 0;
    bool release = !busy && first && skill != 0 && p.charge > 0 && ((skill == SK_DASH && o.hd) || (skill == SK_HAMMER && o.hh));
    if (!busy && !release && want != 0 && skill != 0 && skill != want) { p.charge = 0; p.skill = 0; }
    bool jab = first && !busy && o.at && p.attack_cd == 0 && !(release && skill == SK_HAMMER);
    bool charging = !busy && !release && !jab && want != 0 && p.bar > 0;
    if (p.pause > 0) p.pause -= 1;
    else if (!charging) p.bar = std::max(p.bar, std::min(1. - p.charge, p.bar + R.refill));
    if (release) {
        double power = p.charge;
        p.charge = 0; p.skill = 0; p.pause = R.pause_t;
        if (skill == SK_DASH) { p.vx += o.dx * R.dashs * power; p.vy += o.dy * R.dashs * power; }
        else {
            p.hammer_cd = R.hcd;
            swing(s, seat, tick, a, na, true, o.hx, o.hy, power);
            p.act = ACT_REC; p.act_ticks = R.hrect;
        }
    }
    if (jab) { p.act = ACT_JAB; p.act_ticks = R.awind; p.ax = o.ax; p.ay = o.ay; p.attack_cd = R.acd; }
    if (charging) {
        double amount = std::min(R.crate, p.bar);
        p.bar -= amount; p.charge += amount; p.skill = want;
    }
    if (p.charge <= 0 || charging) p.held = 0;
    else {
        p.held += 1;
        if (p.held >= R.overch) { p.charge = 0; p.skill = 0; p.held = 0; p.pause = R.pause_t; }
    }
    if (o.mv && !busy && p.act == 0) {
        double f = p.skill == SK_DASH ? R.cf : p.skill == SK_HAMMER ? R.hcf : 1.0;
        p.vx += o.mx * R.accel * f; p.vy += o.my * R.accel * f;
    }
    if (p.bomb_cd > 0) p.bomb_cd -= 1;
    else if (first && o.bo && !busy && p.act == 0) {
        if (s.nb < 40) {
            double gap = R.rad + R.brad + R.bgap;
            s.b[s.nb++] = Bm{p.x + o.bx * gap, p.y + o.by * gap, p.vx + o.bx * R.bthrow, p.vy + o.by * R.bthrow, o.fuse, seat};
        }
        p.bomb_cd = R.bcd;
    }
}

inline void explode(St& s, const Bm& bomb, int tick, const int* a, int na) {
    double reach = R.bblast;
    for (int k = 0; k < na; ++k) {
        int i = a[k]; Pl& q = s.p[i]; if (q.out) continue;
        double dx = q.x - bomb.x, dy = q.y - bomb.y, d2 = dx * dx + dy * dy;
        if (d2 > reach * reach) continue;
        double ux, uy; if (!unitv(dx, dy, ux, uy)) { ux = 1; uy = 0; }
        double gain = R.bpow * (1 - std::sqrt(d2) / reach);
        q.vx += ux * gain; q.vy += uy * gain;
        q.lh_by = bomb.owner; q.lh_tick = tick;
    }
    for (int k = 0; k < s.nb; ++k) {
        Bm& q = s.b[k];
        double dx = q.x - bomb.x, dy = q.y - bomb.y, d2 = dx * dx + dy * dy;
        if (d2 > reach * reach) continue;
        double ux, uy; if (!unitv(dx, dy, ux, uy)) { ux = 1; uy = 0; }
        double gain = R.bpow * (1 - std::sqrt(d2) / reach) / R.bmass;
        q.vx += ux * gain; q.vy += uy * gain;
    }
    for (int i = 0; i < s.nw; ++i) {
        Wl& w = s.w[i];
        if (!w.present || !w.up || w.spring) continue;
        double qx, qy; nearestPt(bomb.x, bomb.y, w, qx, qy);
        double dx = qx - bomb.x, dy = qy - bomb.y;
        if (dx * dx + dy * dy <= reach * reach) { w.up = false; w.back_at = tick + R.regen; }
    }
}

inline void regrow(St& s, Wl& w, const int* a, int na) {
    w.up = true; w.back_at = -1;
    auto push = [&](double& x, double& y, double& vx, double& vy, double radius) {
        double reach = radius + R.wth / 2;
        double qx, qy; nearestPt(x, y, w, qx, qy);
        double dx = x - qx, dy = y - qy;
        if (dx * dx + dy * dy >= reach * reach) return;
        double ux, uy;
        if (!unitv(dx, dy, ux, uy)) { if (!unitv(-(w.y2 - w.y1), w.x2 - w.x1, ux, uy)) { ux = 1; uy = 0; } }
        x = qx + ux * reach; y = qy + uy * reach;
        double normal = vx * ux + vy * uy;
        vx += (R.wpush - normal) * ux; vy += (R.wpush - normal) * uy;
    };
    for (int k = 0; k < na; ++k) { Pl& q = s.p[a[k]]; if (!q.out) push(q.x, q.y, q.vx, q.vy, R.rad); }
    for (int k = 0; k < s.nb; ++k) push(s.b[k].x, s.b[k].y, s.b[k].vx, s.b[k].vy, R.brad);
}

inline bool wallOff(const Wl& w, double half) {
    return std::max({std::fabs(w.x1), std::fabs(w.y1), std::fabs(w.x2), std::fabs(w.y2)}) > half;
}

inline void frictionBody(double& x, double& y, double& vx, double& vy, double fr) {
    double keep = 1 - fr, ovx = vx, ovy = vy;
    vx = ovx * keep; vy = ovy * keep;
    if (vx * vx + vy * vy < R.minspeed * R.minspeed) { vx = 0; vy = 0; }
    x = rnd4(x) + 0.; y = rnd4(y) + 0.; vx = rnd4(vx) + 0.; vy = rnd4(vy) + 0.;
    if (keep < 1 && vx == ovx) vx = 0;
    if (keep < 1 && vy == ovy) vy = 0;
}

// One tick. `orders` indexed by seat. first = first tick of a turn.
inline void tick(St& s, const Ord* orders, bool first) {
    int t = s.tick + 1;
    for (auto& p : R.ph) {
        int st = (p.start - 1) * R.D + 1, en = p.end * R.D;
        if (st <= t && t <= en) s.half = rnd4(p.from + (p.to - p.from) * (t - st + 1) / (en - st + 1));
    }
    for (int i = 0; i < s.nw; ++i) {
        Wl& w = s.w[i]; if (!w.present) continue;
        if (w.up) { if (wallOff(w, s.half)) { w.up = false; w.present = false; } }
        else if (w.back_at >= 0 && wallOff(w, s.half)) { w.present = false; w.back_at = -1; }
    }
    int a[8], na = 0;
    for (int i = 0; i < s.n; ++i) if (!s.p[i].out) a[na++] = i;
    for (int i = 0; i < s.nw; ++i) {
        Wl& w = s.w[i];
        if (w.present && !w.up && w.back_at >= 0 && t >= w.back_at) regrow(s, w, a, na);
    }
    for (int k = 0; k < s.nb;) {
        s.b[k].fuse -= 1;
        if (s.b[k].fuse <= 0) {
            Bm dead = s.b[k];
            for (int j = k; j + 1 < s.nb; ++j) s.b[j] = s.b[j + 1];
            --s.nb;
            explode(s, dead, t, a, na);
        } else ++k;
    }
    static const Ord none{};
    for (int k = 0; k < na; ++k) actTick(s, a[k], orders ? orders[a[k]] : none, first, t, a, na);
    for (int k = 0; k < na; ++k) { Pl& p = s.p[a[k]]; p.x += p.vx; p.y += p.vy; }
    for (int k = 0; k < s.nb; ++k) { s.b[k].x += s.b[k].vx; s.b[k].y += s.b[k].vy; }
    // collide
    double sr = R.rad, br = R.brad;
    for (int k = 0; k < na; ++k) for (int l = k + 1; l < na; ++l) {
        Pl& A = s.p[a[k]]; Pl& B = s.p[a[l]];
        double imp = pairc(A.x, A.y, A.vx, A.vy, B.x, B.y, B.vx, B.vy, sr + sr, 1., 1., R.rest);
        if (imp >= R.minimp) { A.lh_by = a[l]; A.lh_tick = t; B.lh_by = a[k]; B.lh_tick = t; }
    }
    for (int k = 0; k < na; ++k) {
        Pl& p = s.p[a[k]];
        for (int j = 0; j < s.nb; ++j) {
            Bm& b = s.b[j];
            double nx = b.x - p.x, ny = b.y - p.y;
            bool hitter = (p.vx + b.vx) * nx + (p.vy + b.vy) * ny > 0;
            double imp = pairc(p.x, p.y, p.vx, p.vy, b.x, b.y, b.vx, b.vy, sr + br, 1., R.bmass, R.rest);
            if (imp >= R.minimp && hitter) b.owner = a[k];
        }
    }
    for (int k = 0; k < s.nb; ++k) for (int l = k + 1; l < s.nb; ++l) {
        Bm& A = s.b[k]; Bm& B = s.b[l];
        pairc(A.x, A.y, A.vx, A.vy, B.x, B.y, B.vx, B.vy, br + br, R.bmass, R.bmass, R.rest);
    }
    for (int i = 0; i < s.nw; ++i) {
        const Wl& w = s.w[i]; if (!w.present || !w.up) continue;
        for (int k = 0; k < na; ++k) { Pl& p = s.p[a[k]]; bounceWall(p.x, p.y, p.vx, p.vy, sr, w); }
    }
    // (engine order is per body over walls; walls are independent unless two walls overlap a body — equivalent here)
    for (int k = 0; k < s.nb; ++k) {
        for (int i = 0; i < s.nw; ++i) { const Wl& w = s.w[i]; if (!w.present || !w.up) continue; bounceWall(s.b[k].x, s.b[k].y, s.b[k].vx, s.b[k].vy, br, w); }
    }
    for (int k = 0; k < na; ++k) {
        Pl& p = s.p[a[k]];
        frictionBody(p.x, p.y, p.vx, p.vy, R.fr);
        p.bar = rnd4(p.bar) + 0.; p.charge = rnd4(p.charge) + 0.;
    }
    for (int k = 0; k < s.nb; ++k) frictionBody(s.b[k].x, s.b[k].y, s.b[k].vx, s.b[k].vy, R.bfr);
    for (int k = 0; k < na; ++k) {
        Pl& p = s.p[a[k]];
        if (std::max(std::fabs(p.x), std::fabs(p.y)) > s.half) setOut(s, a[k], t, true);
    }
    for (int k = 0; k < s.nb;) {
        if (std::max(std::fabs(s.b[k].x), std::fabs(s.b[k].y)) > s.half) {
            for (int j = k; j + 1 < s.nb; ++j) s.b[j] = s.b[j + 1];
            --s.nb;
        } else ++k;
    }
    s.tick = t;
}

inline int aliveCount(const St& s) { int c = 0; for (int i = 0; i < s.n; ++i) if (!s.p[i].out) ++c; return c; }

// ---- observation parsing ----
inline St parseObs(const J& obs) {
    St s;
    s.n = (int)obs["n"].as_int(); s.tick = (int)obs["tick"].as_int();
    s.half = num(obs["arena"], "half");
    const auto& ps = obs["players"].as_array();
    const auto& lh = obs["last_hit"].as_array();
    const auto& ks = obs["kills"].as_array();
    for (int i = 0; i < s.n && i < 8; ++i) {
        const J& j = ps[i]; Pl& q = s.p[i];
        q.x = num(j, "x"); q.y = num(j, "y"); q.vx = num(j, "vx"); q.vy = num(j, "vy");
        q.bar = num(j, "bar"); q.charge = num(j, "charge");
        q.skill = j["skill"].is_null() ? 0 : (j["skill"].as_string() == "dash" ? SK_DASH : SK_HAMMER);
        q.pause = (int)j["pause"].as_int(); q.out = j["out"].as_bool();
        if (j["act"].is_null()) q.act = 0;
        else { std::string a = j["act"].as_string(); q.act = a == "jab" ? ACT_JAB : a == "jab_recover" ? ACT_JABREC : ACT_REC; }
        q.act_ticks = (int)j["act_ticks"].as_int();
        if (!j["aim"].is_null()) { q.ax = j["aim"][0].as_double(); q.ay = j["aim"][1].as_double(); }
        q.bomb_cd = (int)j["bomb_cd"].as_int(); q.attack_cd = (int)j["attack_cd"].as_int(); q.hammer_cd = (int)j["hammer_cd"].as_int();
        q.held = (int)j["held"].as_int();
        if (!lh[i].is_null()) { q.lh_by = (int)lh[i][0].as_int(); q.lh_tick = (int)lh[i][1].as_int(); }
        q.kills = (int)ks[i].as_int();
    }
    for (const J& b : obs["bombs"].as_array()) {
        if (s.nb >= 40) break;
        s.b[s.nb++] = Bm{num(b, "x"), num(b, "y"), num(b, "vx"), num(b, "vy"), (int)b["fuse"].as_int(), (int)b["owner"].as_int()};
    }
    for (const J& w : obs["walls"].as_array()) {
        int id = (int)w["id"].as_int(); if (id < 0 || id >= 16) continue;
        Wl& q = s.w[id]; q.x1 = num(w, "x1"); q.y1 = num(w, "y1"); q.x2 = num(w, "x2"); q.y2 = num(w, "y2");
        q.spring = w["kind"].as_string() == "spring"; q.present = true; q.up = true; q.back_at = -1;
        s.nw = std::max(s.nw, id + 1);
    }
    for (const J& w : obs["broken"].as_array()) {
        int id = (int)w["wall"].as_int(); if (id < 0 || id >= 16) continue;
        Wl& q = s.w[id]; q.x1 = num(w, "x1"); q.y1 = num(w, "y1"); q.x2 = num(w, "x2"); q.y2 = num(w, "y2");
        q.spring = w["kind"].as_string() == "spring"; q.present = true; q.up = false; q.back_at = (int)w["at"].as_int();
        s.nw = std::max(s.nw, id + 1);
    }
    return s;
}
}  // namespace kbs
