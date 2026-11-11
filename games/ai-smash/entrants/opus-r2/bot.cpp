// Simulation bot: copies the official engine, tries a set of candidate plans for this turn against a few opponent
// models, scores the outcome (stocks, damage, launches toward the blast lines, own safety) and plays the best one.
#include "include/engine.hpp"
#include "include/step_worker.hpp"
#include "include/params.hpp"
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
using J = mini_json::Json;
using smash::Engine;
using smash::Fighter;
using smash::Order;
using smash::Shot;

namespace bot {
static Engine *E = nullptr;
static int N = 0, ME = 0;
static double hookX[8], hookY[8];
static bool hookSeen[8];
static std::chrono::steady_clock::time_point T0;
static double prevDmg[8];
static int prevStocks[8], creditTick[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static bool predictedHit[8], havePrev = false;
static double elapsed_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count(); }

static int phase_of(const std::string &s) {
  if (s == "charging") return smash::CHARGING;
  if (s == "windup") return smash::WINDUP;
  if (s == "active") return smash::ACTIVE;
  if (s == "recover") return smash::RECOVER;
  return smash::IDLE;
}
static double D(const J &o, const char *k) { return o.at(k).as_double(); }
static int I(const J &o, const char *k) { return int(o.at(k).as_int()); }

static void load_obs(const J &obs) {
  const auto &ps = obs.at("players").as_array();
  N = int(ps.size());
  ME = I(obs, "seat");
  if (!E) {
    J subs = J::array{};
    for (int i = 0; i < N; ++i) subs.push_back(J::object{});
    E = new Engine(subs, J(0), obs.at("rules"));
  }
  E->turn = I(obs, "turn") - 1;
  E->tick_no = I(obs, "tick");
  E->done = false;
  E->events = J::array{};
  std::vector<bool> hasHook(N, false);
  E->shots.clear();
  int maxid = 0;
  for (const auto &s : obs.at("shots").as_array()) {
    Shot b;
    b.id = I(s, "id"); b.owner = I(s, "owner"); b.kind = s.at("kind").as_string();
    b.x = D(s, "x"); b.y = D(s, "y"); b.vx = D(s, "vx"); b.vy = D(s, "vy"); b.ttl = I(s, "ttl");
    if (b.kind == "wave") { b.damage = 6.0; b.power = 0.55; }
    else { b.damage = 3.0; b.power = 0; if (b.owner >= 0 && b.owner < N) { hasHook[b.owner] = true; hookX[b.owner] = b.x; hookY[b.owner] = b.y; hookSeen[b.owner] = true; } }
    maxid = std::max(maxid, b.id);
    E->shots.push_back(b);
  }
  E->next_shot = maxid + 1;
  for (int i = 0; i < N; ++i) {
    const J &p = ps[i];
    Fighter &f = E->fighters[i];
    f.x = D(p, "x"); f.y = D(p, "y"); f.vx = D(p, "vx"); f.vy = D(p, "vy");
    f.facing = I(p, "facing"); f.grounded = p.at("grounded").as_bool(); f.air_jumps = I(p, "air_jumps");
    f.damage = D(p, "damage"); f.stocks = I(p, "stocks"); f.meter = D(p, "meter");
    f.act = p.at("act").is_null() ? std::string() : p.at("act").as_string();
    f.phase = phase_of(p.at("phase").as_string());
    f.phase_t = I(p, "phase_ticks"); f.charge = I(p, "charge");
    f.dx = p.at("dir").at(0).as_double(); f.dy = p.at("dir").at(1).as_double();
    f.hitstun = I(p, "hitstun"); f.gap = I(p, "gap"); f.hook_cd = I(p, "hook_cd");
    f.respawn = I(p, "respawn"); f.invuln = I(p, "invuln"); f.out = p.at("out").as_bool();
    f.power = f.act == "charge" ? 1.0 + std::min(1.0, f.charge / 45.0) : 1.0;
    f.drop = 0; f.hit_mask = 0; f.last_hit = -1; f.last_hit_tick = -1; f.pulled_by = -1; f.pull_t = 0;
    f.reeling = false;
    // a fighter falling fast while in hitstun was most likely slammed: the landing cap applies to it
    f.slam_landing = f.hitstun > 15 && f.vy < -0.45 && !f.grounded;
    if (f.act == "hook" && f.phase == smash::ACTIVE && !hasHook[i] && hookSeen[i]) {
      f.reeling = true; f.hook_x = hookX[i]; f.hook_y = hookY[i];
    }
    if (f.act != "hook") hookSeen[i] = false;
  }
  for (auto &k : E->kills) k = 0;
  // who owns the last hit on each opponent: me if my own prediction said my move connects and their damage rose
  int tick = E->tick_no;
  for (int i = 0; i < N && i < 8; ++i) {
    if (i == ME) continue;
    Fighter &f = E->fighters[i];
    if (!havePrev || f.out || f.respawn > 0 || f.stocks != prevStocks[i]) creditTick[i] = -1;
    else if (f.damage > prevDmg[i] + 0.01) creditTick[i] = predictedHit[i] ? tick : -1;
    if (creditTick[i] >= 0 && tick - creditTick[i] <= 300) { f.last_hit = ME; f.last_hit_tick = creditTick[i]; }
    prevDmg[i] = f.damage; prevStocks[i] = f.stocks;
  }
  havePrev = true;
  for (int i = 0; i < 8; ++i) predictedHit[i] = false;
}

struct Snap {
  std::vector<Fighter> f; std::vector<Shot> s; std::vector<int> kills; int turn = 0, tick = 0, next_shot = 0; bool done = false;
};
static Snap save() { Snap s; s.f = E->fighters; s.s = E->shots; s.kills = E->kills; s.turn = E->turn; s.tick = E->tick_no; s.next_shot = E->next_shot; s.done = E->done; return s; }
static void restore(const Snap &s) { E->fighters = s.f; E->shots = s.s; E->kills = s.kills; E->turn = s.turn; E->tick_no = s.tick; E->next_shot = s.next_shot; E->done = s.done; E->events = J::array{}; }

enum Cont { SAME = 0, CHARGE_HOLD = 1, GUARD_HOLD = 2, IDLE_AFTER = 3 };
struct Plan { Order o; int cont = SAME; int hold = 0; std::string name; std::vector<Order> seq; };

static bool actionable(const Fighter &f) { return f.act.empty() && f.hitstun == 0 && f.respawn == 0 && !f.out && f.pull_t == 0; }

static double blast_x_at(int tick) {
  double bx = 26.0;
  if (tick > 3000) bx = std::max(0.0, bx - (tick - 3000) * 0.02);
  return bx;
}
static double blast_top_at(int tick) {
  double bt = 24.0;
  if (tick > 3000) bt = std::max(1.0, bt - (tick - 3000) * 0.02 * 0.9);
  return bt;
}

// Ballistic extrapolation of a launched fighter: 2 = crosses a blast line while still in hitstun, 1 = ends hitstun
// in a spot that is very hard to recover from, 0 = fine.
static int launch_fate(const Fighter &f, int tick) {
  double x = f.x, y = f.y, vx = f.vx, vy = f.vy;
  int hs = f.hitstun;
  for (int t = 0; t < 80; ++t) {
    ++tick;
    if (hs > 0) { vy = std::max(vy - 0.035, -2.0); vx *= 0.97; vy *= 0.97; --hs; }
    else { vy = std::max(vy - 0.035, -0.8); vx *= 0.985; }
    x += vx; y += vy;
    if (std::fabs(x) - 0.6 < 13.0 && y < 0 && y > -4.0 - 1.8) return 0;  // back on/into the stage
    if (std::fabs(x) > blast_x_at(tick) || y > blast_top_at(tick) || y < -16.0) return hs > 0 ? 2 : (t < 40 ? 1 : 0);
    if (hs == 0) {
      // out of hitstun: can it still come back?
      double hx = std::fabs(x) - 13.0;
      if (y < -9.0 || hx > 17.0) return 1;
      return 0;
    }
  }
  return 0;
}

static double offstage_pen(const Fighter &f) {
  if (f.out || f.respawn > 0) return 0;
  double ax = std::fabs(f.x);
  double pen = 0;
  if (ax > 13.0) pen += (ax - 13.0) * 12.0;
  if (f.y < -0.2 && !f.grounded) pen += (-f.y) * 25.0 * (ax > 12.4 ? 1.0 : 0.5);
  if (ax > 13.0 || f.y < -0.2) {
    if (f.air_jumps == 0) pen += 20;
    if (f.meter < 0.25) pen += 25;
  }
  return pen;
}

// Opponent model orders. 0 passive, 1 striker (attack / slam from above), 2 grabber, 3 reactive (counter or guard
// against my slow moves).
static Order opp_order(int i, int model) {
  Order o;
  const Fighter &f = E->fighters[i];
  const Fighter &m = E->fighters[ME];
  if (f.out || f.respawn > 0) return o;
  if (!f.grounded && (std::fabs(f.x) > 13.0 || f.y < -0.3)) {
    o.move = f.x > 0 ? -1 : 1;
    if (f.vy < 0 && f.air_jumps > 0) o.jump = true;
    else if (f.vy < 0 && f.meter >= 0.25) o.skill = "uppercut";
    return o;
  }
  {
    double gx = m.x - f.x, gy = m.y - f.y;
    if (f.act == "guard" && f.phase == smash::ACTIVE && gx * gx + gy * gy < 7.0 * 7.0 && !m.out && m.respawn == 0) { o.skill = "guard"; return o; }
  }
  if (model == 0) return o;
  if (m.out || m.respawn > 0) return o;
  double dx = m.x - f.x, dy = m.y - f.y, d2 = dx * dx + dy * dy;
  o.move = dx > 0 ? 1 : -1;
  // a held charge: keep it until I walk into its reach, then let go
  if (f.act == "charge" && f.phase == smash::CHARGING && f.hitstun == 0) {
    o.move = dx > 0 ? 1 : -1;
    if (dy > 1.5 && f.grounded && std::fabs(dx) < 5.0) o.jump = true;
    if (d2 > 3.8 * 3.8 && f.charge < 70) { o.skill = "charge"; o.dx = dx > 0 ? 1 : -1; o.dy = 0; o.has_dir = true; }
    return o;
  }
  bool act = actionable(f) && f.gap == 0;
  bool myCounter = m.act == "counter" && m.phase != smash::RECOVER;
  bool myGuard = m.act == "guard";
  bool myOpen = (m.hitstun > 10) || (!m.act.empty() && m.phase == smash::RECOVER && m.phase_t >= 5);
  if (model == 3) {
    bool slow = (m.act == "charge" && m.phase == smash::WINDUP) || (m.act == "uppercut" && m.phase == smash::WINDUP) ||
                (m.act == "attack" && m.phase == smash::WINDUP) || (m.act == "slam" && m.phase != smash::RECOVER);
    if (m.act == "charge" && m.phase == smash::CHARGING && d2 < 4.8 * 4.8) { o.move = 0; return o; }
    if (slow && d2 < 4.8 * 4.8 && act) {
      if (f.meter >= 0.3 && m.act == "charge") o.skill = "counter";
      else o.skill = "guard";
      o.move = 0;
      return o;
    }
    if (f.act == "guard" && slow && d2 < 4.8 * 4.8) { o.skill = "guard"; o.move = 0; return o; }
    if (d2 < 3.0 * 3.0 && act && (m.act.empty() || myOpen) && !myCounter) { o.skill = "attack"; o.dx = dx; o.dy = dy; o.has_dir = true; o.move = 0; }
    return o;
  }
  if (!act) return o;
  if (myCounter) {
    if (model == 2 && f.meter >= 0.3 && d2 < 3.0 * 3.0) { o.skill = "grab"; o.dx = dx; o.dy = dy; o.has_dir = true; o.move = 0; return o; }
    o.move = 0;  // wait it out
    return o;
  }
  if (!f.grounded && f.meter >= 0.2 && m.y < f.y - 0.3 && std::fabs(dx) < 2.8 && f.y - m.y < 9.0) { o.skill = "slam"; o.move = 0; return o; }
  if (f.meter >= 0.25 && m.y > f.y + 0.8 && std::fabs(dx) < 1.8 && d2 < 3.6 * 3.6) { o.skill = "uppercut"; o.move = 0; return o; }
  if (model == 2 && f.meter >= 0.3 && d2 < 3.0 * 3.0) { o.skill = "grab"; o.dx = dx; o.dy = dy; o.has_dir = true; o.move = 0; return o; }
  if (myGuard && f.meter >= 0.3 && d2 < 3.0 * 3.0) { o.skill = "grab"; o.dx = dx; o.dy = dy; o.has_dir = true; o.move = 0; return o; }
  if (myOpen && d2 < 3.5 * 3.5) { o.skill = "charge"; o.dx = dx > 0 ? 1 : -1; o.dy = 0; o.has_dir = true; o.move = 0; return o; }
  if (d2 < 3.2 * 3.2) { o.skill = "attack"; o.dx = dx; o.dy = dy; o.has_dir = true; o.move = 0; }
  else if (dy > 1.5 && f.grounded) o.jump = true;
  return o;
}

static Order my_order(const Plan &p, int t) {
  if (t == 0) return p.o;
  if (t < int(p.seq.size())) return p.seq[t];
  Order o;
  o.move = p.o.move;
  if (p.cont == CHARGE_HOLD) {
    if (t < p.hold) { o.skill = "charge"; o.dx = p.o.dx; o.dy = p.o.dy; o.has_dir = p.o.has_dir; }
  } else if (p.cont == GUARD_HOLD) {
    if (t < p.hold) o.skill = "guard";
  } else if (p.cont == IDLE_AFTER) {
    o.move = 0;
  }
  // stay safe in the follow-up: steer home if off the stage
  const Fighter &f = E->fighters[ME];
  if (!f.grounded && (std::fabs(f.x) > 12.5 || f.y < -0.3)) {
    o.move = f.x > 0 ? -1 : 1;
    if (f.vy < 0 && f.air_jumps > 0 && f.y < 1.0) o.jump = true;
  }
  return o;
}

struct Base { std::vector<double> dmg; std::vector<int> stocks; };

static double evaluate(const Base &b) {
  double sc = 0;
  const Fighter &m = E->fighters[ME];
  int tick = E->tick_no;
  // me
  int lost = b.stocks[ME] - (m.out ? 0 : m.stocks);
  if (lost > 0) sc -= 5000.0 * lost;
  else {
    double taken = m.damage - b.dmg[ME];
    sc -= taken * (2.0 + b.dmg[ME] / 40.0);
    if (m.hitstun > 0) {
      int fate = launch_fate(m, tick);
      if (fate == 2) sc -= 4200; else if (fate == 1) sc -= 1200;
    }
    sc -= offstage_pen(m);
    sc -= m.hitstun * 1.2;
    // late game: hug the middle
    double bx = blast_x_at(tick + 90);
    if (tick > 2700) sc -= std::fabs(m.x) * 6.0 + (std::fabs(m.x) > bx - 3 ? 400 : 0);
    if (tick > 3000 && m.y > blast_top_at(tick + 60) - 3) sc -= 300;
  }
  // opponents
  double nearest = 1e9;
  for (int i = 0; i < N; ++i) {
    if (i == ME) continue;
    const Fighter &f = E->fighters[i];
    int st = f.out ? 0 : f.stocks;
    int olost = b.stocks[i] - st;
    if (olost > 0) {
      sc += (f.last_hit == ME || E->kills[ME] > 0) ? 4000.0 * olost : 300.0 * olost;
      continue;
    }
    if (f.out || f.respawn > 0) continue;
    double dealt = f.damage - b.dmg[i];
    if (dealt > 0) sc += dealt * (2.5 + b.dmg[i] / 50.0);
    if (f.hitstun > 0 && f.last_hit == ME) sc += f.hitstun * 1.0;
    if (f.hitstun > 0 && f.last_hit == ME) {
      int fate = launch_fate(f, tick);
      if (fate == 2) sc += 3500; else if (fate == 1) sc += 1200;
      else sc += std::max(0.0, std::fabs(f.x) - 10.0) * 15.0;
    }
    if (f.invuln == 0) {
      double dx = f.x - m.x, dy = f.y - m.y;
      nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
    }
  }
  if (nearest < 1e8 && b.dmg[ME] < 120) sc -= nearest * P_NEAR_W;
  if (lost == 0) sc += m.meter * P_METER_W;
  // prefer the middle of the stage a bit
  sc -= std::max(0.0, std::fabs(m.x) - 8.0) * 3.0;
  return sc;
}

static double simulate(const Plan &p, int model, int turns, const Base &b, const Snap &s) {
  restore(s);
  std::vector<Order> orders(N);
  for (int t = 0; t < turns && !E->done; ++t) {
    for (int i = 0; i < N; ++i) orders[i] = i == ME ? my_order(p, t) : opp_order(i, model);
    for (int k = 0; k < 3 && !E->done; ++k) E->tick(orders, k == 0);
    ++E->turn;
  }
  return evaluate(b);
}

static Order mk(double move, bool jump, const std::string &skill = "", double dx = 0, double dy = 0, bool hasdir = false, bool drop = false) {
  Order o; o.move = move; o.jump = jump; o.skill = skill; o.dx = dx; o.dy = dy; o.has_dir = hasdir; o.drop = drop; return o;
}

static std::vector<Plan> candidates() {
  std::vector<Plan> c;
  const Fighter &m = E->fighters[ME];
  auto add = [&](Order o, int cont, int hold, const std::string &nm) { Plan p; p.o = o; p.cont = cont; p.hold = hold; p.name = nm; c.push_back(p); };
  for (int mv = -1; mv <= 1; ++mv)
    for (int j = 0; j < 2; ++j) add(mk(mv, j == 1), SAME, 0, "move");
  bool onPlat = m.grounded && m.y > 1.0;
  if (onPlat) add(mk(0, false, "", 0, 0, false, true), SAME, 0, "drop");

  bool free = m.hitstun == 0 && m.respawn == 0 && m.pull_t == 0;
  if (!free) return c;
  if (m.act == "charge" && m.phase == smash::CHARGING) {
    // keep charging (and release later) or release now, or bail out into a guard
    for (int h = 1; h <= 4; h += 3) add(mk(0, false, "charge", m.dx, m.dy, true), CHARGE_HOLD, h, "charge-hold");
    add(mk(0, false), IDLE_AFTER, 0, "release");
    add(mk(0, false, "guard"), GUARD_HOLD, 2, "guard");
    add(mk(0, false, "blink", 0, 1, true), IDLE_AFTER, 0, "blink-up");
    return c;
  }
  if (m.act == "guard") {
    add(mk(0, false, "guard"), GUARD_HOLD, 2, "guard");
    return c;
  }
  if (!m.act.empty() || m.gap > 3) return c;
  std::vector<std::pair<double, double>> dirs = {{1, 0}, {-1, 0}, {0.8, 0.6}, {-0.8, 0.6}};
  for (int i = 0; i < N; ++i) {
    if (i == ME) continue;
    const Fighter &f = E->fighters[i];
    if (f.out || f.respawn > 0) continue;
    double dx = f.x - m.x, dy = f.y - m.y, l = std::sqrt(dx * dx + dy * dy);
    if (l < 1e-6) continue;
    if (l < 6.0) dirs.push_back({dx / l, dy / l});
  }
  double meter = m.meter;
  for (auto &d : dirs) add(mk(0, false, "attack", d.first, d.second, true), IDLE_AFTER, 0, "attack");
  if (meter >= 0.3)
    for (auto &d : dirs) add(mk(0, false, "grab", d.first, d.second, true), IDLE_AFTER, 0, "grab");
  for (int k = 0; k < 2; ++k) {
    double dx = k == 0 ? 1 : -1;
    for (int h : {2, 4, 7}) add(mk(dx, false, "charge", dx, 0, true), CHARGE_HOLD, h, "charge");
  }
  add(mk(0, false, "guard"), GUARD_HOLD, 2, "guard");
  if (meter >= 0.3) add(mk(0, false, "counter"), IDLE_AFTER, 0, "counter");
  if (meter >= 0.25) add(mk(0, false, "uppercut"), IDLE_AFTER, 0, "uppercut");
  if (meter >= 0.2 && !m.grounded) add(mk(0, false, "slam"), IDLE_AFTER, 0, "slam");
  if (meter >= 0.6) {
    for (int i = 0; i < N; ++i) {
      if (i == ME) continue;
      const Fighter &f = E->fighters[i];
      if (f.out || f.respawn > 0) continue;
      double dx = f.x - m.x, dy = f.y + 0.9 - (m.y + 0.9), l = std::sqrt(dx * dx + dy * dy);
      if (l < 1e-6 || l > 30) continue;
      add(mk(0, false, "wave", dx / l, dy / l, true), IDLE_AFTER, 0, "wave");
    }
  }
  if (meter >= 0.4 && m.hook_cd == 0) {
    for (int i = 0; i < N; ++i) {
      if (i == ME) continue;
      const Fighter &f = E->fighters[i];
      if (f.out || f.respawn > 0) continue;
      double dx = f.x - m.x, dy = f.y - m.y, l = std::sqrt(dx * dx + dy * dy);
      if (l < 1e-6 || l > 18) continue;
      add(mk(0, false, "hook", dx / l, dy / l, true), IDLE_AFTER, 0, "hook");
    }
  }
  // two-step plans: hop (or keep moving) and strike on the next turn or the one after
  for (int mv = -1; mv <= 1; ++mv) {
    for (int jmp = 0; jmp < 2; ++jmp) {
      if (jmp == 0 && mv == 0) continue;
      for (int wait = 1; wait <= 2; ++wait) {
        std::vector<std::string> sk = {"attack"};
        if (meter >= 0.2 && (jmp || !m.grounded)) sk.push_back("slam");
        if (meter >= 0.3) sk.push_back("grab");
        for (const auto &s : sk) {
          Plan p; p.o = mk(mv, jmp == 1); p.cont = IDLE_AFTER; p.name = "seq-" + s;
          p.seq.push_back(p.o);
          for (int w = 1; w < wait; ++w) p.seq.push_back(mk(mv, false));
          Order fin = mk(0, false, s);
          if (s != "slam") {
            // aim at the nearest opponent at strike time: approximate with current relative position, flattened
            double bx = 0, by = 0, bd = 1e9;
            for (int i = 0; i < N; ++i) {
              if (i == ME) continue;
              const Fighter &f = E->fighters[i];
              if (f.out || f.respawn > 0) continue;
              double dx = f.x - m.x, dy = f.y - m.y, dd = dx * dx + dy * dy;
              if (dd < bd) { bd = dd; bx = dx; by = dy; }
            }
            if (bd > 9 * 9) continue;
            double l = std::sqrt(bx * bx + by * by);
            if (l < 1e-6) { bx = m.facing; by = 0; l = 1; }
            fin.dx = bx / l; fin.dy = by / l; fin.has_dir = true;
          }
          p.seq.push_back(fin);
          c.push_back(p);
        }
      }
    }
  }
  if (meter >= 0.2) {
    for (auto d : std::vector<std::pair<double, double>>{{1, 0}, {-1, 0}, {0, 1}, {0.7, 0.7}, {-0.7, 0.7}})
      add(mk(0, false, "blink", d.first, d.second, true), IDLE_AFTER, 0, "blink");
  }
  return c;
}

// Recovery: off the stage, search over (first rescue move, delay) with a simple follow-up policy and keep the plan
// that gets back on the stage soonest without losing the stock.
static Order rescue_policy(int t, int delay, const Order &act) {
  const Fighter &m = E->fighters[ME];
  Order o;
  double side = m.x > 0 ? 1 : -1;
  o.move = std::fabs(m.x) > 11.0 ? -side : 0;
  if (t < delay) return o;
  if (t == delay) { Order a = act; a.move = o.move; return a; }
  bool can = m.act.empty() && m.gap == 0 && m.hitstun == 0;
  if (m.vy < 0.05 && m.air_jumps > 0 && m.y < 2.0) { o.jump = true; return o; }
  if (can && m.air_jumps == 0 && m.vy < 0 && m.y < -0.3 && m.meter >= 0.25) { o.skill = "uppercut"; return o; }
  if (can && m.air_jumps == 0 && m.vy < 0 && m.y < -2.5 && m.meter >= 0.2) {
    double bx = side * 12.0 - m.x, by = 1.0 - m.y, bl = std::sqrt(bx * bx + by * by);
    o.skill = "blink"; o.dx = bx / bl; o.dy = by / bl; o.has_dir = true;
  }
  return o;
}

static bool recover(Order &out) {
  const Fighter &m0 = E->fighters[ME];
  if (m0.grounded || m0.respawn > 0 || m0.out) return false;
  double ax = std::fabs(m0.x);
  bool off = ax > 13.4 || m0.y < -0.6;
  if (!off) return false;
  if (m0.hitstun > 0) { out.move = m0.x > 0 ? -1 : 1; return true; }
  double side = m0.x > 0 ? 1 : -1;
  std::vector<Order> acts;
  acts.push_back(mk(0, false));
  acts.push_back(mk(0, true));
  if (m0.meter >= 0.25) acts.push_back(mk(0, false, "uppercut"));
  if (m0.meter >= 0.2) {
    std::vector<std::pair<double, double>> tg = {{side * 12.0, 1.0}, {side * 12.5, -0.5}};
    for (auto &p : tg) {
      double bx = p.first - m0.x, by = p.second - m0.y, bl = std::sqrt(bx * bx + by * by);
      if (bl > 1e-6) acts.push_back(mk(0, false, "blink", bx / bl, by / bl, true));
    }
    acts.push_back(mk(0, false, "blink", 0, 1, true));
    acts.push_back(mk(0, false, "blink", -side * 0.7071, 0.7071, true));
  }
  if (m0.meter >= 0.4 && m0.hook_cd == 0) {
    std::vector<std::pair<double, double>> tg = {{side * 12.6, -0.4}, {side * 12.8, -2.0}, {side * 11.5, -3.6}};
    for (auto &p : tg) {
      double bx = p.first - m0.x, by = p.second - (m0.y + 0.9), bl = std::sqrt(bx * bx + by * by);
      if (bl > 1e-6) acts.push_back(mk(0, false, "hook", bx / bl, by / bl, true));
    }
  }
  Snap s = save();
  int stocks0 = m0.stocks;
  double best = -1e18; Order bestO = mk(-side, false);
  const int delays[5] = {0, 1, 2, 4, 7};
  std::vector<Order> orders(N);
  for (size_t a = 0; a < acts.size(); ++a) {
    for (int di = 0; di < 5; ++di) {
      int delay = delays[di];
      if (a == 0 && di > 0) break;
      if (elapsed_ms() > 6.0) break;
      restore(s);
      double sc = 0; bool done = false;
      for (int t = 0; t < 22 && !done; ++t) {
        for (int i = 0; i < N; ++i) orders[i] = i == ME ? rescue_policy(t, delay, acts[a]) : Order();
        for (int k = 0; k < 3 && !E->done; ++k) E->tick(orders, k == 0);
        ++E->turn;
        const Fighter &m = E->fighters[ME];
        if (m.out || m.stocks < stocks0 || m.respawn > 0) { sc = -100000 + t * 100; done = true; break; }
        if (m.grounded) { sc = 50000 - t * 60 + m.meter * 300 - std::max(0.0, std::fabs(m.x) - 9) * 50; done = true; break; }
      }
      if (!done) {
        const Fighter &m = E->fighters[ME];
        double ddx = std::max(0.0, std::fabs(m.x) - 12.5), ddy = std::max(0.0, -m.y);
        sc = -std::sqrt(ddx * ddx + ddy * ddy) * 100 + m.meter * 300 + m.air_jumps * 200;
      }
      // small preference for acting later when equal (keeps resources)
      if (sc > best) {
        best = sc;
        Order o0 = delay == 0 ? acts[a] : Order();
        o0.move = std::fabs(m0.x) > 11.0 ? -side : 0;
        bestO = o0;
      }
    }
  }
  restore(s);
  out = bestO;
  return true;
}

static J to_json(const Order &o) {
  J r = J::object{};
  r["move"] = o.move;
  if (o.jump) r["jump"] = true;
  if (o.drop) r["drop"] = true;
  if (!o.skill.empty()) r["skill"] = o.skill;
  if (o.has_dir) r["dir"] = J::array{o.dx, o.dy};
  return r;
}

static J decide(const J &obs) {
  T0 = std::chrono::steady_clock::now();
  load_obs(obs);
  const Fighter &m = E->fighters[ME];
  if (m.out || m.respawn > 0) return J::object{};
  Order ro;
  if (recover(ro)) return to_json(ro);
  Snap s = save();
  Base b;
  for (int i = 0; i < N; ++i) { b.dmg.push_back(E->fighters[i].damage); b.stocks.push_back(E->fighters[i].out ? 0 : E->fighters[i].stocks); }
  std::vector<Plan> cands = candidates();
  const double w[4] = {0.3, 0.25, 0.2, 0.25};
  double best = -1e18; int bi = 0;
  int turns = P_HORIZON;
  for (int ci = 0; ci < int(cands.size()); ++ci) {
    if (elapsed_ms() > 6.0) break;
    double sc = 0, worst = 1e18;
    bool complete = true;
    for (int md = 0; md < 4; ++md) {
      if (md > 0 && elapsed_ms() > 8.0) { complete = false; break; }
      double v = simulate(cands[ci], md, turns, b, s); sc += w[md] * v; worst = std::min(worst, v);
    }
    if (!complete) break;
    sc = (1.0 - P_PESS) * sc + P_PESS * worst;
    if (sc > best) { best = sc; bi = ci; }
  }
  restore(s);
  Order o = cands.empty() ? Order() : cands[bi].o;
  if (!cands.empty()) {
    // one-turn replay of the chosen order against idle opponents: does my move connect this turn?
    std::vector<Order> orders(N);
    orders[ME] = o;
    for (int k = 0; k < 3 && !E->done; ++k) E->tick(orders, k == 0);
    for (int i = 0; i < N && i < 8; ++i)
      if (i != ME) predictedHit[i] = E->fighters[i].last_hit == ME && E->fighters[i].last_hit_tick > s.tick;
    restore(s);
  }
  return to_json(o);
}
}  // namespace bot

int main() {
  return sm_step_worker::run([](const J &obs, J &memory) {
    J order = bot::decide(obs);
    return std::make_pair(order, memory);
  });
}
