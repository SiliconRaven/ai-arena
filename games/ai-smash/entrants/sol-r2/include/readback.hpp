#pragma once

#include "sim.hpp"

namespace ring {

struct Habit {
    std::array<int, SkillCount> starts{};
    double travel = 0;
    int still = 0;
    double guard_rate() const {
        return double(starts[Guard]) / (4 + starts[Guard] + starts[Attack] + starts[Grab] + starts[Slam]);
    }
    double slam_rate() const {
        return double(starts[Slam]) / (4 + starts[Slam] + starts[Attack] + starts[Grab] + starts[Charge]);
    }
    double charge_rate() const {
        return double(starts[Charge]) / (4 + starts[Slam] + starts[Attack] + starts[Grab] + starts[Charge]);
    }
};

// Observations omit hit masks and hook state. Carry forward only information
// that can be inferred from two successive observations.
class Readback {
    World previous{};
    Order sent{};

    static double output_damage(const Fighter& a, const Rules& r) {
        if (a.act == Charge) return r.moves[Charge].damage * r.charge_power(a.charge);
        return r.moves[a.act].damage;
    }

    void infer_fighters(World& w) {
        const Rules& r = w.r();
        for (int i = 0; i < seats; ++i) {
            Fighter& a = w.f[i];
            const Fighter& old = previous.f[i];
            if (a.act != None && (old.act != a.act || (old.phase == Recover && a.phase == Windup))) {
                ++habit[i].starts[a.act];
            }
            if (!a.present() || !old.present() || a.stocks != old.stocks) continue;
            if (a.mobile() && old.mobile() && a.stun == 0 && a.damage == old.damage) {
                const double raw = std::abs(a.v.x) < 0.035 ? 0 : std::clamp(a.v.x / r.run_speed, -1.0, 1.0);
                habit[i].travel = 0.45 * habit[i].travel + 0.55 * raw;
                const bool resting = a.grounded && old.grounded && std::abs(a.p.x - old.p.x) < 0.075;
                habit[i].still = std::clamp(habit[i].still + (resting ? 1 : -3), 0, 36);
            }
            a.drop = std::max(0, old.drop - r.per_turn);
            if (old.grounded && a.p.y < old.p.y && old.p.y > r.solids.front().top && a.damage == old.damage) {
                for (const Platform& p : r.platforms) {
                    if (old.p.y == p.y && a.p.x > p.left - r.width && a.p.x < p.right + r.width) {
                        a.drop = r.drop_ticks;
                        break;
                    }
                }
            }
            if (i == me && sent.drop && old.grounded) a.drop = r.drop_ticks;
            a.last_hit = old.last_hit;
            a.hit_tick = old.hit_tick;
            a.slam_landing = old.slam_landing && !a.grounded && a.stun > 0;
            if (a.damage > old.damage + 0.01) {
                const double d = a.damage - old.damage;
                double best = 0;
                a.last_hit = -1;
                for (int k = 0; k < seats; ++k) {
                    if (k == i) continue;
                    const Fighter& before = previous.f[k];
                    Fighter& attacker = w.f[k];
                    const Fighter& attack = is_melee(attacker.act) ? attacker : before;
                    if (!attack.present() || (!is_melee(attack.act) && attack.act != Counter)) continue;
                    if (attack.phase == Charging || (attack.phase == Windup && attack.left > r.per_turn)) continue;
                    const MoveRule& m = r.moves[attack.act];
                    const double reach = m.radius + r.half_height + 1.7;
                    const double dist = norm2(old.p - attack.p - attack.dir * m.offset);
                    if (dist > reach * reach && attack.act != Counter) continue;
                    const double fit = 3.0 / (1 + dist) + (std::abs(output_damage(attack, r) - d) < 0.06 ? 2.5 : 0)
                                     + (attacker.phase == Active ? 0.7 : 0);
                    if (fit > best) {
                        a.last_hit = k;
                        best = fit;
                    }
                }
                for (int s = 0; s < previous.shot_count; ++s) {
                    const Shot& b = previous.shots[s];
                    if (b.owner == i || std::abs(b.damage - d) > 0.06) continue;
                    if (norm2(b.p + b.v * 1.7 - old.p - Vec{0, r.half_height}) > 12.0) continue;
                    bool still_flying = false;
                    for (int j = 0; j < w.shot_count; ++j) still_flying |= w.shots[j].id == b.id;
                    if (still_flying) continue;
                    a.last_hit = b.owner;
                    if (b.kind == Hook && a.stun == 0) {
                        a.pull = r.hook_ticks - r.per_turn;
                        a.pulled_by = b.owner;
                    }
                }
                if (a.stun == 0 && a.pull == 0 && std::abs(d - r.moves[Hook].damage) < 0.06) {
                    for (int k = 0; k < seats; ++k) {
                        const Fighter& before = previous.f[k];
                        const Fighter& after = w.f[k];
                        if (k == i || before.act != Hook || before.phase != Windup ||
                            before.left > r.per_turn || after.act != Hook || after.phase != Recover) continue;
                        const Vec shot = before.p + before.dir * (r.width + 0.5 + r.moves[Hook].speed);
                        if (norm2(shot - old.p) > 13.0) continue;
                        a.last_hit = a.pulled_by = k;
                        a.pull = r.hook_ticks - r.per_turn;
                        break;
                    }
                }
                a.slam_landing = r.slam_landing_cap >= 0 && a.stun > 0 && !a.grounded &&
                                 std::abs(a.v.x) < 0.02 && a.v.y < 0 &&
                                 std::abs(d - r.moves[Slam].damage) < 0.03;
                a.hit_tick = w.tick;
            } else if (old.pull > r.per_turn && w.f[old.pulled_by].present() &&
                       norm2(w.f[old.pulled_by].p - a.p) > r.hook_stop * r.hook_stop) {
                a.pull = old.pull - r.per_turn;
                a.pulled_by = old.pulled_by;
            }
        }
        for (int i = 0; i < seats; ++i) {
            Fighter& a = w.f[i];
            const Fighter& old = previous.f[i];
            if (a.act == Hook && a.phase == Active && a.left <= r.hook_ticks) {
                a.reeling = true;
                if (old.reeling) {
                    a.hook = old.hook;
                } else {
                    a.hook = a.p + Vec{0, r.half_height} + unit(a.v, a.dir) * 3.0;
                    for (int s = 0; s < previous.shot_count; ++s) {
                        const Shot& b = previous.shots[s];
                        if (b.kind != Hook || b.owner != i) continue;
                        for (int t = 1; t <= r.per_turn; ++t) {
                            const Vec p = b.p + b.v * t;
                            if (r.in_solid(p)) {
                                a.hook = p;
                                break;
                            }
                        }
                    }
                }
            }
            if (a.phase != Active || !is_melee(a.act)) continue;
            if (old.act == a.act && old.phase == Active && old.left >= a.left) a.hit_mask = old.hit_mask;
            for (int k = 0; k < seats; ++k) {
                const Fighter& t = w.f[k];
                if (k == i || !t.present()) continue;
                if (t.last_hit == i && t.hit_tick == w.tick) a.hit_mask |= 1 << k;
                if (old.act == a.act && old.phase == Active && (t.guarding() || t.countering())) {
                    const MoveRule& m = r.moves[a.act];
                    const double reach = m.radius + r.half_height;
                    if (norm2(previous.f[k].p - old.p - old.dir * m.offset) <= reach * reach) a.hit_mask |= 1 << k;
                }
            }
        }
    }

public:
    int me = 0;
    std::array<Habit, seats> habit{};

    World read(const J& obs, const Rules& rules) {
        World w;
        w.rules = &rules;
        w.tick = count(obs, "tick");
        me = count(obs, "seat");
        const bool continuous = previous.rules == &rules && previous.tick + rules.per_turn == w.tick;
        if (w.tick == 0) habit = {};
        for (int i = 0; i < seats; ++i) w.f[i] = read_fighter(obs.at("players").at(i), rules);
        for (const J& b : obs.at("shots").as_array()) {
            if (w.shot_count == shot_capacity) break;
            Shot s;
            s.p = {number(b, "x"), number(b, "y")};
            s.v = {number(b, "vx"), number(b, "vy")};
            s.kind = skill_from(b.at("kind"));
            s.id = count(b, "id");
            s.owner = count(b, "owner");
            s.ttl = count(b, "ttl");
            s.damage = rules.moves[s.kind].damage;
            s.power = rules.moves[s.kind].power;
            if (continuous) {
                for (int k = 0; k < previous.shot_count; ++k) {
                    const Shot& old = previous.shots[k];
                    if (old.id != s.id) continue;
                    const double reflect = old.owner == s.owner ? 1 : rules.counter_mult;
                    s.damage = old.damage * reflect;
                    s.power = old.power * reflect;
                    break;
                }
            }
            w.shots[w.shot_count++] = s;
            w.next_shot = std::max(w.next_shot, s.id + 1);
        }
        if (continuous) infer_fighters(w);
        previous = w;
        return w;
    }
    void record(Order order) { sent = order; }
};

}  // namespace ring
