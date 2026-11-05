// Knockback arena bot: edge-aware hammer/dash fighter.
#include "include/step_worker.hpp"
#include "include/sim.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
using J = mini_json::Json;

struct V {
    double x = 0, y = 0;
};
static inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y}; }
static inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y}; }
static inline V operator*(V a, double s) { return {a.x * s, a.y * s}; }
static inline double dot(V a, V b) { return a.x * b.x + a.y * b.y; }
static inline double len(V a) { return std::sqrt(dot(a, a)); }
static inline V unit(V a) {
    double l = len(a);
    if (l < 1e-12) return {0, 0};
    return {a.x / l, a.y / l};
}
static inline V perp(V a) { return {-a.y, a.x}; }
static inline double clampd(double x, double a, double b) { return std::max(a, std::min(b, x)); }
static inline double edge_of(V p, double half) { return half - std::max(std::abs(p.x), std::abs(p.y)); }
static inline double num(const J& j, const char* k) { return j.at(k).as_double(); }
static inline J vecj(V v) { return J::array{v.x, v.y}; }

struct PInfo {
    V pos, vel;
    double bar = 0, charge = 0, held = 0;
    bool out = false, act = false, has_skill = false;
    std::string skill;
    int attack_cd = 0, bomb_cd = 0, hammer_cd = 0;
    double edge = 0;
};
static PInfo read_player(const J& p, double half) {
    PInfo q;
    q.pos = {num(p, "x"), num(p, "y")};
    q.vel = {num(p, "vx"), num(p, "vy")};
    q.bar = num(p, "bar");
    q.charge = num(p, "charge");
    q.held = num(p, "held");
    q.out = p.at("out").as_bool();
    q.act = !p.at("act").is_null();
    if (!p.at("skill").is_null()) {
        q.has_skill = true;
        q.skill = p.at("skill").as_string();
    }
    q.attack_cd = int(num(p, "attack_cd"));
    q.bomb_cd = int(num(p, "bomb_cd"));
    q.hammer_cd = int(num(p, "hammer_cd"));
    q.edge = edge_of(q.pos, half);
    return q;
}
static inline V nearest_on_seg(V p, V a, V b) {
    V e = b - a;
    double l = dot(e, e);
    double t = l <= 0 ? 0.0 : clampd(dot(p - a, e) / l, 0.0, 1.0);
    return a + e * t;
}
static inline double travel(double v) { return v * 0.94 / 0.06; }

static J decide_base(const J& obs) {
    const double half = num(obs.at("arena"), "half");
    const J& nextJ = obs.at("arena").at("next");
    const int n = int(num(obs, "n"));
    const int you = int(num(obs, "you"));
    const J& players = obs.at("players");
    PInfo me = read_player(players[you], half);
    if (me.out) return J::object{};
    const J& R = obs.at("rules");
    const double hammer_reach = num(R.at("hammer"), "reach");
    const double attack_reach = num(R.at("attack"), "reach");
    const double attack_power = num(R.at("attack"), "power");
    const double hammer_power = num(R.at("hammer"), "power");
    const double dash_speed = num(R.at("dash"), "speed");
    const double blast_r = num(R.at("bomb"), "blast_radius");
    const double overcharge = num(R.at("bar"), "overcharge_ticks");

    std::vector<PInfo> ps;
    ps.reserve(n);
    for (int i = 0; i < n; ++i) ps.push_back(read_player(players[i], half));

    double plan_half = half;
    if (!nextJ.is_null()) plan_half = std::min(half, num(nextJ, "half"));
    double plan_edge = edge_of(me.pos, plan_half);
    // hard boundary guard: never let the shrinking rim catch us
    double box = std::max(0.8, plan_half - 3.0);
    V pred = me.pos + me.vel * 10.0;
    bool pred_out = std::max(std::abs(pred.x), std::abs(pred.y)) > plan_half - 1.2;
    bool outside = std::max(std::abs(me.pos.x), std::abs(me.pos.y)) > box;
    bool emergency = pred_out || plan_edge < 2.4;
    V safe_target{clampd(me.pos.x, -box, box), clampd(me.pos.y, -box, box)};
    if (!outside && emergency) safe_target = me.pos * 0.72;

    struct Wall { V a, b; };
    std::vector<Wall> walls;
    for (const J& w : obs.at("walls").as_array())
        walls.push_back({{num(w, "x1"), num(w, "y1")}, {num(w, "x2"), num(w, "y2")}});
    for (const J& w : obs.at("broken").as_array())
        walls.push_back({{num(w, "x1"), num(w, "y1")}, {num(w, "x2"), num(w, "y2")}});

    struct Bomb { V pos, vel; int fuse; };
    std::vector<Bomb> bombs;
    for (const J& b : obs.at("bombs").as_array())
        bombs.push_back({{num(b, "x"), num(b, "y")}, {num(b, "vx"), num(b, "vy")}, int(num(b, "fuse"))});

    // ---- threats ----
    V threat_dir{0, 0};
    V hammer_danger{0, 0};
    double threat = 0;
    for (int i = 0; i < n; ++i) {
        if (i == you) continue;
        const PInfo& p = ps[i];
        if (p.out) continue;
        double d = len(p.pos - me.pos);
        double reach = 0;
        if (p.has_skill && p.charge > 0.02) {
            if (p.skill == "hammer") reach = hammer_reach + 0.4;
            else reach = 2.0 + dash_speed * p.charge * 6.0;
        }
        if (p.has_skill && p.skill == "hammer" && p.charge > 0.05 && d < 5.0) {
            double dh = 5.0 - d;
            hammer_danger = hammer_danger + unit(me.pos - p.pos) * (dh * dh * 0.6);
        }
        if (p.attack_cd == 0 && !p.act && d < attack_reach + 0.8) reach = std::max(reach, 3.0);
        if (d < reach + 1.0) {
            double w = clampd((reach + 1.0 - d) / (reach + 1.0), 0.0, 1.0) * (reach > 3.2 ? 1.0 : 0.45);
            if (p.has_skill && p.charge > 0.25) w *= 1.0 + p.charge;
            if (w > threat) {
                threat = w;
                threat_dir = unit(me.pos - p.pos);
            }
        }
    }

    // ---- bomb danger ----
    V bomb_flee{0, 0};
    double bomb_danger = 0;
    for (const Bomb& b : bombs) {
        if (b.fuse > 14) continue;
        double t = std::max(1, b.fuse);
        V bp = b.pos + b.vel * ((1.0 - std::pow(0.95, t)) / 0.05);
        V d = me.pos - bp;
        double dist = len(d);
        if (dist > blast_r + 1.0) continue;
        double pw = 1.4 * (1.0 - std::min(1.0, dist / blast_r));
        V push = unit(d) * pw;
        V after = me.pos + push * 15.0 + me.vel * 10.0;
        double ae = edge_of(after, plan_half);
        if (ae < 1.2) {
            bomb_danger = std::max(bomb_danger, 1.0);
            V f = unit(V{-me.pos.x, -me.pos.y});
            V tang = perp(unit(d));
            if (dot(tang, f) < 0) tang = tang * -1.0;
            bomb_flee = bomb_flee + f * (1.2 - ae) * 4.0 + tang * 1.5 - unit(d) * 0.8;
        }
    }

    // ---- target selection ----
    int tgt = -1;
    double tgt_score = 1e18;
    for (int i = 0; i < n; ++i) {
        if (i == you) continue;
        const PInfo& p = ps[i];
        if (p.out) continue;
        double d = len(p.pos - me.pos);
        if (d > 14.0) continue;
        double score = 0.75 * d + 1.5 * p.edge + 6.0 * (p.charge > 0.35 ? 1.0 : 0.0);
        if (p.act) score -= 1.5;
        V outn = len(p.pos) > 1.5 ? unit(p.pos) : unit(p.pos - me.pos);
        if (dot(outn, unit(p.pos - me.pos)) > 0.25) score -= 1.6;
        if (p.edge < 3.0) score -= 0.8;
        if (score < tgt_score) {
            tgt_score = score;
            tgt = i;
        }
    }

    J order = J::object{};
    bool released = false, did_attack = false;
    bool holding = me.has_skill && me.charge > 1e-9;

    // ---- hammer release: hold out for high charge unless it clearly kills ----
    if (!me.act && holding && me.skill == "hammer") {
        double best = -1e18;
        V bestdir{0, 0};
        for (int i = 0; i < n; ++i) {
            if (i == you) continue;
            const PInfo& p = ps[i];
            if (p.out) continue;
            V d = p.pos - me.pos;
            double dist = len(d);
            bool ready = me.charge >= 0.70;
            bool closing = dot(p.vel, d) < -0.02 * dist;
            double reach2 = (ready && (closing || me.held > 30)) ? 4.1 : hammer_reach + 0.001;
            if (dist > reach2) continue;
            V u = unit(d);
            V vv = p.vel + u * (hammer_power * me.charge);
            V after = p.pos + vv * 15.0;
            double ae = edge_of(after, half);
            bool kill = ae < 0.5;
            bool duel = p.has_skill && p.charge > 0.05 && dist < 3.25 && me.charge >= 0.45;
            bool soft = p.edge < 2.4 && me.charge >= 0.4;
            if (!kill && !duel && !soft && !ready) continue;
            double sc = me.charge * 20.0 - dist * 1.2 + (4.0 - std::min(4.0, p.edge)) * 6.0;
            if (kill) sc += 300.0;
            if (duel) sc += 40.0;
            if (p.edge < 3.5) sc += 15.0;
            if (!kill && p.edge > 5.0 && me.charge < 0.8) sc -= 8.0;
            if (sc > best) {
                best = sc;
                bestdir = u;
            }
        }
        if (best > 0) {
            order["hammer"] = vecj(bestdir);
            released = true;
        }
    }

    // ---- dash release ----
    if (!me.act && holding && me.skill == "dash" && !released) {
        double best = -1e18;
        V bestdir{0, 0};
        for (int i = 0; i < n; ++i) {
            if (i == you) continue;
            const PInfo& p = ps[i];
            if (p.out) continue;
            V d = p.pos - me.pos;
            double dist = len(d);
            if (me.charge < 0.4) continue;
            if (dist < 1.2 || dist > 2.5 + me.charge * 12.0) continue;
            V u = unit(d);
            V outn = len(p.pos) > 1.5 ? unit(p.pos) : u;
            V vv = p.vel + u * (dash_speed * me.charge * 0.95);
            V after = p.pos + vv * 15.0;
            double ae = edge_of(after, half);
            bool kill = ae < 0.3;
            V myafter = me.pos + u * travel(dash_speed * me.charge);
            double myae = edge_of(myafter, plan_half);
            double myae_hit = edge_of(me.pos + u * std::min(dist, 15.0), plan_half);
            double sc = me.charge * 23.0 - dist * 0.7 + dot(u, outn) * 3.0;
            if (kill) sc += 180.0;
            if (p.edge < 5.0) sc += (5.0 - p.edge) * 3.0;
            if (myae < 0.8 && myae_hit < 1.0) sc -= 300.0;
            if (myae < 2.0) sc -= 60.0;
            if (sc > best) {
                best = sc;
                bestdir = u;
            }
        }
        if (best <= 20.0 && me.held > overcharge - 12) {
            double bestSafe = -1e18;
            for (int k = 0; k < 24; ++k) {
                double a = k * 3.14159265358979323846 * 2.0 / 24.0;
                V u{std::cos(a), std::sin(a)};
                V after = me.pos + u * travel(dash_speed * me.charge);
                double ae = edge_of(after, plan_half);
                if (ae > bestSafe) {
                    bestSafe = ae;
                    bestdir = u;
                }
            }
            if (bestSafe > 1.5) best = 100.0;
        }
        // escape burst toward the safest direction near the rim
        if (!released && me.charge >= 0.15 && (plan_edge < 3.2 || threat > 0.7)) {
            double freeBest = edge_of(me.pos, plan_half);
            V esc = unit(V{-me.pos.x, -me.pos.y});
            for (int k = 0; k < 24; ++k) {
                double a = k * 3.14159265358979323846 * 2.0 / 24.0;
                V u{std::cos(a), std::sin(a)};
                V after = me.pos + u * travel(dash_speed * me.charge);
                double ae = edge_of(after, plan_half);
                double score2 = ae * 2.0 + dot(u, unit(V{-me.pos.x, -me.pos.y})) * 0.6;
                if (score2 > freeBest) {
                    freeBest = score2;
                    esc = u;
                }
            }
            if (freeBest > 1.0) {
                bestdir = esc;
                best = 100.0;
            }
        }
        if (best > 0) {
            order["dash"] = vecj(bestdir);
            released = true;
        }
    }

    // ---- bomb throw: choose fuse and direction by predicted blast value ----
    if (!me.act && !released && !emergency && me.bomb_cd == 0) {
        static const int fuses[3] = {24, 30, 36};
        double best = -1e18;
        V bestdir{0, 0};
        int bestfuse = 24;
        for (int fi = 0; fi < 3; ++fi) {
            int fuse = fuses[fi];
            double fd = double(fuse);
            double bdec = (1.0 - std::pow(0.95, fd)) / 0.05;
            double edec = (1.0 - std::pow(0.94, fd)) / 0.06;
            double need = 1.8 + 0.5 * bdec;
            V selfp = me.pos + me.vel * bdec;
            for (int k = 0; k < 16; ++k) {
                double a = k * 3.14159265358979323846 * 2.0 / 16.0;
                V dir{std::cos(a), std::sin(a)};
                V land = me.pos + me.vel * bdec + dir * need;
                if (edge_of(land, half) < 0.5) continue;
                if (len(selfp - land) < 5.2) continue;
                double sc = 0;
                for (int i = 0; i < n; ++i) {
                    if (i == you) continue;
                    const PInfo& p = ps[i];
                    if (p.out) continue;
                    V pp = p.pos + p.vel * (edec * 0.6);
                    double dd = len(pp - land);
                    if (dd >= blast_r) continue;
                    V outn = len(p.pos) > 2.0 ? unit(p.pos) : unit(pp - me.pos);
                    double align = dot(unit(pp - land), outn);
                    double w = (blast_r - dd) * (0.8 + 1.6 * std::max(0.0, align));
                    if (p.edge < 6.0) w *= 1.5;
                    if (p.act) w *= 1.2;
                    sc += w;
                }
                if (sc > best) {
                    best = sc;
                    bestdir = dir;
                    bestfuse = fuse;
                }
            }
        }
        if (best > 1.5) {
            order["bomb"] = J::object{{"dir", vecj(bestdir)}, {"fuse", bestfuse}};
        }
    }

    // ---- normal attack ----
    bool melee_hammer_near = false;
    for (int i = 0; i < n; ++i) {
        if (i == you) continue;
        const PInfo& q = ps[i];
        if (q.out || !q.has_skill || q.skill != "hammer" || q.charge < 0.1) continue;
        if (len(q.pos - me.pos) < 4.7) melee_hammer_near = true;
    }
    if (!me.act && !released && !emergency && !melee_hammer_near && me.attack_cd == 0) {
        double best = -1e18;
        V bestdir{0, 0};
        for (int i = 0; i < n; ++i) {
            if (i == you) continue;
            const PInfo& p = ps[i];
            if (p.out) continue;
            V relafter = (p.pos + p.vel * 7.0) - (me.pos + me.vel * 7.0);
            double dist = len(relafter);
            if (dist > attack_reach) continue;
            V u = unit(relafter);
            V vv = p.vel + u * attack_power;
            V after = p.pos + vv * 15.0;
            double ae = edge_of(after, half);
            bool kill = ae < 0.3;
            double sc = 2.5 - dist + (kill ? 130.0 : 0.0) + (4.0 - std::min(4.0, p.edge)) * 5.0;
            if (p.act) sc += 4.0;
            if (p.charge > 0.4) sc += 3.0;
            if (edge_of(me.pos + me.vel * 7.0, plan_half) < 1.2) sc -= 60.0;
            if (sc > best) {
                best = sc;
                bestdir = u;
            }
        }
        if (best > 0) {
            order["attack"] = vecj(bestdir);
            did_attack = true;
        }
    }

    // ---- charge management ----
    std::string want;
    if (!me.act && !released && !did_attack && !emergency) {
        if (holding) {
            bool stale = me.held > overcharge - 8;
            if (!stale) {
                bool keep = me.skill == "hammer" && me.charge >= 0.75 && me.bar < 0.12 && me.held < 34;
                if (me.bar > 0.02 && !keep) want = me.skill;
            } else {
                std::string other = (me.skill == "hammer") ? "dash" : "hammer";
                if (!(other == "hammer" && me.hammer_cd > 0)) want = other;
            }
        } else if (me.bar > 0.05) {
            double near = 1e9;
            if (tgt >= 0) near = len(ps[tgt].pos - me.pos);
            bool want_hammer = me.hammer_cd == 0;
            bool worthwhile = near < 9.0 || me.bar > 0.85;
            if (plan_edge < 2.2) worthwhile = false;
            for (int i = 0; i < n; ++i)
                if (i != you && !ps[i].out && ps[i].has_skill && ps[i].skill == "hammer" &&
                    ps[i].charge > 0.05 && len(ps[i].pos - me.pos) < 3.9)
                    worthwhile = false;
            if (plan_edge < 3.0 && me.bar > 0.08) { want = "dash"; }
            else if (want_hammer && worthwhile) want = "hammer";
            if (!want_hammer) {
                bool dash_plan = tgt >= 0 && ps[tgt].edge < 6.0 && near < 12.5;
                if (dash_plan && worthwhile) want = "dash";
            }
        }
    }
    if (!want.empty() && !me.act) order["charge"] = J(want);

    // ---- movement ----
    if (!me.act && !released) {
        V acc{0, 0};
        if (emergency) {
            V toSafe = safe_target - me.pos;
            if (len(toSafe) < 0.3) toSafe = V{-me.pos.x, -me.pos.y};
            order["move"] = vecj(unit(toSafe));
            return order;
        }
        if (plan_edge < 6.0) {
            V inward = unit(V{-me.pos.x, -me.pos.y});
            if (len(me.pos) < 0.2) inward = {0, 0};
            double w = (6.0 - plan_edge) * (plan_edge < 3.0 ? 3.0 : 1.2);
            acc = acc + inward * w;
        }
        acc = acc + hammer_danger * 1.4;
        if (threat > 0.25) {
            V inward = unit(V{-me.pos.x, -me.pos.y});
            V side = perp(threat_dir);
            if (dot(side, inward) < 0) side = side * -1.0;
            double w = (0.8 + threat * 3.2) * (plan_edge < 6.0 ? 1.4 : 0.8);
            acc = acc + side * w + inward * (threat * 0.8);
        }
        if (bomb_danger > 0) acc = acc + bomb_flee * 1.6;
        if (tgt >= 0) {
            const PInfo& p = ps[tgt];
            V d = p.pos - me.pos;
            double dist = len(d);
            V outn = len(p.pos) > 2.0 ? unit(p.pos) : unit(d);
            double desire = 2.5;
            bool hammer_ready = me.has_skill && me.skill == "hammer" && me.charge > 0.35;
            if (hammer_ready) desire = 2.4;
            else if (me.hammer_cd > 0) desire = 3.6;
            if (p.charge > 0.5) desire = std::max(desire, 4.0);
            V stand = p.pos - outn * desire;
            double want_margin = (!nextJ.is_null() && plan_half < half - 0.2) ? 4.6 : 4.1;
            double se = edge_of(stand, plan_half);
            if (se < want_margin) stand = stand + unit(V{-stand.x, -stand.y}) * (want_margin - se);
            V toStand = stand - me.pos;
            V pull = len(toStand) < 0.35 ? V{0, 0} : unit(toStand);
            if (dist < 2.3 && dot(unit(d), outn) < 0.1) {
                V tang = perp(unit(d));
                if (dot(tang, outn) < 0) tang = tang * -1.0;
                pull = tang;
            }
            acc = acc + pull * 1.6;
        }
        for (int i = 0; i < n; ++i) {
            if (i == you || i == tgt) continue;
            const PInfo& p = ps[i];
            if (p.out) continue;
            V d = me.pos - p.pos;
            double dist = len(d);
            if (dist < 2.2 && dist > 1e-6) acc = acc + unit(d) * ((2.2 - dist) * 0.8);
        }
        for (const Wall& w : walls) {
            V q = nearest_on_seg(me.pos, w.a, w.b);
            double dist = len(me.pos - q);
            if (dist < 1.1) {
                V away = me.pos - q;
                if (len(away) < 1e-9) away = perp(unit(w.b - w.a));
                acc = acc + unit(away) * ((1.1 - dist) * 1.5);
            }
        }
        double al = len(acc);
        if (al > 0.05) order["move"] = vecj(unit(acc));
    }
    return order;
}


// ---- short lookahead: pick among a few tactical orders using the official engine ----
static std::vector<J> make_candidates(const J& obs, const J& base) {
    std::vector<J> cs;
    cs.push_back(base);
    const int n = int(num(obs, "n"));
    const int you = int(num(obs, "you"));
    const J& players = obs.at("players");
    const J& me = players[you];
    if (!me.at("act").is_null()) return cs;
    const int attack_cd = int(num(me, "attack_cd"));
    const int bomb_cd = int(num(me, "bomb_cd"));
    const bool holding = !me.at("skill").is_null() && num(me, "charge") > 0.02;
    const std::string skill = me.at("skill").is_null() ? "" : me.at("skill").as_string();
    const double charge = num(me, "charge");
    const double half = num(obs.at("arena"), "half");
    V mpos{num(me, "x"), num(me, "y")};
    std::string ch;
    if (base.contains("charge") && !base.at("charge").is_null()) ch = base.at("charge").as_string();
    std::vector<V> dirs;
    for (int k = 0; k < 8; ++k) {
        double a = k * 3.14159265358979323846 / 4.0;
        dirs.push_back({std::cos(a), std::sin(a)});
    }
    dirs.push_back({0, 0});
    if (base.contains("move") && !base.at("move").is_null())
        dirs.push_back({base.at("move").at(0).as_double(), base.at("move").at(1).as_double()});

    auto add_plain = [&](V mv, const std::string& c) {
        J o = J::object{};
        if (std::abs(mv.x) + std::abs(mv.y) > 1e-9) o["move"] = vecj(unit(mv));
        if (!c.empty()) o["charge"] = J(c);
        cs.push_back(o);
    };
    for (const V& d : dirs) {
        add_plain(d, ch);
        if (!ch.empty()) add_plain(d, "");
        if (holding) add_plain(d, "");
    }
    // hammer release variants: only a real kill or a charged swing
    if (holding && skill == "hammer") {
        for (int i = 0; i < n; ++i) {
            if (i == you) continue;
            const J& q = players[i];
            if (q.at("out").as_bool()) continue;
            V d{num(q, "x") - mpos.x, num(q, "y") - mpos.y};
            double dist = len(d);
            bool closing = (num(q, "vx") * d.x + num(q, "vy") * d.y) < -0.02 * dist;
            double reach2 = (charge >= 0.70 && (closing || num(me, "held") > 30)) ? 4.1 : 3.21;
            if (dist > reach2 || dist < 1e-6) continue;
            V u = unit(d);
            V after{num(q, "x") + (num(q, "vx") + u.x * 0.9 * charge) * 15.0,
                    num(q, "y") + (num(q, "vy") + u.y * 0.9 * charge) * 15.0};
            double ae = edge_of(after, half);
            if (charge < 0.55 && ae > 0.5) continue;
            J o = J::object{};
            o["hammer"] = vecj(u);
            cs.push_back(o);
        }
    }
    // dash release variants
    if (holding && skill == "dash" && charge >= 0.18) {
        for (int i = 0; i < n; ++i) {
            if (i == you) continue;
            const J& q = players[i];
            if (q.at("out").as_bool()) continue;
            V d{num(q, "x") - mpos.x, num(q, "y") - mpos.y};
            double dist = len(d);
            if (dist < 1.2 || dist > 2.5 + charge * 12.0) continue;
            for (const V& mv : dirs) {
                J o = J::object{};
                o["dash"] = vecj(unit(d));
                if (std::abs(mv.x) + std::abs(mv.y) > 1e-9) o["move"] = vecj(unit(mv));
                cs.push_back(o);
            }
        }
    }
    // dash escape candidate near the rim
    if (holding && skill == "dash" && charge >= 0.18) {
        V inward = unit(V{-mpos.x, -mpos.y});
        if (len(V{mpos.x, mpos.y}) > 0.1) {
            J o = J::object{};
            o["dash"] = vecj(inward);
            o["move"] = vecj(inward);
            cs.push_back(o);
        }
    }
    // jab variants
    if (attack_cd == 0) {
        for (int i = 0; i < n; ++i) {
            if (i == you) continue;
            const J& q = players[i];
            if (q.at("out").as_bool()) continue;
            V rel{num(q, "x") + num(q, "vx") * 6.0 - mpos.x - num(me, "vx") * 6.0,
                  num(q, "y") + num(q, "vy") * 6.0 - mpos.y - num(me, "vy") * 6.0};
            if (len(rel) > 2.8 || len(rel) < 1e-6) continue;
            J o = J::object{};
            o["attack"] = vecj(unit(rel));
            cs.push_back(o);
        }
    }
    (void)bomb_cd;
    (void)half;
    return cs;
}

static J search_order(const J& obs, const J& base) {
    const int n = int(num(obs, "n"));
    const int you = int(num(obs, "you"));
    static int g_next_bomb = 0;
    int maxid = -1;
    for (const J& b : obs.at("bombs").as_array()) maxid = std::max(maxid, int(num(b, "id")));
    g_next_bomb = std::max(g_next_bomb, maxid + 1);
    J hdr = botsim::build_header(obs);
    J st = botsim::build_state(obs, g_next_bomb);
    knockback::Engine eng(hdr);
    eng.restore(st);
    std::vector<J> opp[2];
    for (int m = 0; m < 2; ++m) {
        opp[m].resize(n);
        for (int i = 0; i < n; ++i) opp[m][i] = botsim::model_order(eng, i, m);
    }
    const double base_kills = double(obs.at("kills").at(you).as_int());
    std::vector<J> cands = make_candidates(obs, base);
    if (cands.size() > 40) cands.resize(40);
    double best = -1.0e18;
    size_t besti = 0;
    J o1 = J::array{}, o2 = J::array{};
    const auto t_start = std::chrono::steady_clock::now();
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        if (ci > 0) {
            double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            if (used > 0.022) break;
        }
        double sc = 0;
        for (int m = 0; m < 2; ++m) {
            eng.restore(st);
            o1 = J::array{};
            for (int i = 0; i < n; ++i) o1.push_back(i == you ? cands[ci] : opp[m][i]);
            eng.step(o1);
            if (eng.players[you].at("out").as_bool() || eng.done) {
                sc += botsim::score_state(eng, you, base_kills);
                continue;
            }
            o2 = J::array{};
            for (int i = 0; i < n; ++i) o2.push_back(botsim::model_order(eng, i, m));
            eng.step(o2);
            sc += botsim::score_state(eng, you, base_kills);
        }
        if (ci == 0) sc += 30.0;
        if (sc > best) {
            best = sc;
            besti = ci;
        }
    }
    return cands[besti];
}

static J decide(const J& obs) {
    J base = decide_base(obs);
    const J& me = obs.at("players").at(int(num(obs, "you")));
    if (me.at("out").as_bool()) return base;
    return search_order(obs, base);
}

int main() {
    return kb_step_worker::run([](const J& obs, J& memory) {
        J order;
        try {
            order = decide(obs);
        } catch (const std::exception& e) {
            J base = decide_base(obs);
            std::cout << "DECIDE_ERR " << e.what() << " turn=" << obs.at("turn").as_int()
                      << " base=" << base.dump() << " me=" << obs.at("me").dump() << std::endl;
            throw;
        }
        return std::make_pair(order, memory);
    });
}
