#include "include/baseline_policy.hpp"
#include "include/step_worker.hpp"
#include <algorithm>

namespace {
using smash_baseline::J;
using smash_baseline::get;

J step(const J &obs) {
    J order = J::object{};
    const J &players = obs.at("players");
    const J &me = players.at(obs.at("seat").get<int>());
    if (me.at("respawn").get<int>() > 0 || smash_baseline::recover(me, obs.at("rules"), order)) return order;
    const int target = smash_baseline::nearest(obs);
    if (target < 0) return order;

    const double x = get(me, "x");
    const J &foe = players.at(target);
    const double dx = get(foe, "x") - x;
    const double dy = get(foe, "y") - get(me, "y");
    const double distance = std::hypot(dx, dy);
    order["move"] = std::clamp(-x / 3.0, -1.0, 1.0);
    order["dir"] = J::array{dx, dy};

    const J &phase = foe.at("phase");
    const J &act = foe.at("act");
    if (distance < 3.2 && (phase == J("windup") || phase == J("active")) &&
        act != J("grab") && act != J("hook")) {
        order["skill"] = "guard";
    } else if (me.at("phase") == J("charging")) {
        if (distance > 2.0 && get(me, "charge") < 24) order["skill"] = "charge";
    } else if (get(foe, "invuln") == 0 && distance < 3.8) {
        order["skill"] = distance < 2.2 ? "attack" : "charge";
    }
    return order;
}
} // namespace

int main() { return sm_step_worker::run_policy(step); }
