#pragma once
// The public baseline: walk to the nearest fighter, swing when close, and get back on the stage when knocked off.
#include "json.hpp"
#include <cmath>
namespace smash_baseline {
using J = mini_json::Json;
inline double get(const J &o, const char *k) { return o.at(k).as_double(); }
inline int nearest(const J &obs) {
  const auto &ps = obs.at("players").as_array();
  int me = obs.at("seat").get<int>(), best = -1;
  double best_d = 1e18;
  for (int i = 0; i < int(ps.size()); ++i) {
    if (i == me || ps[i].at("out").as_bool() || ps[i].at("respawn").get<int>() > 0) continue;
    double dx = get(ps[i], "x") - get(ps[me], "x"), dy = get(ps[i], "y") - get(ps[me], "y"), d = dx * dx + dy * dy;
    if (d < best_d) { best_d = d; best = i; }
  }
  return best;
}
// Off the stage: steer back, spend the air jump when falling, then the uppercut if the meter allows.
inline bool recover(const J &me, const J &rules, J &order) {
  double half = rules.at("stage").at("solids").at(0).at("x2").as_double(), x = get(me, "x"), y = get(me, "y");
  if (me.at("grounded").as_bool() || (std::fabs(x) <= half && y >= -0.5)) return false;
  order["move"] = x > 0 ? -1.0 : 1.0;
  if (get(me, "vy") < 0 && me.at("air_jumps").get<int>() > 0) order["jump"] = true;
  else if (get(me, "vy") < 0 && get(me, "meter") >= rules.at("moves").at("uppercut").at("cost").as_double()) order["skill"] = "uppercut";
  return true;
}
inline J step(const J &obs) {
  J order = J::object{};
  const J &me = obs.at("players").at(obs.at("seat").get<int>());
  if (me.at("respawn").get<int>() > 0 || recover(me, obs.at("rules"), order)) return order;
  int t = nearest(obs);
  if (t < 0) return order;
  const J &other = obs.at("players").at(t);
  double dx = get(other, "x") - get(me, "x"), dy = get(other, "y") - get(me, "y");
  order["move"] = dx > 0 ? 1.0 : -1.0;
  if (dy > 2 && me.at("grounded").as_bool()) order["jump"] = true;
  if (dx * dx + dy * dy < 6.0) { order["skill"] = "attack"; order["dir"] = J::array{dx, dy}; }
  return order;
}
}  // namespace smash_baseline
