#include "include/step_worker.hpp"
#include "include/physics.hpp"
#include "include/setup.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using mini_json::Json;
using J = Json;

namespace {

constexpr int MAX_PLAYERS = 8;
constexpr int MAX_BOMBS = 20;
constexpr int MAX_WALLS = 32;

enum Skill : int { SKILL_NONE = 0, SKILL_DASH = 1, SKILL_HAMMER = 2 };
enum ActType : int { ACT_NONE = 0, ACT_RECOVER = 1, ACT_JAB = 2, ACT_JAB_RECOVER = 3 };
enum WallKind : int { WALL_SPRING = 0, WALL_NORMAL = 1 };

inline double fround4(double x) {
    return std::nearbyint(x * 10000.0) * 0.0001;
}

struct WallDef {
    int id = 0;
    int kind = WALL_NORMAL;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    double minx = 0, maxx = 0, miny = 0, maxy = 0;
    double max_abs = 0;
    bool valid = false;
};

struct ShrinkPhase {
    int warn_turn = 0;
    int turn = 0;
    int end_turn = 0;
    double from = 0;
    double half = 0;
};

struct SimContext {
    int n = 4;
    int you = 0;
    int turn = 1;
    int base_tick = 0;
    int D = 6;
    int max_turns = 200;
    int precision = 4;
    double init_half = 16.5;
    double curr_half = 16.5;
    double next_half = 16.5;
    int next_turn = 999;

    double player_radius = 1.0;
    double friction = 0.06;
    double restitution = 0.9;
    double min_speed = 0.001;
    double min_impulse = 0.03;

    double move_accel = 0.018;
    double dash_charging_factor = 0.2;
    double hammer_charging_factor = 0.3;

    double charge_rate = 0.04;
    double refill_rate = 0.02;
    int pause_ticks = 30;
    int overcharge_ticks = 45;

    double dash_speed = 0.9;

    double hammer_reach = 3.2;
    double hammer_cone_cos = 0.5;
    double hammer_power = 0.9;
    double hammer_recoil = 0.15;
    int hammer_recover_ticks = 5;
    int hammer_cd_ticks = 60;

    int attack_windup_ticks = 9;
    double attack_reach = 2.8;
    double attack_cone_cos = 0.5;
    double attack_power = 0.35;
    double attack_recoil = 0.05;
    int attack_recover_ticks = 6;
    int attack_cd_ticks = 15;

    double bomb_radius = 0.6;
    double bomb_mass = 0.5;
    double bomb_friction = 0.05;
    double bomb_throw_speed = 0.5;
    double bomb_spawn_gap = 0.2;
    int bomb_fuse_min = 24;
    int bomb_fuse_max = 90;
    int bomb_cd_ticks = 90;
    double bomb_blast_radius = 3.5;
    double bomb_blast_power = 1.4;

    double wall_thickness = 0.5;
    double wall_restitution = 0.8;
    int wall_regen_ticks = 300;
    int wall_warn_ticks = 90;
    double wall_regen_push = 0.5;

    double spring_factor = 1.5;
    double spring_max_speed = 0.5;

    int kill_credit_ticks = 90;

    int max_wall_id = -1;
    std::array<WallDef, MAX_WALLS> walls{};
    std::vector<ShrinkPhase> phases;
    std::array<double, 128> half_at_dt{};
};

struct SimOrder {
    bool has_move = false;
    double move_x = 0, move_y = 0;
    int charge = SKILL_NONE;
    bool has_dash = false;
    double dash_x = 0, dash_y = 0;
    bool has_hammer = false;
    double hammer_x = 0, hammer_y = 0;
    bool has_attack = false;
    double attack_x = 0, attack_y = 0;
    bool has_bomb = false;
    double bomb_x = 0, bomb_y = 0;
    int bomb_fuse = 30;

    void set_move(double dx, double dy) {
        auto u = knockback::unit(dx, dy);
        if (u) {
            has_move = true;
            move_x = u->first;
            move_y = u->second;
        } else {
            has_move = false;
            move_x = move_y = 0;
        }
    }
    void set_dash(double dx, double dy) {
        auto u = knockback::unit(dx, dy);
        if (u) {
            has_dash = true;
            dash_x = u->first;
            dash_y = u->second;
        }
    }
    void set_hammer(double dx, double dy) {
        auto u = knockback::unit(dx, dy);
        if (u) {
            has_hammer = true;
            hammer_x = u->first;
            hammer_y = u->second;
        }
    }
    void set_attack(double dx, double dy) {
        auto u = knockback::unit(dx, dy);
        if (u) {
            has_attack = true;
            attack_x = u->first;
            attack_y = u->second;
        }
    }
    void set_bomb(double dx, double dy, int fuse) {
        auto u = knockback::unit(dx, dy);
        if (u) {
            has_bomb = true;
            bomb_x = u->first;
            bomb_y = u->second;
            bomb_fuse = fuse;
        }
    }

    J to_json() const {
        J o = J::object{};
        if (has_move) o["move"] = J::array{move_x, move_y};
        if (charge == SKILL_DASH) o["charge"] = "dash";
        else if (charge == SKILL_HAMMER) o["charge"] = "hammer";
        if (has_dash) o["dash"] = J::array{dash_x, dash_y};
        if (has_hammer) o["hammer"] = J::array{hammer_x, hammer_y};
        if (has_attack) o["attack"] = J::array{attack_x, attack_y};
        if (has_bomb) o["bomb"] = J{{"dir", J::array{bomb_x, bomb_y}}, {"fuse", bomb_fuse}};
        return o;
    }
};

struct SimPlayer {
    double x = 0, y = 0, vx = 0, vy = 0;
    double bar = 1.0, charge = 0.0;
    double aim_x = 0, aim_y = 0;
    int skill = SKILL_NONE;
    int pause = 0;
    bool out = false;
    int act = ACT_NONE;
    int act_ticks = 0;
    int bomb_cd = 0;
    int attack_cd = 0;
    int hammer_cd = 0;
    int held = 0;
    int last_hit_who = -1;
    int last_hit_tick = -999;
    int kills = 0;
    int out_tick = 0;
    bool overcharged = false;
};

struct SimBomb {
    int id = 0;
    double x = 0, y = 0, vx = 0, vy = 0;
    int fuse = 30;
    int owner = 0;
};

struct SimBroken {
    int wall = 0;
    int at = 0;
};

struct SimState {
    int tick = 0;
    double half = 16.5;
    int next_bomb = 0;
    std::array<SimPlayer, MAX_PLAYERS> players{};
    int num_bombs = 0;
    std::array<SimBomb, MAX_BOMBS> bombs{};
    int num_standing = 0;
    std::array<int, MAX_WALLS> standing{};
    int num_broken = 0;
    std::array<SimBroken, MAX_WALLS> broken{};

    int my_hammer_hits = 0;
    double my_hammer_hit_power = 0.0;
    int my_hammer_bomb_hits = 0;
    int my_jab_hits = 0;
    int my_jab_bomb_hits = 0;
    int my_bomb_hits = 0;
    int my_bumps_dealt = 0;
    double my_impulse_dealt = 0.0;
    bool hit_by_enemy_hammer = false;
    bool hit_by_enemy_jab = false;
    bool hit_by_enemy_bomb = false;
};

inline std::pair<double, double> nearest_on_wall(double px, double py, const WallDef& w) {
    double ax = w.x1, ay = w.y1, ex = w.x2 - ax, ey = w.y2 - ay;
    double l = ex * ex + ey * ey;
    double t = (l == 0.0) ? 0.0 : std::max(0.0, std::min(1.0, ((px - ax) * ex + (py - ay) * ey) / l));
    return {ax + t * ex, ay + t * ey};
}

void parse_observation(const J& obs, SimContext& ctx, SimState& st) {
    ctx.turn = int(obs.at("turn").as_int());
    ctx.base_tick = int(obs.at("tick").as_int());
    ctx.you = int(obs.at("you").as_int());
    ctx.n = int(obs.at("n").as_int());

    const J& r = obs.at("rules");
    ctx.D = int(r.at("decision_ticks").as_int());
    ctx.max_turns = int(r.at("max_turns").as_int());
    ctx.precision = int(r.at("precision").as_int());
    ctx.init_half = r.at("arena").at("half").as_double();
    ctx.curr_half = obs.at("arena").at("half").as_double();
    ctx.next_half = ctx.curr_half;
    ctx.next_turn = 999;
    if (obs.at("arena").contains("next") && !obs.at("arena").at("next").is_null()) {
        const J& nx = obs.at("arena").at("next");
        ctx.next_half = nx.at("half").as_double();
        ctx.next_turn = int(nx.at("turn").as_int());
    }

    ctx.player_radius = r.at("player").at("radius").as_double();
    ctx.friction = r.at("physics").at("friction").as_double();
    ctx.restitution = r.at("physics").at("restitution").as_double();
    ctx.min_speed = r.at("physics").at("min_speed").as_double();
    ctx.min_impulse = r.at("physics").at("min_impulse").as_double();

    ctx.move_accel = r.at("move").at("accel").as_double();
    ctx.dash_charging_factor = r.at("move").at("charging_factor").as_double();
    ctx.hammer_charging_factor = r.at("hammer").at("charging_factor").as_double();

    ctx.charge_rate = r.at("bar").at("charge_rate").as_double();
    ctx.refill_rate = r.at("bar").at("refill_rate").as_double();
    ctx.pause_ticks = int(r.at("bar").at("pause_ticks").as_int());
    ctx.overcharge_ticks = int(r.at("bar").at("overcharge_ticks").as_int());

    ctx.dash_speed = r.at("dash").at("speed").as_double();

    ctx.hammer_reach = r.at("hammer").at("reach").as_double();
    ctx.hammer_cone_cos = r.at("hammer").at("cone_cos").as_double();
    ctx.hammer_power = r.at("hammer").at("power").as_double();
    ctx.hammer_recoil = r.at("hammer").at("recoil").as_double();
    ctx.hammer_recover_ticks = int(r.at("hammer").at("recover_ticks").as_int());
    ctx.hammer_cd_ticks = int(r.at("hammer").at("cooldown_ticks").as_int());

    ctx.attack_windup_ticks = int(r.at("attack").at("windup_ticks").as_int());
    ctx.attack_reach = r.at("attack").at("reach").as_double();
    ctx.attack_cone_cos = r.at("attack").at("cone_cos").as_double();
    ctx.attack_power = r.at("attack").at("power").as_double();
    ctx.attack_recoil = r.at("attack").at("recoil").as_double();
    ctx.attack_recover_ticks = int(r.at("attack").at("recover_ticks").as_int());
    ctx.attack_cd_ticks = int(r.at("attack").at("cooldown_ticks").as_int());

    ctx.bomb_radius = r.at("bomb").at("radius").as_double();
    ctx.bomb_mass = r.at("bomb").at("mass").as_double();
    ctx.bomb_friction = r.at("bomb").at("friction").as_double();
    ctx.bomb_throw_speed = r.at("bomb").at("throw_speed").as_double();
    ctx.bomb_spawn_gap = r.at("bomb").at("spawn_gap").as_double();
    ctx.bomb_fuse_min = int(r.at("bomb").at("fuse_min").as_int());
    ctx.bomb_fuse_max = int(r.at("bomb").at("fuse_max").as_int());
    ctx.bomb_cd_ticks = int(r.at("bomb").at("cooldown_ticks").as_int());
    ctx.bomb_blast_radius = r.at("bomb").at("blast_radius").as_double();
    ctx.bomb_blast_power = r.at("bomb").at("blast_power").as_double();

    ctx.wall_thickness = r.at("walls").at("thickness").as_double();
    ctx.wall_restitution = r.at("walls").at("restitution").as_double();
    ctx.wall_regen_ticks = int(r.at("walls").at("regen_ticks").as_int());
    ctx.wall_warn_ticks = int(r.at("walls").at("warn_ticks").as_int());
    ctx.wall_regen_push = r.at("walls").at("regen_push").as_double();

    ctx.spring_factor = r.at("spring").at("factor").as_double();
    ctx.spring_max_speed = r.at("spring").at("max_speed").as_double();

    ctx.kill_credit_ticks = int(r.at("kill_credit_ticks").as_int());

    ctx.phases.clear();
    J phases_json = knockback::setup_phases(r);
    for (const auto& p : phases_json.as_array()) {
        ctx.phases.push_back(ShrinkPhase{
            int(p.at("warn_turn").as_int()),
            int(p.at("turn").as_int()),
            int(p.at("end_turn").as_int()),
            p.at("from").as_double(),
            p.at("half").as_double()
        });
    }

    double h = ctx.curr_half;
    ctx.half_at_dt[0] = h;
    for (int dt = 1; dt < 128; ++dt) {
        int t = ctx.base_tick + dt;
        for (const auto& p : ctx.phases) {
            int start = (p.turn - 1) * ctx.D + 1;
            int end = p.end_turn * ctx.D;
            if (start <= t && t <= end) {
                h = knockback::py_round(p.from + (p.half - p.from) * (t - start + 1) / (end - start + 1), ctx.precision);
            }
        }
        ctx.half_at_dt[dt] = h;
    }

    st = SimState{};
    st.tick = ctx.base_tick;
    st.half = ctx.curr_half;

    for (int i = 0; i < ctx.n; ++i) {
        const J& pj = obs.at("players").at(i);
        SimPlayer& sp = st.players[i];
        sp.x = pj.at("x").as_double();
        sp.y = pj.at("y").as_double();
        sp.vx = pj.at("vx").as_double();
        sp.vy = pj.at("vy").as_double();
        sp.bar = pj.at("bar").as_double();
        sp.charge = pj.at("charge").as_double();
        if (pj.at("skill").is_null()) sp.skill = SKILL_NONE;
        else if (pj.at("skill").as_string() == "dash") sp.skill = SKILL_DASH;
        else sp.skill = SKILL_HAMMER;
        sp.pause = int(pj.at("pause").as_int());
        sp.out = pj.at("out").as_bool();
        if (pj.at("act").is_null()) sp.act = ACT_NONE;
        else if (pj.at("act").as_string() == "recover") sp.act = ACT_RECOVER;
        else if (pj.at("act").as_string() == "jab") sp.act = ACT_JAB;
        else sp.act = ACT_JAB_RECOVER;
        sp.act_ticks = int(pj.at("act_ticks").as_int());
        if (!pj.at("aim").is_null()) {
            sp.aim_x = pj.at("aim").at(0).as_double();
            sp.aim_y = pj.at("aim").at(1).as_double();
        }
        sp.bomb_cd = int(pj.at("bomb_cd").as_int());
        sp.attack_cd = int(pj.at("attack_cd").as_int());
        sp.hammer_cd = int(pj.at("hammer_cd").as_int());
        sp.held = int(pj.at("held").as_int());
        sp.kills = int(obs.at("kills").at(i).as_int());

        const J& lh = obs.at("last_hit").at(i);
        if (!lh.is_null()) {
            sp.last_hit_who = int(lh.at(0).as_int());
            sp.last_hit_tick = int(lh.at(1).as_int());
        } else {
            sp.last_hit_who = -1;
            sp.last_hit_tick = -999;
        }
    }

    st.num_bombs = 0;
    st.next_bomb = 0;
    for (const auto& bj : obs.at("bombs").as_array()) {
        if (st.num_bombs >= MAX_BOMBS) break;
        SimBomb& sb = st.bombs[st.num_bombs++];
        sb.id = int(bj.at("id").as_int());
        sb.x = bj.at("x").as_double();
        sb.y = bj.at("y").as_double();
        sb.vx = bj.at("vx").as_double();
        sb.vy = bj.at("vy").as_double();
        sb.fuse = int(bj.at("fuse").as_int());
        sb.owner = int(bj.at("owner").as_int());
        st.next_bomb = std::max(st.next_bomb, sb.id + 1);
    }

    ctx.max_wall_id = -1;
    for (auto& w : ctx.walls) w.valid = false;

    auto register_wall = [&](int id, const J& wj) {
        if (id < 0 || id >= MAX_WALLS) return;
        WallDef& wd = ctx.walls[id];
        wd.id = id;
        wd.kind = (wj.at("kind").as_string() == "spring") ? WALL_SPRING : WALL_NORMAL;
        wd.x1 = wj.at("x1").as_double();
        wd.y1 = wj.at("y1").as_double();
        wd.x2 = wj.at("x2").as_double();
        wd.y2 = wj.at("y2").as_double();
        double x2_interp = wd.x1 + (wd.x2 - wd.x1);
        double y2_interp = wd.y1 + (wd.y2 - wd.y1);
        wd.minx = std::min(wd.x1, x2_interp);
        wd.maxx = std::max(wd.x1, x2_interp);
        wd.miny = std::min(wd.y1, y2_interp);
        wd.maxy = std::max(wd.y1, y2_interp);
        wd.max_abs = std::max({std::abs(wd.x1), std::abs(wd.y1), std::abs(wd.x2), std::abs(wd.y2)});
        wd.valid = true;
        ctx.max_wall_id = std::max(ctx.max_wall_id, id);
    };

    st.num_standing = 0;
    for (const auto& wj : obs.at("walls").as_array()) {
        int id = int(wj.at("id").as_int());
        register_wall(id, wj);
        if (st.num_standing < MAX_WALLS) st.standing[st.num_standing++] = id;
    }

    st.num_broken = 0;
    for (const auto& bj : obs.at("broken").as_array()) {
        int id = int(bj.at("wall").as_int());
        register_wall(id, bj);
        if (st.num_broken < MAX_WALLS) {
            st.broken[st.num_broken++] = SimBroken{id, int(bj.at("at").as_int())};
        }
    }
}

void sim_swing(const SimContext& ctx, SimState& st, int seat, int tick, const int* alive, int num_alive,
               bool hammer, double ax, double ay, double scale) {
    SimPlayer& p = st.players[seat];
    double reach = hammer ? ctx.hammer_reach : ctx.attack_reach;
    double gain = hammer ? ctx.hammer_power : ctx.attack_power;
    double cone = hammer ? ctx.hammer_cone_cos : ctx.attack_cone_cos;
    double recoil = hammer ? ctx.hammer_recoil : ctx.attack_recoil;

    for (int idx = 0; idx < num_alive; ++idx) {
        int i = alive[idx];
        if (i == seat || st.players[i].out) continue;
        SimPlayer& q = st.players[i];
        double dx = q.x - p.x, dy = q.y - p.y;
        if (dx * dx + dy * dy > reach * reach) continue;
        auto u = knockback::unit(dx, dy).value_or(std::make_pair(ax, ay));
        if (u.first * ax + u.second * ay < cone) continue;
        q.vx += u.first * gain * scale;
        q.vy += u.second * gain * scale;
        q.last_hit_who = seat;
        q.last_hit_tick = tick;
        if (seat == ctx.you) {
            if (hammer) {
                st.my_hammer_hits++;
                st.my_hammer_hit_power += scale;
            } else {
                st.my_jab_hits++;
            }
            st.my_impulse_dealt += gain * scale;
        } else if (i == ctx.you) {
            if (hammer) st.hit_by_enemy_hammer = true;
            else st.hit_by_enemy_jab = true;
        }
    }

    for (int b_idx = 0; b_idx < st.num_bombs; ++b_idx) {
        SimBomb& b = st.bombs[b_idx];
        double dx = b.x - p.x, dy = b.y - p.y;
        if (dx * dx + dy * dy > reach * reach) continue;
        auto u = knockback::unit(dx, dy).value_or(std::make_pair(ax, ay));
        if (u.first * ax + u.second * ay < cone) continue;
        b.vx += u.first * gain * scale / ctx.bomb_mass;
        b.vy += u.second * gain * scale / ctx.bomb_mass;
        b.owner = seat;
        if (seat == ctx.you) {
            if (hammer) st.my_hammer_bomb_hits++;
            else st.my_jab_bomb_hits++;
        }
    }

    p.vx -= ax * recoil * scale;
    p.vy -= ay * recoil * scale;
}

void sim_throw_bomb(const SimContext& ctx, SimState& st, int seat, const SimOrder& o) {
    if (st.num_bombs >= MAX_BOMBS) return;
    SimPlayer& p = st.players[seat];
    double gap = ctx.player_radius + ctx.bomb_radius + ctx.bomb_spawn_gap;
    SimBomb& b = st.bombs[st.num_bombs++];
    b.id = st.next_bomb++;
    b.x = p.x + o.bomb_x * gap;
    b.y = p.y + o.bomb_y * gap;
    b.vx = p.vx + o.bomb_x * ctx.bomb_throw_speed;
    b.vy = p.vy + o.bomb_y * ctx.bomb_throw_speed;
    b.fuse = o.bomb_fuse;
    b.owner = seat;
    p.bomb_cd = ctx.bomb_cd_ticks;
}

void sim_explode(const SimContext& ctx, SimState& st, const SimBomb& dead, int tick, const int* alive, int num_alive) {
    double reach = ctx.bomb_blast_radius;
    double reach2 = reach * reach;
    for (int idx = 0; idx < num_alive; ++idx) {
        int s = alive[idx];
        SimPlayer& p = st.players[s];
        if (p.out) continue;
        double dx = p.x - dead.x, dy = p.y - dead.y;
        double d2 = dx * dx + dy * dy;
        if (d2 > reach2) continue;
        auto u = knockback::unit(dx, dy).value_or(std::make_pair(1.0, 0.0));
        double gain = ctx.bomb_blast_power * (1.0 - std::sqrt(d2) / reach);
        p.vx += u.first * gain;
        p.vy += u.second * gain;
        p.last_hit_who = dead.owner;
        p.last_hit_tick = tick;
        if (dead.owner == ctx.you && s != ctx.you) {
            st.my_bomb_hits++;
            st.my_impulse_dealt += gain;
        } else if (s == ctx.you) {
            st.hit_by_enemy_bomb = true;
        }
    }

    for (int i = 0; i < st.num_bombs; ++i) {
        SimBomb& b = st.bombs[i];
        double dx = b.x - dead.x, dy = b.y - dead.y;
        double d2 = dx * dx + dy * dy;
        if (d2 > reach2) continue;
        auto u = knockback::unit(dx, dy).value_or(std::make_pair(1.0, 0.0));
        double gain = ctx.bomb_blast_power * (1.0 - std::sqrt(d2) / reach) / ctx.bomb_mass;
        b.vx += u.first * gain;
        b.vy += u.second * gain;
    }

    int new_num_standing = 0;
    for (int i = 0; i < st.num_standing; ++i) {
        int id = st.standing[i];
        const WallDef& w = ctx.walls[id];
        if (w.kind == WALL_NORMAL) {
            auto q = nearest_on_wall(dead.x, dead.y, w);
            double dx = q.first - dead.x, dy = q.second - dead.y;
            if (dx * dx + dy * dy <= reach2) {
                if (st.num_broken < MAX_WALLS) {
                    st.broken[st.num_broken++] = SimBroken{id, tick + ctx.wall_regen_ticks};
                    std::sort(st.broken.begin(), st.broken.begin() + st.num_broken,
                              [](const SimBroken& a, const SimBroken& b) { return a.wall < b.wall; });
                }
                continue;
            }
        }
        st.standing[new_num_standing++] = id;
    }
    st.num_standing = new_num_standing;
}

void sim_regrow(const SimContext& ctx, SimState& st, const WallDef& w, const int* alive, int num_alive) {
    if (st.num_standing < MAX_WALLS) {
        st.standing[st.num_standing++] = w.id;
        std::sort(st.standing.begin(), st.standing.begin() + st.num_standing);
    }
    auto push_body = [&](double& x, double& y, double& vx, double& vy, double radius) {
        double reach = radius + ctx.wall_thickness * 0.5;
        auto q = nearest_on_wall(x, y, w);
        double dx = x - q.first, dy = y - q.second;
        if (dx * dx + dy * dy >= reach * reach) return;
        auto u = knockback::unit(dx, dy).value_or(
            knockback::unit(-(w.y2 - w.y1), w.x2 - w.x1).value_or(std::make_pair(1.0, 0.0)));
        x = q.first + u.first * reach;
        y = q.second + u.second * reach;
        double normal = vx * u.first + vy * u.second;
        vx += (ctx.wall_regen_push - normal) * u.first;
        vy += (ctx.wall_regen_push - normal) * u.second;
    };
    for (int idx = 0; idx < num_alive; ++idx) {
        int s = alive[idx];
        if (!st.players[s].out) {
            push_body(st.players[s].x, st.players[s].y, st.players[s].vx, st.players[s].vy, ctx.player_radius);
        }
    }
    for (int i = 0; i < st.num_bombs; ++i) {
        push_body(st.bombs[i].x, st.bombs[i].y, st.bombs[i].vx, st.bombs[i].vy, ctx.bomb_radius);
    }
}

void sim_act(const SimContext& ctx, SimState& st, int seat, const SimOrder& o, bool first, int tick,
             const int* alive, int num_alive) {
    SimPlayer& p = st.players[seat];
    p.attack_cd = std::max(0, p.attack_cd - 1);
    p.hammer_cd = std::max(0, p.hammer_cd - 1);
    bool busy = (p.act != ACT_NONE);

    if (busy) {
        p.act_ticks -= 1;
        if (p.act_ticks <= 0) {
            if (p.act == ACT_JAB) {
                sim_swing(ctx, st, seat, tick, alive, num_alive, false, p.aim_x, p.aim_y, 1.0);
                p.act = ACT_JAB_RECOVER;
                p.act_ticks = ctx.attack_recover_ticks;
                p.aim_x = p.aim_y = 0.0;
            }
            if ((p.act == ACT_RECOVER || p.act == ACT_JAB_RECOVER) && p.act_ticks <= 0) {
                p.act = ACT_NONE;
                p.act_ticks = 0;
            }
        }
    }

    int want = o.charge;
    int skill = p.skill;
    if (want == SKILL_HAMMER && p.hammer_cd > 0) want = SKILL_NONE;
    bool has_rel_order = (skill == SKILL_DASH && o.has_dash) || (skill == SKILL_HAMMER && o.has_hammer);
    bool release = !busy && first && skill != SKILL_NONE && p.charge > 0.0 && has_rel_order;
    if (!busy && !release && want != SKILL_NONE && skill != SKILL_NONE && skill != want) {
        p.charge = 0.0;
        p.skill = SKILL_NONE;
        skill = SKILL_NONE;
    }
    bool jab = first && !busy && o.has_attack && p.attack_cd == 0 && !(release && skill == SKILL_HAMMER);
    bool charging = !busy && !release && !jab && want != SKILL_NONE && p.bar > 0.0;

    if (p.pause > 0) {
        p.pause -= 1;
    } else if (!charging) {
        p.bar = std::max(p.bar, std::min(1.0 - p.charge, p.bar + ctx.refill_rate));
    }

    if (release) {
        double power = p.charge;
        p.charge = 0.0;
        p.skill = SKILL_NONE;
        p.pause = ctx.pause_ticks;
        if (skill == SKILL_DASH) {
            p.vx += o.dash_x * ctx.dash_speed * power;
            p.vy += o.dash_y * ctx.dash_speed * power;
        } else {
            p.hammer_cd = ctx.hammer_cd_ticks;
            sim_swing(ctx, st, seat, tick, alive, num_alive, true, o.hammer_x, o.hammer_y, power);
            p.act = ACT_RECOVER;
            p.act_ticks = ctx.hammer_recover_ticks;
            p.aim_x = p.aim_y = 0.0;
        }
    }

    if (jab) {
        p.act = ACT_JAB;
        p.act_ticks = ctx.attack_windup_ticks;
        p.aim_x = o.attack_x;
        p.aim_y = o.attack_y;
        p.attack_cd = ctx.attack_cd_ticks;
    }

    if (charging) {
        double amount = std::min(ctx.charge_rate, p.bar);
        p.bar -= amount;
        p.charge += amount;
        p.skill = want;
    }

    if (p.charge <= 0.0 || charging) {
        p.held = 0;
    } else {
        p.held += 1;
        if (p.held >= ctx.overcharge_ticks) {
            p.charge = 0.0;
            p.skill = SKILL_NONE;
            p.held = 0;
            p.pause = ctx.pause_ticks;
            p.overcharged = true;
        }
    }

    if (o.has_move && !busy && p.act == ACT_NONE) {
        double factor = (p.skill == SKILL_DASH) ? ctx.dash_charging_factor
                      : (p.skill == SKILL_HAMMER) ? ctx.hammer_charging_factor : 1.0;
        p.vx += o.move_x * ctx.move_accel * factor;
        p.vy += o.move_y * ctx.move_accel * factor;
    }

    if (p.bomb_cd > 0) {
        p.bomb_cd -= 1;
    } else if (first && o.has_bomb && !busy && p.act == ACT_NONE) {
        sim_throw_bomb(ctx, st, seat, o);
    }
}

void sim_bounce(const SimContext& ctx, double& x, double& y, double& vx, double& vy, double radius, const WallDef& w) {
    double reach = radius + ctx.wall_thickness * 0.5;
    if (x < w.minx - reach - 1e-9 || x > w.maxx + reach + 1e-9 ||
        y < w.miny - reach - 1e-9 || y > w.maxy + reach + 1e-9) {
        return;
    }
    auto q = nearest_on_wall(x, y, w);
    double dx = x - q.first, dy = y - q.second;
    double d2 = dx * dx + dy * dy;
    if (d2 >= reach * reach) return;
    std::pair<double, double> u;
    if (d2 > 0.0) {
        double d = std::sqrt(d2);
        u = {dx / d, dy / d};
    } else {
        u = knockback::unit(-(w.y2 - w.y1), w.x2 - w.x1).value_or(std::make_pair(1.0, 0.0));
    }
    double nx = u.first, ny = u.second;
    x = q.first + nx * reach;
    y = q.second + ny * reach;
    double inward = vx * nx + vy * ny;
    if (inward >= 0.0) return;
    double speed = (w.kind == WALL_NORMAL)
                 ? (ctx.wall_restitution * -inward)
                 : std::min(ctx.spring_factor * -inward, ctx.spring_max_speed);
    vx += (speed - inward) * nx;
    vy += (speed - inward) * ny;
}

void sim_tick(const SimContext& ctx, SimState& st, const std::array<SimOrder, MAX_PLAYERS>& orders, bool first) {
    st.tick += 1;
    int t = st.tick;
    int dt = t - ctx.base_tick;
    if (dt >= 0 && dt < 128) {
        st.half = ctx.half_at_dt[dt];
    }

    int new_standing = 0;
    for (int i = 0; i < st.num_standing; ++i) {
        int id = st.standing[i];
        if (ctx.walls[id].max_abs <= st.half) {
            st.standing[new_standing++] = id;
        }
    }
    st.num_standing = new_standing;

    int alive[MAX_PLAYERS];
    int num_alive = 0;
    for (int i = 0; i < ctx.n; ++i) {
        if (!st.players[i].out) alive[num_alive++] = i;
    }

    for (int k = 0; k < st.num_broken;) {
        SimBroken e = st.broken[k];
        const WallDef& w = ctx.walls[e.wall];
        bool remove = false;
        if (w.max_abs > st.half) {
            remove = true;
        } else if (t >= e.at) {
            remove = true;
            sim_regrow(ctx, st, w, alive, num_alive);
        }
        if (remove) {
            for (int j = k; j + 1 < st.num_broken; ++j) st.broken[j] = st.broken[j + 1];
            st.num_broken -= 1;
        } else {
            ++k;
        }
    }

    for (int k = 0; k < st.num_bombs;) {
        st.bombs[k].fuse -= 1;
        if (st.bombs[k].fuse <= 0) {
            SimBomb dead = st.bombs[k];
            for (int j = k; j + 1 < st.num_bombs; ++j) st.bombs[j] = st.bombs[j + 1];
            st.num_bombs -= 1;
            sim_explode(ctx, st, dead, t, alive, num_alive);
        } else {
            ++k;
        }
    }

    for (int idx = 0; idx < num_alive; ++idx) {
        int s = alive[idx];
        sim_act(ctx, st, s, orders[s], first, t, alive, num_alive);
    }

    for (int idx = 0; idx < num_alive; ++idx) {
        int s = alive[idx];
        st.players[s].x += st.players[s].vx;
        st.players[s].y += st.players[s].vy;
    }
    for (int k = 0; k < st.num_bombs; ++k) {
        st.bombs[k].x += st.bombs[k].vx;
        st.bombs[k].y += st.bombs[k].vy;
    }

    double sr = ctx.player_radius;
    double br = ctx.bomb_radius;
    double bm = ctx.bomb_mass;
    double least = ctx.min_impulse;

    for (int k = 0; k < num_alive; ++k) {
        for (int l = k + 1; l < num_alive; ++l) {
            int i = alive[k], j = alive[l];
            SimPlayer& pa = st.players[i];
            SimPlayer& pb = st.players[j];
            double dx = pb.x - pa.x, dy = pb.y - pa.y;
            if (dx * dx + dy * dy >= (sr + sr) * (sr + sr)) continue;
            knockback::Body aa{pa.x, pa.y, pa.vx, pa.vy};
            knockback::Body bb{pb.x, pb.y, pb.vx, pb.vy};
            double impulse = knockback::pair(aa, bb, sr + sr, 1.0, 1.0, ctx.restitution);
            pa.x = aa.x; pa.y = aa.y; pa.vx = aa.vx; pa.vy = aa.vy;
            pb.x = bb.x; pb.y = bb.y; pb.vx = bb.vx; pb.vy = bb.vy;
            if (impulse >= least) {
                pa.last_hit_who = j; pa.last_hit_tick = t;
                pb.last_hit_who = i; pb.last_hit_tick = t;
                if (i == ctx.you || j == ctx.you) {
                    st.my_bumps_dealt++;
                    st.my_impulse_dealt += impulse;
                }
            }
        }
    }

    for (int idx = 0; idx < num_alive; ++idx) {
        int i = alive[idx];
        SimPlayer& pa = st.players[i];
        for (int k = 0; k < st.num_bombs; ++k) {
            SimBomb& b = st.bombs[k];
            double nx = b.x - pa.x, ny = b.y - pa.y;
            if (nx * nx + ny * ny >= (sr + br) * (sr + br)) continue;
            bool hitter = (pa.vx + b.vx) * nx + (pa.vy + b.vy) * ny > 0.0;
            knockback::Body aa{pa.x, pa.y, pa.vx, pa.vy};
            knockback::Body bb{b.x, b.y, b.vx, b.vy};
            double impulse = knockback::pair(aa, bb, sr + br, 1.0, bm, ctx.restitution);
            pa.x = aa.x; pa.y = aa.y; pa.vx = aa.vx; pa.vy = aa.vy;
            b.x = bb.x; b.y = bb.y; b.vx = bb.vx; b.vy = bb.vy;
            if (impulse >= least && hitter) b.owner = i;
        }
    }

    for (int k = 0; k < st.num_bombs; ++k) {
        for (int l = k + 1; l < st.num_bombs; ++l) {
            SimBomb& ba = st.bombs[k];
            SimBomb& bb = st.bombs[l];
            double dx = bb.x - ba.x, dy = bb.y - ba.y;
            if (dx * dx + dy * dy >= (br + br) * (br + br)) continue;
            knockback::Body a_body{ba.x, ba.y, ba.vx, ba.vy};
            knockback::Body b_body{bb.x, bb.y, bb.vx, bb.vy};
            knockback::pair(a_body, b_body, br + br, bm, bm, ctx.restitution);
            ba.x = a_body.x; ba.y = a_body.y; ba.vx = a_body.vx; ba.vy = a_body.vy;
            bb.x = b_body.x; bb.y = b_body.y; bb.vx = b_body.vx; bb.vy = b_body.vy;
        }
    }

    for (int idx = 0; idx < num_alive; ++idx) {
        int i = alive[idx];
        SimPlayer& p = st.players[i];
        for (int w_idx = 0; w_idx < st.num_standing; ++w_idx) {
            sim_bounce(ctx, p.x, p.y, p.vx, p.vy, sr, ctx.walls[st.standing[w_idx]]);
        }
    }
    for (int k = 0; k < st.num_bombs; ++k) {
        SimBomb& b = st.bombs[k];
        for (int w_idx = 0; w_idx < st.num_standing; ++w_idx) {
            sim_bounce(ctx, b.x, b.y, b.vx, b.vy, br, ctx.walls[st.standing[w_idx]]);
        }
    }

    double slow2 = ctx.min_speed * ctx.min_speed;
    double p_keep = 1.0 - ctx.friction;
    for (int idx = 0; idx < num_alive; ++idx) {
        SimPlayer& p = st.players[alive[idx]];
        double old_vx = p.vx, old_vy = p.vy;
        p.vx *= p_keep;
        p.vy *= p_keep;
        if (p.vx * p.vx + p.vy * p.vy < slow2) {
            p.vx = 0.0;
            p.vy = 0.0;
        }
        p.x = fround4(p.x);
        p.y = fround4(p.y);
        p.vx = fround4(p.vx);
        p.vy = fround4(p.vy);
        p.bar = fround4(p.bar);
        p.charge = fround4(p.charge);
        if (p.vx == old_vx) p.vx = 0.0;
        if (p.vy == old_vy) p.vy = 0.0;
    }

    double b_keep = 1.0 - ctx.bomb_friction;
    for (int k = 0; k < st.num_bombs; ++k) {
        SimBomb& b = st.bombs[k];
        double old_vx = b.vx, old_vy = b.vy;
        b.vx *= b_keep;
        b.vy *= b_keep;
        if (b.vx * b.vx + b.vy * b.vy < slow2) {
            b.vx = 0.0;
            b.vy = 0.0;
        }
        b.x = fround4(b.x);
        b.y = fround4(b.y);
        b.vx = fround4(b.vx);
        b.vy = fround4(b.vy);
        if (b.vx == old_vx) b.vx = 0.0;
        if (b.vy == old_vy) b.vy = 0.0;
    }

    for (int idx = 0; idx < num_alive; ++idx) {
        int s = alive[idx];
        SimPlayer& p = st.players[s];
        if (std::max(std::abs(p.x), std::abs(p.y)) > st.half) {
            p.out = true;
            p.vx = 0.0;
            p.vy = 0.0;
            p.act = ACT_NONE;
            p.act_ticks = 0;
            p.out_tick = t;
            if (p.last_hit_who >= 0 && p.last_hit_who != s && (t - p.last_hit_tick) <= ctx.kill_credit_ticks) {
                st.players[p.last_hit_who].kills += 1;
            }
        }
    }

    for (int k = 0; k < st.num_bombs;) {
        if (std::max(std::abs(st.bombs[k].x), std::abs(st.bombs[k].y)) > st.half) {
            for (int j = k; j + 1 < st.num_bombs; ++j) st.bombs[j] = st.bombs[j + 1];
            st.num_bombs -= 1;
        } else {
            ++k;
        }
    }
}

void sim_turn(const SimContext& ctx, SimState& st, const std::array<SimOrder, MAX_PLAYERS>& orders) {
    for (int k = 0; k < ctx.D; ++k) {
        int alive_cnt = 0;
        for (int i = 0; i < ctx.n; ++i) if (!st.players[i].out) ++alive_cnt;
        if (alive_cnt <= 1) break;
        sim_tick(ctx, st, orders, k == 0);
    }
}

std::pair<double, double> compute_braking_pos(const SimContext& ctx, const SimPlayer& p, int ticks = 15) {
    double x = p.x, y = p.y, vx = p.vx, vy = p.vy;
    int act_t = (p.act != ACT_NONE) ? p.act_ticks : 0;
    double keep = 1.0 - ctx.friction;
    double max_ax = 0.0, max_ay = 0.0;
    for (int k = 0; k < ticks; ++k) {
        if (act_t > 0) {
            act_t--;
        } else {
            double bx = -x - vx * 12.0;
            double by = -y - vy * 12.0;
            auto u = knockback::unit(bx, by);
            if (u) {
                vx += u->first * ctx.move_accel;
                vy += u->second * ctx.move_accel;
            }
        }
        x += vx;
        y += vy;
        vx *= keep;
        vy *= keep;
        if (k >= 1) {
            max_ax = std::max(max_ax, std::abs(x));
            max_ay = std::max(max_ay, std::abs(y));
        }
    }
    return {max_ax, max_ay};
}

SimOrder predict_opponent_order(const SimContext& ctx, const SimState& st, int seat, bool aggressive) {
    SimOrder o{};
    const SimPlayer& p = st.players[seat];
    if (p.out) return o;

    double eff_half = st.half;
    if (ctx.next_turn - ctx.turn <= 8) eff_half = std::min(eff_half, ctx.next_half + 0.5);

    int target = -1;
    double best_d2 = 1e18;
    for (int i = 0; i < ctx.n; ++i) {
        if (i == seat || st.players[i].out) continue;
        double dx = st.players[i].x - p.x;
        double dy = st.players[i].y - p.y;
        double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            target = i;
        }
    }

    auto maybe_hammer_swing = [&]() -> bool {
        if (p.skill == SKILL_HAMMER && p.charge > 0.0 && p.act == ACT_NONE) {
            double r2 = ctx.hammer_reach * ctx.hammer_reach - 1e-6;
            if (!st.players[ctx.you].out) {
                double my_dx = st.players[ctx.you].x - p.x;
                double my_dy = st.players[ctx.you].y - p.y;
                if (my_dx * my_dx + my_dy * my_dy <= r2 &&
                    (aggressive || p.charge >= 0.44 || p.held >= 30)) {
                    o.set_hammer(my_dx, my_dy);
                    return true;
                }
            }
            if (target >= 0) {
                double dx = st.players[target].x - p.x;
                double dy = st.players[target].y - p.y;
                if (best_d2 <= r2 && (aggressive || p.charge >= 0.48 || p.held >= 32)) {
                    o.set_hammer(dx, dy);
                    return true;
                }
            }
        }
        return false;
    };

    double proj_x = p.x + p.vx * 10.0;
    double proj_y = p.y + p.vy * 10.0;
    if (std::max(std::abs(proj_x), std::abs(proj_y)) > eff_half - 1.8) {
        o.set_move(-p.x - p.vx * 8.0, -p.y - p.vy * 8.0);
        if (maybe_hammer_swing()) return o;
        if (p.skill == SKILL_DASH && p.charge >= 0.12 &&
            std::max(std::abs(proj_x), std::abs(proj_y)) > eff_half - 0.8) {
            o.set_dash(-p.x - p.vx * 6.0, -p.y - p.vy * 6.0);
            return o;
        }
        if (p.skill == SKILL_NONE && p.bar >= 0.12) o.charge = SKILL_DASH;
        return o;
    }

    for (int k = 0; k < st.num_bombs; ++k) {
        const SimBomb& b = st.bombs[k];
        if (b.fuse <= 24) {
            double bx = b.x + b.vx * std::min(b.fuse, 10);
            double by = b.y + b.vy * std::min(b.fuse, 10);
            double dx = p.x - bx, dy = p.y - by;
            if (dx * dx + dy * dy < 16.0) {
                o.set_move(dx - p.x * 0.15, dy - p.y * 0.15);
                if (maybe_hammer_swing()) return o;
                if (p.skill != SKILL_NONE && p.charge < 1.0 && p.bar > 0) o.charge = p.skill;
                return o;
            }
        }
    }

    if (target < 0) {
        o.set_move(-p.x, -p.y);
        return o;
    }

    const SimPlayer& q = st.players[target];
    double dx = q.x - p.x, dy = q.y - p.y;
    double dist = std::sqrt(best_d2);
    double pred7_dx = (q.x + q.vx * 7.0) - (p.x + p.vx * 7.0);
    double pred7_dy = (q.y + q.vy * 7.0) - (p.y + p.vy * 7.0);
    double pred7_dist = std::hypot(pred7_dx, pred7_dy);

    double mx = dx, my = dy;
    if (std::max(std::abs(p.x), std::abs(p.y)) > eff_half - 3.0) {
        mx = -p.x;
        my = -p.y;
    }
    o.set_move(mx, my);

    if (p.skill == SKILL_HAMMER && p.charge > 0.0) {
        if (maybe_hammer_swing()) return o;
        if (p.charge < 1.0 && p.bar > 0.0) o.charge = SKILL_HAMMER;
    } else if (p.skill == SKILL_DASH && p.charge > 0.0) {
        double pred_dx = (q.x + q.vx * 3.0) - (p.x + p.vx * 3.0);
        double pred_dy = (q.y + q.vy * 3.0) - (p.y + p.vy * 3.0);
        if (aggressive) {
            if ((p.charge >= 0.44 && dist < 8.5) || p.held >= 38) {
                o.set_dash(pred_dx, pred_dy);
                return o;
            }
        } else {
            if ((p.charge >= 0.96 && dist < 9.0) || p.held >= 38) {
                o.set_dash(pred_dx, pred_dy);
                return o;
            }
        }
        if (p.charge < 1.0 && p.bar > 0.0) o.charge = SKILL_DASH;
    } else {
        if (aggressive && p.attack_cd == 0 && p.act == ACT_NONE &&
            (dist < ctx.attack_reach || pred7_dist < ctx.attack_reach)) {
            o.set_attack(pred7_dx, pred7_dy);
        }
        if (p.bar >= 0.24) {
            if (p.hammer_cd == 0 && dist < 7.5) o.charge = SKILL_HAMMER;
            else if (dist < 11.5) o.charge = SKILL_DASH;
        }
    }
    return o;
}

SimOrder rollout_my_order(const SimContext& ctx, const SimState& st) {
    SimOrder o{};
    const SimPlayer& me = st.players[ctx.you];
    if (me.out) return o;

    double eff_half = st.half;
    if (ctx.next_turn - ctx.turn <= 8) eff_half = std::min(eff_half, ctx.next_half + 0.5);

    auto stop = compute_braking_pos(ctx, me, 12);
    double stop_max = std::max(std::abs(stop.first), std::abs(stop.second));
    double curr_max = std::max(std::abs(me.x), std::abs(me.y));

    if (me.skill == SKILL_HAMMER && (me.charge >= 0.44 || me.held >= 30 || stop_max > eff_half - 2.2) &&
        me.act == ACT_NONE && stop_max <= st.half - 0.8) {
        for (int i = 0; i < ctx.n; ++i) {
            if (i == ctx.you || st.players[i].out) continue;
            double dx = st.players[i].x - me.x;
            double dy = st.players[i].y - me.y;
            if (dx * dx + dy * dy <= ctx.hammer_reach * ctx.hammer_reach) {
                o.set_hammer(dx, dy);
                o.set_move(-me.x - me.vx * 8.0, -me.y - me.vy * 8.0);
                return o;
            }
        }
    }

    if (stop_max > eff_half - 2.2 || curr_max > eff_half - 2.2) {
        double ix = -me.x - me.vx * 10.0;
        double iy = -me.y - me.vy * 10.0;
        o.set_move(ix, iy);
        if (me.skill == SKILL_DASH && me.charge > 0.0 && stop_max > eff_half - 1.2) {
            o.set_dash(ix, iy);
            return o;
        }
        if (me.skill == SKILL_HAMMER && me.charge > 0.0 && stop_max > eff_half - 1.0) {
            if (me.hammer_cd == 0) o.charge = SKILL_DASH;
            return o;
        }
        if (me.skill != SKILL_NONE && me.charge < 0.88 && me.bar > 0.0) {
            o.charge = me.skill;
        }
        return o;
    }

    double move_x = -me.x * 0.25, move_y = -me.y * 0.25;
    for (int k = 0; k < st.num_bombs; ++k) {
        const SimBomb& b = st.bombs[k];
        if (b.fuse <= 24) {
            double bx = b.x + b.vx * std::min(b.fuse, 10);
            double by = b.y + b.vy * std::min(b.fuse, 10);
            double dx = me.x - bx, dy = me.y - by;
            double d2 = dx * dx + dy * dy;
            if (d2 < 18.0) {
                double d = std::sqrt(std::max(0.01, d2));
                move_x += (dx / d) * 3.0;
                move_y += (dy / d) * 3.0;
            }
        }
    }

    o.set_move(move_x, move_y);
    if (me.skill != SKILL_NONE && me.charge < 1.0 && me.bar > 0.0 && (me.charge < 0.88 || me.held >= 28)) {
        o.charge = me.skill;
    }
    return o;
}

double evaluate_state(const SimContext& ctx, const SimState& init_st, const SimState& st, const SimOrder& first_order) {
    const SimPlayer& me = st.players[ctx.you];
    const SimPlayer& init_me = init_st.players[ctx.you];

    if (me.out) {
        int my_new_kills = me.kills - init_me.kills;
        return -1000000.0 + (me.out_tick - ctx.base_tick) * 200.0 + my_new_kills * 500.0;
    }

    double score = 0.0;

    double eff_half = st.half;
    if (ctx.next_turn - ctx.turn <= 12) {
        double weight = std::max(0.0, std::min(1.0, (13.0 - (ctx.next_turn - ctx.turn)) / 10.0));
        eff_half = st.half * (1.0 - weight) + ctx.next_half * weight;
    }

    auto my_stop = compute_braking_pos(ctx, me, 16);
    double stop_abs_x = std::abs(my_stop.first);
    double stop_abs_y = std::abs(my_stop.second);
    double stop_max = std::max(stop_abs_x, stop_abs_y);
    double curr_max = std::max(std::abs(me.x), std::abs(me.y));

    double margin = eff_half - stop_max;
    double curr_margin = st.half - curr_max;

    if (margin < 0.0) {
        score -= 250000.0 - margin * 40000.0;
    } else if (margin < 4.2) {
        double pen = 4.2 - margin;
        score -= 180.0 * pen * pen * pen;
    }
    if (curr_margin < 3.0) {
        double pen = 3.0 - curr_margin;
        score -= 140.0 * pen * pen;
    }

    double norm_x = stop_abs_x / std::max(1.2, eff_half);
    double norm_y = stop_abs_y / std::max(1.2, eff_half);
    score -= 52.0 * (norm_x * norm_x + norm_y * norm_y);
    score -= 80.0 * (norm_x * norm_x * norm_y * norm_y);

    int my_new_kills = me.kills - init_me.kills;
    score += my_new_kills * 2800.0;

    int alive_opps = 0;
    int best_target = -1;
    double best_target_priority = -1e18;

    for (int i = 0; i < ctx.n; ++i) {
        if (i == ctx.you) continue;
        const SimPlayer& p = st.players[i];
        const SimPlayer& ip = init_st.players[i];
        if (p.out) {
            if (!ip.out) {
                score += 1100.0;
            }
            continue;
        }
        alive_opps++;

        auto opp_stop = compute_braking_pos(ctx, p, 14);
        double opp_max = std::max(std::abs(opp_stop.first), std::abs(opp_stop.second));
        double opp_margin = eff_half - opp_max;
        bool credited_to_me = (p.last_hit_who == ctx.you && (st.tick - p.last_hit_tick) <= ctx.kill_credit_ticks);

        if (opp_margin < 0.0) {
            score += credited_to_me ? (2100.0 - opp_margin * 200.0) : 750.0;
        } else {
            double opp_danger = std::max(0.0, (eff_half - opp_margin) / std::max(2.0, eff_half));
            double danger_sq = opp_danger * opp_danger;
            if (credited_to_me) {
                score += 55.0 + 240.0 * danger_sq + 140.0 * danger_sq * opp_danger;
            } else {
                score += 35.0 * danger_sq;
            }
        }

        double dx = p.x - me.x, dy = p.y - me.y;
        double dist = std::sqrt(dx * dx + dy * dy);
        if (p.skill == SKILL_HAMMER && p.charge >= 0.3 && p.act == ACT_NONE) {
            if (dist < ctx.hammer_reach + 0.45) {
                score -= 220.0 * p.charge * (ctx.hammer_reach + 0.55 - dist);
            }
        }

        if (opp_margin < -0.25 && credited_to_me) continue;

        double prio = -dist * 2.5 + opp_max * 1.4 + (credited_to_me ? 8.0 : 0.0);
        if (p.act != ACT_NONE) prio += 5.0;
        if (p.skill == SKILL_HAMMER && p.charge >= 0.5) prio -= 4.0;
        if (prio > best_target_priority) {
            best_target_priority = prio;
            best_target = i;
        }
    }

    if (best_target >= 0) {
        const SimPlayer& q = st.players[best_target];
        double q_dist_center = std::sqrt(q.x * q.x + q.y * q.y);
        double ux = (q_dist_center > 0.5) ? (q.x / q_dist_center) : 0.0;
        double uy = (q_dist_center > 0.5) ? (q.y / q_dist_center) : 0.0;

        double ideal_dist = 2.8;
        if (q.skill == SKILL_HAMMER && q.charge >= 0.35 && q.act == ACT_NONE &&
            !(me.skill == SKILL_HAMMER && me.charge >= q.charge)) {
            ideal_dist = 3.9;
        } else if (me.skill == SKILL_HAMMER && me.charge >= 0.36) {
            ideal_dist = 2.2;
        } else if (me.skill == SKILL_DASH && me.charge >= 0.44) {
            ideal_dist = 3.5;
        } else if (me.pause > 10 && me.charge == 0.0) {
            ideal_dist = 4.6;
        }

        double stalk_x = q.x - ux * std::min(ideal_dist, q_dist_center);
        double stalk_y = q.y - uy * std::min(ideal_dist, q_dist_center);
        double max_stalk = std::max(0.4, std::min(eff_half - 3.6, eff_half * (alive_opps >= 3 ? 0.50 : 0.62)));
        stalk_x = std::clamp(stalk_x, -max_stalk, max_stalk);
        stalk_y = std::clamp(stalk_y, -max_stalk, max_stalk);

        double d_stalk = std::hypot(me.x - stalk_x, me.y - stalk_y);
        score -= d_stalk * 7.5;

        double my_r = std::hypot(me.x, me.y);
        if (my_r < q_dist_center + 0.5) {
            score += 18.0;
        }
    }

    for (int k = 0; k < st.num_bombs; ++k) {
        const SimBomb& b = st.bombs[k];
        double travel = (1.0 - std::pow(1.0 - ctx.bomb_friction, b.fuse)) / ctx.bomb_friction;
        double bx = b.x + b.vx * travel;
        double by = b.y + b.vy * travel;
        if (std::max(std::abs(bx), std::abs(by)) > st.half) continue;

        double my_d = std::hypot(me.x - bx, me.y - by);
        double safe_r = (b.owner == ctx.you && b.fuse > 18) ? (ctx.bomb_blast_radius + 0.5) : (ctx.bomb_blast_radius + 1.4);
        if (my_d < safe_r) {
            double urgency = 40.0 / (b.fuse + 6.0);
            double pen = (safe_r - my_d);
            score -= 95.0 * urgency * pen * pen;
        }

        if (b.owner == ctx.you) {
            for (int i = 0; i < ctx.n; ++i) {
                if (i == ctx.you || st.players[i].out) continue;
                double ox = st.players[i].x + st.players[i].vx * std::min(b.fuse, 12);
                double oy = st.players[i].y + st.players[i].vy * std::min(b.fuse, 12);
                double od = std::hypot(ox - bx, oy - by);
                if (od < ctx.bomb_blast_radius + 0.5) {
                    score += 75.0 * (1.0 - od / (ctx.bomb_blast_radius + 0.5));
                }
            }
        }
    }

    for (int k = 0; k < st.num_broken; ++k) {
        int ticks_left = st.broken[k].at - st.tick;
        if (ticks_left <= ctx.wall_warn_ticks) {
            const WallDef& w = ctx.walls[st.broken[k].wall];
            auto q = nearest_on_wall(me.x, me.y, w);
            double d = std::hypot(me.x - q.first, me.y - q.second);
            if (d < 2.4) {
                score -= 140.0 * (2.4 - d) * (1.0 + 20.0 / std::max(1, ticks_left));
            }
        }
    }

    if (first_order.has_hammer) {
        score += st.my_hammer_hits * 70.0 + st.my_hammer_hit_power * 210.0;
    } else {
        score += st.my_hammer_hits * 35.0 + st.my_hammer_hit_power * 115.0;
    }
    score += st.my_hammer_bomb_hits * 85.0;
    score += st.my_jab_hits * 85.0;
    score += st.my_jab_bomb_hits * 75.0;
    score += st.my_bomb_hits * 165.0;
    score += st.my_impulse_dealt * 95.0;

    if (st.hit_by_enemy_hammer) score -= 260.0;
    if (st.hit_by_enemy_jab) score -= 110.0;
    if (st.hit_by_enemy_bomb) score -= 220.0;
    if (me.overcharged) score -= 90.0;

    bool init_danger = (st.half - std::max(std::abs(init_me.x + init_me.vx * 10.0), std::abs(init_me.y + init_me.vy * 10.0))) < 2.6;
    if (first_order.has_hammer && init_me.skill == SKILL_HAMMER && init_me.charge > 0.0) {
        if (st.my_hammer_hits == 0 && st.my_hammer_bomb_hits == 0) {
            score -= 400.0;
        } else if (st.my_hammer_hits == 0 && st.my_hammer_bomb_hits > 0 && st.my_bomb_hits == 0) {
            score -= 80.0;
        }
    }
    if (first_order.has_dash && init_me.skill == SKILL_DASH && init_me.charge > 0.0) {
        if (st.my_bumps_dealt == 0 && !init_danger && init_me.held < 38) {
            score -= 115.0 * init_me.charge;
        }
    }
    if (first_order.has_attack && st.my_jab_hits == 0 && st.my_jab_bomb_hits == 0) {
        score -= 180.0;
    }

    score += me.bar * 48.0;
    if (me.skill == SKILL_HAMMER) {
        score += me.charge * 96.0;
    } else if (me.skill == SKILL_DASH) {
        score += me.charge * 78.0;
    }
    if (me.held > 32) {
        score -= (me.held - 32) * 4.0;
    }

    return score;
}

struct BombCand {
    double dx;
    double dy;
    int fuse;
    double val;
};

std::vector<BombCand> find_best_bombs(const SimContext& ctx, const SimState& st, double post_vx, double post_vy) {
    std::vector<BombCand> res;
    const SimPlayer& me = st.players[ctx.you];
    if (me.bomb_cd > 0 || me.act != ACT_NONE) return res;

    double gap = ctx.player_radius + ctx.bomb_radius + ctx.bomb_spawn_gap;

    for (int i = 0; i < ctx.n; ++i) {
        if (i == ctx.you || st.players[i].out) continue;
        const SimPlayer& q = st.players[i];
        double raw_dist = std::hypot(q.x - me.x, q.y - me.y);
        double min_safe_dist = (q.skill == SKILL_HAMMER && q.charge >= 0.35) ? 3.8 : 2.8;
        if (raw_dist < min_safe_dist || raw_dist > 13.5) continue;

        for (int fuse : {24, 36, 48}) {
            double travel_free = (1.0 - std::pow(1.0 - ctx.bomb_friction, fuse)) / ctx.bomb_friction;
            double pred_t = std::min(double(fuse), std::max(6.0, raw_dist * 1.4));
            double tx = std::clamp(q.x + q.vx * pred_t, -st.half + 1.5, st.half - 1.5);
            double ty = std::clamp(q.y + q.vy * pred_t, -st.half + 1.5, st.half - 1.5);

            double target_dist = std::hypot(tx - me.x, ty - me.y);
            double eff_travel = std::min(travel_free, std::max(2.0, (target_dist - gap) / ctx.bomb_throw_speed));

            double rhs_x = tx - me.x - post_vx * eff_travel;
            double rhs_y = ty - me.y - post_vy * eff_travel;
            auto u_opt = knockback::unit(rhs_x, rhs_y);
            if (!u_opt) continue;
            double ux = u_opt->first, uy = u_opt->second;

            double bx = me.x + ux * gap + (post_vx + ux * ctx.bomb_throw_speed) * eff_travel;
            double by = me.y + uy * gap + (post_vy + uy * ctx.bomb_throw_speed) * eff_travel;

            if (std::max(std::abs(bx), std::abs(by)) > st.half - 1.0) continue;

            double bx_free = me.x + ux * gap + (post_vx + ux * ctx.bomb_throw_speed) * travel_free;
            double by_free = me.y + uy * gap + (post_vy + uy * ctx.bomb_throw_speed) * travel_free;
            double my_pred_x = me.x + post_vx * 8.0;
            double my_pred_y = me.y + post_vy * 8.0;
            double self_d_free = std::hypot(bx_free - my_pred_x, by_free - my_pred_y);
            if (self_d_free < ctx.bomb_blast_radius + 0.6) continue;
            if (fuse > 24 && std::hypot(bx - my_pred_x, by - my_pred_y) < ctx.bomb_blast_radius + 0.6) continue;

            double hit_d = std::hypot(bx - tx, by - ty);
            if (hit_d > ctx.bomb_blast_radius - 0.2) continue;

            double val = (ctx.bomb_blast_radius - hit_d) * 20.0 + (30.0 / fuse) * 12.0;
            if (fuse == 24 && raw_dist <= 6.8) val += 14.0;
            if (q.skill == SKILL_HAMMER) val += 15.0;
            if (std::max(std::abs(q.x), std::abs(q.y)) > st.half - 5.0) val += 15.0;

            if (val > 14.0) {
                res.push_back({ux, uy, fuse, val});
            }
        }
    }
    std::sort(res.begin(), res.end(), [](const BombCand& a, const BombCand& b) { return a.val > b.val; });
    if (res.size() > 2) res.resize(2);
    return res;
}

J decide(const J& obs, J& memory) {
    auto start_time = std::chrono::steady_clock::now();

    SimContext ctx;
    SimState init_st;
    parse_observation(obs, ctx, init_st);

    const SimPlayer& me = init_st.players[ctx.you];
    if (me.out) return J::object{};

    std::array<SimOrder, MAX_PLAYERS> opp_orders_A{};
    std::array<SimOrder, MAX_PLAYERS> opp_orders_B{};
    for (int i = 0; i < ctx.n; ++i) {
        if (i == ctx.you || init_st.players[i].out) continue;
        opp_orders_A[i] = predict_opponent_order(ctx, init_st, i, true);
        opp_orders_B[i] = predict_opponent_order(ctx, init_st, i, false);
    }

    std::vector<std::pair<double, double>> move_dirs;
    move_dirs.reserve(24);
    move_dirs.push_back({0.0, 0.0});
    constexpr int NUM_ANGLES = 16;
    for (int k = 0; k < NUM_ANGLES; ++k) {
        double ang = k * (2.0 * M_PI / NUM_ANGLES);
        move_dirs.push_back({std::cos(ang), std::sin(ang)});
    }
    if (std::hypot(me.x, me.y) > 0.1) move_dirs.push_back({-me.x, -me.y});
    if (std::hypot(me.vx, me.vy) > 0.01) move_dirs.push_back({-me.vx, -me.vy});
    for (int i = 0; i < ctx.n; ++i) {
        if (i == ctx.you || init_st.players[i].out) continue;
        double dx = init_st.players[i].x - me.x;
        double dy = init_st.players[i].y - me.y;
        if (std::hypot(dx, dy) > 0.05) {
            move_dirs.push_back({dx, dy});
            move_dirs.push_back({-dx, -dy});
        }
    }

    bool my_edge_danger = std::max(std::abs(me.x + me.vx * 8.0), std::abs(me.y + me.vy * 8.0)) > init_st.half - 2.2;
    std::vector<int> charge_opts;
    if (me.skill == SKILL_NONE) {
        charge_opts.push_back(SKILL_NONE);
        if (me.bar >= 0.36 && me.hammer_cd == 0) {
            charge_opts.push_back(SKILL_HAMMER);
        }
        if ((my_edge_danger && me.bar >= 0.16) || (me.hammer_cd >= 12 && me.bar >= 0.44)) {
            charge_opts.push_back(SKILL_DASH);
        }
    } else {
        if (me.charge < 1.0 && me.bar > 0.0) {
            charge_opts.push_back(me.skill);
        }
        if ((me.charge >= 0.80 || me.bar == 0.0) && me.held < 42) {
            charge_opts.push_back(SKILL_NONE);
        }
        if (my_edge_danger && me.skill == SKILL_HAMMER && me.bar >= 0.24) {
            charge_opts.push_back(SKILL_DASH);
        }
    }
    if (charge_opts.empty()) {
        charge_opts.push_back(SKILL_NONE);
    }

    std::vector<SimOrder> candidates;
    candidates.reserve(140);

    for (int ch : charge_opts) {
        for (const auto& md : move_dirs) {
            SimOrder o{};
            if (md.first != 0.0 || md.second != 0.0) o.set_move(md.first, md.second);
            o.charge = ch;
            candidates.push_back(o);
        }
    }

    if (me.skill == SKILL_HAMMER && me.charge > 0.0 && me.act == ACT_NONE) {
        std::vector<std::pair<double, double>> hammer_aims;
        for (int i = 0; i < ctx.n; ++i) {
            if (i == ctx.you || init_st.players[i].out) continue;
            double dx = init_st.players[i].x - me.x;
            double dy = init_st.players[i].y - me.y;
            if (dx * dx + dy * dy <= ctx.hammer_reach * ctx.hammer_reach + 1e-6) {
                hammer_aims.push_back({dx, dy});
            }
        }
        for (int k = 0; k < init_st.num_bombs; ++k) {
            const SimBomb& b = init_st.bombs[k];
            double dx = b.x - me.x;
            double dy = b.y - me.y;
            if (dx * dx + dy * dy <= (ctx.hammer_reach + 0.15) * (ctx.hammer_reach + 0.15)) {
                hammer_aims.push_back({dx, dy});
            }
        }
        size_t n_base = hammer_aims.size();
        for (size_t i = 0; i < n_base; ++i) {
            for (size_t j = i + 1; j < n_base; ++j) {
                auto u1 = knockback::unit(hammer_aims[i].first, hammer_aims[i].second);
                auto u2 = knockback::unit(hammer_aims[j].first, hammer_aims[j].second);
                if (u1 && u2) {
                    hammer_aims.push_back({u1->first + u2->first, u1->second + u2->second});
                }
            }
        }
        for (const auto& aim : hammer_aims) {
            SimOrder o{};
            o.set_hammer(aim.first, aim.second);
            o.set_move(-me.x, -me.y);
            candidates.push_back(o);
        }
    }

    if (me.skill == SKILL_DASH && me.charge > 0.0 && me.act == ACT_NONE) {
        std::vector<std::pair<double, double>> dash_dirs;
        bool edge_danger = std::max(std::abs(me.x + me.vx * 10.0), std::abs(me.y + me.vy * 10.0)) > init_st.half - 2.2;
        if (edge_danger || me.charge >= 0.40 || me.held >= 38) {
            dash_dirs.push_back({-me.x - me.vx * 6.0, -me.y - me.vy * 6.0});
        }
        if (me.charge >= 0.40 || me.held >= 38) {
            for (int k = 0; k < 8; ++k) {
                double ang = k * (2.0 * M_PI / 8.0);
                dash_dirs.push_back({std::cos(ang), std::sin(ang)});
            }
            for (int i = 0; i < ctx.n; ++i) {
                if (i == ctx.you || init_st.players[i].out) continue;
                const SimPlayer& q = init_st.players[i];
                for (double lead : {2.0, 4.5}) {
                    dash_dirs.push_back({(q.x + q.vx * lead) - me.x, (q.y + q.vy * lead) - me.y});
                }
            }
        }
        for (const auto& dd : dash_dirs) {
            SimOrder o{};
            o.set_dash(dd.first, dd.second);
            o.set_move(dd.first, dd.second);
            candidates.push_back(o);

            SimOrder o_brake{};
            o_brake.set_dash(dd.first, dd.second);
            o_brake.set_move(-me.x - dd.first * 2.0, -me.y - dd.second * 2.0);
            candidates.push_back(o_brake);
        }
    }

    if (me.attack_cd == 0 && me.act == ACT_NONE && me.skill == SKILL_NONE &&
        std::max(std::abs(me.x), std::abs(me.y)) <= init_st.half - 2.6) {
        bool safe_to_jab = true;
        for (int k = 0; k < init_st.num_bombs; ++k) {
            const SimBomb& b = init_st.bombs[k];
            if (b.fuse <= 16 && std::hypot(b.x - me.x, b.y - me.y) < 5.5) {
                safe_to_jab = false;
                break;
            }
        }
        for (int i = 0; i < ctx.n && safe_to_jab; ++i) {
            if (i == ctx.you || init_st.players[i].out) continue;
            const SimPlayer& q = init_st.players[i];
            if (q.skill == SKILL_HAMMER && q.charge > 0.0 && std::hypot(q.x - me.x, q.y - me.y) < 4.5) {
                safe_to_jab = false;
                break;
            }
        }
        if (safe_to_jab) {
            if (me.hammer_cd > 0 || me.bar < 0.48) {
                for (int i = 0; i < ctx.n; ++i) {
                    if (i == ctx.you || init_st.players[i].out) continue;
                    const SimPlayer& q = init_st.players[i];
                    double cur_d2 = (q.x - me.x) * (q.x - me.x) + (q.y - me.y) * (q.y - me.y);
                    double pred_dx = (q.x + q.vx * 7.0) - (me.x + me.vx * 7.0);
                    double pred_dy = (q.y + q.vy * 7.0) - (me.y + me.vy * 7.0);
                    if (cur_d2 <= 2.55 * 2.55 && pred_dx * pred_dx + pred_dy * pred_dy <= 2.60 * 2.60) {
                        SimOrder o{};
                        o.set_attack(pred_dx, pred_dy);
                        o.set_move(-me.x, -me.y);
                        candidates.push_back(o);
                    }
                }
            }
            for (int k = 0; k < init_st.num_bombs; ++k) {
                const SimBomb& b = init_st.bombs[k];
                if (b.fuse < 15) continue;
                double cur_d2 = (b.x - me.x) * (b.x - me.x) + (b.y - me.y) * (b.y - me.y);
                double pred_dx = (b.x + b.vx * 6.0) - (me.x + me.vx * 6.0);
                double pred_dy = (b.y + b.vy * 6.0) - (me.y + me.vy * 6.0);
                if (cur_d2 <= 2.85 * 2.85 && pred_dx * pred_dx + pred_dy * pred_dy <= 2.55 * 2.55) {
                    SimOrder o{};
                    o.set_attack(pred_dx, pred_dy);
                    o.set_move(-me.x, -me.y);
                    candidates.push_back(o);
                }
            }
        }
    }

    double best_score = -1e18;
    SimOrder best_order{};

    auto eval_turn1_pos = [&](const SimState& st) -> double {
        const SimPlayer& p = st.players[ctx.you];
        if (p.out) return -500000.0;
        double eff_half = st.half;
        if (ctx.next_turn - ctx.turn <= 12) {
            double weight = std::max(0.0, std::min(1.0, (13.0 - (ctx.next_turn - ctx.turn)) / 10.0));
            eff_half = st.half * (1.0 - weight) + ctx.next_half * weight;
        }
        auto my_stop = compute_braking_pos(ctx, p, 16);
        double stop_abs_x = std::abs(my_stop.first);
        double stop_abs_y = std::abs(my_stop.second);
        double stop_max = std::max(stop_abs_x, stop_abs_y);
        double curr_max = std::max(std::abs(p.x), std::abs(p.y));
        double margin = eff_half - stop_max;
        double curr_margin = st.half - curr_max;
        double s = 0.0;
        if (margin < 0.0) {
            s -= 250000.0 - margin * 40000.0;
        } else if (margin < 4.2) {
            double pen = 4.2 - margin;
            s -= 180.0 * pen * pen * pen;
        }
        if (curr_margin < 3.0) {
            double pen = 3.0 - curr_margin;
            s -= 140.0 * pen * pen;
        }
        double norm_x = stop_abs_x / std::max(1.2, eff_half);
        double norm_y = stop_abs_y / std::max(1.2, eff_half);
        s -= 52.0 * (norm_x * norm_x + norm_y * norm_y);
        s -= 80.0 * (norm_x * norm_x * norm_y * norm_y);
        s += p.bar * 48.0;
        if (p.skill == SKILL_HAMMER) s += p.charge * 96.0;
        else if (p.skill == SKILL_DASH) s += p.charge * 78.0;
        return s;
    };

    auto evaluate_candidate = [&](const SimOrder& cand) -> double {
        auto run_scenario = [&](const std::array<SimOrder, MAX_PLAYERS>& opp_turn1, bool agg_rollout) -> double {
            SimState st = init_st;
            std::array<SimOrder, MAX_PLAYERS> orders = opp_turn1;
            orders[ctx.you] = cand;
            sim_turn(ctx, st, orders);
            double s1_pos = eval_turn1_pos(st);

            for (int r_turn = 0; r_turn < 4; ++r_turn) {
                if (st.players[ctx.you].out) break;
                int alive_cnt = 0;
                for (int i = 0; i < ctx.n; ++i) if (!st.players[i].out) ++alive_cnt;
                if (alive_cnt <= 1) break;

                std::array<SimOrder, MAX_PLAYERS> r_orders{};
                for (int i = 0; i < ctx.n; ++i) {
                    if (st.players[i].out) continue;
                    if (i == ctx.you) {
                        r_orders[i] = rollout_my_order(ctx, st);
                    } else {
                        r_orders[i] = predict_opponent_order(ctx, st, i, agg_rollout || (r_turn == 0));
                    }
                }
                sim_turn(ctx, st, r_orders);
            }
            return 0.45 * s1_pos + evaluate_state(ctx, init_st, st, cand);
        };

        double sA = run_scenario(opp_orders_A, true);
        double sB = run_scenario(opp_orders_B, false);
        return 0.65 * sA + 0.35 * sB + 0.15 * std::min(sA, sB);
    };

    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        if ((idx & 7) == 0 && idx > 0) {
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - start_time).count();
            if (ms > 52.0) break;
        }
        double sc = evaluate_candidate(candidates[idx]);
        if (sc > best_score) {
            best_score = sc;
            best_order = candidates[idx];
        }
    }

    if (me.bomb_cd == 0 && me.act == ACT_NONE && !best_order.has_hammer && !best_order.has_attack) {
        double post_vx = me.vx;
        double post_vy = me.vy;
        if (best_order.has_dash && me.skill == SKILL_DASH && me.charge > 0.0) {
            post_vx += best_order.dash_x * ctx.dash_speed * me.charge;
            post_vy += best_order.dash_y * ctx.dash_speed * me.charge;
        }
        if (best_order.has_move) {
            double factor = (me.skill == SKILL_DASH && !best_order.has_dash) ? ctx.dash_charging_factor
                          : (me.skill == SKILL_HAMMER) ? ctx.hammer_charging_factor : 1.0;
            post_vx += best_order.move_x * ctx.move_accel * factor;
            post_vy += best_order.move_y * ctx.move_accel * factor;
        }
        auto bomb_cands = find_best_bombs(ctx, init_st, post_vx, post_vy);
        SimOrder best_with_bomb = best_order;
        double best_bomb_score = -1e18;
        for (const auto& bc : bomb_cands) {
            SimOrder with_bomb = best_order;
            with_bomb.set_bomb(bc.dx, bc.dy, bc.fuse);
            double sc_bomb = evaluate_candidate(with_bomb);
            if (sc_bomb > best_bomb_score) {
                best_bomb_score = sc_bomb;
                best_with_bomb = with_bomb;
            }
        }
        if (best_bomb_score >= best_score - 8.0) {
            best_order = best_with_bomb;
            best_score = best_bomb_score;
        }
    }

    return best_order.to_json();
}

} // namespace

int main() {
    return kb_step_worker::run([](const J& obs, J& memory) {
        return std::make_pair(decide(obs, memory), memory);
    });
}
