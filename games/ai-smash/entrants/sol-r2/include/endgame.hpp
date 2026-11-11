#pragma once

#include "tactics.hpp"
#include <optional>
#include <tuple>

namespace ring {

// At full shrink the only valid horizontal coordinate is exactly zero.
// On the main floor the rounded dynamics can be searched on an integer grid.
// A guard can stop movement after just one tick if started through a one-tick gap.
class Center {
    struct Axis {
        int x = 0, v = 0, guard = 0, gap = 0;
    };
    struct Node {
        Axis axis;
        Order first;
        double value;
    };
    const Rules& r;
    const int speed, accel;

    Axis drive(Axis a, int wanted, bool guard, int tick, bool* valid = nullptr) const {
        for (int t = 1; t <= r.per_turn; ++t) {
            if (a.guard && !guard) {
                a.guard = 0;
                a.gap = r.gap;
            }
            if (a.guard) {
                if (--a.guard == 0) a.gap = r.guard_lockout;
            } else if (a.gap > 0) {
                --a.gap;
            } else if (guard) {
                a.guard = r.guard_max;
            }
            a.v = a.guard ? 0 : a.v + std::clamp(wanted - a.v, -accel, accel);
            a.x += a.v;
            if (valid && double(std::abs(a.x)) / r.precision > r.bounds(tick + t).x) *valid = false;
        }
        return a;
    }

    int toward_zero(Axis a, bool guard, bool settle) const {
        int low = -speed, high = speed;
        while (low < high) {
            const int mid = low + (high - low) / 2;
            Axis b = drive(a, mid, guard, 0);
            if (settle) b = drive(b, 0, false, 0);
            if (b.x < 0) low = mid + 1;
            else high = mid;
        }
        return low;
    }

    explicit Center(const Rules& rules)
        : r(rules), speed(int(std::round(r.run_speed * r.precision))),
          accel(int(std::round(r.ground_accel * r.precision))) {}

    std::optional<Order> search(const Fighter& a, int tick) const {
        const Axis root{int(std::round(a.p.x * r.precision)), int(std::round(a.v.x * r.precision)),
                        a.guarding() ? a.left : 0, a.gap};
        if (!root.x && !root.v) return std::nullopt;
        constexpr std::size_t width = 22;
        std::vector<Node> frontier{{root, {}, 0}}, next;
        next.reserve(width * 36);
        std::optional<Order> best;
        for (int ply = 0; ply < 7; ++ply) {
            next.clear();
            for (const Node& parent : frontier) {
                const Axis p = parent.axis;
                for (bool guard : {false, true}) {
                    const int exact = toward_zero(p, guard, false);
                    const int brake = toward_zero(p, guard, true);
                    const std::array<int, 18> speeds = {0, -speed, speed, -accel - 1, accel + 1,
                        -accel / 2, accel / 2, p.v, -p.x / (r.per_turn + 1), -p.x / r.per_turn,
                        exact - 2, exact - 1, exact, exact + 1, exact + 2, brake - 1, brake, brake + 1};
                    for (int v : speeds) {
                        v = std::clamp(v, -speed, speed);
                        bool safe = true;
                        const Axis b = drive(p, v, guard, tick + ply * r.per_turn, &safe);
                        if (!safe) continue;
                        const Order first = ply ? parent.first : Order{double(v) / speed, {}, guard ? Guard : None};
                        if (b.x == 0 && b.v == 0) return first;
                        const double braking = double(b.v) * std::abs(b.v) / (2.0 * accel);
                        double value = std::abs(b.x + braking) + 0.35 * std::abs(b.x) + 0.48 * std::abs(b.v);
                        if (b.gap == 1) value -= 11;
                        if (b.guard > r.per_turn) value -= 4;
                        if (b.gap > r.gap) value += 1.1 * b.gap;
                        next.push_back({b, first, value});
                    }
                }
            }
            if (next.empty()) break;
            std::stable_sort(next.begin(), next.end(), [](const Node& a, const Node& b) { return a.value < b.value; });
            best = next.front().first;
            frontier.clear();
            for (const Node& n : next) {
                bool duplicate = false;
                for (const Node& p : frontier) {
                    const Axis a = p.axis, b = n.axis;
                    if (std::tie(a.x, a.v, a.guard, a.gap) == std::tie(b.x, b.v, b.guard, b.gap)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) frontier.push_back(n);
                if (frontier.size() == width) break;
            }
        }
        return best;
    }

public:
    static std::optional<Order> choose(const World& w, int i) {
        const Fighter& a = w.f[i];
        const Rules& r = w.r();
        if (r.bounds(w.tick + 12).x > 2.2 || !a.grounded || a.p.y != r.solids.front().top ||
            a.stun || a.pull || (a.act != None && !a.guarding())) return std::nullopt;
        return Center(r).search(a, w.tick);
    }
};

}  // namespace ring
