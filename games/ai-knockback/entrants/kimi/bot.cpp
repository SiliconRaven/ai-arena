#include "include/step_worker.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using J = mini_json::Json;

struct V {
    double x = 0, y = 0;
    V() = default;
    V(double a, double b) : x(a), y(b) {}
    V operator+(V b) const { return {x + b.x, y + b.y}; }
    V operator-(V b) const { return {x - b.x, y - b.y}; }
    V operator-() const { return {-x, -y}; }
    V operator*(double k) const { return {x * k, y * k}; }
    V operator/(double k) const { return {x / k, y / k}; }
};
double dot(V a, V b) { return a.x * b.x + a.y * b.y; }
double norm2(V a) { return dot(a, a); }
double len(V a) { return std::sqrt(norm2(a)); }
V unit(V a) { double d = len(a); return d > 1e-9 ? a / d : V{1, 0}; }
V vec(double x, double y) { return {x, y}; }
J jv(V a) { return J::array{a.x, a.y}; }
double num(const J& j, const char* k) { return j.at(k).as_double(); }
int integer(const J& j, const char* k) { return static_cast<int>(j.at(k).as_int()); }
V pos(const J& p) { return {num(p, "x"), num(p, "y")}; }
V vel(const J& p) { return {num(p, "vx"), num(p, "vy")}; }
double edge(V p, double h) { return h - std::max(std::abs(p.x), std::abs(p.y)); }
V nearestEdge(V p) {
    if (std::abs(p.x) > std::abs(p.y)) return {p.x >= 0 ? 1.0 : -1.0, 0};
    return {0, p.y >= 0 ? 1.0 : -1.0};
}
V closest(V p, const J& w) {
    V a{num(w, "x1"), num(w, "y1")}, b{num(w, "x2"), num(w, "y2")};
    V d = b - a;
    double z = norm2(d);
    double t = z > 0 ? std::clamp(dot(p - a, d) / z, 0.0, 1.0) : 0;
    return a + d * t;
}
V drift(V p, V v, int ticks, double friction) {
    double keep = 1 - friction;
    double scale = keep * (1 - std::pow(keep, ticks)) / friction;
    return p + v * scale;
}
V aimAt(V from, V target, V tv, double speed) {
    double t = std::clamp(len(target - from) / std::max(0.2, speed), 0.0, 24.0);
    V lead = target + tv * (1 - std::pow(0.94, t)) / 0.06 * 0.94;
    return unit(lead - from);
}

J policy(const J& obs) {
    const J& me = obs.at("me");
    if (me.at("out").as_bool()) return J::object{};
    int you = integer(obs, "you"), tick = integer(obs, "tick");
    int n = integer(obs, "n");
    V p = pos(me), v = vel(me);
    double h = num(obs.at("arena"), "half");
    double futureH = h;
    if (!obs.at("arena").at("next").is_null()) futureH = num(obs.at("arena").at("next"), "half");
    int target = -1;
    double targetScore = 1e100;
    for (int i = 0; i < n; ++i) {
        const J& q = obs.at("players").at(i);
        if (i == you || q.at("out").as_bool()) continue;
        V qp = pos(q);
        double d = len(qp - p);
        double score = d - 3.2 * std::clamp((4.5 - edge(qp, h)) / 4.5, 0.0, 1.0);
        const J& lh = obs.at("last_hit").at(i);
        if (!lh.is_null() && lh.at(0).as_int() == you && tick - lh.at(1).as_int() < 90) score -= 2.0;
        if (score < targetScore) { targetScore = score; target = i; }
    }
    if (target < 0) return J::object{};
    const J& tp = obs.at("players").at(target);
    V q = pos(tp), qv = vel(tp), delta = q - p;
    double dist = len(delta);
    V toward = unit(delta);
    V outward = nearestEdge(q);
    double myEdge = edge(p, h), targetEdge = edge(q, h);
    std::string skill = me.at("skill").is_string() ? me.at("skill").as_string() : "";
    double charge = num(me, "charge"), bar = num(me, "bar");
    int held = integer(me, "held"), hammerCd = integer(me, "hammer_cd");
    bool busy = !me.at("act").is_null();

    V danger{0, 0};
    double dangerWeight = 0;
    for (const J& b : obs.at("bombs").as_array()) {
        int fuse = integer(b, "fuse");
        if (fuse > 42) continue;
        V bp = drift(pos(b), vel(b), fuse, 0.05);
        V future = drift(p, v, std::min(fuse, 30), 0.06);
        V away = future - bp;
        double d = len(away);
        if (d < 4.6) {
            double w = (4.6 - d) * (fuse < 18 ? 2.0 : 1.0);
            danger = danger + unit(away) * w;
            dangerWeight += w;
        }
    }
    V wallAvoid{0, 0};
    for (const J& w : obs.at("walls").as_array()) {
        V c = closest(p, w), away = p - c;
        double d = len(away);
        double limit = w.at("kind") == J("spring") ? 2.0 : 1.55;
        if (d < limit && d > 1e-8) {
            V u = away / d;
            double into = -dot(v, u);
            wallAvoid = wallAvoid + u * ((limit - d) * (into > 0 ? 2.2 : 0.7));
        }
    }
    for (const J& w : obs.at("broken").as_array()) {
        if (integer(w, "at") - tick > 30) continue;
        V c = closest(p, w), away = p - c;
        double d = len(away);
        if (d < 2.0) wallAvoid = wallAvoid + unit(away) * (2.0 - d) * 2.0;
    }

    J order = J::object{};
    V move = toward;
    bool emergency = myEdge < 1.45 || edge(p + v * 7.0, h) < 0.4;
    if (emergency) {
        V inward = -nearestEdge(p);
        move = inward;
        if (skill == "dash" && charge > 0.08 && !busy) {
            order["dash"] = jv(inward);
            order["move"] = jv(inward);
            return order;
        }
    }

    if (!busy && skill == "hammer" && charge >= 0.16) {
        int best = -1;
        double bestValue = -1e100;
        for (int i = 0; i < n; ++i) {
            const J& e = obs.at("players").at(i);
            if (i == you || e.at("out").as_bool()) continue;
            V d = pos(e) - p;
            double r = len(d);
            if (r > 3.18) continue;
            double value = charge * 12 + (5.0 - edge(pos(e), h)) * 2.0 - r * 0.1;
            if (value > bestValue) { bestValue = value; best = i; }
        }
        if (best >= 0) {
            V dir = unit(pos(obs.at("players").at(best)) - p);
            order["hammer"] = jv(dir);
            order["move"] = jv(-nearestEdge(p) * (myEdge < 2.5 ? 1.0 : 0.0) + dir * 0.2);
            return order;
        }
    }

    if (!busy && skill == "dash" && charge >= 0.45 && dist > 1.7 && dist < 10.5) {
        double speed = 0.9 * charge;
        V dir = aimAt(p, q, qv, speed);
        V landing = drift(p, v + dir * speed, 22, 0.06);
        bool safe = edge(landing, h) > 0.4 && edge(p + dir * std::min(dist, 5.0), h) > 0.3;
        if (safe && (targetEdge < 6.0 || dist < 4.2 || charge > 0.72)) {
            order["dash"] = jv(dir);
            order["move"] = jv(dir);
            return order;
        }
    }

    if (!busy && integer(me, "attack_cd") == 0 && myEdge > 1.8 && held < 28 && (n != 4 || !(skill == "hammer" && charge > 0.08))) {
        V futureQ = drift(q, qv, 9, 0.06);
        V futureP = drift(p, v, 9, 0.06);
        double futureD = len(futureQ - futureP);
        if ((dist < 3.1 || futureD < 2.75) && (targetEdge < 7 || dist < 2.5)) {
            order["attack"] = jv(unit(futureQ - futureP));
        }
    }

    if (!busy && integer(me, "bomb_cd") == 0 && dist > 4.2 && dist < 16.5 && myEdge > 1.5) {
        int fuse = dist < 11.5 ? 24 : dist < 13.2 ? 36 : dist < 14.4 ? 48 : dist < 15.5 ? 60 : 90;
        V lead = drift(q, qv, fuse, 0.06);
        V self = drift(p, v, std::min(fuse, 24), 0.06);
        V dir = unit(lead - self);
        order["bomb"] = J{{"dir", jv(dir)}, {"fuse", fuse}};
    }

    if (dangerWeight > 0.1) move = unit(danger) + (-nearestEdge(p)) * (myEdge < 3 ? 0.8 : 0.0);
    else if (emergency) move = -nearestEdge(p);
    else {
        V goal = q - outward * std::min(2.1, std::max(0.0, dist - 1.5));
        if (targetEdge > 6 && dist < 3.0 && skill == "hammer") goal = p - toward;
        move = unit(goal - p);
        if (dist < 2.15) move = move * 0.4 + (-toward) * 0.6;
        for (int i = 0; i < n; ++i) {
            if (i == you) continue;
            const J& e = obs.at("players").at(i);
            if (e.at("out").as_bool()) continue;
            V d = p - pos(e);
            double r = len(d);
            if (r < 3.8 && e.at("skill").is_string() && e.at("skill").as_string() == "hammer" && num(e, "charge") > 0.15) {
                V side{-toward.y, toward.x};
                if (dot(side, p) < 0) side = side * -1.0;
                move = unit(d) * 0.75 + side * 0.65;
            }
        }
    }
    move = move + wallAvoid * 1.4;
    double safeH = std::max(0.5, futureH - 0.8);
    if (std::max(std::abs(p.x), std::abs(p.y)) > safeH - 1.0) move = move + (-nearestEdge(p)) * 2.0;
    if (len(move) < 1e-8) move = -p;
    order["move"] = jv(unit(move));

    if (!busy) {
        if (skill == "hammer") {
            bool keepCharging = n != 4 ? (charge < 0.72 && held < 30 && dist < 10.0) : (((charge < 0.72 && dist < 10.0) || held >= 24) && held < 42);
            if (keepCharging && bar > 0.01) order["charge"] = "hammer";
        } else if (skill == "dash") {
            if (charge < 0.72 && held < 30 && bar > 0.01 && dist < 12.0) order["charge"] = "dash";
        } else if (hammerCd == 0 && dist < 9.0 && bar > 0.12) {
            order["charge"] = "hammer";
        } else if (dist > 8.0 && dist < 14.0 && bar > 0.75) {
            order["charge"] = "dash";
        }
    }
    return order;
}

#include "include/engine.hpp"
#include <chrono>
#include <set>
#include <vector>

J simpleOrder(V move, const std::string& charge = "") {
    J o{{"move", jv(unit(move))}};
    if (!charge.empty()) o["charge"] = charge;
    return o;
}

J opponentOrder(const knockback::Engine& e, int seat) {
    const J& me = e.players.at(seat);
    if (!me.at("act").is_null()) return J::object{};
    V p = pos(me);
    int target = -1;
    double best = 1e100;
    for (int i = 0; i < e.n; ++i) {
        if (i == seat || e.players.at(i).at("out").as_bool()) continue;
        double d = norm2(pos(e.players.at(i)) - p);
        if (d < best) { best = d; target = i; }
    }
    if (target < 0) return J::object{};
    const J& q = e.players.at(target);
    V d = pos(q) - p;
    double dist = len(d);
    std::string skill = me.at("skill").is_string() ? me.at("skill").as_string() : "";
    double charge = num(me, "charge");
    J o = simpleOrder(d);
    if (skill == "hammer" && charge > 0 && dist < 3.16) {
        o.erase("charge"); o["hammer"] = jv(unit(d));
    } else if (skill == "dash" && charge > 0.45 && dist < 8.5) {
        o.erase("charge"); o["dash"] = jv(unit(d));
    } else if (!skill.empty() && charge < 0.7 && integer(me, "held") < 28) {
        o["charge"] = skill;
    } else if (skill.empty() && integer(me, "hammer_cd") == 0 && dist < 7.0) {
        o["charge"] = "hammer";
    }
    if (integer(me, "attack_cd") == 0 && dist < 2.9 && skill.empty()) o["attack"] = jv(unit(d));
    if (integer(me, "bomb_cd") == 0 && dist > 5 && dist < 12 && (e.turn + seat) % 3 == 0)
        o["bomb"] = J{{"dir", jv(unit(d))}, {"fuse", 24}};
    return o;
}

knockback::Engine restoreEngine(const J& obs, const J& header) {
    knockback::Engine e(header);
    int n = integer(obs, "n");
    J state{{"turn", integer(obs, "turn") - 1}, {"done", false}, {"half", num(obs.at("arena"), "half")},
            {"players", obs.at("players")}, {"bombs", obs.at("bombs")}, {"next_bomb", 0},
            {"last_hit", obs.at("last_hit")}, {"kills", obs.at("kills")}, {"walls", J::array{}},
            {"broken", J::array{}}, {"out_turn", J::array{}}, {"out_tick", J::array{}},
            {"out_reason", J::array{}}, {"invalid", J::array{}}, {"program_errors", J::array{}}};
    for (const J& b : obs.at("bombs").as_array()) state["next_bomb"] = std::max(state["next_bomb"].as_int(), b.at("id").as_int() + 1);
    for (const J& w : obs.at("walls").as_array()) state["walls"].push_back(w.at("id"));
    for (const J& b : obs.at("broken").as_array()) state["broken"].push_back(J{{"wall", b.at("wall")}, {"at", b.at("at")}});
    for (int i = 0; i < n; ++i) {
        bool out = obs.at("players").at(i).at("out").as_bool();
        state["out_turn"].push_back(out ? J(integer(obs, "turn") - 1) : J(nullptr));
        state["out_tick"].push_back(out ? J(integer(obs, "tick")) : J(nullptr));
        state["out_reason"].push_back(out ? J("self") : J(nullptr));
        state["invalid"].push_back(nullptr);
        state["program_errors"].push_back(0);
    }
    e.restore(state);
    return e;
}

J makeHeader(const J& obs) {
    int n = integer(obs, "n"), maxId = -1;
    J::array walls, spawns, invalid;
    auto place = [&](J w, int id) {
        if (id < 0) return;
        while (static_cast<int>(walls.size()) <= id)
            walls.push_back(J{{"id", static_cast<int>(walls.size())}, {"kind", "wall"}, {"x1", 1000.0}, {"y1", 1000.0}, {"x2", 1000.0}, {"y2", 1000.0}});
        w["id"] = id; walls[id] = w; maxId = std::max(maxId, id);
    };
    for (const J& w : obs.at("walls").as_array()) place(w, integer(w, "id"));
    for (const J& w : obs.at("broken").as_array()) place(w, integer(w, "wall"));
    for (int i = 0; i < n; ++i) { spawns.push_back(J::array{0.0, 0.0}); invalid.push_back(nullptr); }
    J rules = obs.at("rules");
    return J{{"rules", rules}, {"players", n}, {"spawns", spawns}, {"invalid", invalid},
             {"walls", walls}, {"shrink", knockback::setup_phases(rules)}};
}

double stateValue(const knockback::Engine& e, int you) {
    const J& me = e.players.at(you);
    if (me.at("out").as_bool()) return -100000.0 + e.turn * 20.0;
    V p = pos(me), v = vel(me);
    double ed = edge(p, e.half);
    double value = e.kills.at(you).as_int() * 1800.0;
    value += std::min(ed, 5.0) * 12.0;
    if (ed < 2.5) value -= (2.5 - ed) * (2.5 - ed) * 45.0;
    V projected = drift(p, v, 12, 0.06);
    double futureEdge = edge(projected, e.half);
    if (futureEdge < 0) value -= 2500.0 + 1000.0 * -futureEdge;
    else if (futureEdge < 1.0) value -= (1.0 - futureEdge) * 180.0;
    int nearest = -1;
    double nearD = 1e100;
    for (int i = 0; i < e.n; ++i) {
        if (i == you) continue;
        const J& q = e.players.at(i);
        if (q.at("out").as_bool()) { value += 240.0; continue; }
        V qp = pos(q);
        double d = len(qp - p);
        if (d < nearD) { nearD = d; nearest = i; }
        double qe = edge(qp, e.half);
        if (qe < 3.0) value += (3.0 - qe) * 13.0;
        V qProjected = drift(qp, vel(q), 15, 0.06);
        if (edge(qProjected, e.half) < 0) {
            const J& h = e.last_hit.at(i);
            value += !h.is_null() && h.at(0).as_int() == you ? 750.0 : 120.0;
        }
    }
    if (nearest >= 0 && me.at("skill").is_string()) {
        if (me.at("skill").as_string() == "hammer") value += std::max(0.0, 7.0 - nearD) * 4.0;
        value += num(me, "charge") * 12.0;
    }
    value += num(me, "bar") * 3.0;
    for (const J& b : e.bombs.as_array()) {
        int fuse = integer(b, "fuse");
        if (fuse > 30) continue;
        V bp = drift(pos(b), vel(b), fuse, 0.05);
        double d = len(drift(p, v, std::min(fuse, 18), 0.06) - bp);
        if (d < 3.5) value -= (3.5 - d) * 150.0;
    }
    return value;
}

double rollout(knockback::Engine e, int you, const J& first, int turns) {
    double value = 0;
    for (int t = 0; t < turns && !e.done; ++t) {
        J orders = J::array{};
        for (int s = 0; s < e.n; ++s) {
            if (e.players.at(s).at("out").as_bool()) orders.push_back(nullptr);
            else if (s == you) orders.push_back(t == 0 ? first : policy(e.observation(you)));
            else orders.push_back(opponentOrder(e, s));
        }
        e.step(orders);
        for (const J& ev : e.events.as_array()) {
            std::string type = ev.at("type").as_string();
            if ((type == "hammer" || type == "attack") && integer(ev, "seat") == you)
                value += static_cast<double>(ev.at("hits").size()) * (type == "hammer" ? 65.0 : 30.0);
            if (type == "bomb_explode" && integer(ev, "owner") == you) {
                for (const J& hit : ev.at("hits").as_array()) if (hit.as_int() != you) value += 85.0;
                for (const J& hit : ev.at("hits").as_array()) if (hit.as_int() == you) value -= 70.0;
            }
        }
        double now = stateValue(e, you);
        if (now < -50000) return now + value;
        value += now * (t + 1 == turns ? 1.0 : 0.08);
    }
    return value;
}

J choose(const J& obs) {
    static J header;
    static int cachedN = 0;
    int you = integer(obs, "you"), n = integer(obs, "n");
    if (integer(obs, "turn") == 1 || cachedN != n) { header = makeHeader(obs); cachedN = n; }
    J fallback = policy(obs);
    auto start = std::chrono::steady_clock::now();
    knockback::Engine base = restoreEngine(obs, header);
    std::vector<J> candidates{fallback};
    const J& me = obs.at("me");
    V p = pos(me);
    double currentHalf = num(obs.at("arena"), "half");
    if (n == 4 && me.at("act").is_null() && me.at("skill").is_string() && me.at("skill").as_string() == "dash" && num(me, "charge") > 0.08 &&
        (edge(p, currentHalf) < 1.6 || edge(p + vel(me) * 7.0, currentHalf) < 0.4)) return fallback;
    std::string skill = me.at("skill").is_string() ? me.at("skill").as_string() : "";
    double charge = num(me, "charge");
    bool busy = !me.at("act").is_null();
    std::vector<int> targets;
    for (int i = 0; i < n; ++i) if (i != you && !obs.at("players").at(i).at("out").as_bool()) targets.push_back(i);
    std::sort(targets.begin(), targets.end(), [&](int a, int b) {
        return norm2(pos(obs.at("players").at(a)) - p) < norm2(pos(obs.at("players").at(b)) - p);
    });
    if (!busy && skill == "hammer" && charge > 0.04) {
        for (int i : targets) {
            V d = pos(obs.at("players").at(i)) - p;
            if (len(d) > 3.3) continue;
            for (double angle : {-0.22, 0.0, 0.22}) {
                V u = unit(d), dir{u.x * std::cos(angle) - u.y * std::sin(angle), u.x * std::sin(angle) + u.y * std::cos(angle)};
                J o = fallback; o.erase("charge"); o.erase("attack"); o["hammer"] = jv(dir); candidates.push_back(o);
            }
        }
    }
    for (int k = 0; k < 16; ++k) {
        double a = 6.283185307179586 * k / 16;
        V dir{std::cos(a), std::sin(a)};
        J o = fallback;
        o.erase("dash"); o.erase("hammer"); o.erase("attack"); o.erase("bomb");
        if (skill.empty()) o.erase("charge");
        o["move"] = jv(dir); candidates.push_back(o);
    }
    if (!busy && skill == "dash" && (charge >= 0.3 || edge(p, num(obs.at("arena"), "half")) < 2.5)) {
        for (int k = 0; k < 12; ++k) {
            double a = 6.283185307179586 * k / 12;
            V dir{std::cos(a), std::sin(a)};
            J o = fallback; o.erase("charge"); o.erase("attack"); o.erase("bomb"); o["dash"] = jv(dir); o["move"] = jv(dir); candidates.push_back(o);
        }
        for (int i : targets) {
            J o = fallback; o.erase("charge"); o.erase("attack"); o.erase("bomb"); o["dash"] = jv(aimAt(p, pos(obs.at("players").at(i)), vel(obs.at("players").at(i)), 0.9 * charge)); candidates.push_back(o);
        }
    }
    for (int k = 0; k < 12; ++k) {
        double a = 6.283185307179586 * k / 12;
        J o = fallback; o["move"] = jv(V{std::cos(a), std::sin(a)}); candidates.push_back(o);
    }
    if (!busy) {
        for (const std::string& wanted : {std::string(), std::string("dash"), std::string("hammer")}) {
            if (wanted == skill || (wanted == "hammer" && integer(me, "hammer_cd") > 0)) continue;
            J o = fallback; o.erase("dash"); o.erase("hammer"); o.erase("attack"); o.erase("bomb");
            if (wanted.empty()) o.erase("charge"); else o["charge"] = wanted;
            candidates.push_back(o);
        }
    }
    if (!busy && integer(me, "bomb_cd") == 0) {
        int count = std::min<int>(3, targets.size());
        for (int k = 0; k < count; ++k) {
            const J& q = obs.at("players").at(targets[k]);
            for (int fuse : {24, 36, 48}) {
                V lead = drift(pos(q), vel(q), fuse, 0.06);
                J o = fallback; o.erase("attack"); o["bomb"] = J{{"dir", jv(unit(lead - p))}, {"fuse", fuse}}; candidates.push_back(o);
            }
        }
    }
    if (!busy && integer(me, "attack_cd") == 0 && (n != 4 || !(skill == "hammer" && charge > 0.08))) {
        int count = std::min<int>(2, targets.size());
        for (int k = 0; k < count; ++k) {
            const J& q = obs.at("players").at(targets[k]);
            J o = fallback; o["attack"] = jv(aimAt(p, pos(q), vel(q), 0.4)); candidates.push_back(o);
        }
    }
    std::set<std::string> seen;
    double best = -1e200;
    J bestOrder = fallback;
    int evaluated = 0;
    for (const J& o : candidates) {
        std::string key = o.dump();
        if (!seen.insert(key).second) continue;
        auto now = std::chrono::steady_clock::now();
        if (evaluated > 0 && std::chrono::duration<double, std::milli>(now - start).count() > 48.0) break;
        int turns = o.contains("bomb") && !o.at("bomb").is_null() ? 5 : 4;
        double value = rollout(base, you, o, turns);
        ++evaluated;
        if (value > best) { best = value; bestOrder = o; }
    }
    return bestOrder;
}

int main() {
    return kb_step_worker::run([](const J& obs, J& memory) {
        memory = J::object{};
        return std::make_pair(choose(obs), memory);
    });
}
