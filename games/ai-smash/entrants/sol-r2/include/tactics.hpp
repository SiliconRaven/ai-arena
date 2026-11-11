#pragma once

#include "readback.hpp"

namespace ring {

inline int ready_in(const Fighter& a, const Rules& r) {
    if (a.pull > 0 || a.stun > 0) return a.pull + a.stun;
    if (a.act == None) return a.gap;
    if (a.charging()) return 0;
    if (a.guarding()) return r.gap;
    const MoveRule& m = r.moves[a.act];
    if (a.phase == Recover) return a.left + r.gap;
    return a.left + m.recover + r.gap + (a.phase == Windup ? m.active : 0);
}

inline Vec project(const World& w, const Fighter& a, int ticks, bool locked = false) {
    const Rules& r = w.r();
    const double n = ticks;
    Vec p = a.p;
    if (a.grounded && !a.stun && (locked || !a.mobile())) {
        if (!a.guarding()) p.x += a.v.x * std::min(3.0, n * 0.75);
        return p;
    }
    if (a.blinking()) {
        return p + a.v * std::min(n, double(a.left)) + a.v * (std::max(0.0, n - a.left) * 0.2);
    }
    const double drift = a.stun > 0 ? 0.015 : (a.grounded ? 0.0 : 0.0065);
    p.x += a.v.x * n * std::max(0.43, 1 - drift * (n + 1));
    if (a.grounded) return p;
    p.y += std::max(a.v.y - 0.5 * r.gravity * (n + 1), -r.max_fall) * n;
    if (p.y >= a.p.y) return p;
    for (const Platform& b : r.platforms) {
        if (a.drop < ticks && a.p.y >= b.y && p.y <= b.y && p.x > b.left - r.width && p.x < b.right + r.width) p.y = b.y;
    }
    for (const Solid& s : r.solids) {
        if (a.p.y >= s.top && p.y <= s.top && p.x > s.left - r.width && p.x < s.right + r.width) p.y = s.top;
    }
    return p;
}

inline bool offstage(const World& w, const Fighter& a) {
    const Solid& s = w.r().solids.front();
    return a.p.y < s.top - 0.25 || a.p.x < s.left - 0.4 || a.p.x > s.right + 0.4;
}

inline double stage_center(const World& w) {
    const Solid& s = w.r().solids.front();
    return (s.left + s.right) / 2;
}
inline double steer(const World& w, const Fighter& a, double x) {
    const Rules& r = w.r();
    const double error = x - a.p.x;
    const double accel = a.grounded ? r.ground_accel : r.air_accel;
    const double braking = a.v.x * std::abs(a.v.x) / (2 * accel);
    if (std::abs(error) > 1.8) return sign(error);
    return std::clamp((error - 0.75 * braking) * 1.9, -1.0, 1.0);
}

inline int target_for(const World& w, int i, bool nearest = false) {
    const Fighter& a = w.f[i];
    double best = 1e9;
    int target = -1;
    for (int k = 0; k < seats; ++k) {
        const Fighter& b = w.f[k];
        if (i == k || !b.present() || b.invuln > 12) continue;
        const Vec d = b.p - a.p;
        double value = std::sqrt(d.x * d.x + d.y * d.y * 1.24);
        if (!nearest) {
            value += 0.11 * std::abs(b.p.x) - std::min(3.0, b.damage * 0.030);
            value -= std::min(1.2, b.stun * 0.09);
            if (offstage(w, b) && !offstage(w, a)) value += 4.0;
        }
        if (value < best) {
            target = k;
            best = value;
        }
    }
    return target;
}

inline double waypoint(const World& w, int i, int target) {
    const Fighter& a = w.f[i];
    const Rules& r = w.r();
    const Solid& stage = r.solids.front();
    const Bounds bounds = r.bounds(w.tick + 24);
    const double center = stage_center(w);
    const double safe = std::max(0.0, std::min(bounds.x - 1.45, (stage.right - stage.left) * 0.40));
    double x = std::clamp(a.p.x, center - safe * 0.43, center + safe * 0.43);
    if (target >= 0 && !offstage(w, a)) {
        const Fighter& b = w.f[target];
        const Vec p = project(w, b, 8);
        const double dx = a.p.x - p.x;
        const double spacing = a.invuln > 10 ? 1.55 : (b.stun > 5 ? 1.9 : 2.75);
        x = p.x + (std::abs(dx) < 0.7 ? sign(center - p.x) : sign(dx)) * spacing;
        if (std::abs(p.y - a.p.y) > 2.9) x = p.x + sign(dx) * 1.1;
        if (b.guarding() && a.meter < r.moves[Grab].cost) x = p.x + sign(dx) * 3.4;
    }
    for (int k = 0; k < seats; ++k) {
        const Fighter& b = w.f[k];
        if (k == i || !b.present() || b.invuln <= 10 || a.invuln >= b.invuln) continue;
        const Vec d = a.p - b.p;
        if (std::abs(d.y) < 4.5 && std::abs(d.x) < 6.5) {
            x += sign(std::abs(d.x) < 0.35 ? center - b.p.x : d.x) * (6.5 - std::abs(d.x)) * 0.9;
        }
    }
    return std::clamp(x, center - safe, center + safe);
}

inline double slam_distance(const World& w, int i, int k) {
    const Fighter& a = w.f[i];
    const Fighter& b = w.f[k];
    const Rules& r = w.r();
    const MoveRule& m = r.moves[Slam];
    const int delay = m.windup + std::min(2, ready_in(a, r));
    const Vec before = project(w, a, delay, true);
    const Vec after = before + Vec{a.v.x * 0.27, -r.max_fall};
    if (after.y < r.solids.front().top && !offstage(w, a)) return 1e3;
    return norm2(project(w, b, delay + 1) - after + Vec{0, m.offset});
}

inline Order simple_rescue(const World& w, int i) {
    const Rules& r = w.r();
    const Fighter& a = w.f[i];
    const Solid& s = r.solids.front();
    const Bounds b = r.bounds(w.tick + 8);
    const double toward = sign(stage_center(w) - a.p.x);
    const double edge = toward < 0 ? s.right : s.left;
    Order o;
    o.move = toward;
    const bool below = a.p.y < s.top - 0.4;
    if (a.stun >= r.per_turn || a.pull > 0) return o;
    const bool soon = a.act == None && ready_in(a, r) < r.per_turn;
    if (soon && a.meter + r.refill >= r.moves[Blink].cost &&
        (below || std::abs(a.p.x + a.v.x * 6) > b.x - 1.3)) {
        o.skill = Blink;
        o.dir = {toward, below ? (a.p.y < s.bottom ? 2.5 : 1.2) : 0.35};
    } else if (a.air > 0 && a.mobile() && a.v.y < -0.12 && (below || std::abs(a.p.x - edge) > 1.0)) {
        o.jump = true;
    } else if (soon && below && a.v.y < 0.0 && a.meter + r.refill >= r.moves[Upper].cost) {
        o.skill = Upper;
    }
    return o;
}

inline Vec lead(const World& w, int i, int k, Skill skill, int extra = 0) {
    const Rules& r = w.r();
    const Fighter& a = w.f[i];
    const Fighter& b = w.f[k];
    const MoveRule& m = r.moves[skill];
    int delay = m.windup + 1 + extra + std::min(5, ready_in(a, r));
    if (skill == Wave || skill == Hook) {
        delay += int(std::clamp((std::sqrt(norm2(b.p - a.p)) - 1.2) / m.speed, 0.0, 22.0));
    }
    return project(w, b, delay) - project(w, a, m.windup + 1 + extra, true);
}

struct Threat {
    Skill skill = None;
    int in = 99, from = -1;
    bool blockable() const { return skill != Grab && skill != Hook; }
    bool reflectable() const { return blockable() && skill != Slam; }
};

inline Threat incoming(const World& w, int i, int horizon = 9) {
    const Rules& r = w.r();
    const Fighter& a = w.f[i];
    Threat threat;
    if (a.invuln > horizon) return threat;
    for (int k = 0; k < seats; ++k) {
        const Fighter& b = w.f[k];
        if (i == k || !b.present() || !is_melee(b.act) || b.hit_mask & (1 << i)) continue;
        if (b.phase != Active && b.phase != Windup) continue;
        const MoveRule& m = r.moves[b.act];
        const int start = b.phase == Active ? 1 : b.left;
        const int last = b.phase == Active ? b.left - 1 : start + m.active - 1;
        if (start > horizon || start >= threat.in || norm2(b.p - a.p) > 130) continue;
        const Vec dir = b.act == Slam ? Vec{0, -1} : (b.act == Upper ? Vec{0, 1} : b.dir);
        const double reach = m.radius + r.half_height + 0.21;
        for (int t = start; t <= std::min(horizon, last) && t < threat.in; ++t) {
            Vec p = project(w, b, t);
            if (b.phase == Windup && b.act == Slam) p.y = project(w, b, start - 1).y - r.max_fall * (t - start + 1);
            if (b.phase == Windup && b.act == Upper) p.y = project(w, b, start - 1).y + (r.upper_rise - r.gravity) * (t - start + 1);
            if (t >= a.invuln && norm2(project(w, a, t) - p - dir * m.offset) <= reach * reach) threat = {b.act, t, k};
        }
    }
    for (int s = 0; s < w.shot_count; ++s) {
        const Shot& b = w.shots[s];
        if (b.owner == i || norm2(b.p - a.p) > 95) continue;
        const double reach = r.moves[b.kind].radius + r.half_height + 0.1;
        for (int t = 1; t <= std::min(b.ttl, horizon) && t < threat.in; ++t) {
            if (t < a.invuln) continue;
            if (norm2(project(w, a, t) + Vec{0, r.half_height} - b.p - b.v * t) <= reach * reach) threat = {b.kind, t, b.owner};
        }
    }
    return threat;
}

enum Model { Observe, Pressure, Cover, Dive, Tactical, Finisher, ModelCount };

inline Order opponent(const World& w, int i, Model model, const Habit& habit, bool initial, int our_seat = -1) {
    const Rules& r = w.r();
    const Fighter& a = w.f[i];
    if (!a.present() || a.stun >= r.per_turn || a.pull > 0) return {};
    if (offstage(w, a)) return simple_rescue(w, i);
    if (ready_in(a, r) >= r.per_turn && !a.guarding() && !a.mobile() && a.act != None) return {};
    int k = target_for(w, i, true);
    if (model == Dive && our_seat >= 0) {
        const Fighter& victim = w.f[our_seat];
        const Vec d = victim.p - a.p;
        if (victim.present() && victim.invuln < 7 && d.y < 1.4 && d.y > -7.1 &&
            std::abs(d.x) < 6.3 && a.meter + r.refill >= r.moves[Slam].cost) k = our_seat;
    }
    if (model == Finisher && our_seat >= 0) {
        const Fighter& victim = w.f[our_seat];
        if (victim.present() && victim.invuln < 6 && !victim.guarding() &&
            norm2(victim.p - a.p) < 35 && (a.charging() || ready_in(victim, r) >= 8)) k = our_seat;
    }
    const bool cautious = model == Cover || model == Tactical || (model == Observe && habit.guard_rate() > 0.19);
    const bool short_charge = model == Finisher || (model == Observe && habit.starts[Charge] > 2 && habit.charge_rate() > 0.29);
    const bool available = (a.act == None && a.gap < r.per_turn) || a.charging();
    const double meter = a.meter + r.refill * (1 + std::min(a.gap, r.per_turn - 1));
    if ((available || a.guarding()) && cautious) {
        const Threat threat = incoming(w, i, 8);
        if (threat.skill != None) {
            if (!a.charging() && !a.guarding() && threat.reflectable() && threat.in >= r.moves[Counter].windup + 1 + a.gap &&
                meter >= r.moves[Counter].cost && (habit.starts[Counter] > 1 || model == Cover)) {
                return {0, {}, Counter};
            }
            if (threat.blockable() && (threat.in <= 4 || a.guarding())) {
                if (model == Tactical && !a.guarding() && meter >= r.moves[Blink].cost &&
                    (a.gap >= threat.in || a.damage > 55)) return {0, {sign(-a.p.x), a.grounded ? 0.48 : -0.2}, Blink};
                return {0, {}, Guard};
            }
            if (!threat.blockable() && !a.guarding() && threat.in <= 5 && meter >= r.moves[Blink].cost) {
                return {0, {sign(-a.p.x), 0.48}, Blink};
            }
        }
    }
    if (k < 0) return {steer(w, a, waypoint(w, i, -1))};
    const Fighter& b = w.f[k];
    const Vec d = b.p - a.p;
    Order o;
    const double bside = sign(d.x);
    const bool camp = model == Observe && habit.still >= 12 && norm2(d) > 18;
    o.move = (model == Pressure || model == Dive) ? bside : steer(w, a, waypoint(w, i, k));
    if (model == Observe) {
        if (camp) o.move = habit.travel * 0.65;
        else if (initial) o.move = std::clamp(0.72 * habit.travel + 0.28 * o.move, -1.0, 1.0);
    }
    if (w.r().bounds(w.tick + 18).x < 10) o.move = steer(w, a, waypoint(w, i, k));
    if (a.guarding()) {
        if (cautious) {
            const Threat next = incoming(w, i, 8);
            if (!next.blockable()) return {sign(-d.x), {}, None, a.grounded || a.air > 0};
        }
        if (model != Pressure && std::abs(d.y) < 2.8 && std::abs(d.x) < 3.3 && a.left > 7 &&
            (model == Observe || b.phase != Idle)) o.skill = Guard;
        return o;
    }
    if (a.charging()) {
        o.dir = lead(w, i, k, Charge);
        if (short_charge && !b.guarding() && !b.countering() && norm2(o.dir) < 13.2 &&
            (ready_in(b, r) >= 6 || norm2(d) < 10.5 || a.charge >= 15)) {
            if (b.damage >= 24 && std::abs(o.dir.y) < 0.65 && std::abs(o.dir.x) < 3.0) {
                o.dir = {sign(o.dir.x), -0.53};
            }
            return o;
        }
        if (norm2(o.dir) > 13.5 || (a.charge < 12 && norm2(d) > 6.5)) o.skill = Charge;
        return o;
    }
    if (available) {
        const Vec swing = lead(w, i, k, Attack);
        const double dist2 = norm2(swing);
        const bool grabs = model == Cover || habit.starts[Grab] > habit.starts[Attack] / 2;
        if (grabs && meter >= r.moves[Grab].cost && (b.guarding() || b.countering() || b.damage > 53) && dist2 < 9.5) {
            return {o.move, lead(w, i, k, Grab), Grab};
        }
        if (short_charge && !b.guarding() && !b.countering() && norm2(d) < 23 &&
            (b.stun >= 10 || (b.phase == Recover && ready_in(b, r) >= 12) ||
             (model == Finisher && a.invuln >= 15 && b.damage >= 30 && norm2(d) < 13.5))) {
            const Vec aim = lead(w, i, k, Charge, r.per_turn);
            if (norm2(aim) < 17.5) return {o.move, aim, Charge};
        }
        if (model != Pressure && !a.grounded && a.p.y > b.p.y + 0.75 && a.p.y < b.p.y + 7.4 &&
            std::abs(d.x) < 6.4 && meter >= r.moves[Slam].cost && (!b.guarding())) {
            const double reach = r.moves[Slam].radius + r.half_height;
            if (slam_distance(w, i, k) < (reach - 0.19) * (reach - 0.19)) return {o.move, {}, Slam};
        }
        if (model == Dive && a.grounded && a.mobile() && b.stun < 11 && d.y > -0.8 && d.y < 2.6 &&
            std::abs(d.x) < 5.2 && meter >= r.moves[Slam].cost &&
            r.bounds(w.tick + 18).top > a.p.y + 5.8 && r.bounds(w.tick + 18).x > std::abs(a.p.x) + 3.0) {
            return {bside, {}, None, true};
        }
        if (!b.guarding() && !b.countering() && dist2 <= (model == Pressure ? 9.3 : 8.1)) {
            return {o.move, swing, Attack};
        }
        if (d.y > 1.7 && d.y < 5.6 && std::abs(d.x) < 1.8 && meter >= r.moves[Upper].cost && model == Cover) {
            return {o.move, {}, Upper};
        }
    }
    if (a.grounded && a.mobile() && !camp && w.r().bounds(w.tick + 22).top > a.p.y + 7) {
        o.jump = (d.y > 2.6 && std::abs(d.x) < 6) ||
                 ((model == Cover || model == Dive || model == Tactical || (model == Observe && habit.starts[Slam] > 2)) &&
                  d.y >= -0.55 && d.y < 2 && std::abs(d.x) < (model == Dive ? 5.6 : 3.9) &&
                  meter >= r.moves[Slam].cost);
        o.drop = a.p.y > r.solids.front().top + 0.1 && d.y < -2.6 && std::abs(d.x) < 4.1 && model == Dive;
    }
    return o;
}

// The simultaneous move is hidden. This additional scenario asks each
// opponent which immediate move is rewarding against plausible first inputs.
inline Orders tactical_opening(const World& root, int self, Orders opening) {
    const Rules& r = root.r();
    const Fighter& us = root.f[self];
    opening[self] = {steer(root, us, waypoint(root, self, target_for(root, self)))};
    if (us.guarding()) opening[self].skill = Guard;
    if (us.charging()) opening[self].skill = Charge;
    Orders result = opening;
    for (int i = 0; i < seats; ++i) {
        const Fighter& a = root.f[i];
        if (i == self || !a.present() || (ready_in(a, r) >= r.per_turn && !a.guarding()) || offstage(root, a)) continue;
        if (norm2(a.p - us.p) > 104) continue;
        std::array<Order, 36> choices{};
        int n = 0;
        const auto add = [&](Order o) {
            for (int j = 0; j < n; ++j) {
                if (o.skill == choices[j].skill && o.jump == choices[j].jump &&
                    std::abs(o.move - choices[j].move) < 0.1 &&
                    norm2(unit(o.dir, {}) - unit(choices[j].dir, {})) < 0.09) return;
            }
            if (n < int(choices.size())) choices[n++] = o;
        };
        const auto can = [&](Skill s) { return a.meter + r.refill * r.per_turn >= r.moves[s].cost; };
        add(opening[i]);
        add({steer(root, a, waypoint(root, i, target_for(root, i))), {}, a.charging() ? Charge : None});
        add({0, {}, Guard});
        if (can(Blink) && !a.guarding()) add({0, {sign(-a.p.x), a.grounded ? 0.65 : 0.08}, Blink});
        if (!a.charging() && !a.guarding()) {
            if (!a.grounded && can(Slam)) add({0, {}, Slam});
            if (can(Upper)) add({0, {}, Upper});
            if (can(Counter)) add({0, {}, Counter});
        }
        for (int k = 0; k < seats; ++k) {
            const Fighter& b = root.f[k];
            if (i == k || !b.present() || b.invuln > 11) continue;
            const Vec d = b.p - a.p;
            if (norm2(d) > 43) continue;
            if (a.guarding()) {
                add({sign(-d.x), {}, None, a.grounded || a.air > 0});
                continue;
            }
            if (a.charging()) {
                add({0, lead(root, i, k, Charge), None});
                if (std::abs(d.y) < 0.9 && std::abs(d.x) < 3.6) {
                    add({0, {sign(d.x), -0.53}, None});
                }
                continue;
            }
            add({0, lead(root, i, k, Attack), Attack});
            if (std::abs(d.y) < 1.6) add({0, {sign(d.x), -0.57}, Attack});
            if (can(Grab)) {
                add({0, lead(root, i, k, Grab), Grab});
                if (std::abs(d.y) < 1.4) add({0, {sign(d.x), -0.34}, Grab});
            }
            if (a.grounded && r.bounds(root.tick + 14).top > a.p.y + 6) add({sign(d.x), {}, None, true});
        }
        double best = -1e9;
        for (int j = 0; j < n; ++j) {
            World w = root;
            Orders orders = opening;
            orders[i] = choices[j];
            for (int t = 0; t < 12; ++t) {
                if (t == r.per_turn) {
                    for (int k = 0; k < seats; ++k) {
                        Order& o = orders[k];
                        o.jump = false;
                        if (o.skill != Guard && o.skill != Charge) o.skill = None;
                        if (w.f[k].charging()) o.skill = Charge;
                    }
                }
                w.step(orders, t % r.per_turn == 0);
            }
            const Fighter& after = w.f[i];
            const Fighter& target = w.f[self];
            double value = 0.89 * w.dealt[i] + 33 * w.kills[i] + 5.2 * (after.meter - a.meter);
            value -= 0.93 * (after.damage - a.damage) + 76 * (a.stocks - after.stocks);
            value -= 0.04 * std::min(32, ready_in(after, r));
            value -= 0.15 * std::max(0.0, std::abs(after.p.x) - 8.0);
            if (target.last_hit == i && target.hit_tick > root.tick) value += 0.18 * w.dealt[i];
            if (j == 0) value += 0.36;
            if (value > best) {
                result[i] = choices[j];
                best = value;
            }
        }
    }
    return result;
}

}  // namespace ring
