#pragma once
#include "fast_sim.hpp"
#include <chrono>

namespace smash_bot {

using namespace fast_sim;

struct OpponentStats {
  int turns_seen = 0;
  int guard_frames = 0;
  int counter_uses = 0;
  int charge_uses = 0;
  int hook_uses = 0;
  int wave_uses = 0;
  int grab_uses = 0;
  int slam_uses = 0;
  int attack_uses = 0;
};

struct BotContext {
  RulesConfig rc;
  bool has_prev = false;
  FastState prev_state;
  FastOrder last_my_order;
  int my_drop_counter = 0;
  OpponentStats opp_stats[4];

  void sync_from_obs(const J &obs, FastState &st, int &my_seat) {
    rc.load_from_json(obs.at("rules"));
    my_seat = obs.at("seat").get<int>();
    const auto &ps = obs.at("players").as_array();
    st.n = int(ps.size());
    st.turn = obs.at("turn").get<int>() - 1;
    st.tick_no = obs.at("tick").get<int>();
    st.done = false;

    if (st.turn == 0 || !has_prev) {
      has_prev = false;
      my_drop_counter = 0;
      for (int i = 0; i < 4; ++i) {
        st.kills[i] = 0;
        st.out_turn[i] = -1;
        st.out_tick[i] = -1;
        opp_stats[i] = OpponentStats{};
      }
    } else {
      for (int i = 0; i < 4; ++i) {
        st.kills[i] = prev_state.kills[i];
        st.out_turn[i] = prev_state.out_turn[i];
        st.out_tick[i] = prev_state.out_tick[i];
      }
    }

    st.n_shots = 0;
    int max_shot_id = 0;
    bool has_live_hook_shot[4] = {false, false, false, false};
    for (const auto &sj : obs.at("shots").as_array()) {
      if (st.n_shots >= 16) break;
      FastShot &b = st.shots[st.n_shots++];
      b.id = sj.at("id").get<int>();
      max_shot_id = std::max(max_shot_id, b.id);
      b.owner = sj.at("owner").get<int>();
      b.kind = parse_skill(sj.at("kind"));
      b.x = sj.at("x").as_double();
      b.y = sj.at("y").as_double();
      b.vx = sj.at("vx").as_double();
      b.vy = sj.at("vy").as_double();
      b.ttl = sj.at("ttl").get<int>();
      b.dead = false;
      const MoveSpec &ms = rc.moves[b.kind];
      b.damage = ms.damage;
      b.power = ms.power;
      if (b.kind == SK_WAVE && has_prev) {
        for (int k = 0; k < prev_state.n_shots; ++k) {
          if (prev_state.shots[k].id == b.id) {
            if (prev_state.shots[k].owner != b.owner) {
              b.damage = prev_state.shots[k].damage * rc.moves[SK_COUNTER].mult;
              b.power = prev_state.shots[k].power * rc.moves[SK_COUNTER].mult;
            } else {
              b.damage = prev_state.shots[k].damage;
              b.power = prev_state.shots[k].power;
            }
            break;
          }
        }
      }
      if (b.kind == SK_HOOK && b.owner >= 0 && b.owner < 4) {
        has_live_hook_shot[b.owner] = true;
      }
    }
    st.next_shot = std::max(has_prev ? prev_state.next_shot : 1, max_shot_id + 1);

    for (int i = 0; i < st.n; ++i) {
      const J &p = ps[i];
      FastFighter &f = st.fighters[i];
      f.x = p.at("x").as_double();
      f.y = p.at("y").as_double();
      f.vx = p.at("vx").as_double();
      f.vy = p.at("vy").as_double();
      f.facing = p.at("facing").get<int>();
      f.grounded = p.at("grounded").as_bool();
      f.air_jumps = p.at("air_jumps").get<int>();
      f.damage = p.at("damage").as_double();
      f.stocks = p.at("stocks").get<int>();
      f.meter = p.at("meter").as_double();
      f.act = parse_skill(p.at("act"));
      f.phase = parse_phase(p.at("phase"));
      f.phase_t = p.at("phase_ticks").get<int>();
      f.charge = p.at("charge").get<int>();
      const auto &d = p.at("dir").as_array();
      f.dx = d[0].as_double();
      f.dy = d[1].as_double();
      f.hitstun = p.at("hitstun").get<int>();
      f.gap = p.at("gap").get<int>();
      f.hook_cd = p.at("hook_cd").get<int>();
      f.respawn = p.at("respawn").get<int>();
      f.invuln = p.at("invuln").get<int>();
      f.out = p.at("out").as_bool();

      if (f.act == SK_CHARGE && f.phase != PH_IDLE && f.phase != PH_CHARGING) {
        double t = std::min(1.0, double(f.charge) / rc.moves[SK_CHARGE].max_ticks);
        f.power = 1.0 + (rc.moves[SK_CHARGE].max_mult - 1.0) * t;
      } else {
        f.power = 1.0;
      }

      f.hit_mask = 0;
      f.drop = (i == my_seat) ? my_drop_counter : 0;
      f.reeling = false;
      f.hook_x = f.x;
      f.hook_y = f.y;
      f.pulled_by = -1;
      f.pull_t = 0;
      f.last_hit = -1;
      f.last_hit_tick = -1;
      f.slam_landing = false;

      if (has_prev) {
        const FastFighter &pf = prev_state.fighters[i];
        if (!f.grounded && f.hitstun > 0 && rc.moves[SK_SLAM].landing_hitstun_cap >= 0) {
          if (f.damage > pf.damage + 0.01) {
            if (std::fabs((f.damage - pf.damage) - rc.moves[SK_SLAM].damage) < 0.25 && f.vy < -0.25) {
              f.slam_landing = true;
            }
          } else {
            f.slam_landing = pf.slam_landing;
          }
        }
        if (i != my_seat && !f.out && f.respawn == 0) {
          opp_stats[i].turns_seen++;
          if (f.act == SK_GUARD) opp_stats[i].guard_frames++;
          if (f.act != pf.act) {
            if (f.act == SK_COUNTER) opp_stats[i].counter_uses++;
            else if (f.act == SK_CHARGE) opp_stats[i].charge_uses++;
            else if (f.act == SK_HOOK) opp_stats[i].hook_uses++;
            else if (f.act == SK_WAVE) opp_stats[i].wave_uses++;
            else if (f.act == SK_GRAB) opp_stats[i].grab_uses++;
            else if (f.act == SK_SLAM) opp_stats[i].slam_uses++;
            else if (f.act == SK_ATTACK) opp_stats[i].attack_uses++;
          }
        }

        if (f.stocks < pf.stocks || f.out || f.respawn > 0) {
          f.last_hit = -1;
          f.last_hit_tick = -1;
        } else if (f.damage > pf.damage + 0.01) {
          double diff = f.damage - pf.damage;
          int best_a = -1;
          double best_d2 = 1e18;
          if (std::fabs(diff - rc.moves[SK_HOOK].damage) < 0.25 && f.hitstun == 0) {
            for (int k = 0; k < prev_state.n_shots; ++k) {
              const auto &pshot = prev_state.shots[k];
              if (pshot.kind == SK_HOOK && pshot.owner != i) {
                double dx = pshot.x - f.x, dy = pshot.y - (f.y + rc.height * 0.5);
                double d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best_a = pshot.owner; }
              }
            }
            if (best_a >= 0) {
              f.pulled_by = best_a;
              f.pull_t = std::max(0, rc.moves[SK_HOOK].pull_ticks - 2);
            }
          }
          if (best_a < 0) {
            for (int a = 0; a < st.n; ++a) {
              if (a == i || prev_state.fighters[a].out || prev_state.fighters[a].respawn > 0) continue;
              double dx = prev_state.fighters[a].x - f.x, dy = prev_state.fighters[a].y - f.y;
              double d2 = dx * dx + dy * dy;
              if (d2 < best_d2) { best_d2 = d2; best_a = a; }
            }
          }
          f.last_hit = best_a;
          f.last_hit_tick = st.tick_no - 1;
        } else {
          f.last_hit = pf.last_hit;
          f.last_hit_tick = pf.last_hit_tick;
          if (pf.pull_t > 3 && !f.grounded && f.hitstun == 0 && pf.pulled_by >= 0) {
            const auto &puller = ps[pf.pulled_by];
            double dx = puller.at("x").as_double() - f.x;
            double dy = puller.at("y").as_double() - f.y;
            if (dx * dx + dy * dy > rc.moves[SK_HOOK].stop * rc.moves[SK_HOOK].stop) {
              f.pulled_by = pf.pulled_by;
              f.pull_t = pf.pull_t - 3;
            }
          }
        }

        if (f.act == SK_HOOK && f.phase == PH_ACTIVE && !has_live_hook_shot[i]) {
          f.reeling = true;
          if (pf.reeling) {
            f.hook_x = pf.hook_x;
            f.hook_y = pf.hook_y;
          } else {
            bool found = false;
            for (int k = 0; k < prev_state.n_shots; ++k) {
              const auto &pshot = prev_state.shots[k];
              if (pshot.kind == SK_HOOK && pshot.owner == i) {
                for (int step = 1; step <= 4; ++step) {
                  double hx = pshot.x + pshot.vx * step;
                  double hy = pshot.y + pshot.vy * step;
                  if (rc.in_solid(hx, hy)) {
                    f.hook_x = hx; f.hook_y = hy; found = true; break;
                  }
                }
                if (!found) { f.hook_x = pshot.x + pshot.vx; f.hook_y = pshot.y + pshot.vy; found = true; }
                break;
              }
            }
            if (!found) {
              f.hook_x = f.x + f.dx * 6.0;
              f.hook_y = f.y + rc.height * 0.5 + f.dy * 6.0;
            }
          }
        }
      }
    }
  }

  FastOrder recovery_heuristic(const FastState &st, int i) const {
    const FastFighter &f = st.fighters[i];
    FastOrder o;
    double edge = rc.solids[0].x2;
    if (std::fabs(f.x) < edge + 0.3 && f.y < rc.solids[0].bottom + 0.5) {
      o.move = (f.x >= 0) ? 1.0 : -1.0;
      if (f.y < rc.solids[0].bottom - 1.2 && f.meter >= rc.moves[SK_BLINK].cost && f.act == SK_NONE && f.gap == 0) {
        o.skill = SK_BLINK;
        o.has_dir = true;
        o.dx = (f.x >= 0) ? 0.85 : -0.85;
        o.dy = 0.52;
      }
      return o;
    }
    o.move = (f.x > 0) ? -1.0 : 1.0;
    if (f.vy < 0.08) {
      if (f.air_jumps > 0 && (f.y < 0.5 || std::fabs(f.x) > edge + 2.5)) {
        o.jump = true;
      } else if (f.act == SK_NONE && f.gap == 0) {
        if (std::fabs(f.x) <= edge + 0.55 && f.y < 0.0) {
          if (f.meter >= rc.moves[SK_UPPERCUT].cost && f.y > -3.5) {
            o.skill = SK_UPPERCUT;
          } else if (f.meter >= rc.moves[SK_BLINK].cost) {
            o.skill = SK_BLINK;
            o.has_dir = true;
            o.dx = (f.x > 0) ? -0.35 : 0.35;
            o.dy = 0.94;
          }
        } else if (std::fabs(f.x) > edge + 0.55 && f.meter >= rc.moves[SK_BLINK].cost) {
          double tx = (f.x > 0) ? (edge - 0.5) : -(edge - 0.5);
          double ty = 1.8;
          o.skill = SK_BLINK;
          o.has_dir = true;
          o.dx = tx - f.x;
          o.dy = ty - f.y;
        } else if (f.y < 0.0 && f.meter >= rc.moves[SK_UPPERCUT].cost) {
          o.skill = SK_UPPERCUT;
        }
      }
    }
    return o;
  }

  // Check if an incoming enemy attack or projectile is about to hit fighter `me` within `max_ticks`
  bool incoming_blockable_threat(const FastState &st, int me, int max_ticks = 4) const {
    const FastFighter &mf = st.fighters[me];
    for (int j = 0; j < st.n; ++j) {
      if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
      const FastFighter &ej = st.fighters[j];
      if (ej.act == SK_NONE || ej.act == SK_GUARD || ej.act == SK_BLINK || ej.act == SK_GRAB || ej.act == SK_HOOK || ej.act == SK_COUNTER) continue;
      if (ej.act == SK_SLAM) {
        if ((ej.phase == PH_ACTIVE || (ej.phase == PH_WINDUP && ej.phase_t <= max_ticks)) &&
            std::fabs(ej.x - mf.x) <= 4.0 && ej.y >= mf.y - 0.5 && ej.y <= mf.y + 5.8) {
          return true;
        }
        continue;
      }
      double dx = (ej.x + 2.0 * ej.vx) - (mf.x + 2.0 * mf.vx);
      double dy = (ej.y + 2.0 * ej.vy) - (mf.y + 2.0 * mf.vy);
      double d2 = dx * dx + dy * dy;
      double reach = (ej.act == SK_CHARGE) ? 4.0 : 3.5;
      if (d2 <= reach * reach) {
        if (ej.phase == PH_ACTIVE) return true;
        if (ej.phase == PH_WINDUP && ej.phase_t <= max_ticks) return true;
      }
    }
    for (int k = 0; k < st.n_shots; ++k) {
      const FastShot &s = st.shots[k];
      if (s.owner == me || s.kind != SK_WAVE || s.dead) continue;
      for (int t = 1; t <= max_ticks && t <= s.ttl; ++t) {
        double sx = s.x + s.vx * t;
        double sy = s.y + s.vy * t;
        double dx = mf.x - sx, dy = (mf.y + rc.height * 0.5) - sy;
        double reach = rc.moves[SK_WAVE].radius + rc.height * 0.5 + 0.3;
        if (dx * dx + dy * dy <= reach * reach) return true;
      }
    }
    return false;
  }

  // Predict opponent order for profile (0 = aggressive/striker, 1 = defensive/warden-counter)
  FastOrder predict_opponent(const FastState &st, int i, int profile) const {
    const FastFighter &f = st.fighters[i];
    FastOrder o;
    if (f.out || f.respawn > 0 || f.hitstun > 2) return o;
    double edge = rc.solids[0].x2;
    if (!f.grounded && (std::fabs(f.x) > edge + 1.5 || f.y < -0.5)) {
      return recovery_heuristic(st, i);
    }

    int best_t = -1;
    double best_d2 = 1e18;
    bool can_slam_anyone = false;
    for (int j = 0; j < st.n; ++j) {
      if (j == i || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
      const auto &fj = st.fighters[j];
      double dx = fj.x - f.x;
      double dy = fj.y - f.y;
      double d2 = dx * dx + dy * dy;
      if (!f.grounded && f.y > 0.4 && fj.invuln == 0 && std::fabs(dx) < 3.9 && f.y > fj.y + 0.5 && f.y < fj.y + 6.5) {
        can_slam_anyone = true;
      }
      if (fj.invuln > 0) d2 += 400.0;
      if (d2 < best_d2) { best_d2 = d2; best_t = j; }
    }
    if (best_t < 0) return o;
    const FastFighter &t = st.fighters[best_t];
    double dx = t.x - f.x, dy = t.y - f.y;
    double dist = std::sqrt(dx * dx + dy * dy);

    if (f.act == SK_CHARGE && f.phase == PH_CHARGING) {
      o.has_dir = true; o.dx = dx; o.dy = dy;
      o.move = (dx > 0) ? 1.0 : -1.0;
      if (profile == 0 || dist <= 3.0 || f.charge >= 12) {
        o.skill = SK_NONE;
      } else {
        o.skill = SK_CHARGE;
      }
      return o;
    }

    if (f.act == SK_GUARD && f.phase == PH_ACTIVE) {
      if (profile == 1 && dist < 3.6 && (t.phase == PH_WINDUP || t.phase == PH_ACTIVE || t.phase == PH_CHARGING)) {
        o.skill = SK_GUARD;
        return o;
      }
      o.skill = SK_NONE;
      return o;
    }

    o.move = (dx > 0) ? 1.0 : -1.0;
    if (dy > 2.0 && f.grounded) o.jump = true;
    if (dy < -1.5 && f.grounded && f.y > 0.5) o.drop = true;

    if (f.act == SK_NONE && f.gap <= 2 && t.invuln == 0) {
      double sdir = (dx >= 0) ? 1.0 : -1.0;
      if (profile == 1) {
        // 1. Reactive Counter on locked incoming Charge windup, Uppercut, or Attack windup
        if (dist < 4.0 && f.meter >= rc.moves[SK_COUNTER].cost) {
          if ((t.act == SK_CHARGE && t.phase == PH_WINDUP && t.phase_t >= 4) ||
              (t.act == SK_UPPERCUT && (t.phase == PH_WINDUP || (t.phase == PH_ACTIVE && t.phase_t >= 4))) ||
              (t.act == SK_ATTACK && t.phase == PH_WINDUP && t.phase_t >= 4)) {
            o.skill = SK_COUNTER;
            return o;
          }
        }
        // 2. Shield incoming melee windup/active or overhead airborne slam threat
        if ((dist < 3.9 && (t.phase == PH_WINDUP || t.phase == PH_ACTIVE) &&
             t.act != SK_GRAB && t.act != SK_HOOK && t.act != SK_GUARD && t.act != SK_COUNTER && t.act != SK_BLINK) ||
            (!t.grounded && std::fabs(dx) < 3.9 && t.y > f.y + 0.3 && t.y < f.y + 6.5 &&
             (t.act == SK_SLAM || t.meter >= rc.moves[SK_SLAM].cost))) {
          o.skill = SK_GUARD;
          return o;
        }
        // 3. Against opponent holding Charge: grab if in range, or stay uncommitted to reactively Counter/Guard on release!
        if (t.act == SK_CHARGE && t.phase == PH_CHARGING && dist < 4.2) {
          if (dist < 2.9 && f.meter >= rc.moves[SK_GRAB].cost) {
            o.skill = SK_GRAB; o.has_dir = true; o.dx = sdir; o.dy = -0.30;
            return o;
          }
          if (f.meter >= rc.moves[SK_COUNTER].cost) {
            return o;
          }
        }
        // 4. Airborne slam if safe
        if (!f.grounded && std::fabs(dx) < 3.5 && f.y > t.y + 0.6 && f.y < t.y + 6.5 && f.meter >= rc.moves[SK_SLAM].cost && t.act != SK_GUARD) {
          o.skill = SK_SLAM;
          return o;
        }
        // 5. Heavy punish on opponent in recovery (e.g. slam/recover or charge/recover)
        if (dist < 3.6 && ((t.phase == PH_RECOVER && t.phase_t >= 11) || t.hitstun >= 11)) {
          o.skill = SK_CHARGE; o.has_dir = true; o.dx = sdir; o.dy = -0.42;
          return o;
        }
        if (dist < 2.9) {
          if ((t.act == SK_GUARD || t.act == SK_COUNTER) && f.meter >= rc.moves[SK_GRAB].cost) {
            o.skill = SK_GRAB; o.has_dir = true; o.dx = sdir; o.dy = -0.30;
          } else {
            o.skill = SK_ATTACK; o.has_dir = true; o.dx = sdir; o.dy = (std::fabs(dy) < 0.8) ? -0.42 : dy;
          }
        }
      } else {
        if ((can_slam_anyone || (!f.grounded && std::fabs(dx) < 3.5 && f.y > t.y + 0.6 && f.y < t.y + 6.5)) && f.meter >= rc.moves[SK_SLAM].cost) {
          o.skill = SK_SLAM;
          return o;
        }
        if (dist < 3.6 && ((t.phase == PH_RECOVER && t.phase_t >= 11) || t.hitstun >= 11)) {
          o.skill = SK_CHARGE; o.has_dir = true; o.dx = sdir; o.dy = -0.42;
        } else if (dist < 3.1) {
          if ((t.act == SK_GUARD || t.act == SK_COUNTER || (t.act == SK_CHARGE && t.phase == PH_CHARGING)) && f.meter >= rc.moves[SK_GRAB].cost) {
            o.skill = SK_GRAB; o.has_dir = true; o.dx = sdir; o.dy = -0.30;
          } else if (dy > 1.0 && std::fabs(dx) < 2.0 && f.meter >= rc.moves[SK_UPPERCUT].cost) {
            o.skill = SK_UPPERCUT;
          } else {
            o.skill = SK_ATTACK; o.has_dir = true; o.dx = sdir; o.dy = (std::fabs(dy) < 0.8) ? -0.42 : dy;
          }
        } else if (dist < 4.2 && (t.hitstun > 4 || t.phase == PH_RECOVER)) {
          o.skill = SK_CHARGE; o.has_dir = true; o.dx = sdir; o.dy = -0.42;
        }
      }
    }
    return o;
  }

  // Greedy follow-up policy for OUR fighter during turns 1..K of rollout
  FastOrder my_followup(const FastState &st, int me, int best_target, bool allow_release_charge) const {
    const FastFighter &f = st.fighters[me];
    FastOrder o;
    if (f.out || f.respawn > 0) return o;
    double edge = rc.solids[0].x2;
    if (!f.grounded && (std::fabs(f.x) > edge - 0.3 || f.y < -0.2)) {
      return recovery_heuristic(st, me);
    }

    // If currently guarding and an enemy attack/wave is still about to hit us, KEEP GUARDING!
    if (f.act == SK_GUARD && f.phase == PH_ACTIVE) {
      if (incoming_blockable_threat(st, me, 5)) {
        o.skill = SK_GUARD;
        return o;
      }
    }

    int t_idx = best_target;
    if (t_idx < 0 || st.fighters[t_idx].out || st.fighters[t_idx].respawn > 0) {
      double bd = 1e18;
      for (int j = 0; j < st.n; ++j) {
        if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
        double d2 = (st.fighters[j].x - f.x) * (st.fighters[j].x - f.x) + (st.fighters[j].y - f.y) * (st.fighters[j].y - f.y);
        if (d2 < bd) { bd = d2; t_idx = j; }
      }
    }
    if (t_idx < 0) return o;
    const FastFighter &t = st.fighters[t_idx];
    double dx = (t.x + 4.0 * t.vx) - (f.x + 4.0 * f.vx);
    double dy = (t.y + 4.0 * t.vy) - (f.y + 4.0 * f.vy);
    double dist = std::sqrt((t.x - f.x) * (t.x - f.x) + (t.y - f.y) * (t.y - f.y));

    if (f.act == SK_CHARGE && f.phase == PH_CHARGING) {
      o.has_dir = true; o.dx = dx; o.dy = dy;
      o.move = (dx > 0) ? 1.0 : -1.0;
      if (incoming_blockable_threat(st, me, 4)) {
        o.skill = SK_GUARD;
        return o;
      }
      for (int j = 0; j < st.n; ++j) {
        if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
        const auto &ej = st.fighters[j];
        double edx = ej.x - f.x, edy = ej.y - f.y;
        if (edx * edx + edy * edy < 14.0 && ej.act == SK_GRAB && (ej.phase == PH_WINDUP || ej.phase == PH_ACTIVE)) {
          if (f.meter >= rc.moves[SK_BLINK].cost) {
            o.skill = SK_BLINK; o.dx = (edx > 0) ? -1.0 : 1.0; o.dy = 0.3;
            return o;
          }
        }
        if (edx * edx + edy * edy < 16.0 && ej.act == SK_COUNTER && (ej.phase == PH_WINDUP || (ej.phase == PH_ACTIVE && ej.phase_t > 6))) {
          o.skill = SK_CHARGE;
          return o;
        }
      }
      if (allow_release_charge && dist <= 3.6) {
        o.skill = SK_NONE;
        if (std::fabs(t.y - f.y) < 0.8) {
          o.dx = (dx >= 0) ? 1.0 : -1.0;
          o.dy = -0.42;
        }
      } else {
        o.skill = SK_CHARGE;
      }
      return o;
    }

    if (t.invuln > 0) {
      o.move = (t.x > f.x) ? -1.0 : 1.0;
      return o;
    }

    // Block imminent slam/attack/wave when idle
    if (f.act == SK_NONE && f.gap <= 1 && incoming_blockable_threat(st, me, 3)) {
      o.skill = SK_GUARD;
      return o;
    }

    o.move = (dx > 0) ? 1.0 : -1.0;
    if (t.y > f.y + 2.0 && f.grounded) o.jump = true;
    if (t.y < f.y - 1.5 && f.grounded && f.y > 0.5) o.drop = true;

    if (f.act == SK_NONE && f.gap <= 1) {
      double sdir = (dx >= 0) ? 1.0 : -1.0;
      // Airborne slam follow-up only when opponent is offstage or at KO damage
      if (!f.grounded && std::fabs(t.x - f.x) < 3.4 && f.y > t.y + 0.3 && f.y < t.y + 5.5 &&
          (std::fabs(t.x) > edge - 0.5 || t.damage >= 90.0) && f.meter >= rc.moves[SK_SLAM].cost && t.act != SK_GUARD) {
        o.skill = SK_SLAM;
        return o;
      }
      if (dist < 2.9) {
        if ((t.act == SK_GUARD || t.act == SK_COUNTER) && f.meter >= rc.moves[SK_GRAB].cost) {
          o.skill = SK_GRAB; o.has_dir = true; o.dx = sdir; o.dy = -0.30;
        } else if (t.y > f.y + 0.8 && std::fabs(t.x - f.x) < 1.8 && f.meter >= rc.moves[SK_UPPERCUT].cost && t.act != SK_COUNTER) {
          o.skill = SK_UPPERCUT;
        } else if (t.act != SK_COUNTER) {
          o.skill = SK_ATTACK; o.has_dir = true;
          if (std::fabs(t.y - f.y) < 0.8) {
            o.dx = sdir; o.dy = -0.42;
          } else {
            o.dx = dx; o.dy = dy;
          }
        }
      }
    }
    return o;
  }

  double estimate_opponent_peril(const FastState &st, int j, int me) const {
    const FastFighter &f = st.fighters[j];
    if (f.out || f.respawn > 0) return 0.0;
    if (f.last_hit != me || (st.tick_no - f.last_hit_tick > rc.kill_credit_ticks)) return 0.0;

    double bx, btop;
    rc.current_blast(st.tick_no, bx, btop);
    double edge = rc.solids[0].x2;

    double x = f.x, y = f.y, vx = f.vx, vy = f.vy;
    int hs = f.hitstun;
    for (int k = 0; k < hs + 12; ++k) {
      double next_bx, next_btop;
      rc.current_blast(st.tick_no + k + 1, next_bx, next_btop);
      vy = std::max(vy - rc.gravity, hs > 0 ? -2.0 : -rc.max_fall);
      if (hs > 0) {
        vx *= rc.hit_friction; vy *= rc.hit_friction; --hs;
      } else {
        vx *= rc.air_friction;
        double want = (x > 0 ? -1.0 : 1.0) * rc.run_speed;
        vx += std::max(-rc.air_accel, std::min(rc.air_accel, want - vx));
      }
      x += vx; y += vy;
      if (std::fabs(x) > next_bx || y > next_btop || y < rc.blast_bottom) {
        return 1900.0;
      }
    }

    double peril = 0.0;
    if (std::fabs(x) > edge || y < 0.0) {
      double out_x = std::max(0.0, std::fabs(x) - edge);
      double out_y = std::max(0.0, -y);
      peril += out_x * 26.0 + out_y * 36.0;
      double max_rec_y = (f.air_jumps > 0 ? 4.2 : 0.0) + (f.meter >= 0.25 ? 6.0 : 0.0) + (f.meter >= 0.2 ? 4.5 : 0.0);
      if (y + max_rec_y < -0.5 || (std::fabs(x) < edge - 0.5 && y < rc.solids[0].bottom - 0.5)) {
        peril += 1200.0;
      }
    }
    return peril;
  }

  int select_best_target(const FastState &st, int me) const {
    const FastFighter &f = st.fighters[me];
    int best_j = -1;
    double best_score = -1e18;
    for (int j = 0; j < st.n; ++j) {
      if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
      const FastFighter &v = st.fighters[j];
      double dx = v.x - f.x, dy = v.y - f.y;
      double dist = std::sqrt(dx * dx + dy * dy);
      double sc = -dist * 12.0;
      if (v.invuln > 0) {
        sc -= 500.0;
      } else {
        if (v.hitstun > 0) sc += v.hitstun * 6.5;
        if (v.phase == PH_RECOVER) sc += v.phase_t * 6.0;
        if (v.act == SK_CHARGE && (v.phase == PH_CHARGING || v.phase == PH_WINDUP) && dist < 4.8) sc += 85.0;
        if (v.act == SK_BLINK && v.phase == PH_RECOVER && dist < 4.2) sc += 55.0;
        if (v.pull_t > 0 && v.pulled_by == me) sc += 220.0;
        bool already_dying_for_us = (!v.grounded && (std::fabs(v.x) > 15.0 || v.y < -3.0) && v.last_hit == me);
        if (already_dying_for_us) {
          sc -= 250.0;
        } else {
          sc += std::min(150.0, v.damage) * 0.85;
          if (v.damage >= 70.0 && v.last_hit != me) sc += 45.0;
          if (std::fabs(v.x) > 10.0) sc += 25.0;
        }
      }
      if (sc > best_score) { best_score = sc; best_j = j; }
    }
    return best_j;
  }

  double evaluate_rollout(const FastState &s0, const FastState &s1, const FastOrder &cand,
                          int me, int best_target, int profile, int horizon_turns, int release_turn) const {
    FastState sim = s0;
    SimEvents ev;

    FastOrder orders[4];
    for (int i = 0; i < sim.n; ++i) {
      orders[i] = (i == me) ? cand : predict_opponent(sim, i, profile);
    }
    sim.step_turn(orders, rc, me, &ev);

    double min_blast_margin = 1e9;
    bool ever_under_stage = false;
    bool landed_on_stage = sim.fighters[me].grounded;

    auto check_frame_safety = [&](const FastState &st) {
      const FastFighter &mf = st.fighters[me];
      if (mf.out || mf.respawn > 0) return;
      if (mf.grounded) landed_on_stage = true;
      double bx, btop;
      rc.current_blast(st.tick_no, bx, btop);
      double mx = bx - std::fabs(mf.x);
      double mtop = btop - mf.y;
      double mbot = mf.y - rc.blast_bottom;
      min_blast_margin = std::min({min_blast_margin, mx, mtop, mbot});
      if (std::fabs(mf.x) < rc.solids[0].x2 - 0.3 && mf.y < rc.solids[0].bottom + 0.2) {
        ever_under_stage = true;
      }
    };
    check_frame_safety(sim);

    for (int t = 1; t < horizon_turns && !sim.done; ++t) {
      bool allow_rel = (t >= release_turn);
      for (int i = 0; i < sim.n; ++i) {
        orders[i] = (i == me) ? my_followup(sim, me, best_target, allow_rel) : predict_opponent(sim, i, profile);
      }
      sim.step_turn(orders, rc, me, &ev);
      check_frame_safety(sim);
    }

    const FastFighter &mf0 = s0.fighters[me];
    const FastFighter &mf = sim.fighters[me];
    double score = 0.0;

    if (ev.my_falls > 0 || mf.stocks < mf0.stocks) {
      score -= 25000.0 * ev.my_falls;
      if (mf.out) score -= 50000.0;
    }
    if (ever_under_stage) {
      score -= 3500.0;
    }
    if (min_blast_margin < 5.0) {
      score -= (5.0 - min_blast_margin) * (5.0 - min_blast_margin) * 48.0;
    }

    score += ev.my_kills * 2800.0;
    for (int j = 0; j < sim.n; ++j) {
      if (j != me) {
        score += estimate_opponent_peril(sim, j, me);
      }
    }

    double my_vuln_mult = 14.0 + 0.18 * mf0.damage;
    score -= ev.dmg_taken * my_vuln_mult;
    score -= ev.hits_taken * 40.0;
    score -= ev.got_hooked * 420.0;
    if (mf.hitstun > 0) {
      score -= mf.hitstun * 13.0;
      if (mf.hitstun >= 18) score -= 220.0;
    }
    if (mf.pull_t > 0) score -= 240.0;

    score += ev.dmg_dealt * 9.5;
    score += ev.hits_landed * 28.0;
    score += ev.grabs_landed * 36.0;
    score += ev.hooks_landed * 45.0;
    score += ev.tags_made * 58.0;
    score += ev.counters_made * 120.0;
    score += ev.blocks_made * 34.0;

    for (int j = 0; j < sim.n; ++j) {
      if (j == me) continue;
      double d_inc = sim.fighters[j].damage - s0.fighters[j].damage;
      if (d_inc > 0.1 && sim.fighters[j].last_hit == me) {
        score += d_inc * (s0.fighters[j].damage * 0.09);
        if (s0.fighters[j].last_hit != me && s0.fighters[j].damage >= 50.0) {
          score += s0.fighters[j].damage * 0.75; // High-value kill credit steal/claim!
        }
      }
    }

    double edge = rc.solids[0].x2;
    double bx, btop;
    rc.current_blast(sim.tick_no, bx, btop);
    double safe_edge = std::min(edge - 1.0, bx - 5.0);

    bool started_offstage = !mf0.grounded && (std::fabs(mf0.x) > edge - 0.2 || mf0.y < -0.1);
    if (started_offstage) {
      if (landed_on_stage || (std::fabs(mf.x) <= edge - 0.5 && mf.y >= 0.0)) {
        score += 950.0;
      }
      if (ev.hooks_staged > 0) {
        score += 350.0;
      }
    }

    if (!mf.out && mf.respawn == 0) {
      if (std::fabs(mf.x) > edge || mf.y < 0.0) {
        double ox = std::max(0.0, std::fabs(mf.x) - edge);
        double oy = std::max(0.0, -mf.y);
        score -= ox * ox * 16.0 + oy * oy * 22.0;
        double rec_y = (mf.air_jumps > 0 ? 4.0 : 0.0) + (mf.meter >= 0.25 ? 5.8 : 0.0) + (mf.meter >= 0.20 ? 4.2 : 0.0);
        if (mf.y + rec_y < 0.2) {
          score -= 8000.0;
        }
        if (std::fabs(mf.x) < edge + 0.2 && mf.y < -0.5) {
          score -= (edge + 0.5 - std::fabs(mf.x)) * (-mf.y) * 90.0;
        }
      } else {
        if (std::fabs(mf.x) > safe_edge) {
          double over = std::fabs(mf.x) - safe_edge;
          double shrink_mult = (sim.tick_no > 2800) ? 10.0 : 4.5;
          score -= over * over * (shrink_mult + 0.04 * mf.damage);
        }
        if (btop - mf.y < 7.0) {
          score -= (7.0 - (btop - mf.y)) * 28.0;
        }
      }

      double meter_spent = std::max(0.0, mf0.meter - s1.fighters[me].meter);
      bool productive = (ev.hits_landed > 0 || ev.hooks_landed > 0 || ev.counters_made > 0 ||
                         ev.blocks_made > 0 || (started_offstage && (ev.hooks_staged > 0 || landed_on_stage)));
      if (meter_spent > 0.01 && !productive) {
        double reserve_mult = (s1.fighters[me].meter < 0.45) ? 230.0 : 115.0;
        score -= meter_spent * reserve_mult;
      }
      if (cand.skill == SK_SLAM && ev.my_kills == 0) {
        score -= 135.0;
      }
      if (cand.skill == SK_HOOK && !productive) {
        score -= 170.0;
      }
      if (cand.skill == SK_COUNTER && ev.counters_made == 0) {
        score -= 180.0;
      }
      if (cand.skill == SK_GRAB && ev.grabs_landed == 0) {
        score -= 60.0;
      }
      if (cand.skill == SK_GUARD && ev.blocks_made == 0 && ev.dmg_taken == 0) {
        score -= 18.0;
      }

      double min_enemy_dist = 1e9;
      int enemies_left = 0, enemies_right = 0;
      int my_lock = 0;
      if (mf.phase == PH_RECOVER) {
        my_lock = mf.phase_t + rc.gap_ticks;
      } else if (mf.phase == PH_WINDUP || mf.phase == PH_ACTIVE) {
        if (mf.act != SK_GUARD && mf.act != SK_BLINK) {
          my_lock = mf.phase_t + rc.moves[mf.act].recover + rc.gap_ticks;
        }
      } else {
        my_lock = mf.gap;
      }
      int worst_frame_disadv = 0;
      for (int j = 0; j < sim.n; ++j) {
        if (j == me || sim.fighters[j].out || sim.fighters[j].respawn > 0) continue;
        const auto &ej = sim.fighters[j];
        double dx = ej.x - mf.x, dy = ej.y - mf.y;
        double d = std::sqrt(dx * dx + dy * dy);
        min_enemy_dist = std::min(min_enemy_dist, d);
        if (std::fabs(dx) < 5.5 && std::fabs(dy) < 3.0) {
          if (dx < 0) enemies_left++;
          else enemies_right++;
        }
        if (my_lock >= 4 && d < 3.9) {
          int ej_lock = std::max(ej.hitstun, ej.pull_t);
          if (ej.phase == PH_RECOVER) ej_lock = std::max(ej_lock, ej.phase_t + rc.gap_ticks);
          else if (ej.act == SK_NONE) ej_lock = std::max(ej_lock, ej.gap);
          int disadv = my_lock - ej_lock;
          if (disadv > worst_frame_disadv) worst_frame_disadv = disadv;
        }
      }
      if (enemies_left > 0 && enemies_right > 0) {
        score -= 24.0;
      }
      if (mf.act == SK_SLAM && mf.phase == PH_RECOVER && min_enemy_dist < 4.5 && ev.my_kills == 0) {
        score -= 140.0;
      }
      if (worst_frame_disadv >= 4) {
        score -= (worst_frame_disadv * 22.0 + 45.0) * (1.0 + mf.damage * 0.008);
      } else if (!productive && (mf.phase == PH_RECOVER || (mf.act == SK_HOOK && !mf.reeling)) && min_enemy_dist < 4.5) {
        score -= std::min(30, mf.phase_t) * 5.0;
      }

      if (best_target >= 0 && !sim.fighters[best_target].out && sim.fighters[best_target].respawn == 0) {
        const FastFighter &bt = sim.fighters[best_target];
        double dx = bt.x - mf.x, dy = bt.y - mf.y;
        double dist = std::sqrt(dx * dx + dy * dy);
        if (bt.invuln > 0) {
          if (dist < 6.5) score -= (6.5 - dist) * 22.0;
        } else if (!started_offstage) {
          double ideal_d = (mf.gap > 0 || mf.phase == PH_RECOVER) ? 3.8 : 2.1;
          score -= std::fabs(dist - ideal_d) * 3.5;
          score -= std::fabs(dy) * 1.8;
          if ((dx > 0 && mf.facing == 1) || (dx < 0 && mf.facing == -1)) score += 2.0;
        }
      }
    }

    return score;
  }

  bool is_counter_viable(const FastState &st, int me) const {
    const FastFighter &mf = st.fighters[me];
    for (int j = 0; j < st.n; ++j) {
      if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
      const FastFighter &ej = st.fighters[j];
      double dx = ej.x - mf.x, dy = ej.y - mf.y;
      double d2 = dx * dx + dy * dy;
      if (d2 > 22.0) continue;
      if (ej.act == SK_ATTACK || ej.act == SK_CHARGE || ej.act == SK_UPPERCUT) {
        if (ej.phase == PH_WINDUP && ej.phase_t >= 3 && ej.phase_t <= 14) return true;
        if (ej.phase == PH_ACTIVE && ej.phase_t >= 4) return true;
        if (ej.act == SK_CHARGE && ej.phase == PH_CHARGING && ej.charge >= 5 && d2 <= 14.5) return true;
      }
    }
    for (int k = 0; k < st.n_shots; ++k) {
      const FastShot &s = st.shots[k];
      if (s.owner == me || s.kind != SK_WAVE || s.dead) continue;
      for (int t = 4; t <= 14 && t <= s.ttl; ++t) {
        double sx = s.x + s.vx * t;
        double sy = s.y + s.vy * t;
        double dx = mf.x - sx, dy = (mf.y + rc.height * 0.5) - sy;
        double reach = rc.moves[SK_WAVE].radius + rc.height * 0.5;
        if (dx * dx + dy * dy <= reach * reach) return true;
      }
    }
    return false;
  }

  J step(const J &obs) {
    auto t_start = std::chrono::steady_clock::now();
    FastState st;
    int me = 0;
    sync_from_obs(obs, st, me);

    const FastFighter &mf = st.fighters[me];
    if (mf.out || mf.respawn > 0) {
      prev_state = st;
      has_prev = true;
      last_my_order = FastOrder{};
      return J::object{};
    }

    int best_target = select_best_target(st, me);
    double edge = rc.solids[0].x2;
    bool offstage = !mf.grounded && (std::fabs(mf.x) > edge - 0.2 || mf.y < -0.1);
    int horizon_turns = offstage ? 9 : 6;

    struct CandEntry {
      FastOrder ord;
      int rel_turn = 1;
    };
    std::vector<CandEntry> cands;
    cands.reserve(80);

    auto add_cand = [&](FastOrder o, int rel_turn = 1) {
      cands.push_back({o, rel_turn});
    };

    bool is_charging = (mf.act == SK_CHARGE && mf.phase == PH_CHARGING);
    bool locked_against_overhead = (mf.gap > 0 || mf.phase == PH_RECOVER);
    bool overhead_slam_threat = false;
    if (locked_against_overhead && mf.grounded) {
      for (int j = 0; j < st.n; ++j) {
        if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
        const auto &ej = st.fighters[j];
        if (!ej.grounded && std::fabs(ej.x - mf.x) < 4.5 && ej.y > mf.y + 0.5 && ej.y < mf.y + 7.0 &&
            (ej.act == SK_SLAM || ej.meter >= rc.moves[SK_SLAM].cost)) {
          overhead_slam_threat = true;
          break;
        }
      }
    }
    for (double mv : {-1.0, 0.0, 1.0}) {
      for (bool jmp : {false, true}) {
        if (jmp && !mf.grounded && mf.air_jumps <= 0) continue;
        if (jmp && overhead_slam_threat) continue; // Don't jump straight up into an overhead slam while unable to act!
        FastOrder o;
        o.move = mv;
        o.jump = jmp;
        if (is_charging) {
          o.skill = SK_CHARGE;
          if (best_target >= 0) {
            o.has_dir = true;
            o.dx = st.fighters[best_target].x - mf.x;
            o.dy = st.fighters[best_target].y - mf.y;
          }
          add_cand(o, 1);
          add_cand(o, 2);
        } else {
          add_cand(o);
        }
      }
      if (mf.grounded && mf.y > 0.5) {
        FastOrder o;
        o.move = mv;
        o.drop = true;
        if (is_charging) o.skill = SK_CHARGE;
        add_cand(o, 1);
      }
    }

    if (is_charging) {
      for (int j = 0; j < st.n; ++j) {
        if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0) continue;
        const auto &tj = st.fighters[j];
        for (double lead : {0.0, 6.0}) {
          double ldx = (tj.x + lead * tj.vx) - mf.x;
          double ldy = (tj.y + lead * tj.vy) - mf.y;
          double sdir = (ldx >= 0) ? 1.0 : -1.0;
          FastOrder o;
          o.skill = SK_NONE;
          o.has_dir = true;
          o.dx = ldx;
          o.dy = ldy;
          o.move = sdir;
          add_cand(o, 1);

          // Low line-drive angle (cancels lift=0.5 for maximum horizontal KO speed!)
          FastOrder o_low = o;
          o_low.dx = sdir;
          o_low.dy = -0.45;
          add_cand(o_low, 1);
        }
      }
      if (!offstage) {
        FastOrder o; o.skill = SK_GUARD; add_cand(o);
      }
      if (mf.meter >= rc.moves[SK_BLINK].cost) {
        const double bdirs[4][2] = {{-1.0, 0.2}, {1.0, 0.2}, {-0.7, 0.7}, {0.7, 0.7}};
        for (auto &bd : bdirs) {
          FastOrder o; o.skill = SK_BLINK; o.has_dir = true; o.dx = bd[0]; o.dy = bd[1];
          add_cand(o);
        }
      }
    }

    // If currently guarding on stage, always include keeping guard active as a candidate!
    if (!offstage && mf.act == SK_GUARD && mf.phase == PH_ACTIVE) {
      FastOrder o; o.skill = SK_GUARD; add_cand(o);
    }

    bool can_act_this_turn = (!is_charging && mf.act == SK_NONE && mf.gap <= 2 && mf.hitstun <= 2 && mf.pull_t <= 2);

    if (can_act_this_turn) {
      if (!offstage) {
        FastOrder o; o.skill = SK_GUARD; add_cand(o);
      }
      if (mf.meter >= rc.moves[SK_COUNTER].cost && is_counter_viable(st, me)) {
        FastOrder o; o.skill = SK_COUNTER; add_cand(o);
      }

      if (mf.meter >= rc.moves[SK_BLINK].cost) {
        const double bdirs[7][2] = {
          {-1.0, 0.1}, {1.0, 0.1},
          {-0.8, 0.6}, {0.8, 0.6},
          {0.0, 1.0},
          {-0.5, 0.86}, {0.5, 0.86}
        };
        for (auto &bd : bdirs) {
          FastOrder o;
          o.skill = SK_BLINK;
          o.has_dir = true;
          o.dx = bd[0]; o.dy = bd[1];
          o.move = (bd[0] >= 0) ? 1.0 : -1.0;
          add_cand(o);
        }
        if (offstage) {
          for (double ty : {1.8, 0.8}) {
            double tx = (mf.x >= 0) ? (edge - 0.5) : -(edge - 0.5);
            FastOrder o;
            o.skill = SK_BLINK;
            o.has_dir = true;
            o.dx = tx - mf.x;
            o.dy = ty - mf.y;
            o.move = (mf.x > 0) ? -1.0 : 1.0;
            add_cand(o);
          }
        }
      }

      if (mf.meter >= rc.moves[SK_UPPERCUT].cost) {
        for (double mv : {-1.0, 0.0, 1.0}) {
          FastOrder o; o.skill = SK_UPPERCUT; o.move = mv;
          add_cand(o);
        }
        if (!mf.grounded && mf.air_jumps > 0) {
          FastOrder o; o.skill = SK_UPPERCUT; o.jump = true; o.move = (mf.x > 0) ? -1.0 : 1.0;
          add_cand(o);
        }
      }

      if (mf.meter >= rc.moves[SK_SLAM].cost && !offstage) {
        if (!mf.grounded) {
          for (double mv : {-1.0, 0.0, 1.0}) {
            FastOrder o; o.skill = SK_SLAM; o.move = mv;
            add_cand(o);
          }
        } else {
          bool ko_target_near = false;
          for (int j = 0; j < st.n; ++j) {
            if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0 || st.fighters[j].invuln > 0) continue;
            if (std::fabs(st.fighters[j].x - mf.x) < 4.0 && st.fighters[j].damage >= 85.0) {
              ko_target_near = true;
              break;
            }
          }
          if (ko_target_near) {
            for (double mv : {-1.0, 0.0, 1.0}) {
              FastOrder o; o.jump = true; o.skill = SK_SLAM; o.move = mv;
              add_cand(o);
            }
          }
        }
      }

      if (offstage && mf.meter >= rc.moves[SK_HOOK].cost && mf.hook_cd == 0) {
        double target_x = (mf.x >= 0) ? (edge - 0.5) : -(edge - 0.5);
        for (double target_y : {-0.5, -1.5, -2.5, -3.3}) {
          FastOrder o;
          o.skill = SK_HOOK;
          o.has_dir = true;
          o.dx = target_x - mf.x;
          o.dy = target_y - (mf.y + rc.height * 0.5);
          o.move = (mf.x > 0) ? -1.0 : 1.0;
          add_cand(o);
        }
      }

      for (int j = 0; j < st.n; ++j) {
        if (j == me || st.fighters[j].out || st.fighters[j].respawn > 0 || st.fighters[j].invuln > 3) continue;
        const auto &tj = st.fighters[j];
        double raw_dx = tj.x - mf.x, raw_dy = tj.y - mf.y;
        double dist = std::sqrt(raw_dx * raw_dx + raw_dy * raw_dy);
        double lead4_dx = (tj.x + 4.0 * tj.vx) - (mf.x + 4.0 * mf.vx);
        double lead4_dy = (tj.y + 4.0 * tj.vy) - (mf.y + 4.0 * mf.vy);

        if (dist < 4.8) {
          double sdir = (lead4_dx >= 0) ? 1.0 : -1.0;
          FastOrder o;
          o.skill = SK_ATTACK; o.has_dir = true;
          o.dx = lead4_dx; o.dy = lead4_dy;
          o.move = sdir;
          add_cand(o);

          // Low horizontal line-drive angle (cancels lift=0.5)
          FastOrder o_low = o;
          o_low.dx = sdir;
          o_low.dy = -0.45;
          add_cand(o_low);

          if (std::fabs(lead4_dx) < 0.65) {
            FastOrder o_out = o;
            o_out.dx = (tj.x >= 0) ? 1.0 : -1.0;
            o_out.dy = -0.45;
            add_cand(o_out);
          }
        }

        if (dist < 4.5 && mf.meter >= rc.moves[SK_GRAB].cost) {
          double lead6_dx = (tj.x + 6.0 * tj.vx) - (mf.x + 6.0 * mf.vx);
          double lead6_dy = (tj.y + 6.0 * tj.vy) - (mf.y + 6.0 * mf.vy);
          double sdir = (lead6_dx >= 0) ? 1.0 : -1.0;
          FastOrder o;
          o.skill = SK_GRAB; o.has_dir = true;
          o.dx = lead6_dx;
          o.dy = lead6_dy;
          o.move = sdir;
          add_cand(o);

          // Pure horizontal / downward line-drive throw toward stage edge!
          FastOrder o_low = o;
          o_low.dx = sdir;
          o_low.dy = -0.35;
          add_cand(o_low);

          if (std::fabs(lead6_dx) < 0.65) {
            FastOrder o_out = o;
            o_out.dx = (tj.x >= 0) ? 1.0 : -1.0;
            o_out.dy = -0.35;
            add_cand(o_out);
          }
        }

        if (dist < 6.5 && !offstage) {
          double sdir = (lead4_dx >= 0) ? 1.0 : -1.0;
          for (bool jmp : {false, true}) {
            if (jmp && !mf.grounded) continue;
            FastOrder o;
            o.skill = SK_CHARGE; o.has_dir = true;
            o.dx = sdir;
            o.dy = (std::fabs(lead4_dy) < 0.8) ? -0.42 : (lead4_dy / std::max(0.5, std::fabs(lead4_dx)));
            o.move = sdir;
            o.jump = jmp;
            add_cand(o, 1);
            add_cand(o, 2);
          }
        }

        if (!offstage && dist >= 5.0 && dist <= 15.0 && mf.meter >= 0.96) {
          double t_flight = dist / rc.moves[SK_WAVE].speed + 6.0;
          FastOrder o;
          o.skill = SK_WAVE; o.has_dir = true;
          o.dx = (tj.x + t_flight * 0.6 * tj.vx) - mf.x;
          o.dy = (tj.y + t_flight * 0.6 * tj.vy) - mf.y;
          add_cand(o);
        }
      }
    }

    int best_idx = 0;
    double best_val = -1e18;
    double time_budget_ms = (st.turn == 0) ? 400.0 : 28.0;

    for (int idx = 0; idx < int(cands.size()); ++idx) {
      if ((idx & 3) == 0 && idx > 0) {
        double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
        if (elapsed_ms > time_budget_ms) break;
      }
      const auto &ce = cands[idx];
      FastState s1 = st;
      FastOrder o0[4];
      for (int i = 0; i < st.n; ++i) {
        o0[i] = (i == me) ? ce.ord : predict_opponent(st, i, 0);
      }
      s1.step_turn(o0, rc, me, nullptr);

      double v_agg = evaluate_rollout(st, s1, ce.ord, me, best_target, 0, horizon_turns, ce.rel_turn);
      double v_def = evaluate_rollout(st, s1, ce.ord, me, best_target, 1, horizon_turns, ce.rel_turn);
      double worst_v = std::min(v_agg, v_def);
      double avg_v = 0.52 * v_agg + 0.48 * v_def;
      double val = 0.60 * avg_v + 0.40 * worst_v;

      if (val > best_val) {
        best_val = val;
        best_idx = idx;
      }
    }

    FastOrder chosen = cands[best_idx].ord;
    if (chosen.drop && mf.grounded) {
      my_drop_counter = rc.drop_ticks - 3;
    } else {
      my_drop_counter = std::max(0, my_drop_counter - 3);
    }
    prev_state = st;
    has_prev = true;
    last_my_order = chosen;
    return chosen.to_json();
  }
};

inline J step(const J &obs) {
  static BotContext ctx;
  return ctx.step(obs);
}

}  // namespace smash_bot
