// Platform brawl bot: reactive defence + guaranteed punishes, jump-slam combos, edge-guard spikes.
#include "include/step_worker.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
using J = mini_json::Json;

namespace {
const double G = 0.035, MAXFALL = 0.8, RUN = 0.32, HALF = 0.6, H = 1.8, C = 0.9;
const double PLAT[3][3] = {{-8.5, -3.5, 4.5}, {3.5, 8.5, 4.5}, {-2.5, 2.5, 9.0}};

struct P {
  double x = 0, y = 0, vx = 0, vy = 0, dmg = 0, meter = 0, dx = 1, dy = 0;
  int facing = 1, aj = 0, stocks = 0, pt = 0, charge = 0, hitstun = 0, gap = 0, hookcd = 0, respawn = 0, invuln = 0, phase = 0;
  bool grounded = false, out = false;
  std::string act;
};
struct Shot { int owner; std::string kind; double x, y, vx, vy; int ttl; };

int g_blocks[8] = {0, 0, 0, 0, 0, 0, 0, 0};
double g_hit[8] = {0}, g_miss[8] = {0}, g_ghit[8] = {0}, g_gmiss[8] = {0};
struct Pend { bool on = false; int target = -1, tick = 0, T = 0, stocks = 0; double dmg = 0; bool grab = false; } g_pend;
double pStrike(int i) { return (1.2 + g_hit[i]) / (2.0 + g_hit[i] + 1.6 * g_miss[i]); }
double pGrab(int i) { double p = (0.9 + g_ghit[i]) / (2.0 + g_ghit[i] + 1.6 * g_gmiss[i]); if (g_miss[i] >= 1 && g_ghit[i] + g_gmiss[i] < 0.5) p = std::max(p, 0.6); return p; }
bool g_marked = false;
int g_chargeTarget = -1, g_chargeTurns = 0;
unsigned g_rng = 2463534242u;
const int HOLD_TICK = 4245;
double g_floor = 0.0;
std::vector<double> g_plan; int g_planTick = -100;
double rnd01() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng % 100000) / 100000.0; }

double blastX(int tick) { return tick > 3000 ? std::max(0.0, 26.0 - (tick - 3000) * 0.02) : 26.0; }
double blastTop(int tick) { return tick > 3000 ? std::max(14.0, 24.0 - (tick - 3000) * 0.018) : 24.0; }
double clampd(double v, double a, double b) { return std::max(a, std::min(b, v)); }

P parseP(const J &j) {
  P p;
  p.x = j.at("x").as_double(); p.y = j.at("y").as_double(); p.vx = j.at("vx").as_double(); p.vy = j.at("vy").as_double();
  p.dmg = j.at("damage").as_double(); p.meter = j.at("meter").as_double();
  p.facing = j.at("facing").get<int>(); p.aj = j.at("air_jumps").get<int>(); p.stocks = j.at("stocks").get<int>();
  p.pt = j.at("phase_ticks").get<int>(); p.charge = j.at("charge").get<int>(); p.hitstun = j.at("hitstun").get<int>();
  p.gap = j.at("gap").get<int>(); p.hookcd = j.at("hook_cd").get<int>(); p.respawn = j.at("respawn").get<int>();
  p.invuln = j.at("invuln").get<int>(); p.grounded = j.at("grounded").as_bool(); p.out = j.at("out").as_bool();
  p.act = j.at("act").is_null() ? std::string() : j.at("act").as_string();
  const std::string &ph = j.at("phase").as_string();
  p.phase = ph == "idle" ? 0 : ph == "charging" ? 1 : ph == "windup" ? 2 : ph == "active" ? 3 : 4;
  p.dx = j.at("dir").at(0).as_double(); p.dy = j.at("dir").at(1).as_double();
  return p;
}

struct Pos { double x, y; bool gr; };
// Where an enemy will be after T ticks if it does nothing new.
Pos predictE(const P &e, int T) {
  double x = e.x, y = e.y, vx = e.vx, vy = e.vy;
  bool gr = e.grounded;
  int hs = e.hitstun;
  bool locked = !e.act.empty() && e.act != "charge";
  for (int t = 1; t <= T; ++t) {
    if (gr) {
      if (hs > 0) { vx *= 0.97; --hs; }
      else if (locked) vx *= 0.75;
      else vx *= 0.85;
      x += vx;
      if (std::fabs(x) > 13.6 && y <= 0.01) gr = false;
      continue;
    }
    vy = std::max(vy - G, hs > 0 ? -2.0 : -MAXFALL);
    if (hs > 0) { vx *= 0.97; vy *= 0.97; --hs; } else vx *= 0.985;
    x += vx;
    double foot = y;
    y += vy;
    if (vy <= 0) {
      if (foot >= 0 && y < 0 && std::fabs(x) < 13.6) { y = 0; vy = 0; gr = true; }
      else for (auto &pl : PLAT) if (foot >= pl[2] && y <= pl[2] && x + HALF > pl[0] && x - HALF < pl[1]) { y = pl[2]; vy = 0; gr = true; break; }
    }
  }
  return {x, y, gr};
}

struct LR { bool kill; double x, y; int stun; };
LR simLaunch(double x, double y, double dx, double dy, double dmgAfter, double power, int tick) {
  double l = std::sqrt(dx * dx + dy * dy);
  if (l < 1e-9) { dx = 1; dy = 0; l = 1; }
  dx /= l; dy /= l;
  double sp = (0.3 + 1.1 * dmgAfter / 100) * power, vx = dx * sp, vy = dy * sp;
  int stun = std::min(45, 6 + int(sp * 28));
  LR r; r.kill = false; r.stun = stun;
  bool gr = false;
  for (int t = 1; t <= stun + 5; ++t) {
    bool hs = t <= stun;
    if (gr) {
      vx *= hs ? 0.97 : 0.75;
      x += vx;
      if (std::fabs(x) > 13.6 && y <= 0.01 && y >= -0.01) gr = false;
      else { bool on = y <= 0.01; if (!on) { on = false; for (auto &pl : PLAT) if (std::fabs(y - pl[2]) < 0.01 && x + HALF > pl[0] && x - HALF < pl[1]) on = true; if (!on) gr = false; } }
    } else {
      vy = std::max(vy - G, hs ? -2.0 : -MAXFALL);
      if (hs) { vx *= 0.97; vy *= 0.97; } else vx *= 0.985;
      double nx = x + vx;
      if (nx + HALF > -13 && nx - HALF < 13 && y < 0 && y + H > -4) { nx = vx > 0 ? -13 - HALF : 13 + HALF; vx = hs ? -vx * 0.6 : 0; }
      x = nx;
      double foot = y;
      y += vy;
      if (x + HALF > -13 && x - HALF < 13 && y < 0 && y + H > -4) {
        if (vy <= 0) { y = 0; vy = 0; gr = true; } else { y = -4 - H; vy = hs ? -vy * 0.6 : 0; }
      }
      if (vy <= 0 && !gr) for (auto &pl : PLAT) if (foot >= pl[2] && y <= pl[2] && x + HALF > pl[0] && x - HALF < pl[1]) { y = pl[2]; vy = 0; gr = true; break; }
    }
    if (std::fabs(x) > blastX(tick + t) || y > blastTop(tick + t) || y < -16) { r.kill = true; break; }
  }
  r.x = x; r.y = y;
  return r;
}

// First tick (>=1) at which the enemy's current strike can reach my body, or -1.
int strikeThreat(const P &e, const P &me, double margin, int horizon = 11) {
  const std::string &a = e.act;
  double off, reach; int dur;
  if (a == "attack") { off = 1.3; reach = 1.8; dur = 3; }
  else if (a == "charge") { if (e.phase == 1) return -1; off = 1.5; reach = 2.1; dur = 3; }
  else if (a == "grab") { off = 1.2; reach = 1.8; dur = 3; }
  else if (a == "uppercut") { off = 1.2; reach = 2.0; dur = 6; }
  else if (a == "slam") { off = 0.9; reach = 3.9; dur = 30; }
  else return -1;
  if (e.phase != 2 && e.phase != 3) return -1;
  int tA = e.phase == 2 ? e.pt : 0;
  int last = e.phase == 2 ? tA + dur - 1 : e.pt - 1;
  double x = e.x, y = e.y, vx = e.vx, vy = e.vy; bool gr = e.grounded;
  double mx = me.x, my = me.y, mvx = me.vx, mvy = me.vy; bool mgr = me.grounded;
  for (int t = 1; t <= std::min(horizon, last); ++t) {
    if (t == tA) {
      if (a == "uppercut") { vy = 0.75; gr = false; }
      if (a == "slam") { vy = -0.95; vx *= 0.3; }
    }
    if (!gr) {
      vy = std::max(vy - G, -MAXFALL); vx *= 0.985; x += vx; double foot = y; y += vy;
      if (vy <= 0 && foot >= 0 && y < 0 && std::fabs(x) < 13.6) { y = 0; vy = 0; gr = true; }
      if (vy <= 0 && !gr) for (auto &pl : PLAT) if (foot >= pl[2] && y <= pl[2] && x + HALF > pl[0] && x - HALF < pl[1]) { y = pl[2]; vy = 0; gr = true; break; }
    } else { vx *= 0.75; x += vx; }
    if (!mgr) {
      mvy = std::max(mvy - G, -MAXFALL); mvx *= 0.985; mx += mvx; double foot = my; my += mvy;
      if (mvy <= 0 && foot >= 0 && my < 0 && std::fabs(mx) < 13.6) { my = 0; mvy = 0; mgr = true; }
    }
    if (t >= tA) {
      double hx, hy;
      if (a == "uppercut") { hx = x; hy = y + C + 1.2; }
      else if (a == "slam") { hx = x; hy = y; }
      else { hx = x + off * e.dx; hy = y + C + off * e.dy; }
      double ddx = mx - hx, ddy = my + C - hy, r = reach + margin;
      if (ddx * ddx + ddy * ddy <= r * r) return t;
      if (a == "slam" && gr) break;
    }
  }
  return -1;
}

// Recovery simulation. action: 0 none, 1 jump now, 2 uppercut, 3 blink(bdx,bdy). Later: auto air jump when low.
bool simRec(double x, double y, double vx, double vy, int aj, int action, double bdx, double bdy, double sgn, int tick) {
  int lock = 0, blinkT = 0;
  if (action == 1) { if (aj > 0) { vy = 0.55; --aj; } }
  else if (action == 2) lock = 19;
  else if (action == 3) { blinkT = 5; lock = 9; vx = bdx * 1.2; vy = bdy * 1.2; }
  for (int t = 1; t <= 260; ++t) {
    if (std::fabs(x) > blastX(tick + t) - 0.3 || y < -15.6 || y > blastTop(tick + t) - 0.3) return false;
    if (blinkT > 0) {
      double nx = x + vx;
      if (y < 0 && y + H > -4 && std::fabs(nx) < 13.6) nx = sgn * 13.6;
      x = nx; double foot = y; y += vy;
      if (std::fabs(x) < 13.6 && y < 0 && y + H > -4) { if (foot >= 0) return std::fabs(x) < 13.45; y = -4 - H; }
      --blinkT; --lock;
      if (blinkT == 0) { vx *= 0.2; vy *= 0.2; }
      continue;
    }
    if (action == 2 && t == 4) vy = 0.75;
    if (lock > 0) --lock;
    else {
      if (aj > 0 && vy <= 0 && y < 0.5) { vy = 0.55; --aj; }
      double want = -sgn * RUN;
      vx += clampd(want - vx, -0.03, 0.03);
    }
    vy = std::max(vy - G, -MAXFALL); vx *= 0.985;
    double nx = x + vx;
    if (y < 0 && y + H > -4 && std::fabs(nx) < 13.6) { nx = sgn * 13.6; vx = 0; }
    x = nx; double foot = y; y += vy;
    if (std::fabs(x) < 13.6 && y < 0 && y + H > -4) {
      if (vy <= 0 && foot >= 0) return std::fabs(x) < 13.45;
      if (vy > 0) { y = -4 - H; vy = 0; } else return false;
    }
    if (y < -7 && vy < 0 && aj == 0) return false;
  }
  return false;
}

struct MS { double x, vx, y, vy; bool gr; };
inline double rnd4(double v) { return std::round(v * 10000.0) / 10000.0; }
// Exact replica of the engine's free movement (no action, no jump) for one turn of 3 ticks near the stage centre.
void stepTurn(MS &s, double move) {
  for (int k = 0; k < 3; ++k) {
    double want = move * RUN, a = s.gr ? 0.08 : 0.03;
    s.vx += std::max(-a, std::min(a, want - s.vx));
    s.vy = std::max(s.vy - G, -MAXFALL);
    if (!s.gr) s.vx *= 0.985;
    s.x += s.vx;
    s.y += s.vy;
    bool g = false;
    if (s.y < g_floor) { s.y = g_floor; s.vy = 0; g = true; }
    s.gr = g;
    s.x = rnd4(s.x); s.y = rnd4(s.y); s.vx = rnd4(s.vx); s.vy = rnd4(s.vy);
  }
}

// sm-1.3: a short charge launches less (0.85 at no charge, back to 1 at half of the full charge time)
inline double shortMult(double chargeTicks) { double rec = std::min(1.0, (chargeTicks / 45.0) / 0.5); return 1 + (0.85 - 1) * (1 - rec); }

struct Cand { std::string skill; double dx = 1, dy = 0; bool jump = false; int T = 0; double score = -1e9; bool kill = false; int k = 0; int target = -1; bool sure = false; double third = 0; };

struct Ctx {
  std::vector<P> ps; std::vector<Shot> shots; int seat = 0, tick = 0; P me; int g = 0;
};

void lockInfo(const P &e, int &lockAny, int &counterUntil, bool &guarding) {
  lockAny = 0; counterUntil = 0; guarding = false;
  if (e.hitstun > 0) {
    lockAny = e.hitstun;
    return;
  }
  const std::string &a = e.act;
  if (a.empty()) { lockAny = e.gap; return; }
  if (a == "guard") { guarding = true; return; }
  if (a == "charge" && e.phase == 1) return;
  int W = 0, A = 0, R = 0;
  if (a == "attack") { A = 3; R = 6; } else if (a == "charge") { A = 3; R = 16; } else if (a == "grab") { A = 3; R = 12; }
  else if (a == "uppercut") { A = 6; R = 10; } else if (a == "slam") { A = 3; R = 16; } else if (a == "wave") { A = 0; R = 10; }
  else if (a == "counter") { A = 12; R = 16; } else if (a == "hook") { A = 1; R = 6; } else if (a == "blink") { A = 5; R = 4; }
  (void)W;
  if (e.phase == 2) lockAny = e.pt + A + R + 4;
  else if (e.phase == 3) lockAny = (a == "hook" ? 1 : e.pt) + R + 4;
  else if (e.phase == 4) lockAny = e.pt + 4;
  if (a == "counter") { if (e.phase == 2) counterUntil = e.pt + 12; else if (e.phase == 3) counterUntil = e.pt; }
  if (a == "blink" && e.phase == 3) counterUntil = e.pt;  // untouchable until then
}

bool landsOnStage(double x) { return std::fabs(x) < 12.9; }

// Evaluate my best strike on enemy e.
void evalTarget(const Ctx &c, int idx, std::vector<Cand> &out, bool chargingNow) {
  const P &me = c.me, &e = c.ps[idx];
  if (e.out || e.respawn > 0) return;
  int lockAny, counterUntil; bool guarding;
  lockInfo(e, lockAny, counterUntil, guarding);
  int g = c.g;
  double thirdPen = 0, farThird = 0;
  for (int j = 0; j < int(c.ps.size()); ++j) {
    if (j == idx || j == c.seat) continue;
    const P &q = c.ps[j];
    if (q.out || q.respawn > 0) continue;
    if (std::fabs(q.x - me.x) < 6.5 && std::fabs(q.y - me.y) < 5 && q.hitstun < 12) thirdPen += 1.0;
    // a free fighter a little further out walks in and punishes a long recovery (a slam would not reach him)
    if (std::fabs(q.x - me.x) >= 3.4 && std::fabs(q.x - me.x) < 8.0 && std::fabs(q.y - me.y) < 5 && q.hitstun < 6 && q.invuln == 0) farThird += 1.0;
  }
  bool eOff = std::fabs(e.x) > 13.6 || e.y < -0.3;
  auto finish = [&](Cand cd, double dmg, double power, double ldx, double ldy, const Pos &ep, double cost, bool strike, bool sure) {
    if (e.invuln >= cd.T) return;
    double dmgAfter = e.dmg + dmg;
    LR r = simLaunch(ep.x, ep.y, ldx, ldy, dmgAfter, power, c.tick + cd.T);
    double s = dmg;
    if (r.kill) s += (c.tick > 3700 ? 120 : 400);
    else {
      bool off = std::fabs(r.x) > 13.6 || r.y < -0.3;
      if (off) s += 25 + 2.0 * std::max(0.0, std::fabs(r.x) - 13.6) + 3.0 * std::max(0.0, -r.y);
    }
    if (cd.skill == "slam") {
      bool vgr = ep.gr || (ep.y < 1.5 && std::fabs(ep.x) < 13.4 && ep.y >= 0);
      if (vgr && !eOff) s -= 16 * (1 + me.dmg / 60.0);  // sm-1.3: landing caps the stun at 15, the victim acts 5-7 ticks before me
    }
    double p = 1.0;
    if (!sure) {
      if (guarding) p = strike ? 0.0 : 0.75;
      else if (strike && counterUntil >= cd.T && cd.skill != "slam") p = 0.0;
      else if (cd.skill == "slam" && counterUntil >= cd.T && e.act == "counter") p = 0.95;
      else p = strike ? pStrike(idx) : pGrab(idx);
      if (cd.skill == "charge" || cd.skill == "uppercut") p = 0.0;
      if (c.tick > 3600 && !guarding && !(e.act == "counter" && cd.skill == "slam") && (strike ? g_hit[idx] : g_ghit[idx]) < 0.5) p = 0.0;  // late: no unproven gambles
      if (e.act == "blink" && e.phase == 3) p = 0.0;
      double risk = (cd.skill == "attack" ? 14 : 30) * (1 + me.dmg / 60.0);
      s = s * p - (1 - p) * risk;
    }
    bool late = c.tick > 3700;
    if (!r.kill && !late) s -= thirdPen * (cd.skill == "attack" ? 1.5 : cd.skill == "uppercut" ? 4.0 : 5.0) * (1 + me.dmg / 50.0);
    if (!r.kill && !late && cd.skill != "attack") s -= farThird * 20.0 * (1 + me.dmg / 40.0) * (1 + std::max(0.0, std::fabs(me.x) - 5.0) / 4.0);
    if (late) s -= thirdPen * (cd.skill == "attack" ? 6.0 : cd.skill == "grab" ? 45.0 : 60.0);
    cd.third = thirdPen;
    s -= cost * 25;
    if (me.meter - cost < 0.2 && !(r.kill && p > 0.9)) s -= 12;
    s -= cd.T * 0.4;
    cd.score = s; cd.kill = r.kill; cd.target = idx; cd.sure = sure;
    out.push_back(cd);
  };
  auto myGroundX = [&](int T) { return me.grounded ? me.x + me.vx * 3 * (1 - std::pow(0.75, T)) : me.x + me.vx * T; };
  auto myY = [&](int T) { if (me.grounded) return me.y; double y = me.y + me.vy * T - G * T * (T + 1) / 2; if (y < 0 && std::fabs(me.x) < 13.6 && me.y >= 0) y = 0; return y; };
  // --- directional strikes: attack, grab, charge
  struct DS { const char *name; int T; double off, reach, dmg, power, lift, cost; int k; };
  std::vector<DS> ds;
  if (!chargingNow) {
    ds.push_back({"attack", g + 5, 1.3, 1.8, 7, 0.8, 0.5, 0.0, 0});
    if (me.meter >= 0.3) ds.push_back({"grab", g + 7, 1.2, 1.8, 9, 1.1, 0.3, 0.3, 0});
    for (int k = 1; k <= 8; ++k) { double c = 3 * k - 1, m = 1 + c / 45.0; ds.push_back({"charge", g + 3 * k + 9, 1.5, 2.1, 12 * m, 1.2 * m * shortMult(c), 0.5, 0.0, k}); }
  } else {
    double m0 = 1 + std::min(1.0, me.charge / 45.0);
    ds.push_back({"release", 9, 1.5, 2.1, 12 * m0, 1.2 * m0 * shortMult(me.charge), 0.5, 0.0, 0});
  }
  for (auto &d : ds) {
    bool strike = std::string(d.name) != "grab";
    bool sure = d.T <= lockAny && (!strike || d.T > counterUntil);
    if (std::string(d.name) == "grab" && counterUntil > 0 && e.act == "counter" && d.T <= lockAny) sure = true;
    if ((std::string(d.name) == "charge" || std::string(d.name) == "release") && !sure && !chargingNow) continue;
    Pos ep = predictE(e, d.T);
    double mx = myGroundX(d.T), my = myY(d.T);
    double ddx = ep.x - mx, ddy = ep.y - my;
    bool still = sure && (ep.gr || e.hitstun == 0) && std::fabs(e.vx) < 0.05 && std::fabs(e.vy) < 0.05;
    double margin = still ? 0.15 : (sure ? 0.45 : 0.6);
    double dist = std::sqrt(ddx * ddx + ddy * ddy);
    std::vector<std::pair<double, double>> dirs;
    if (dist > 1e-6) dirs.push_back({ddx / dist, ddy / dist});
    for (double sx : {1.0, -1.0}) { dirs.push_back({sx, 0}); dirs.push_back({sx * 0.866, -0.5}); dirs.push_back({sx * 0.954, -0.3}); dirs.push_back({sx * 0.8, 0.6}); dirs.push_back({sx * 0.6, -0.8}); }
    Cand best; bool any = false; std::vector<Cand> tmp;
    for (auto &dr : dirs) {
      double hx = d.off * dr.first, hy = d.off * dr.second;
      double q = std::sqrt((ddx - hx) * (ddx - hx) + (ddy - hy) * (ddy - hy));
      if (q > d.reach - margin) continue;
      Cand cd; cd.skill = d.name; cd.dx = dr.first; cd.dy = dr.second; cd.T = d.T; cd.k = d.k;
      // a free bystander with meter inside this swing can counter a slow charge: twice the blow comes back
      double ctrPen = 0;
      if (d.T >= 9 + g && strike && d.reach > 2.0) {
        for (int j = 0; j < int(c.ps.size()); ++j) {
          if (j == idx || j == c.seat) continue;
          const P &q2 = c.ps[j];
          if (q2.out || q2.respawn > 0 || q2.invuln > 0 || q2.meter < 0.28) continue;
          int la, cu; bool gd; lockInfo(q2, la, cu, gd);
          if (la >= d.T - 3) continue;
          double bx2 = q2.x - (mx + hx), by2 = q2.y - (my + hy);
          if (bx2 * bx2 + by2 * by2 <= (d.reach + 0.7) * (d.reach + 0.7)) ctrPen = 260;
          // he can also walk into the swing during the 8 tick windup (replays: 3 of 8 such releases were countered, none killed)
          double mx2 = q2.x - me.x, my2 = q2.y - me.y;
          if (la <= d.T - 4 && mx2 * mx2 + my2 * my2 < 6.5 * 6.5) ctrPen = 260;
        }
      }
      size_t before = out.size();
      finish(cd, d.dmg, d.power, dr.first, dr.second + d.lift, ep, d.cost, strike, sure);
      if (out.size() > before) { Cand r = out.back(); out.pop_back(); r.score -= q * 0.5 + ctrPen; if (!any || r.score > best.score) { best = r; any = true; } }
    }
    if (any) out.push_back(best);
  }
  if (chargingNow) return;
  // --- slam
  if (me.meter >= 0.2) {
    int s0 = me.grounded ? std::max(2, g + 1) : g + 1, a = s0 + 3;
    double x = me.x, y = me.y, vx = me.vx, vy = me.grounded ? 0.62 : me.vy;
    bool ok = false; int hitT = -1; Pos hp{0, 0, false};
    double baseY = me.grounded ? me.y : -100;
    for (int t = 1; t <= a + 12; ++t) {
      if (t < a) { vy -= G; if (vy < -MAXFALL) vy = -MAXFALL; vx *= 0.985; x += vx; y += vy; }
      else { if (t == a) vx *= 0.3; x += vx; y -= 0.8; }
      if (t >= a) {
        Pos ep = predictE(e, t);
        double ddx = ep.x - x, ddy = ep.y + C - y;
        bool sureT = t <= lockAny;
        double margin = sureT ? 0.3 : 0.55;
        if (hitT < 0 && ddx * ddx + ddy * ddy <= (3.9 - margin) * (3.9 - margin)) { hitT = t; hp = ep; }
        double floorY = (std::fabs(x) < 13.6) ? 0.0 : -100.0;
        if (baseY > floorY) floorY = baseY;
        if (y <= floorY) { ok = landsOnStage(x) ; break; }
        if (y < -3) { ok = false; break; }
      } else if (t > 1 && y < 0 && std::fabs(x) < 13.6 && !me.grounded) { ok = false; break; }
    }
    if (ok && hitT > 0) {
      Cand cd; cd.skill = "slam"; cd.jump = me.grounded; cd.T = hitT; cd.dx = 0; cd.dy = -1;
      bool sure = hitT <= lockAny;
      finish(cd, 13, 1.2, 0, -1, hp, 0.2, true, sure);
    }
  }
  // --- uppercut (only when sure)
  if (me.meter >= 0.25) {
    int a = g + 4;
    double y = myY(a - 1), vy = 0.75, x = myGroundX(a);
    for (int t = a; t <= a + 5; ++t) {
      vy -= G; y += vy;
      Pos ep = predictE(e, t);
      double ddx = ep.x - x, ddy = ep.y + C - (y + C + 1.2);
      bool sure = t <= lockAny && t > counterUntil;
      if (!sure) break;
      if (ddx * ddx + ddy * ddy <= (2.0 - 0.3) * (2.0 - 0.3)) {
        Cand cd; cd.skill = "uppercut"; cd.T = t; cd.dx = 0; cd.dy = 1;
        size_t before = out.size();
        finish(cd, 11, 1.1, 0, 1, ep, 0.25, true, true);
        if (out.size() > before && !out.back().kill) out.back().score -= 12;  // leaves me airborne and locked
        break;
      }
    }
  }
}

J decideRaw(const J &obs);
J decide(const J &obs) {
  J o = decideRaw(obs);
  const J &mej = obs.at("players").at(obs.at("seat").get<int>());
  double y = mej.at("y").as_double(); bool gr = mej.at("grounded").as_bool(); int tick = obs.at("tick").get<int>();
  double top = blastTop(tick + 30) - 0.5;
  if (o.contains("jump") && o.at("jump").as_bool() && y + (gr ? 5.6 : 4.4) > top) o.erase("jump");
  if (o.contains("skill")) {
    const std::string sk = o.at("skill").as_string();
    if (sk == "uppercut" && y + 8.3 > top) o.erase("skill");
    else if (sk == "blink" && o.contains("dir")) {
      double dx = o.at("dir").at(0).as_double(), dy = o.at("dir").at(1).as_double(), l = std::sqrt(dx * dx + dy * dy);
      if (l > 1e-9 && y + 6.6 * dy / l > top) o.erase("skill");
    }
  }
  return o;
}
J decideRaw(const J &obs) {
  Ctx c;
  c.seat = obs.at("seat").get<int>(); c.tick = obs.at("tick").get<int>();
  for (const auto &pj : obs.at("players").as_array()) c.ps.push_back(parseP(pj));
  for (const auto &sj : obs.at("shots").as_array()) {
    Shot s; s.owner = sj.at("owner").get<int>(); s.kind = sj.at("kind").as_string(); s.x = sj.at("x").as_double(); s.y = sj.at("y").as_double();
    s.vx = sj.at("vx").as_double(); s.vy = sj.at("vy").as_double(); s.ttl = sj.at("ttl").get<int>(); c.shots.push_back(s);
  }
  c.me = c.ps[c.seat];
  const P &me = c.me;
  int n = int(c.ps.size());
  J o = J::object{};
  if (me.out || me.respawn > 0) return o;
  int tick = c.tick;
  double bx = blastX(tick + 45);
  bool charging = me.act == "charge" && me.phase == 1;
  bool guardingNow = me.act == "guard";
  bool busy = !me.act.empty() && !charging && !guardingNow;
  int g = me.hitstun > 0 ? me.hitstun : me.gap;
  c.g = g;
  bool canSkill = !busy && g <= 2;
  double sgn = me.x >= 0 ? 1 : -1;
  // book-keeping: who blocks my strikes
  if (me.act.empty()) g_marked = false;
  else if (!g_marked && (me.act == "attack" || me.act == "slam" || me.act == "charge" || me.act == "uppercut") && (me.phase == 3 || me.phase == 4)) {
    for (int i = 0; i < n; ++i) if (i != c.seat && !c.ps[i].out && (c.ps[i].act == "guard" || c.ps[i].act == "counter") && std::fabs(c.ps[i].x - me.x) < 4.5 && std::fabs(c.ps[i].y - me.y) < 4.5) { ++g_blocks[i]; g_marked = true; }
  }
  if (g_pend.on) {
    const P &t = c.ps[g_pend.target];
    bool hit = t.dmg > g_pend.dmg + 1.0 || t.stocks < g_pend.stocks;
    if (hit) { (g_pend.grab ? g_ghit : g_hit)[g_pend.target] += 1; g_pend.on = false; }
    else if (tick > g_pend.tick + g_pend.T + 4) { (g_pend.grab ? g_gmiss : g_miss)[g_pend.target] += 1; g_pend.on = false; }
  }
  bool hold = tick >= HOLD_TICK;
  bool offstage = !me.grounded && (std::fabs(me.x) > 13.3 || me.y < -0.05);
  double toCentre = me.x > 0 ? -1.0 : 1.0;
  if (busy || me.hitstun > 2 || (!busy && g > 2 && offstage)) {
    o["move"] = offstage ? -sgn : (hold ? 0.0 : (std::fabs(me.x) > 9 ? toCentre : 0.0));
    if (!busy && me.hitstun == 0 && offstage && me.aj > 0 && me.vy <= 0 && me.y < 0.5 && !simRec(me.x, me.y, me.vx, me.vy, 0, 0, 0, 0, sgn, tick)) o["jump"] = true;
    return o;
  }

  // ---------------- threats ----------------
  int tStrike = 99, tStrikeTight = 99, tGrab = 99, tSlam = 99, grabber = -1;
  for (int i = 0; i < n; ++i) {
    if (i == c.seat) continue;
    const P &e = c.ps[i];
    if (e.out || e.respawn > 0) continue;
    int t = strikeThreat(e, me, 0.55);
    if (t < 0) continue;
    if (e.act == "grab") { if (t < tGrab) { tGrab = t; grabber = i; } }
    else if (e.act == "slam") tSlam = std::min(tSlam, t);
    else { tStrike = std::min(tStrike, t); int t2 = strikeThreat(e, me, 0.1); if (t2 > 0) tStrikeTight = std::min(tStrikeTight, t2); }
  }
  int tWave = 99, tHook = 99; double hookVx = 0, hookVy = 0;
  for (auto &s : c.shots) {
    if (s.owner == c.seat) continue;
    double x = s.x, y = s.y;
    for (int t = 1; t <= std::min(s.ttl, 9); ++t) {
      x += s.vx; y += s.vy;
      double ddx = me.x + me.vx * t * (me.grounded ? 0.3 : 1.0) - x, ddy = me.y + C - y + (me.grounded ? 0 : me.vy * t);
      if (ddx * ddx + ddy * ddy <= 1.9 * 1.9) { if (s.kind == "wave") tWave = std::min(tWave, t); else { if (t < tHook) { tHook = t; hookVx = s.vx; hookVy = s.vy; } } break; }
    }
  }
  for (int i = 0; i < n; ++i) {
    if (i == c.seat) continue;
    const P &e = c.ps[i];
    if (e.out || e.respawn > 0 || e.act != "wave" || e.phase != 2) continue;
    double sx = e.x + e.dx * 1.1, sy = e.y + C + e.dy * 1.1;
    for (int t = e.pt; t <= e.pt + 6; ++t) {
      sx += e.dx * 0.75; sy += e.dy * 0.75;
      double ddx = me.x - sx, ddy = me.y + C - sy;
      if (ddx * ddx + ddy * ddy <= 1.9 * 1.9) { tWave = std::min(tWave, std::max(1, t)); break; }
    }
  }
  if (me.invuln > 12) { tStrike = tStrikeTight = tGrab = tSlam = tWave = tHook = 99; }
  auto blinkAway = [&](double awayFrom) {
    double dir = me.x >= awayFrom ? 1 : -1;
    double bxs = dir, bys = 0.25;
    if (hold && !offstage) { bxs = 0; bys = 1; }
    else if (offstage) { bxs = -sgn; bys = 0.5; }
    else if (std::fabs(me.x + dir * 6.5) > std::min(12.0, bx - 4)) { bxs = 0; bys = 1; }
    o["skill"] = "blink"; o["dir"] = J::array{bxs, bys};
  };
  if (canSkill || guardingNow || charging) {
    bool actNow = canSkill || charging;   // can start a skill (blink/guard cancel charge instantly)
    if (!offstage && ((hold && me.grounded) || (tick > 3600 && !me.grounded && tGrab < 99)) && (tGrab < 99 || tHook < 99)) {
      // in-place dodge: a blink aimed into the floor keeps x exactly and is untouchable for 5 ticks
      int tg = std::min(tGrab, tHook);
      if (tStrike <= tg || tSlam <= tg) { o["skill"] = "guard"; return o; }
      bool nowOk = tHook >= g + 1 && tHook <= g + 5;
      if (tGrab < 99) { const P &gb = c.ps[grabber]; nowOk = (gb.phase == 3 && g == 0) || (gb.phase == 2 && gb.pt >= g + 1 && gb.pt <= g + 3); }
      if (nowOk && actNow && !guardingNow && me.meter >= 0.2) { o["skill"] = "blink"; o["dir"] = J::array{0.0, -1.0}; return o; }
      o["move"] = 0.0;
      return o;  // wait: the blink has to cover the active ticks
    }
    if (tGrab < 99) {
      if (actNow && !guardingNow && me.meter >= 0.2 && tGrab > g && (offstage || (hold ? me.y < 0.5 : me.y < 4.0))) { blinkAway(c.ps[grabber].x); return o; }
      // no meter (or guarding): run and jump away
      o["move"] = me.x >= c.ps[grabber].x ? 1.0 : -1.0;
      if (std::fabs(me.x) > 11.5) o["move"] = toCentre;
      if (hold) { o["move"] = 0.0; if (me.grounded && me.y < 0.5) o["jump"] = true; return o; }
      o["jump"] = true;
      return o;
    }
    if (tHook < 99 && actNow && !guardingNow && me.meter >= 0.2 && tHook > g && (me.y < 4.0 || offstage)) {
      double bxs = 0, bys = 1;
      if (std::fabs(hookVy) > std::fabs(hookVx) && !hold) { bxs = offstage ? -sgn : toCentre; bys = 0; }
      if (offstage) { bxs = -sgn; bys = 0.6; }
      o["skill"] = "blink"; o["dir"] = J::array{bxs, bys};
      return o;
    }
    int tAny = std::min(tStrike, std::min(tSlam, tWave));
    if (hold && guardingNow && me.grounded && std::fabs(me.vx) > 1e-9) { o["skill"] = "guard"; return o; }  // let the guard cancel the pushback
    if (tAny < 99) {
      bool counterOk = !guardingNow && !charging && me.meter >= 0.3 && tSlam == 99 &&
                       ((tStrikeTight < 99 && tStrikeTight >= g + 4 && tStrikeTight <= g + 13 && tStrike >= g + 4) || (tStrike == 99 && tWave >= g + 4 && tWave <= g + 13));
      if (counterOk && canSkill) { o["skill"] = "counter"; return o; }
      if (tAny > g || guardingNow) { o["skill"] = "guard"; if (offstage) o["move"] = -sgn; return o; }
    }
  }

  // ---------------- recovery ----------------
  if (offstage) {
    o["move"] = -sgn;
    if (simRec(me.x, me.y, me.vx, me.vy, me.aj, 0, 0, 0, sgn, tick)) {
      if (me.aj > 0 && me.vy <= 0 && me.y < 0.5) o["jump"] = true;
      return o;
    }
    if (canSkill) {
      if (me.meter >= 0.2) {
        double tx = sgn * 12.0 - me.x, ty = 1.0 - me.y, l = std::sqrt(tx * tx + ty * ty);
        std::vector<std::pair<double, double>> dirs = {{tx / l, ty / l}, {-sgn, 0.0}, {-sgn * 0.8, 0.6}, {-sgn * 0.6, 0.8}, {-sgn * 0.3, 0.954}, {0.0, 1.0}};
        for (auto &d : dirs) if (simRec(me.x, me.y, me.vx, me.vy, me.aj, 3, d.first, d.second, sgn, tick)) { o["skill"] = "blink"; o["dir"] = J::array{d.first, d.second}; return o; }
      }
      if (me.meter >= 0.25 && simRec(me.x, me.y, me.vx, me.vy, me.aj, 2, 0, 0, sgn, tick)) { o["skill"] = "uppercut"; return o; }
      if (me.meter >= 0.4 && me.hookcd == 0) {
        double tx, ty;
        if (me.y > 0.3) { tx = sgn * 11.5; ty = -0.25; } else { tx = sgn * 12.7; ty = std::min(-0.4, std::max(-3.5, me.y + 0.9)); }
        double ddx = tx - me.x, ddy = ty - (me.y + C), l = std::sqrt(ddx * ddx + ddy * ddy);
        if (l < 18.5) { o["skill"] = "hook"; o["dir"] = J::array{ddx / l, ddy / l}; return o; }
      }
      if (me.aj > 0 && me.vy <= 0.05) { o["jump"] = true; return o; }
      if (me.meter >= 0.25 && me.vy < 0 && std::fabs(me.x) < 16.5) { o["skill"] = "uppercut"; return o; }
      if (me.meter >= 0.2) { double tx = sgn * 12.0 - me.x, ty = 1.5 - me.y, l = std::sqrt(tx * tx + ty * ty); o["skill"] = "blink"; o["dir"] = J::array{tx / l, ty / l}; return o; }
    }
    if (me.aj > 0 && me.vy <= 0.05) o["jump"] = true;
    return o;
  }

  // ---------------- charging: continue or release ----------------
  if (charging) {
    std::vector<Cand> cs;
    int t = g_chargeTarget;
    if (t >= 0 && t < n && t != c.seat) evalTarget(c, t, cs, true);
    Cand rel; bool has = false;
    for (auto &cd : cs) if (!has || cd.score > rel.score) { rel = cd; has = true; }
    if (!has) {  // target gone: any other target in reach?
      for (int i = 0; i < n; ++i) if (i != c.seat && i != t) evalTarget(c, i, cs, true);
      for (auto &cd : cs) if (cd.sure && (!has || cd.score > rel.score)) { rel = cd; has = true; }
      if (!has) { o["skill"] = "guard"; return o; }  // abort the charge without recovery
      o["dir"] = J::array{rel.dx, rel.dy};
      return o;
    }
    if (rel.score < -100) { o["skill"] = "guard"; return o; }  // someone can counter it: drop the charge (no recovery)
    int lockAny, cu; bool gd;
    lockInfo(c.ps[t], lockAny, cu, gd);
    bool cont = !rel.kill && lockAny >= 12 && me.charge < 44 && g_chargeTurns < 6;
    ++g_chargeTurns;
    o["dir"] = J::array{rel.dx, rel.dy};
    if (cont) o["skill"] = "charge";
    return o;
  }

  if (guardingNow) canSkill = false;  // dropping the guard this turn: nothing can start, and a jump would only expose me
  // ---------------- end game: stand on exactly x = 0 ----------------
  if (hold) {
    bool onTop = false, useTop = false;  // (a top-platform refuge was tried: respawning fighters land on it and grab)
    g_floor = onTop ? 9.0 : 0.0;
    bool exact = me.grounded && (onTop || me.y < 0.01) && me.x == 0.0 && me.vx == 0.0;
    if (useTop && !onTop) {  // climb to the top platform: out of reach of the brawl on the floor
      if (me.grounded && me.y > 0.5) { o["drop"] = true; if (std::fabs(me.x) > 1.0) o["move"] = toCentre; return o; }
      if (me.grounded) {
        if (std::fabs(me.x) > 1.2) { o["move"] = clampd(-me.x / 2.0, -1.0, 1.0); return o; }
        o["move"] = clampd(-me.x / 3.0 / RUN, -1.0, 1.0); o["jump"] = true; return o;
      }
      o["move"] = clampd((-me.x / 4.0 - me.vx * 0.5) / RUN, -1.0, 1.0);
      if (me.aj > 0 && me.vy <= 0.06 && me.y > 3.5 && me.y < 8.9) o["jump"] = true;
      return o;
    }
    if (tick >= 4290 && tick <= 4294 && canSkill && me.grounded && !onTop && me.meter >= 0.2) {
      bool any = false, rival = false;
      for (int i = 0; i < n; ++i) if (i != c.seat && !c.ps[i].out && c.ps[i].respawn == 0) {
        if (c.ps[i].invuln < 5 && std::fabs(c.ps[i].x - me.x) < 3.3 && std::fabs(c.ps[i].y - me.y) < 2.5) any = true;
        if (c.ps[i].x == 0.0) rival = true;
      }
      if (rival && exact) any = false;
      if (any && (tick >= 4293 || !exact)) { o["skill"] = "slam"; o["jump"] = true; o["move"] = 0.0; return o; }
    }
    if (!exact) {
      if (me.grounded && me.y > 0.5 && !onTop) { o["drop"] = true; if (std::fabs(me.x) > 0.5) o["move"] = toCentre; return o; }
      if (std::fabs(me.x) > 0.9) { o["move"] = clampd(-me.x / 2.0, -1.0, 1.0); return o; }
      MS s0{me.x, me.vx, me.y, me.vy, me.grounded};
      double mv = 0; bool found = false;
      auto U = [](int u) { return u * 1e-4 / RUN; };
      auto t0 = std::chrono::steady_clock::now();
      auto over = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 6.0; };
      // one turn to x == 0, and able to stand still afterwards
      for (int k = 0; k <= 2500 && !found; ++k) for (int sg = -1; sg <= 1 && !found; sg += 2) {
        if (k == 0 && sg > 0) continue;
        MS t = s0; stepTurn(t, U(sg * k));
        if (t.x != 0.0) continue;
        MS w = t; stepTurn(w, 0.0);
        if (w.x == 0.0 && w.vx == 0.0) { mv = U(sg * k); found = true; }
      }
      // two turns
      for (int k = 0; k <= 1700 && !found; ++k) for (int sg = -1; sg <= 1 && !found; sg += 2) {
        if (k == 0 && sg > 0) continue;
        if ((k & 63) == 0 && over()) { k = 100000; break; }
        MS t = s0; stepTurn(t, U(sg * k));
        if (std::fabs(t.x) > 0.5) continue;
        long kb = std::lround(-t.x * 1e4 / 3.0);
        for (int d = -2; d <= 2 && !found; ++d) {
          int ub = t.gr ? int(kb) + d : int(std::lround(kb / 0.985)) + d;
          if (std::abs(ub) > 3200) continue;
          MS w = t; stepTurn(w, U(ub));
          if (w.x != 0.0) continue;
          MS z = w; stepTurn(z, 0.0);
          if (z.x == 0.0 && z.vx == 0.0) { mv = U(sg * k); found = true; }
        }
      }
      if (!found) {
        if (me.grounded) mv = std::fabs(me.vx) > 1e-9 ? 0.0 : (std::fabs(me.x) < 0.3 ? (me.x >= 0 ? 1.0 : -1.0) * 0.25 : clampd(-me.x / 0.96, -1.0, 1.0));
        else mv = clampd((-me.x / 4.0 - me.vx * 0.5) / RUN, -1.0, 1.0);
      }
      o["move"] = mv;
      return o;
    }
    if (onTop) return o;
    std::vector<Cand> hs;
    if (canSkill) for (int i = 0; i < n; ++i) if (i != c.seat) evalTarget(c, i, hs, false);
    int standing = 0, rival = -1;
    for (int i = 0; i < n; ++i) if (i != c.seat && !c.ps[i].out && c.ps[i].respawn == 0) { ++standing; rival = i; }
    bool duel = tick > 4306 && standing == 1;  // the walls have closed: one rival left, also on x = 0
    Cand hb; bool hh = false;
    for (auto &cd : hs) if (cd.sure && cd.third < 0.5 && (!hh || cd.score > hb.score)) { hb = cd; hh = true; }
    if (duel && !hh) {
      const P &r = c.ps[rival];
      bool behind = me.stocks < r.stocks || (me.stocks == r.stocks && me.dmg > r.dmg + 1e-9);
      if (behind) {  // losing the tie-break anyway: nothing to lose by forcing it
        int bestPri = -1;
        for (auto &cd : hs) {
          if (cd.target != rival) continue;
          int pri = cd.skill == "grab" ? 3 : cd.skill == "attack" ? 2 : cd.skill == "slam" ? 1 : 0;
          if (pri > bestPri) { bestPri = pri; hb = cd; hh = true; }
        }
        if (hh) hb.score = 100;
      }
    }
    if (hh && hb.score > 2.0 && (tick < 4270 || duel)) {
      if (hb.skill == "slam") { o["skill"] = "slam"; if (hb.jump) o["jump"] = true; o["move"] = 0.0; return o; }
      if (hb.skill == "charge") { g_chargeTarget = hb.target; g_chargeTurns = 1; }
      o["skill"] = hb.skill; o["dir"] = J::array{hb.dx, hb.dy};
    }
    return o;
  }

  // ---------------- offence ----------------
  std::vector<Cand> cs;
  if (canSkill) for (int i = 0; i < n; ++i) if (i != c.seat) evalTarget(c, i, cs, false);
  Cand best; bool has = false;
  for (auto &cd : cs) if (!has || cd.score > best.score) { best = cd; has = true; }
  if (has && best.score > 2.0) {
    if (!best.sure) { g_pend.on = true; g_pend.target = best.target; g_pend.tick = tick; g_pend.T = best.T; g_pend.dmg = c.ps[best.target].dmg; g_pend.stocks = c.ps[best.target].stocks; g_pend.grab = best.skill == "grab"; }
    if (best.skill == "slam") { o["skill"] = "slam"; if (best.jump) o["jump"] = true; o["move"] = 0.0; return o; }
    if (best.skill == "charge") { g_chargeTarget = best.target; g_chargeTurns = 1; }
    o["skill"] = best.skill; o["dir"] = J::array{best.dx, best.dy};
    return o;
  }

  // ---------------- movement ----------------
  int tgt = -1; double bestV = -1e9;
  for (int i = 0; i < n; ++i) {
    if (i == c.seat) continue;
    const P &e = c.ps[i];
    if (e.out || e.respawn > 0) continue;
    double d = std::fabs(e.x - me.x) + 0.5 * std::fabs(e.y - me.y);
    double v = -d * 2 + e.dmg * 0.25;
    if (e.invuln > 20) v -= 60;
    if (pStrike(i) < 0.3 && me.meter < 0.3) v -= 15;
    if (v > bestV) { bestV = v; tgt = i; }
  }
  double move = 0; bool jump = false, drop = false;
  if (tgt >= 0) {
    const P &e = c.ps[tgt];
    bool eOff = std::fabs(e.x) > 13.6 || e.y < -0.5;
    double goal = e.x;
    double keep = 2.7;
    if (e.invuln > 0) keep = 6.5;
    else if (pStrike(tgt) < 0.3 && me.meter < 0.3) keep = 4.6;
    else if (e.act == "guard" && me.meter < 0.3) keep = 4.2;
    if (eOff) { goal = (e.x > 0 ? 1 : -1) * 12.2; keep = 0.3; }
    double ddx = goal - me.x;
    if (std::fabs(ddx) > keep) move = ddx > 0 ? 1 : -1;
    else if (std::fabs(ddx) < keep - 1.2 && keep > 3) move = ddx > 0 ? -1 : 1;
    if (!eOff && e.invuln == 0) {
      if (e.y - me.y > 2.5 && std::fabs(e.x - me.x) < 4.5 && me.grounded && e.grounded) jump = true;
      if (me.y - e.y > 2.5 && me.grounded && me.y > 1 && e.grounded) drop = true;
    }
  } else if (std::fabs(me.x) > 3) move = toCentre;
  // stay inside the stage and the shrinking boundary
  double lim = std::min(12.4, bx - 3.0);
  if (lim < 0.5) lim = 0.0;
  if (me.x + move * 1.5 > lim) move = me.x > lim ? -1 : 0;
  if (me.x + move * 1.5 < -lim) move = me.x < -lim ? 1 : 0;
  if (lim <= 0.3) move = clampd(-me.x / 0.96, -1.0, 1.0);
  o["move"] = move;
  if (jump) o["jump"] = true;
  if (drop) o["drop"] = true;
  return o;
}
}  // namespace

int main() {
  return sm_step_worker::run([](const J &obs, J &memory) {
    J order = J::object{};
    try { order = decide(obs); } catch (...) { order = J::object{}; }
    return std::make_pair(order, memory);
  });
}
