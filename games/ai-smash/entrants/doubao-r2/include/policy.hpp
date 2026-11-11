#pragma once
// Round 2 policy: evidence from ep1 replays showed the round-1 bot was far too passive
// (8.2 hits dealt vs 20.2 taken, 65% airborne, 46% of deaths from charge attacks, almost no
// counters). This version stays grounded, fights aggressively, punishes every windup with a
// counter (doubled knockback), dodges grabs, and uses slams for area pressure.
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace mybot {
using J = mini_json::Json;

struct Cfg {
  bool ready = false;
  double half = 13.0, blastX = 26.0, blastTop = 24.0, blastBot = -16.0;
  double hw = 0.6, bodyH = 1.8;
  double grav = 0.035, maxFall = 0.8, run = 0.32, gAcc = 0.08, aAcc = 0.03;
  double airFric = 0.985, hitFric = 0.97;
  double gJump = 0.62, aJump = 0.55;
  double refill = 0.004, meterMax = 1.0;
  int shrinkStart = 3000;
  double shrinkRate = 0.02, shrinkTopRatio = 0.9;
  double costBlink = 0.2, costUpper = 0.25, costSlam = 0.2, costWave = 0.6;
  double costGrab = 0.3, costCounter = 0.3, costHook = 0.4;
  int hookMissCd = 150;
};

struct P {
  double x = 0, y = 0, vx = 0, vy = 0, damage = 0, meter = 0, dx = 1, dy = 0;
  int facing = 1, airJumps = 0, stocks = 3, hitstun = 0, gap = 0, hookCd = 0;
  int respawn = 0, invuln = 0, phase = 0, phaseT = 0, charge = 0;
  bool grounded = false, out = false;
  std::string act;
};

struct ShotS {
  int owner = 0;
  std::string kind;
  double x = 0, y = 0, vx = 0, vy = 0;
  int ttl = 0;
};

inline Cfg cfg;
inline int lastTick = -1;
inline int chargePlan = 0;

inline double clampv(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
inline double len2(double x, double y) { return std::sqrt(x * x + y * y); }
inline int phaseId(const std::string &s) {
  if (s == "charging") return 1;
  if (s == "windup") return 2;
  if (s == "active") return 3;
  if (s == "recover") return 4;
  return 0;
}
inline double jd(const J &o, const char *k, double dflt) {
  return o.contains(k) && !o.at(k).is_null() ? o.at(k).as_double() : dflt;
}
inline int ji(const J &o, const char *k, int dflt) {
  return o.contains(k) && !o.at(k).is_null() ? int(o.at(k).as_int()) : dflt;
}

inline void loadCfg(const J &rules) {
  if (cfg.ready) return;
  const auto &stage = rules.at("stage");
  cfg.half = stage.at("solids").at(0).at("x2").as_double();
  const auto &b = stage.at("blast");
  cfg.blastX = jd(b, "x", 26);
  cfg.blastTop = jd(b, "top", 24);
  cfg.blastBot = jd(b, "bottom", -16);
  if (stage.contains("blast_shrink")) {
    const auto &s = stage.at("blast_shrink");
    cfg.shrinkStart = ji(s, "start_tick", 3000);
    cfg.shrinkRate = jd(s, "rate", 0.02);
    cfg.shrinkTopRatio = jd(s, "top_ratio", 0.9);
  }
  const auto &body = rules.at("body");
  cfg.hw = jd(body, "half_width", 0.6);
  cfg.bodyH = jd(body, "height", 1.8);
  const auto &ph = rules.at("physics");
  cfg.grav = jd(ph, "gravity", 0.035);
  cfg.maxFall = jd(ph, "max_fall", 0.8);
  cfg.run = jd(ph, "run_speed", 0.32);
  cfg.gAcc = jd(ph, "ground_accel", 0.08);
  cfg.aAcc = jd(ph, "air_accel", 0.03);
  cfg.airFric = jd(ph, "air_friction", 0.985);
  cfg.hitFric = jd(ph, "hit_friction", 0.97);
  const auto &jp = rules.at("jump");
  cfg.gJump = jd(jp, "ground_speed", 0.62);
  cfg.aJump = jd(jp, "air_speed", 0.55);
  const auto &mt = rules.at("meter");
  cfg.refill = jd(mt, "refill", 0.004);
  cfg.meterMax = jd(mt, "max", 1.0);
  const auto &mv = rules.at("moves");
  cfg.costBlink = jd(mv.at("blink"), "cost", 0.2);
  cfg.costUpper = jd(mv.at("uppercut"), "cost", 0.25);
  cfg.costSlam = jd(mv.at("slam"), "cost", 0.2);
  cfg.costWave = jd(mv.at("wave"), "cost", 0.6);
  cfg.costGrab = jd(mv.at("grab"), "cost", 0.3);
  cfg.costCounter = jd(mv.at("counter"), "cost", 0.3);
  cfg.costHook = jd(mv.at("hook"), "cost", 0.4);
  cfg.hookMissCd = ji(mv.at("hook"), "miss_cooldown_ticks", 150);
  cfg.ready = true;
}

inline P readP(const J &j) {
  P p;
  p.x = jd(j, "x", 0); p.y = jd(j, "y", 0);
  p.vx = jd(j, "vx", 0); p.vy = jd(j, "vy", 0);
  p.damage = jd(j, "damage", 0); p.meter = jd(j, "meter", 0);
  p.facing = ji(j, "facing", 1);
  p.airJumps = ji(j, "air_jumps", 0);
  p.stocks = ji(j, "stocks", 3);
  p.hitstun = ji(j, "hitstun", 0);
  p.gap = ji(j, "gap", 0);
  p.hookCd = ji(j, "hook_cd", 0);
  p.respawn = ji(j, "respawn", 0);
  p.invuln = ji(j, "invuln", 0);
  p.phase = phaseId(j.contains("phase") && !j.at("phase").is_null() ? j.at("phase").as_string() : "idle");
  p.phaseT = ji(j, "phase_ticks", 0);
  p.charge = ji(j, "charge", 0);
  p.grounded = j.at("grounded").as_bool();
  p.out = j.at("out").as_bool();
  if (j.contains("act") && !j.at("act").is_null()) p.act = j.at("act").as_string();
  if (j.contains("dir") && j.at("dir").is_array() && j.at("dir").size() == 2) {
    p.dx = j.at("dir").at(0).as_double();
    p.dy = j.at("dir").at(1).as_double();
  } else {
    p.dx = p.facing;
  }
  return p;
}

inline bool canAct(const P &p) {
  return p.act.empty() && p.gap == 0 && p.hitstun == 0 && p.respawn == 0;
}
inline bool offstage(const P &p) {
  return !p.grounded && (std::fabs(p.x) > cfg.half - cfg.hw - 0.35 || p.y < -2.0);
}
inline double blastXAt(int tick) {
  if (tick <= cfg.shrinkStart) return cfg.blastX;
  double closed = (tick - cfg.shrinkStart) * cfg.shrinkRate;
  return std::max(0.0, cfg.blastX - closed);
}
inline double blastTopAt(int tick) {
  if (tick <= cfg.shrinkStart) return cfg.blastTop;
  double closed = (tick - cfg.shrinkStart) * cfg.shrinkRate;
  return std::max(1.0, cfg.blastTop - closed * cfg.shrinkTopRatio);
}
inline void setDir(J &o, double dx, double dy) {
  double l = len2(dx, dy);
  if (l < 1e-9) { dx = 1; dy = 0; l = 1; }
  o["dir"] = J::array{dx / l, dy / l};
}
inline J skill(const char *s, double dx, double dy) {
  J o = J::object{};
  o["skill"] = s;
  setDir(o, dx, dy);
  return o;
}

// ---------- recovery (kept conservative: it worked in round 1) ----------
inline bool recovery(const P &me, int tick, J &o) {
  double top = blastTopAt(tick), bx = blastXAt(tick);
  if (me.y > top - 4.0 && !me.grounded) {
    if (canAct(me) && me.meter >= cfg.costSlam && std::fabs(me.x) < cfg.half - 1.0) {
      o = skill("slam", 0, -1);
      return true;
    }
    o["move"] = me.x > 0 ? -1.0 : 1.0;
    return true;
  }
  if (!offstage(me)) return false;
  int inward = me.x > 0 ? -1 : 1;
  int side = me.x >= 0 ? 1 : -1;
  bool under = me.y < -2.5 && std::fabs(me.x) < cfg.half - 1.0;
  double steer = under ? double(side) : double(inward);
  o["move"] = steer;
  bool nearBlast = std::fabs(me.x) > bx - 4.0 || me.y < cfg.blastBot + 5.0;
  if (nearBlast && canAct(me) && me.meter >= cfg.costBlink) {
    double up = clampv(-me.y / 10.0, 0.15, 0.7);
    o = skill("blink", inward * (1.0 - up), up);
    o["move"] = double(inward);
    return true;
  }
  if (me.airJumps > 0 && me.vy < 0.05 && me.y < top - 5.0) {
    o["jump"] = true;
    return true;
  }
  double distX = std::fabs(me.x) - (cfg.half - cfg.hw);
  bool hookRoom = std::fabs(me.x) < bx - 8.0 && me.y > cfg.blastBot + 8.0;
  bool desperate = me.y < -6.0 || distX > 9.0 ||
                   (me.airJumps == 0 && me.vy < -0.25 && me.y < -2.5 &&
                    me.meter < cfg.costUpper + 0.05);
  if (hookRoom && desperate && canAct(me) && me.hookCd == 0 && me.meter >= cfg.costHook) {
    double aimY = clampv(me.y + cfg.bodyH / 2, -3.6, -0.6);
    double aimX = inward * (cfg.half - 0.05);
    o = skill("hook", aimX - me.x, aimY - (me.y + cfg.bodyH / 2));
    o["move"] = steer;
    return true;
  }
  if (canAct(me) && me.airJumps == 0 && me.vy < 0.0 && me.meter >= cfg.costUpper &&
      me.y < 9.0 && me.y < top - 6.0) {
    o = skill("uppercut", inward, 0.9);
    o["move"] = steer;
    return true;
  }
  if (canAct(me) && me.meter >= cfg.costBlink) {
    bool need = me.y < -10.5 || (me.airJumps == 0 && me.meter < cfg.costUpper && me.y < -3.5) ||
                distX > 11.0;
    if (need) {
      double up = clampv((-me.y - 2.0) / 12.0, 0.2, 0.7);
      o = skill("blink", inward * (1.0 - up), up);
      o["move"] = steer;
      return true;
    }
  }
  return true;
}

struct Threat {
  bool any = false, grab = false, slam = false, charge = false, strike = false, wave = false;
  int from = -1;
  double tClose = 99;
};

inline Threat scanThreats(const std::vector<P> &ps, const std::vector<ShotS> &shots, int seat) {
  const P &me = ps[seat];
  Threat th;
  double mx = me.x, my = me.y + cfg.bodyH / 2;
  for (int i = 0; i < int(ps.size()); ++i) {
    if (i == seat || ps[i].out || ps[i].respawn > 0) continue;
    const P &e = ps[i];
    for (const auto &s : shots) {
      if (s.owner != i) continue;
      double rx = s.x - mx, ry = s.y - my;
      double rvx = s.vx - me.vx, rvy = s.vy - me.vy;
      double rv2 = rvx * rvx + rvy * rvy;
      if (rv2 < 1e-9) continue;
      double tc = clampv(-(rx * rvx + ry * rvy) / rv2, 0.0, 24.0);
      double cx = rx + rvx * tc, cy = ry + rvy * tc;
      double md = len2(cx, cy);
      double reach = 0.5 + cfg.bodyH / 2 + 0.35;
      if (md < reach && tc < 22 && tc < s.ttl) {
        th.any = true;
        th.from = i;
        th.tClose = std::min(th.tClose, tc);
        if (s.kind == "hook") th.grab = true;
        else th.wave = true;
      }
    }
    if (e.act.empty() || e.phase < 2) continue;
    bool melee = e.act == "attack" || e.act == "charge" || e.act == "uppercut" ||
                 e.act == "slam" || e.act == "grab";
    if (!melee) continue;
    double off = 1.3, rad = 1.8;
    if (e.act == "grab") { off = 1.2; rad = 0.9 + cfg.bodyH / 2; }
    else if (e.act == "slam") { off = 0.9; rad = 3.0; }
    else if (e.act == "uppercut") { off = 1.2; rad = 1.1 + cfg.bodyH / 2; }
    else if (e.act == "charge") { off = 1.5; rad = 1.2 + cfg.bodyH / 2; }
    double hx = e.x + e.dx * off, hy = e.y + cfg.bodyH / 2 + e.dy * off;
    double d = len2(hx - mx, hy - my);
    int horizon = e.phase == 2 ? e.phaseT + 4 : e.phaseT + 1;
    if (d < rad + 0.8 && horizon > 0 && horizon < 22) {
      th.any = true;
      th.from = i;
      if (e.act == "grab") th.grab = true;
      else if (e.act == "slam") th.slam = true;
      else if (e.act == "charge") th.charge = true;
      else th.strike = true;
    }
  }
  return th;
}

inline int pickTarget(const std::vector<P> &ps, int seat) {
  int best = -1;
  double bestScore = -1e18;
  const P &me = ps[seat];
  for (int i = 0; i < int(ps.size()); ++i) {
    if (i == seat || ps[i].out || ps[i].respawn > 0 || ps[i].invuln > 0) continue;
    const P &e = ps[i];
    double dx = e.x - me.x, dy = e.y - me.y;
    double d = len2(dx, dy);
    double s = e.damage * 0.7;
    if (offstage(e)) s += 30;
    s += std::max(0.0, std::fabs(e.x) - 8.0) * 2.5;
    s += (3 - e.stocks) * 10.0;
    if (e.hitstun > 0) s += 20;
    if (e.phase == 4) s += 8;
    if (e.grounded) s += 4;                       // grounded targets are easier to combo
    if (e.hitstun > 0 && std::fabs(e.x) > 10 && (e.x * e.vx > 0)) s += 25;
    s -= d * 1.4;
    if (s > bestScore) { bestScore = s; best = i; }
  }
  return best;
}

inline void predictPos(const P &e, int t, double &px, double &py) {
  px = e.x + e.vx * t;
  double g = e.grounded ? 0 : cfg.grav;
  py = e.y + e.vy * t - 0.5 * g * t * t;
}

// an enemy windup we can punish with a counter
inline bool counterable(const P &e, double d, double ady) {
  if (e.phase != 2) return false;
  if (e.act != "attack" && e.act != "charge" && e.act != "uppercut") return false;
  return d <= 4.0 && std::fabs(ady) <= 2.2 && e.phaseT <= 12;
}

// ---------- main decision ----------
inline J decide(const J &obs) {
  J o = J::object{};
  const J &rules = obs.at("rules");
  loadCfg(rules);
  int seat = int(obs.at("seat").as_int());
  int tick = int(obs.at("tick").as_int());
  lastTick = tick;
  const auto &pj = obs.at("players").as_array();
  std::vector<P> ps;
  ps.reserve(pj.size());
  for (const auto &j : pj) ps.push_back(readP(j));
  std::vector<ShotS> shots;
  if (obs.contains("shots")) {
    for (const auto &s : obs.at("shots").as_array()) {
      ShotS sh;
      sh.owner = int(s.at("owner").as_int());
      sh.kind = s.at("kind").as_string();
      sh.x = s.at("x").as_double(); sh.y = s.at("y").as_double();
      sh.vx = s.at("vx").as_double(); sh.vy = s.at("vy").as_double();
      sh.ttl = int(s.at("ttl").as_int());
      shots.push_back(sh);
    }
  }
  const P &me = ps[seat];
  if (me.out || me.respawn > 0) return o;
  if (me.hitstun > 0) return o;

  double bx = blastXAt(tick), btop = blastTopAt(tick);
  bool late = btop < 14.0 || (me.stocks == 1 && tick > cfg.shrinkStart);

  // 1) survival first
  if (recovery(me, tick, o)) return o;

  // 2) airborne edge fence: never commit to an action while drifting near a side edge
  double edgeX = std::min(cfg.half - 4.0, bx - 5.0);
  if (!me.grounded && std::fabs(me.x) > edgeX) {
    int inward = me.x > 0 ? -1 : 1;
    bool under = me.y < -2.5 && std::fabs(me.x) < cfg.half - 1.0;
    o["move"] = under ? (me.x >= 0 ? 1.0 : -1.0) : double(inward);
    if (me.vy < 0.05 && me.airJumps > 0 && me.y < btop - 5.0) o["jump"] = true;
    return o;
  }
  // 3) high above the stage: drift back to centre, no rising moves
  if (!me.grounded && me.y > 14.0) {
    o["move"] = me.x > 1.0 ? -1.0 : (me.x < -1.0 ? 1.0 : 0.0);
    return o;
  }

  Threat th = scanThreats(ps, shots, seat);

  // pre-guard an enemy diving onto us from the air (slams are blockable but not counterable)
  if (canAct(me) && me.grounded) {
    for (int k = 0; k < int(ps.size()); ++k) {
      if (k == seat || ps[k].out || ps[k].respawn > 0 || ps[k].grounded) continue;
      double dd = len2(ps[k].x - me.x, ps[k].y - me.y);
      if (dd < 3.6 && ps[k].y > me.y + 1.2 && ps[k].vy < 0.1) {
        o["skill"] = "guard";
        return o;
      }
    }
  }
  // leave thin platforms for the main stage while enemies are near
  if (me.grounded && me.y > 1.0) {
    for (int k = 0; k < int(ps.size()); ++k) {
      if (k == seat || ps[k].out || ps[k].respawn > 0) continue;
      if (len2(ps[k].x - me.x, ps[k].y - me.y) < 6.0) { o["drop"] = true; break; }
    }
  }

  // continuing a charge: release as soon as the target is in range or the plan is done
  if (me.act == "charge" && me.phase == 1) {
    int ti = pickTarget(ps, seat);
    if (ti < 0) { o["skill"] = "attack"; chargePlan = 0; return o; }
    const P &e = ps[ti];
    double d = len2(e.x - me.x, e.y - me.y);
    if ((chargePlan > 0 && me.charge >= chargePlan) || d < 3.0 || me.charge >= 44) {
      o = skill("attack", e.x - me.x, e.y - me.y);
      chargePlan = 0;
    } else {
      o["skill"] = "charge";
      setDir(o, e.x - me.x, e.y - me.y);
      o["move"] = e.x > me.x ? 1.0 : -1.0;
    }
    return o;
  }
  chargePlan = 0;

  int ti = pickTarget(ps, seat);
  if (ti < 0) {
    double safeX = tick > cfg.shrinkStart ? bx * 0.6 : bx - 5.0;
    if (std::fabs(me.x) > safeX) o["move"] = me.x > 0 ? -1.0 : 1.0;
    return o;
  }
  const P &e = ps[ti];
  double px, py;
  predictPos(e, 5, px, py);
  double dx = px - me.x, dy = py - me.y;
  double adx = e.x - me.x, ady = e.y - me.y;
  double d = len2(adx, ady);
  bool guarding = e.act == "guard" && e.phase == 3;
  bool countering = e.act == "counter" && e.phase == 3;
  bool central = std::fabs(me.x) < cfg.half - 3.0;

  // 4) emergency defence (only for things an attack cannot beat)
  if (th.any && canAct(me)) {
    // grabs and hooks bypass guard and counter: dodge
    if (th.grab && th.tClose < 15) {
      if (me.meter >= cfg.costBlink + 0.1) {
        double bdx = th.from >= 0 ? me.x - ps[th.from].x : 0.0;
        if (std::fabs(me.x) > bx - 7) bdx = (me.x > 0 ? -1.0 : 1.0);
        o = skill("blink", bdx, 0.55);
        return o;
      }
      o["move"] = th.from >= 0 && ps[th.from].x < me.x ? 1.0 : -1.0;
      if (me.grounded) o["jump"] = true;
      return o;
    }
    // slams cannot be countered; a guard stops them, otherwise reposition
    if (th.slam) {
      if (me.grounded) { o["skill"] = "guard"; return o; }
      o["move"] = th.from >= 0 ? (ps[th.from].x < me.x ? 1.0 : -1.0) : (me.x > 0 ? -1.0 : 1.0);
      return o;
    }
    // reflect an incoming wave with a counter (doubled projectile), guard as fallback
    if (th.wave && !th.strike && !th.charge) {
      if (me.grounded && central && me.meter >= cfg.costCounter + 0.15 && th.tClose < 14) {
        o = skill("counter", th.from >= 0 ? ps[th.from].x - me.x : 1.0, 0.0);
        return o;
      }
      if (me.grounded) { o["skill"] = "guard"; return o; }
    }
  }

  // 5) punish windups with a counter: doubled knockback wins exchanges and secures kills
  if (canAct(me) && me.grounded && central && me.meter >= cfg.costCounter + 0.1 &&
      counterable(e, d, ady) && !th.grab) {
    o = skill("counter", adx, ady);
    return o;
  }

  // 6) approach and fight
  bool fence = std::fabs(me.x) > cfg.half - 2.5 &&
               (offstage(e) || (adx * (me.x >= 0 ? 1.0 : -1.0) > 0.0));
  if (fence) {
    o["move"] = me.x > 0 ? -1.0 : 1.0;  // never walk off the edge chasing
    if (th.any && canAct(me) && me.grounded) o["skill"] = "guard";
    return o;
  }
  o["move"] = dx > 0.4 ? 1.0 : (dx < -0.4 ? -1.0 : 0.0);

  if (canAct(me)) {
    // whiffed slam recovery (16 ticks) is a free punish
    if (e.act == "slam" && e.phase == 4 && d < 3.0 && std::fabs(ady) < 2.0) {
      o = skill("attack", adx, ady);
      return o;
    }
    // guard/counter stance in our face: grab beats both (throws ignore them)
    if ((guarding || countering) && d < 2.4 && std::fabs(ady) < 1.8 &&
        me.meter >= cfg.costGrab + 0.1) {
      o = skill("grab", me.x >= 0 ? 1.0 : -1.0, clampv(ady * 0.3, -0.5, 0.5));
      return o;
    }
    // grounded uppercut: tight range, low altitude, never chain rises into the top blast
    if (me.grounded && me.y < 5.0 && e.y - me.y >= 1.4 && e.y - me.y <= 3.2 &&
        std::fabs(adx) < 1.9 && me.meter >= cfg.costUpper + 0.1) {
      o = skill("uppercut", adx, 1.0);
      return o;
    }
    // reactive slam only: already airborne with a target clearly below (never self-hop into one)
    if (!me.grounded && me.y - e.y >= 2.0 && std::fabs(adx) < 2.6 && me.y < 10.0 &&
        std::fabs(me.x) < cfg.half - 3.0 && me.meter >= cfg.costSlam + 0.1 && !th.any) {
      o = skill("slam", adx, -1);
      return o;
    }

    // kill confirm on an enemy flying out
    if (e.hitstun > 0 && e.x * e.vx > 0 && std::fabs(e.x) > 10) {
      if (d < 2.8) { o = skill("attack", adx, ady); return o; }
      if (me.meter >= cfg.costWave + 0.15 && d < 24 && std::fabs(ady) < 8) {
        double kx, ky;
        predictPos(e, int(d / 0.75), kx, ky);
        o = skill("wave", kx - me.x, ky - me.y);
        return o;
      }
    }
    // bread-and-butter strike with target leading
    if (std::fabs(dx) <= 2.8 && std::fabs(dy) <= 1.9) {
      o = skill("attack", dx, dy);
      return o;
    }
    // short charge against a stuck nearby enemy (hold at least to the half-charge point)
    bool stuck = e.hitstun > 10 || (e.phase == 4 && e.phaseT > 10);
    if (stuck && d < 3.6 && std::fabs(ady) < 1.5 && central) {
      o["skill"] = "charge";
      setDir(o, adx, ady);
      chargePlan = std::min(40, std::max(23, e.hitstun + e.phaseT + 8));
      return o;
    }
    // edge guard an offstage enemy from the lip, without leaving the stage
    if (offstage(e) && me.grounded && me.damage < 110 && me.meter > 0.5 &&
        std::fabs(me.x) < cfg.half - 0.8) {
      if (me.meter >= cfg.costWave + 0.2 && d < 26 && std::fabs(ady) < 8 && me.y > -1) {
        double wx, wy;
        predictPos(e, int(d / 0.75), wx, wy);
        o = skill("wave", wx - me.x, wy - me.y);
        return o;
      }
      if (me.hookCd == 0 && d > 4 && d < 18 && std::fabs(ady) < 6 &&
          me.meter >= cfg.costHook + 0.1) {
        double hx, hy;
        predictPos(e, int(d / 0.7), hx, hy);
        o = skill("hook", hx - me.x, hy - me.y);
        return o;
      }
    }
    // hook to break a turtling or distant enemy
    if (me.hookCd == 0 && d > 5.0 && d < 15.0 && std::fabs(ady) < 3.2 &&
        me.meter >= cfg.costHook + 0.15 && (guarding || std::fabs(e.x) > 9)) {
      double hx, hy;
      predictPos(e, int(d / 0.7), hx, hy);
      o = skill("hook", hx - me.x, hy - me.y);
      return o;
    }
    // mid-range wave pressure with meter to spare
    if (d > 5.0 && d < 20.0 && std::fabs(ady) < 4.0 && me.y > -0.5 &&
        me.meter >= cfg.costWave + 0.2 && !countering) {
      double wx, wy;
      predictPos(e, int(d / 0.75), wx, wy);
      o = skill("wave", wx - me.x, wy - me.y);
      return o;
    }

  }
  return o;
}
}  // namespace mybot
