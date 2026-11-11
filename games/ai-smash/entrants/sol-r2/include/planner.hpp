#pragma once

#include "endgame.hpp"
#include "recovery.hpp"
#include <limits>

namespace ring {

struct Plan {
    Order first{};
    int hold = 1;
    Order follow{};
    int later = -1, aim_at = -1;
    Skill aiming_as = None;
};

inline double health_damage(double damage) {
    return damage / (1 + damage / 140);
}

inline bool blocked_respawn(const World& w, int seat, int delay) {
    const Rules& r = w.r();
    return r.outside(r.respawn_position(seat) - Vec{0, r.gravity}, w.tick + delay);
}

inline bool forced_fall(const World& w, int i) {
    const Fighter& a = w.f[i];
    if (!a.present()) return false;
    const Rules& r = w.r();
    const int n = std::min(48, std::max(a.stun, ready_in(a, r)));
    if (n == 0) return false;
    const Bounds b = r.bounds(w.tick + n);
    const Solid& stage = r.solids.front();
    const double x = std::max(std::abs(a.p.x), std::abs(a.p.x + a.v.x * n));
    const double peak = std::max(0.0, a.v.y) * std::min(double(n), std::max(0.0, a.v.y) / r.gravity);
    if (x < std::min(b.x - 0.5, stage.right - 0.6) && a.p.y >= stage.top &&
        a.p.y + peak + (a.act == Upper && a.phase == Windup ? 8 : 0) < b.top - 0.5) return false;
    World solo;
    solo.rules = &r;
    solo.tick = w.tick;
    solo.f = w.f;
    for (int t = 0; t < n; ++t) {
        ++solo.tick;
        solo.control(i, {}, false);
        solo.motion(i);
        const Fighter& p = solo.f[i];
        if (r.outside(p.p, solo.tick)) return true;
        if (p.mobile() && p.gap == 0) return false;
        solo.quantize(solo.f[i]);
    }
    return false;
}

class Planner {
    const World& root;
    const std::array<Habit, seats>& habit;
    const int me;
    const Rules& r;
    const Deadline limit;
    int focus;
    std::array<Orders, ModelCount> initial{};
    std::vector<Plan> plans;

    bool can_pay(Skill s) const {
        const Fighter& a = root.f[me];
        return a.meter + r.refill * r.per_turn + 1e-9 >= r.moves[s].cost;
    }

    void add(Plan p) {
        for (const Plan& old : plans) {
            if (old.hold != p.hold || old.later != p.later || old.follow.skill != p.follow.skill || old.aim_at != p.aim_at ||
                old.aiming_as != p.aiming_as || old.first.skill != p.first.skill || old.first.jump != p.first.jump ||
                old.first.drop != p.first.drop || std::abs(old.first.move - p.first.move) > 0.06) continue;
            if (norm2(unit(old.first.dir, {}) - unit(p.first.dir, {})) < 0.045) return;
        }
        plans.push_back(p);
    }

    void aims(Skill s, Skill emitted, int k, bool hopping = false) {
        const Fighter& a = root.f[me];
        const Fighter& b = root.f[k];
        const Vec now = b.p - a.p;
        Vec aim = lead(root, me, k, s);
        const MoveRule& m = r.moves[s];
        if (hopping) {
            const double delay = m.windup + 1;
            const double speed = a.grounded ? r.ground_speed : r.air_speed;
            aim.y -= delay * (speed - a.v.y - 0.06);
        }
        const double reach = m.offset + m.radius + r.half_height + 1.45;
        if (std::min(norm2(aim), norm2(now)) > reach * reach) return;
        add({{0, aim, emitted, hopping}});
        if (!hopping) add({{0, now, emitted}});
        const double side = sign(std::abs(aim.x) > 0.12 ? aim.x : now.x);
        if (std::abs(aim.y) < 2.6) {
            add({{0, {side, -0.52}, emitted, hopping}});
            add({{0, {side, 0}, emitted, hopping}});
        }
        if (std::abs(aim.y) > 0.8 || std::abs(aim.x) < 2.2) {
            add({{0, {aim.x * 0.52, aim.y + 0.45}, emitted, hopping}});
            if (std::abs(aim.x) < 1.7) add({{0, {0, sign(aim.y + 0.1)}, emitted, hopping}});
        }
        if (!hopping && (b.guarding() || b.grounded) && norm2(now) < 9.8 && std::abs(now.x) > 0.85) {
            add({{0, {side, 0.55}, emitted}});
        }
    }

    void generate() {
        const Fighter& a = root.f[me];
        const Bounds b = r.bounds(root.tick + 22);
        const bool parked = b.x < 1.65 && a.grounded && a.p.x == 0 && a.v.x == 0;
        const double guided = parked ? 0 : steer(root, a, waypoint(root, me, focus));
        const Threat visible = incoming(root, me, 11);
        const bool threat = visible.skill != None;
        double dist = 99;
        bool charge_near = false;
        for (int k = 0; k < seats; ++k) {
            const Fighter& t = root.f[k];
            if (k == me || !t.present()) continue;
            const double d = std::sqrt(norm2(t.p - a.p));
            dist = std::min(dist, d);
            if (d < 5.9 && (t.charging() || (t.act == Charge && t.phase == Windup) ||
                (habit[k].charge_rate() >= 0.24 && ready_in(t, r) < 8))) charge_near = true;
        }
        const int ready = ready_in(a, r);
        const bool has_action = ready < r.per_turn && !a.guarding();
        const Skill keep = a.charging() ? Charge : None;
        const bool jump_ok = (a.mobile() || a.guarding()) && (a.grounded || a.air > 0) &&
                             b.top > a.p.y + 5.2 && b.x > 2.2;
        add({{guided, {}, keep}});
        if (!parked) for (double move : {0.0, -1.0, 1.0}) add({{move, {}, keep}});
        if (!parked && charge_near && a.invuln < 9) {
            add({{-1, {}, keep}, 3});
            add({{1, {}, keep}, 3});
            if (jump_ok) {
                add({{-1, {}, keep, true}, 3});
                add({{1, {}, keep, true}, 3});
            }
        }
        if (!parked && focus >= 0 && dist > 4.3) {
            const Fighter& t = root.f[focus];
            const double d = t.p.x - a.p.x;
            const double direct = steer(root, a, std::clamp(t.p.x, -std::min(10.0, b.x * 0.82), std::min(10.0, b.x * 0.82)));
            add({{direct, {}, keep}, 3});
            if (jump_ok && (a.grounded || t.p.y > a.p.y + 3.1) && t.p.y > a.p.y + 1.2 && std::abs(d) < 16.5) {
                add({{direct, {}, keep, true}, 4});
                add({{guided, {}, keep, true}, 3});
            }
        }
        if (b.x < 7 && a.grounded) {
            add({{steer(root, a, 0), {}, keep}});
            if (std::abs(a.v.x) < r.ground_accel && std::abs(a.p.x) <= 0.6) {
                add({{std::clamp(-a.p.x / (r.run_speed * r.per_turn), -1.0, 1.0), {}, keep}});
            }
        }
        if (thin_floor(root, a)) {
            add({{guided, {}, keep, false, true}});
            add({{0, {}, keep, false, true}});
            if (a.guarding() || has_action) add({{0, {}, Guard, false, true}, 2});
        }
        if (a.guarding() || (has_action && (dist < 8 || root.shot_count > 0))) {
            add({{0, {}, Guard}});
            add({{0, {}, Guard}, 3});
            if (visible.skill == Charge && visible.from >= 0) {
                const int hold = (visible.in + 2 + r.per_turn - 1) / r.per_turn;
                add({{0, {}, Guard}, hold, {0, {}, Attack}, hold + 2, visible.from, Attack});
                if (can_pay(Grab)) add({{0, {}, Guard}, hold, {0, {}, Grab}, hold + 2, visible.from, Grab});
            }
        }
        if (has_action && can_pay(Blink) && (!parked || threat) && (dist < 9 || threat || a.charging())) {
            for (const Vec d : {Vec{-1, 0}, Vec{1, 0}, Vec{-1, 0.8}, Vec{1, 0.8},
                                Vec{-1, -0.55}, Vec{1, -0.55}, Vec{0, 1}}) {
                if (d.y > 0.1 && b.top - a.p.y < 5.2) continue;
                add({{0, d, Blink}});
            }
        }
        if (has_action && !a.charging()) {
            const bool counter_window = threat && visible.reflectable() &&
                visible.in >= r.moves[Counter].windup + 1 + std::max(0, ready);
            if ((counter_window || (!charge_near && (dist < 6.7 || threat))) &&
                can_pay(Counter) && a.invuln < 8) add({{0, {}, Counter}});
            if (!a.grounded && dist < 8 && can_pay(Slam)) add({{0, {}, Slam}});
            if (dist < 7.5 && can_pay(Upper) && b.top > a.p.y + 4.8) add({{0, {}, Upper}});
            if (a.grounded && a.act == None && a.gap > 0 && dist < 6.7 && jump_ok && can_pay(Slam)) {
                add({{guided, {}, Slam, true}});
                add({{0, {}, Slam, true}});
            }
        }
        for (int k = 0; k < seats; ++k) {
            if (k == me || !root.f[k].present() || root.f[k].invuln > 23) continue;
            const Fighter& t = root.f[k];
            const double d2 = norm2(t.p - a.p);
            if (has_action && a.charging()) {
                aims(Charge, None, k);
                if (jump_ok && d2 < 46 && t.p.y > a.p.y + 0.4) aims(Charge, None, k, true);
                if (d2 < 46) add({{guided, lead(root, me, k, Charge), Charge}, 2, {}, 2, k, Charge});
            } else if (has_action) {
                aims(Attack, Attack, k);
                if (can_pay(Grab)) aims(Grab, Grab, k);
                if (d2 > 10 && d2 < 300 && can_pay(Wave)) {
                    add({{0, lead(root, me, k, Wave), Wave}});
                    add({{0, lead(root, me, k, Wave, 4), Wave}});
                }
                if (d2 > 12 && d2 < 156 && can_pay(Hook) && a.hook_cd < r.per_turn && !offstage(root, t)) {
                    add({{0, lead(root, me, k, Hook), Hook}});
                }
            }
            if (d2 < 65 && ready < 7 && (k == focus || d2 < 23)) {
                const double move = parked ? 0 : steer(root, a, waypoint(root, me, k));
                const int delay = a.guarding() ? 2 : std::max(1, ready / r.per_turn);
                if (!a.charging()) {
                    add({{move}, delay, {0, {}, Attack}, delay, k, Attack});
                    if (can_pay(Grab)) add({{move}, delay, {0, {}, Grab}, delay, k, Grab});
                }
                if (jump_ok && d2 < 61) {
                    if (t.p.y > a.p.y + 1.1 || t.act == Slam) {
                        add({{sign(a.p.x - t.p.x), {}, keep, true}});
                    }
                    for (double drive : {move, sign(t.p.x - a.p.x)}) {
                        add({{drive, {}, keep, true}, 1});
                        if (can_pay(Slam) && !a.charging() && t.p.y < a.p.y + 4) {
                            add({{drive, {}, None, true}, 1, {0, {}, Slam}, 1});
                            add({{drive, {}, None, true}, 2, {0, {}, Slam}, 2});
                        }
                        if (t.p.y >= a.p.y + 0.5) {
                            add({{drive, {}, keep, true}, 2, {0, {}, keep == Charge ? None : Attack}, 2, k,
                                 keep == Charge ? Charge : Attack});
                        }
                    }
                }
                if (thin_floor(root, a) && !a.charging() && t.p.y < a.p.y - 1.7 && can_pay(Slam)) {
                    add({{move, {}, None, false, true}, 1, {0, {}, Slam}, 1});
                    add({{move, {}, None, false, true}, 2, {0, {}, Slam}, 2});
                }
                if (has_action && !a.charging() && d2 > 10 && d2 < 65 && k == focus) {
                    add({{move, t.p - a.p, Charge}, 3, {}, 3, k, Charge});
                    if (jump_ok && (t.p.y > a.p.y + 1.2 || a.invuln > 12 || t.stun > 12)) {
                        add({{move, t.p - a.p, Charge, true}, 3, {}, 3, k, Charge});
                    }
                }
                if (has_action && !a.charging() && !t.guarding() && !t.countering() && d2 < 16 &&
                    (t.stun >= 12 || (t.phase == Recover && ready_in(t, r) >= 12))) {
                    add({{move, t.p - a.p, Charge}, 1, {}, 1, k, Charge});
                    if (std::abs(t.p.y - a.p.y) < 0.75 && std::abs(t.p.x - a.p.x) < 3.2) {
                        add({{move, t.p - a.p, Charge}, 1, {0, {sign(t.p.x - a.p.x), -0.53}}, 1});
                    }
                }
            }
            if (jump_ok && a.act == Charge && !a.charging() && k == focus && d2 < 95) {
                add({{guided, {}, None, true}});
                add({{sign(t.p.x - a.p.x), {}, None, true}});
            }
        }
    }

    Order continued(const World& w) const {
        const Fighter& a = w.f[me];
        if (!a.present() || a.stun >= r.per_turn) return {};
        if (offstage(w, a)) return simple_rescue(w, me);
        const int target = target_for(w, me);
        const double goal = r.bounds(w.tick + 18).x < 1.65 ? 0 : waypoint(w, me, target);
        Order o{steer(w, a, goal)};
        if (a.charging()) {
            if (target >= 0) o.dir = lead(w, me, target, Charge);
            o.skill = Charge;
        }
        if ((a.act == None || a.guarding() || a.charging()) && a.invuln < r.per_turn && a.gap < r.per_turn) {
            const Threat threat = incoming(w, me, r.per_turn + 1);
            if (threat.skill != None && threat.blockable()) o.skill = Guard;
        }
        if (a.act == None && a.stun == 0 && a.gap < r.per_turn && o.skill == None && target >= 0) {
            const Fighter& t = w.f[target];
            const double timing = r.moves[Attack].windup + a.gap + 1;
            if (t.stun > timing + 1 && !offstage(w, t)) {
                const Vec d = lead(w, me, target, Attack);
                const double reach = r.moves[Attack].offset + r.moves[Attack].radius + r.half_height - 0.22;
                if (norm2(d) <= reach * reach) {
                    o.skill = Attack;
                    o.dir = std::abs(d.y) < 0.7 ? Vec{sign(d.x), -0.50} : d;
                }
            }
        }
        return o;
    }

    double closing_progress(const World& w) const {
        if (focus < 0 || !w.f[me].present() || !root.f[focus].present()) return 0;
        const Fighter& a = w.f[me];
        const Fighter& s = root.f[me];
        const Fighter& t = root.f[focus];
        if (t.invuln > 4 || offstage(root, t)) return 0;
        const Vec d = t.p - s.p;
        if (norm2(d) < 20.25) return 0;
        const Bounds b = r.bounds(w.tick + 20);
        const double target_x = std::clamp(t.p.x, -b.x * 0.83, b.x * 0.83);
        const double x0 = std::max(0.0, std::abs(s.p.x - target_x) - 2.4);
        const double x1 = std::max(0.0, std::abs(a.p.x - target_x) - 2.4);
        const double y = std::min(t.p.y, b.top - 4.7);
        const double dy = std::max(0.0, y - s.p.y - 1.2) - std::max(0.0, y - a.p.y - 1.2);
        return (x0 - x1 + 0.52 * dy) * (habit[focus].still >= 12 ? 0.34 : 0.14);
    }

    Order command(const World& w, const Plan& p, int turn) const {
        if (turn < p.hold) {
            Order o = p.first;
            if (turn > 0) o.jump = false;
            return o;
        }
        if (turn == p.later) {
            Order o = p.follow;
            if (p.aim_at >= 0 && w.f[p.aim_at].present()) o.dir = lead(w, me, p.aim_at, p.aiming_as);
            return o;
        }
        return continued(w);
    }

    double evaluate(const World& w) const {
        const Fighter& a = w.f[me];
        const Fighter& start = root.f[me];
        const int stock_loss = start.stocks - a.stocks;
        const bool sudden = blocked_respawn(w, me, a.respawn > 0 ? a.respawn : r.respawn);
        const double stock_price = sudden ? 124 : 73 + (start.stocks <= 1 ? 31 : 0);
        const double damage_price = (start.damage > 38 ? 1.16 : 1.0) + (sudden ? 0.15 : 0.0);
        double score = -stock_price * stock_loss - damage_price * (health_damage(a.damage) - health_damage(start.damage));
        score += w.dealt[me] * 0.80 + w.kills[me] * 32;
        int alive = 0;
        for (int k = 0; k < seats; ++k) {
            if (k == me) continue;
            const Fighter& t = w.f[k];
            alive += !t.out;
            score += (root.f[k].stocks - t.stocks) * 9.5;
            const bool no_respawn = blocked_respawn(w, k, t.respawn > 0 ? t.respawn : r.respawn);
            if (no_respawn && t.stocks < root.f[k].stocks && !t.out) score += 8;
            if (t.out && !root.f[k].out) score += 7;
            if (t.present() && forced_fall(w, k)) {
                score += (t.last_hit == me && w.tick - t.hit_tick < r.credit - 45 ? 31 : 5) + (no_respawn ? 6 : 0);
            }
        }
        if (!a.present() || stock_loss > 0) return score;
        if (alive == 0) return score + 15;
        if (forced_fall(w, me)) return score - 0.86 * stock_price;
        const Solid& s = r.solids.front();
        const Bounds b = r.bounds(w.tick + 12);
        const double x = std::abs(a.p.x);
        const double outer = std::max(0.0, x - std::min(s.right - 2.1, b.x * 0.73));
        const double below = std::max(0.0, s.top - a.p.y);
        const double projected_x = std::abs(project(w, a, 8).x);
        score += 7.3 * (a.meter - start.meter);
        score -= 0.13 * x + 0.34 * outer * outer + 2.8 * below;
        score -= 2.0 * std::max(0.0, projected_x - b.x + 0.9);
        score -= 3.4 * std::max(0.0, a.p.y - b.top + 2.0);
        score -= 0.075 * a.stun;
        if (offstage(w, a)) score -= 1.2 * (1 + std::max(0.0, x - s.right)) + (a.air == 0 ? 2.2 : 0);
        double best = 38, crowd = 0;
        for (int k = 0; k < seats; ++k) {
            if (k == me || !w.f[k].present()) continue;
            const Fighter& t = w.f[k];
            const Vec d = t.p - a.p;
            const double distance = std::sqrt(d.x * d.x + d.y * d.y * 1.15);
            if (t.invuln > 4 && t.invuln > a.invuln) {
                score -= 1.8 * std::max(0.0, 6.0 - distance);
                continue;
            }
            double preference = std::max(0.0, distance - 2.15) + (k == focus ? 0 : 1.2);
            preference -= std::min(1.0, t.damage * 0.012);
            preference += offstage(w, t) ? 5.0 : 0;
            best = std::min(best, preference);
            if (distance < 4.7 && t.stun < 5 && !t.guarding() && a.invuln <= 6) {
                crowd += (4.7 - distance) * (d.y > 1.2 ? 1.18 : 1);
            }
            if (d.y > 1.0 && d.y < 7.3 && std::abs(d.x) < 5.0 && a.invuln <= 6 &&
                ready_in(t, r) < 6 && t.meter >= r.moves[Slam].cost - r.refill) {
                score -= 0.33 * (5.0 - std::abs(d.x)) * (a.guarding() ? 0.22 : 1.0);
            }
            if (t.stun > 8 && distance < 4.9 && a.stun < t.stun && !offstage(w, t)) {
                score += 0.036 * (t.stun - 8) * (4.9 - distance);
            }
            if (a.invuln < 7 && ready_in(a, r) > r.moves[Charge].windup && t.charging() &&
                distance < 5.2 && !t.untouchable()) {
                const Vec strike = lead(w, k, me, Charge);
                const double range = r.moves[Charge].offset + r.moves[Charge].radius + r.half_height;
                score -= (0.8 + 0.014 * std::min(90.0, a.damage)) * std::max(0.0, range - std::sqrt(norm2(strike)));
            }
        }
        score -= std::min(best, 22.0) * (a.invuln > 10 ? 0.77 : 0.49);
        score -= std::max(0.0, crowd - 2.45) * (0.36 + 0.008 * std::min(90.0, a.damage));
        const int ready = ready_in(a, r);
        if (best < 3.0 && a.invuln < 10) score -= 0.052 * std::min(ready, 40);
        if (a.charging()) score += 0.024 * std::min(a.charge, 40);
        if (sudden) score -= 0.30 * x;
        if (b.x < 12.0) score -= 0.39 * x + std::max(0.0, 3.1 - b.x) * (x * 1.7 + a.p.y * 0.6);
        score += closing_progress(w);
        return score;
    }

    double run(const Plan& p, Model model, int turns, bool refine = false) const {
        World w = root;
        double tempo = 0;
        for (int turn = 0; turn < turns && w.tick < r.max_tick; ++turn) {
            Orders o = initial[model];
            if (turn > 0) {
                for (int i = 0; i < seats; ++i) {
                    if (i != me) o[i] = opponent(w, i, model, habit[i], false, me);
                }
            }
            if (refine && model == Tactical && turn == 1 && w.f[me].phase == Windup) {
                o = tactical_opening(w, me, o);
            }
            o[me] = command(w, p, turn);
            for (int tick = 0; tick < r.per_turn; ++tick) w.step(o, tick == 0);
            if (turn == 1) tempo = 0.72 * closing_progress(w);
            if (turn == 3) tempo += 0.12 * w.dealt[me] - 0.065 * std::max(0, w.f[me].stun - 9);
            if (!w.f[me].present()) break;
            int alive = 0;
            for (const Fighter& a : w.f) alive += !a.out;
            if (alive <= 1) break;
        }
        double value = evaluate(w) + tempo;
        if (p.first.jump && !root.f[me].grounded) value -= 0.28;
        if (p.first.skill == Charge && !root.f[me].charging()) value -= 0.13;
        if (p.later > 0) value -= 0.11 * p.later;
        if (w.dealt[me] <= 0.01 && (is_melee(p.first.skill) || p.first.skill == Wave || p.first.skill == Hook) &&
            p.first.skill != Charge) value -= p.first.skill == Attack ? 0.45 : 0.85;
        return value;
    }

public:
    Planner(const World& w, const std::array<Habit, seats>& habits, int seat, Deadline deadline)
        : root(w), habit(habits), me(seat), r(w.r()), limit(deadline), focus(target_for(w, seat)) {}

    Order choose() {
        const Fighter& a = root.f[me];
        if (!a.present() || a.stun >= r.per_turn || a.pull >= r.per_turn) return {};
        if (a.act != None && ready_in(a, r) >= r.per_turn && !a.mobile() && !a.guarding() && !thin_floor(root, a)) return {};
        const auto center = Center::choose(root, me);
        if (center && incoming(root, me, r.per_turn + 2).skill == None) return *center;
        if (!center && emergency(root, me)) return Rescue::choose(root, me, limit);
        plans.reserve(130);
        if (center) add({*center});
        generate();
        for (int model = Observe; model < ModelCount; ++model) {
            for (int i = 0; i < seats; ++i) {
                if (i != me) initial[model][i] = opponent(root, i, static_cast<Model>(model), habit[i], true, me);
            }
        }
        initial[Tactical] = tactical_opening(root, me, initial[Tactical]);
        struct Rated { std::size_t i; double score; };
        std::vector<Rated> ranked;
        ranked.reserve(plans.size());
        for (std::size_t k = 0; k < plans.size(); ++k) {
            const double observe = run(plans[k], Observe, 7);
            const double tactical = run(plans[k], Tactical, 7);
            ranked.push_back({k, 0.61 * observe + 0.39 * tactical});
            if ((k & 3) == 3 && Clock::now() >= limit) break;
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](Rated a, Rated b) { return a.score > b.score; });
        Order best = plans[ranked.front().i].first;
        double best_score = -std::numeric_limits<double>::infinity();
        std::array<int, SkillCount> seen{};
        int inspected = 0;
        double guard_weight = 0.23;
        if (focus >= 0) guard_weight += std::clamp((habit[focus].guard_rate() - 0.11) * 0.25, -0.035, 0.10);
        double dive_weight = 0.105;
        double finish_weight = 0.09;
        for (int k = 0; k < seats; ++k) {
            if (k == me || !root.f[k].present() || norm2(root.f[k].p - a.p) > 65) continue;
            dive_weight = std::max(dive_weight, 0.105 + 0.14 * habit[k].slam_rate());
            finish_weight = std::max(finish_weight, 0.09 + 0.18 * habit[k].charge_rate() +
                                                    (root.f[k].charging() ? 0.07 : 0));
        }
        for (Rated candidate : ranked) {
            const Plan& p = plans[candidate.i];
            if (seen[p.first.skill] >= (p.first.skill == None ? 8 : 4)) continue;
            if (inspected >= 32 || Clock::now() >= limit) break;
            ++seen[p.first.skill];
            ++inspected;
            const double v = run(p, Observe, 9);
            const double rush = run(p, Pressure, 9);
            const double cover = run(p, Cover, 9);
            const double dive = run(p, Dive, 9);
            const double threat = run(p, Tactical, 9, true);
            const double finish = run(p, Finisher, 9);
            const double base_mean = v * (0.65 - guard_weight - dive_weight) + rush * 0.13 +
                                     cover * guard_weight + dive * dive_weight + threat * 0.22;
            const double mean = (1 - finish_weight) * base_mean + finish_weight * finish;
            const double risk = a.invuln > 9 ? 0.095 : (a.damage > 37 ? 0.26 : 0.19);
            const double tail = std::min({v, rush, cover, dive, threat});
            const double score = mean * (1 - risk) + risk * (finish_weight >= 0.16 ? std::min(tail, finish) : tail);
            if (score > best_score) {
                best_score = score;
                best = p.first;
            }
        }
        return best;
    }
};

}  // namespace ring
