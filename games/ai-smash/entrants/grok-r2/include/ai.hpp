#pragma once
// Search-backed policy. The copied engine is the forward model; opponent reactions are a small mix
// (approach-and-attack, shield the strike, blink out) weighted by what each seat has actually done.
#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace ai {

using smash::Engine;
using smash::Fighter;
using smash::Order;
using smash::Shot;
using J = mini_json::Json;

constexpr int AGGRO = 0, SHIELD = 1, DODGE = 2;
constexpr double STAGE = 13.0;

struct Snap {
    int turn = 0, tick = 0, next_shot = 1, n = 0;
    std::vector<Fighter> fighters;
    std::vector<Shot> shots;
    std::vector<int> kills, out_turn, out_tick;
};

struct Cand {
    Order order;
    const char* name;
};

static Engine* g_eng = nullptr;
static bool g_ready = false;
static double g_shield[4] = {0.34, 0.34, 0.34, 0.34};
static double g_dodge[4] = {0.14, 0.14, 0.14, 0.14};
static double g_counter[4] = {0.16, 0.16, 0.16, 0.16};
static int g_last_hit[4] = {-1, -1, -1, -1};
static int g_last_tick[4] = {-10000, -10000, -10000, -10000};
static int g_kills[4] = {0, 0, 0, 0};
static int g_mask[4] = {0, 0, 0, 0};
static bool g_have_prev = false;
static double g_px[4], g_py[4], g_pd[4];
static int g_ps[4], g_pph[4];
static std::string g_pact[4];

inline double D(const J& j, const char* k) {
    const J& v = j.at(k);
    return v.is_int() ? static_cast<double>(v.as_int()) : v.as_double();
}
inline int I(const J& j, const char* k) {
    const J& v = j.at(k);
    return v.is_int() ? static_cast<int>(v.as_int()) : static_cast<int>(v.as_double());
}
inline double hyp(double x, double y) { return std::sqrt(x * x + y * y); }
inline double cl(double v, double a, double b) { return std::max(a, std::min(b, v)); }

inline bool alive(const Fighter& f) { return !f.out && f.respawn <= 0; }
inline bool idle_act(const Fighter& f) {
    return f.act.empty() && f.phase == smash::IDLE && f.hitstun <= 0 && f.gap <= 0 && f.respawn <= 0 && !f.out;
}

inline void blast_at(int tick, double& bx, double& top) {
    bx = 26;
    top = 24;
    if (tick > 3000) {
        double s = (tick - 3000) * 0.02;
        bx = std::max(0.0, 26.0 - s);
        top = std::max(1.0, 24.0 - s * 0.9);
    }
}

inline bool foe_offstage(const Fighter& f) {
    if (!alive(f)) return false;
    if (f.y < -1.2) return true;
    if (std::fabs(f.x) > 14.0) return true;
    if (!f.grounded && std::fabs(f.x) > 13.35 && f.y < 7.5) return true;
    return false;
}

inline bool need_recover(const Fighter& f) {
    if (f.out || f.respawn > 0) return false;
    if (f.grounded && std::fabs(f.x) <= 13.45 && f.y >= -0.05 && f.y <= 12) return false;
    if (f.y < -0.6) return true;
    if (std::fabs(f.x) > 13.7) return true;
    if (!f.grounded && std::fabs(f.x) > 12.6 && f.vy < 0 && f.y < 3.2 && f.air_jumps <= 0 && f.meter < 0.22) return true;
    return false;
}

inline void aim(Order& o, double dx, double dy, int facing) {
    if (std::fabs(dx) + std::fabs(dy) < 1e-6) {
        dx = facing >= 0 ? 1 : -1;
        dy = 0;
    }
    o.has_dir = true;
    o.dx = dx;
    o.dy = dy;
}

inline void aim_stage(const Fighter& f, double& dx, double& dy) {
    double tx = cl(f.x, -12.0, 12.0);
    double ty = -1.3;
    if (std::fabs(f.x) > 13.0) tx = f.x > 0 ? 12.0 : -12.0;
    if (f.y < -0.2 && std::fabs(f.x) < 12.5) {
        tx = cl(f.x, -11.0, 11.0);
        ty = -1.0;
    }
    dx = tx - f.x;
    dy = ty - (f.y + 0.9);
}

inline int nearest_foe(const Engine& e, int i) {
    int best = -1;
    double bd = 1e18;
    const Fighter& f = e.fighters[i];
    for (int j = 0; j < e.n; ++j) {
        if (j == i || !alive(e.fighters[j])) continue;
        double d = hyp(e.fighters[j].x - f.x, e.fighters[j].y - f.y);
        if (d < bd) {
            bd = d;
            best = j;
        }
    }
    return best;
}

inline int best_target(const Engine& e, int me) {
    int best = -1;
    double bs = -1e18;
    const Fighter& f = e.fighters[me];
    double bx, top;
    blast_at(e.tick_no, bx, top);
    for (int j = 0; j < e.n; ++j) {
        if (j == me || e.fighters[j].out) continue;
        const Fighter& g = e.fighters[j];
        if (g.respawn > 25) continue;
        double dx = g.x - f.x, dy = g.y - f.y;
        double d = hyp(dx, dy);
        double s = g.damage * 1.4 + (3 - g.stocks) * 28.0 - d * 5.5;
        if (g.invuln > 8) s -= 220;
        else if (g.invuln > 0) s -= 40;
        if (g.hitstun > 0) s += 36 + std::min(20, g.hitstun);
        if (g.phase == smash::RECOVER) s += 28;
        if (g.act == "guard") s += 8;
        if (foe_offstage(g)) s += 90 + g.damage * 0.35;
        if (g.damage > 70 && std::fabs(g.x) > 8) s += 25;
        // Late shrink: bodies near the moving line are free stocks if we touch them.
        if (e.tick_no > 3000 && (std::fabs(g.x) > bx - 6 || g.y > top - 6)) s += 35;
        if (s > bs) {
            bs = s;
            best = j;
        }
    }
    return best;
}

inline int crowded(const Engine& e, int me, int except, double r) {
    int c = 0;
    const Fighter& f = e.fighters[me];
    for (int j = 0; j < e.n; ++j) {
        if (j == me || j == except || !alive(e.fighters[j]) || e.fighters[j].invuln > 0) continue;
        if (hyp(e.fighters[j].x - f.x, e.fighters[j].y - f.y) < r) ++c;
    }
    return c;
}

inline bool shot_hits(const Engine& e, int me, int ticks, bool hook_only) {
    const Fighter& f = e.fighters[me];
    if (f.out || f.respawn > 0 || f.invuln > 0) return false;
    for (const Shot& s : e.shots) {
        if (s.dead || s.owner == me) continue;
        if (hook_only && s.kind != "hook") continue;
        if (!hook_only && s.kind == "hook") continue;
        double x = s.x, y = s.y;
        double px = f.x, py = f.y, vx = f.vx, vy = f.vy;
        bool grounded = f.grounded;
        int n = std::min(ticks, std::max(0, s.ttl));
        for (int t = 0; t <= n; ++t) {
            if (x > -13 && x < 13 && y < 0 && y > -4) break;
            double dx = px - x, dy = py + 0.9 - y;
            if (dx * dx + dy * dy < 1.85 * 1.85) return true;
            if (t == n) break;
            x += s.vx;
            y += s.vy;
            if (!grounded) {
                vy = std::max(vy - 0.035, -0.8);
                vx *= 0.985;
                py += vy;
            }
            px += vx;
            if (grounded && py <= 0 && std::fabs(px) < 13.4) py = 0;
        }
    }
    return false;
}

inline bool under_stage(const Fighter& f);

inline Order steer(const Engine& e, int me) {
    Order o;
    const Fighter& f = e.fighters[me];
    if (under_stage(f)) {
        o.move = f.x >= 0 ? 1 : -1;
        if (f.hitstun <= 0 && f.respawn <= 0 && f.air_jumps > 0 && f.vy < 0.1 && f.y < -9.2) o.jump = true;
        return o;
    }
    int t = best_target(e, me);
    double tx = 0;
    if (t >= 0 && !need_recover(f)) tx = e.fighters[t].x;
    if (f.damage > 85) tx *= 0.35;
    if (need_recover(f) || std::fabs(f.x) > 13.2) tx = 0;
    if (f.grounded && f.y < 3 && std::fabs(f.x) > 11.3 && (tx - f.x) * f.x > 0) o.move = f.x > 0 ? -1 : 1;
    else if (tx > f.x + 0.35) o.move = 1;
    else if (tx < f.x - 0.35) o.move = -1;
    if (f.hitstun <= 0 && f.respawn <= 0 && (f.act.empty() || f.act == "charge") && !f.grounded && f.air_jumps > 0 &&
        f.vy < 0 && f.y < 4.2 && (std::fabs(f.x) > 10 || f.y < 1.2))
        o.jump = true;
    return o;
}

inline bool under_stage(const Fighter& f) {
    // The solid occupies y in [-4, 0]. Rising into it from below bonks and drops us back down.
    return !f.grounded && f.y < -0.4 && std::fabs(f.x) < 13.15;
}

inline Order recover_order(const Fighter& f) {
    Order o;
    bool under = under_stage(f);
    // Under the stage, the only way up is to clear a side edge first.
    double side = f.x >= 0 ? 1.0 : -1.0;
    o.move = under ? side : (f.x > 0.35 ? -1 : (f.x < -0.35 ? 1 : 0));
    bool falling = f.vy < 0.1;
    double hx = std::max(0.0, std::fabs(f.x) - 12.4);
    bool can_input = f.hitstun <= 0 && f.respawn <= 0 && (f.act.empty() || f.act == "charge");
    if (can_input && !f.grounded && f.air_jumps > 0 && falling) {
        if (under) {
            // A jump from here must not reach the floor (about 4.3 units of height).
            if (f.y < -9.2) o.jump = true;
        } else if (f.y < 7.5 && (f.y < 3.6 || hx > 3.5)) {
            o.jump = true;
            if (f.y > 6.5 && hx < 5) o.jump = false;
        }
    }
    if (!idle_act(f)) return o;
    if (under && f.meter >= 0.2) {
        o.skill = "blink";
        // Pure sideways. A sliver of up is enough to fight gravity after the blink ends.
        aim(o, side, f.y < -8 ? 0.08 : 0.0, f.facing);
        o.jump = false;
        o.move = side;
        return o;
    }
    if (!under && f.meter >= 0.2 && (f.y < -1.0 || hx > 5.5 || (f.y < 1.6 && f.air_jumps <= 0 && hx > 1.2))) {
        o.skill = "blink";
        double dx = f.x >= 0 ? -1 : 1;
        double dy = f.y < 0 ? 1.05 : (f.y < 1 ? 0.85 : (f.y < 4 ? 0.4 : (f.y > 11 ? -0.45 : 0.08)));
        aim(o, dx, dy, f.facing);
        o.jump = false;
        return o;
    }
    // Hook only once we are outside the footprint, aiming into the side lip.
    if (!under && f.meter >= 0.4 && f.hook_cd <= 0 && (f.y < -3.5 || hx > 8.0)) {
        o.skill = "hook";
        double dx, dy;
        aim_stage(f, dx, dy);
        aim(o, dx, dy, f.facing);
        return o;
    }
    if (!under && f.meter >= 0.25 && f.air_jumps <= 0 && falling && f.y < 1.5 && std::fabs(f.x) > 12.5) {
        o.skill = "uppercut";
        o.move = f.x > 0 ? -1 : 1;
        return o;
    }
    // Deep below the lip but already outside: uppercut buys the height blink did not.
    if (!under && f.meter >= 0.25 && f.air_jumps <= 0 && f.y < -2.5 && falling) {
        o.skill = "uppercut";
        o.move = f.x > 0 ? -1 : 1;
        return o;
    }
    return o;
}

inline bool is_strike_order(const Order& o, const Fighter& f) {
    if (o.skill == "attack" || o.skill == "uppercut" || o.skill == "slam" || o.skill == "wave") return true;
    if (f.act == "charge" && f.phase == smash::CHARGING && o.skill != "charge" && o.skill != "guard" && o.skill != "blink")
        return true;
    if ((f.act == "attack" || f.act == "uppercut" || f.act == "slam" || f.act == "charge") &&
        (f.phase == smash::WINDUP || f.phase == smash::ACTIVE))
        return true;
    return false;
}

inline Order foe_policy(const Engine& e, int i, int me, int mode, bool strike, int focus) {
    const Fighter& f = e.fighters[i];
    Order o;
    if (f.out || f.respawn > 0) return o;
    if (need_recover(f) && f.hitstun <= 0) return recover_order(f);
    if (f.hitstun > 0) {
        o.move = f.x > 0 ? -1 : 1;
        return o;
    }
    // Good players smack a whiffed swing and a naked charge. Model that in every reaction style.
    if (idle_act(f)) {
        const Fighter& mef = e.fighters[me];
        double mdx = mef.x - f.x, mdy = mef.y - f.y, md = hyp(mdx, mdy);
        bool punish = mef.phase == smash::RECOVER || (mef.act == "charge" && mef.phase == smash::CHARGING);
        if (punish && md < 3.55 && mef.invuln <= 0 && mef.hitstun <= 0) {
            o.skill = "attack";
            aim(o, mdx, mdy, f.facing);
            o.move = mdx >= 0 ? 1 : -1;
            return o;
        }
    }
    int t = nearest_foe(e, i);
    if (t < 0) {
        o.move = f.x > 0 ? -1 : 1;
        return o;
    }
    const Fighter& g = e.fighters[t];
    double dx = g.x - f.x, dy = g.y - f.y, d = hyp(dx, dy);
    bool react = mode != AGGRO && strike && i == focus && d < 5.6 && f.hitstun <= 0 && f.respawn <= 0 &&
                 (f.act.empty() || f.act == "charge" || f.act == "guard");
    if (react && mode == SHIELD) {
        o.skill = "guard";
        return o;
    }
    // A guard-heavy opponent grabs once we are in range and not swinging.
    if (mode == SHIELD && i == focus && !strike && idle_act(f) && d < 2.75 && g.invuln <= 0) {
        o.skill = "grab";
        aim(o, dx, dy, f.facing);
        o.move = dx >= 0 ? 1 : -1;
        return o;
    }
    if (react && mode == DODGE && f.meter >= 0.2 && (f.act.empty() || f.act == "charge")) {
        o.skill = "blink";
        aim(o, f.x - e.fighters[me].x, 0.65, f.facing);
        o.move = (f.x >= e.fighters[me].x) ? 1 : -1;
        return o;
    }
    if (f.act == "charge" && f.phase == smash::CHARGING) {
        if (d < 3.6) {
            aim(o, dx, dy, f.facing);
            return o;  // release
        }
        o.skill = "charge";
        aim(o, dx, dy, f.facing);
        o.move = dx > 0 ? 1 : -1;
        return o;
    }
    if (f.act == "guard" && f.phase == smash::ACTIVE) {
        if (d < 3.4) {
            o.skill = "guard";
            return o;
        }
        return o;
    }
    if (!f.act.empty()) return o;
    // Falling onto a body is a slam. The search has to expect that, or it stands under them.
    if (!f.grounded && f.meter >= 0.2 && f.y > 0.45 && f.y < 5.5 && g.y < f.y + 0.4 && g.y > -1.2 && d < 4.0 &&
        g.invuln <= 0 && f.vy <= 0.35) {
        o.skill = "slam";
        return o;
    }
    o.move = dx > 0.2 ? 1 : (dx < -0.2 ? -1 : 0);
    if (f.grounded && f.y < 2 && std::fabs(f.x) > 11.6 && o.move * f.x > 0) o.move = 0;
    if (dy > 2.3 && f.grounded && std::fabs(dx) < 5.5) o.jump = true;
    if (f.gap <= 0 && d < 2.95 && std::fabs(dy) < 2.4 && g.invuln <= 0) {
        o.skill = "attack";
        aim(o, dx, dy, f.facing);
    }
    return o;
}

inline Order self_policy(const Engine& e, int me) {
    const Fighter& f = e.fighters[me];
    if (f.out || f.respawn > 0 || f.hitstun > 0) return steer(e, me);
    if (f.act == "charge" && f.phase == smash::CHARGING) {
        Order o;
        int t = best_target(e, me);
        int threat = nearest_foe(e, me);
        double td = 99;
        if (threat >= 0) td = hyp(e.fighters[threat].x - f.x, e.fighters[threat].y - f.y);
        bool danger = shot_hits(e, me, 6, true) || (threat >= 0 && td < 3.3 && e.fighters[threat].hitstun <= 0 &&
                                                     e.fighters[threat].act != "charge" && idle_act(e.fighters[threat]) == false &&
                                                     (e.fighters[threat].phase == smash::ACTIVE ||
                                                      e.fighters[threat].act == "slam"));
        if (shot_hits(e, me, 7, true) && f.meter >= 0.2) {
            o.skill = "blink";
            double sx = 0, sy = 0;
            for (const Shot& s : e.shots)
                if (s.kind == "hook" && s.owner != me) {
                    sx = s.vx;
                    sy = s.vy;
                }
            aim(o, -sy, sx == 0 && sy == 0 ? 1 : sx, f.facing);
            return o;
        }
        if (danger && f.damage > 45) {
            o.skill = (f.meter >= 0.2 && f.damage > 80) ? "blink" : "guard";
            if (o.skill == "blink") aim(o, -f.x, 0.4, f.facing);
            return o;
        }
        if (t < 0) {
            o.skill = "charge";
            aim(o, static_cast<double>(f.facing), 0, f.facing);
            return o;
        }
        const Fighter& g = e.fighters[t];
        double dx = g.x - f.x, dy = g.y - f.y, d = hyp(dx, dy);
        // Short charges are scaled down to 0.85 until half of max_ticks (23). Hold there unless they are about to wake.
        bool in_range = d < 3.9 && std::fabs(dy) < 2.5 && g.invuln <= 0;
        bool can_hold = g.hitstun > 14 && d < 5.5;
        auto release_out = [&]() {
            double want = std::fabs(g.x) < 2.0 ? (dx >= 0 ? 1.0 : -1.0) : (g.x >= 0 ? 1.0 : -1.0);
            double adx = dx * want > 0 ? dx + want * 1.1 : dx * 0.55 + want * 0.35;
            aim(o, adx, 0.62, f.facing);
            o.move = want > 0 ? 1 : -1;
        };
        if (in_range && !can_hold && f.charge >= 1) {
            release_out();
            return o;  // tap: holding a short charge in neutral just gets us hit
        }
        if (in_range && f.charge >= 23) {
            release_out();
            return o;
        }
        o.skill = "charge";
        aim(o, dx, dy, f.facing);
        o.move = dx > 0 ? 1 : -1;
        if (f.grounded && std::fabs(f.x) > 11.2 && o.move * f.x > 0) o.move = f.x > 0 ? -1 : 1;
        return o;
    }
    if (f.act == "guard" && f.phase == smash::ACTIVE) {
        Order o;
        bool hold = shot_hits(e, me, 6, false);
        int threat = nearest_foe(e, me);
        if (threat >= 0) {
            const Fighter& g = e.fighters[threat];
            double d = hyp(g.x - f.x, g.y - f.y);
            if (d < 4.0 && (g.phase == smash::WINDUP || g.phase == smash::ACTIVE) && g.act != "grab" && g.act != "hook")
                hold = true;
            if (g.act == "grab" && d < 3.5) hold = false;
        }
        if (hold && f.phase_t > 2) {
            o.skill = "guard";
            return o;
        }
        return steer(e, me);
    }
    if (!f.act.empty()) return steer(e, me);
    if (need_recover(f)) return recover_order(f);

    // Projectile answers are immediate; search also considers them, but the autopilot must not eat a hook.
    if (shot_hits(e, me, 8, true) && f.meter >= 0.2 && idle_act(f)) {
        Order o;
        o.skill = "blink";
        double sx = 0, sy = 1;
        for (const Shot& s : e.shots)
            if (!s.dead && s.kind == "hook" && s.owner != me) {
                sx = s.vx;
                sy = s.vy;
            }
        aim(o, -sy, sx, f.facing);
        return o;
    }
    if (shot_hits(e, me, 6, false) && idle_act(f)) {
        Order o;
        o.skill = "guard";
        return o;
    }

    int t = best_target(e, me);
    if (t < 0) return steer(e, me);
    const Fighter& g = e.fighters[t];
    double dx = g.x - f.x, dy = g.y - f.y, d = hyp(dx, dy);
    int crowd = crowded(e, me, t, 4.6);
    Order o;
    o.move = dx > 0.25 ? 1 : (dx < -0.25 ? -1 : 0);
    if (f.grounded && f.y < 3.5 && std::fabs(f.x) > 11.4 && o.move * f.x > 0 && !foe_offstage(g)) o.move = f.x > 0 ? -1 : 1;

    // Someone is already swinging. Shield strikes; leave the ground against grabs.
    if (idle_act(f)) {
        for (int j = 0; j < e.n; ++j) {
            if (j == me || !alive(e.fighters[j])) continue;
            const Fighter& a = e.fighters[j];
            if (a.phase != smash::WINDUP && a.phase != smash::ACTIVE) continue;
            double ad = hyp(a.x - f.x, a.y - f.y);
            if (ad > 4.2) continue;
            if (a.act == "grab" || a.act == "hook") {
                if (f.meter >= 0.2 && f.damage > 35) {
                    o.skill = "blink";
                    aim(o, f.x - a.x, 0.7, f.facing);
                    return o;
                }
                o.jump = f.grounded || f.air_jumps > 0;
                o.move = (f.x >= a.x) ? 1 : -1;
                return o;
            }
            if (a.act == "attack" || a.act == "charge" || a.act == "uppercut" || a.act == "slam" || a.act == "wave") {
                if (a.phase == smash::WINDUP && a.phase_t >= 3 && a.phase_t <= 8 && ad < 3.3 && f.meter >= 0.3 &&
                    crowd == 0 && a.act != "slam" && g_shield[j] < 0.55) {
                    o.skill = "counter";
                    return o;
                }
                o.skill = "guard";
                return o;
            }
        }
    }

    if (idle_act(f) && !g.grounded && g.y >= f.y - 0.2 && g.y < f.y + 3.2 && std::fabs(dx) < 2.7 && g.vy < 0.35 &&
        g.invuln <= 0 && g.hitstun <= 0 && g.act != "grab") {
        o.skill = "guard";
        return o;
    }

    if (g.invuln > 12) {
        // Space their landing without eating a free hit.
        double want = 2.6;
        if (d > want + 0.4) return o;
        o.move = dx > 0 ? -1 : 1;
        return o;
    }

    bool can = idle_act(f) && g.invuln <= 0;
    // Short-hop slam: jump from just outside attack range, then slam on the way down.
    if (can && !f.grounded && f.meter >= 0.2 && f.y > 0.45 && f.y < 5.2 && g.y > -1.0 && g.y < f.y + 0.5 && d < 4.0 &&
        f.vy <= 0.4 && std::fabs(f.x) < 10.5) {
        o.skill = "slam";
        o.move = dx >= 0 ? 1 : -1;
        return o;
    }
    if (can && f.grounded && f.meter >= 0.2 && f.damage < 95 && g.grounded && g.hitstun <= 0 && g.phase != smash::ACTIVE &&
        d > 2.5 && d < 4.2 && std::fabs(dy) < 1.8 && std::fabs(f.x) < 11.5) {
        o.jump = true;
        o.move = dx >= 0 ? 1 : -1;
        return o;
    }
    bool in_atk = d < 3.05 && std::fabs(dy) < 2.15;
    bool in_grab = d < 2.85 && std::fabs(dy) < 1.9;
    bool inside = std::fabs(f.x) + 0.4 < std::fabs(g.x) || f.x * g.x < 0;
    double edge = std::fabs(g.x);
    bool kill_atk = g.damage >= (edge > 10 ? 52 : edge > 7 ? 68 : 100);
    bool kill_grab = g.damage >= (edge > 10 ? 24 : edge > 7 ? 42 : 62);
    bool kill_up = g.damage >= 102 && d < 2.35 && g.y >= f.y - 0.8;

    if (foe_offstage(g)) {
        double ledge = g.x >= 0 ? 10.6 : -10.6;
        // Slam locks a long dive. Chasing someone already in the pit rides us out the bottom.
        if (!f.grounded && f.y > g.y + 0.7 && f.y > 0.6 && f.y < 8 && g.y > -1.2 && f.y - g.y < 3.6 &&
            std::fabs(f.x - g.x) < 3.3 && f.meter >= 0.2 && (f.air_jumps > 0 || f.meter >= 0.45)) {
            o.skill = "slam";
            o.move = g.x > 0 ? 1 : -1;
            return o;
        }
        if (can && in_atk) {
            o.skill = "attack";
            aim(o, dx, dy, f.facing);
            o.move = g.x > 0 ? 1 : -1;
            return o;
        }
        if (can && f.meter >= 0.82 && d > 5.5 && d < 20 && g.damage > 35 && f.damage < 110) {
            o.skill = "wave";
            double lead = cl(d / 0.75, 0, 28);
            aim(o, g.x + g.vx * lead - f.x, g.y + g.vy * lead - f.y, f.facing);
            return o;
        }
        if (can && kill_up && f.meter >= 0.25 && g.y > f.y && f.y < 12) {
            o.skill = "uppercut";
            return o;
        }
        o.move = ledge > f.x + 0.3 ? 1 : (ledge < f.x - 0.3 ? -1 : 0);
        if (f.grounded && std::fabs(f.x) > 11.8 && o.move * f.x > 0) o.move = 0;
        if (dy > 2.8 && f.grounded && std::fabs(dx) < 4) o.jump = true;
        return o;
    }

    // Step to the inside for a kill angle only when we are not already in swing range.
    if (can && !inside && !in_atk && g.damage > 55 && g.hitstun < 8 && g.phase != smash::RECOVER && edge > 5 && d > 3.2 && d < 8) {
        o.move = g.x > 0 ? -1 : 1;
        if (f.grounded && std::fabs(f.x) > 11.5 && o.move * f.x > 0) o.move = 0;
        return o;
    }

    if (can && g.act == "counter" && g.phase == smash::ACTIVE) {
        if (in_grab && f.meter >= 0.3) {
            o.skill = "grab";
            aim(o, dx, dy, f.facing);
            return o;
        }
        o.move = dx > 0 ? -1 : 1;
        return o;
    }
    if (can && g.act == "guard" && in_grab && f.meter >= 0.3) {
        o.skill = "grab";
        double ox = g.x >= 0 ? 1.0 : -1.0;
        if (std::fabs(g.x) < 3) ox = dx > 0 ? 1 : -1;
        aim(o, ox * 1.2 + dx * 0.15, dy * 0.25, f.facing);
        return o;
    }
    if (can && g.act == "charge" && g.phase == smash::CHARGING && in_atk) {
        o.skill = "attack";
        aim(o, dx, dy, f.facing);
        return o;
    }

    // Charge is how the top bots take stocks: on hitstun, on a whiffed slam, or as a spaced tap.
    bool feeds_counter = g_counter[t] > 0.38;
    bool stun_punish = g.hitstun > 4 && d < 5.2 && !feeds_counter;
    bool whiff_punish = g.phase == smash::RECOVER && d < 4.4 && !feeds_counter &&
                        (g.act == "slam" || g.act == "charge" || g.act == "attack" || g.act == "grab");
    bool spaced = g.hitstun <= 0 && g.phase != smash::ACTIVE && g.phase != smash::WINDUP && d > 2.4 && d < 4.6 &&
                  std::fabs(dy) < 2.2 && g.damage >= 18 && crowd == 0 && !feeds_counter && f.damage < 110;
    bool kill_charge = kill_atk && d < 4.3 && !feeds_counter && g_shield[t] < 0.5;
    if (can && (stun_punish || whiff_punish || spaced || kill_charge) && g.invuln <= 0 && f.damage < 125) {
        o.skill = "charge";
        double want = std::fabs(g.x) < 2.0 ? (dx >= 0 ? 1.0 : -1.0) : (g.x >= 0 ? 1.0 : -1.0);
        double adx = dx * want > 0 ? dx + want : dx;
        aim(o, adx, dy * 0.4 + 0.5, f.facing);
        o.move = want > 0 ? 1 : -1;
        return o;
    }

    if (can && in_atk && f.damage > 95 && crowd >= 1 && f.meter >= 0.2) {
        o.skill = "blink";
        aim(o, -f.x, 0.35, f.facing);
        return o;
    }

    if (can && (in_grab || in_atk)) {
        double sp = g_shield[t];
        if ((sp > 0.42 || g.act == "guard" || g_counter[t] > 0.34) && in_grab && f.meter >= 0.3) {
            o.skill = "grab";
            aim(o, dx, dy, f.facing);
            return o;
        }
        if (kill_up && f.meter >= 0.25 && g_counter[t] < 0.38 && f.y < 12) {
            o.skill = "uppercut";
            return o;
        }
        if (kill_grab && inside && !kill_atk && f.meter >= 0.3 && g_shield[t] > 0.22) {
            o.skill = "grab";
            aim(o, dx, dy * 0.3, f.facing);
            return o;
        }
        if (kill_atk && g_shield[t] < 0.45 && feeds_counter) {
            o.skill = "attack";
            aim(o, dx, dy * 0.35, f.facing);
            return o;
        }
        o.skill = "attack";
        double lead = (g.hitstun > 0) ? 0 : 2.5;
        aim(o, g.x + g.vx * lead - f.x, g.y + g.vy * lead - f.y, f.facing);
        return o;
    }

    if (dy > 2.6 && f.grounded && std::fabs(dx) < 6.5 && g.y < 12) o.jump = true;
    if (f.grounded && f.y > 3.8 && dy < -2.2 && std::fabs(dx) < 3.2) o.drop = true;
    if (d < 1.7 && f.damage > 60) o.move = dx > 0 ? -1 : 1;
    return o;
}

inline bool can_begin(const Fighter& f) {
    if (f.out || f.respawn > 0) return false;
    if (!f.act.empty()) return false;
    int hs = f.hitstun, g = f.gap;
    for (int t = 0; t < 3; ++t) {
        if (hs > 0) {
            if (--hs == 0) g = 0;
            continue;
        }
        if (g > 0) {
            --g;
            continue;
        }
        return true;
    }
    return false;
}

inline bool project_dead(const Fighter& g, int tick) {
    if (g.out || g.respawn > 0) return false;
    if (g.hitstun <= 0 && g.y > -1 && std::fabs(g.x) < 13.6 && (g.grounded || g.vy > -0.2)) return false;
    double x = g.x, y = g.y, vx = g.vx, vy = g.vy;
    int hs = g.hitstun;
    double bx, top;
    blast_at(tick, bx, top);
    bool grounded = g.grounded;
    for (int t = 0; t < 36; ++t) {
        bool h = hs > 0;
        if (hs > 0) --hs;
        vy = std::max(vy - 0.035, h ? -2.0 : -0.8);
        if (h) {
            vx *= 0.97;
            vy *= 0.97;
        } else if (!grounded)
            vx *= 0.985;
        x += vx;
        double foot = y;
        y += vy;
        grounded = false;
        if (vy <= 0 && foot >= 0 && y <= 0 && std::fabs(x) <= 13.55) {
            y = 0;
            vy = 0;
            grounded = true;
            if (!h) return false;
        }
        if (std::fabs(x) > bx || y > top || y < -16) return true;
    }
    return false;
}

inline double score_state(const Engine& e, int me, const Snap& snap) {
    const Fighter& f = e.fighters[me];
    const Fighter& was = snap.fighters[me];
    double s = 0;
    if (f.out) s -= 34000;
    else if (f.stocks < was.stocks) s -= 1600;
    s += f.stocks * 2800;
    s -= f.damage * 9.5;
    s += e.kills[me] * 4200.0;
    s += f.meter * 70;
    s -= f.hitstun * 6;
    s -= std::fabs(f.x) * 1.3;
    if (f.grounded && std::fabs(f.x) <= 12) s += 35;
    if (!f.grounded && (std::fabs(f.x) > 14 || f.y < -2)) s -= 280;
    if (f.y < -8) s -= 700;
    double bx, top;
    blast_at(e.tick_no, bx, top);
    if (std::fabs(f.x) > bx - 1.5) s -= 500;
    if (!f.out && project_dead(f, e.tick_no)) s -= 4500;
    if (f.hook_cd > was.hook_cd + 40) s -= 380;

    double meter_spent = was.meter - f.meter;
    double dealt = 0;
    double near = 1e9;
    for (int j = 0; j < e.n; ++j) {
        if (j == me) continue;
        const Fighter& g = e.fighters[j];
        const Fighter& w = snap.fighters[j];
        if (g.out && !w.out) s += 4600;
        else if (!g.out && g.stocks < w.stocks) s += 1700;
        if (!g.out) {
            double gain = std::max(0.0, g.damage - w.damage);
            dealt += gain;
            s += gain * 6.5;
            s -= g.stocks * 350;
            if (g.hitstun > 0) s += 18;
            if (foe_offstage(g)) s += 120;
            if (!g.out && project_dead(g, e.tick_no)) s += 1100;
            if (alive(g) && g.invuln <= 0) near = std::min(near, hyp(g.x - f.x, g.y - f.y));
        }
    }
    if (meter_spent > 0.18 && dealt < 1 && e.kills[me] == snap.kills[me] && f.stocks == was.stocks) s -= meter_spent * 220;
    if (near < 1e8 && f.damage < 120) s -= near * (f.damage > 90 ? 1.6 : 3.4);
    return s;
}

inline void restore(Engine& e, const Snap& s) {
    e.turn = s.turn;
    e.tick_no = s.tick;
    e.next_shot = s.next_shot;
    e.done = false;
    e.n = s.n;
    e.fighters = s.fighters;
    e.shots = s.shots;
    e.kills = s.kills;
    e.out_turn = s.out_turn;
    e.out_tick = s.out_tick;
    e.quiet = true;
}

inline double rollout(const Snap& snap, int me, const Order& first, int mode, int horizon) {
    restore(*g_eng, snap);
    int focus = best_target(*g_eng, me);
    if (focus < 0) focus = nearest_foe(*g_eng, me);
    for (int t = 0; t < horizon && !g_eng->done; ++t) {
        if (g_eng->fighters[me].out) break;
        std::vector<Order> ords(g_eng->n);
        Order mine = t == 0 ? first : self_policy(*g_eng, me);
        ords[me] = mine;
        bool strike = is_strike_order(mine, g_eng->fighters[me]);
        for (int i = 0; i < g_eng->n; ++i)
            if (i != me) ords[i] = foe_policy(*g_eng, i, me, mode, strike, focus);
        g_eng->step_orders(ords);
    }
    return score_state(*g_eng, me, snap);
}

inline J pack(const Order& o) {
    J j = J::object{};
    if (o.move > 0.2) j["move"] = 1.0;
    else if (o.move < -0.2) j["move"] = -1.0;
    if (o.jump) j["jump"] = true;
    if (o.drop) j["drop"] = true;
    if (!o.skill.empty()) j["skill"] = o.skill;
    if (o.has_dir) {
        double dx = o.dx, dy = o.dy;
        if (!std::isfinite(dx) || !std::isfinite(dy)) {
            dx = 1;
            dy = 0;
        }
        j["dir"] = J::array{dx, dy};
    }
    return j;
}

inline void push_skill(std::vector<Cand>& cs, const char* skill, double dx, double dy, int facing, double move, const char* name) {
    Order o;
    o.skill = skill;
    o.move = move;
    aim(o, dx, dy, facing);
    cs.push_back({o, name});
}

inline std::vector<Cand> candidates(const Engine& e, int me, const Order& heur) {
    std::vector<Cand> cs;
    cs.push_back({heur, "heur"});
    const Fighter& f = e.fighters[me];
    int t = best_target(e, me);
    bool charging = f.act == "charge" && f.phase == smash::CHARGING;
    bool guarding = f.act == "guard" && f.phase == smash::ACTIVE;
    bool begin = can_begin(f);
    bool jump_ok = f.hitstun <= 0 && f.respawn <= 0 && (f.act.empty() || charging) && (f.grounded || f.air_jumps > 0);

    auto add_move = [&](double m, bool jump, const char* name) {
        Order o;
        o.move = m;
        o.jump = jump && jump_ok;
        cs.push_back({o, name});
    };
    if (need_recover(f) || foe_offstage(f)) {
        bool under = under_stage(f);
        double side = f.x >= 0 ? 1.0 : -1.0;
        double inward = f.x >= 0 ? -1.0 : 1.0;
        add_move(under ? side : inward, false, "in");
        if (!under) add_move(inward, true, "in_j");
        else add_move(side, f.y < -9.2, "out_j");
        if (begin && f.meter >= 0.2) {
            if (under) {
                push_skill(cs, "blink", side, 0.05, f.facing, side, "blink_side");
                push_skill(cs, "blink", side, 0.0, f.facing, side, "blink_flat");
            } else {
                push_skill(cs, "blink", inward, f.y < 2 ? 0.9 : 0.2, f.facing, inward, "blink_in");
                push_skill(cs, "blink", inward * 0.45, 1, f.facing, 0, "blink_up");
            }
        }
        // Upward tools from under the solid bonk the floor. Use them only past the lip.
        if (!under && begin && f.meter >= 0.4 && f.hook_cd <= 0) {
            double dx, dy;
            aim_stage(f, dx, dy);
            push_skill(cs, "hook", dx, dy, f.facing, f.x > 0 ? -1 : 1, "hook_stage");
        }
        if (!under && begin && f.meter >= 0.25) {
            Order o;
            o.skill = "uppercut";
            o.move = f.x > 0 ? -1 : 1;
            cs.push_back({o, "upper"});
        }
        if (t >= 0 && begin) {
            double dx = e.fighters[t].x - f.x, dy = e.fighters[t].y - f.y;
            if (hyp(dx, dy) < 3.4) push_skill(cs, "attack", dx, dy, f.facing, 0, "atk_t");
            if (!f.grounded && f.y > e.fighters[t].y && f.y > 0.6 && f.y < 8 && e.fighters[t].y > -1.2 &&
                f.meter >= 0.2) {
                Order o;
                o.skill = "slam";
                cs.push_back({o, "slam"});
            }
        }
        return cs;
    }

    add_move(0, false, "stop");
    add_move(f.x > 0 ? -1 : 1, false, "center");
    if (jump_ok && (f.grounded || f.air_jumps > 0)) add_move(t >= 0 && e.fighters[t].x > f.x ? 1 : -1, true, "jump");
    if (t < 0) return cs;
    const Fighter& g = e.fighters[t];
    double dx = g.x - f.x, dy = g.y - f.y;
    double d = hyp(dx, dy);
    add_move(dx > 0 ? 1 : -1, false, "chase");
    add_move(dx > 0 ? -1 : 1, false, "back");
    if (guarding) {
        Order hold;
        hold.skill = "guard";
        cs.push_back({hold, "hold"});
        return cs;
    }
    if (charging) {
        Order keep;
        keep.skill = "charge";
        aim(keep, dx, dy, f.facing);
        keep.move = dx > 0 ? 1 : -1;
        cs.push_back({keep, "hold_c"});
        Order rel;
        aim(rel, dx, dy, f.facing);
        rel.move = dx > 0 ? 1 : -1;
        cs.push_back({rel, "release"});
        if (f.meter >= 0.2) {
            Order b;
            b.skill = "blink";
            aim(b, -f.x, 0.3, f.facing);
            cs.push_back({b, "cancel_b"});
        }
        Order gd;
        gd.skill = "guard";
        cs.push_back({gd, "cancel_g"});
        return cs;
    }
    if (!begin) return cs;

    bool fear_counter = false;
    if (!fear_counter) {
        push_skill(cs, "attack", dx, dy, f.facing, dx > 0 ? 1 : -1, "atk");
        push_skill(cs, "attack", g.x + g.vx * 3 - f.x, g.y + g.vy * 3 - f.y, f.facing, 0, "atk_lead");
    }
    double ox = g.x >= 0 ? 1.0 : -1.0;
    push_skill(cs, "grab", dx, dy, f.facing, 0, "grab");
    push_skill(cs, "grab", ox + dx * 0.2, dy * 0.25, f.facing, 0, "grab_out");
    Order gd;
    gd.skill = "guard";
    cs.push_back({gd, "guard"});
    if (f.meter >= 0.25 && g_counter[t] < 0.38 && f.y < 12) {
        Order u;
        u.skill = "uppercut";
        cs.push_back({u, "upper"});
    }
    if (f.meter >= 0.2) {
        push_skill(cs, "blink", -f.x, 0.25, f.facing, 0, "blink_c");
        push_skill(cs, "blink", -dx, 0.55, f.facing, 0, "blink_out");
    }
    if (!f.grounded && f.meter >= 0.2 && dy < -0.2 && f.y > 0.6 && f.y < 8 && g.y > -1.2 && f.y - g.y < 3.6 &&
        std::fabs(f.x) < 10.5) {
        Order s;
        s.skill = "slam";
        cs.push_back({s, "slam"});
    }
    if (f.meter >= 0.62 && d > 4 && d < 22) {
        double lead = cl(d / 0.75, 0, 26);
        push_skill(cs, "wave", g.x + g.vx * lead - f.x, g.y + g.vy * lead - (f.y + 0.4), f.facing, 0, "wave");
    }
    // Charge only when they cannot walk over and interrupt it. Counter only a swing already started.
    if ((g.hitstun >= 12 || g.phase == smash::RECOVER) && d < 4.5 && crowded(e, me, t, 5.0) == 0 && g_counter[t] < 0.38) {
        Order c;
        c.skill = "charge";
        aim(c, dx, dy, f.facing);
        c.move = dx > 0 ? 1 : -1;
        cs.push_back({c, "charge"});
    }
    bool swing = (g.phase == smash::WINDUP || g.phase == smash::ACTIVE) &&
                 (g.act == "attack" || g.act == "charge" || g.act == "uppercut" || g.act == "wave");
    if (f.meter >= 0.3 && swing && d < 3.6) {
        Order c;
        c.skill = "counter";
        cs.push_back({c, "counter"});
    }
    if (f.meter >= 0.4 && f.hook_cd <= 0 && d > 3.5 && d < 14 && g.act == "guard") {
        push_skill(cs, "hook", dx, dy, f.facing, 0, "hook");
    }
    // Second nearby body, so a free punish is not ignored because the primary target is awkward.
    for (int j = 0; j < e.n; ++j) {
        if (j == me || j == t || !alive(e.fighters[j]) || e.fighters[j].invuln > 0) continue;
        double ex = e.fighters[j].x - f.x, ey = e.fighters[j].y - f.y;
        if (hyp(ex, ey) < 4.0 && g_counter[j] < 0.32) push_skill(cs, "attack", ex, ey, f.facing, 0, "atk2");
    }
    if (f.grounded && f.y > 3.5) {
        Order o;
        o.drop = true;
        o.move = dx > 0 ? 1 : -1;
        cs.push_back({o, "drop"});
    }
    return cs;
}

inline void load_mem(const J& m) {
    if (!m.is_object()) return;
    auto take = [&](const char* k, double* arr) {
        if (!m.contains(k) || !m.at(k).is_array()) return;
        const auto& a = m.at(k).as_array();
        for (int i = 0; i < 4 && i < static_cast<int>(a.size()); ++i) arr[i] = a[i].is_number() ? a[i].as_double() : arr[i];
    };
    take("sh", g_shield);
    take("dg", g_dodge);
    take("ct", g_counter);
    if (m.contains("k") && m.at("k").is_array()) {
        const auto& a = m.at("k").as_array();
        for (int i = 0; i < 4 && i < static_cast<int>(a.size()); ++i)
            if (a[i].is_number()) g_kills[i] = static_cast<int>(a[i].as_double());
    }
    if (m.contains("lh") && m.at("lh").is_array()) {
        const auto& a = m.at("lh").as_array();
        for (int i = 0; i < 4 && i < static_cast<int>(a.size()); ++i)
            if (a[i].is_number()) g_last_hit[i] = static_cast<int>(a[i].as_double());
    }
}

inline void save_mem(J& m) {
    J sh = J::array{}, dg = J::array{}, ct = J::array{}, k = J::array{}, lh = J::array{};
    for (int i = 0; i < 4; ++i) {
        sh.push_back(g_shield[i]);
        dg.push_back(g_dodge[i]);
        ct.push_back(g_counter[i]);
        k.push_back(g_kills[i]);
        lh.push_back(g_last_hit[i]);
    }
    m = J::object{};
    m["sh"] = sh;
    m["dg"] = dg;
    m["ct"] = ct;
    m["k"] = k;
    m["lh"] = lh;
}

inline int phase_of(const std::string& s) {
    if (s == "charging") return smash::CHARGING;
    if (s == "windup") return smash::WINDUP;
    if (s == "active") return smash::ACTIVE;
    if (s == "recover") return smash::RECOVER;
    return smash::IDLE;
}

inline void remember_hit(const J& obs) {
    const auto& ps = obs.at("players").as_array();
    int tick = I(obs, "tick");
    int n = static_cast<int>(ps.size());
    std::vector<double> x(n), y(n), dmg(n);
    std::vector<int> st(n), ph(n);
    std::vector<std::string> act(n);
    for (int i = 0; i < n; ++i) {
        x[i] = D(ps[i], "x");
        y[i] = D(ps[i], "y");
        dmg[i] = D(ps[i], "damage");
        st[i] = I(ps[i], "stocks");
        const J& a = ps[i].at("act");
        act[i] = a.is_null() ? std::string() : a.as_string();
        ph[i] = phase_of(ps[i].at("phase").as_string());
    }
    if (!g_have_prev) {
        for (int i = 0; i < n && i < 4; ++i) {
            g_px[i] = x[i];
            g_py[i] = y[i];
            g_pd[i] = dmg[i];
            g_ps[i] = st[i];
            g_pact[i] = act[i];
            g_pph[i] = ph[i];
        }
        g_have_prev = true;
        return;
    }
    auto offensive = [](const std::string& a, int phase) {
        if (phase != smash::ACTIVE && phase != smash::RECOVER && phase != smash::WINDUP) return false;
        return a == "attack" || a == "charge" || a == "uppercut" || a == "slam" || a == "grab" || a == "counter" || a == "wave" ||
               a == "hook";
    };
    for (int i = 0; i < n && i < 4; ++i) {
        if (act[i] != g_pact[i] || ph[i] != smash::ACTIVE) g_mask[i] = 0;
        if (st[i] < g_ps[i]) {
            int cred = g_last_hit[i];
            int dt = tick - g_last_tick[i];
            if (cred >= 0 && cred < 4 && dt >= 0 && dt <= 300) g_kills[cred] += 1;
            g_last_hit[i] = -1;
        } else if (dmg[i] > g_pd[i] + 0.4) {
            int best = -1;
            double bd = 1e18;
            bool found_off = false;
            for (int j = 0; j < n && j < 4; ++j) {
                if (j == i) continue;
                double dist = hyp(g_px[j] - g_px[i], g_py[j] - g_py[i]);
                bool off = offensive(g_pact[j], g_pph[j]) || offensive(act[j], ph[j]);
                if (dist > 8) continue;
                if (off && (!found_off || dist < bd)) {
                    found_off = true;
                    bd = dist;
                    best = j;
                } else if (!found_off && dist < bd) {
                    bd = dist;
                    best = j;
                }
            }
            if (best >= 0) {
                g_last_hit[i] = best;
                g_last_tick[i] = tick;
                if (ph[best] == smash::ACTIVE) g_mask[best] |= 1 << i;
            }
        }
        if (act[i] != g_pact[i] && hyp(x[i] - g_px[0], y[i] - g_py[0]) < 6) {
            // Updated properly below once seat is known; placeholder replaced in decide.
        }
    }
    for (int i = 0; i < n && i < 4; ++i) {
        g_px[i] = x[i];
        g_py[i] = y[i];
        g_pd[i] = dmg[i];
        g_ps[i] = st[i];
        g_pact[i] = act[i];
        g_pph[i] = ph[i];
    }
}

inline Snap build_snap(const J& obs) {
    Snap s;
    s.turn = I(obs, "turn") - 1;
    s.tick = I(obs, "tick");
    const auto& ps = obs.at("players").as_array();
    s.n = static_cast<int>(ps.size());
    s.fighters.resize(s.n);
    s.kills.assign(s.n, 0);
    s.out_turn.assign(s.n, -1);
    s.out_tick.assign(s.n, -1);
    for (int i = 0; i < s.n; ++i) {
        const J& p = ps[i];
        Fighter& f = s.fighters[i];
        f.x = D(p, "x");
        f.y = D(p, "y");
        f.vx = D(p, "vx");
        f.vy = D(p, "vy");
        f.damage = D(p, "damage");
        f.meter = D(p, "meter");
        f.facing = I(p, "facing");
        f.dx = p.at("dir").at(0).as_double();
        f.dy = p.at("dir").at(1).as_double();
        f.air_jumps = I(p, "air_jumps");
        f.stocks = I(p, "stocks");
        f.respawn = I(p, "respawn");
        f.invuln = I(p, "invuln");
        f.hitstun = I(p, "hitstun");
        f.gap = I(p, "gap");
        f.hook_cd = I(p, "hook_cd");
        f.charge = I(p, "charge");
        f.phase_t = I(p, "phase_ticks");
        f.phase = phase_of(p.at("phase").as_string());
        f.grounded = p.at("grounded").as_bool();
        f.out = p.at("out").as_bool();
        const J& a = p.at("act");
        f.act = a.is_null() ? std::string() : a.as_string();
        f.power = 1;
        if (f.act == "charge" && (f.phase == smash::WINDUP || f.phase == smash::ACTIVE || f.phase == smash::RECOVER)) {
            double t = std::min(1.0, f.charge / 45.0);
            f.power = 1.0 + t;
        }
        f.hit_mask = (i < 4) ? g_mask[i] : 0;
        f.last_hit = (i < 4) ? g_last_hit[i] : -1;
        f.last_hit_tick = (i < 4) ? g_last_tick[i] : -1;
        f.pulled_by = -1;
        f.pull_t = 0;
        f.reeling = false;
        f.drop = 0;
        f.hook_x = f.hook_y = 0;
        if (i < 4) s.kills[i] = g_kills[i];
        if (f.out) {
            s.out_turn[i] = 0;
            s.out_tick[i] = 0;
        }
    }
    for (int i = 0; i < s.n; ++i) {
        Fighter& f = s.fighters[i];
        if (f.grounded || f.hitstun <= 0) continue;
        for (int j = 0; j < s.n; ++j) {
            if (j != i && s.fighters[j].act == "slam") f.slam_landing = true;
        }
    }
    int shot_n = 0;
    for (const J& sh : obs.at("shots").as_array()) {
        Shot b;
        b.id = I(sh, "id");
        b.owner = I(sh, "owner");
        b.kind = sh.at("kind").as_string();
        b.x = D(sh, "x");
        b.y = D(sh, "y");
        b.vx = D(sh, "vx");
        b.vy = D(sh, "vy");
        b.ttl = I(sh, "ttl");
        if (b.kind == "wave") {
            b.damage = 6;
            b.power = 0.55;
        } else {
            b.damage = 3;
            b.power = 0;
        }
        s.shots.push_back(b);
        s.next_shot = std::max(s.next_shot, b.id + 1);
        ++shot_n;
        if (b.kind == "hook" && b.owner >= 0 && b.owner < s.n) {
            // owner is still firing
        }
    }
    std::vector<int> hooks(s.n, 0);
    for (const Shot& b : s.shots)
        if (b.kind == "hook" && b.owner >= 0 && b.owner < s.n) hooks[b.owner] = 1;
    for (int i = 0; i < s.n; ++i) {
        Fighter& f = s.fighters[i];
        if (f.act == "hook" && f.phase == smash::ACTIVE && f.phase_t <= 40 && !hooks[i]) {
            f.reeling = true;
            double sp = hyp(f.vx, f.vy);
            if (sp > 0.25) {
                f.hook_x = f.x + f.vx / sp * 7;
                f.hook_y = f.y + 0.9 + f.vy / sp * 7;
            } else {
                f.hook_x = cl(f.x, -12, 12);
                f.hook_y = -1;
            }
        }
    }
    (void)shot_n;
    return s;
}

inline void ensure(const J& obs) {
    if (g_eng) return;
    int n = static_cast<int>(obs.at("players").as_array().size());
    J subs = J::array{};
    for (int i = 0; i < n; ++i) subs.push_back(J(J::object{}));
    g_eng = new Engine(subs, J(static_cast<std::int64_t>(1)), obs.at("rules"));
    g_eng->quiet = true;
}

// Guard stops a slam. Search never sees one coming, so a committed slam or a dive at high damage skips it.
inline bool overhead_escape(const Engine& e, int me, Order& o) {
    const Fighter& f = e.fighters[me];
    if (f.out || f.respawn > 0 || f.hitstun > 0) return false;
    bool can_skill = f.act.empty() || (f.act == "charge" && f.phase == smash::CHARGING) ||
                     (f.act == "guard" && f.phase == smash::ACTIVE);
    if (!can_skill) return false;
    int slam = -1;
    double slam_dx = 0;
    for (int j = 0; j < e.n; ++j) {
        if (j == me || !alive(e.fighters[j])) continue;
        const Fighter& a = e.fighters[j];
        double dx = a.x - f.x;
        if (a.act == "slam" && (a.phase == smash::WINDUP || a.phase == smash::ACTIVE) && std::fabs(dx) < 4.5 &&
            a.y > f.y - 1.4) {
            slam = j;
            slam_dx = dx;
        } else if (slam < 0 && !a.grounded && a.hitstun <= 0 && a.invuln <= 0 && a.act != "grab" && a.vy < 0.35 &&
                   a.y >= f.y - 0.15 && a.y < f.y + 3.2 && std::fabs(dx) < 2.7) {
            slam = j;
            slam_dx = dx;
        }
    }
    if (slam < 0) return false;
    double away = slam_dx >= 0 ? -1.0 : 1.0;
    if (f.meter >= 0.2 && f.damage > 75 && f.act != "guard") {
        o.skill = "blink";
        aim(o, away, 0.12, f.facing);
        o.move = away;
        return true;
    }
    o.skill = "guard";
    return true;
}

// Search likes to swing into a move that is already out. At high damage or near the edge, take the heuristic answer.
inline bool committed_threat(const Engine& e, int me, Order& o) {
    const Fighter& f = e.fighters[me];
    if (f.out || f.respawn > 0 || f.hitstun > 0) return false;
    bool can_skill = f.act.empty() || (f.act == "charge" && f.phase == smash::CHARGING) ||
                     (f.act == "guard" && f.phase == smash::ACTIVE);
    if (!can_skill) return false;
    bool exposed = f.damage >= 48 || std::fabs(f.x) > 9.2;
    if (!exposed) return false;
    for (int j = 0; j < e.n; ++j) {
        if (j == me || !alive(e.fighters[j])) continue;
        const Fighter& a = e.fighters[j];
        if (a.phase != smash::WINDUP && a.phase != smash::ACTIVE) continue;
        double d = hyp(a.x - f.x, a.y - f.y);
        if (d > 4.15) continue;
        if (a.act == "counter") {
            if (f.meter >= 0.3 && d < 3.1) {
                o.skill = "grab";
                aim(o, a.x - f.x, a.y - f.y, f.facing);
            } else {
                o.move = f.x >= a.x ? 1 : -1;
            }
            return true;
        }
        if (a.act == "grab" || a.act == "hook") {
            if (f.meter >= 0.2) {
                o.skill = "blink";
                aim(o, f.x - a.x, 0.65, f.facing);
            } else {
                o.jump = true;
                o.move = f.x >= a.x ? 1 : -1;
            }
            return true;
        }
        if (a.act == "attack" || a.act == "charge" || a.act == "uppercut" || a.act == "wave" || a.act == "slam") {
            if (std::fabs(f.x) > 10.5 && f.meter >= 0.2 && f.damage > 65) {
                o.skill = "blink";
                aim(o, -f.x, 0.25, f.facing);
                return true;
            }
            o.skill = "guard";
            return true;
        }
    }
    return false;
}

inline J decide_impl(const J& obs, J& memory) {
    auto t0 = std::chrono::steady_clock::now();
    if (!g_ready) {
        load_mem(memory);
        g_ready = true;
    }
    // Style update uses the previous action, so it has to run before remember_hit overwrites history.
    if (g_have_prev && obs.contains("seat")) {
        int me = I(obs, "seat");
        const auto& ps = obs.at("players").as_array();
        double mx = g_px[me], my = g_py[me];
        for (int i = 0; i < static_cast<int>(ps.size()) && i < 4; ++i) {
            if (i == me) continue;
            const J& a = ps[i].at("act");
            std::string act = a.is_null() ? std::string() : a.as_string();
            if (act == g_pact[i]) continue;
            double dist = hyp(D(ps[i], "x") - mx, D(ps[i], "y") - my);
            if (dist > 5.0) continue;
            if (act == "guard") g_shield[i] = g_shield[i] * 0.75 + 0.25;
            else if (act == "blink") g_dodge[i] = g_dodge[i] * 0.7 + 0.3;
            else if (act == "counter") g_counter[i] = g_counter[i] * 0.65 + 0.35;
            else if (act == "attack" || act == "grab" || act == "uppercut" || act == "slam" || act == "charge") {
                g_shield[i] *= 0.82;
                g_dodge[i] *= 0.9;
                g_counter[i] *= 0.88;
            }
        }
    }
    remember_hit(obs);
    ensure(obs);
    Snap snap = build_snap(obs);
    restore(*g_eng, snap);
    int me = I(obs, "seat");
    const Fighter& f = g_eng->fighters[me];
    auto ms = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    if (f.out || f.respawn > 0 || f.hitstun >= 3) {
        save_mem(memory);
        return J::object{};
    }
    bool charging = f.act == "charge" && f.phase == smash::CHARGING;
    bool guarding = f.act == "guard" && f.phase == smash::ACTIVE;
    bool busy = !f.act.empty() && !charging && !guarding;
    Order heur = self_policy(*g_eng, me);
    // A short rollout treats every under-stage option as death, then prefers a swing. Don't ask it.
    if (under_stage(f) && f.hitstun <= 0 && !busy) {
        save_mem(memory);
        std::cout << "under " << static_cast<int>(ms()) << "ms\n";
        return pack(recover_order(f));
    }
    Order esc;
    if (overhead_escape(*g_eng, me, esc)) {
        save_mem(memory);
        std::cout << "over " << (esc.skill.empty() ? "mv" : esc.skill) << " " << static_cast<int>(ms()) << "ms\n";
        return pack(esc);
    }
    // Search still prefers a jab. Charge, slam, and the jump that sets up a slam are the kill plan.
    if (!busy && (heur.skill == "charge" || heur.skill == "slam" || (heur.jump && f.grounded && heur.skill.empty()))) {
        save_mem(memory);
        std::cout << "commit " << (heur.skill.empty() ? "jump" : heur.skill) << " " << static_cast<int>(ms()) << "ms\n";
        return pack(heur);
    }
    if ((idle_act(f) || charging || guarding) &&
        (shot_hits(*g_eng, me, 9, true) || shot_hits(*g_eng, me, 7, false))) {
        save_mem(memory);
        std::cout << "shot " << static_cast<int>(ms()) << "ms\n";
        return pack(heur);
    }
    if (busy && f.hitstun <= 0) {
        // Still allow search when an attack will end inside this window? Skip: the locked move barely matters.
        save_mem(memory);
        std::cout << "busy " << (heur.skill.empty() ? heur.move : 0) << " " << static_cast<int>(ms()) << "\n";
        return pack(heur);
    }
    std::vector<Cand> cs = candidates(*g_eng, me, heur);
    int focus = best_target(*g_eng, me);
    bool react = false;
    if (focus >= 0) {
        const Fighter& g = g_eng->fighters[focus];
        react = idle_act(g) || g.act == "guard" || (g.act == "charge" && g.phase == smash::CHARGING) || g.hitstun <= 2;
    }
    double sp = focus >= 0 ? cl(g_shield[focus], 0, 0.75) : 0;
    double dp = focus >= 0 ? cl(g_dodge[focus], 0, 0.55) : 0;
    if (!react) {
        sp = 0;
        dp = 0;
    }
    double ap = std::max(0.2, 1.0 - sp - dp);
    double z = ap + sp + dp;
    ap /= z;
    sp /= z;
    dp /= z;

    auto eval_one = [&](const Order& o, int horizon, bool all_modes) {
        double sa = rollout(snap, me, o, AGGRO, horizon);
        if (!all_modes || (sp < 0.04 && dp < 0.04)) return sa;
        double ss = sp > 0.04 ? rollout(snap, me, o, SHIELD, horizon) : sa;
        double sd = dp > 0.04 ? rollout(snap, me, o, DODGE, horizon) : sa;
        return ap * sa + sp * ss + dp * sd;
    };

    int horizon1 = need_recover(f) ? 8 : 5;
    std::vector<double> rough(cs.size(), -1e100);
    int kept = 0;
    int aggro_i = 0;
    for (int i = 0; i < static_cast<int>(cs.size()); ++i) {
        if (ms() > 20) break;
        rough[i] = rollout(snap, me, cs[i].order, AGGRO, horizon1);
        ++kept;
        if (rough[i] > rough[aggro_i]) aggro_i = i;
    }
    Order best = cs.empty() ? heur : cs[aggro_i].order;
    const char* best_name = cs.empty() ? "heur" : cs[aggro_i].name;
    double best_s = cs.empty() ? 0 : rough[aggro_i];
    // Longer horizon plus shield/blink reactions. Scores are only compared inside this pass.
    if (ms() < 16 && !cs.empty()) {
        std::vector<int> ord(cs.size());
        for (int i = 0; i < static_cast<int>(cs.size()); ++i) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](int a, int b) { return rough[a] > rough[b]; });
        int take = std::min(4, static_cast<int>(cs.size()));
        bool seeded = false;
        int horizon2 = need_recover(f) ? 11 : 9;
        for (int k = 0; k < take; ++k) {
            if (ms() > 32) break;
            int i = ord[k];
            if (rough[i] < -1e90) continue;
            double s = eval_one(cs[i].order, horizon2, react);
            if (!seeded || s > best_s) {
                seeded = true;
                best_s = s;
                best = cs[i].order;
                best_name = cs[i].name;
            }
        }
    }
    save_mem(memory);
    std::cout << best_name << " s" << static_cast<int>(best_s) << " n" << kept << " " << static_cast<int>(ms()) << "ms\n";
    return pack(best);
}

inline J decide(const J& obs, J& memory) {
    try {
        return decide_impl(obs, memory);
    } catch (const std::exception& e) {
        std::cout << "EX " << e.what() << "\n";
        J o = J::object{};
        try {
            int s = I(obs, "seat");
            double x = D(obs.at("players").at(s), "x");
            double vy = D(obs.at("players").at(s), "vy");
            o["move"] = x > 0 ? -1.0 : 1.0;
            if (vy < 0 && I(obs.at("players").at(s), "air_jumps") > 0) o["jump"] = true;
        } catch (...) {
        }
        return o;
    }
}

}  // namespace ai
