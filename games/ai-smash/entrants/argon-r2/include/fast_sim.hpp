#pragma once
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fast_sim {

using J = mini_json::Json;

enum Skill : int {
  SK_NONE = 0,
  SK_ATTACK = 1,
  SK_CHARGE = 2,
  SK_GUARD = 3,
  SK_BLINK = 4,
  SK_UPPERCUT = 5,
  SK_SLAM = 6,
  SK_WAVE = 7,
  SK_GRAB = 8,
  SK_COUNTER = 9,
  SK_HOOK = 10,
  SK_COUNT = 11
};

enum Phase : int {
  PH_IDLE = 0,
  PH_CHARGING = 1,
  PH_WINDUP = 2,
  PH_ACTIVE = 3,
  PH_RECOVER = 4
};

inline Skill parse_skill(const J &j) {
  if (j.is_null() || !j.is_string()) return SK_NONE;
  const std::string &s = j.as_string();
  if (s.empty()) return SK_NONE;
  switch (s[0]) {
    case 'a': return s == "attack" ? SK_ATTACK : SK_NONE;
    case 'b': return s == "blink" ? SK_BLINK : SK_NONE;
    case 'c': return s == "charge" ? SK_CHARGE : (s == "counter" ? SK_COUNTER : SK_NONE);
    case 'g': return s == "guard" ? SK_GUARD : (s == "grab" ? SK_GRAB : SK_NONE);
    case 'h': return s == "hook" ? SK_HOOK : SK_NONE;
    case 's': return s == "slam" ? SK_SLAM : SK_NONE;
    case 'u': return s == "uppercut" ? SK_UPPERCUT : SK_NONE;
    case 'w': return s == "wave" ? SK_WAVE : SK_NONE;
    default: return SK_NONE;
  }
}

inline const char *skill_str(Skill s) {
  switch (s) {
    case SK_ATTACK: return "attack";
    case SK_CHARGE: return "charge";
    case SK_GUARD: return "guard";
    case SK_BLINK: return "blink";
    case SK_UPPERCUT: return "uppercut";
    case SK_SLAM: return "slam";
    case SK_WAVE: return "wave";
    case SK_GRAB: return "grab";
    case SK_COUNTER: return "counter";
    case SK_HOOK: return "hook";
    default: return "";
  }
}

inline Phase parse_phase(const J &j) {
  if (j.is_null() || !j.is_string()) return PH_IDLE;
  const std::string &s = j.as_string();
  if (s == "idle") return PH_IDLE;
  if (s == "charging") return PH_CHARGING;
  if (s == "windup") return PH_WINDUP;
  if (s == "active") return PH_ACTIVE;
  if (s == "recover") return PH_RECOVER;
  return PH_IDLE;
}

struct MoveSpec {
  double cost = 0.0;
  int windup = 0;
  int active = 0;
  int recover = 0;
  double damage = 0.0;
  double power = 0.0;
  double offset = 0.0;
  double radius = 0.0;
  double lift = 0.0;
  double speed = 0.0;
  double rise = 0.0;
  int ttl = 0;
  // charge specific
  int max_ticks = 45;
  int auto_release_ticks = 75;
  double max_mult = 2.0;
  double move_factor = 0.25;
  double short_power_mult = 1.0;
  double short_power_full_fraction = 0.5;
  // slam specific
  int landing_hitstun_cap = -1;
  // guard specific
  int max_guard_ticks = 60;
  int lockout_ticks = 30;
  double pushback = 0.12;
  // counter specific
  double mult = 2.0;
  int spent_recover = 4;
  // hook specific
  double reel_speed = 0.9;
  double pull_speed = 0.3;
  int pull_ticks = 30;
  double stop = 1.6;
  int miss_cooldown_ticks = 150;
};

struct SolidBox {
  double x1, x2, top, bottom;
};

struct PlatBox {
  double x1, x2, y;
};

struct RulesConfig {
  bool loaded = false;
  int decision_ticks = 3;
  int max_turns = 1800;
  double factor = 10000.0;
  int stocks = 3;

  int n_solids = 0;
  SolidBox solids[4];
  int n_plats = 0;
  PlatBox plats[8];

  double blast_x = 26.0, blast_top = 24.0, blast_bottom = -16.0;
  int shrink_start_tick = 3000;
  double shrink_rate = 0.02, shrink_top_ratio = 0.9, shrink_min_x = 0.0, shrink_min_top = 1.0;
  double respawn_x = 0.0, respawn_y = 13.0;

  double half_w = 0.6, height = 1.8;
  double gravity = 0.035, max_fall = 0.8, run_speed = 0.32, ground_accel = 0.08, air_accel = 0.03;
  double ground_friction = 0.75, air_friction = 0.985, hit_friction = 0.97, bounce = 0.6;

  double jump_ground_speed = 0.62, jump_air_speed = 0.55;
  int jump_air_jumps = 1, drop_ticks = 10;

  double kb_base = 0.3, kb_scale = 1.1;
  int hitstun_base = 6, hitstun_max = 45;
  double hitstun_per_speed = 28.0;

  double meter_max = 1.0, meter_start = 1.0, meter_refill = 0.004;
  int gap_ticks = 4, respawn_ticks = 60, respawn_invuln_ticks = 90, kill_credit_ticks = 300;

  MoveSpec moves[SK_COUNT];

  void load_from_json(const J &r) {
    if (loaded) return;
    decision_ticks = r.at("decision_ticks").get<int>();
    max_turns = r.at("max_turns").get<int>();
    int prec = r.at("precision").get<int>();
    factor = 1.0;
    for (int i = 0; i < prec; ++i) factor *= 10.0;
    stocks = r.at("stocks").get<int>();

    const J &st = r.at("stage");
    n_solids = 0;
    for (const auto &s : st.at("solids").as_array()) {
      if (n_solids < 4) {
        solids[n_solids++] = {s.at("x1").as_double(), s.at("x2").as_double(), s.at("top").as_double(), s.at("bottom").as_double()};
      }
    }
    n_plats = 0;
    for (const auto &p : st.at("platforms").as_array()) {
      if (n_plats < 8) {
        plats[n_plats++] = {p.at("x1").as_double(), p.at("x2").as_double(), p.at("y").as_double()};
      }
    }
    const J &bl = st.at("blast");
    blast_x = bl.at("x").as_double();
    blast_top = bl.at("top").as_double();
    blast_bottom = bl.at("bottom").as_double();
    if (st.contains("blast_shrink")) {
      const J &bs = st.at("blast_shrink");
      shrink_start_tick = bs.at("start_tick").get<int>();
      shrink_rate = bs.at("rate").as_double();
      shrink_top_ratio = bs.at("top_ratio").as_double();
      shrink_min_x = bs.at("min_x").as_double();
      shrink_min_top = bs.at("min_top").as_double();
    }
    const auto &resp = st.at("respawn").as_array();
    respawn_x = resp[0].as_double();
    respawn_y = resp[1].as_double();

    const J &bd = r.at("body");
    half_w = bd.at("half_width").as_double();
    height = bd.at("height").as_double();

    const J &ph = r.at("physics");
    gravity = ph.at("gravity").as_double();
    max_fall = ph.at("max_fall").as_double();
    run_speed = ph.at("run_speed").as_double();
    ground_accel = ph.at("ground_accel").as_double();
    air_accel = ph.at("air_accel").as_double();
    ground_friction = ph.at("ground_friction").as_double();
    air_friction = ph.at("air_friction").as_double();
    hit_friction = ph.at("hit_friction").as_double();
    bounce = ph.at("bounce").as_double();

    const J &jm = r.at("jump");
    jump_ground_speed = jm.at("ground_speed").as_double();
    jump_air_speed = jm.at("air_speed").as_double();
    jump_air_jumps = jm.at("air_jumps").get<int>();
    drop_ticks = jm.at("drop_ticks").get<int>();

    const J &kb = r.at("knockback");
    kb_base = kb.at("base").as_double();
    kb_scale = kb.at("scale").as_double();
    hitstun_base = kb.at("hitstun_base").get<int>();
    hitstun_per_speed = kb.at("hitstun_per_speed").as_double();
    hitstun_max = kb.at("hitstun_max").get<int>();

    const J &mt = r.at("meter");
    meter_max = mt.at("max").as_double();
    meter_start = mt.at("start").as_double();
    meter_refill = mt.at("refill").as_double();

    gap_ticks = r.at("gap_ticks").get<int>();
    respawn_ticks = r.at("respawn_ticks").get<int>();
    respawn_invuln_ticks = r.at("respawn_invuln_ticks").get<int>();
    kill_credit_ticks = r.at("kill_credit_ticks").get<int>();

    const J &mvs = r.at("moves");
    auto load_mv = [&](Skill sk, const char *name) {
      const J &m = mvs.at(name);
      MoveSpec &ms = moves[sk];
      auto gd = [&](const char *k, double d = 0.0) { return m.contains(k) ? m.at(k).as_double() : d; };
      auto gi = [&](const char *k, int d = 0) { return m.contains(k) ? m.at(k).get<int>() : d; };
      ms.cost = gd("cost");
      ms.windup = gi("windup");
      ms.active = gi("active");
      ms.recover = gi("recover", 1);
      ms.damage = gd("damage");
      ms.power = gd("power");
      ms.offset = gd("offset");
      ms.radius = gd("radius");
      ms.lift = gd("lift");
      ms.speed = gd("speed");
      ms.rise = gd("rise");
      ms.ttl = gi("ttl");
      ms.max_ticks = gi("max_ticks", 45);
      ms.auto_release_ticks = gi("auto_release_ticks", 75);
      ms.max_mult = gd("max_mult", 2.0);
      ms.move_factor = gd("move_factor", 0.25);
      ms.short_power_mult = gd("short_power_mult", 1.0);
      ms.short_power_full_fraction = gd("short_power_full_fraction", 0.5);
      ms.landing_hitstun_cap = gi("landing_hitstun_cap", -1);
      ms.max_guard_ticks = gi("max_ticks", 60);
      ms.lockout_ticks = gi("lockout_ticks", 30);
      ms.pushback = gd("pushback", 0.12);
      ms.mult = gd("mult", 2.0);
      ms.spent_recover = gi("spent_recover", 4);
      ms.reel_speed = gd("reel_speed", 0.9);
      ms.pull_speed = gd("pull_speed", 0.3);
      ms.pull_ticks = gi("pull_ticks", 30);
      ms.stop = gd("stop", 1.6);
      ms.miss_cooldown_ticks = gi("miss_cooldown_ticks", 150);
    };
    load_mv(SK_ATTACK, "attack");
    load_mv(SK_CHARGE, "charge");
    load_mv(SK_GUARD, "guard");
    load_mv(SK_BLINK, "blink");
    load_mv(SK_UPPERCUT, "uppercut");
    load_mv(SK_SLAM, "slam");
    load_mv(SK_WAVE, "wave");
    load_mv(SK_GRAB, "grab");
    load_mv(SK_COUNTER, "counter");
    load_mv(SK_HOOK, "hook");

    loaded = true;
  }

  inline double rnd(double v) const { return std::round(v * factor) / factor; }

  inline void current_blast(int tick_no, double &bx, double &btop) const {
    bx = blast_x;
    btop = blast_top;
    if (tick_no > shrink_start_tick) {
      double closed = (tick_no - shrink_start_tick) * shrink_rate;
      bx = std::max(shrink_min_x, bx - closed);
      btop = std::max(shrink_min_top, btop - closed * shrink_top_ratio);
    }
  }

  inline bool in_solid(double x, double y) const {
    for (int i = 0; i < n_solids; ++i) {
      const auto &s = solids[i];
      if (x > s.x1 && x < s.x2 && y < s.top && y > s.bottom) return true;
    }
    return false;
  }
};

struct FastOrder {
  double move = 0.0;
  double dx = 0.0, dy = 0.0;
  bool jump = false;
  bool drop = false;
  bool has_dir = false;
  Skill skill = SK_NONE;

  J to_json() const {
    J o = J::object{};
    if ( std::fabs(move) > 1e-6 ) o["move"] = std::max(-1.0, std::min(1.0, move));
    if (jump) o["jump"] = true;
    if (drop) o["drop"] = true;
    if (skill != SK_NONE) o["skill"] = skill_str(skill);
    if (has_dir) o["dir"] = J::array{dx, dy};
    return o;
  }
};

struct FastFighter {
  double x = 0, y = 0, vx = 0, vy = 0, damage = 0, meter = 0, dx = 1, dy = 0, power = 1;
  int facing = 1, air_jumps = 0, stocks = 0, respawn = 0, invuln = 0, hitstun = 0, gap = 0, hook_cd = 0, drop = 0;
  int phase = PH_IDLE, phase_t = 0, charge = 0, hit_mask = 0, last_hit = -1, last_hit_tick = -1, pulled_by = -1, pull_t = 0;
  bool slam_landing = false;
  bool grounded = false, out = false, reeling = false;
  double hook_x = 0, hook_y = 0;
  Skill act = SK_NONE;
};

struct FastShot {
  int id = 0, owner = 0, ttl = 0;
  Skill kind = SK_NONE;
  double x = 0, y = 0, vx = 0, vy = 0, damage = 0, power = 0;
  bool dead = false;
};

struct FastHit {
  int a, t;
  Skill skill;
  double dx, dy, damage, power;
};

struct SimEvents {
  int my_kills = 0;
  int my_falls = 0;
  double dmg_dealt = 0.0;
  double dmg_taken = 0.0;
  int hits_landed = 0;
  int hits_taken = 0;
  int blocks_made = 0;
  int counters_made = 0;
  int hooks_landed = 0;
  int hooks_staged = 0;
  int grabs_landed = 0;
  int tags_made = 0;
  int got_hooked = 0;
};

struct FastState {
  int n = 4;
  int turn = 0;
  int tick_no = 0;
  int next_shot = 1;
  bool done = false;
  FastFighter fighters[4];
  int n_shots = 0;
  FastShot shots[16];
  int kills[4] = {0, 0, 0, 0};
  int out_turn[4] = {-1, -1, -1, -1};
  int out_tick[4] = {-1, -1, -1, -1};

  static inline void unit(double &x, double &y, double fx, double fy) {
    double l = std::sqrt(x * x + y * y);
    if (l < 1e-9) { x = fx; y = fy; return; }
    x /= l; y /= l;
  }

  inline bool untouchable(const FastFighter &t) const {
    return t.out || t.respawn > 0 || t.invuln > 0 || (t.act == SK_BLINK && t.phase == PH_ACTIVE);
  }
  inline bool countering(const FastFighter &t) const { return t.act == SK_COUNTER && t.phase == PH_ACTIVE; }
  inline bool guarding(const FastFighter &t) const { return t.act == SK_GUARD && t.phase == PH_ACTIVE; }
  inline bool melee(const FastFighter &f) const {
    return f.phase == PH_ACTIVE &&
           (f.act == SK_ATTACK || f.act == SK_CHARGE || f.act == SK_UPPERCUT || f.act == SK_SLAM || f.act == SK_GRAB);
  }

  void end_action(FastFighter &f, int gap) {
    f.act = SK_NONE; f.phase = PH_IDLE; f.phase_t = 0; f.charge = 0; f.hit_mask = 0; f.reeling = false;
    f.gap = gap;
  }

  void cancel_action(int i) {
    auto &f = fighters[i];
    for (int s = 0; s < n_shots; ++s) {
      if (shots[s].owner == i && shots[s].kind == SK_HOOK) shots[s].dead = true;
    }
    f.act = SK_NONE; f.phase = PH_IDLE; f.phase_t = 0; f.charge = 0; f.hit_mask = 0; f.reeling = false;
  }

  void purge_shots() {
    int w = 0;
    for (int r = 0; r < n_shots; ++r) {
      if (!shots[r].dead) {
        if (w != r) shots[w] = shots[r];
        ++w;
      }
    }
    n_shots = w;
  }

  void set_dir(FastFighter &f, const FastOrder &o) {
    if (!o.has_dir) return;
    double x = o.dx, y = o.dy;
    unit(x, y, f.facing, 0);
    f.dx = x; f.dy = y;
    if (x > 0.05) f.facing = 1;
    else if (x < -0.05) f.facing = -1;
  }

  void enter_active(int i, const RulesConfig &rc) {
    auto &f = fighters[i];
    const MoveSpec &m = rc.moves[f.act];
    f.phase = PH_ACTIVE; f.hit_mask = 0;
    f.phase_t = m.active;
    if (f.act == SK_UPPERCUT) {
      f.vy = m.rise; f.grounded = false; f.dx = 0; f.dy = 1;
    } else if (f.act == SK_SLAM) {
      f.vy = -m.speed; f.vx *= 0.3; f.dx = 0; f.dy = -1;
    } else if (f.act == SK_BLINK) {
      f.vx = f.dx * m.speed; f.vy = f.dy * m.speed; f.grounded = false;
    } else if (f.act == SK_WAVE || f.act == SK_HOOK) {
      if (n_shots < 16) {
        FastShot &b = shots[n_shots++];
        b.id = next_shot++; b.owner = i; b.kind = f.act;
        b.vx = f.dx * m.speed; b.vy = f.dy * m.speed; b.ttl = m.ttl;
        b.x = f.x + f.dx * (rc.half_w + 0.5);
        b.y = f.y + rc.height * 0.5 + f.dy * (rc.half_w + 0.5);
        b.damage = m.damage; b.power = m.power; b.dead = false;
      }
      if (f.act == SK_WAVE) {
        f.phase = PH_RECOVER; f.phase_t = m.recover;
      } else {
        f.phase_t = 1000000;
      }
    }
  }

  bool start(int i, const FastOrder &o, const RulesConfig &rc) {
    auto &f = fighters[i];
    Skill s = o.skill;
    if (s == SK_NONE) return false;
    const MoveSpec &m = rc.moves[s];
    if (f.meter + 1e-9 < m.cost) return false;
    if (s == SK_SLAM && f.grounded) return false;
    if (s == SK_HOOK && f.hook_cd > 0) return false;
    f.dx = f.facing; f.dy = 0;
    set_dir(f, o);
    f.act = s; f.meter -= m.cost; f.hit_mask = 0; f.charge = 0; f.power = 1.0;
    if (s == SK_CHARGE) {
      f.phase = PH_CHARGING; f.phase_t = 0;
    } else if (s == SK_GUARD) {
      f.phase = PH_ACTIVE; f.phase_t = m.max_guard_ticks;
    } else if (s == SK_BLINK) {
      enter_active(i, rc);
    } else {
      f.phase = PH_WINDUP; f.phase_t = m.windup;
    }
    return true;
  }

  void to_recover(int i, const RulesConfig &rc) {
    auto &f = fighters[i];
    if (f.act == SK_BLINK) { f.vx *= 0.2; f.vy *= 0.2; }
    f.reeling = false;
    f.phase = PH_RECOVER;
    f.phase_t = std::max(1, rc.moves[f.act].recover);
  }

  void release_charge(int i, const RulesConfig &rc) {
    auto &f = fighters[i];
    const MoveSpec &m = rc.moves[SK_CHARGE];
    double t = std::min(1.0, double(f.charge) / m.max_ticks);
    f.power = 1.0 + (m.max_mult - 1.0) * t;
    f.phase = PH_WINDUP; f.phase_t = m.windup;
  }

  void advance(int i, const RulesConfig &rc) {
    auto &f = fighters[i];
    if (f.phase == PH_CHARGING) {
      ++f.charge;
      if (f.charge >= rc.moves[SK_CHARGE].auto_release_ticks) release_charge(i, rc);
      return;
    }
    if (f.phase == PH_WINDUP) {
      if (--f.phase_t <= 0) enter_active(i, rc);
      return;
    }
    if (f.phase == PH_ACTIVE) {
      if (f.act == SK_GUARD) {
        if (--f.phase_t <= 0) end_action(f, rc.moves[SK_GUARD].lockout_ticks);
        return;
      }
      if (--f.phase_t <= 0) to_recover(i, rc);
      return;
    }
    if (f.phase == PH_RECOVER && --f.phase_t <= 0) end_action(f, rc.gap_ticks);
  }

  void reappear(int i, const RulesConfig &rc) {
    auto &f = fighters[i];
    f.x = rc.respawn_x + (i - (n - 1) / 2.0) * 1.5;
    f.y = rc.respawn_y;
    f.vx = 0; f.vy = 0; f.grounded = false; f.drop = 0;
    f.air_jumps = rc.jump_air_jumps;
    f.invuln = rc.respawn_invuln_ticks;
  }

  void do_jump(int i, const RulesConfig &rc) {
    auto &f = fighters[i];
    if (f.grounded) {
      f.vy = rc.jump_ground_speed; f.grounded = false;
    } else if (f.air_jumps > 0) {
      --f.air_jumps; f.vy = rc.jump_air_speed;
    }
  }

  void drive(int i, const FastOrder &o, const RulesConfig &rc) {
    auto &f = fighters[i];
    if (f.act == SK_GUARD) {
      if (f.grounded) f.vx = 0;
      return;
    }
    if ((f.act == SK_BLINK && f.phase == PH_ACTIVE) || f.reeling) return;
    bool locked = (f.act != SK_NONE) && (f.act != SK_CHARGE);
    if (locked) {
      if (f.grounded) f.vx *= rc.ground_friction;
      return;
    }
    double mv = std::max(-1.0, std::min(1.0, o.move));
    double want = mv * rc.run_speed;
    if (f.act == SK_CHARGE) want *= rc.moves[SK_CHARGE].move_factor;
    double a = f.grounded ? rc.ground_accel : rc.air_accel;
    f.vx += std::max(-a, std::min(a, want - f.vx));
    if (f.act == SK_NONE) {
      if (mv > 0.05) f.facing = 1;
      else if (mv < -0.05) f.facing = -1;
    }
  }

  void control(int i, const FastOrder &o, bool first, const RulesConfig &rc) {
    auto &f = fighters[i];
    if (f.respawn > 0) {
      if (--f.respawn == 0) reappear(i, rc);
      return;
    }
    if (f.invuln > 0) --f.invuln;
    if (f.hook_cd > 0) --f.hook_cd;
    if (f.drop > 0) --f.drop;
    f.meter = std::min(rc.meter_max, f.meter + rc.meter_refill);
    if (f.pull_t > 0) return;
    if (f.hitstun > 0) {
      if (--f.hitstun == 0) f.gap = 0;
      return;
    }
    bool released = false;
    if (f.act == SK_GUARD && f.phase == PH_ACTIVE && o.skill != SK_GUARD) {
      end_action(f, rc.gap_ticks);
    } else if (f.act == SK_CHARGE && f.phase == PH_CHARGING) {
      set_dir(f, o);
      if (o.skill == SK_GUARD || o.skill == SK_BLINK) {
        end_action(f, 0);
      } else if (o.skill != SK_CHARGE) {
        release_charge(i, rc);
        released = true;
      }
    }
    if (f.act == SK_NONE) {
      if (f.gap > 0) --f.gap;
      else start(i, o, rc);
    } else if (!released) {
      advance(i, rc);
    }
    if (first && o.jump && (f.act == SK_NONE || f.act == SK_CHARGE)) do_jump(i, rc);
    if (o.drop && f.grounded) f.drop = rc.drop_ticks;
    drive(i, o, rc);
  }

  bool overlaps(const FastFighter &f, const SolidBox &s, const RulesConfig &rc) const {
    return f.x + rc.half_w > s.x1 && f.x - rc.half_w < s.x2 && f.y < s.top && f.y + rc.height > s.bottom;
  }

  void collide_x(FastFighter &f, const RulesConfig &rc) {
    for (int k = 0; k < rc.n_solids; ++k) {
      const auto &s = rc.solids[k];
      if (!overlaps(f, s, rc)) continue;
      f.x = f.vx > 0 ? s.x1 - rc.half_w : s.x2 + rc.half_w;
      f.vx = f.hitstun > 0 ? -f.vx * rc.bounce : 0;
    }
  }

  void land(int i, double y, const RulesConfig &rc) {
    auto &f = fighters[i];
    f.y = y; f.vy = 0; f.grounded = true;
    if (f.slam_landing) {
      int cap = rc.moves[SK_SLAM].landing_hitstun_cap;
      if (cap >= 0 && f.hitstun > cap) {
        f.hitstun = cap;
      }
      f.slam_landing = false;
    }
    f.air_jumps = rc.jump_air_jumps;
    if ((f.act == SK_SLAM || f.act == SK_UPPERCUT) && f.phase == PH_ACTIVE) to_recover(i, rc);
  }

  void collide_y(int i, double foot, const RulesConfig &rc) {
    auto &f = fighters[i];
    for (int k = 0; k < rc.n_solids; ++k) {
      const auto &s = rc.solids[k];
      if (!overlaps(f, s, rc)) continue;
      if (f.vy <= 0) land(i, s.top, rc);
      else {
        f.y = s.bottom - rc.height;
        f.vy = f.hitstun > 0 ? -f.vy * rc.bounce : 0;
      }
    }
    if (f.vy <= 0 && f.drop == 0 && !f.grounded) {
      for (int k = 0; k < rc.n_plats; ++k) {
        const auto &p = rc.plats[k];
        if (foot >= p.y && f.y <= p.y && f.x + rc.half_w > p.x1 && f.x - rc.half_w < p.x2) {
          land(i, p.y, rc);
          break;
        }
      }
    }
  }

  void motion(int i, const RulesConfig &rc) {
    auto &f = fighters[i];
    const MoveSpec &hook = rc.moves[SK_HOOK];
    if (f.reeling) {
      double dx = f.hook_x - f.x, dy = f.hook_y - (f.y + rc.height * 0.5), stop = hook.stop;
      if (dx * dx + dy * dy <= stop * stop) {
        f.vx = 0; f.vy = 0.2; to_recover(i, rc);
      } else {
        unit(dx, dy, 0, 1);
        f.vx = dx * hook.reel_speed; f.vy = dy * hook.reel_speed;
      }
    }
    if (f.pull_t > 0) {
      auto &a = fighters[f.pulled_by];
      double dx = a.x - f.x, dy = a.y - f.y, stop = hook.stop;
      --f.pull_t;
      if (dx * dx + dy * dy <= stop * stop || a.out || a.respawn > 0) {
        f.vx = 0; f.vy = 0; f.pull_t = 0; f.pulled_by = -1;
      } else {
        unit(dx, dy, 1, 0);
        f.vx = dx * hook.pull_speed; f.vy = dy * hook.pull_speed;
      }
    }
    bool floating = (f.act == SK_BLINK && f.phase == PH_ACTIVE) || f.reeling || f.pull_t > 0;
    if (!floating) {
      f.vy = std::max(f.vy - rc.gravity, f.hitstun > 0 ? -2.0 : -rc.max_fall);
      if (f.hitstun > 0) {
        f.vx *= rc.hit_friction; f.vy *= rc.hit_friction;
      } else if (!f.grounded) {
        f.vx *= rc.air_friction;
      }
    }
    f.x += f.vx;
    collide_x(f, rc);
    double foot = f.y;
    f.y += f.vy;
    f.grounded = false;
    collide_y(i, foot, rc);
  }

  void launch(int a, int t, double dx, double dy, double damage, double power, Skill sk, const RulesConfig &rc, int my_seat, SimEvents *ev) {
    auto &f = fighters[t];
    unit(dx, dy, fighters[a].x <= f.x ? 1 : -1, 0);
    if (ev) {
      if (a == my_seat && t != my_seat) {
        ev->dmg_dealt += damage;
        ev->hits_landed += 1;
        if (sk == SK_GRAB) ev->grabs_landed += 1;
        if (f.last_hit != my_seat || (tick_no - f.last_hit_tick > 120)) ev->tags_made += 1;
      } else if (t == my_seat) {
        ev->dmg_taken += damage;
        ev->hits_taken += 1;
      }
    }
    f.damage += damage;
    double speed = (rc.kb_base + rc.kb_scale * f.damage / 100.0) * power;
    cancel_action(t);
    f.pull_t = 0; f.pulled_by = -1;
    f.vx = dx * speed; f.vy = dy * speed; f.grounded = false;
    f.slam_landing = (sk == SK_SLAM && rc.moves[SK_SLAM].landing_hitstun_cap >= 0);
    f.hitstun = std::min(rc.hitstun_max, rc.hitstun_base + int(speed * rc.hitstun_per_speed));
    f.gap = 0;
    f.last_hit = a; f.last_hit_tick = tick_no;
  }

  void counter_back(int t, int a, double damage, double power, const RulesConfig &rc, int my_seat, SimEvents *ev) {
    auto &target = fighters[t];
    const MoveSpec &m = rc.moves[SK_COUNTER];
    target.phase = PH_RECOVER; target.phase_t = m.spent_recover;
    if (ev && t == my_seat) ev->counters_made += 1;
    launch(t, a, fighters[a].x - target.x, 0.4, damage * m.mult, power * m.mult, SK_COUNTER, rc, my_seat, ev);
  }

  void strike(const FastHit &h, const RulesConfig &rc, int my_seat, SimEvents *ev) {
    auto &t = fighters[h.t];
    if (untouchable(t)) return;
    if (h.skill != SK_GRAB) {
      if (h.skill != SK_SLAM && countering(t)) {
        counter_back(h.t, h.a, h.damage, h.power, rc, my_seat, ev);
        return;
      }
      if (guarding(t)) {
        t.vx = (fighters[h.a].x <= t.x ? 1 : -1) * rc.moves[SK_GUARD].pushback;
        if (ev && h.t == my_seat) ev->blocks_made += 1;
        return;
      }
    }
    launch(h.a, h.t, h.dx, h.dy, h.damage, h.power, h.skill, rc, my_seat, ev);
  }

  void resolve_melee(const RulesConfig &rc, int my_seat, SimEvents *ev) {
    FastHit hits[16];
    int n_hits = 0;
    double centre = rc.height * 0.5;
    for (int a = 0; a < n; ++a) {
      auto &f = fighters[a];
      if (f.out || f.respawn > 0 || !melee(f)) continue;
      const MoveSpec &m = rc.moves[f.act];
      double ax = f.x + f.dx * m.offset, ay = f.y + centre + f.dy * m.offset;
      double reach = m.radius + centre;
      for (int t = 0; t < n; ++t) {
        if (t == a || ((f.hit_mask >> t) & 1)) continue;
        const auto &v = fighters[t];
        double dx = v.x - ax, dy = v.y + centre - ay;
        if (dx * dx + dy * dy > reach * reach || untouchable(v)) continue;
        f.hit_mask |= (1 << t);
        double scale = f.act == SK_CHARGE ? f.power : 1.0;
        double launch_scale = scale;
        if (f.act == SK_CHARGE && m.short_power_mult != 1.0) {
          double hold_fraction = double(f.charge) / double(m.max_ticks);
          double recovery = std::min(1.0, hold_fraction / m.short_power_full_fraction);
          launch_scale *= 1.0 + (m.short_power_mult - 1.0) * (1.0 - recovery);
        }
        if (n_hits < 16) {
          hits[n_hits++] = FastHit{a, t, f.act, f.dx, f.dy + m.lift, m.damage * scale, m.power * launch_scale};
        }
      }
    }
    for (int i = 0; i < n_hits; ++i) strike(hits[i], rc, my_seat, ev);
  }

  void attach(const FastShot &s, const RulesConfig &rc, int my_seat, SimEvents *ev) {
    auto &f = fighters[s.owner];
    if (f.act != SK_HOOK) return;
    f.reeling = true; f.hook_x = s.x; f.hook_y = s.y;
    f.phase = PH_ACTIVE; f.phase_t = rc.moves[SK_HOOK].pull_ticks;
    if (ev && s.owner == my_seat) ev->hooks_staged += 1;
  }

  void hook_target(const FastShot &s, int t, const RulesConfig &rc, int my_seat, SimEvents *ev) {
    auto &v = fighters[t];
    const MoveSpec &m = rc.moves[SK_HOOK];
    cancel_action(t);
    if (ev) {
      if (s.owner == my_seat && t != my_seat) {
        ev->dmg_dealt += m.damage;
        ev->hooks_landed += 1;
        if (v.last_hit != my_seat || (tick_no - v.last_hit_tick > 120)) ev->tags_made += 1;
      } else if (t == my_seat) {
        ev->dmg_taken += m.damage;
        ev->hits_taken += 1;
        ev->got_hooked += 1;
      }
    }
    v.damage += m.damage;
    v.slam_landing = false;
    v.pulled_by = s.owner; v.pull_t = m.pull_ticks; v.hitstun = 0; v.grounded = false;
    v.last_hit = s.owner; v.last_hit_tick = tick_no;
    if (fighters[s.owner].act == SK_HOOK) to_recover(s.owner, rc);
  }

  void hook_miss(int owner, const RulesConfig &rc) {
    auto &f = fighters[owner];
    f.hook_cd = rc.moves[SK_HOOK].miss_cooldown_ticks;
    if (f.act == SK_HOOK) to_recover(owner, rc);
  }

  void move_shots(const RulesConfig &rc, int my_seat, SimEvents *ev) {
    double centre = rc.height * 0.5;
    for (int idx = 0; idx < n_shots; ++idx) {
      auto &s = shots[idx];
      if (s.dead) continue;
      s.x += s.vx; s.y += s.vy; --s.ttl;
      if (rc.in_solid(s.x, s.y)) {
        if (s.kind == SK_HOOK) attach(s, rc, my_seat, ev);
        s.dead = true;
        continue;
      }
      const MoveSpec &m = rc.moves[s.kind];
      double reach = m.radius + centre;
      for (int t = 0; t < n && !s.dead; ++t) {
        if (t == s.owner) continue;
        auto &v = fighters[t];
        double dx = v.x - s.x, dy = v.y + centre - s.y;
        if (dx * dx + dy * dy > reach * reach || untouchable(v)) continue;
        if (s.kind == SK_HOOK) {
          hook_target(s, t, rc, my_seat, ev);
          s.dead = true;
          break;
        }
        if (countering(v)) {
          double mult = rc.moves[SK_COUNTER].mult;
          v.phase = PH_RECOVER; v.phase_t = rc.moves[SK_COUNTER].spent_recover;
          if (ev && t == my_seat) ev->counters_made += 1;
          s.owner = t; s.vx = -s.vx; s.vy = -s.vy; s.damage *= mult; s.power *= mult; s.ttl = m.ttl;
          break;
        }
        if (guarding(v)) {
          if (ev && t == my_seat) ev->blocks_made += 1;
          s.dead = true;
          break;
        }
        launch(s.owner, t, s.vx, s.vy + 0.2, s.damage, s.power, SK_WAVE, rc, my_seat, ev);
        s.dead = true;
      }
      if (!s.dead && s.ttl <= 0) {
        if (s.kind == SK_HOOK) hook_miss(s.owner, rc);
        s.dead = true;
      }
    }
  }

  void eliminate(int i) {
    auto &f = fighters[i];
    if (f.out) return;
    cancel_action(i);
    f.out = true; f.stocks = 0;
    out_turn[i] = turn; out_tick[i] = tick_no;
  }

  void blast_check(int i, const RulesConfig &rc, int my_seat, SimEvents *ev) {
    auto &f = fighters[i];
    double bx, btop;
    rc.current_blast(tick_no, bx, btop);
    if (std::fabs(f.x) <= bx && f.y <= btop && f.y >= rc.blast_bottom) return;
    int by = (f.last_hit >= 0 && f.last_hit != i && (tick_no - f.last_hit_tick <= rc.kill_credit_ticks)) ? f.last_hit : -1;
    if (by >= 0) {
      ++kills[by];
      if (ev && by == my_seat) ev->my_kills += 1;
    }
    if (ev && i == my_seat) ev->my_falls += 1;
    --f.stocks;
    cancel_action(i);
    f.slam_landing = false;
    f.damage = 0; f.vx = 0; f.vy = 0; f.hitstun = 0; f.gap = 0; f.pull_t = 0; f.pulled_by = -1; f.last_hit = -1; f.last_hit_tick = -1;
    if (f.stocks <= 0) {
      eliminate(i);
      return;
    }
    f.respawn = rc.respawn_ticks;
  }

  void round_state(const RulesConfig &rc) {
    for (int i = 0; i < n; ++i) {
      auto &f = fighters[i];
      f.x = rc.rnd(f.x); f.y = rc.rnd(f.y); f.vx = rc.rnd(f.vx); f.vy = rc.rnd(f.vy);
      f.meter = rc.rnd(f.meter); f.damage = rc.rnd(f.damage);
    }
    for (int i = 0; i < n_shots; ++i) {
      shots[i].x = rc.rnd(shots[i].x); shots[i].y = rc.rnd(shots[i].y);
    }
  }

  void check_done(const RulesConfig &rc) {
    int alive_cnt = 0;
    for (int i = 0; i < n; ++i) if (!fighters[i].out) ++alive_cnt;
    if (alive_cnt <= 1 || turn >= rc.max_turns) done = true;
  }

  void step_tick(const FastOrder orders[4], bool first, const RulesConfig &rc, int my_seat = -1, SimEvents *ev = nullptr) {
    ++tick_no;
    for (int i = 0; i < n; ++i) if (!fighters[i].out) control(i, orders[i], first, rc);
    for (int i = 0; i < n; ++i) if (!fighters[i].out && fighters[i].respawn == 0) motion(i, rc);
    move_shots(rc, my_seat, ev);
    resolve_melee(rc, my_seat, ev);
    for (int i = 0; i < n; ++i) if (!fighters[i].out && fighters[i].respawn == 0) blast_check(i, rc, my_seat, ev);
    purge_shots();
    round_state(rc);
    check_done(rc);
  }

  void step_turn(const FastOrder orders[4], const RulesConfig &rc, int my_seat = -1, SimEvents *ev = nullptr) {
    for (int k = 0; k < rc.decision_ticks && !done; ++k) {
      step_tick(orders, k == 0, rc, my_seat, ev);
    }
    ++turn;
    check_done(rc);
  }
};

}  // namespace fast_sim
