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
    const double x = get(me, "x");
    const double dx = get(foe, "x") - x;
    const double dy = get(foe, "y") - get(me, "y");
    const double toward = dx > 0 ? 1.0 : -1.0;
    const double stage_edge = get(rules.at("stage").at("solids").at(0), "x2");
    const J &wave = rules.at("moves").at("wave");
    if (std::abs(x) > stage_edge - 2.0) order["move"] = x > 0 ? -1.0 : 1.0;
    else if (std::abs(dx) < 6.0) order["move"] = -toward;
    else if (std::abs(dx) > 10.0) order["move"] = toward;
    if (get(foe, "invuln") > 0) return order;

    order["dir"] = J::array{dx, dy};
    if (std::hypot(dx, dy) < 2.2) order["skill"] = "attack";
    else if (std::abs(dx) < 12.0 && std::abs(dy) < 2.0 && get(me, "meter") >= get(wave, "cost")) {
        order["skill"] = "wave";
    }
    return order;
}
} // namespace

int main() { return sm_step_worker::run_policy(step); }
