// Platform smash bot: recovery planner + target selection + spacing/attack heuristics.
#include "include/step_worker.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using J = mini_json::Json;

namespace {

constexpr double kPi = 3.14159265358979323846;

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static double sq(double v) { return v * v; }

// ---------- cached rules ----------
struct MoveCfg {
  double cost = 0, damage = 0, power = 0, offset = 0, radius = 0, lift = 0, speed = 0;
  int windup = 0, active = 0, recover = 0, ttl = 0, max_ticks = 0, auto_release = 0;
};
struct Cfg {
  bool ok = false;
  double gravity = 0, max_fall = 0, run_speed = 0, ground_accel = 0, air_accel = 0;
  double ground_friction = 0, air_friction = 0, hit_friction = 0, bounce = 0;
  double jump_ground = 0, jump_air = 0;
  int air_jumps = 1, drop_ticks = 10;
  double half_w = 0.6, body_h = 1.8;
  double blast_x = 26, blast_top = 24, blast_bottom = -16;
  double respawn_x = 0, respawn_y = 13;
  int respawn_ticks = 60, respawn_invuln = 90, kill_credit = 300, gap_ticks = 4, max_turns = 1800;
  double meter_max = 1, meter_start = 1, meter_refill = 0.004;
  int shrink_start = 3000;
  double shrink_rate = 0.02, shrink_top_ratio = 0.9, min_x = 0, min_top = 1;
  int decision_ticks = 3;
  MoveCfg attack, charge, guard, blink, uppercut, slam, wave, grab, counter, hook;
  // stage geometry (fixed by rules but read once)
  struct Solid { double x1, x2, top, bottom; };
  struct Plat { double x1, x2, y; };
  std::vector<Solid> solids;
  std::vector<Plat> plats;
};
static Cfg C;

static const J &mj(const J &rules, const char *k) { return rules.at("moves").at(k); }
static MoveCfg parse_move(const J &rules, const char *k) {
  const J &m = mj(rules, k);
  MoveCfg r;
  auto gd = [&](const char *f, double def) { return m.contains(f) ? m.at(f).as_double() : def; };
  auto gi = [&](const char *f, int def) { return m.contains(f) ? m.at(f).get<int>() : def; };
  r.cost = gd("cost", 0);
  r.damage = gd("damage", 0);
  r.power = gd("power", 0);
  r.offset = gd("offset", 0);
  r.radius = gd("radius", 0);
  r.lift = gd("lift", 0);
  r.speed = gd("speed", 0);
  r.windup = gi("windup", 0);
  r.active = gi("active", 1);
  r.recover = gi("recover", 0);
  r.ttl = gi("ttl", 0);
  r.max_ticks = gi("max_ticks", 0);
  r.auto_release = gi("auto_release_ticks", 0);
  return r;
}
static void init_cfg(const J &rules) {
  if (C.ok) return;
  C.ok = true;
  const J &ph = rules.at("physics");
  C.gravity = ph.at("gravity").as_double();
  C.max_fall = ph.at("max_fall").as_double();
  C.run_speed = ph.at("run_speed").as_double();
  C.ground_accel = ph.at("ground_accel").as_double();
  C.air_accel = ph.at("air_accel").as_double();
  C.ground_friction = ph.at("ground_friction").as_double();
  C.air_friction = ph.at("air_friction").as_double();
  C.hit_friction = ph.at("hit_friction").as_double();
  C.bounce = ph.at("bounce").as_double();
  C.jump_ground = rules.at("jump").at("ground_speed").as_double();
  C.jump_air = rules.at("jump").at("air_speed").as_double();
  C.air_jumps = rules.at("jump").at("air_jumps").get<int>();
  C.drop_ticks = rules.at("jump").at("drop_ticks").get<int>();
  C.half_w = rules.at("body").at("half_width").as_double();
  C.body_h = rules.at("body").at("height").as_double();
  const J &st = rules.at("stage");
  C.blast_x = st.at("blast").at("x").as_double();
  C.blast_top = st.at("blast").at("top").as_double();
  C.blast_bottom = st.at("blast").at("bottom").as_double();
  C.respawn_x = st.at("respawn").at(0).as_double();
  C.respawn_y = st.at("respawn").at(1).as_double();
  if (st.contains("blast_shrink")) {
    const J &s = st.at("blast_shrink");
    C.shrink_start = s.at("start_tick").get<int>();
    C.shrink_rate = s.at("rate").as_double();
    C.shrink_top_ratio = s.at("top_ratio").as_double();
    C.min_x = s.at("min_x").as_double();
    C.min_top = s.at("min_top").as_double();
  }
  C.respawn_ticks = rules.at("respawn_ticks").get<int>();
  C.respawn_invuln = rules.at("respawn_invuln_ticks").get<int>();
  C.kill_credit = rules.at("kill_credit_ticks").get<int>();
  C.gap_ticks = rules.at("gap_ticks").get<int>();
  C.max_turns = rules.at("max_turns").get<int>();
  C.decision_ticks = rules.at("decision_ticks").get<int>();
  C.meter_max = rules.at("meter").at("max").as_double();
  C.meter_start = rules.at("meter").at("start").as_double();
  C.meter_refill = rules.at("meter").at("refill").as_double();
  for (const auto &a : st.at("solids").as_array())
    C.solids.push_back({a.at("x1").as_double(), a.at("x2").as_double(), a.at("top").as_double(), a.at("bottom").as_double()});
  for (const auto &a : st.at("platforms").as_array())
    C.plats.push_back({a.at("x1").as_double(), a.at("x2").as_double(), a.at("y").as_double()});
  C.attack = parse_move(rules, "attack");
  C.charge = parse_move(rules, "charge");
  C.guard = parse_move(rules, "guard");
  C.blink = parse_move(rules, "blink");
  C.uppercut = parse_move(rules, "uppercut");
  C.slam = parse_move(rules, "slam");
  C.wave = parse_move(rules, "wave");
  C.grab = parse_move(rules, "grab");
  C.counter = parse_move(rules, "counter");
  C.hook = parse_move(rules, "hook");
}

// ---------- observation entities ----------
struct Ent {
  double x = 0, y = 0, vx = 0, vy = 0, damage = 0, meter = 0, dx = 1, dy = 0;
  int facing = 1, grounded = 0, air_jumps = 0, stocks = 0, respawn = 0, invuln = 0;
  int hitstun = 0, gap = 0, hook_cd = 0, phase_ticks = 0, charge = 0;
  bool out = false;
  std::string act, phase;
};
static Ent parse_ent(const J &p) {
  Ent e;
  e.x = p.at("x").as_double();
  e.y = p.at("y").as_double();
  e.vx = p.at("vx").as_double();
  e.vy = p.at("vy").as_double();
  e.damage = p.at("damage").as_double();
  e.meter = p.at("meter").as_double();
  e.facing = p.at("facing").get<int>();
  e.grounded = p.at("grounded").as_bool() ? 1 : 0;
  e.air_jumps = p.at("air_jumps").get<int>();
  e.stocks = p.at("stocks").get<int>();
  e.respawn = p.at("respawn").get<int>();
  e.invuln = p.at("invuln").get<int>();
  e.hitstun = p.at("hitstun").get<int>();
  e.gap = p.at("gap").get<int>();
  e.hook_cd = p.at("hook_cd").get<int>();
  e.phase_ticks = p.at("phase_ticks").get<int>();
  e.charge = p.at("charge").get<int>();
  e.out = p.at("out").as_bool();
  if (p.at("act").is_string()) e.act = p.at("act").as_string();
  e.phase = p.at("phase").as_string();
  if (p.contains("dir") && p.at("dir").is_array()) {
    e.dx = p.at("dir").at(0).as_double();
    e.dy = p.at("dir").at(1).as_double();
  }
  return e;
}
struct Shot {
  int owner = 0, ttl = 0;
  double x = 0, y = 0, vx = 0, vy = 0;
  std::string kind;
};
static Shot parse_shot(const J &s) {
  Shot r;
  r.owner = s.at("owner").get<int>();
  r.ttl = s.at("ttl").get<int>();
  r.x = s.at("x").as_double();
  r.y = s.at("y").as_double();
  r.vx = s.at("vx").as_double();
  r.vy = s.at("vy").as_double();
  r.kind = s.at("kind").as_string();
  return r;
}

static double bound_x(int tick) {
  double b = C.blast_x;
  if (tick > C.shrink_start) b = std::max(C.min_x, b - (tick - C.shrink_start) * C.shrink_rate);
  return b;
}
static double bound_top(int tick) {
  double b = C.blast_top;
  if (tick > C.shrink_start) b = std::max(C.min_top, b - (tick - C.shrink_start) * C.shrink_rate * C.shrink_top_ratio);
  return b;
}

// ---------- local physics simulation (mirrors engine motion for one fighter) ----------
struct Sim {
  double x = 0, y = 0, vx = 0, vy = 0, meter = 1;
  int facing = 1, grounded = 1, air_jumps = 1, drop = 0, gap = 0;
  std::string act;
  int phase = 0, phase_t = 0;  // 0 idle 2 windup 3 active 4 recover
  double dx = 1, dy = 0;
};

static bool sim_overlap(const Sim &f, const Cfg::Solid &s) {
  return f.x + C.half_w > s.x1 && f.x - C.half_w < s.x2 && f.y < s.top && f.y + C.body_h > s.bottom;
}
static void sim_land(Sim &f, double y) {
  f.y = y;
  f.vy = 0;
  f.grounded = true;
  f.air_jumps = C.air_jumps;
  if ((f.act == "slam" || f.act == "uppercut") && f.phase == 3) {
    f.act.clear();
    f.phase = 0;
    f.phase_t = 0;
    f.gap = C.gap_ticks;
  }
}
static void sim_collide_y(Sim &f) {
  for (const auto &s : C.solids) {
    if (!sim_overlap(f, s)) continue;
    if (f.vy <= 0) sim_land(f, s.top);
    else { f.y = s.bottom - C.body_h; f.vy = 0; }
  }
  if (f.vy <= 0 && f.drop == 0 && !f.grounded) {
    for (const auto &p : C.plats) {
      if (f.y >= p.y && f.y <= p.y && f.x + C.half_w > p.x1 && f.x - C.half_w < p.x2) {
        sim_land(f, p.y);
        break;
      }
    }
  }
}
// same as engine: "foot" is y before vertical motion
static void sim_collide_y2(Sim &f, double foot) {
  for (const auto &s : C.solids) {
    if (!sim_overlap(f, s)) continue;
    if (f.vy <= 0) sim_land(f, s.top);
    else { f.y = s.bottom - C.body_h; f.vy = 0; }
  }
  if (f.vy <= 0 && f.drop == 0 && !f.grounded) {
    for (const auto &p : C.plats) {
      if (foot >= p.y && f.y <= p.y && f.x + C.half_w > p.x1 && f.x - C.half_w < p.x2) {
        sim_land(f, p.y);
        break;
      }
    }
  }
}
static void sim_motion(Sim &f) {
  bool floating = (f.act == "blink" && f.phase == 3);
  if (!floating) {
    f.vy = std::max(f.vy - C.gravity, -C.max_fall);
    if (!f.grounded) f.vx *= C.air_friction;
  }
  f.x += f.vx;
  for (const auto &s : C.solids) {
    if (!sim_overlap(f, s)) continue;
    f.x = f.vx > 0 ? s.x1 - C.half_w : s.x2 + C.half_w;
    f.vx = 0;
  }
  double foot = f.y;
  f.y += f.vy;
  f.grounded = false;
  sim_collide_y2(f, foot);
}
static void sim_start(Sim &f, const std::string &skill, double dx, double dy) {
  if (skill == "uppercut") {
    if (f.meter + 1e-9 < C.uppercut.cost) return;
    f.meter -= C.uppercut.cost;
    f.act = skill;
    f.phase = 2;
    f.phase_t = C.uppercut.windup;
  } else if (skill == "blink") {
    if (f.meter + 1e-9 < C.blink.cost) return;
    f.meter -= C.blink.cost;
    f.act = skill;
    f.phase = 3;
    f.phase_t = C.blink.active;
    double l = std::sqrt(dx * dx + dy * dy);
    if (l < 1e-9) { dx = f.facing; dy = 0; l = 1; }
    f.dx = dx / l;
    f.dy = dy / l;
    f.vx = f.dx * C.blink.speed;
    f.vy = f.dy * C.blink.speed;
    f.grounded = false;
  }
}
static void sim_advance(Sim &f) {
  if (f.act == "uppercut") {
    if (f.phase == 2) {
      if (--f.phase_t <= 0) {
        f.phase = 3;
        f.phase_t = C.uppercut.active;
        f.vy = 0.75;
        f.grounded = false;
        f.dx = 0;
        f.dy = 1;
      }
    } else if (f.phase == 3) {
      if (--f.phase_t <= 0) { f.phase = 4; f.phase_t = C.uppercut.recover; }
    } else if (f.phase == 4) {
      if (--f.phase_t <= 0) { f.act.clear(); f.phase = 0; f.phase_t = 0; f.gap = C.gap_ticks; }
    }
  } else if (f.act == "blink") {
    if (f.phase == 3) {
      if (--f.phase_t <= 0) { f.phase = 4; f.phase_t = C.blink.recover; f.vx *= 0.2; f.vy *= 0.2; }
    } else if (f.phase == 4) {
      if (--f.phase_t <= 0) { f.act.clear(); f.phase = 0; f.phase_t = 0; f.gap = C.gap_ticks; }
    }
  }
}
// one tick: move/jump control then motion. begin=true for the first tick of a decision.
static void sim_tick(Sim &f, double move, bool jump, const std::string &skill, double sdx, double sdy, bool begin) {
  if (f.drop > 0) --f.drop;
  if (f.act.empty()) {
    if (f.gap > 0) --f.gap;
    else if (!skill.empty()) sim_start(f, skill, sdx, sdy);
  } else {
    sim_advance(f);
  }
  if (begin && jump && (f.act.empty() || f.act == "charge")) {
    if (f.grounded) { f.vy = C.jump_ground; f.grounded = false; }
    else if (f.air_jumps > 0) { --f.air_jumps; f.vy = C.jump_air; }
  }
  // drive
  bool locked = !f.act.empty() && f.act != "charge";
  if (f.act == "guard" ) {
    if (f.grounded) f.vx = 0;
  } else if ((f.act == "blink" && f.phase == 3)) {
    // velocity set at start; keep
  } else if (locked) {
    if (f.grounded) f.vx *= C.ground_friction;
  } else {
    double want = move * C.run_speed;
    double a = f.grounded ? C.ground_accel : C.air_accel;
    f.vx += clampd(want - f.vx, -a, a);
    if (f.act.empty()) { if (move > 0.05) f.facing = 1; else if (move < -0.05) f.facing = -1; }
  }
  sim_motion(f);
}

// ---------- recovery plan search ----------
struct FirstAct {
  double move = 0, dx = 0, dy = 0;
  bool jump = false;
  std::string skill;
};
struct RecEval {
  bool success = false;
  int land_tick = 100000;
  double min_y = 1e9;
  double max_y = -1e9;
  double end_dist = 1e9;
  FirstAct first;
};
static RecEval run_recovery_plan(Sim s, int jump_thresh_x10, int upper_thresh_x10, bool open_blink, int start_tick) {
  RecEval r;
  bool upper_used = false;
  bool blinked = false;
  int sim_tick_no = start_tick;
  for (int step = 0; step < 60; ++step) {
    double target_x = 0;
    double move = s.x > target_x ? -1.0 : 1.0;
    if (std::fabs(s.x - target_x) < 0.7) move = 0;
    bool jump = false;
    std::string skill;
    double sdx = 0, sdy = 0;
    double bt = bound_top(sim_tick_no), bx = bound_x(sim_tick_no);
    if (!s.grounded) {
      double jt = jump_thresh_x10 / 10.0, ut = upper_thresh_x10 / 10.0;
      if (open_blink && !blinked && step <= 2 && s.meter >= C.blink.cost + 1e-9 && std::fabs(s.x) > 13.0) {
        skill = "blink";
        sdx = (s.x > 0 ? -1.0 : 1.0);
        sdy = (s.y + 6.0 > bt - 2.0) ? -0.25 : 0.2;
        if (s.y - 6.0 < C.blast_bottom + 2.0) sdy = 0.3;
        blinked = true;
      } else if (s.air_jumps > 0 && s.vy < 0.05 && s.y < jt) {
        jump = true;
      } else if (!upper_used && s.meter >= C.uppercut.cost + 1e-9 && s.vy < 0.0 && s.y < ut && s.y + 8.0 < bt - 0.5) {
        skill = "uppercut";
        upper_used = true;
      }
    }
    for (int k = 0; k < C.decision_ticks; ++k) {
      if (step == 0 && k == 0) { r.first.move = move; r.first.jump = jump; r.first.skill = skill; r.first.dx = sdx; r.first.dy = sdy; }
      sim_tick(s, move, jump && k == 0, skill, sdx, sdy, k == 0);
      ++sim_tick_no;
      if (s.y < r.min_y) r.min_y = s.y;
      if (s.y > r.max_y) r.max_y = s.y;
      double btc = bound_top(sim_tick_no), bxc = bound_x(sim_tick_no);
      if (s.grounded) {
        r.success = true;
        r.land_tick = step * C.decision_ticks + k;
        r.end_dist = std::fabs(s.x);
        return r;
      }
      if (s.y < C.blast_bottom + 0.5 || std::fabs(s.x) > bxc - 0.3 || s.y > btc - 0.3) {
        r.success = false;
        r.land_tick = 100000;
        r.end_dist = std::fabs(s.x) + std::fabs(s.y) * 2;
        return r;
      }
    }
    r.end_dist = std::fabs(s.x - target_x) + std::max(0.0, -s.y) * 2;
  }
  return r;
}

struct Decision {
  double move = 0;
  bool jump = false, drop = false;
  std::string skill;
  double dx = 0, dy = 0, has_dir = false;
};

static FirstAct plan_recovery(const Ent &me, int tick) {
  Sim base;
  base.x = me.x; base.y = me.y; base.vx = me.vx; base.vy = me.vy;
  base.facing = me.facing; base.grounded = me.grounded; base.air_jumps = me.air_jumps;
  base.meter = me.meter; base.gap = me.gap;
  FirstAct best;
  bool have = false;
  int best_land = 100000;
  double best_min = -1e9;
  static const int jump_ts[] = {50, 30, 15, 0, -15, -30};
  static const int upper_ts[] = {20, 5, -10, -25, -40, -55};
  for (int open = 0; open < 2; ++open) {
    for (int jt : jump_ts) {
      for (int ut : upper_ts) {
        RecEval e = run_recovery_plan(base, jt, ut, open != 0, tick);
        bool better = false;
        if (!have) better = true;
        else if (e.success != (best_land < 100000)) better = e.success;
        else if (e.success) better = (e.land_tick < best_land) || (e.land_tick == best_land && e.min_y > best_min);
        else better = (e.min_y > best_min + 0.25) || (std::fabs(e.min_y - best_min) <= 0.25 && e.end_dist < 1e8);
        if (better) {
          have = true;
          best = e.first;
          best_land = e.success ? e.land_tick : 100000;
          best_min = e.min_y;
        }
      }
    }
  }
  return best;
}

// ---------- combat helpers ----------
static double pred_y_at(const Ent &t, double dt) {
  double y = t.y + t.vy * dt - 0.5 * C.gravity * dt * dt;
  double x = t.x + t.vx * dt;
  // ground floor approximation
  if (y < 0 && std::fabs(x) < 13.0 && t.vy <= 0.2) y = 0;
  return y;
}
static double pred_x_at(const Ent &t, double dt) { return t.x + t.vx * dt; }

static void aim_dir(double fx, double fy, double tx, double ty, double &dx, double &dy) {
  double vx = tx - fx, vy = ty - fy;
  double l = std::sqrt(vx * vx + vy * vy);
  if (l < 1e-9) { dx = 1; dy = 0; return; }
  dx = vx / l;
  dy = vy / l;
}

static bool attack_would_hit(const Ent &me, const Ent &t, const std::string &skill, double dirx, double diry, double &hit_dt) {
  double wind = 0, active = 1, offset = 0, radius = 0;
  if (skill == "attack") { wind = C.attack.windup; active = C.attack.active; offset = C.attack.offset; radius = C.attack.radius; }
  else if (skill == "grab") { wind = C.grab.windup; active = C.grab.active; offset = C.grab.offset; radius = C.grab.radius; }
  else if (skill == "uppercut") { wind = C.uppercut.windup; active = C.uppercut.active; offset = C.uppercut.offset; radius = C.uppercut.radius; }
  else return false;
  double reach = radius + C.body_h / 2.0;
  for (int k = 0; k < (int)active; ++k) {
    double dt = wind + k;
    double tx = pred_x_at(t, dt), ty = pred_y_at(t, dt);
    double ox = me.x, oy = me.y;
    if (!me.grounded) {
      ox += me.vx * dt;
      oy += me.vy * dt - 0.5 * C.gravity * dt * dt;
    }
    if (skill == "uppercut") {
      double rise = 0.75 * (k + 1) - 0.5 * C.gravity * (k + 1) * (k + 1);
      if (rise < 0) rise = 0;
      oy += rise;
    }
    double ax = ox + dirx * offset, ay = oy + C.body_h / 2.0 + diry * offset;
    double dx = tx - ax, dy = (ty + C.body_h / 2.0) - ay;
    if (dx * dx + dy * dy <= sq(reach * 0.97)) { hit_dt = dt; return true; }
  }
  return false;
}
static bool slam_would_hit(const Ent &me, const Ent &t) {
  if (me.grounded) return false;
  // we fall fast while active; target must be below-ish and close
  double dx = t.x - me.x;
  double dy = t.y - me.y;
  if (std::fabs(dx) > 3.6) return false;
  if (dy > 0.2 || dy < -6.0) return false;
  return true;
}
// Will a slam that misses still land on some solid surface (instead of falling off the stage)?
static bool slam_has_landing(const Ent &me) {
  double x0 = me.x, x1 = me.x + me.vx * (C.slam.windup + C.slam.active + 2);
  // main stage
  if (x0 > -12.4 && x1 > -12.4 && x0 < 12.4 && x1 < 12.4) return true;
  // thin platforms below us
  for (const auto &p : C.plats) {
    if (me.y > p.y && x0 > p.x1 + 0.4 && x0 < p.x2 - 0.4 && x1 > p.x1 + 0.4 && x1 < p.x2 - 0.4) return true;
  }
  return false;
}
static bool wave_would_hit(const Ent &me, const Ent &t, double &dirx, double &diry) {
  if (me.meter + 1e-9 < C.wave.cost) return false;
  double dist = std::sqrt(sq(t.x - me.x) + sq(t.y - me.y));
  if (dist < 3.0 || dist > 24.0) return false;
  // lead the target using wave speed
  double sx = me.x, sy = me.y + C.body_h / 2.0;
  for (int it = 0; it < 3; ++it) {
    aim_dir(sx, sy, t.x, t.y + C.body_h / 2.0, dirx, diry);
    double travel = std::sqrt(sq(t.x - (sx + dirx * 1.1)) + sq(t.y + C.body_h / 2.0 - (sy + diry * 1.1)));
    double dt = travel / std::max(0.05, C.wave.speed);
    aim_dir(sx, sy, pred_x_at(t, dt), pred_y_at(t, dt) + C.body_h / 2.0, dirx, diry);
  }
  return true;
}

struct TargetPick { int idx = -1; double score = -1e18; };
static int g_focus = -1;
static int g_focus_tick = -100000;
static TargetPick pick_target(const std::vector<Ent> &ps, const Ent &me, int seat, int tick) {
  TargetPick best;
  for (int i = 0; i < (int)ps.size(); ++i) {
    if (i == seat) continue;
    const Ent &t = ps[i];
    if (t.out || t.respawn > 0 || t.invuln > 0) continue;
    double dx = t.x - me.x, dy = t.y - me.y;
    double dist = std::sqrt(dx * dx + dy * dy);
    double edge = std::max(0.0, std::fabs(t.x) - 8.0) + std::max(0.0, -(t.y + 1.0)) * 0.8;
    double score = -dist * 1.0 + t.damage * 0.022 + (t.stocks <= 1 ? 3.5 : 0.0) + edge * 0.45
                 + (t.damage >= 85.0 ? 2.0 : 0.0);
    // late shrink: prefer whoever is closer to the closing wall
    if (tick > C.shrink_start) {
      double bx = bound_x(tick);
      score += std::max(0.0, std::fabs(t.x) - (bx - 3.0)) * 1.5;
    }
    if (i == g_focus && tick - g_focus_tick < 210) score += 1.4;
    if (score > best.score) { best.score = score; best.idx = i; }
  }
  if (best.idx >= 0) { g_focus = best.idx; g_focus_tick = tick; }
  return best;
}

static bool is_guarding(const Ent &t) { return t.act == "guard" && t.phase == "active"; }
static bool is_countering(const Ent &t) { return t.act == "counter" && t.phase == "active"; }

static bool enemy_melee_would_hit(const Ent &t, const Ent &me, int tick) {
  if (t.out || t.respawn > 0 || t.invuln > 0) return false;
  bool charging = (t.act == "charge" && t.phase == "charging");
  if (t.phase != "windup" && t.phase != "active" && !charging) return false;
  double off = 0, rad = 0, ox = 0, oy = 0;
  bool above = false, below = false;
  if (t.act == "attack") { off = C.attack.offset; rad = C.attack.radius; }
  else if (t.act == "charge") { off = C.charge.offset; rad = C.charge.radius; }
  else if (t.act == "grab") { off = C.grab.offset; rad = C.grab.radius; }
  else if (t.act == "uppercut") { off = C.uppercut.offset; rad = C.uppercut.radius; above = true; }
  else if (t.act == "slam") { off = C.slam.offset; rad = C.slam.radius; below = true; }
  else return false;
  if (charging) { off = C.charge.offset; rad = C.charge.radius; }
  double ux = t.dx, uy = t.dy;
  double l = std::sqrt(ux * ux + uy * uy);
  if (l < 0.2) { ux = t.facing; uy = 0; l = 1; }
  ux /= l; uy /= l;
  // project the attacker a few ticks ahead (through the wind-up), with gravity for airborne
  int rem = 0;
  if (t.phase == "windup") rem = std::max(0, t.phase_ticks);
  if (charging) rem = 8;
  if (rem > 14) rem = 14;
  double px0 = t.x + t.vx * rem;
  double py0 = t.y + t.vy * rem - 0.5 * C.gravity * rem * rem;
  if (t.grounded || py0 < 0) py0 = std::max(py0, 0.0);
  double tx = me.x, ty = me.y + C.body_h / 2.0;
  double reach = rad + C.body_h / 2.0 + 0.4;
  // Evaluate the move over the first few active ticks with the engine's actual active-phase motion.
  int active_ticks = 4;
  if (t.act == "slam") active_ticks = 10;
  else if (t.act == "charge" || t.act == "attack" || t.act == "grab") active_ticks = 3;
  else if (t.act == "uppercut") active_ticks = 6;
  for (int k = 0; k < active_ticks; ++k) {
    double px = px0 + t.vx * k;
    double py = py0;
    if (t.act == "slam") py = py0 - 0.8 * (k + 1);
    else if (t.act == "uppercut") py = py0 + 0.75 * (k + 1) - 0.5 * C.gravity * (k + 1) * (k + 1);
    double cx = px + ux * off, cy = py + C.body_h / 2.0 + uy * off;
    if (t.act == "slam") { cx = px; cy = py; }
    double dx = tx - cx, dy = ty - cy;
    if (!(below && t.grounded) && dx * dx + dy * dy <= reach * reach) return true;
    if (above && std::fabs(me.x - px) < 2.0 && me.y > py + 0.1 && me.y < py + 3.4) return true;
    if (below) {
      double adx = me.x - px, ady = me.y - py;
      double rr = C.slam.radius + C.body_h / 2.0;
      if (adx * adx + ady * ady <= rr * rr) return true;
    }
  }
  return false;
}

// returns true if we should guard now against an imminent melee attack
static bool melee_threat(const std::vector<Ent> &ps, const Ent &me, int seat, int tick) {
  for (int i = 0; i < (int)ps.size(); ++i) {
    if (i == seat) continue;
    if (enemy_melee_would_hit(ps[i], me, tick)) return true;
  }
  return false;
}

struct ShotThreat {
  bool any = false, hook = false;
  double t = 1e9;
  double vx = 0, vy = 0, x = 0, y = 0;
};
static ShotThreat find_shot_threat(const std::vector<Shot> &shots, const Ent &me, int seat) {
  ShotThreat best;
  for (const auto &s : shots) {
    if (s.owner == seat) continue;
    double rx = s.x - me.x, ry = s.y - (me.y + C.body_h / 2.0);
    double vv = s.vx * s.vx + s.vy * s.vy;
    if (vv < 1e-9) continue;
    double tca = -(rx * s.vx + ry * s.vy) / vv;
    if (tca < 0 || tca > 16) continue;
    double cx = rx + s.vx * tca, cy = ry + s.vy * tca;
    double rad = (s.kind == "hook" ? 1.55 : 1.75);
    if (cx * cx + cy * cy < rad * rad && tca < best.t) {
      best.any = true;
      best.t = tca;
      best.hook = (s.kind == "hook");
      best.vx = s.vx; best.vy = s.vy; best.x = s.x; best.y = s.y;
    }
  }
  return best;
}
// choose a blink direction that dodges a shot and stays inside the blast lines
static bool safe_blink_dir(const Ent &me, const ShotThreat &th, int tick, double &ox, double &oy) {
  double bx = bound_x(tick + 7), bt = bound_top(tick + 7);
  if (bx < 2.5) return false;
  double best_score = -1e18;
  bool found = false;
  static const double dirs[8][2] = {{1, 0}, {-1, 0}, {0.7071, 0.7071}, {-0.7071, 0.7071},
                                    {0.7071, -0.7071}, {-0.7071, -0.7071}, {0, 1}, {0, -1}};
  for (int c = 0; c < 8; ++c) {
    double ux = dirs[c][0], uy = dirs[c][1];
    double px = me.x + ux * 6.0, py = me.y + uy * 6.0;
    if (std::fabs(px) > bx - 1.8) continue;
    if (py > bt - 1.8) continue;
    if (py < C.blast_bottom + 1.8) continue;
    if (py < -0.2 && std::fabs(px) > 13.0 && uy < 0) continue;
    double shotlen = std::sqrt(th.vx * th.vx + th.vy * th.vy);
    double along = shotlen > 1e-6 ? (ux * th.vx + uy * th.vy) / shotlen : 0;
    double score = -std::fabs(px) * 0.35 - std::fabs(py - me.y) * 0.5 - std::fabs(along) * 3.0;
    if (score > best_score) { best_score = score; ox = ux; oy = uy; found = true; }
  }
  return found;
}

// Wave aim helper for edgeguarding: aim at target with lead.
static void lead_wave_dir(const Ent &me, const Ent &t, double &dx, double &dy) {
  dx = (t.x > me.x ? 1.0 : -1.0);
  dy = 0;
  double sx = me.x, sy = me.y + C.body_h / 2.0;
  for (int it = 0; it < 3; ++it) {
    aim_dir(sx, sy, t.x, t.y + C.body_h / 2.0, dx, dy);
    double px = pred_x_at(t, 4), py = pred_y_at(t, 4);
    double d = std::sqrt(sq(px - sx) + sq(py + C.body_h / 2.0 - sy));
    double dt = d / std::max(0.05, C.wave.speed);
    aim_dir(sx, sy, pred_x_at(t, dt), pred_y_at(t, dt) + C.body_h / 2.0, dx, dy);
  }
}

static J make_order(const Decision &d) {
  J o = J::object{};
  if (std::fabs(d.move) > 1e-9) o["move"] = clampd(d.move, -1.0, 1.0);
  if (d.jump) o["jump"] = true;
  if (d.drop) o["drop"] = true;
  if (!d.skill.empty()) o["skill"] = d.skill;
  if (d.has_dir) o["dir"] = J::array{d.dx, d.dy};
  return o;
}

static const char *skill_name(const std::string &s) { return s.c_str(); }

}  // namespace

static J policy(const J &obs, J &memory) {
  (void)memory;
  init_cfg(obs.at("rules"));
  int seat = obs.at("seat").get<int>();
  int tick = obs.at("tick").get<int>();
  const auto &pj = obs.at("players").as_array();
  std::vector<Ent> ps;
  ps.reserve(pj.size());
  for (const auto &p : pj) ps.push_back(parse_ent(p));
  std::vector<Shot> shots;
  for (const auto &s : obs.at("shots").as_array()) shots.push_back(parse_shot(s));
  const Ent &me = ps[seat];

  J empty = J::object{};
  if (me.out) return empty;
  if (me.respawn > 0) return empty;
  if (me.hitstun > 0) return empty;
  // being reeled by a hook or pulled: wait it out
  if (me.act == "hook" && me.phase == "active" && me.phase_ticks > 100) return empty;

  Decision d;
  double bx = bound_x(tick + 2), bt = bound_top(tick + 2);

  bool offstage = (!me.grounded && (me.y < -0.25 || std::fabs(me.x) > 13.0));
  if (offstage) {
    if (!me.act.empty()) {
      // committed to an action: nothing useful can be done until it ends
      d.move = (me.x > 0 ? -1.0 : 1.0);
      return make_order(d);
    }
    FirstAct fa = plan_recovery(me, tick);
    d.move = fa.move;
    d.jump = fa.jump;
    if (!fa.skill.empty()) { d.skill = fa.skill; d.has_dir = true; d.dx = fa.dx; d.dy = fa.dy; }
    // If the search found no useful plan and we are far out, try a hook toward the stage.
    if (fa.skill.empty() && me.hook_cd == 0 && me.meter >= C.hook.cost + 1e-9 &&
        (std::fabs(me.x) > 18.0 || me.y < -10.0)) {
      double tx = (me.x > 0 ? 11.5 : -11.5), ty = 0.5;
      d.skill = "hook";
      aim_dir(me.x, me.y + C.body_h / 2.0, tx, ty, d.dx, d.dy);
      d.has_dir = true;
    }
    return make_order(d);
  }

  // ---- hard boundary avoidance ----
  bool late_mode = bound_top(tick + C.respawn_ticks + 6) < 13.0;
  if (tick > C.shrink_start) {
    double bx3 = bound_x(tick + 3), bt3 = bound_top(tick + 3);
    double margin = late_mode ? 3.2 : (me.stocks <= 1 ? 2.6 : 1.9);
    if (std::fabs(me.x) > std::max(0.35, bx3 - margin)) {
      d.move = (me.x > 0 ? -1.0 : 1.0);
      if (!me.grounded && me.vy < 0.0 && me.air_jumps > 0) d.jump = true;
      return make_order(d);
    }
    if (me.y > bt3 - (late_mode ? 3.2 : (me.stocks <= 1 ? 2.4 : 1.9))) {
      if (me.grounded && me.y > 0.25) d.drop = true;
      d.move = (me.x > 0 ? -1.0 : (me.x < 0 ? 1.0 : 0.0));
      return make_order(d);
    }
  }

  // ---- defense: shots ----
  ShotThreat th = find_shot_threat(shots, me, seat);
  if (th.any && me.act.empty() && me.gap == 0) {
    double reserve = (me.damage >= 55 ? 0.25 : 0.0);
    if (th.hook) {
      double bdx = 0, bdy = 0;
      if (me.meter >= C.blink.cost + reserve + 1e-9 && safe_blink_dir(me, th, tick, bdx, bdy)) {
        d.skill = "blink"; d.has_dir = true; d.dx = bdx; d.dy = bdy;
        return make_order(d);
      }
      if (me.grounded) d.jump = true;
      d.move = (th.vx > 0 ? -1.0 : 1.0);
      return make_order(d);
    }
    d.skill = "guard";
    return make_order(d);
  }

  // ---- defense: melee ----
  if (me.act.empty() && me.gap == 0 && melee_threat(ps, me, seat, tick)) {
    double reserve = (me.damage >= 55 ? 0.25 : 0.0);
    // A grab beats guard; try to blink away from a grab.
    bool grab_close = false;
    for (int i = 0; i < (int)ps.size(); ++i) {
      if (i == seat) continue;
      const Ent &t = ps[i];
      if (t.out || t.respawn > 0 || t.invuln > 0) continue;
      if (t.act == "grab" && t.phase != "recover" && std::fabs(t.x - me.x) < 2.8 && std::fabs(t.y - me.y) < 1.6) grab_close = true;
    }
    if (grab_close && me.meter >= C.blink.cost + 1e-9) {
      d.skill = "blink"; d.has_dir = true; d.dx = 0; d.dy = 1;
      return make_order(d);
    }
    // A counterable, telegraphed strike: reflect it for double damage instead of blocking.
    if (me.meter >= C.counter.cost + 0.25 + 1e-9 && !late_mode) {
      for (int i = 0; i < (int)ps.size(); ++i) {
        if (i == seat) continue;
        const Ent &t = ps[i];
        if (t.out || t.respawn > 0 || t.invuln > 0) continue;
        bool counterable = (t.act == "attack" || t.act == "uppercut" || t.act == "charge");
        if (!counterable || t.phase != "windup") continue;
        if (t.phase_ticks < 3 || t.phase_ticks > 10) continue;
        if (std::fabs(t.x - me.x) > 3.2 || std::fabs(t.y - me.y) > 2.8) continue;
        if (!enemy_melee_would_hit(t, me, tick)) continue;
        d.skill = "counter";
        return make_order(d);
      }
    }
    d.skill = "guard";
    return make_order(d);
  }

  // keep guarding if we already are and a threat is still around
  if (me.act == "guard") {
    if (melee_threat(ps, me, seat, tick)) { d.skill = "guard"; return make_order(d); }
    // otherwise let the guard drop and keep moving
  }

  // sitting on the top platform at high damage invites an uppercut into the top blast
  if (me.grounded && me.y > 8.5) {
    bool close_fight = false;
    for (int i = 0; i < (int)ps.size(); ++i) {
      if (i == seat || ps[i].out || ps[i].respawn > 0 || ps[i].invuln > 0) continue;
      if (std::fabs(ps[i].x - me.x) < 3.5 && std::fabs(ps[i].y - me.y) < 2.6) close_fight = true;
    }
    if (!close_fight && (me.damage >= 40.0 || tick > C.shrink_start - 400)) {
      d.drop = true;
      d.move = (me.x > 0 ? -1.0 : 1.0);
      return make_order(d);
    }
  }

  // ---- boundary awareness ----
  bool must_center = false;
  if (tick > C.shrink_start) {
    double danger_x = bx - 1.5;
    if (std::fabs(me.x) > danger_x) must_center = true;
    if (me.y > bt - 1.2) must_center = true;
  }
  // never voluntarily walk off the main stage
  auto clamp_stage_move = [&](double &mv) {
    double nx = me.x + mv * C.run_speed * C.decision_ticks;
    if (me.grounded && me.y < 0.2) {
      if (std::fabs(nx) > 12.3) { mv = (me.x > 0 ? (nx > 0 ? -1.0 : mv) : (nx < 0 ? 1.0 : mv)); }
    }
  };

  // If standing on a platform that the top boundary will eat, drop off.
  if (me.grounded && me.y > 0.2 && me.y + C.body_h > bt - 0.6) {
    d.drop = true;
    d.move = (me.x > 0 ? -1.0 : 1.0);
    return make_order(d);
  }

  // ---- edge guard / target ----
  TargetPick tp = pick_target(ps, me, seat, tick);
  if (tp.idx < 0) {
    // no valid target: head toward center, and down from platforms
    if (must_center) d.move = (me.x > 0 ? -1.0 : 1.0);
    else {
      if (me.grounded && me.y > 0.3) d.drop = true;
      d.move = (me.x > 0.5 ? -1.0 : (me.x < -0.5 ? 1.0 : 0.0));
    }
    return make_order(d);
  }
  const Ent &t = ps[tp.idx];
  double dx = t.x - me.x, dy = t.y - me.y;
  double dist = std::sqrt(dx * dx + dy * dy);
  int my_side = (me.x >= 0 ? 1 : -1);
  bool target_off = (t.y < -0.5 || std::fabs(t.x) > 13.5);
  double reserve = (me.damage >= 55 || tick > C.shrink_start - 300) ? 0.25 : 0.0;
  bool can_start = me.act.empty() && me.gap == 0;

  // ---- edge guarding ----
  if (target_off && t.invuln == 0 && can_start && !must_center && !late_mode &&
      !(tick > C.shrink_start - 300 && me.damage > 40)) {
    double edge = (t.x >= 0 ? 11.8 : -11.8);
    if (me.damage > 50.0) edge = (t.x >= 0 ? 10.6 : -10.6);
    if (me.grounded) {
      d.move = (edge - me.x > 0.4 ? 1.0 : (edge - me.x < -0.4 ? -1.0 : 0.0));
      // wave at them if we have the meter
      if (me.meter >= C.wave.cost + reserve + 1e-9 && dist < 24.0) {
        double wx, wy;
        lead_wave_dir(me, t, wx, wy);
        d.skill = "wave"; d.has_dir = true; d.dx = wx; d.dy = wy;
        return make_order(d);
      }
      // jump straight up at the ledge: a safe slam can reach just over the edge
      if (t.y < -0.3 && std::fabs(t.x - me.x) < 4.6 && std::fabs(me.x) > 9.0 &&
          bx > 15.0 && me.meter >= C.slam.cost + reserve + 1e-9) {
        d.jump = true;
        d.move = (me.x > 0 ? -0.25 : 0.25);
        return make_order(d);
      }
    } else {
      // in the air near the edge: slam if above, else hit if in reach, else drift back
      bool over_stage = std::fabs(me.x) < 13.0;
      double need = over_stage ? 0.5 : 2.2;
      if (me.meter >= C.slam.cost + reserve + 1e-9 && me.y > t.y + need && std::fabs(dx) < 3.2 && me.y < 9.0 &&
          slam_has_landing(me) && (over_stage || std::fabs(me.x) < 14.0)) {
        d.skill = "slam"; d.has_dir = true; d.dx = 0; d.dy = -1;
        return make_order(d);
      }
      double adx = 0, ady = 0;
      aim_dir(me.x, me.y, t.x, t.y, adx, ady);
      if (attack_would_hit(me, t, "attack", adx, ady, d.dx)) {
        d.skill = "attack"; d.has_dir = true; d.dx = adx; d.dy = ady;
        return make_order(d);
      }
      d.move = (me.x > 0 ? -1.0 : 1.0);
      if (me.y > 1.0) d.move = 0;
      return make_order(d);
    }
    if (!can_start) { d.move = (edge - me.x > 0.4 ? 1.0 : (edge - me.x < -0.4 ? -1.0 : 0.0)); clamp_stage_move(d.move); return make_order(d); }
  }

  if (!can_start) {
    // committed: keep moving a little, but stay on the stage
    d.move = (dx > 0 ? 1.0 : -1.0);
    if (std::fabs(dx) < 1.2) d.move = 0;
    clamp_stage_move(d.move);
    return make_order(d);
  }

  // ---- late game: the safe area is tiny and a respawn above the closing top line is fatal ----
  if (late_mode && bx < 3.0 && can_start && !target_off) {
    // final arena: the only safe column is the exact centre; hold it while fighting
    d.move = clampd(-me.x * 1.5, -1.0, 1.0);
    double fdx, fdy;
    aim_dir(me.x, me.y + C.body_h / 2.0, t.x, t.y + C.body_h / 2.0, fdx, fdy);
    if (std::fabs(dx) < 1.7 && std::fabs(dy) < 1.3) {
      d.skill = "grab"; d.has_dir = true; d.dx = fdx; d.dy = fdy;
      return make_order(d);
    }
    if (std::fabs(dx) < 2.4 && std::fabs(dy) < 1.6) {
      double hdt = 0;
      if (attack_would_hit(me, t, "attack", fdx, fdy, hdt)) {
        d.skill = "attack"; d.has_dir = true; d.dx = fdx; d.dy = fdy;
      }
    }
    return make_order(d);
  }
  if (late_mode) {
    double safe = std::max(0.9, bx - 3.0);
    if (std::fabs(me.x) > safe) {
      d.move = (me.x > 0 ? -1.0 : 1.0);
      double hdx, hdy;
      aim_dir(me.x, me.y + C.body_h / 2.0, t.x, t.y + C.body_h / 2.0, hdx, hdy);
      double hdt = 0;
      if (std::fabs(dx) < 2.6 && std::fabs(dy) < 1.6 && attack_would_hit(me, t, "attack", hdx, hdy, hdt)) {
        d.skill = "attack"; d.has_dir = true; d.dx = hdx; d.dy = hdy;
      }
      return make_order(d);
    }
  }

  // ---- choose an attack ----
  double adx = 0, ady = 0;
  aim_dir(me.x, me.y + C.body_h / 2.0, t.x, t.y + C.body_h / 2.0, adx, ady);
  bool tguard = is_guarding(t), tcounter = is_countering(t);
  int nearby_third = 0;
  for (int i = 0; i < (int)ps.size(); ++i) {
    if (i == seat || i == tp.idx || ps[i].out || ps[i].respawn > 0 || ps[i].invuln > 0) continue;
    if (std::fabs(ps[i].x - me.x) < 4.5 && std::fabs(ps[i].y - me.y) < 3.0) ++nearby_third;
  }
  bool cautious = (me.damage >= 65.0 && nearby_third > 0 && !(t.damage >= 95.0 || (t.stocks <= 1 && t.damage >= 80.0)));
  if (cautious && dist > 2.6) {
    // hurt and outnumbered: reposition toward center instead of committing
    d.move = (me.x > 0 ? -1.0 : 1.0);
    if (std::fabs(me.x) < 1.0) d.move = 0;
    return make_order(d);
  }

  // grab/counter beating: grab if close, hook if farther
  if (tguard || tcounter) {
    double gdx, gdy;
    aim_dir(me.x, me.y + C.body_h / 2.0, t.x, t.y + C.body_h / 2.0, gdx, gdy);
    if (std::fabs(dx) < 1.9 && std::fabs(dy) < 1.4) {
      d.skill = "grab"; d.has_dir = true; d.dx = gdx; d.dy = gdy;
      return make_order(d);
    }
    if (me.hook_cd == 0 && me.meter >= C.hook.cost + reserve + 1e-9 && dist > 2.5 && dist < 16.0) {
      double hx, hy;
      aim_dir(me.x, me.y + C.body_h / 2.0, pred_x_at(t, 6), pred_y_at(t, 6) + C.body_h / 2.0, hx, hy);
      d.skill = "hook"; d.has_dir = true; d.dx = hx; d.dy = hy;
      return make_order(d);
    }
    if (tguard) {
      // close in for a grab; a guard also stops a slam, so never swing into it
      if (dist > 1.55) d.move = (dx > 0 ? 1.0 : -1.0);
      if (me.grounded && dy > 1.3 && std::fabs(dx) < 4.0) d.jump = true;
      return make_order(d);
    }
    // counter stance: slam from directly above is safe and wins the exchange
    if (!me.grounded && me.meter >= C.slam.cost + reserve + 1e-9 && me.y > t.y + 0.8 && std::fabs(dx) < 2.6 &&
        slam_has_landing(me)) {
      d.skill = "slam"; d.has_dir = true; d.dx = 0; d.dy = -1;
      return make_order(d);
    }
    if (dist > 1.55) d.move = (dx > 0 ? 1.0 : -1.0);
    return make_order(d);
  }

  // slam from above (never into a guard, and only when a miss still lands on stage)
  if (!tguard && !me.grounded && me.meter >= C.slam.cost + reserve + 1e-9 && slam_would_hit(me, t) && me.y > t.y + 0.4 &&
      slam_has_landing(me) && (std::fabs(me.x) < 13.0 || me.y - t.y < 2.4)) {
    d.skill = "slam"; d.has_dir = true; d.dx = 0; d.dy = -1;
    return make_order(d);
  }
  // uppercut when the target is above and the swing will actually connect
  if (!tguard && !tcounter && me.meter >= C.uppercut.cost + reserve + 1e-9 && dy > 0.6 && std::fabs(dx) < 2.6 && dy < 3.6 &&
      me.y + 8.5 < bt - 1.0) {
    double ux, uy;
    aim_dir(me.x, me.y + C.body_h / 2.0, t.x, t.y + C.body_h / 2.0, ux, uy);
    double udt = 0;
    if (attack_would_hit(me, t, "uppercut", ux, uy, udt)) {
      d.skill = "uppercut"; d.has_dir = true; d.dx = ux; d.dy = uy;
      return make_order(d);
    }
  }
  // normal attack if predicted to connect
  double hitdt = 0;
  if (!tguard && !tcounter && attack_would_hit(me, t, "attack", adx, ady, hitdt)) {
    d.skill = "attack"; d.has_dir = true; d.dx = adx; d.dy = ady;
    return make_order(d);
  }
  // grab against a guard slightly farther away (grab has 6 windup but longer commit)
  if (!tguard && !tcounter && std::fabs(dx) < 1.7 && std::fabs(dy) < 1.2) {
    d.skill = "attack"; d.has_dir = true; d.dx = adx; d.dy = ady;
    return make_order(d);
  }
  // wave poke
  if (!tguard && !tcounter && me.meter >= C.wave.cost + reserve + 1e-9 && dist > 4.5 && dist < 20.0 && std::fabs(dy) < 3.5) {
    double wx, wy;
    if (wave_would_hit(me, t, wx, wy)) {
      d.skill = "wave"; d.has_dir = true; d.dx = wx; d.dy = wy;
      return make_order(d);
    }
  }

  // ---- approach / reposition ----
  double want = 1.9;
  double mv = 0;
  if (dx > want + 0.25) mv = 1;
  else if (dx < -want - 0.25) mv = -1;
  else if (std::fabs(dx) < 0.9) mv = (dx > 0 ? -0.6 : 0.6);
  // do not chase outward past the safe working area unless the target is nearly dead
  bool finishing = (t.damage >= 95.0) || (t.stocks <= 1 && t.damage >= 75.0);
  if (me.grounded && mv != 0 && (mv > 0) == (me.x > 0)) {
    double nx = me.x + mv * C.run_speed * C.decision_ticks;
    double cap = (me.damage >= 50.0 ? 9.0 : 10.6);
    if (late_mode) cap = std::max(0.8, bx - 3.0);
    if (!finishing && std::fabs(nx) > cap) mv = (me.x > 0 ? -0.35 : 0.35);
  }
  d.move = mv;
  if (must_center) {
    d.move = (me.x > 0 ? -1.0 : 1.0);
  }
  // vertical pursuit: jump toward targets above, drop toward targets below
  if (me.grounded && dy > 1.3 && std::fabs(dx) < 6.0) d.jump = true;
  if (me.grounded && me.y > 0.3 && dy < -0.5) d.drop = true;
  clamp_stage_move(d.move);
  return make_order(d);
}

int main() {
  return sm_step_worker::run([](const J &obs, J &memory) {
    return std::make_pair(policy(obs, memory), memory);
  });
}
