// Knockback arena bot v1: layered heuristic.
// Survival first (edge / shrink / bombs / telegraphs / walls), then finishers (hammer, dash, jab, bombs),
// with charge-bar management. Stateless across turns except a tiny memory blob.
#include "include/step_worker.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using J = mini_json::Json;

namespace {

constexpr double PI = 3.14159265358979323846;

struct V {
    double x = 0, y = 0;
    V() = default;
    V(double x_, double y_) : x(x_), y(y_) {}
    V operator+(const V& o) const { return {x + o.x, y + o.y}; }
    V operator-(const V& o) const { return {x - o.x, y - o.y}; }
    V operator*(double k) const { return {x * k, y * k}; }
    double dot(const V& o) const { return x * o.x + y * o.y; }
    double len2() const { return x * x + y * y; }
    double len() const { return std::sqrt(len2()); }
    double maxabs() const { return std::max(std::abs(x), std::abs(y)); }
};

V normv(V v) {
    double l = v.len();
    if (l < 1e-12) return {0, 0};
    return {v.x / l, v.y / l};
}

struct Player {
    V pos, vel;
    double bar = 0, charge = 0;
    int pause = 0, held = 0;
    int bomb_cd = 0, attack_cd = 0, hammer_cd = 0;
    bool out = false;
    std::string skill, act;
    V aim;
    bool has_aim = false;
};

struct Wall {
    int id = 0;
    bool spring = false;
    V a, b;
};

struct Bomb {
    int id = 0;
    V pos, vel;
    int fuse = 0;
    int owner = -1;
};

struct BrokenWall {
    int id = 0;
    int at = 0;
    V a, b;
};

struct Cfg {
    double friction = 0.06, bomb_friction = 0.05;
    double move_accel = 0.018, dash_charge_factor = 0.2, hammer_charge_factor = 0.3;
    double charge_rate = 0.04, overcharge = 45;
    double dash_speed = 0.9;
    double hammer_reach = 3.2, hammer_cone = 0.5, hammer_power = 0.9, hammer_recoil = 0.15;
    double hammer_cd = 60;
    double attack_reach = 2.8, attack_cone = 0.5, attack_power = 0.35, attack_windup = 9, attack_cd = 15;
    double bomb_radius = 0.6, bomb_mass = 0.5, bomb_throw = 0.5, bomb_gap = 1.8;
    int fuse_min = 24, fuse_max = 90, bomb_cd = 90;
    double blast_radius = 3.5, blast_power = 1.4;
    double wall_thick = 0.5, wall_restitution = 0.8, regen_push = 0.5, warn_ticks = 90;
    double player_radius = 1.0;
};

struct World {
    Cfg c;
    int turn = 0, tick = 0, me = 0, n = 0;
    double half = 16.5, next_half = -1;
    int next_turn = 0, next_end = 0;
    std::vector<Player> ps;
    std::vector<Wall> walls;
    std::vector<Bomb> bombs;
    std::vector<BrokenWall> broken;
};

double dnum(const J& j, const char* k) { return j[k].as_double(); }
long di(const J& j, const char* k) { return j[k].as_int(); }

Player parse_player(const J& j) {
    Player p;
    p.pos = {dnum(j, "x"), dnum(j, "y")};
    p.vel = {dnum(j, "vx"), dnum(j, "vy")};
    p.bar = dnum(j, "bar");
    p.charge = dnum(j, "charge");
    p.pause = int(di(j, "pause"));
    p.held = int(di(j, "held"));
    p.bomb_cd = int(di(j, "bomb_cd"));
    p.attack_cd = int(di(j, "attack_cd"));
    p.hammer_cd = int(di(j, "hammer_cd"));
    p.out = j["out"].as_bool();
    if (j["skill"].is_string()) p.skill = j["skill"].as_string();
    if (j["act"].is_string()) p.act = j["act"].as_string();
    if (j["aim"].is_array()) {
        p.has_aim = true;
        p.aim = {j["aim"][0].as_double(), j["aim"][1].as_double()};
    }
    return p;
}

// Displacement after t ticks of friction glide with no acceleration: v * (1-k^t)/(1-k).
double glide_sum(double keep, int t) {
    return (1.0 - std::pow(keep, t)) / (1.0 - keep);
}

// Resting position of a player after fully sliding.
V rest_pos(const V& p, const V& v, double friction) {
    double s = 1.0 / friction;
    return p + v * s;
}

V closest_on_wall(const V& p, const Wall& w) {
    V e = w.b - w.a;
    double l2 = e.len2();
    double t = l2 <= 0 ? 0 : std::clamp((p - w.a).dot(e) / l2, 0.0, 1.0);
    return w.a + e * t;
}

// A central-ish hold point that keeps clear of walls (spring walls especially).
V home_point(const World& w, double L) {
    double r0 = std::min(3.2, std::max(1.0, L - 9.0));
    V best{0, 0};
    double best_score = -1e18;
    for (int k = 0; k < 12; ++k) {
        double a = 2 * PI * k / 12;
        V cand{std::cos(a) * r0, std::sin(a) * r0};
        double score = -cand.len();
        for (const Wall& wl : w.walls) {
            double d = (cand - closest_on_wall(cand, wl)).len();
            double want = wl.spring ? 3.6 : 2.2;
            if (d < want) score -= (want - d) * 4.0;
        }
        if (score > best_score) { best_score = score; best = cand; }
    }
    return best;
}

// Build the full world view from an observation.
World parse_world(const J& obs) {
    World w;
    w.turn = int(di(obs, "turn"));
    w.tick = int(di(obs, "tick"));
    w.me = int(di(obs, "you"));
    w.n = int(di(obs, "n"));
    const J& rules = obs["rules"];
    Cfg& c = w.c;
    c.friction = dnum(rules["physics"], "friction");
    c.move_accel = dnum(rules["move"], "accel");
    c.dash_charge_factor = dnum(rules["move"], "charging_factor");
    c.hammer_charge_factor = dnum(rules["hammer"], "charging_factor");
    c.charge_rate = dnum(rules["bar"], "charge_rate");
    c.overcharge = dnum(rules["bar"], "overcharge_ticks");
    c.dash_speed = dnum(rules["dash"], "speed");
    c.hammer_reach = dnum(rules["hammer"], "reach");
    c.hammer_cone = dnum(rules["hammer"], "cone_cos");
    c.hammer_power = dnum(rules["hammer"], "power");
    c.hammer_recoil = dnum(rules["hammer"], "recoil");
    c.hammer_cd = dnum(rules["hammer"], "cooldown_ticks");
    c.attack_reach = dnum(rules["attack"], "reach");
    c.attack_cone = dnum(rules["attack"], "cone_cos");
    c.attack_power = dnum(rules["attack"], "power");
    c.attack_windup = dnum(rules["attack"], "windup_ticks");
    c.attack_cd = dnum(rules["attack"], "cooldown_ticks");
    c.bomb_radius = dnum(rules["bomb"], "radius");
    c.bomb_mass = dnum(rules["bomb"], "mass");
    c.bomb_throw = dnum(rules["bomb"], "throw_speed");
    c.bomb_gap = c.player_radius + c.bomb_radius + dnum(rules["bomb"], "spawn_gap");
    c.fuse_min = int(di(rules["bomb"], "fuse_min"));
    c.fuse_max = int(di(rules["bomb"], "fuse_max"));
    c.bomb_cd = int(di(rules["bomb"], "cooldown_ticks"));
    c.blast_radius = dnum(rules["bomb"], "blast_radius");
    c.blast_power = dnum(rules["bomb"], "blast_power");
    c.bomb_friction = dnum(rules["bomb"], "friction");
    c.wall_thick = dnum(rules["walls"], "thickness");
    c.wall_restitution = dnum(rules["walls"], "restitution");
    c.regen_push = dnum(rules["walls"], "regen_push");
    c.warn_ticks = dnum(rules["walls"], "warn_ticks");

    w.half = dnum(obs["arena"], "half");
    if (obs["arena"]["next"].is_object()) {
        w.next_half = dnum(obs["arena"]["next"], "half");
        w.next_turn = int(di(obs["arena"]["next"], "turn"));
        w.next_end = int(di(obs["arena"]["next"], "end_turn"));
    }

    const J& ps = obs["players"];
    w.ps.resize(ps.size());
    for (size_t i = 0; i < ps.size(); ++i) w.ps[i] = parse_player(ps[i]);

    for (const J& jw : obs["walls"].as_array()) {
        Wall wl;
        wl.id = int(di(jw, "id"));
        wl.spring = jw["kind"].as_string() == "spring";
        wl.a = {dnum(jw, "x1"), dnum(jw, "y1")};
        wl.b = {dnum(jw, "x2"), dnum(jw, "y2")};
        w.walls.push_back(wl);
    }
    for (const J& jb : obs["bombs"].as_array()) {
        Bomb b;
        b.id = int(di(jb, "id"));
        b.pos = {dnum(jb, "x"), dnum(jb, "y")};
        b.vel = {dnum(jb, "vx"), dnum(jb, "vy")};
        b.fuse = int(di(jb, "fuse"));
        b.owner = int(di(jb, "owner"));
        w.bombs.push_back(b);
    }
    for (const J& jb : obs["broken"].as_array()) {
        BrokenWall bw;
        bw.id = int(di(jb, "wall"));
        bw.at = int(di(jb, "at"));
        bw.a = {dnum(jb, "x1"), dnum(jb, "y1")};
        bw.b = {dnum(jb, "x2"), dnum(jb, "y2")};
        w.broken.push_back(bw);
    }
    return w;
}

// The half-side that actually matters now: an announced shrink is committed, so respect the target.
double effective_limit(const World& w) {
    if (w.next_half > 0) return std::min(w.half, w.next_half);
    return w.half;
}

int alive_count(const World& w) {
    int k = 0;
    for (const auto& p : w.ps) if (!p.out) ++k;
    return k;
}

// Steer a desired move vector around walls. Mutates mv.
void avoid_walls(const World& w, const V& me, V& mv) {
    V to_center = normv(V(-me.x, -me.y));
    for (const Wall& wl : w.walls) {
        V q = closest_on_wall(me, wl);
        V n = me - q;
        double d = n.len();
        double margin = wl.spring ? 3.2 : 1.7;
        if (d >= margin) continue;
        V un = d > 1e-9 ? normv(n) : normv(V(-(wl.b.y - wl.a.y), wl.b.x - wl.a.x));
        if (un.len2() == 0) un = {1, 0};
        double into = mv.dot(un);
        if (into < 0) mv = mv - un * into; // remove wall-ward component
        if (wl.spring) {
            // Route along the wall instead of scraping it; pick the tangent toward center.
            V tan = V(-(wl.b.y - wl.a.y), wl.b.x - wl.a.x);
            tan = normv(tan);
            if (tan.len2() > 0 && tan.dot(to_center) < 0) tan = tan * -1;
            double push = (margin - d) / margin;
            mv = mv + tan * (0.5 + push) + un * (0.5 + push);
        } else {
            double push = (margin - d) / margin;
            mv = mv + un * (0.4 + 0.8 * push);
        }
    }
}

struct Order {
    V move{0, 0};
    bool has_move = false;
    const char* charge = nullptr; // "dash"/"hammer"/null
    V dash{0, 0};
    bool has_dash = false;
    V hammer{0, 0};
    bool has_hammer = false;
    V attack{0, 0};
    bool has_attack = false;
    V bomb_dir{0, 0};
    int bomb_fuse = 0;
    bool has_bomb = false;
};

J to_json(const Order& o) {
    J j = J::object{};
    if (o.has_move) j["move"] = J::array{o.move.x, o.move.y};
    if (o.charge) j["charge"] = J(o.charge);
    if (o.has_dash) j["dash"] = J::array{o.dash.x, o.dash.y};
    if (o.has_hammer) j["hammer"] = J::array{o.hammer.x, o.hammer.y};
    if (o.has_attack) j["attack"] = J::array{o.attack.x, o.attack.y};
    if (o.has_bomb)
        j["bomb"] = J::object{{"dir", J::array{o.bomb_dir.x, o.bomb_dir.y}}, {"fuse", o.bomb_fuse}};
    return j;
}

// Pick the main target: near-edge, close, immobile enemies first.
int pick_target(const World& w) {
    const Player& m = w.ps[w.me];
    double L = effective_limit(w);
    int best = -1;
    double best_score = -1e18;
    for (int i = 0; i < w.n; ++i) {
        if (i == w.me || w.ps[i].out) continue;
        const Player& e = w.ps[i];
        double d = (e.pos - m.pos).len();
        double edge = L - e.pos.maxabs();              // smaller = closer to edge
        double immobile = e.act.empty() ? 0 : 1.5;     // busy enemies are easy
        double score = -d * 1.0 + (L - edge) * 0.9 + immobile * 2.0;
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

// Is it safe for me to commit to an immobile action (jab / hammer recover) right now?
bool safe_to_commit(const World& w, int ticks_locked) {
    const Player& m = w.ps[w.me];
    double L = effective_limit(w);
    if (m.pos.maxabs() > L - 2.2) return false;
    if (rest_pos(m.pos, m.vel, w.c.friction).maxabs() > L - 2.6) return false;
    double kp = 1 - w.c.friction, kb = 1 - w.c.bomb_friction;
    for (const Bomb& b : w.bombs) {
        int t = std::min(b.fuse, ticks_locked + 6);
        V bp = b.pos + b.vel * glide_sum(kb, t);
        V mp = m.pos + m.vel * glide_sum(kp, t);
        if ((bp - mp).len() < w.c.blast_radius + 0.8) return false;
    }
    return true;
}

// Best bomb throw at an enemy; returns false if no acceptable throw.
// Round 2: bombs are a primary weapon for the top bots (8+ throws/game). Pick the best target ourselves.
bool plan_bomb(const World& w, Order& o) {
    const Player& m = w.ps[w.me];
    double L = effective_limit(w);
    if (m.bomb_cd > 0 || !m.act.empty()) return false;
    if (w.turn < 4) return false;
    if (L < 7.5) return false; // bombs become self-traps in the late tiny arena
    if (m.pos.maxabs() > L - 4.5) return false; // never throw from the danger ring
    double kp = 1 - w.c.friction, kb = 1 - w.c.bomb_friction;
    int fuse_cap = std::min(w.c.fuse_max, 55);
    double best_score = -1e18;
    V best_dir{0, 0}, best_bp{0, 0};
    int best_f = -1;
    for (int t = 0; t < w.n; ++t) {
        if (t == w.me || w.ps[t].out) continue;
        const Player& e = w.ps[t];
        V lead = e.pos + e.vel * 12;
        if ((lead - m.pos).len() > 13.0) continue;
        bool near_edge = e.pos.maxabs() > L - 5.5;
        bool cluster = false;
        for (int i = 0; i < w.n; ++i) {
            if (i == w.me || i == t || w.ps[i].out) continue;
            if ((w.ps[i].pos - lead).len() < 3.4) cluster = true;
        }
        if (!near_edge && !cluster) continue;
        V dir = normv(lead - m.pos);
        if (dir.len2() == 0) continue;
        V start = m.pos + dir * w.c.bomb_gap;
        V bv0 = m.vel + dir * w.c.bomb_throw;
        int f_best = -1;
        double d_best = 1e18;
        V bp_best{0, 0};
        for (int f = w.c.fuse_min; f <= fuse_cap; f += 2) {
            V bp = start + bv0 * glide_sum(kb, f);
            if (bp.maxabs() > L - 0.8) continue;
            V ep = e.pos + e.vel * glide_sum(kp, f);
            double d = (bp - ep).len();
            if (d < d_best) { d_best = d; f_best = f; bp_best = bp; }
        }
        if (f_best < 0 || d_best > 3.0) continue;
        bool spring_bad = false;
        for (const Wall& wl : w.walls) {
            if (wl.spring && (bp_best - closest_on_wall(bp_best, wl)).len() < 1.8) spring_bad = true;
        }
        if (spring_bad) continue;
        V mp = m.pos + m.vel * glide_sum(kp, f_best);
        double self_d = (bp_best - mp).len();
        if (self_d < w.c.blast_radius + 0.5) continue;
        double score = (near_edge ? 2.0 : 0.0) + (cluster ? 1.5 : 0.0) - d_best * 0.5
                       + std::min(2.0, (self_d - w.c.blast_radius) * 0.4);
        if (score > best_score) { best_score = score; best_dir = dir; best_f = f_best; best_bp = bp_best; }
    }
    if (best_f < 0) return false;
    o.has_bomb = true;
    o.bomb_dir = best_dir;
    o.bomb_fuse = best_f;
    return true;
}

Order decide(const World& w) {
    Order o;
    const Player& m = w.ps[w.me];
    if (m.out) return o;
    const Cfg& c = w.c;
    double L = effective_limit(w);
    double kp = 1 - c.friction;
    int alive = alive_count(w);
    bool tiny = L <= 5.5;

    V to_center = normv(V(-m.pos.x, -m.pos.y));
    V my_rest = rest_pos(m.pos, m.vel, c.friction);
    double radial = m.pos.maxabs();

    // ---- Threat 1: I am sliding/standing too close to the edge. ----
    bool edge_emergency = my_rest.maxabs() > L - 1.2 || radial > L - 2.2;
    bool edge_warning = my_rest.maxabs() > L - 3.2 || radial > L - 4.0;
    V out_dir = normv(m.pos);
    double outward_speed = out_dir.len2() ? m.vel.dot(out_dir) : 0.0;
    // Outward momentum I cannot cancel while holding a charge (slow walk factor).
    bool momentum_danger = outward_speed > 0.12 && radial > L - 6.5;
    bool must_retreat = edge_warning || momentum_danger; (void)must_retreat;
    // Two-mode policy: outside RING I travel home at full walk speed (no charging); inside I charge and fight.
    // Holding a charge while travelling means 0.2-0.3x walk, which is how I got bounced off earlier.
    // Round 2: top bots hold radial ~3-4; tighten the ring and fight from the centre.
    double RING = tiny ? std::max(1.5, L - 2.2) : std::max(2.5, std::min(4.5, L - 8.0));
    V home = home_point(w, L);
    bool inside = (radial <= RING) && ((m.pos - home).len() <= RING + 1.5);
    // Recharge mode: after spending the bar I am vulnerable for ~13 turns; hide deep and refuse fights.
    bool recharging = m.charge <= 0.001 && m.bar < 0.45;
    bool fight_allowed = inside && !edge_warning && !recharging;
    double fight_radius = RING;

    // ---- Threat 2: incoming bombs. ----
    V bomb_escape{0, 0};
    bool bomb_danger = false;
    for (const Bomb& b : w.bombs) {
        int t = std::min(b.fuse, 40);
        V bp = b.pos + b.vel * glide_sum(1 - c.bomb_friction, t);
        V mp = m.pos + m.vel * glide_sum(kp, t);
        double d = (bp - mp).len();
        double danger_r = c.blast_radius + (b.fuse <= 14 ? 1.2 : 0.6);
        if (d < danger_r) {
            bomb_danger = true;
            V away = normv(mp - bp);
            if (away.len2() == 0) away = to_center;
            double weight = (danger_r - d) / danger_r + (b.fuse <= 10 ? 0.8 : 0.0);
            bomb_escape = bomb_escape + away * weight;
        }
    }

    // ---- Threat 3: telegraphed jabs landing on me. ----
    V jab_dodge{0, 0};
    bool jab_threat = false;
    for (int i = 0; i < w.n; ++i) {
        if (i == w.me || w.ps[i].out) continue;
        const Player& e = w.ps[i];
        if (e.act != "jab" || !e.has_aim) continue;
        int land = e.act.empty() ? 0 : 9; // act_ticks counts down; landing within at most 9 ticks
        (void)land;
        V ep = e.pos + e.vel * glide_sum(kp, 9);
        V mp = m.pos + m.vel * glide_sum(kp, 9);
        double d = (mp - ep).len();
        if (d > c.attack_reach - 0.2) continue;
        V u = normv(mp - ep);
        if (u.len2() > 0 && u.dot(e.aim) < c.attack_cone) continue; // not in its cone
        // perpendicular dodge, prefer the inward side
        V perp = normv(V(-e.aim.y, e.aim.x));
        if (perp.dot(to_center) < 0) perp = perp * -1;
        jab_dodge = jab_dodge + perp;
        jab_threat = true;
    }

    // ---- Threat 4: regrowing walls (warning phase). ----
    V regen_escape{0, 0};
    bool regen_danger = false;
    for (const BrokenWall& bw : w.broken) {
        int ticks_left = bw.at - w.tick;
        if (ticks_left > int(c.warn_ticks) || ticks_left <= 0) continue;
        Wall tmp{bw.id, false, bw.a, bw.b};
        V q = closest_on_wall(m.pos, tmp);
        V n = m.pos - q;
        double d = n.len();
        double reach = c.player_radius + c.wall_thick / 2 + 0.5;
        if (d >= reach) continue;
        V un = d > 1e-9 ? normv(n) : normv(V(-(bw.b.y - bw.a.y), bw.b.x - bw.a.x));
        if (un.len2() == 0) un = {1, 0};
        regen_escape = regen_escape + un * (1.0 + (reach - d));
        regen_danger = true;
    }

    // Pinned against a spring wall: top-priority escape, including a dash when one is ready.
    V spring_escape{0, 0};
    bool spring_pinned = false;
    for (const Wall& wl : w.walls) {
        if (!wl.spring) continue;
        V q = closest_on_wall(m.pos, wl);
        double d = (m.pos - q).len();
        if (d > 3.0) continue;
        V n = normv(m.pos - q);
        if (n.len2() > 0 && m.vel.dot(n) > -0.08) {
            V tan = normv(V(-(wl.b.y - wl.a.y), wl.b.x - wl.a.x));
            if (tan.dot(to_center) < 0) tan = tan * -1;
            spring_escape = spring_escape + tan + n * 0.4;
            spring_pinned = true;
        }
    }

    // Spring wall near a given direction (for vetting commits/dashes).
    auto spring_near = [&](V from, V dir, double span, double margin) {
        for (const Wall& wl : w.walls) {
            if (!wl.spring) continue;
            for (double s = 0; s <= span; s += 1.0) {
                V p = from + dir * s;
                V q = closest_on_wall(p, wl);
                if ((p - q).len() < margin) return true;
            }
        }
        return false;
    };
    // A dash direction is safe if my full slide stays inside and no spring wall sits on the path.
    auto safe_dash_dir = [&](V dir, double margin_inside) {
        if (dir.len2() == 0) return false;
        dir = normv(dir);
        V after = rest_pos(m.pos, m.vel + dir * c.dash_speed * m.charge, c.friction);
        return after.maxabs() < L - margin_inside && !spring_near(m.pos, dir, 4.5, 2.2);
    };
    // Search inward-esque directions (center first, then rotated) for a safe dash vector.
    auto inward_dash = [&](double margin_inside) -> V {
        V base = to_center;
        if (base.len2() == 0) base = V(1, 0);
        double ang[] = {0.0, 0.45, -0.45, 0.9, -0.9, 1.35, -1.35};
        for (double a : ang) {
            double cs = std::cos(a), sn = std::sin(a);
            V dir{base.x * cs - base.y * sn, base.x * sn + base.y * cs};
            if (safe_dash_dir(dir, margin_inside)) return dir;
        }
        // Desperate fallback (e.g. trapped between spring walls): ignore spring corridor, keep inside.
        V fallback{0, 0};
        double best_rest = 1e18;
        for (double a : ang) {
            double cs = std::cos(a), sn = std::sin(a);
            V dir = normv(V{base.x * cs - base.y * sn, base.x * sn + base.y * cs});
            V after = rest_pos(m.pos, m.vel + dir * c.dash_speed * m.charge, c.friction);
            if (after.maxabs() < L - 0.6 && after.maxabs() < best_rest) { best_rest = after.maxabs(); fallback = dir; }
        }
        return fallback;
    };

    // ---- Emergency dash inward: edge crossing (any usable charge), or spring-trapped (only meaningful
    // charge — tiny dumps just reset the bar's pause and prevent recovery). ----
    double min_emergency = spring_pinned && !edge_emergency ? 0.35 : 0.15;
    if (m.skill == "dash" && m.charge >= min_emergency && (edge_emergency || spring_pinned)) {
        V dir = inward_dash(1.2);
        if (dir.len2() > 0) {
            o.has_dash = true;
            o.dash = dir;
            o.has_move = true;
            o.move = dir;
            return o;
        }
    }

    bool busy = !m.act.empty();

    // Jabbing locks me for 15 ticks: only veto when a spring wall is right at my back.
    bool jab_space = true;
    for (const Wall& wl : w.walls) {
        if (wl.spring && (m.pos - closest_on_wall(m.pos, wl)).len() < 2.2) jab_space = false;
    }
    // ---- Offense: hammer finisher. ----
    int target = pick_target(w);
    bool force_release = m.held >= 30 && m.charge > 0; // overcharge deadline is 45 ticks; act early
    if (!busy && target >= 0 && m.skill == "hammer" && m.charge > 0) {
        const Player& e = w.ps[target];
        V u = normv(e.pos - m.pos);
        double d = (e.pos - m.pos).len();
        if (u.len2() > 0 && d <= c.hammer_reach - 0.15) {
            V e_rest = rest_pos(e.pos, e.vel + u * (c.hammer_power * m.charge), c.friction);
            bool lethal = e_rest.maxabs() > L - 0.3;
            // Whiffing a full charge costs a ~13-turn vulnerability window: demand a high-certainty target.
            bool certain = d <= 2.9 || (!e.act.empty() && d <= c.hammer_reach - 0.15);
            bool strong = ((lethal && m.charge >= 0.3) || m.charge >= 0.85 ||
                           (force_release && m.charge >= 0.25) || (edge_emergency && m.charge >= 0.4)) &&
                          (certain || force_release || edge_emergency);
            V me_after = rest_pos(m.pos, m.vel + u * (-c.hammer_recoil * m.charge), c.friction);
            bool spring_behind = spring_near(m.pos, u * -1, 3.2, 2.6);
            bool i_am_safe = me_after.maxabs() < L - 2.2 && !spring_behind &&
                             safe_to_commit(w, 6) && (!jab_threat || lethal);
            if (strong && i_am_safe) {
                o.has_hammer = true;
                o.hammer = u;
                return o;
            }
        }
        if (force_release && m.charge >= 0.15) {
            // Spend the charge rather than lose it, but never swing at an uncertain target: a whiffed full
            // hammer leaves us empty and stationary. Dump outward (recoil pushes me inward) instead.
            bool certain_release = u.len2() > 0 && d <= 2.6;
            if (certain_release) {
                o.has_hammer = true;
                o.hammer = u;
            } else {
                V outw = normv(m.pos);
                if (outw.len2() == 0) outw = u;
                V after = rest_pos(m.pos, m.vel + outw * (-c.hammer_recoil * m.charge), c.friction);
                if (after.maxabs() < L - 1.5) { o.has_hammer = true; o.hammer = outw; }
            }
            return o;
        }
        // Hammer is only wanted deep inside; dumped on real emergency, overcharge, or drifting far out.
        if ((edge_emergency || force_release || (!inside && radial > RING + 2.0)) && m.charge >= 0.15) {
            if (u.len2() > 0 && d <= c.hammer_reach - 0.15 &&
                rest_pos(e.pos, e.vel + u * (c.hammer_power * m.charge), c.friction).maxabs() > L - 0.3) {
                o.has_hammer = true; o.hammer = u; return o;
            }
            // Whiff outward: recoil points inward and releasing restores full walk speed.
            V outw = normv(m.pos);
            if (outw.len2() > 0) { o.has_hammer = true; o.hammer = outw; return o; }
        }
    }

    // ---- Offense: dash ram toward an edge. ----
    if (!busy && target >= 0 && m.skill == "dash" && m.charge > 0) {
        const Player& e = w.ps[target];
        V u = normv(e.pos - m.pos);
        double d = (e.pos - m.pos).len();
        double travel = c.dash_speed * m.charge * glide_sum(kp, 6) + 1.6;
        if (u.len2() > 0 && d <= travel && d <= 4.2 && fight_allowed && !jab_threat) {
            double ram = c.dash_speed * 0.95 * m.charge; // equal-mass elastic transfer
            V e_rest = rest_pos(e.pos, e.vel + u * ram, c.friction);
            bool lethal = e_rest.maxabs() > L + 0.3;
            V me_after = rest_pos(m.pos, m.vel + u * (c.dash_speed * m.charge), c.friction);
            bool i_am_safe = me_after.maxabs() < L - 4.0 &&
                             !spring_near(m.pos, u, travel + 1, 2.2);
            // A third body at the contact point can redirect the dash straight outward.
            bool crowded = false;
            V contact = m.pos + u * std::min(d, travel);
            for (int i = 0; i < w.n; ++i) {
                if (i == w.me || i == target || w.ps[i].out) continue;
                if ((w.ps[i].pos - contact).len() < 2.4) crowded = true;
            }
            bool charge_ok = !tiny || m.charge <= 0.4 || edge_emergency;
            if (lethal && i_am_safe && charge_ok && !crowded) {
                o.has_dash = true;
                o.dash = u;
                return o;
            }
        }
        if ((force_release && m.charge >= 0.2) || (edge_emergency && m.charge >= 0.15)) {
            // Spend charge safely: inward dash (with rotated fallbacks). A full dash slides ~14 units, so
            // non-emergency dumps must end inside the hold ring rather than overshooting to the far side.
            V in{0, 0};
            if (edge_emergency) in = inward_dash(1.2);
            else if (!tiny || m.charge <= 0.4) {
                V cand = inward_dash(2.0);
                if (cand.len2() > 0) {
                    V after = rest_pos(m.pos, m.vel + cand * c.dash_speed * m.charge, c.friction);
                    if (after.maxabs() <= RING + 1.5) in = cand;
                }
            }
            if (in.len2() > 0) {
                o.has_dash = true;
                o.dash = in;
                return o;
            }
            // No safe vector: fall through; an overcharge only costs the charge, not my position.
            if (target >= 0) {
                V u2 = normv(w.ps[target].pos - m.pos);
                V after = rest_pos(m.pos, m.vel + u2 * c.dash_speed * m.charge, c.friction);
                double need = m.charge > 0.6 ? 3.5 : 2.5;
                if (after.maxabs() < L - need && !spring_near(m.pos, u2, 4.0, 2.2)) {
                    o.has_dash = true;
                    o.dash = u2;
                    return o;
                }
            }
            // Last resort: low-charge inward dash only. A full-charge fling with no vetted vector
            // redirects off collisions/spring walls and throws us out — let it overcharge instead.
            if (m.charge <= 0.5) {
                V last = inward_dash(0.8);
                if (last.len2() > 0) { o.has_dash = true; o.dash = last; return o; }
            }
        }
    }

    // ---- Credit jab: an enemy already sliding out is free kill credit if I can tag them safely. ----
    if (!busy && m.attack_cd == 0 && !o.has_hammer && !o.has_dash && !jab_threat) {
        for (int i = 0; i < w.n; ++i) {
            if (i == w.me || w.ps[i].out) continue;
            const Player& e = w.ps[i];
            double d = (e.pos - m.pos).len();
            V u = normv(e.pos - m.pos);
            bool sliding_out = rest_pos(e.pos, e.vel, c.friction).maxabs() > L - 0.5;
            if (u.len2() > 0 && d <= c.attack_reach - 0.3 && sliding_out &&
                my_rest.maxabs() < L - 3.5 && safe_to_commit(w, 16) && jab_space) {
                o.has_attack = true;
                o.attack = u;
                break;
            }
        }
    }

    // ---- Offense: jab (free, but 9-tick windup locks me). Top bots jab far more often than we did. ----
    if (!busy && m.attack_cd == 0 && target >= 0 && !o.has_hammer && !o.has_dash && !o.has_attack &&
        !jab_threat && fight_allowed) {
        const Player& e = w.ps[target];
        double d = (e.pos - m.pos).len();
        V u = normv(e.pos - m.pos);
        bool sliding_out = rest_pos(e.pos, e.vel, c.friction).maxabs() > L - 0.5;
        bool slow_target = e.vel.len() < 0.15 || !e.act.empty();
        if (u.len2() > 0 && d <= 2.6 && (slow_target || sliding_out || d <= 1.9) &&
            safe_to_commit(w, 16) && my_rest.maxabs() < L - 3.0 && jab_space) {
            o.has_attack = true;
            o.attack = u;
        }
    }

    // ---- Offense: bomb throw. ----
    if (!busy && !o.has_attack && plan_bomb(w, o)) {
        // order populated inside plan_bomb
    }

    // ---- Charge management: hammer only inside the ring; dash held near the ring as escape insurance.
    // Far outside the ring, walk home at full speed instead of crawling with a held charge.
    // Note: spring_pinned must NOT suppress charging — the dash charge is what lets us escape next turn.
    // Far outside the ring: walk home at full speed (slow charge-crawl never gets back). Near the ring,
    // keep charging dash so an escape tool is ready.
    bool far_out = !inside && radial > RING + 2.5;
    bool suppress_charge = edge_emergency || bomb_danger || regen_danger || far_out;
    bool suppress_hammer = suppress_charge || edge_warning || momentum_danger || !inside;
    if (!busy) {
        if (m.skill == "hammer") {
            o.charge = "hammer"; // keep accumulating / hold (dump branches above handle retreats)
        } else if (m.skill == "dash") {
            o.charge = "dash";
        } else if (!suppress_charge) {
            double d = target >= 0 ? (w.ps[target].pos - m.pos).len() : 1e9;
            bool want_hammer = !suppress_hammer && m.hammer_cd == 0 && d < 8.0;
            o.charge = want_hammer ? "hammer" : "dash";
        }
    }

    // ---- Movement. ----
    V mv{0, 0};
    if (edge_emergency || edge_warning || momentum_danger) {
        mv = to_center;
    } else if (spring_pinned) {
        // Strong normal-away plus tangent; bypass generic wall steering so this cannot cancel itself out.
        mv = normv(spring_escape + to_center * 0.8);
    } else if (bomb_danger) {
        mv = normv(bomb_escape);
        if (mv.dot(to_center) < 0 && radial > L - 5) mv = normv(mv + to_center * 0.8);
    } else if (regen_danger) {
        mv = normv(regen_escape);
        if (mv.dot(to_center) < 0) mv = normv(mv + to_center * 0.6);
    } else if (jab_threat) {
        mv = normv(jab_dodge);
    } else {
        // Crowd control: two+ enemies within 5 and no strong hammer ready -> back out toward home.
        int close = 0;
        V centroid{0, 0};
        for (int i = 0; i < w.n; ++i) {
            if (i == w.me || w.ps[i].out) continue;
            if ((w.ps[i].pos - m.pos).len() < 5.0) { ++close; centroid = centroid + w.ps[i].pos; }
        }
        bool hammer_ready = m.skill == "hammer" && m.charge >= 0.6;
        if (close >= 2 && !hammer_ready) {
            V away = normv(m.pos - centroid * (1.0 / close));
            mv = normv(away + normv(home - m.pos) * 0.8);
        } else if (target >= 0 && fight_allowed) {
        const Player& e = w.ps[target];
        double d = (e.pos - m.pos).len();
        double want_d = tiny ? 2.2 : (alive <= 3 ? 2.6 : 3.4);
        if (d > want_d + 0.3) {
            mv = normv(e.pos - m.pos);
        } else if (d < want_d - 0.6) {
            mv = normv(m.pos - e.pos); // back off a touch
        } else {
            // strafe around target, direction chosen to keep me toward center
            V perp = normv(V(-(e.pos.y - m.pos.y), e.pos.x - m.pos.x));
            if (perp.dot(to_center) < 0) perp = perp * -1;
            mv = perp * 0.7;
        }
        // Never approach past my own safety radius.
        if (radial > fight_radius - 1.0) mv = normv(mv + to_center * 1.5);
        } else {
            // Travel home (full walk speed) or hold the inner ring at a wall-clearing home point.
            if ((m.pos - home).len() > 1.0) mv = normv(home - m.pos);
            else if (radial > RING) mv = to_center;
            else if (target >= 0) mv = normv(w.ps[target].pos - m.pos) * 0.3;
        }
    }
    if (!spring_pinned) avoid_walls(w, m.pos, mv);
    if (mv.len2() > 1e-8) {
        o.has_move = true;
        o.move = normv(mv);
    }
    return o;
}

J policy(const J& obs) {
    World w = parse_world(obs);
    Order o = decide(w);
    return to_json(o);
}

} // namespace

int main() {
    return kb_step_worker::run([](const J& obs, J& memory) {
        J order = policy(obs);
        if (!memory.is_object()) memory = J::object{};
        memory["t"] = obs["turn"];
        return std::make_pair(order, memory);
    });
}
