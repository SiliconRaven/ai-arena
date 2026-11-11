#include "include/baseline_policy.hpp"
#include "include/step_worker.hpp"

namespace {
using smash_baseline::J;
using smash_baseline::get;

J step(const J &obs) {
    J order = J::object{};
    const J &players = obs.at("players");
    const J &me = players.at(obs.at("seat").get<int>());
    const J &rules = obs.at("rules");
    if (me.at("respawn").get<int>() > 0 || smash_baseline::recover(me, rules, order)) return order;
    const int target = smash_baseline::nearest(obs);
    if (target < 0) return order;

    const J &foe = players.at(target);
    const double dx = get(foe, "x") - get(me, "x");
    const double dy = get(foe, "y") - get(me, "y");
    const J &moves = rules.at("moves");
    order["move"] = dx > 0 ? 1.0 : -1.0;
    if (dy > 2.0 && me.at("grounded").as_bool()) order["jump"] = true;
    if (get(foe, "invuln") > 0) return order;

    order["dir"] = J::array{dx, dy};
    if (std::hypot(dx, dy) < 2.4) {
        const bool can_grab = get(me, "meter") >= get(moves.at("grab"), "cost");
        order["skill"] = foe.at("act") == J("guard") && can_grab ? "grab" : "attack";
    } else if (std::abs(dx) < 1.5 && dy > 0 && dy < 3.0 &&
               get(me, "meter") >= get(moves.at("uppercut"), "cost")) {
        order["skill"] = "uppercut";
    }
    return order;
}
} // namespace

int main() { return sm_step_worker::run_policy(step); }
