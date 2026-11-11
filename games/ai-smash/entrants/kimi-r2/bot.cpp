#include "include/step_worker.hpp"
#include <algorithm>
#include <cmath>
#include <string>

using J = mini_json::Json;

namespace {
double n(const J &j, const char *k) { return j.at(k).as_double(); }
int i(const J &j, const char *k) { return j.at(k).get<int>(); }
std::string text(const J &j, const char *k) {
    return j.at(k).is_string() ? j.at(k).as_string() : "";
}
void direction(J &o, double x, double y) { o["dir"] = J::array{x, y}; }
double cost(const J &rules, const char *skill) {
    return n(rules.at("moves").at(skill), "cost");
}
bool present(const J &p) { return !p.at("out").as_bool() && i(p, "respawn") == 0; }

J policy(const J &obs, J &memory) {
    J o = J::object{};
    const auto &ps = obs.at("players").as_array();
    int seat = i(obs, "seat");
    if (!memory.is_object() || !memory.contains("guard")) {
        memory = J::object{};
        memory["guard"] = J::array{0, 0, 0, 0};
        memory["counter"] = J::array{0, 0, 0, 0};
        memory["offense"] = J::array{0, 0, 0, 0};
    }
    for (int k = 0; k < int(ps.size()); ++k) {
        std::string a = text(ps[k], "act"), ph = text(ps[k], "phase");
        if (a == "guard") memory["guard"][k] = memory["guard"][k].as_int() + 1;
        if (a == "counter") memory["counter"][k] = memory["counter"][k].as_int() + 1;
        if (ph == "windup" && (a == "attack" || a == "charge" || a == "grab" || a == "uppercut" || a == "slam"))
            memory["offense"][k] = memory["offense"][k].as_int() + 1;
    }
    const J &me = ps.at(seat);
    if (me.at("out").as_bool() || i(me, "respawn") > 0) return o;
    const J &rules = obs.at("rules");
    double x = n(me, "x"), y = n(me, "y"), vx = n(me, "vx"), vy = n(me, "vy");
    double meter = n(me, "meter");
    bool grounded = me.at("grounded").as_bool();
    int jumps = i(me, "air_jumps"), stun = i(me, "hitstun"), gap = i(me, "gap");
    std::string act = text(me, "act"), phase = text(me, "phase");
    int phase_t = i(me, "phase_ticks");
    bool idle = act.empty() && stun == 0;
    bool ready = idle && gap == 0;
    double half = n(rules.at("stage").at("solids").at(0), "x2");

    int target = -1;
    double best = 1e100;
    for (int k = 0; k < int(ps.size()); ++k) {
        if (k == seat || !present(ps[k])) continue;
        const J &p = ps[k];
        double dx = n(p, "x") - x, dy = n(p, "y") - y;
        double score = std::sqrt(dx * dx + dy * dy) - n(p, "damage") * 0.022;
        score += i(p, "stocks") * 0.7 + i(p, "invuln") * 0.08;
        if (std::fabs(n(p, "x")) > half || n(p, "y") < -0.5) score -= 4.0;
        if (score < best) { best = score; target = k; }
    }

    bool outside = std::fabs(x) > half - 0.7 || y < -0.35 ||
                   (!grounded && y < 0.8 && std::fabs(x) > half - 1.5);
    if (outside) {
        double inward = x > 0.5 ? -1.0 : (x < -0.5 ? 1.0 : (vx > 0 ? -1.0 : 1.0));
        o["move"] = inward;
        if (stun == 0 && !grounded) {
            bool side_emergency = std::fabs(x) > half + 9.5 && (x * vx > 0 || std::fabs(x) > half + 11.0);
            bool need_jump = jumps > 0 && ((y < 1.0 && vy < 0.12) || vy < -0.42 || std::fabs(x) > half + 6.0);
            if (ready && side_emergency && meter >= cost(rules, "blink")) {
                o["skill"] = "blink";
                direction(o, inward, y < -1.0 ? 0.35 : (y > 18.0 ? -0.5 : 0.05));
            } else if (need_jump && (idle || act == "charge")) o["jump"] = true;
            else if (ready && y < 1.5 && vy < 0.15 && meter >= cost(rules, "uppercut")) {
                o["skill"] = "uppercut";
            } else if (ready && meter >= cost(rules, "blink") &&
                       (y < -5.0 || (jumps == 0 && vy < -0.25 && meter < cost(rules, "uppercut")))) {
                o["skill"] = "blink";
                direction(o, inward, y < -1.0 ? 0.35 : 0.05);
            }
        }
        return o;
    }

    double tick = n(obs, "tick");
    double closed = std::max(0.0, (tick - 3000.0) * 0.02);
    double blast_x = std::max(0.0, n(rules.at("stage").at("blast"), "x") - closed);
    double blast_top = std::max(1.0, n(rules.at("stage").at("blast"), "top") - closed * 0.9);
    if ((std::fabs(x) > blast_x - 5.0 || y > blast_top - 4.0) && tick > 2850) {
        o["move"] = std::max(-1.0, std::min(1.0, -x * 1.5));
        if (y > blast_top - 3.0 && grounded && y > 0.2) o["drop"] = true;
        if (ready && !grounded && y > blast_top - 3.0) {
            if (meter >= cost(rules, "blink") && (y > blast_top - 0.8 || vy > 0.15)) {
                o["skill"] = "blink";
                direction(o, -x, -std::max(1.0, y));
            } else if (meter >= cost(rules, "slam")) {
                o["skill"] = "slam";
            }
        }
        return o;
    }
    if (y > 19.0 && !grounded) {
        o["move"] = x > 0.3 ? -1.0 : (x < -0.3 ? 1.0 : 0.0);
        if (ready && y > 22.0 && vy > 0.0 && meter >= cost(rules, "blink")) {
            o["skill"] = "blink";
            direction(o, -x, -1.0);
        } else if (ready && meter >= cost(rules, "slam") && (y > 20.0 || vy > 0.3)) {
            o["skill"] = "slam";
        }
        return o;
    }

    for (const J &s : obs.at("shots").as_array()) {
        if (i(s, "owner") == seat) continue;
        double sx = n(s, "x"), sy = n(s, "y"), svx = n(s, "vx"), svy = n(s, "vy");
        double rx = x - sx, ry = y + 0.9 - sy;
        double vv = svx * svx + svy * svy;
        if (vv < 0.01) continue;
        double t = (rx * svx + ry * svy) / vv;
        if (t < -0.2 || t > 9.0) continue;
        double ex = rx - t * svx, ey = ry - t * svy;
        if (ex * ex + ey * ey > 2.0) continue;
        std::string kind = text(s, "kind");
        if (kind == "hook") {
            o["move"] = sx < x ? 1.0 : -1.0;
            if (grounded) o["jump"] = true;
            if (ready && t < 4.5 && meter >= cost(rules, "blink")) {
                o["skill"] = "blink";
                direction(o, sx < x ? 0.5 : -0.5, 1.0);
            }
            return o;
        }
        if (act == "guard") { o["skill"] = "guard"; return o; }
        if (ready) {
            if (t >= 4.0 && t < 7.0 && meter >= cost(rules, "counter")) o["skill"] = "counter";
            else o["skill"] = "guard";
            return o;
        }
    }

    int threat = -1;
    double threat_d = 1e100;
    for (int k = 0; k < int(ps.size()); ++k) {
        if (k == seat || !present(ps[k]) || i(ps[k], "invuln") > 0) continue;
        const J &p = ps[k];
        std::string a = text(p, "act"), ph = text(p, "phase");
        if (a.empty() || a == "guard" || a == "counter" || a == "charge" && ph == "charging") continue;
        double dx = n(p, "x") - x, dy = n(p, "y") - y;
        double d = std::sqrt(dx * dx + dy * dy);
        if (d < threat_d && d < 4.6) { threat_d = d; threat = k; }
    }
    if (threat >= 0) {
        const J &p = ps[threat];
        std::string a = text(p, "act"), ph = text(p, "phase");
        int left = i(p, "phase_ticks");
        bool close = threat_d < 3.8 || a == "slam" && threat_d < 4.5;
        if (close && (ph == "windup" || ph == "active")) {
            if (a == "grab") {
                o["move"] = n(p, "x") > x ? -1.0 : 1.0;
                if (ready && meter >= cost(rules, "blink")) {
                    o["skill"] = "blink";
                    direction(o, x - n(p, "x"), 0.25);
                } else if (ready && ph == "windup" && left >= 5) {
                    o["skill"] = "attack";
                    direction(o, n(p, "x") - x, n(p, "y") - y);
                } else if (grounded) {
                    o["jump"] = true;
                }
                return o;
            }
            if (act == "guard") { o["skill"] = "guard"; return o; }
            if (ready) {
                if (a != "slam" && ph == "windup" && left >= 4 && meter >= cost(rules, "counter")) {
                    o["skill"] = "counter";
                } else {
                    o["skill"] = "guard";
                }
                return o;
            }
        }
    }

    if (act == "charge" && phase == "charging") {
        if (target < 0) { o["skill"] = "guard"; return o; }
        const J &p = ps[target];
        double dx = n(p, "x") - x, dy = n(p, "y") - y;
        double d = std::sqrt(dx * dx + dy * dy);
        double lead_x = dx + n(p, "vx") * 8.0, lead_y = dy + n(p, "vy") * 8.0;
        double predicted_d = std::sqrt(lead_x * lead_x + lead_y * lead_y);
        o["move"] = dx > 0 ? 0.6 : -0.6;
        direction(o, lead_x, lead_y);
        std::string pa = text(p, "act"), pp = text(p, "phase");
        bool incoming = pp == "windup" || pp == "active";
        int charge = i(me, "charge");
        if (pa == "grab" && incoming && d < 3.6 && meter >= cost(rules, "blink")) {
            o["skill"] = "blink";
            direction(o, -dx, 0.2);
            return o;
        }
        if (incoming && d < 2.6 && charge < 2 &&
            (pa == "attack" || pa == "uppercut" || pa == "slam" || pa == "charge")) {
            o["skill"] = "guard";
            return o;
        }
        if ((d > 6.0 && charge > 4) || (charge >= 10 && predicted_d > 3.35)) {
            o["skill"] = "guard";
            return o;
        }
        if (charge < 2 || predicted_d > 3.35) o["skill"] = "charge";
        return o;
    }
    if (act == "guard") {
        if (threat >= 0) {
            std::string a = text(ps[threat], "act"), ph = text(ps[threat], "phase");
            bool melee_threat = a == "attack" || a == "charge" || a == "uppercut" || a == "slam" || a == "grab";
            if (melee_threat && (ph == "windup" || ph == "active")) o["skill"] = "guard";
        }
        return o;
    }
    if (!idle) {
        if (target >= 0 && act == "charge") o["move"] = n(ps[target], "x") > x ? 1.0 : -1.0;
        return o;
    }
    if (target < 0) {
        o["move"] = x > 0.5 ? -1.0 : (x < -0.5 ? 1.0 : 0.0);
        return o;
    }

    const J &p = ps[target];
    double px = n(p, "x"), py = n(p, "y"), pvx = n(p, "vx");
    double dx = px - x, dy = py - y, adx = std::fabs(dx);
    double distance = std::sqrt(dx * dx + dy * dy);
    std::string pa = text(p, "act"), pp = text(p, "phase");
    bool target_off = std::fabs(px) > half - 0.3 || py < -0.4;
    double move = dx > 0.15 ? 1.0 : (dx < -0.15 ? -1.0 : 0.0);
    if (target_off) {
        double edge = px > 0 ? std::min(10.8, half - 2.0) : -std::min(10.8, half - 2.0);
        move = edge > x + 0.3 ? 1.0 : (edge < x - 0.3 ? -1.0 : 0.0);
    }
    if ((pa == "counter" || pa == "attack" && pp == "active") && adx < 3.3 && i(p, "invuln") == 0) {
        if (pa == "counter") move = dx > 0 ? -0.25 : 0.25;
    }
    o["move"] = move;

    if (grounded && dy > 2.0 && y + 5.8 < blast_top) o["jump"] = true;
    else if (!grounded && y < std::min(15.5, blast_top - 5.5) && dy > 3.0 && vy < 0.12 && jumps > 0 && std::fabs(x) < half - 2.0) o["jump"] = true;
    if (grounded && y > 0.5 && dy < -2.2) o["drop"] = true;
    if (!ready || i(p, "invuln") > 0) return o;

    double predicted = dx + pvx * 3.0;
    if (pa == "counter" || pa == "guard") {
        if (adx < 2.75 && std::fabs(dy) < 1.8 && meter >= cost(rules, "grab")) {
            o["skill"] = "grab";
            direction(o, predicted, dy * 0.15);
        } else if (adx > 4.5 && adx < 10.5 && std::fabs(dy) < 1.5 && i(me, "hook_cd") == 0 &&
                   meter >= 0.82 && !target_off) {
            o["skill"] = "hook";
            direction(o, predicted + pvx * 5.0, dy + 0.1);
        }
        return o;
    }
    if (grounded && adx > 1.7 && adx < 4.8 && std::fabs(dy) < 1.35 && meter >= cost(rules, "slam") &&
        std::fabs(x) < half - 3.0 && (i(obs, "turn") + seat + target) % 4 != 0) {
        o["jump"] = true;
        return o;
    }
    if (!grounded && dy < -0.8 && dy > -8.0 && adx < 3.2 && meter >= cost(rules, "slam") &&
        std::fabs(x) < half - 3.0 && py > -0.2) {
        o["skill"] = "slam";
        return o;
    }
    if (std::fabs(dy) < 1.75 && adx < 2.8) {
        if (pa == "grab" && pp == "windup" && meter >= cost(rules, "blink")) {
            o["skill"] = "blink";
            direction(o, -dx, 0.2);
        } else if (pa == "charge" && pp == "charging") {
            o["skill"] = "attack";
            direction(o, predicted, dy * 0.12);
        } else {
            int mix = (i(obs, "turn") + seat * 7 + target * 3) % 7;
            if (mix == 0 && meter >= cost(rules, "grab") && adx < 2.45) o["skill"] = "grab";
            else o["skill"] = "attack";
            direction(o, predicted, dy * 0.12);
        }
        return o;
    }
    if (target_off && std::fabs(dy) < 3.0 && adx < 14.0 && meter >= 0.82) {
        o["skill"] = "wave";
        direction(o, dx + pvx * std::min(12.0, distance / 0.75), dy + n(p, "vy") * std::min(12.0, distance / 0.75));
        return o;
    }
    return o;
}
}

int main() {
    return sm_step_worker::run([](const J &obs, J &memory) {
        return std::make_pair(policy(obs, memory), memory);
    });
}
