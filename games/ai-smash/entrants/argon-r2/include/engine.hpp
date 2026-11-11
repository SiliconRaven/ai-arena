#pragma once
// AI Smash game engine: the authority on the rules. Contestants may include it for forward simulation; it uses only
// the C++ standard library. Deterministic: + - * / and sqrt only, and every tick ends by rounding to rules.precision.
#include "json.hpp"
#include "sha2.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
namespace smash {
using J = mini_json::Json;
inline J arr() { return J::array{}; }
inline double num(const J &j, const char *k) { return j.at(k).as_double(); }
inline int integer(const J &j, const char *k) { return j.at(k).get<int>(); }
inline std::string sha256(const std::string &s) { return sha2::hex(sha2::sha256_bytes(s)); }
struct StaleAnswer : std::runtime_error { using std::runtime_error::runtime_error; };

enum Phase { IDLE = 0, CHARGING = 1, WINDUP = 2, ACTIVE = 3, RECOVER = 4 };
inline const char *phase_name(int p) {
  static const char *n[] = {"idle", "charging", "windup", "active", "recover"};
  return n[p];
}
inline const std::vector<std::string> &skill_names() {
  static const std::vector<std::string> n = {"attack", "charge", "guard", "blink", "uppercut",
                                             "slam", "wave", "grab", "counter", "hook"};
  return n;
}

struct Order { double move = 0, dx = 0, dy = 0; bool jump = false, drop = false, has_dir = false; std::string skill; };

struct Fighter {
  double x = 0, y = 0, vx = 0, vy = 0, damage = 0, meter = 0, dx = 1, dy = 0, power = 1;
  int facing = 1, air_jumps = 0, stocks = 0, respawn = 0, invuln = 0, hitstun = 0, gap = 0, hook_cd = 0, drop = 0;
  int phase = IDLE, phase_t = 0, charge = 0, hit_mask = 0, last_hit = -1, last_hit_tick = -1, pulled_by = -1, pull_t = 0;
  bool slam_landing = false;
  bool grounded = false, out = false, reeling = false;
  double hook_x = 0, hook_y = 0;
  std::string act;
};
struct Shot { int id = 0, owner = 0, ttl = 0; std::string kind; double x = 0, y = 0, vx = 0, vy = 0, damage = 0, power = 0; bool dead = false; };

class Engine {
  struct Hit { int a, t; std::string skill; double dx, dy, damage, power; };

public:
  J rules, events = arr();
  int n = 0, turn = 0, tick_no = 0, next_shot = 1;
  bool done = false;
  std::int64_t seed = 0;
  std::vector<Fighter> fighters;
  std::vector<Shot> shots;
  std::vector<int> kills, out_turn, out_tick, program_errors;
  std::vector<std::string> out_reason;
  J invalid = arr();

  Engine(const J &submissions, const J &seed_value, const J &game_rules) : rules(game_rules), seed(seed_value.as_int()) {
    const auto &subs = submissions.as_array();
    n = int(subs.size());
    cache();
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::uint64_t rng = std::uint64_t(seed) * 2 + 1;
    for (int i = n - 1; i > 0; --i) std::swap(order[i], order[int(next(rng) % std::uint64_t(i + 1))]);
    const auto &spawns = stage.at("spawns").as_array();
    fighters.resize(n);
    for (int i = 0; i < n; ++i) {
      auto &f = fighters[i];
      const auto &s = spawns.at(order[i] % int(spawns.size())).as_array();
      f.x = s[0].as_double(); f.y = s[1].as_double();
      f.facing = f.x <= 0 ? 1 : -1; f.dx = f.facing;
      f.stocks = integer(rules, "stocks");
      f.meter = num(rules.at("meter"), "start");
      f.air_jumps = integer(rules.at("jump"), "air_jumps");
      f.grounded = true;
    }
    kills.assign(n, 0); out_turn.assign(n, -1); out_tick.assign(n, -1); program_errors.assign(n, 0); out_reason.assign(n, "");
    for (int i = 0; i < n; ++i) {
      if (subs[i].is_string()) {
        invalid.push_back(subs[i]);
        fighters[i].out = true; fighters[i].stocks = 0; out_turn[i] = 0; out_tick[i] = 0; out_reason[i] = "invalid";
      } else invalid.push_back(nullptr);
    }
    done = int(alive().size()) <= 1;
    initial = J{{"game", "smash"}, {"rules_version", rules.at("version")}, {"seed", seed}, {"players", n},
                {"tick_rate", rules.at("tick_rate")}, {"decision_ticks", rules.at("decision_ticks")},
                {"max_turns", rules.at("max_turns")}, {"stage", stage}, {"body", rules.at("body")}, {"stocks", rules.at("stocks")},
                {"rules", rules}, {"invalid", invalid}};
  }

  J header() const { return initial; }
  double limit(int) const { return num(rules.at("time_limits"), turn == 0 ? "first" : "other"); }
  std::vector<int> alive() const {
    std::vector<int> a;
    for (int i = 0; i < n; ++i) if (!fighters[i].out) a.push_back(i);
    return a;
  }

  // ---- protocol ----
  J observation(int seat) const {
    J public_rules = rules;
    public_rules.erase("comment"); public_rules.erase("sandbox");
    return J{{"turn", turn + 1}, {"tick", tick_no}, {"seat", seat}, {"seed", seed}, {"turn_limit", limit(seat)},
             {"rules", public_rules}, {"players", players_json()}, {"shots", shots_json()}};
  }
  J decode(int seat, const std::string &line, J *notes = nullptr) {
    J a;
    try { a = J::parse(line); } catch (...) { throw std::runtime_error("not JSON"); }
    if (!a.is_object()) throw std::runtime_error("the answer must be a JSON object");
    if (!a.contains("turn") || !a.at("turn").is_int()) throw std::runtime_error("the answer has no integer turn");
    auto at = a.at("turn").as_int();
    if (at < turn + 1) throw StaleAnswer("answer to an earlier turn");
    if (at != turn + 1) throw std::runtime_error("turn is not the current turn");
    if (a.contains("error") && !a.at("error").is_null()) {
      ++program_errors.at(seat);
      if (notes) { auto &e = a.at("error"); (*notes)["program_error"] = (e.is_string() ? e.as_string() : e.dump()).substr(0, 2000); }
      return J::object{};
    }
    return a.contains("order") && !a.at("order").is_null() ? checked_order(a.at("order")) : J(J::object{});
  }
  static J checked_order(const J &o) {
    if (!o.is_object()) throw std::runtime_error("the order must be an object");
    J out = J::object{};
    if (o.contains("move") && !o.at("move").is_null()) {
      if (!o.at("move").is_number()) throw std::runtime_error("move must be a number from -1 to 1");
      out["move"] = std::max(-1.0, std::min(1.0, o.at("move").as_double()));
    }
    for (const char *k : {"jump", "drop"}) if (o.contains(k) && !o.at(k).is_null()) {
      if (!o.at(k).is_bool()) throw std::runtime_error(std::string(k) + " must be true or false");
      out[k] = o.at(k);
    }
    if (o.contains("skill") && !o.at("skill").is_null()) {
      if (!o.at("skill").is_string()) throw std::runtime_error("skill must be a skill name");
      const auto &names = skill_names();
      if (std::find(names.begin(), names.end(), o.at("skill").as_string()) == names.end())
        throw std::runtime_error("unknown skill: " + o.at("skill").as_string().substr(0, 40));
      out["skill"] = o.at("skill");
    }
    if (o.contains("dir") && !o.at("dir").is_null()) {
      const J &d = o.at("dir");
      if (!d.is_array() || d.size() != 2 || !d.at(0).is_number() || !d.at(1).is_number()) throw std::runtime_error("dir must be [dx, dy]");
      out["dir"] = d;
    }
    return out;
  }

  // ---- stepping ----
  J step(const J &orders, const J &faults) {
    events = arr();
    int per = integer(rules, "decision_ticks");
    std::vector<Order> parsed(n);
    for (int i = 0; i < n; ++i) parsed[i] = order_from(orders.at(i));
    J ticks = arr();
    for (int k = 0; k < per && !done; ++k) { tick(parsed, k == 0); ticks.push_back(tick_json()); }
    ++turn;
    for (const auto &seat : faults.as_array()) eliminate(seat.get<int>(), "fault");
    check_done();
    J f = frame();
    f["turn"] = turn; f["events"] = events; f["ticks"] = ticks;
    return f;
  }
  J frame() const {
    J out_list = arr();
    for (int v : out_turn) out_list.push_back(v < 0 ? J(nullptr) : J(v));
    return J{{"players", players_json()}, {"shots", shots_json()}, {"out", out_list}};
  }
  J tick_json() const {
    J p = arr(), s = arr();
    for (const auto &f : fighters) {
      if (f.out || f.respawn > 0) p.push_back(nullptr);
      else p.push_back(J::array{f.x, f.y, f.vx, f.vy, f.facing});
    }
    for (const auto &b : shots) s.push_back(J::array{b.id, b.kind, b.x, b.y});
    return J{{"p", p}, {"s", s}};
  }
  void tick(const std::vector<Order> &orders, bool first) {
    ++tick_no;
    for (int i = 0; i < n; ++i) if (!fighters[i].out) control(i, orders[i], first);
    for (int i = 0; i < n; ++i) if (!fighters[i].out && fighters[i].respawn == 0) motion(i);
    move_shots();
    resolve_melee();
    for (int i = 0; i < n; ++i) if (!fighters[i].out && fighters[i].respawn == 0) blast_check(i);
    purge_shots();
    round_state();
    check_done();
  }

  // ---- result ----
  J result() const {
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::uint64_t rng = std::uint64_t(seed) * 4 + 3;
    std::vector<std::uint64_t> lot(n);
    for (int i = 0; i < n; ++i) lot[i] = next(rng);
    auto key = [&](int i) {
      const auto &f = fighters[i];
      return std::make_tuple(f.out ? 0 : 1, f.out ? out_tick[i] : f.stocks, f.out ? 0.0 : -f.damage, kills[i], lot[i]);
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return key(a) > key(b); });
    J rank = arr(), basis = arr(), rp = arr(), kp = arr(), score = arr(), k = arr(), ot = arr(), oti = arr(), why = arr(), errors = arr();
    std::vector<int> place(n);
    for (int p = 0; p < n; ++p) place[order[p]] = p + 1;
    double rank_unit = num(rules.at("score"), "rank_points"), kill_unit = num(rules.at("score"), "kill_points");
    for (int i = 0; i < n; ++i) {
      rank.push_back(place[i]);
      basis.push_back(fighters[i].out ? "out_tick" : "stocks_then_damage");
      double r = rank_unit * (n - place[i]), ks = kill_unit * kills[i];
      rp.push_back(r); kp.push_back(ks); score.push_back(r + ks); k.push_back(kills[i]);
      ot.push_back(out_turn[i] < 0 ? J(nullptr) : J(out_turn[i])); oti.push_back(out_tick[i] < 0 ? J(nullptr) : J(out_tick[i]));
      why.push_back(out_reason[i].empty() ? J(nullptr) : J(out_reason[i])); errors.push_back(program_errors[i]);
    }
    return J{{"score", score}, {"rank", rank}, {"rank_basis", basis}, {"rank_points", rp}, {"kill_points", kp}, {"kills", k},
             {"out_turn", ot}, {"out_tick", oti}, {"out_reason", why}, {"invalid", invalid}, {"program_errors", errors}, {"turns", turn}};
  }

  // ---- state (forward simulation and replay audit) ----
  J state() const {
    J fs = arr();
    for (const auto &f : fighters) {
      J fighter{{"x", f.x}, {"y", f.y}, {"vx", f.vx}, {"vy", f.vy}, {"damage", f.damage}, {"meter", f.meter}, {"dx", f.dx}, {"dy", f.dy},
                     {"power", f.power}, {"facing", f.facing}, {"air_jumps", f.air_jumps}, {"stocks", f.stocks}, {"respawn", f.respawn},
                     {"invuln", f.invuln}, {"hitstun", f.hitstun}, {"gap", f.gap}, {"hook_cd", f.hook_cd}, {"drop", f.drop}, {"phase", f.phase},
                     {"phase_t", f.phase_t}, {"charge", f.charge}, {"hit_mask", f.hit_mask}, {"last_hit", f.last_hit},
                     {"last_hit_tick", f.last_hit_tick}, {"pulled_by", f.pulled_by}, {"pull_t", f.pull_t}, {"grounded", f.grounded},
                     {"out", f.out}, {"reeling", f.reeling}, {"hook_x", f.hook_x}, {"hook_y", f.hook_y}, {"act", f.act}};
      if (mv("slam").contains("landing_hitstun_cap")) fighter["slam_landing"] = f.slam_landing;
      fs.push_back(std::move(fighter));
    }
    J ss = arr();
    for (const auto &b : shots)
      ss.push_back(J{{"id", b.id}, {"owner", b.owner}, {"ttl", b.ttl}, {"kind", b.kind}, {"x", b.x}, {"y", b.y}, {"vx", b.vx}, {"vy", b.vy}, {"damage", b.damage}, {"power", b.power}});
    return J{{"turn", turn}, {"tick", tick_no}, {"next_shot", next_shot}, {"done", done}, {"fighters", fs}, {"shots", ss}, {"kills", ints(kills)},
             {"out_turn", ints(out_turn)}, {"out_tick", ints(out_tick)}, {"program_errors", ints(program_errors)}};
  }
  void load(const J &s) {
    turn = integer(s, "turn"); tick_no = integer(s, "tick"); next_shot = integer(s, "next_shot"); done = s.at("done").as_bool();
    const auto &fs = s.at("fighters").as_array();
    for (int i = 0; i < n; ++i) {
      const J &j = fs.at(i); auto &f = fighters[i];
      f.x = num(j, "x"); f.y = num(j, "y"); f.vx = num(j, "vx"); f.vy = num(j, "vy"); f.damage = num(j, "damage"); f.meter = num(j, "meter");
      f.dx = num(j, "dx"); f.dy = num(j, "dy"); f.power = num(j, "power"); f.facing = integer(j, "facing"); f.air_jumps = integer(j, "air_jumps");
      f.stocks = integer(j, "stocks"); f.respawn = integer(j, "respawn"); f.invuln = integer(j, "invuln"); f.hitstun = integer(j, "hitstun");
      f.gap = integer(j, "gap"); f.hook_cd = integer(j, "hook_cd"); f.drop = integer(j, "drop"); f.phase = integer(j, "phase"); f.phase_t = integer(j, "phase_t");
      f.charge = integer(j, "charge"); f.hit_mask = integer(j, "hit_mask"); f.last_hit = integer(j, "last_hit"); f.last_hit_tick = integer(j, "last_hit_tick");
      f.pulled_by = integer(j, "pulled_by"); f.pull_t = integer(j, "pull_t"); f.grounded = j.at("grounded").as_bool(); f.out = j.at("out").as_bool();
      f.slam_landing = j.contains("slam_landing") && j.at("slam_landing").as_bool();
      f.reeling = j.at("reeling").as_bool(); f.hook_x = num(j, "hook_x"); f.hook_y = num(j, "hook_y"); f.act = j.at("act").as_string();
    }
    shots.clear();
    for (const auto &j : s.at("shots").as_array()) {
      Shot b; b.id = integer(j, "id"); b.owner = integer(j, "owner"); b.ttl = integer(j, "ttl"); b.kind = j.at("kind").as_string();
      b.x = num(j, "x"); b.y = num(j, "y"); b.vx = num(j, "vx"); b.vy = num(j, "vy"); b.damage = num(j, "damage"); b.power = num(j, "power");
      shots.push_back(b);
    }
    kills = from_ints(s.at("kills")); out_turn = from_ints(s.at("out_turn"));
    out_tick = from_ints(s.at("out_tick")); program_errors = from_ints(s.at("program_errors"));
  }
  std::string state_sha256() const { return sha256(state().dump()); }

private:
  J initial, stage;
  std::map<std::string, J> moves;
  double gravity = 0, half_w = 0, height = 0, factor = 1e4;

  static std::uint64_t next(std::uint64_t &s) {  // SplitMix64
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  static J ints(const std::vector<int> &v) { J a = arr(); for (int x : v) a.push_back(x); return a; }
  static std::vector<int> from_ints(const J &a) { std::vector<int> v; for (const auto &x : a.as_array()) v.push_back(x.get<int>()); return v; }
  void cache() {
    stage = rules.at("stage");
    for (const auto &s : skill_names()) moves[s] = rules.at("moves").at(s);
    gravity = num(rules.at("physics"), "gravity");
    half_w = num(rules.at("body"), "half_width"); height = num(rules.at("body"), "height");
    factor = 1;
    for (int i = 0; i < integer(rules, "precision"); ++i) factor *= 10;
  }
  const J &mv(const std::string &s) const { return moves.at(s); }
  double phys(const char *k) const { return num(rules.at("physics"), k); }
  int geti(const J &j, const char *k, int fallback = 0) const { return j.contains(k) ? j.at(k).get<int>() : fallback; }
  void event(J e, const char *type) { e["type"] = type; e["tick"] = tick_no; events.push_back(std::move(e)); }
  double rnd(double v) const { return std::round(v * factor) / factor; }
  static void unit(double &x, double &y, double fx, double fy) {
    double l = std::sqrt(x * x + y * y);
    if (l < 1e-9) { x = fx; y = fy; return; }
    x /= l; y /= l;
  }
  J players_json() const {
    J a = arr();
    for (const auto &f : fighters)
      a.push_back(J{{"x", f.x}, {"y", f.y}, {"vx", f.vx}, {"vy", f.vy}, {"facing", f.facing}, {"grounded", f.grounded}, {"air_jumps", f.air_jumps},
                    {"damage", f.damage}, {"stocks", f.stocks}, {"meter", f.meter}, {"act", f.act.empty() ? J(nullptr) : J(f.act)}, {"phase", phase_name(f.phase)},
                    {"phase_ticks", f.phase_t}, {"charge", f.charge}, {"dir", J::array{f.dx, f.dy}}, {"hitstun", f.hitstun}, {"gap", f.gap}, {"hook_cd", f.hook_cd},
                    {"respawn", f.respawn}, {"invuln", f.invuln}, {"out", f.out}});
    return a;
  }
  J shots_json() const {
    J a = arr();
    for (const auto &b : shots)
      a.push_back(J{{"id", b.id}, {"owner", b.owner}, {"kind", b.kind}, {"x", b.x}, {"y", b.y}, {"vx", b.vx}, {"vy", b.vy}, {"ttl", b.ttl}});
    return a;
  }
  static Order order_from(const J &o) {
    Order r;
    if (o.contains("move")) r.move = o.at("move").as_double();
    if (o.contains("jump")) r.jump = o.at("jump").as_bool();
    if (o.contains("drop")) r.drop = o.at("drop").as_bool();
    if (o.contains("skill")) r.skill = o.at("skill").as_string();
    if (o.contains("dir")) { r.dx = o.at("dir").at(0).as_double(); r.dy = o.at("dir").at(1).as_double(); r.has_dir = true; }
    return r;
  }

  // ---- input and moves ----
  void end_action(Fighter &f, int gap) {
    f.act.clear(); f.phase = IDLE; f.phase_t = 0; f.charge = 0; f.hit_mask = 0; f.reeling = false;
    f.gap = gap;
  }
  void cancel_action(int i) {
    auto &f = fighters[i];
    for (auto &s : shots) if (s.owner == i && s.kind == "hook") s.dead = true;
    f.act.clear(); f.phase = IDLE; f.phase_t = 0; f.charge = 0; f.hit_mask = 0; f.reeling = false;
  }
  void purge_shots() { shots.erase(std::remove_if(shots.begin(), shots.end(), [](const Shot &s) { return s.dead; }), shots.end()); }
  void set_dir(Fighter &f, const Order &o) {
    if (!o.has_dir) return;
    double x = o.dx, y = o.dy;
    unit(x, y, f.facing, 0);
    f.dx = x; f.dy = y;
    if (x > 0.05) f.facing = 1; else if (x < -0.05) f.facing = -1;
  }
  bool start(int i, const Order &o) {
    auto &f = fighters[i];
    const auto &s = o.skill;
    if (s.empty()) return false;
    const J &m = mv(s);
    double cost = num(m, "cost");
    if (f.meter + 1e-9 < cost) return false;
    if (s == "slam" && f.grounded) return false;
    if (s == "hook" && f.hook_cd > 0) return false;
    f.dx = f.facing; f.dy = 0;
    set_dir(f, o);
    f.act = s; f.meter -= cost; f.hit_mask = 0; f.charge = 0; f.power = 1;
    event(J{{"seat", i}, {"skill", s}, {"dir", J::array{f.dx, f.dy}}}, "skill");
    if (s == "charge") { f.phase = CHARGING; f.phase_t = 0; }
    else if (s == "guard") { f.phase = ACTIVE; f.phase_t = integer(m, "max_ticks"); }
    else if (s == "blink") enter_active(i);
    else { f.phase = WINDUP; f.phase_t = integer(m, "windup"); }
    return true;
  }
  void enter_active(int i) {
    auto &f = fighters[i];
    const J &m = mv(f.act);
    f.phase = ACTIVE; f.hit_mask = 0;
    f.phase_t = geti(m, "active");
    if (f.act == "uppercut") { f.vy = num(m, "rise"); f.grounded = false; f.dx = 0; f.dy = 1; }
    else if (f.act == "slam") { f.vy = -num(m, "speed"); f.vx *= 0.3; f.dx = 0; f.dy = -1; }
    else if (f.act == "blink") { f.vx = f.dx * num(m, "speed"); f.vy = f.dy * num(m, "speed"); f.grounded = false; }
    else if (f.act == "wave" || f.act == "hook") {
      Shot b; b.id = next_shot++; b.owner = i; b.kind = f.act;
      double speed = num(m, "speed");
      b.vx = f.dx * speed; b.vy = f.dy * speed; b.ttl = integer(m, "ttl");
      b.x = f.x + f.dx * (half_w + 0.5); b.y = f.y + height / 2 + f.dy * (half_w + 0.5);
      b.damage = num(m, "damage"); b.power = m.contains("power") ? num(m, "power") : 0;
      shots.push_back(b);
      if (f.act == "wave") { f.phase = RECOVER; f.phase_t = integer(m, "recover"); }
      else f.phase_t = 1000000;  // a hook stays out until it lands or misses
    }
  }
  void advance(int i) {
    auto &f = fighters[i];
    if (f.phase == CHARGING) {
      ++f.charge;
      if (f.charge >= integer(mv("charge"), "auto_release_ticks")) release_charge(i);
      return;
    }
    if (f.phase == WINDUP) { if (--f.phase_t <= 0) enter_active(i); return; }
    if (f.phase == ACTIVE) {
      if (f.act == "guard") {
        if (--f.phase_t <= 0) end_action(f, integer(mv("guard"), "lockout_ticks"));
        return;
      }
      if (--f.phase_t <= 0) to_recover(i);
      return;
    }
    if (f.phase == RECOVER && --f.phase_t <= 0) end_action(f, integer(rules, "gap_ticks"));
  }
  void to_recover(int i) {
    auto &f = fighters[i];
    if (f.act == "blink") { f.vx *= 0.2; f.vy *= 0.2; }
    f.reeling = false;
    f.phase = RECOVER; f.phase_t = std::max(1, geti(mv(f.act), "recover", 1));
  }
  void release_charge(int i) {
    auto &f = fighters[i];
    const J &m = mv("charge");
    double t = std::min(1.0, double(f.charge) / integer(m, "max_ticks"));
    f.power = 1 + (num(m, "max_mult") - 1) * t;
    f.phase = WINDUP; f.phase_t = integer(m, "windup");
    event(J{{"seat", i}, {"power", rnd(f.power)}}, "charge_release");
  }
  void control(int i, const Order &o, bool first) {
    auto &f = fighters[i];
    if (f.respawn > 0) { if (--f.respawn == 0) reappear(i); return; }
    if (f.invuln > 0) --f.invuln;
    if (f.hook_cd > 0) --f.hook_cd;
    if (f.drop > 0) --f.drop;
    const J &meter = rules.at("meter");
    f.meter = std::min(num(meter, "max"), f.meter + num(meter, "refill"));
    if (f.pull_t > 0) return;
    if (f.hitstun > 0) { if (--f.hitstun == 0) f.gap = 0; return; }
    bool released = false;
    if (f.act == "guard" && f.phase == ACTIVE && o.skill != "guard") end_action(f, integer(rules, "gap_ticks"));
    else if (f.act == "charge" && f.phase == CHARGING) {
      set_dir(f, o);
      if (o.skill == "guard" || o.skill == "blink") {      // giving up the charge: no blow, no recovery, the defence starts at once
        end_action(f, 0);
        event(J{{"seat", i}}, "charge_cancel");
      } else if (o.skill != "charge") { release_charge(i); released = true; }
    }
    if (f.act.empty()) {
      if (f.gap > 0) --f.gap;
      else start(i, o);
    } else if (!released) advance(i);
    if (first && o.jump && (f.act.empty() || f.act == "charge")) do_jump(i);
    if (o.drop && f.grounded) f.drop = integer(rules.at("jump"), "drop_ticks");
    drive(i, o);
  }
  void do_jump(int i) {
    auto &f = fighters[i];
    const J &j = rules.at("jump");
    if (f.grounded) { f.vy = num(j, "ground_speed"); f.grounded = false; event(J{{"seat", i}}, "jump"); }
    else if (f.air_jumps > 0) { --f.air_jumps; f.vy = num(j, "air_speed"); event(J{{"seat", i}, {"air", true}}, "jump"); }
  }
  void drive(int i, const Order &o) {
    auto &f = fighters[i];
    if (f.act == "guard") { if (f.grounded) f.vx = 0; return; }
    if ((f.act == "blink" && f.phase == ACTIVE) || f.reeling) return;
    bool locked = !f.act.empty() && f.act != "charge";
    if (locked) { if (f.grounded) f.vx *= phys("ground_friction"); return; }
    double want = o.move * phys("run_speed");
    if (f.act == "charge") want *= num(mv("charge"), "move_factor");
    double a = phys(f.grounded ? "ground_accel" : "air_accel");
    f.vx += std::max(-a, std::min(a, want - f.vx));
    if (f.act.empty()) { if (o.move > 0.05) f.facing = 1; else if (o.move < -0.05) f.facing = -1; }
  }

  // ---- movement ----
  void motion(int i) {
    auto &f = fighters[i];
    const J &hook = mv("hook");
    if (f.reeling) {
      double dx = f.hook_x - f.x, dy = f.hook_y - (f.y + height / 2), stop = num(hook, "stop");
      if (dx * dx + dy * dy <= stop * stop) { f.vx = 0; f.vy = 0.2; to_recover(i); }
      else { unit(dx, dy, 0, 1); f.vx = dx * num(hook, "reel_speed"); f.vy = dy * num(hook, "reel_speed"); }
    }
    if (f.pull_t > 0) {
      auto &a = fighters[f.pulled_by];
      double dx = a.x - f.x, dy = a.y - f.y, stop = num(hook, "stop");
      --f.pull_t;
      if (dx * dx + dy * dy <= stop * stop || a.out || a.respawn > 0) { f.vx = 0; f.vy = 0; f.pull_t = 0; f.pulled_by = -1; }
      else { unit(dx, dy, 1, 0); f.vx = dx * num(hook, "pull_speed"); f.vy = dy * num(hook, "pull_speed"); }
    }
    bool floating = (f.act == "blink" && f.phase == ACTIVE) || f.reeling || f.pull_t > 0;
    if (!floating) {
      f.vy = std::max(f.vy - gravity, f.hitstun > 0 ? -2.0 : -phys("max_fall"));
      if (f.hitstun > 0) { f.vx *= phys("hit_friction"); f.vy *= phys("hit_friction"); }
      else if (!f.grounded) f.vx *= phys("air_friction");
    }
    f.x += f.vx;
    collide_x(f);
    double foot = f.y;
    f.y += f.vy;
    f.grounded = false;
    collide_y(i, foot);
  }
  bool overlaps(const Fighter &f, const J &s) const {
    return f.x + half_w > num(s, "x1") && f.x - half_w < num(s, "x2") && f.y < num(s, "top") && f.y + height > num(s, "bottom");
  }
  void collide_x(Fighter &f) {
    for (const auto &s : stage.at("solids").as_array()) {
      if (!overlaps(f, s)) continue;
      f.x = f.vx > 0 ? num(s, "x1") - half_w : num(s, "x2") + half_w;
      f.vx = f.hitstun > 0 ? -f.vx * phys("bounce") : 0;
    }
  }
  void land(int i, double y) {
    auto &f = fighters[i];
    f.y = y; f.vy = 0; f.grounded = true;
    if (f.slam_landing) {
      int cap = geti(mv("slam"), "landing_hitstun_cap", -1);
      if (cap >= 0 && f.hitstun > cap) {
        event(J{{"seat", i}, {"before", f.hitstun}, {"after", cap}}, "slam_land_cap");
        f.hitstun = cap;
      }
      f.slam_landing = false;
    }
    f.air_jumps = integer(rules.at("jump"), "air_jumps");
    if ((f.act == "slam" || f.act == "uppercut") && f.phase == ACTIVE) to_recover(i);
  }
  void collide_y(int i, double foot) {
    auto &f = fighters[i];
    for (const auto &s : stage.at("solids").as_array()) {
      if (!overlaps(f, s)) continue;
      if (f.vy <= 0) land(i, num(s, "top"));
      else { f.y = num(s, "bottom") - height; f.vy = f.hitstun > 0 ? -f.vy * phys("bounce") : 0; }
    }
    if (f.vy <= 0 && f.drop == 0 && !f.grounded) {
      for (const auto &p : stage.at("platforms").as_array()) {
        double y = num(p, "y");
        if (foot >= y && f.y <= y && f.x + half_w > num(p, "x1") && f.x - half_w < num(p, "x2")) { land(i, y); break; }
      }
    }
  }
  bool in_solid(double x, double y) const {
    for (const auto &s : stage.at("solids").as_array())
      if (x > num(s, "x1") && x < num(s, "x2") && y < num(s, "top") && y > num(s, "bottom")) return true;
    return false;
  }

  // ---- hits ----
  bool untouchable(const Fighter &t) const { return t.out || t.respawn > 0 || t.invuln > 0 || (t.act == "blink" && t.phase == ACTIVE); }
  bool countering(const Fighter &t) const { return t.act == "counter" && t.phase == ACTIVE; }
  bool guarding(const Fighter &t) const { return t.act == "guard" && t.phase == ACTIVE; }
  void launch(int a, int t, double dx, double dy, double damage, double power, const std::string &skill) {
    auto &f = fighters[t];
    const J &k = rules.at("knockback");
    unit(dx, dy, fighters[a].x <= f.x ? 1 : -1, 0);
    f.damage += damage;
    double speed = (num(k, "base") + num(k, "scale") * f.damage / 100) * power;
    cancel_action(t);
    f.pull_t = 0; f.pulled_by = -1;
    f.vx = dx * speed; f.vy = dy * speed; f.grounded = false;
    f.slam_landing = skill == "slam" && mv("slam").contains("landing_hitstun_cap");
    f.hitstun = std::min(integer(k, "hitstun_max"), integer(k, "hitstun_base") + int(speed * num(k, "hitstun_per_speed")));
    f.gap = 0;
    f.last_hit = a; f.last_hit_tick = tick_no;
    event(J{{"attacker", a}, {"target", t}, {"skill", skill}, {"damage", rnd(damage)}, {"total", rnd(f.damage)}, {"speed", rnd(speed)}, {"dir", J::array{rnd(dx), rnd(dy)}}}, "hit");
  }
  void counter_back(int t, int a, double damage, double power, const std::string &skill) {
    auto &target = fighters[t];
    const J &m = mv("counter");
    double mult = num(m, "mult");
    target.phase = RECOVER; target.phase_t = integer(m, "spent_recover");
    event(J{{"seat", t}, {"attacker", a}, {"skill", skill}}, "counter");
    launch(t, a, fighters[a].x - target.x, 0.4, damage * mult, power * mult, "counter");
  }
  void strike(const Hit &h) {
    auto &t = fighters[h.t];
    if (untouchable(t)) return;
    if (h.skill != "grab") {
      if (h.skill != "slam" && countering(t)) { counter_back(h.t, h.a, h.damage, h.power, h.skill); return; }   // a guard still stops a slam
      if (guarding(t)) {
        t.vx = (fighters[h.a].x <= t.x ? 1 : -1) * num(mv("guard"), "pushback");
        event(J{{"seat", h.t}, {"attacker", h.a}, {"skill", h.skill}}, "block");
        return;
      }
    }
    launch(h.a, h.t, h.dx, h.dy, h.damage, h.power, h.skill);
  }
  bool melee(const Fighter &f) const {
    return f.phase == ACTIVE && (f.act == "attack" || f.act == "charge" || f.act == "uppercut" || f.act == "slam" || f.act == "grab");
  }
  void resolve_melee() {
    std::vector<Hit> hits;
    double centre = height / 2;
    for (int a = 0; a < n; ++a) {
      auto &f = fighters[a];
      if (f.out || f.respawn > 0 || !melee(f)) continue;
      const J &m = mv(f.act);
      double ax = f.x + f.dx * num(m, "offset"), ay = f.y + centre + f.dy * num(m, "offset");
      double reach = num(m, "radius") + height / 2;
      for (int t = 0; t < n; ++t) {
        if (t == a || (f.hit_mask >> t & 1)) continue;
        const auto &v = fighters[t];
        double dx = v.x - ax, dy = v.y + centre - ay;
        if (dx * dx + dy * dy > reach * reach || untouchable(v)) continue;
        f.hit_mask |= 1 << t;
        double lift = m.contains("lift") ? num(m, "lift") : 0, scale = f.act == "charge" ? f.power : 1;
        double launch_scale = scale;
        if (f.act == "charge" && m.contains("short_power_mult")) {
          double hold_fraction = double(f.charge) / num(m, "max_ticks");
          double recovery = std::min(1.0, hold_fraction / num(m, "short_power_full_fraction"));
          launch_scale *= 1 + (num(m, "short_power_mult") - 1) * (1 - recovery);
        }
        hits.push_back(Hit{a, t, f.act, f.dx, f.dy + lift, num(m, "damage") * scale, num(m, "power") * launch_scale});
      }
    }
    for (const auto &h : hits) strike(h);
  }
  void hook_miss(int owner) {
    auto &f = fighters[owner];
    f.hook_cd = integer(mv("hook"), "miss_cooldown_ticks");
    if (f.act == "hook") to_recover(owner);
  }
  void move_shots() {
    double centre = height / 2;
    for (auto &s : shots) {
      if (s.dead) continue;
      s.x += s.vx; s.y += s.vy; --s.ttl;
      if (in_solid(s.x, s.y)) {
        if (s.kind == "hook") attach(s);
        s.dead = true;
        continue;
      }
      const J &m = mv(s.kind);
      for (int t = 0; t < n && !s.dead; ++t) {
        if (t == s.owner) continue;
        auto &v = fighters[t];
        double dx = v.x - s.x, dy = v.y + centre - s.y, reach = num(m, "radius") + height / 2;
        if (dx * dx + dy * dy > reach * reach || untouchable(v)) continue;
        if (s.kind == "hook") { hook_target(s, t); s.dead = true; break; }
        if (countering(v)) {
          double mult = num(mv("counter"), "mult");
          event(J{{"seat", t}, {"attacker", s.owner}, {"skill", "wave"}}, "counter");
          v.phase = RECOVER; v.phase_t = integer(mv("counter"), "spent_recover");
          s.owner = t; s.vx = -s.vx; s.vy = -s.vy; s.damage *= mult; s.power *= mult; s.ttl = integer(m, "ttl");
          break;
        }
        if (guarding(v)) { event(J{{"seat", t}, {"attacker", s.owner}, {"skill", "wave"}}, "block"); s.dead = true; break; }
        launch(s.owner, t, s.vx, s.vy + 0.2, s.damage, s.power, "wave");
        s.dead = true;
      }
      if (!s.dead && s.ttl <= 0) { if (s.kind == "hook") hook_miss(s.owner); s.dead = true; }
    }
  }
  void attach(const Shot &s) {
    auto &f = fighters[s.owner];
    if (f.act != "hook") return;
    f.reeling = true; f.hook_x = s.x; f.hook_y = s.y; f.phase = ACTIVE; f.phase_t = integer(mv("hook"), "pull_ticks");
    event(J{{"seat", s.owner}, {"x", rnd(s.x)}, {"y", rnd(s.y)}}, "hook_stage");
  }
  void hook_target(const Shot &s, int t) {
    auto &v = fighters[t];
    const J &m = mv("hook");
    cancel_action(t);
    v.damage += num(m, "damage");
    v.slam_landing = false;
    v.pulled_by = s.owner; v.pull_t = integer(m, "pull_ticks"); v.hitstun = 0; v.grounded = false;
    v.last_hit = s.owner; v.last_hit_tick = tick_no;
    event(J{{"seat", s.owner}, {"target", t}}, "hook_pull");
    if (fighters[s.owner].act == "hook") to_recover(s.owner);
  }

  // ---- stocks ----
  void blast_check(int i) {
    auto &f = fighters[i];
    const J &b = stage.at("blast");
    double blast_x = num(b, "x"), blast_top = num(b, "top");
    if (stage.contains("blast_shrink")) {              // late in a match the side and top lines close in; the bottom line stays
      const J &s = stage.at("blast_shrink");
      int start = integer(s, "start_tick");
      if (tick_no > start) {
        double closed = (tick_no - start) * num(s, "rate");
        blast_x = std::max(num(s, "min_x"), blast_x - closed);
        blast_top = std::max(num(s, "min_top"), blast_top - closed * num(s, "top_ratio"));
      }
    }
    if (std::fabs(f.x) <= blast_x && f.y <= blast_top && f.y >= num(b, "bottom")) return;
    int by = f.last_hit >= 0 && f.last_hit != i && tick_no - f.last_hit_tick <= integer(rules, "kill_credit_ticks") ? f.last_hit : -1;
    if (by >= 0) ++kills[by];
    event(J{{"seat", i}, {"by", by < 0 ? J(nullptr) : J(by)}, {"kind", by >= 0 ? "kill" : "self"}, {"stocks", f.stocks - 1}}, "fall");
    --f.stocks;
    cancel_action(i);
    f.slam_landing = false;
    f.damage = 0; f.vx = 0; f.vy = 0; f.hitstun = 0; f.gap = 0; f.pull_t = 0; f.pulled_by = -1; f.last_hit = -1; f.last_hit_tick = -1;
    if (f.stocks <= 0) { eliminate(i, by >= 0 ? "kill" : "self"); return; }
    f.respawn = integer(rules, "respawn_ticks");
  }
  void reappear(int i) {
    auto &f = fighters[i];
    const auto &r = stage.at("respawn").as_array();
    f.x = r[0].as_double() + (i - (n - 1) / 2.0) * 1.5;
    f.y = r[1].as_double();
    f.vx = 0; f.vy = 0; f.grounded = false; f.drop = 0;
    f.air_jumps = integer(rules.at("jump"), "air_jumps");
    f.invuln = integer(rules, "respawn_invuln_ticks");
    event(J{{"seat", i}}, "respawn");
  }
  void eliminate(int i, const char *why) {
    auto &f = fighters[i];
    if (f.out) return;
    cancel_action(i);
    f.out = true; f.stocks = 0;
    out_turn[i] = turn; out_tick[i] = tick_no; out_reason[i] = why;
    event(J{{"seat", i}, {"reason", why}}, "out");
  }
  void round_state() {
    for (auto &f : fighters) { f.x = rnd(f.x); f.y = rnd(f.y); f.vx = rnd(f.vx); f.vy = rnd(f.vy); f.meter = rnd(f.meter); f.damage = rnd(f.damage); }
    for (auto &s : shots) { s.x = rnd(s.x); s.y = rnd(s.y); }
  }
  void check_done() { if (int(alive().size()) <= 1 || turn >= integer(rules, "max_turns")) done = true; }
};
}  // namespace smash
