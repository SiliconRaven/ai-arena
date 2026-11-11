#pragma once

#include "tactics.hpp"
#include <chrono>
#include <cstdint>
#include <unordered_set>

namespace ring {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

inline bool thin_floor(const World& w, const Fighter& a) {
    return a.grounded && a.p.y > w.r().solids.front().top + 0.1;
}

inline bool emergency(const World& w, int i) {
    const Fighter& a = w.f[i];
    if (offstage(w, a)) return true;
    const Bounds b = w.r().bounds(w.tick + 9);
    const Solid& stage = w.r().solids.front();
    const Vec future = project(w, a, 9);
    const double side = std::min(b.x - std::min(0.9, b.x * 0.25), stage.right + 0.8);
    return std::abs(future.x) > side || future.y > b.top - 0.65;
}

class Rescue {
    struct Node {
        Fighter a;
        Shot hook;
        Order first;
        double value = 0;
    };

    static std::uint64_t key(const Fighter& a) {
        std::uint64_t code = 1469598103934665603ULL;
        const auto add = [&](int k) { code = (code ^ std::uint32_t(k)) * 1099511628211ULL; };
        add(int(std::round(a.p.x * 4)));
        add(int(std::round(a.p.y * 4)));
        add(int(std::round(a.v.x * 11)));
        add(int(std::round(a.v.y * 11)));
        add(int(a.meter * 25));
        add(a.act);
        add(a.phase);
        add(a.left);
        add(a.gap);
        add(a.air);
        add(a.drop > 0);
        add(a.stun);
        add(a.grounded);
        add(a.reeling);
        add(int(a.dir.x * 8));
        add(int(a.dir.y * 8));
        return code;
    }

    static double quality(const World& w, const Fighter& a, int ticks) {
        const Solid& s = w.r().solids.front();
        const Bounds b = w.r().bounds(ticks + 8);
        const double low = std::max(0.0, s.top - a.p.y);
        const double edge = std::max({0.0, a.p.x - s.right + 0.65, s.left + 0.65 - a.p.x});
        const double margin = std::min({b.x - std::abs(a.p.x), b.top - a.p.y, a.p.y - b.bottom});
        const double projected = std::abs(a.p.x + a.v.x * 4);
        const double approach = low > 0.2 ? std::max(0.0, edge - 1.45) : edge;
        return -2.5 * low - 1.7 * approach - 0.17 * std::abs(a.p.x)
               - 0.8 * std::max(0.0, projected - b.x + 1.2) - 2.4 * std::max(0.0, 1.6 - margin)
               + 0.45 * a.v.y + 0.9 * a.meter + 0.43 * a.air;
    }

    static int actions(const World& w, int i, std::array<Order, 22>& list) {
        const Fighter& a = w.f[i];
        const Rules& r = w.r();
        const Solid& s = r.solids.front();
        const double inward = sign(stage_center(w) - a.p.x);
        int size = 0;
        const auto add = [&](Order o) { list[size++] = o; };
        const bool pinned = a.stun >= r.per_turn || a.pull >= r.per_turn || (a.act != None && !a.guarding() &&
                            !a.charging() && ready_in(a, r) >= r.per_turn);
        if (pinned) {
            add({inward});
            if (thin_floor(w, a)) add({inward, {}, None, false, true});
            return size;
        }
        const double safe = std::min(w.r().bounds(w.tick + 18).x * 0.43, s.right - 2.3);
        add({steer(w, a, std::clamp(a.p.x, -safe, safe)), {}, a.charging() ? Charge : None});
        add({inward, {}, a.charging() ? Charge : None});
        add({});
        add({-inward, {}, a.charging() ? Charge : None});
        if (a.mobile() && (a.air > 0 || a.grounded)) {
            add({inward, {}, a.charging() ? Charge : None, true});
            add({0, {}, a.charging() ? Charge : None, true});
            if (a.p.y < s.top) add({-inward, {}, a.charging() ? Charge : None, true});
        }
        if (thin_floor(w, a)) add({inward, {}, None, false, true});
        const bool can_start = a.act == None || a.charging();
        if (can_start && a.meter + r.refill * r.per_turn >= r.moves[Blink].cost) {
            for (const Vec d : {Vec{inward, 0}, Vec{inward, 0.6}, Vec{inward, 1.6}, Vec{0, 1}, Vec{-inward, 2.0}, Vec{inward, -0.5}, Vec{0, -1}}) {
                add({0, d, Blink});
            }
        }
        if (a.act == None && a.meter + r.refill * r.per_turn >= r.moves[Upper].cost) add({inward, {}, Upper});
        if (a.act == None && a.meter + r.refill * r.per_turn >= r.moves[Hook].cost && a.hook_cd < r.per_turn) {
            const double edge = inward < 0 ? s.right : s.left;
            for (double height : {s.top - 0.22, (s.top + s.bottom) * 0.5}) {
                const Vec d = Vec{edge + 1.4 * inward, height} - a.p - Vec{0, r.half_height};
                if (norm2(d) < 100) add({inward, d, Hook});
            }
        }
        if (can_start && !a.grounded && a.p.y > s.top + 4 && w.r().bounds(w.tick + 9).top < a.p.y + 2.4) {
            add({inward, {}, Slam});
        }
        return size;
    }

public:
    static Order choose(const World& w, int i, Deadline limit) {
        constexpr int width = 23, depth = 19;
        World solo = w;
        for (int k = 0; k < seats; ++k) if (k != i) solo.f[k].out = true;
        Node root{w.f[i], {}, {}, 0};
        for (int k = 0; k < w.shot_count; ++k) {
            if (w.shots[k].owner == i && w.shots[k].kind == Hook) root.hook = w.shots[k];
        }
        std::vector<Node> frontier{root}, next;
        frontier.reserve(width);
        next.reserve(width * 16);
        Order best = simple_rescue(w, i), fallback = best;
        double best_value = -1e9;
        const int period = w.r().per_turn;
        const Solid& s = w.r().solids.front();
        for (int ply = 0; ply < depth; ++ply) {
            next.clear();
            const int tick = w.tick + ply * period;
            for (const Node& parent : frontier) {
                solo.tick = tick;
                solo.f[i] = parent.a;
                std::array<Order, 22> moves{};
                const int size = actions(solo, i, moves);
                for (int k = 0; k < size; ++k) {
                    solo.tick = tick;
                    solo.f[i] = parent.a;
                    solo.shot_count = parent.hook.ttl > 0 ? 1 : 0;
                    if (solo.shot_count) solo.shots[0] = parent.hook;
                    Orders orders{};
                    orders[i] = moves[k];
                    for (int t = 0; t < period; ++t) solo.step(orders, t == 0);
                    const Fighter& a = solo.f[i];
                    if (a.stocks != root.a.stocks) continue;
                    const Order first = ply == 0 ? moves[k] : parent.first;
                    const double value = quality(w, a, solo.tick);
                    if (a.grounded && a.p.x > s.left + 0.75 && a.p.x < s.right - 0.75 &&
                        std::abs(a.p.x + a.v.x * 3) < w.r().bounds(solo.tick + 12).x + 1e-9) {
                        const double settled = 3.6 * a.meter + 0.22 * value - 0.24 * ply - 0.09 * ready_in(a, w.r());
                        if (settled > best_value) {
                            best_value = settled;
                            best = first;
                        }
                    }
                    next.push_back({a, solo.shot_count ? solo.shots[0] : Shot{}, first, value});
                }
            }
            if (next.empty()) break;
            const std::size_t keep = std::min(next.size(), std::size_t(width * 3));
            std::partial_sort(next.begin(), next.begin() + keep, next.end(),
                              [](const Node& a, const Node& b) { return a.value > b.value; });
            fallback = next.front().first;
            frontier.clear();
            std::unordered_set<std::uint64_t> seen;
            seen.reserve(keep);
            for (std::size_t k = 0; k < keep && frontier.size() < width; ++k) {
                if (seen.insert(key(next[k].a)).second) frontier.push_back(next[k]);
            }
            if ((best_value > -1e8 && ply >= 7) || Clock::now() >= limit) break;
        }
        return best_value > -1e8 ? best : fallback;
    }
};

}  // namespace ring
