#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace knockback {
struct Body { double x, y, vx, vy; };
inline std::optional<std::pair<double,double>> unit(double dx, double dy) {
    double big = std::max(std::abs(dx), std::abs(dy));
    if (big == 0) return std::nullopt;
    dx /= big; dy /= big;
    double length = std::sqrt(dx * dx + dy * dy);
    return std::pair<double,double>{dx / length, dy / length};
}
// Keep operation order aligned with Game._contact. Do not enable fast-math or FMA contraction.
inline double pair(Body& a, Body& b, double reach, double ma, double mb, double restitution) {
    double dx = b.x - a.x, dy = b.y - a.y;
    double d2 = dx * dx + dy * dy;
    if (d2 >= reach * reach) return 0.0;
    double d = std::sqrt(d2);
    double nx = d > 0 ? dx / d : 1.0, ny = d > 0 ? dy / d : 0.0;
    double share = mb / (ma + mb);
    a.x -= nx * (reach - d) * share;
    a.y -= ny * (reach - d) * share;
    b.x += nx * (reach - d) * (1 - share);
    b.y += ny * (reach - d) * (1 - share);
    double approach = (a.vx - b.vx) * nx + (a.vy - b.vy) * ny;
    if (approach <= 0) return 0.0;
    double impulse = (1 + restitution) * approach / (1 / ma + 1 / mb);
    a.vx -= impulse / ma * nx; a.vy -= impulse / ma * ny;
    b.vx += impulse / mb * nx; b.vy += impulse / mb * ny;
    return impulse;
}
}
