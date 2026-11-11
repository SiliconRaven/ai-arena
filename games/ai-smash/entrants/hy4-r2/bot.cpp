// Platform brawl bot: exact-physics forward simulation (native/include/engine.hpp) drives
// both short combat rollouts and a longer recovery planner. Only the C++ standard library.
#include "include/step_worker.hpp"
#include "include/engine.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>

using J = mini_json::Json;
using smash::Engine;
using smash::Order;
using smash::Fighter;
using smash::arr;
using smash::ACTIVE;
using smash::CHARGING;
using smash::WINDUP;
using smash::RECOVER;

static inline double D(const J &j, const char *k) { return j.at(k).as_double(); }
static inline int I(const J &j, const char *k) { return j.at(k).get<int>(); }
static inline bool Bo(const J &j, const char *k) { return j.at(k).as_bool(); }
static inline const std::string &St(const J &j, const char *k) { return j.at(k).as_string(); }
static inline double cl(double v, double a, double b) { return std::max(a, std::min(b, v)); }
static inline double sg(double v) { return v > 1e-9 ? 1.0 : (v < -1e-9 ? -1.0 : 0.0); }

// ------------------------------------------------------------------ config (read once from obs.rules)
struct Cfg {
  bool init = false;
  double bx = 26, bt = 24, bb = -16, sx = 13;
  double ss = 3000, sr = 0.02, str = 0.9, mnx = 0, mnt = 1;
  double run = 0.32, mm = 1.0, mr = 0.004, jg = 0.62, ja = 0.55;
  double c_blink = 0.2, c_up = 0.25, c_slam = 0.2, c_wave = 0.6, c_grab = 0.3, c_counter = 0.3, c_hook = 0.4;
  double a_off = 1.3, a_rad = 0.9, g_off = 1.2, g_rad = 0.9, u_rad = 1.1, s_rad = 3.0;
  double lift_a = 0.5, lift_g = 0.3;
  int gap = 4, max_t = 45;
};
static Cfg C;

static void init(const J &obs) {
  if (C.init) return;
  C.init = true;
  const J &r = obs.at("rules");
  const J &st = r.at("stage");
  const J &bl = st.at("blast");
  C.bx = D(bl, "x"); C.bt = D(bl, "top"); C.bb = D(bl, "bottom");
  C.sx = D(st.at("solids").at(0), "x2");
  const J &bs = st.at("blast_shrink");
  C.ss = D(bs, "start_tick"); C.sr = D(bs, "rate"); C.str = D(bs, "top_ratio");
  C.mnx = D(bs, "min_x"); C.mnt = D(bs, "min_top");
  C.run = D(r.at("physics"), "run_speed");
  const J &mt = r.at("meter"); C.mm = D(mt, "max"); C.mr = D(mt, "refill");
  const J &jp = r.at("jump"); C.jg = D(jp, "ground_speed"); C.ja = D(jp, "air_speed");
  const J &mv = r.at("moves");
  C.c_blink = D(mv.at("blink"), "cost");
  C.c_up = D(mv.at("uppercut"), "cost");
  C.c_slam = D(mv.at("slam"), "cost");
  C.c_wave = D(mv.at("wave"), "cost");
  C.c_grab = D(mv.at("grab"), "cost");
  C.c_counter = D(mv.at("counter"), "cost");
  C.c_hook = D(mv.at("hook"), "cost");
  C.a_off = D(mv.at("attack"), "offset"); C.a_rad = D(mv.at("attack"), "radius"); C.lift_a = D(mv.at("attack"), "lift");
  C.g_off = D(mv.at("grab"), "offset"); C.g_rad = D(mv.at("grab"), "radius"); C.lift_g = D(mv.at("grab"), "lift");
  C.u_rad = D(mv.at("uppercut"), "radius"); C.s_rad = D(mv.at("slam"), "radius");
  C.gap = I(r, "gap_ticks"); C.max_t = I(mv.at("charge"), "max_ticks");
}
static double boundX(int tick) {
  double bx = C.bx;
  if (tick > C.ss) bx = std::max(C.mnx, bx - (tick - C.ss) * C.sr);
  return bx;
}
static double boundTop(int tick) {
  double bt = C.bt;
  if (tick > C.ss) bt = std::max(C.mnt, bt - (tick - C.ss) * C.sr * C.str);
  return bt;
}

// ------------------------------------------------------------------ observation parsing
struct F {
  double x = 0, y = 0, vx = 0, vy = 0, dmg = 0, meter = 0, dx = 1, dy = 0;
  int facing = 1, air_jumps = 0, stocks = 0, respawn = 0, invuln = 0, hitstun = 0, gap = 0,
      hook_cd = 0, charge = 0, phase_t = 0, phase = 0;
  bool grounded = false, out = false;
  std::string act;
};
static F parse(const J &p) {
  F f;
  f.x = D(p, "x"); f.y = D(p, "y"); f.vx = D(p, "vx"); f.vy = D(p, "vy");
  f.dmg = D(p, "damage"); f.meter = D(p, "meter");
  const J &d = p.at("dir").as_array(); f.dx = d[0].as_double(); f.dy = d[1].as_double();
  f.facing = I(p, "facing"); f.air_jumps = I(p, "air_jumps"); f.stocks = I(p, "stocks");
  f.respawn = I(p, "respawn"); f.invuln = I(p, "invuln"); f.hitstun = I(p, "hitstun");
  f.gap = I(p, "gap"); f.hook_cd = I(p, "hook_cd"); f.charge = I(p, "charge");
  f.phase_t = I(p, "phase_ticks"); f.grounded = Bo(p, "grounded"); f.out = Bo(p, "out");
  const std::string ph = St(p, "phase");
  f.phase = ph == "idle" ? 0 : ph == "charging" ? 1 : ph == "windup" ? 2 : ph == "active" ? 3 : 4;
  f.act = p.at("act").is_null() ? std::string() : St(p, "act");
  return f;
}
static J make_subs(int n) { J a = arr(); for (int i = 0; i < n; ++i) a.push_back(nullptr); return a; }

// Reconstruct a full engine state from the (partial) observation.  Hidden fields get safe defaults.
static J build_state(const J &obs) {
  const auto &ps = obs.at("players").as_array();
  int n = (int)ps.size();
  J fs = arr();
  for (int i = 0; i < n; ++i) {
    F f = parse(ps[i]);
    double power = (f.act == "charge") ? 1.0 + std::min(1.0, (double)f.charge / std::max(1, C.max_t)) : 1.0;
    fs.push_back(J{{"x", f.x}, {"y", f.y}, {"vx", f.vx}, {"vy", f.vy}, {"damage", f.dmg}, {"meter", f.meter},
                   {"dx", f.dx}, {"dy", f.dy}, {"power", power}, {"facing", f.facing}, {"air_jumps", f.air_jumps},
                   {"stocks", f.stocks}, {"respawn", f.respawn}, {"invuln", f.invuln}, {"hitstun", f.hitstun},
                   {"gap", f.gap}, {"hook_cd", f.hook_cd}, {"drop", 0}, {"phase", f.phase}, {"phase_t", f.phase_t},
                   {"charge", f.charge}, {"hit_mask", 0}, {"last_hit", -1}, {"last_hit_tick", -1},
                   {"pulled_by", -1}, {"pull_t", 0}, {"grounded", f.grounded}, {"out", f.out},
                   {"reeling", false}, {"hook_x", 0.0}, {"hook_y", 0.0}, {"act", f.act}});
  }
  J ss = arr();
  int maxid = 0;
  for (const J &s : obs.at("shots").as_array()) {
    int id = I(s, "id"); maxid = std::max(maxid, id);
    std::string k = St(s, "kind");
    double dmg = k == "wave" ? 6.0 : 3.0, pw = k == "wave" ? 0.55 : 0.0;
    ss.push_back(J{{"id", s.at("id")}, {"owner", s.at("owner")}, {"ttl", s.at("ttl")}, {"kind", k},
                   {"x", s.at("x")}, {"y", s.at("y")}, {"vx", s.at("vx")}, {"vy", s.at("vy")},
                   {"damage", dmg}, {"power", pw}});
  }
  J kills = arr(), ot = arr(), oti = arr(), pe = arr();
  for (int i = 0; i < n; ++i) { kills.push_back(0); ot.push_back(-1); oti.push_back(-1); pe.push_back(0); }
  return J{{"turn", I(obs, "turn") - 1}, {"tick", I(obs, "tick")}, {"next_shot", maxid + 1},
           {"done", false}, {"fighters", fs}, {"shots", ss}, {"kills", kills}, {"out_turn", ot},
           {"out_tick", oti}, {"program_errors", pe}};
}

// ------------------------------------------------------------------ order helpers
static Order mk(double move = 0, bool jump = false, const std::string &skill = "",
                bool hasdir = false, double dx = 0, double dy = 0, bool drop = false) {
  Order o;
  o.move = move; o.jump = jump; o.skill = skill; o.has_dir = hasdir; o.dx = dx; o.dy = dy; o.drop = drop;
  return o;
}
static J toOrder(const Order &o) {
  J r = J::object{};
  if (std::fabs(o.move) > 1e-9) r["move"] = cl(o.move, -1.0, 1.0);
  if (o.jump) r["jump"] = true;
  if (o.drop) r["drop"] = true;
  if (!o.skill.empty()) r["skill"] = o.skill;
  if (o.has_dir) r["dir"] = J::array{o.dx, o.dy};
  return r;
}

// ------------------------------------------------------------------ opponent / continuation model
// Aggressive homing model; nearby fighters target us (conservative for our own defence).
// model 0: ordinary attacks + occasional waves + charging foes keep charging.
// model 1: foes that are charging RELEASE the charge (the big damage threat that kills us).
// model 2: close idle foes GRAB (unblockable by guard/counter) and charging foes release.
static std::vector<Order> predict(const Engine &e, int seat, bool allowNew, int model) {
  int n = e.n;
  std::vector<Order> o(n);
  const Fighter &us = e.fighters[seat];
  for (int i = 0; i < n; ++i) {
    const Fighter &f = e.fighters[i];
    if (f.out || f.respawn > 0) continue;
    int tgt = -1;
    double dud = (us.x - f.x) * (us.x - f.x) + (us.y - f.y) * (us.y - f.y);
    if (dud < 16.0) tgt = seat;  // close to us => come at us
    if (tgt < 0) {
      double bd = 1e18;
      for (int j = 0; j < n; ++j) {
        if (j == i) continue;
        const Fighter &h = e.fighters[j];
        if (h.out || h.respawn > 0) continue;
        double dx = h.x - f.x, dy = h.y - f.y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; tgt = j; }
      }
    }
    if (tgt < 0) continue;
    const Fighter &t = e.fighters[tgt];
    double dx = t.x - f.x, dy = t.y - f.y, dist = std::sqrt(dx * dx + dy * dy);
    o[i].move = dist > 1.7 ? sg(dx) : 0.0;
    if (f.act == "guard" && f.phase == ACTIVE) o[i].skill = "guard";
    else if (f.act == "charge" && f.phase == CHARGING) {
      o[i].skill = (model == 1 || model == 2) ? "" : "charge";  // release => charge attack lands
    } else if (f.act.empty() && allowNew && f.gap == 0 && f.hitstun == 0 && dist < 3.0) {
      o[i].skill = (model == 2) ? "grab" : "attack";
      o[i].has_dir = true; o[i].dx = dx; o[i].dy = dy;
    } else if (f.act.empty() && allowNew && model == 0 && f.gap == 0 && f.hitstun == 0 &&
               dist >= 3.0 && dist < 9.0 && f.meter >= 0.6) {
      o[i].skill = "wave"; o[i].has_dir = true; o[i].dx = dx; o[i].dy = dy;
    }
    if (dy > 2.0 && f.grounded && f.act.empty()) o[i].jump = true;
  }
  return o;
}
// Our neutral continuation during a combat rollout: keep guard/charge, otherwise let the
// current action resolve and drift toward the target.
static Order seat_cont(const Engine &e, int seat) {
  Order o;
  const Fighter &f = e.fighters[seat];
  if (f.out || f.respawn > 0) return o;
  int tgt = -1; double bd = 1e18;
  for (int j = 0; j < e.n; ++j) {
    if (j == seat) continue;
    const Fighter &h = e.fighters[j];
    if (h.out || h.respawn > 0) continue;
    double dx = h.x - f.x, dy = h.y - f.y, d = dx * dx + dy * dy;
    if (d < bd) { bd = d; tgt = j; }
  }
  if (tgt >= 0) {
    const Fighter &t = e.fighters[tgt];
    double dx = t.x - f.x, dist = std::sqrt(bd);
    o.move = (dist > 1.8 && std::fabs(f.x) < 10.0) ? sg(dx) : 0.0;
  }
  if (f.act == "guard" && f.phase == ACTIVE) o.skill = "guard";
  else if (f.act == "charge" && f.phase == CHARGING) o.skill = "charge";
  return o;
}

// ------------------------------------------------------------------ combat evaluation
static double evaluate(const Engine &e, int seat) {
  double s = 0;
  for (const J &ev : e.events.as_array()) {
    const std::string &ty = St(ev, "type");
    if (ty == "hit") {
      int a = I(ev, "attacker"), t = I(ev, "target");
      double dmg = D(ev, "damage"), tot = D(ev, "total");
      if (t == seat) {
        s -= 2.6 * dmg * (1.0 + tot / 80.0);
        if (tot > 55.0) s -= 1.5 * (tot - 55.0);  // one more hit at this damage can kill us
      }
      if (a == seat) s += 3.0 * dmg * (1.0 + tot / 100.0);
    } else if (ty == "fall") {
      int sf = I(ev, "seat");
      if (sf == seat) s -= 400;
      else {
        s += 50;
        if (ev.contains("by") && !ev.at("by").is_null() && I(ev, "by") == seat) s += 200;
      }
    } else if (ty == "block") {
      if (I(ev, "seat") == seat) s += 2;
    } else if (ty == "counter") {
      if (I(ev, "seat") == seat) s += 5;
      if (I(ev, "attacker") == seat) s -= 8;
    } else if (ty == "hook_pull") {
      if (I(ev, "target") == seat) s -= 12;
    }
  }
  const Fighter &m = e.fighters[seat];
  if (m.out) s -= 1000;
  double ax = std::fabs(m.x);
  if (m.grounded && ax < C.sx - 0.4 && m.y >= -0.1) s += 6;
  double over = std::max(0.0, ax - (C.sx - 1.0));
  s -= 3.0 * over;
  if (m.y < -0.5) s -= 4.0 * (-0.5 - m.y);
  double bt = boundTop(e.tick_no);
  if (m.y > bt - 4) s -= 2.0 * (m.y - (bt - 4));
  if (m.hitstun > 0) s -= 3;
  double bx = boundX(e.tick_no);
  double margin = bx - ax;
  if (margin < 3.0) s -= 8.0 * (3.0 - margin);
  s += 0.5 * m.meter;
  // reward being close enough to fight, reward damaged opponents slightly
  double dmin = 1e9;
  for (int j = 0; j < e.n; ++j) {
    if (j == seat) continue;
    const Fighter &h = e.fighters[j];
    if (h.out || h.respawn > 0) continue;
    double dx = h.x - m.x, dy = h.y - m.y;
    dmin = std::min(dmin, std::sqrt(dx * dx + dy * dy));
    s += 0.05 * h.damage;
  }
  if (dmin < 1e8) s += 0.6 * (3.0 - cl(dmin, 0.0, 3.0));
  return s;
}

// ------------------------------------------------------------------ recovery
static bool isSafe(const Fighter &f, int tick) {
  if (f.out || f.respawn > 0 || !f.grounded || f.y < -0.1) return false;
  // Late game the blast lines close in: the y=9 centre platform and |x|>blast_x
  // become lethal, so "safe" must use the *current* (shrunk) boundary, not the stage.
  double bx = boundX(tick), bt = boundTop(tick);
  return std::fabs(f.x) <= std::min(C.sx - 0.4, bx - 1.5) && f.y < bt - 1.5;
}
static bool needRecovery(const F &me, int tick) {
  double bx = boundX(tick), bt = boundTop(tick);
  if (std::fabs(me.x) > bx - 3.0) return true;
  if (me.y > bt - 3.5) return true;
  if (me.y < -0.6) return true;
  // Too high: repeated uppercuts used to climb out of the top blast (a "self" death).
  // Recovery mode naturally descends and lands, so route this situation there.
  if (!me.grounded && me.y > 14.0) return true;
  if (!me.grounded && std::fabs(me.x) > C.sx - 0.5) return true;
  return false;
}
// Greedy recovery continuation from a simulated state.
static Order recovery_cont(const Engine &e, int seat) {
  Order o;
  const Fighter &f = e.fighters[seat];
  if (f.out || f.respawn > 0) return o;
  double cent = -sg(f.x);
  double ax = std::fabs(f.x);
  o.move = ax > 0.8 ? cent : 0.0;
  // A shrinking ceiling makes the high platforms lethal: fall through to the main stage.
  if (f.grounded && f.y > boundTop(e.tick_no) - 3.0) o.drop = true;
  if (f.hitstun > 0 || !f.act.empty() || f.gap > 0) return o;
  bool needUp = (f.y < 1.5 || f.vy < -0.15);
  if (f.meter >= C.c_blink && (ax > 14.0 || f.vy < -0.45)) {
    o.skill = "blink"; o.has_dir = true; o.dx = cent; o.dy = f.y < 2.0 ? 0.45 : 0.12;
  } else if (f.air_jumps > 0 && f.vy < 0.0) {
    o.jump = true;
  } else if (f.meter >= C.c_up && needUp && f.air_jumps == 0) {
    o.skill = "uppercut";
  }
  return o;
}
static double evalRec(const Engine &e, int seat) {
  double s = 0;
  for (const J &ev : e.events.as_array()) {
    if (St(ev, "type") == "fall" && I(ev, "seat") == seat) s -= 100000;
  }
  const Fighter &m = e.fighters[seat];
  if (m.out) s -= 100000;
  double ax = std::fabs(m.x);
  if (isSafe(m, e.tick_no)) s += 1000 - 4.0 * ax;
  s -= 30.0 * ax;
  s -= 40.0 * std::max(0.0, -m.y);
  if (m.y > 14.0) s -= 20.0 * (m.y - 14.0);  // discourage climbing out of the top blast
  double bt = boundTop(e.tick_no);
  if (m.y > bt - 3.0) s -= 25.0 * (m.y - (bt - 3.0));  // do not linger under a shrinking ceiling
  if (m.vy < 0) s -= 6.0 * (-m.vy);
  s -= 2.0 * m.damage;
  s += 0.5 * m.meter;
  return s;
}
static double rollout_rec(Engine &e, const J &base, const Order &cand, int seat, int turns, int model) {
  e.load(base); e.events = arr();
  double s = 0;
  auto stepTurn = [&](std::vector<Order> &o) {
    for (int k = 0; k < 3; ++k) e.tick(o, k == 0);
    if (!isSafe(e.fighters[seat], e.tick_no)) s -= 3;
  };
  std::vector<Order> o1 = predict(e, seat, true, model);
  o1[seat] = cand;
  stepTurn(o1);
  for (int t = 1; t < turns; ++t) {
    std::vector<Order> o = predict(e, seat, true, model);
    o[seat] = recovery_cont(e, seat);
    stepTurn(o);
  }
  s += evalRec(e, seat);
  return s;
}

// ------------------------------------------------------------------ combat rollout
static double rollout(Engine &e, const J &base, const Order &cand, int seat, int turns, int model) {
  e.load(base); e.events = arr();
  std::vector<Order> o1 = predict(e, seat, true, model);
  o1[seat] = cand;
  for (int k = 0; k < 3; ++k) e.tick(o1, k == 0);
  for (int t = 1; t < turns; ++t) {
    std::vector<Order> o = predict(e, seat, true, model);
    o[seat] = seat_cont(e, seat);
    for (int k = 0; k < 3; ++k) e.tick(o, k == 0);
  }
  return evaluate(e, seat);
}

// Robust score: average of the opponent models, pulled toward the worst case.
static double score_cand(Engine &e, const J &base, const Order &cand, int seat, int turns, bool rec) {
  double sc[3];
  for (int m = 0; m < 3; ++m)
    sc[m] = rec ? rollout_rec(e, base, cand, seat, turns, m) : rollout(e, base, cand, seat, turns, m);
  double mn = std::min(sc[0], std::min(sc[1], sc[2]));
  double mean = (sc[0] + sc[1] + sc[2]) / 3.0;
  return 0.55 * mean + 0.45 * mn;
}

// ------------------------------------------------------------------ main decision
struct Cand { Order o; double prior; };
static void add(std::vector<Cand> &v, const Order &o, double prior) { v.push_back({o, prior}); }

static J fallback(const J &obs) {
  J r = J::object{};
  int seat = I(obs, "seat");
  const J &me = obs.at("players").at(seat);
  double x = D(me, "x");
  if (std::fabs(x) > 1.0) r["move"] = x > 0 ? -1.0 : 1.0;
  return r;
}

static J decide(const J &obs, J & /*memory*/) {
  try {
    init(obs);
    int seat = I(obs, "seat"), tick = I(obs, "tick");
    const auto &ps = obs.at("players").as_array();
    int n = (int)ps.size();
    F me = parse(ps[seat]);
    if (me.out) return J::object{};
    if (me.respawn > 0) return J::object{};
    if (me.hitstun > 0) {  // cannot act while in hitstun; just lean toward centre
      J r = J::object{};
      r["move"] = -sg(me.x);
      return r;
    }

    // choose a target: nearest alive opponent, slight preference for the more damaged one
    int tgt = -1; double best = 1e18;
    for (int i = 0; i < n; ++i) {
      if (i == seat) continue;
      F f = parse(ps[i]);
      if (f.out || f.respawn > 0) continue;
      double dx = f.x - me.x, dy = f.y - me.y;
      double dist = std::sqrt(dx * dx + dy * dy);
      double kb = std::max(0.0, f.dmg - 60.0);
      double score = dist - 0.02 * f.dmg - 0.045 * kb - (std::fabs(f.x) > C.sx ? 0.8 : 0.0);
      if (score < best) { best = score; tgt = i; }
    }

    Engine eng(make_subs(n), obs.at("seed"), obs.at("rules"));
    J base = build_state(obs);
    eng.load(base);

    // ---------- recovery ----------
    if (needRecovery(me, tick)) {
      std::vector<Cand> cands;
      double cent = -sg(me.x);
      add(cands, mk(cent, false, "", false, 0, 0), 0);
      add(cands, mk(cent, false, "blink", true, cent, 0.0), 0);
      add(cands, mk(cent, false, "blink", true, cent, 0.4), 0);
      add(cands, mk(cent, false, "blink", true, cent, 0.8), 0);
      add(cands, mk(cent, true, "", false, 0, 0), 0);
      add(cands, mk(0, false, "", false, 0, 0, true), 0);  // drop through a high platform
      add(cands, mk(0, false, "uppercut"), 0);
      if (me.meter >= C.c_slam && !me.grounded) add(cands, mk(0, false, "slam", true, 0, -1), 0);
      add(cands, mk(0, false, "guard"), 0);
      if (tgt >= 0) {
        double dx = parse(ps[tgt]).x - me.x, dy = parse(ps[tgt]).y - me.y;
        add(cands, mk(cent, false, "attack", true, dx, dy), 0);
        add(cands, mk(0, false, "counter"), 0);
      }
      double bestS = -1e18; Order bestO;
      for (const Cand &c : cands) {
        double s = score_cand(eng, base, c.o, seat, 8, true) + c.prior;
        if (s > bestS) { bestS = s; bestO = c.o; }
      }
      return toOrder(bestO);
    }

    // ---------- combat ----------
    double tdx = 0, tdy = 0, tdist = 1e9, tdmg = 0;
    bool oppGuard = false, oppCounter = false, oppRecover = false, threat = false, oppAbove = false, oppBelow = false;
    if (tgt >= 0) {
      F t = parse(ps[tgt]);
      tdx = t.x - me.x; tdy = t.y - me.y;
      tdist = std::sqrt(tdx * tdx + tdy * tdy); tdmg = t.dmg;
      oppGuard = (t.act == "guard" && t.phase == ACTIVE);
      oppCounter = (t.act == "counter" && t.phase == ACTIVE);
      oppRecover = (t.phase == RECOVER) || t.hitstun > 0;
      oppAbove = tdy > 1.2;
      oppBelow = tdy < -1.2;
    }
    // Any nearby foe winding up / holding a charge / countering is a threat, not just our target.
    for (int i = 0; i < n; ++i) {
      if (i == seat) continue;
      F f = parse(ps[i]);
      if (f.out || f.respawn > 0) continue;
      double dx = f.x - me.x, dy = f.y - me.y;
      if (dx * dx + dy * dy > 12.0) continue;
      if (f.act == "charge" && f.phase == CHARGING && f.charge >= 12) { threat = true; continue; }
      if (f.act == "attack" || f.act == "charge" || f.act == "grab" || f.act == "uppercut" ||
          f.act == "slam" || f.act == "counter") {
        if (f.phase == WINDUP || f.phase == ACTIVE) threat = true;
      }
    }
    for (const J &s : obs.at("shots").as_array()) {
      double sx = D(s, "x"), sy = D(s, "y"), vx = D(s, "vx");
      double dx = me.x - sx, dy = me.y - sy;
      if (std::fabs(dx) < 8 && sg(vx) == sg(dx) && std::fabs(dy) < 2.5) threat = true;
    }
    double toward = sg(tdx);
    double cent = -sg(me.x);
    double bx = boundX(tick);
    bool nearShrink = (std::fabs(me.x) > bx - 4.0);
    bool hiDmg = me.dmg > 55;
    bool finish = tdmg > 70 && tdist < 3.5;
    bool charging = (me.act == "charge" && me.phase == CHARGING);

    double pA = 1.0, pG = 0.0, pC = 0.0, pD = 0.0, pK = 0.0, pB = 0.0;
    if (tdist < 2.6 && !oppGuard && !oppCounter) pA += 2.0;
    if (oppGuard || oppCounter) { pG += 3.0; pA -= 3.0; pK -= 1.0; }
    if (oppRecover && tdist < 3.2) { pC += 1.0; pA += 1.0; }
    if (oppRecover && tdist > 2.0 && tdist < 6.0 && !threat) pC += 2.0;  // punish window -> charge
    if (threat) { pD += 2.5; pK += 1.0; pB += 1.5; pA -= 1.5; }
    if (hiDmg) { pD += 2.5; pA -= 2.5; pB += 2.0; pC -= 1.5; }
    if (finish) { pA += 1.5; pC += 1.0; pG += 0.5; }
    if (me.invuln > 0) { pA += 1.5; pD -= 1.0; }  // free aggression while invulnerable
    // Commit to a charge we already started (short rollouts undervalue a full charge).
    if (charging && !threat) {
      pC += 4.0;
      if (me.charge < 40) pC += 3.0;
      else pA += 2.0;  // near full: releasing now is a heavy blow
    }

    std::vector<Cand> cands;
    // approach / reposition
    add(cands, mk(toward, false, ""), 0.5);
    add(cands, mk(cent, false, ""), 0.3 + (nearShrink ? 2.5 : 0.0));
    add(cands, mk(0, false, ""), 0.0);
    add(cands, mk(toward, true, ""), 0.2);
    // attacks (different launch angles)
    add(cands, mk(toward, false, "attack", true, tdx, 0.0), pA);
    add(cands, mk(toward, false, "attack", true, tdx, -0.3), pA + 0.3);
    add(cands, mk(toward, false, "attack", true, tdx, 0.8), pA - 0.2);
    add(cands, mk(0, false, "attack", true, 0.0, 1.0), oppAbove ? pA + 1.0 : pA - 2.0);
    add(cands, mk(toward, true, "attack", true, tdx, 0.2), pA - 0.5);
    // grab beats guard/counter
    add(cands, mk(toward, false, "grab", true, tdx, 0.0), pG);
    add(cands, mk(toward, false, "grab", true, tdx, -0.2), pG - 0.2);
    // charge
    add(cands, mk(0, false, "charge"), pC);
    add(cands, mk(toward * 0.5, false, "charge"), pC - 0.3);
    // defence
    add(cands, mk(0, false, "guard"), pD);
    add(cands, mk(cent, false, "guard"), pD - 0.3);
    add(cands, mk(0, false, "counter"), pK);
    // blinks
    add(cands, mk(0, false, "blink", true, toward, 0.2), pB + 0.2);
    add(cands, mk(0, false, "blink", true, -toward, 0.2), pB);
    add(cands, mk(0, false, "blink", true, cent, 0.25), pB);
    add(cands, mk(0, false, "blink", true, toward, 0.7), pB - 0.3 - (me.y > 12.0 ? 4.0 : 0.0));
    // vertical / specials
    add(cands, mk(0, false, "uppercut"), (me.y > 12.0) ? -6.0 : (oppAbove ? 1.0 : 0.0));
    if (!me.grounded && me.meter >= C.c_slam) add(cands, mk(0, false, "slam", true, 0, -1), oppBelow ? 1.5 : 0.0);
    if (me.meter >= C.c_wave) {
      double wp = tdist > 6 ? 0.5 : -2.0;
      if (tdmg > 65 && tdist > 3.0 && tdist < 14.0) wp += 1.5;  // tag a damaged foe for kill credit
      add(cands, mk(0, false, "wave", true, tdx, tdy), wp);
    }
    if (me.meter >= C.c_hook && me.hook_cd == 0) add(cands, mk(0, false, "hook", true, tdx, tdy), tdist > 4 && tdist < 16 ? 0.3 : -1.0);
    // drop through a platform
    if (me.grounded && me.y > 1.0) add(cands, mk(0, false, "", false, 0, 0, true), 0.0);

    double bestS = -1e18; Order bestO;
    int turns = 4;
    for (const Cand &c : cands) {
      double s = score_cand(eng, base, c.o, seat, turns, false) + c.prior;
      if (s > bestS) { bestS = s; bestO = c.o; }
    }
    return toOrder(bestO);
  } catch (...) {
    return fallback(obs);
  }
}

int main() {
  return sm_step_worker::run([](const J &obs, J &memory) {
    return std::make_pair(decide(obs, memory), memory);
  });
}
