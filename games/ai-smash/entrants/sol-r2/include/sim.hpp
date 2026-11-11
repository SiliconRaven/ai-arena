#pragma once

#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

namespace ring {
using J = mini_json::Json;
constexpr int seats = 4;
constexpr int shot_capacity = 24;

struct Vec {
    double x = 0, y = 0;
    Vec operator+(Vec b) const { return {x + b.x, y + b.y}; }
    Vec operator-(Vec b) const { return {x - b.x, y - b.y}; }
    Vec operator*(double k) const { return {x * k, y * k}; }
    Vec& operator+=(Vec b) { x += b.x; y += b.y; return *this; }
};

inline double norm2(Vec a) { return a.x * a.x + a.y * a.y; }
inline double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
inline Vec unit(Vec a, Vec fallback = {1, 0}) {
    const double length = std::sqrt(norm2(a));
    return length < 1e-9 ? fallback : Vec{a.x / length, a.y / length};
}
inline double sign(double x) { return x < 0 ? -1.0 : 1.0; }
inline double number(const J& j, const char* key) { return j.at(key).as_double(); }
inline int count(const J& j, const char* key) { return j.at(key).get<int>(); }
inline Vec vector_from(const J& j) { return {j.at(0).as_double(), j.at(1).as_double()}; }

enum Skill { None, Attack, Charge, Guard, Blink, Upper, Slam, Wave, Grab, Counter, Hook, SkillCount };
enum Phase { Idle, Charging, Windup, Active, Recover };

constexpr const char* names[] = {"", "attack", "charge", "guard", "blink", "uppercut",
                                  "slam", "wave", "grab", "counter", "hook"};
inline Skill skill_from(const J& j) {
    if (j.is_string()) {
        for (int i = 1; i < SkillCount; ++i) {
            if (j.as_string() == names[i]) return static_cast<Skill>(i);
        }
    }
    return None;
}
inline Phase phase_from(std::string_view s) {
    if (s == "active") return Active;
    if (s == "windup") return Windup;
    if (s == "recover") return Recover;
    if (s == "charging") return Charging;
    return Idle;
}
inline bool is_melee(Skill s) {
    return s == Attack || s == Grab || s == Charge || s == Upper || s == Slam;
}

struct MoveRule {
    double cost = 0, damage = 0, power = 0, offset = 0, radius = 0, lift = 0, speed = 0;
    int windup = 0, active = 0, recover = 0, ttl = 0;
};
struct Solid { double left, right, top, bottom; };
struct Platform { double left, right, y; };
struct Bounds { double x, top, bottom; };

struct Rules {
    std::array<MoveRule, SkillCount> moves{};
    std::vector<Solid> solids;
    std::vector<Platform> platforms;
    Bounds blast{};
    Vec spawn{};
    int shrink_tick = 0, max_tick = 0, per_turn = 0;
    double shrink_rate = 0, top_ratio = 0, min_x = 0, min_top = 0;
    double width = 0, half_height = 0, ground_speed = 0, air_speed = 0;
    double gravity = 0, max_fall = 0, run_speed = 0, ground_accel = 0, air_accel = 0;
    double ground_friction = 0, air_friction = 0, hit_friction = 0, bounce = 0;
    double kb_base = 0, kb_scale = 0, stun_per_speed = 0;
    double meter_max = 0, refill = 0, precision = 1;
    double charge_max_mult = 0, charge_move = 0, guard_push = 0, upper_rise = 0;
    double charge_short_mult = 1, charge_short_full = 1;
    double counter_mult = 0, hook_reel = 0, hook_pull = 0, hook_stop = 0;
    int air_jumps = 0, drop_ticks = 0, stun_base = 0, max_stun = 0, gap = 0;
    int respawn = 0, invuln = 0, credit = 0, charge_max = 0, charge_auto = 0;
    int guard_max = 0, guard_lockout = 0, counter_recover = 0, hook_ticks = 0, hook_miss = 0;
    int slam_landing_cap = -1;

    explicit Rules(const J& j) {
        const J& stage = j.at("stage");
        for (const J& s : stage.at("solids").as_array()) {
            solids.push_back({number(s, "x1"), number(s, "x2"), number(s, "top"), number(s, "bottom")});
        }
        for (const J& p : stage.at("platforms").as_array()) {
            platforms.push_back({number(p, "x1"), number(p, "x2"), number(p, "y")});
        }
        const J& b = stage.at("blast");
        blast = {number(b, "x"), number(b, "top"), number(b, "bottom")};
        if (stage.contains("blast_shrink")) {
            const J& s = stage.at("blast_shrink");
            shrink_tick = count(s, "start_tick");
            shrink_rate = number(s, "rate");
            top_ratio = number(s, "top_ratio");
            min_x = number(s, "min_x");
            min_top = number(s, "min_top");
        }
        spawn = vector_from(stage.at("respawn"));
        per_turn = count(j, "decision_ticks");
        max_tick = count(j, "max_turns") * per_turn;
        width = number(j.at("body"), "half_width");
        half_height = number(j.at("body"), "height") / 2;
        const J& p = j.at("physics");
        gravity = number(p, "gravity");
        max_fall = number(p, "max_fall");
        run_speed = number(p, "run_speed");
        ground_accel = number(p, "ground_accel");
        air_accel = number(p, "air_accel");
        ground_friction = number(p, "ground_friction");
        air_friction = number(p, "air_friction");
        hit_friction = number(p, "hit_friction");
        bounce = number(p, "bounce");
        const J& jump = j.at("jump");
        ground_speed = number(jump, "ground_speed");
        air_speed = number(jump, "air_speed");
        air_jumps = count(jump, "air_jumps");
        drop_ticks = count(jump, "drop_ticks");
        const J& k = j.at("knockback");
        kb_base = number(k, "base");
        kb_scale = number(k, "scale");
        stun_base = count(k, "hitstun_base");
        stun_per_speed = number(k, "hitstun_per_speed");
        max_stun = count(k, "hitstun_max");
        meter_max = number(j.at("meter"), "max");
        refill = number(j.at("meter"), "refill");
        gap = count(j, "gap_ticks");
        respawn = count(j, "respawn_ticks");
        invuln = count(j, "respawn_invuln_ticks");
        credit = count(j, "kill_credit_ticks");
        for (int i = 0; i < count(j, "precision"); ++i) precision *= 10;
        const J& all = j.at("moves");
        for (int i = 1; i < SkillCount; ++i) {
            const J& m = all.at(names[i]);
            const auto field = [&](const char* key) { return m.contains(key) ? number(m, key) : 0.0; };
            moves[i] = {field("cost"), field("damage"), field("power"), field("offset"), field("radius"),
                        field("lift"), field("speed"), int(field("windup")), int(field("active")),
                        int(field("recover")), int(field("ttl"))};
        }
        const J& charge = all.at("charge");
        charge_max = count(charge, "max_ticks");
        charge_auto = count(charge, "auto_release_ticks");
        charge_max_mult = number(charge, "max_mult");
        charge_move = number(charge, "move_factor");
        if (charge.contains("short_power_mult")) {
            charge_short_mult = number(charge, "short_power_mult");
            charge_short_full = number(charge, "short_power_full_fraction");
        }
        const J& slam = all.at("slam");
        if (slam.contains("landing_hitstun_cap")) slam_landing_cap = count(slam, "landing_hitstun_cap");
        const J& guard = all.at("guard");
        guard_max = count(guard, "max_ticks");
        guard_lockout = count(guard, "lockout_ticks");
        guard_push = number(guard, "pushback");
        upper_rise = number(all.at("uppercut"), "rise");
        counter_mult = number(all.at("counter"), "mult");
        counter_recover = count(all.at("counter"), "spent_recover");
        const J& hook = all.at("hook");
        hook_reel = number(hook, "reel_speed");
        hook_pull = number(hook, "pull_speed");
        hook_stop = number(hook, "stop");
        hook_ticks = count(hook, "pull_ticks");
        hook_miss = count(hook, "miss_cooldown_ticks");
    }

    Bounds bounds(int tick) const {
        const double closed = std::max(0, tick - shrink_tick) * shrink_rate;
        return {std::max(min_x, blast.x - closed), std::max(min_top, blast.top - closed * top_ratio), blast.bottom};
    }
    bool outside(Vec p, int tick) const {
        const Bounds b = bounds(tick);
        return std::abs(p.x) > b.x || p.y > b.top || p.y < b.bottom;
    }
    double round(double x) const { return std::round(x * precision) / precision; }
    double charge_power(int ticks) const {
        return 1 + (charge_max_mult - 1) * std::min(1.0, double(ticks) / charge_max);
    }
    double charge_launch_scale(int ticks, double scale) const {
        if (charge_short_mult == 1) return scale;
        const double hold_fraction = double(ticks) / charge_max;
        const double recovery = std::min(1.0, hold_fraction / charge_short_full);
        return scale * (1 + (charge_short_mult - 1) * (1 - recovery));
    }
    Vec respawn_position(int seat) const {
        return spawn + Vec{(seat - (seats - 1) / 2.0) * 1.5, 0};
    }
    bool in_solid(Vec p) const {
        for (const Solid& s : solids) {
            if (p.x > s.left && p.x < s.right && p.y < s.top && p.y > s.bottom) return true;
        }
        return false;
    }
    double knockback(double damage, double power) const { return (kb_base + kb_scale * damage / 100) * power; }
};

struct Order {
    double move = 0;
    Vec dir{};
    Skill skill = None;
    bool jump = false, drop = false;
};
inline J encode(const Order& order) {
    J j = J::object{};
    if (order.move != 0) j["move"] = order.move;
    if (order.skill != None) j["skill"] = names[order.skill];
    if (norm2(order.dir) > 0) j["dir"] = J::array{order.dir.x, order.dir.y};
    if (order.jump) j["jump"] = true;
    if (order.drop) j["drop"] = true;
    return j;
}

struct Fighter {
    Vec p{}, v{}, dir{1, 0}, hook{};
    double damage = 0, meter = 0, power = 1;
    int facing = 1, air = 0, stocks = 0, respawn = 0, invuln = 0;
    int stun = 0, gap = 0, hook_cd = 0, drop = 0;
    int left = 0, charge = 0, hit_mask = 0, last_hit = -1, hit_tick = -1000, pulled_by = -1, pull = 0;
    Skill act = None;
    Phase phase = Idle;
    bool grounded = false, out = false, reeling = false;
    bool slam_landing = false;

    bool present() const { return !out && respawn == 0; }
    bool guarding() const { return act == Guard && phase == Active; }
    bool countering() const { return act == Counter && phase == Active; }
    bool charging() const { return act == Charge && phase == Charging; }
    bool blinking() const { return act == Blink && phase == Active; }
    bool untouchable() const { return !present() || invuln > 0 || blinking(); }
    bool mobile() const { return stun == 0 && pull == 0 && (act == None || act == Charge); }
};

inline Fighter read_fighter(const J& j, const Rules& r) {
    Fighter f;
    f.p = {number(j, "x"), number(j, "y")};
    f.v = {number(j, "vx"), number(j, "vy")};
    f.dir = vector_from(j.at("dir"));
    f.damage = number(j, "damage");
    f.meter = number(j, "meter");
    f.facing = count(j, "facing");
    f.air = count(j, "air_jumps");
    f.stocks = count(j, "stocks");
    f.respawn = count(j, "respawn");
    f.invuln = count(j, "invuln");
    f.stun = count(j, "hitstun");
    f.gap = count(j, "gap");
    f.hook_cd = count(j, "hook_cd");
    f.left = count(j, "phase_ticks");
    f.charge = count(j, "charge");
    f.act = skill_from(j.at("act"));
    f.phase = phase_from(j.at("phase").as_string());
    f.grounded = j.at("grounded").as_bool();
    f.out = j.at("out").as_bool();
    if (f.act == Charge && !f.charging()) f.power = r.charge_power(f.charge);
    return f;
}

struct Shot {
    Vec p{}, v{};
    int id = 0, owner = 0, ttl = 0;
    double damage = 0, power = 0;
    Skill kind = None;
    bool dead = false;
};

using Orders = std::array<Order, seats>;

// A numeric copy of the referee's tick rules, with no replay or JSON work in the search.
struct World {
    const Rules* rules = nullptr;
    std::array<Fighter, seats> f{};
    std::array<Shot, shot_capacity> shots{};
    std::array<double, seats> dealt{};
    std::array<int, seats> kills{};
    int tick = 0, shot_count = 0, next_shot = 1;

    const Rules& r() const { return *rules; }

    void end_action(Fighter& a, int gap) {
        a.act = None;
        a.phase = Idle;
        a.left = 0;
        a.charge = 0;
        a.hit_mask = 0;
        a.reeling = false;
        a.gap = gap;
    }
    void cancel(int i) {
        const int gap = f[i].gap;
        end_action(f[i], gap);
        for (int k = 0; k < shot_count; ++k) {
            Shot& s = shots[k];
            if (s.owner == i && s.kind == Hook) s.dead = true;
        }
    }
    void recover(Fighter& a) {
        if (a.act == Blink) a.v = a.v * 0.2;
        a.reeling = false;
        a.phase = Recover;
        a.left = std::max(1, r().moves[a.act].recover);
    }
    void aim(Fighter& a, Vec dir) {
        if (norm2(dir) == 0) return;
        a.dir = unit(dir, {double(a.facing), 0});
        if (std::abs(a.dir.x) > 0.05) a.facing = int(sign(a.dir.x));
    }
    void activate(int i) {
        Fighter& a = f[i];
        const MoveRule& m = r().moves[a.act];
        a.phase = Active;
        a.hit_mask = 0;
        a.left = m.active;
        if (a.act == Upper) {
            a.v.y = r().upper_rise;
            a.grounded = false;
            a.dir = {0, 1};
        } else if (a.act == Slam) {
            a.v.y = -m.speed;
            a.v.x *= 0.3;
            a.dir = {0, -1};
        } else if (a.act == Blink) {
            a.v = a.dir * m.speed;
            a.grounded = false;
        } else if (a.act == Wave || a.act == Hook) {
            if (shot_count < shot_capacity) {
                const Vec origin = a.p + Vec{0, r().half_height} + a.dir * (r().width + 0.5);
                shots[shot_count++] = {origin, a.dir * m.speed, next_shot++, i, m.ttl, m.damage, m.power, a.act};
            }
            if (a.act == Wave) recover(a);
            else a.left = 1000000;
        }
    }
    void start(int i, const Order& o) {
        Fighter& a = f[i];
        const MoveRule& m = r().moves[o.skill];
        if (o.skill == None || a.meter + 1e-9 < m.cost) return;
        if (o.skill == Slam && a.grounded) return;
        if (o.skill == Hook && a.hook_cd > 0) return;
        a.dir = {double(a.facing), 0};
        aim(a, o.dir);
        a.act = o.skill;
        a.meter -= m.cost;
        a.hit_mask = 0;
        a.charge = 0;
        a.power = 1;
        if (o.skill == Charge) {
            a.phase = Charging;
            a.left = 0;
        } else if (o.skill == Guard) {
            a.phase = Active;
            a.left = r().guard_max;
        } else if (o.skill == Blink) {
            activate(i);
        } else {
            a.phase = Windup;
            a.left = m.windup;
        }
    }
    void release(Fighter& a) {
        a.power = r().charge_power(a.charge);
        a.phase = Windup;
        a.left = r().moves[Charge].windup;
    }
    void advance(int i) {
        Fighter& a = f[i];
        if (a.charging()) {
            if (++a.charge >= r().charge_auto) release(a);
        } else if (--a.left <= 0) {
            if (a.phase == Windup) activate(i);
            else if (a.guarding()) end_action(a, r().guard_lockout);
            else if (a.phase == Active) recover(a);
            else end_action(a, r().gap);
        }
    }
    void reappear(int i) {
        Fighter& a = f[i];
        a.p = r().respawn_position(i);
        a.v = {};
        a.grounded = false;
        a.drop = 0;
        a.air = r().air_jumps;
        a.invuln = r().invuln;
    }
    void control(int i, const Order& o, bool first) {
        Fighter& a = f[i];
        if (a.respawn > 0) {
            if (--a.respawn == 0) reappear(i);
            return;
        }
        if (a.invuln > 0) --a.invuln;
        if (a.hook_cd > 0) --a.hook_cd;
        if (a.drop > 0) --a.drop;
        a.meter = std::min(r().meter_max, a.meter + r().refill);
        if (a.pull > 0) return;
        if (a.stun > 0) {
            if (--a.stun == 0) a.gap = 0;
            return;
        }
        bool released = false;
        if (a.guarding() && o.skill != Guard) {
            end_action(a, r().gap);
        } else if (a.charging()) {
            aim(a, o.dir);
            if (o.skill == Guard || o.skill == Blink) {
                end_action(a, 0);
            } else if (o.skill != Charge) {
                release(a);
                released = true;
            }
        }
        if (a.act == None) {
            if (a.gap > 0) --a.gap;
            else start(i, o);
        } else if (!released) {
            advance(i);
        }
        if (first && o.jump && (a.act == None || a.act == Charge)) {
            if (a.grounded) {
                a.v.y = r().ground_speed;
                a.grounded = false;
            } else if (a.air > 0) {
                --a.air;
                a.v.y = r().air_speed;
            }
        }
        if (o.drop && a.grounded) a.drop = r().drop_ticks;
        if (a.guarding()) {
            if (a.grounded) a.v.x = 0;
        } else if (!a.blinking() && !a.reeling) {
            if (a.act != None && a.act != Charge) {
                if (a.grounded) a.v.x *= r().ground_friction;
            } else {
                const double accel = a.grounded ? r().ground_accel : r().air_accel;
                const double target = o.move * r().run_speed * (a.act == Charge ? r().charge_move : 1);
                a.v.x += std::clamp(target - a.v.x, -accel, accel);
                if (a.act == None && std::abs(o.move) > 0.05) a.facing = int(sign(o.move));
            }
        }
    }
    bool overlaps(const Fighter& a, const Solid& s) const {
        return a.p.x + r().width > s.left && a.p.x - r().width < s.right &&
               a.p.y < s.top && a.p.y + 2 * r().half_height > s.bottom;
    }
    void land(Fighter& a, double y) {
        a.p.y = y;
        a.v.y = 0;
        a.grounded = true;
        if (a.slam_landing) {
            a.stun = std::min(a.stun, r().slam_landing_cap);
            a.slam_landing = false;
        }
        a.air = r().air_jumps;
        if ((a.act == Slam || a.act == Upper) && a.phase == Active) recover(a);
    }
    void motion(int i) {
        Fighter& a = f[i];
        const double stop2 = r().hook_stop * r().hook_stop;
        if (a.reeling) {
            const Vec d = a.hook - a.p - Vec{0, r().half_height};
            if (norm2(d) <= stop2) {
                a.v = {0, 0.2};
                recover(a);
            } else {
                a.v = unit(d, {0, 1}) * r().hook_reel;
            }
        }
        if (a.pull > 0) {
            const Fighter& owner = f[a.pulled_by];
            const Vec d = owner.p - a.p;
            --a.pull;
            if (norm2(d) <= stop2 || !owner.present()) {
                a.v = {};
                a.pull = 0;
                a.pulled_by = -1;
            } else {
                a.v = unit(d) * r().hook_pull;
            }
        }
        if (!a.blinking() && !a.reeling && a.pull == 0) {
            a.v.y = std::max(a.v.y - r().gravity, a.stun > 0 ? -2.0 : -r().max_fall);
            if (a.stun > 0) a.v = a.v * r().hit_friction;
            else if (!a.grounded) a.v.x *= r().air_friction;
        }
        a.p.x += a.v.x;
        for (const Solid& s : r().solids) {
            if (!overlaps(a, s)) continue;
            a.p.x = a.v.x > 0 ? s.left - r().width : s.right + r().width;
            a.v.x = a.stun > 0 ? -a.v.x * r().bounce : 0;
        }
        const double foot = a.p.y;
        a.p.y += a.v.y;
        a.grounded = false;
        for (const Solid& s : r().solids) {
            if (!overlaps(a, s)) continue;
            if (a.v.y <= 0) land(a, s.top);
            else {
                a.p.y = s.bottom - 2 * r().half_height;
                a.v.y = a.stun > 0 ? -a.v.y * r().bounce : 0;
            }
        }
        if (a.v.y <= 0 && a.drop == 0 && !a.grounded) {
            for (const Platform& p : r().platforms) {
                if (foot >= p.y && a.p.y <= p.y && a.p.x + r().width > p.left && a.p.x - r().width < p.right) {
                    land(a, p.y);
                    break;
                }
            }
        }
    }
    void launch(int a, int t, Vec direction, double damage, double power, Skill source = None) {
        Fighter& target = f[t];
        target.damage += damage;
        dealt[a] += damage;
        const double speed = r().knockback(target.damage, power);
        cancel(t);
        target.pull = 0;
        target.pulled_by = -1;
        target.v = unit(direction, {target.p.x >= f[a].p.x ? 1.0 : -1.0, 0}) * speed;
        target.grounded = false;
        target.slam_landing = source == Slam && r().slam_landing_cap >= 0;
        target.stun = std::min(r().max_stun, r().stun_base + int(speed * r().stun_per_speed));
        target.gap = 0;
        target.last_hit = a;
        target.hit_tick = tick;
    }
    void melee() {
        struct Hit { int a, t; Skill skill; Vec dir; double damage, power; };
        std::array<Hit, seats * (seats - 1)> hits;
        int size = 0;
        for (int i = 0; i < seats; ++i) {
            Fighter& a = f[i];
            if (!a.present() || a.phase != Active || !is_melee(a.act)) continue;
            const MoveRule& m = r().moves[a.act];
            const Vec center = a.p + a.dir * m.offset;
            const double reach = m.radius + r().half_height;
            for (int j = 0; j < seats; ++j) {
                if (i == j || (a.hit_mask & (1 << j)) || f[j].untouchable()) continue;
                if (norm2(f[j].p - center) > reach * reach) continue;
                a.hit_mask |= 1 << j;
                const double power = a.act == Charge ? a.power : 1;
                const double launch_scale = a.act == Charge ? r().charge_launch_scale(a.charge, power) : power;
                hits[size++] = {i, j, a.act, a.dir + Vec{0, m.lift}, m.damage * power, m.power * launch_scale};
            }
        }
        for (int k = 0; k < size; ++k) {
            const Hit& h = hits[k];
            Fighter& t = f[h.t];
            if (t.untouchable()) continue;
            if (h.skill != Grab && h.skill != Slam && t.countering()) {
                t.phase = Recover;
                t.left = r().counter_recover;
                launch(h.t, h.a, {f[h.a].p.x - t.p.x, 0.4}, h.damage * r().counter_mult, h.power * r().counter_mult);
            } else if (h.skill != Grab && t.guarding()) {
                t.v.x = (f[h.a].p.x <= t.p.x ? 1 : -1) * r().guard_push;
            } else {
                launch(h.a, h.t, h.dir, h.damage, h.power, h.skill);
            }
        }
    }
    void projectiles() {
        for (int k = 0; k < shot_count; ++k) {
            Shot& s = shots[k];
            if (s.dead) continue;
            s.p += s.v;
            --s.ttl;
            Fighter& a = f[s.owner];
            if (r().in_solid(s.p)) {
                if (s.kind == Hook && a.act == Hook) {
                    a.reeling = true;
                    a.hook = s.p;
                    a.phase = Active;
                    a.left = r().hook_ticks;
                }
                s.dead = true;
                continue;
            }
            const double reach = r().moves[s.kind].radius + r().half_height;
            for (int i = 0; i < seats; ++i) {
                Fighter& t = f[i];
                if (i == s.owner || t.untouchable() || norm2(t.p + Vec{0, r().half_height} - s.p) > reach * reach) continue;
                if (s.kind == Hook) {
                    cancel(i);
                    t.damage += r().moves[Hook].damage;
                    t.slam_landing = false;
                    dealt[s.owner] += r().moves[Hook].damage;
                    t.pulled_by = s.owner;
                    t.pull = r().hook_ticks;
                    t.stun = 0;
                    t.grounded = false;
                    t.last_hit = s.owner;
                    t.hit_tick = tick;
                    if (a.act == Hook) recover(a);
                } else if (t.countering()) {
                    t.phase = Recover;
                    t.left = r().counter_recover;
                    s.owner = i;
                    s.v = s.v * -1;
                    s.damage *= r().counter_mult;
                    s.power *= r().counter_mult;
                    s.ttl = r().moves[Wave].ttl;
                    break;
                } else if (!t.guarding()) {
                    launch(s.owner, i, s.v + Vec{0, 0.2}, s.damage, s.power);
                }
                s.dead = true;
                break;
            }
            if (!s.dead && s.ttl <= 0) {
                if (s.kind == Hook) {
                    a.hook_cd = r().hook_miss;
                    if (a.act == Hook) recover(a);
                }
                s.dead = true;
            }
        }
    }
    void blast_check(int i, Bounds b) {
        Fighter& a = f[i];
        if (std::abs(a.p.x) <= b.x && a.p.y <= b.top && a.p.y >= b.bottom) return;
        if (a.last_hit >= 0 && a.last_hit != i && tick - a.hit_tick <= r().credit) ++kills[a.last_hit];
        --a.stocks;
        cancel(i);
        a.slam_landing = false;
        a.damage = 0;
        a.v = {};
        a.stun = a.gap = a.pull = 0;
        a.pulled_by = a.last_hit = -1;
        a.hit_tick = -1000;
        if (a.stocks == 0) a.out = true;
        else a.respawn = r().respawn;
    }
    void quantize(Fighter& a) const {
        a.p = {r().round(a.p.x), r().round(a.p.y)};
        a.v = {r().round(a.v.x), r().round(a.v.y)};
        a.damage = r().round(a.damage);
        a.meter = r().round(a.meter);
    }
    void step(const Orders& orders, bool first) {
        ++tick;
        for (int i = 0; i < seats; ++i) if (!f[i].out) control(i, orders[i], first);
        for (int i = 0; i < seats; ++i) if (f[i].present()) motion(i);
        projectiles();
        melee();
        const Bounds b = r().bounds(tick);
        for (int i = 0; i < seats; ++i) {
            if (f[i].present()) blast_check(i, b);
            quantize(f[i]);
        }
        int kept = 0;
        for (int k = 0; k < shot_count; ++k) {
            if (shots[k].dead) continue;
            Shot& s = shots[k];
            s.p = {r().round(s.p.x), r().round(s.p.y)};
            shots[kept++] = s;
        }
        shot_count = kept;
    }
};

}  // namespace ring
