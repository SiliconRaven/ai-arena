#pragma once
// Forward-simulation helper: rebuilds the official engine from an observation and
// scores short lookahead candidates against simple opponent models.
#include "engine.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace botsim {
using J = mini_json::Json;
using knockback::Engine;

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
static inline J vecj(V v) { return J::array{v.x, v.y}; }
static inline double num(const J& j, const char* k) { return j.at(k).as_double(); }

inline J build_header(const J& obs) {
    const int n = int(num(obs, "n"));
    J rules = obs.at("rules");
    std::map<int, J> known;
    for (const J& w : obs.at("walls").as_array()) {
        int id = int(num(w, "id"));
        known[id] = J{{"id", id}, {"kind", w.at("kind")}, {"x1", num(w, "x1")},
                      {"y1", num(w, "y1")}, {"x2", num(w, "x2")}, {"y2", num(w, "y2")}};
    }
    for (const J& b : obs.at("broken").as_array()) {
        int id = int(num(b, "wall"));
        known[id] = J{{"id", id}, {"kind", "wall"}, {"x1", num(b, "x1")},
                      {"y1", num(b, "y1")}, {"x2", num(b, "x2")}, {"y2", num(b, "y2")}};
    }
    int maxid = -1;
    for (auto& kv : known) maxid = std::max(maxid, kv.first);
    J walls = J::array{};
    for (int i = 0; i <= maxid; ++i) {
        auto it = known.find(i);
        if (it != known.end())
            walls.push_back(it->second);
        else
            walls.push_back(J{{"id", i}, {"kind", "wall"}, {"x1", 1.0e6}, {"y1", 1.0e6},
                              {"x2", 1.0e6}, {"y2", 1.0e6}});
    }
    J spawns = J::array{};
    for (const J& p : obs.at("players").as_array())
        spawns.push_back(J::array{num(p, "x"), num(p, "y")});
    J invalid = J::array{};
    for (int i = 0; i < n; ++i) invalid.push_back(nullptr);
    J phases = knockback::setup_phases(rules);
    return J{{"players", n},      {"spawns", spawns}, {"walls", walls},
             {"shrink", phases},  {"invalid", invalid}, {"rules", rules}};
}

inline J build_state(const J& obs, int next_bomb) {
    const int n = int(num(obs, "n"));
    J st = J::object{};
    st["turn"] = int(num(obs, "turn")) - 1;
    st["done"] = false;
    st["half"] = num(obs.at("arena"), "half");
    st["players"] = obs.at("players");
    st["bombs"] = obs.at("bombs");
    st["next_bomb"] = next_bomb;
    st["last_hit"] = obs.at("last_hit");
    st["kills"] = obs.at("kills");
    J nulls = J::array{};
    for (int i = 0; i < n; ++i) nulls.push_back(nullptr);
    st["out_turn"] = nulls;
    st["out_tick"] = nulls;
    st["out_reason"] = nulls;
    st["invalid"] = nulls;
    J zeros = J::array{};
    for (int i = 0; i < n; ++i) zeros.push_back(0);
    st["program_errors"] = zeros;
    J standing = J::array{};
    for (const J& w : obs.at("walls").as_array()) standing.push_back(w.at("id"));
    st["walls"] = standing;
    st["broken"] = obs.at("broken");
    return st;
}

// ---- a single opponent / continuation policy ----
// model 0: aggressive, model 1: cautious.
inline J model_order(const Engine& e, int seat, int model) {
    const J& ps = e.players;
    const J& p = ps[seat];
    if (p.at("out").as_bool()) return J::object{};
    if (!p.at("act").is_null()) return J::object{};
    const double half = e.half;
    const J next = e.next();
    double plan_half = half;
    if (!next.is_null()) plan_half = std::min(half, num(next, "half"));
    V pos{num(p, "x"), num(p, "y")}, vel{num(p, "vx"), num(p, "vy")};
    double pe = edge_of(pos, plan_half);
    double charge = num(p, "charge");
    std::string skill = p.at("skill").is_null() ? "" : p.at("skill").as_string();

    // nearest reachable target
    int tgt = -1;
    double bd = 1e18;
    V tpos{0, 0}, tvel{0, 0};
    double tedge = 0;
    for (int i = 0; i < int(ps.size()); ++i) {
        if (i == seat) continue;
        const J& q = ps[i];
        if (q.at("out").as_bool()) continue;
        V qp{num(q, "x"), num(q, "y")};
        double d = len(qp - pos);
        double sc = 0.85 * d + 1.1 * edge_of(qp, half);
        if (num(q, "charge") > 0.4) sc += 2.5;
        if (sc < bd) {
            bd = sc;
            tgt = i;
            tpos = qp;
            tvel = {num(q, "vx"), num(q, "vy")};
            tedge = edge_of(qp, half);
        }
    }

    J o = J::object{};
    bool released = false;
    V rel = tgt >= 0 ? tpos - pos : V{0, 0};
    double dist = len(rel);
    if (charge > 0.05) {
        if (skill == "hammer") {
            // release at the first victim inside reach
            double best = 1e18;
            V bdir{0, 0};
            bool any = false;
            for (int i = 0; i < int(ps.size()); ++i) {
                if (i == seat) continue;
                const J& q = ps[i];
                if (q.at("out").as_bool()) continue;
                V qp{num(q, "x"), num(q, "y")};
                V d = qp - pos;
                double dd = len(d);
                if (dd > 3.19 || dd < 1e-6) continue;
                if (dd < best) {
                    best = dd;
                    bdir = unit(d);
                    any = true;
                }
            }
            if (any && (model == 0 || best < 3.19)) {
                o["hammer"] = vecj(bdir);
                released = true;
            }
        } else if (skill == "dash") {
            double reach = 1.6 + 0.9 * charge * 6.0;
            if (tgt >= 0 && dist < reach && dist > 1.2 &&
                (model == 0 || dist < 4.8 || tedge < 4.0)) {
                o["dash"] = vecj(unit(rel));
                released = true;
            }
        }
    }
    if (!released && num(p, "attack_cd") == 0 && tgt >= 0) {
        V rela = (tpos + tvel * 6.0) - (pos + vel * 6.0);
        if (len(rela) < 2.6 && len(rela) > 1e-6) {
            o["attack"] = vecj(unit(rela));
            released = true;
        }
    }
    if (!released && num(p, "bomb_cd") == 0 && model == 0 && tgt >= 0 && dist > 6.5 &&
        dist < 12.0 && ((e.turn + seat) % 6 == 0)) {
        V dir = unit(tpos + tvel * 8.0 - pos - vel * 6.0);
        o["bomb"] = J::object{{"dir", vecj(dir)}, {"fuse", 30}};
        released = true;
    }
    if (!released && num(p, "bar") > 0.05 && !(model == 1 && pe < 2.5)) {
        if (num(p, "hammer_cd") == 0)
            o["charge"] = J("hammer");
        else
            o["charge"] = J("dash");
    }

    V mv{0, 0};
    if (pe < 3.8) {
        V inward = unit(V{-pos.x, -pos.y});
        mv = inward * (1.0 + (3.8 - pe) * 0.5);
    } else if (tgt >= 0) {
        double want = model == 0 ? 2.6 : 3.5;
        if (dist > want + 0.4)
            mv = unit(rel);
        else if (dist < want - 0.9)
            mv = unit(rel) * -1.0;
        else {
            V t = perp(unit(rel));
            if (dot(t, unit(V{-pos.x, -pos.y})) < 0) t = t * -1.0;
            mv = t * 0.7;
        }
        // keep away from closer charged enemies
        for (int i = 0; i < int(ps.size()); ++i) {
            if (i == seat) continue;
            const J& q = ps[i];
            if (q.at("out").as_bool()) continue;
            double qc = num(q, "charge");
            if (qc < 0.4) continue;
            V qp{num(q, "x"), num(q, "y")};
            V d = pos - qp;
            double dd = len(d);
            if (dd < 4.5 && dd > 1e-6) mv = mv + unit(d) * ((4.5 - dd) * 0.6);
        }
    }
    if (len(mv) > 0.06) o["move"] = vecj(unit(mv));
    return o;
}

inline double score_state(const Engine& e, int me, double base_kills) {
    const J& p = e.players[me];
    if (p.at("out").as_bool()) return -1.0e6 + e.turn * 5.0;
    const double half = e.half;
    const J next = e.next();
    double plan_half = half;
    if (!next.is_null()) plan_half = std::min(half, num(next, "half"));
    V pos{num(p, "x"), num(p, "y")}, vel{num(p, "vx"), num(p, "vy")};
    double edge = edge_of(pos, half);
    double pedge = edge_of(pos, plan_half);
    V pred = pos + vel * 12.0;
    double fedge = edge_of(pred, plan_half);
    double s = 0;
    s += 150.0 * std::min(edge, 3.0) + 45.0 * std::min(pedge, 3.5) + 60.0 * std::min(fedge, 2.5);
    s += 380.0 * (e.kills[me].as_int() - base_kills);
    s += 22.0 * num(p, "charge") + 6.0 * num(p, "bar");
    if (!p.at("act").is_null()) s -= 6.0;
    // outward velocity near the rim
    double ax = std::abs(pos.x), ay = std::abs(pos.y);
    V outward{0, 0};
    if (ax >= ay) outward = {pos.x >= 0 ? 1.0 : -1.0, 0};
    else outward = {0, pos.y >= 0 ? 1.0 : -1.0};
    double vout = dot(vel, outward);
    if (vout > 0 && edge < 4.0) s -= vout * 90.0;
    // being tagged recently while near the rim is dangerous
    const J& lh = e.last_hit[me];
    if (!lh.is_null()) {
        int age = e.turn * 6 - int(lh.at(1).as_int());
        if (age < 70) s -= (70 - age) * (edge < 4.0 ? 0.9 : 0.25);
    }
    // enemy vulnerability gives a little pressure value, weighted by our weapon readiness
    double weapon = num(p, "charge");
    for (int i = 0; i < int(e.players.size()); ++i) {
        if (i == me) continue;
        const J& q = e.players[i];
        if (q.at("out").as_bool()) continue;
        V qp{num(q, "x"), num(q, "y")};
        double qe = edge_of(qp, half);
        double d = len(qp - pos);
        if (qe < 3.0) s += (3.0 - qe) * 7.0 / (1.0 + d * 0.18);
        if (d < 3.4 && weapon > 0.3) s += 4.0;
    }
    // bomb threat at the end state
    for (const J& b : e.bombs.as_array()) {
        V bp{num(b, "x"), num(b, "y")}, bv{num(b, "vx"), num(b, "vy")};
        double fuse = num(b, "fuse");
        if (fuse > 20) continue;
        V bpred = bp + bv * ((1.0 - std::pow(0.95, fuse)) / 0.05);
        double d = len(pos - bpred);
        if (d < 3.8) {
            double push = 1.4 * (1.0 - d / 3.5);
            V after = pos + unit(pos - bpred) * push * 15.0;
            if (edge_of(after, plan_half) < 0.8) s -= 120.0 * (1.0 - d / 3.8);
        }
    }
    return s;
}

}  // namespace botsim
