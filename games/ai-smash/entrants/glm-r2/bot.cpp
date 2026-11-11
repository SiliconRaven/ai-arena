// AI Smash submission: a survival-first brawler with forward simulation for recovery.
// Strategy: never SD; counter/blink incoming moves; punish stuck foes; finish
// recovering foes and blast them out; respect the shrinking blast zone at the end.
#include "include/step_worker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using J = mini_json::Json;

namespace smash_bot {

// ---------- rule numbers (filled once from obs.rules) ----------
struct Rules {
  double gravity = 0.035, max_fall = 0.8, run_speed = 0.32;
  double ground_accel = 0.08, air_accel = 0.03, ground_friction = 0.75, air_friction = 0.985;
  double jump_ground = 0.62, jump_air = 0.55;
  double half_w = 0.6, height = 1.8;
  double blast_x = 26, blast_top = 24, blast_bottom = -16;
  double shrink_start = 3000, shrink_rate = 0.02, shrink_top_ratio = 0.9, shrink_min_x = 0, shrink_min_top = 1;
  double kill_credit = 300;
  double cost_wave = 0.6, cost_hook = 0.4, cost_slam = 0.2, cost_blink = 0.2, cost_grab = 0.3, cost_counter = 0.3, cost_uppercut = 0.25;
  int air_jumps = 1;
  double solid_x1 = -13, solid_x2 = 13, solid_top = 0;
  std::vector<std::array<double, 4>> plats;  // x1,x2,y (4th unused)
};
static Rules R;

static void load_rules(const J &rules) {
  auto phys = [&](const char *k) { return rules.at("physics").at(k).as_double(); };
  R.gravity = phys("gravity"); R.max_fall = phys("max_fall"); R.run_speed = phys("run_speed");
  R.ground_accel = phys("ground_accel"); R.air_accel = phys("air_accel");
  R.ground_friction = phys("ground_friction"); R.air_friction = phys("air_friction");
  const J &jp = rules.at("jump");
  R.jump_ground = jp.at("ground_speed").as_double(); R.jump_air = jp.at("air_speed").as_double();
  R.air_jumps = jp.at("air_jumps").get<int>();
  const J &body = rules.at("body");
  R.half_w = body.at("half_width").as_double(); R.height = body.at("height").as_double();
  const J &st = rules.at("stage");
  const J &solid = st.at("solids").at(0);
  R.solid_x1 = solid.at("x1").as_double(); R.solid_x2 = solid.at("x2").as_double(); R.solid_top = solid.at("top").as_double();
  const J &bl = st.at("blast");
  R.blast_x = bl.at("x").as_double(); R.blast_top = bl.at("top").as_double(); R.blast_bottom = bl.at("bottom").as_double();
  const J &bs = st.at("blast_shrink");
  R.shrink_start = bs.at("start_tick").as_double(); R.shrink_rate = bs.at("rate").as_double();
  R.shrink_top_ratio = bs.at("top_ratio").as_double(); R.shrink_min_x = bs.at("min_x").as_double();
  R.shrink_min_top = bs.at("min_top").as_double();
  R.kill_credit = rules.value("kill_credit_ticks", 300.0);
  const J &mv = rules.at("moves");
  R.cost_wave = mv.at("wave").at("cost").as_double(); R.cost_hook = mv.at("hook").at("cost").as_double();
  R.cost_slam = mv.at("slam").at("cost").as_double(); R.cost_blink = mv.at("blink").at("cost").as_double();
  R.cost_grab = mv.at("grab").at("cost").as_double(); R.cost_counter = mv.at("counter").at("cost").as_double();
  R.cost_uppercut = mv.at("uppercut").at("cost").as_double();
  R.plats.clear();
  for (const auto &p : st.at("platforms").as_array())
    R.plats.push_back({p.at("x1").as_double(), p.at("x2").as_double(), p.at("y").as_double(), 0.0});
}

// ---------- player snapshot ----------
struct P {
  int i = -1;
  double x = 0, y = 0, vx = 0, vy = 0, damage = 0, meter = 0;
  int facing = 1, air_jumps = 0, stocks = 0, respawn = 0, invuln = 0, hitstun = 0, gap = 0, hook_cd = 0, drop = 0;
  int phase_t = 0, charge = 0;
  bool grounded = false, out = false;
  std::string act, phase;
};

static P read(const J &j, int i) {
  P p;
  p.i = i;
  p.x = j.at("x").as_double(); p.y = j.at("y").as_double();
  p.vx = j.at("vx").as_double(); p.vy = j.at("vy").as_double();
  p.damage = j.at("damage").as_double(); p.meter = j.at("meter").as_double();
  p.facing = j.at("facing").get<int>(); p.air_jumps = j.at("air_jumps").get<int>();
  p.stocks = j.at("stocks").get<int>(); p.respawn = j.at("respawn").get<int>();
  p.invuln = j.at("invuln").get<int>(); p.hitstun = j.at("hitstun").get<int>();
  p.gap = j.at("gap").get<int>(); p.hook_cd = j.at("hook_cd").get<int>();
  p.drop = j.value("drop", 0);
  p.phase_t = j.at("phase_ticks").get<int>(); p.charge = j.at("charge").get<int>();
  p.grounded = j.at("grounded").as_bool(); p.out = j.at("out").as_bool();
  p.act = j.contains("act") && j.at("act").is_string() ? j.at("act").as_string() : "";
  p.phase = j.at("phase").as_string();
  return p;
}

static bool guarding(const P &p) { return p.act == "guard" && p.phase == "active"; }
static bool countering(const P &p) { return p.act == "counter" && p.phase == "active"; }
static bool blinking(const P &p) { return p.act == "blink" && p.phase == "active"; }
static bool melee_active(const P &p) {
  return p.phase == "active" && (p.act == "attack" || p.act == "charge" || p.act == "uppercut" || p.act == "slam" || p.act == "grab");
}

// ---------- geometry / physics helpers ----------
static double blast_x_at(long long tick) {
  double s = tick > R.shrink_start ? (tick - R.shrink_start) * R.shrink_rate : 0;
  return std::max(R.shrink_min_x, R.blast_x - s);
}
static double blast_top_at(long long tick) {
  double s = tick > R.shrink_start ? (tick - R.shrink_start) * R.shrink_rate : 0;
  return std::max(R.shrink_min_top, R.blast_top - s * R.shrink_top_ratio);
}

static bool over_stage(const P &p) { return p.x > R.solid_x1 - 0.3 && p.x < R.solid_x2 + 0.3; }
static bool off_stage(const P &p) { return !over_stage(p) && p.y < R.solid_top + 1.0; }

struct Kin { double x = 0, y = 0, vx = 0, vy = 0; bool grounded = false; };

// one physics tick of free motion (no actions), main solid only
static void kin_tick(Kin &k) {
  k.vy = std::max(k.vy - R.gravity, -R.max_fall);
  if (!k.grounded) k.vx *= R.air_friction;
  k.x += k.vx;
  if (k.x + R.half_w > R.solid_x1 && k.x - R.half_w < R.solid_x2 && k.y < R.solid_top && k.y + R.height > R.solid_top - 4) {
    k.x = k.vx > 0 ? R.solid_x1 - R.half_w : R.solid_x2 + R.half_w;
    k.vx = 0;
  }
  double foot = k.y;
  k.y += k.vy;
  k.grounded = false;
  if (k.vy <= 0 && k.x + R.half_w > R.solid_x1 && k.x - R.half_w < R.solid_x2 && foot >= R.solid_top && k.y <= R.solid_top) {
    k.y = R.solid_top; k.vy = 0; k.grounded = true;
  }
}

struct SimResult { bool safe = false; int land_tick = -1; };

// simulate free fall plus the standard recovery policy (air jump when falling and low/near death)
static SimResult simulate_recovery(const P &p, long long tick, bool use_jump, double meter, bool use_uc) {
  Kin k{p.x, p.y, p.vx, p.vy, p.grounded};
  SimResult r;
  int jumps = p.air_jumps;
  double m = meter;
  const double bx = blast_x_at(tick), btop = blast_top_at(tick);
  for (int t = 0; t < 400; ++t) {
    if (std::fabs(k.x) > bx || k.y < R.blast_bottom || k.y > btop) return r;  // dead
    if (k.grounded) { r.safe = true; r.land_tick = t; return r; }
    if (use_jump && jumps > 0 && k.vy <= 0.12 &&
        (k.y < R.solid_top - 0.3 || std::fabs(k.x) > bx - 7)) {
      // steer inward while jumping
      if (k.x < 0) k.vx = std::min(k.vx + 0.12, 0.45); else k.vx = std::max(k.vx - 0.12, -0.45);
      --jumps; k.vy = R.jump_air;
    }
    if (use_uc && jumps == 0 && k.vy < -0.2 && m >= R.cost_uppercut && k.y < R.solid_top - 0.5 && k.y > R.blast_bottom + 6) {
      k.vy = 0.75; k.vx *= 0.3;
      m -= R.cost_uppercut;
      // steer inward
      if (k.x < 0) k.vx = std::min(k.vx + 0.3, 0.45); else k.vx = std::max(k.vx - 0.3, -0.45);
      use_uc = false;  // once
    }
    kin_tick(k);
  }
  return r;
}

// the actual recovery control: called when we are off stage
static bool recover_controls(const P &me, long long tick, J &order) {
  if (me.grounded) return false;
  if (over_stage(me)) return false;  // above the stage footprint: normal play handles it
  SimResult with_all = simulate_recovery(me, tick, true, me.meter, true);
  if (!with_all.safe) {
    // even with everything we might die: still do the standard thing
  }
  order["move"] = me.x < 0 ? 1.0 : -1.0;
  // jump when falling and below stage top (or dangerously far out)
  double bx = blast_x_at(tick);
  bool far_out = std::fabs(me.x) > bx - 9;
  if (me.vy < 0 && me.air_jumps > 0 && (me.y < R.solid_top - 0.6 || far_out)) {
    // check that jumping is actually better than falling: simulate without jump
    SimResult no_jump = simulate_recovery(me, tick, false, me.meter, false);
    SimResult with_jump = simulate_recovery(me, tick, true, me.meter, false);
    if (no_jump.safe && (!with_jump.safe || no_jump.land_tick <= with_jump.land_tick + 6)) {
      // falling lands fine and no faster need: hold the jump
    } else if (with_jump.safe) {
      order["jump"] = true;
    } else if (no_jump.safe) {
      // hold
    } else {
      order["jump"] = true;  // nothing works without it; take the chance
    }
  }
  // uppercut as the last resort when out of jumps
  if (me.air_jumps == 0 && me.vy < 0 && me.meter >= R.cost_uppercut && me.y < R.solid_top - 0.3) {
    SimResult uc = simulate_recovery(me, tick, false, me.meter, true);
    if (!uc.safe || uc.land_tick > 60) {
      // only if it helps
    }
    // simple: use it when clearly below the ledge and still falling
    if (me.y < R.solid_top - 1.0) { order["skill"] = "uppercut"; order["dir"] = J::array{me.x < 0 ? 1.0 : -1.0, 0.0}; }
  }
  return true;
}

// hit reaction: direction we would be sent if hit by (dx,dy)
static void steer_home_move(const P &me, J &order, double urgency) {
  double target = std::clamp(-me.x / 4.0, -1.0, 1.0);
  if (std::fabs(me.x) < 1.5) target = 0;
  order["move"] = target * urgency;
}

static J decide(const J &obs) {
  J order = J::object{};
  const auto &ps = obs.at("players").as_array();
  int seat = obs.at("seat").get<int>();
  long long tick = obs.at("tick").as_int();

  if (R.solid_x2 == 13.0 && R.run_speed == 0.0) load_rules(obs.at("rules"));  // unused guard
  static bool loaded = false;
  if (!loaded) { load_rules(obs.at("rules")); loaded = true; }

  const P me = read(ps.at(seat), seat);
  if (me.out || me.respawn > 0) return order;

  int n = int(ps.size());
  std::vector<P> pl(n);
  for (int i = 0; i < n; ++i) pl[i] = read(ps.at(i), i);

  // ---- charging: keep or release ----
  if (me.act == "charge" && me.phase == "charging") {
    // find nearest foe and threats
    double dist = 1e18;
    for (int i = 0; i < n; ++i) {
      if (i == seat || pl[i].out || pl[i].respawn > 0 || pl[i].invuln > 0) continue;
      dist = std::min(dist, std::hypot(pl[i].x - me.x, pl[i].y - me.y));
    }
    bool threat = false;
    for (int i = 0; i < n; ++i) {
      if (i == seat || pl[i].out || pl[i].respawn > 0) continue;
      const P &o = pl[i];
      if (melee_active(o) || (o.phase == "windup" && o.phase_t <= 2)) {
        if (std::fabs(o.x - me.x) < 3.4 && std::fabs(o.y - me.y) < 3.0) threat = true;
      }
      if (blinking(o) && std::fabs(o.x - me.x) < 8) threat = true;  // they may appear next to us
    }
    bool release = me.charge >= 40 || (dist < 3.4 && me.charge >= 14) || threat;
    if (threat && me.meter >= R.cost_blink && me.charge < 14) {
      order["skill"] = "blink";
      order["dir"] = J::array{me.x > 0 ? -1.0 : 1.0, 0.0};
      return order;
    }
    if (release) {
      // release by ordering a different skill or nothing: issuing no skill key releases it
      order["skill"] = "charge";  // NOTE: this continues charging; we must omit to release
    }
    order["move"] = dist < 3.0 ? 0.0 : (pl[0].x < me.x ? 1.0 : -1.0);
    // releasing: omit the skill key entirely (fall through with no skill)
    if (release) order.erase("skill");
    order["move"] = 0.0;
    return order;
  }

  // ---- off stage: recover ----
  // being outside the stage footprint while airborne at all counts: steer home
  if (!me.grounded && !over_stage(me)) {
    if (recover_controls(me, tick, order)) return order;
  }
  if (!me.grounded && me.y < R.solid_top - 2.2 && std::fabs(me.x) < R.solid_x2 + 2.0) {
    // above the stage gap but low: still climb a bit
    SimResult r = simulate_recovery(me, tick, true, me.meter, true);
    if (!r.safe) { if (recover_controls(me, tick, order)) return order; }
  }

  // ---- ledge guard: airborne below the stage top or barely over the lip and not on
  // solid ground counts as recovering; never chase foes from there.
  bool precarious = !me.grounded && (me.y < R.solid_top + 2.6) && std::fabs(me.x) > R.solid_x2 - 1.2;

  // ---- combo escape: just got knocked (damage jumped) and are now actionable -> blink away.
  // Uses memory: last seen damage; if it rose and hitstun is gone but gap-free, spend a blink
  // to leave strike range before the next hit lands. This breaks the attack/charge combo loops
  // that killed us in ep1 (33% of hits we took were combo extensions).
  static double last_damage = -1;
  static long long last_tick = -999;
  bool just_hurt = last_damage >= 0 && me.damage > last_damage + 0.5 && tick - last_tick <= 40;
  last_damage = me.damage;
  last_tick = tick;
  if (just_hurt && me.act.empty() && me.gap == 0 && me.hitstun == 0 && me.meter >= R.cost_blink &&
      me.grounded) {
    // grounded escape: blink toward stage centre
    order["skill"] = "blink";
    order["dir"] = J::array{me.x > 0 ? -1.0 : 1.0, 0.0};
    return order;
  }

  // ---- positioning: never brawl at the very edge of the stage ----
  // (ep1: I repeatedly stood at |x|>12 and got charge/slam'd off with no room to survive)
  double edge_margin = 11.0;  // stay within this |x| while grounded and fighting
  bool too_far_out = me.grounded && std::fabs(me.x) > edge_margin;

  // ---- pick target ----
  int tgt = -1;
  double best_score = -1e18;
  for (int i = 0; i < n; ++i) {
    if (i == seat || pl[i].out || pl[i].respawn > 0) continue;
    const P &o = pl[i];
    double dist = std::hypot(o.x - me.x, o.y - me.y);
    double s = -dist;
    if (o.invuln > 6) continue;
    if (o.invuln > 0) s -= 3;
    if (off_stage(o) && !precarious) s += 7;      // finish them, but not while we dangle
    if (o.hitstun > 6) s += 4;
    if (o.stocks <= 1) s += 1.5;
    if (countering(o) || guarding(o)) s -= 2.5;
    if (o.act.empty() && o.gap == 0 && dist < 4) s -= 0.5;
    if (o.act == "charge" && o.phase == "charging") s += 2.0;  // charge them out of it
    if (precarious) s -= std::fabs(o.x) * 0.5 + 20;  // strongly prefer center-side foes
    if (s > best_score) { best_score = s; tgt = i; }
  }
  if (precarious && tgt >= 0) {
    const P &o = pl[tgt];
    // while dangling near the ledge, move toward the stage interior, not the foe
    order["move"] = me.x > 0 ? -1.0 : 1.0;
    if (me.vy < 0 && me.air_jumps > 0 && me.y < R.solid_top - 0.3) order["jump"] = true;
    if (me.air_jumps == 0 && me.vy < 0 && me.meter >= R.cost_uppercut && me.y < R.solid_top - 1.0) {
      order["skill"] = "uppercut"; order["dir"] = J::array{me.x > 0 ? -1.0 : 1.0, 0.0};
    }
    return order;
  }
  if (tgt < 0) {
    order["move"] = me.x > 0 ? -0.6 : 0.6;
    return order;
  }
  const P &tg = pl[tgt];

  // too close to the edge while grounded: walk back in before fighting
  if (too_far_out) {
    order["move"] = me.x > 0 ? -1.0 : 1.0;
    return order;
  }

  // ---- high-damage retreat: at 60+ damage any solid hit sends us flying, so
  // keep distance from the nearest foe until things cool down (or a free punish shows).
  bool fragile = me.damage > 60.0;
  double nearest_dist = 1e18, nearest_dir = 0;
  for (int i = 0; i < n; ++i) {
    if (i == seat || pl[i].out || pl[i].respawn > 0 || pl[i].invuln > 0) continue;
    double dd = std::hypot(pl[i].x - me.x, pl[i].y - me.y);
    if (dd < nearest_dist) { nearest_dist = dd; nearest_dir = pl[i].x > me.x ? 1.0 : -1.0; }
  }
  bool free_punish = tgt >= 0 && (pl[tgt].hitstun > 10 || (pl[tgt].act != "charge" && !pl[tgt].act.empty() && pl[tgt].phase == "recover" && pl[tgt].phase_t > 6));
  if (fragile && nearest_dist < 6.0 && !free_punish && me.grounded) {
    // back off toward the stage centre, away from the nearest foe
    double away = -nearest_dir;
    if (std::fabs(me.x + away * 3.0) > 11.0) away = me.x > 0 ? -1.0 : 1.0;  // don't retreat off the edge
    order["move"] = away;
    order["jump"] = false;
    return order;
  }

  bool can_act = me.act.empty() && me.gap == 0 && me.hitstun == 0;

  // ---- projectile danger ----
  bool proj_danger = false; int proj_eta = 99; double proj_vx = 0; bool proj_hook = false;
  {
    const auto &shots = obs.at("shots").as_array();
    for (const auto &s : shots) {
      if (s.at("owner").get<int>() == seat) continue;
      std::string kind = s.at("kind").as_string();
      double sx = s.at("x").as_double(), sy = s.at("y").as_double();
      double svx = s.at("vx").as_double(), svy = s.at("vy").as_double();
      int ttl = s.at("ttl").get<int>();
      Kin mk{me.x, me.y, me.vx, me.vy, me.grounded};
      double px = sx, py = sy;
      for (int t = 0; t < std::min(ttl, 30); ++t) {
        px += svx; py += svy;
        kin_tick(mk);
        double ddx = mk.x - px, ddy = (mk.y + R.height / 2) - py;
        double reach = 0.5 + R.height / 2 + 0.25;
        if (ddx * ddx + ddy * ddy <= reach * reach) { proj_danger = true; proj_eta = t; proj_vx = svx; proj_hook = (kind == "hook"); break; }
      }
      if (proj_danger) break;
    }
  }

  // ---- melee danger ----
  int melee_threat = -1, melee_eta = 99;
  for (int i = 0; i < n; ++i) {
    if (i == seat || pl[i].out || pl[i].respawn > 0) continue;
    const P &o = pl[i];
    double reach = 0;
    if (o.act == "attack") reach = 1.3 + 0.9;
    else if (o.act == "charge") reach = 1.5 + 1.2;
    else if (o.act == "uppercut") reach = 1.2 + 1.1;
    else if (o.act == "slam") reach = 0.9 + 3.0;
    else if (o.act == "grab") reach = 1.2 + 0.9;
    else continue;
    if (o.phase != "windup" && o.phase != "active" && o.phase != "charging") continue;
    double dd = std::hypot(o.x - me.x, (o.y + 0.9) - (me.y + 0.9));
    int eta = 99;
    if (o.phase == "windup") eta = o.phase_t;
    else if (o.phase == "active") eta = 0;
    else if (o.phase == "charging") eta = 12;
    if (dd <= reach + 0.6 && eta < melee_eta) { melee_eta = eta; melee_threat = i; }
  }

  // ---- defensive reactions ----
  if (can_act) {
    if (proj_danger && proj_eta <= 2 && me.meter >= R.cost_blink) {
      double away = proj_vx > 0 ? -1.0 : 1.0;
      order["skill"] = "blink"; order["dir"] = J::array{away, 0.2};
      return order;
    }
    if (melee_threat >= 0 && melee_eta <= 3) {  // react while their blow is still coming
      const P &o = pl[melee_threat];
      // guard is free and blocks strikes (not grabs); it is the workhorse defence
      if (o.act != "grab") {
        order["skill"] = "guard";
        return order;
      }
      if (me.meter >= R.cost_blink) {
        order["skill"] = "blink"; order["dir"] = J::array{me.x < o.x ? -1.0 : 1.0, 0.3};
        return order;
      }
    }
  }

  // ---- approaching ----
  double dx = tg.x - me.x, dy = tg.y - me.y;
  double adx = std::fabs(dx), ady = std::fabs(dy);
  // predicted separation when a fresh attack's active frames land (~5 ticks from now)
  double pdx = dx - (tg.vx - me.vx) * 5.0;
  bool want_close = adx > 1.6 || ady > 1.8;
  if (want_close) {
    order["move"] = dx > 0 ? 1.0 : -1.0;
    if (dy > 2.2 && me.grounded && adx < 4.0) order["jump"] = true;
    if (dy > 4.5 && me.grounded && adx < 2.0) { order["jump"] = true; }
  } else {
    order["move"] = 0.0;
  }

  // ---- attacking ----
  if (can_act) {
    bool in_range = std::fabs(pdx) < 1.35 && ady < 1.4;   // predicted hit for attack
    bool grab_ok = !guarding(tg) && !countering(tg) && adx < 1.5 && ady < 1.2;
    if (me.grounded) {
      // only swing when no third party is already winding up at us (whiff = free punish)
      bool bystander_threat = false;
      for (int i = 0; i < n; ++i) {
        if (i == seat || i == tgt || pl[i].out || pl[i].respawn > 0) continue;
        const P &o = pl[i];
        if ((o.phase == "windup" || o.phase == "active" || (o.phase == "charging" && o.charge > 8)) &&
            std::hypot(o.x - me.x, o.y - me.y) < 4.0) bystander_threat = true;
      }
      if (in_range && !bystander_threat) {
        order["skill"] = "attack"; order["dir"] = J::array{dx > 0 ? 1.0 : -1.0, std::clamp(dy, -0.5, 0.5)};
        return order;
      }
      // grab as a mix-up on grounded defensive foes in true range
      if (grab_ok && me.meter >= R.cost_grab && (guarding(tg) || countering(tg) || tg.hitstun > 4)) {
        order["skill"] = "grab"; order["dir"] = J::array{dx > 0 ? 1.0 : -1.0, 0.0};
        return order;
      }
      // charge only as a punish on a long-stunned foe
      if (adx < 2.4 && ady < 1.2 && tg.hitstun > 10) {
        order["skill"] = "charge";
        return order;
      }
    } else {
      // air options — slam has 16t recover and leaves us helpless, so only commit when
      // the target is actually below and near (predicted), never as a hop-in.
      double pdy = (tg.y + tg.vy * 6.0) - (me.y + me.vy * 6.0);
      double pdx2 = (tg.x + tg.vx * 6.0) - (me.x + me.vx * 6.0);
      if (pdy < -1.0 && std::fabs(pdx2) < 1.8 && me.meter >= R.cost_slam) {
        order["skill"] = "slam"; order["dir"] = J::array{0, -1.0};
        return order;
      }
    }
    // wave sniping at off-stage foes
    if (off_stage(tg) && me.meter >= R.cost_wave && me.grounded && std::fabs(dy) < 1.5) {
      double ax = dx, ay = dy * 0.3;
      double l = std::hypot(ax, ay); if (l > 1e-9) { ax /= l; ay /= l; }
      order["skill"] = "wave"; order["dir"] = J::array{ax, ay};
      return order;
    }
  }

  // default: if no movement was chosen, drift toward the stage center
  if (!order.contains("move")) steer_home_move(me, order, 0.4);
  return order;
}

}  // namespace smash_bot

int main() {
  return sm_step_worker::run_policy([](const J &obs) { return smash_bot::decide(obs); });
}
