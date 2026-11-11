#include "include/step_worker.hpp"
#include "include/policy_open.hpp"
#include "include/policy_duel.hpp"

int main() {
    return kb_step_worker::run([](const J& observation, J& memory) {
        int alive = 0;
        for (const auto& player : observation.at("players").as_array())
            alive += !player.at("out").as_bool();
        // Preserve the broadly tested crowd policy, then use the separately
        // tested duel policy in the four-player format's last confrontation.
        const bool duel = observation.at("n").get<int>() == 4 && alive == 2;
        const bool was_duel = memory.is_object() && memory.contains("duel")
            && memory.at("duel").as_bool();
        // Opponents change goals when the last third party falls. Do not carry
        // an acceleration estimate from that different tactical situation.
        J input_memory = duel && !was_duel ? J() : memory;
        auto result = duel ? policy_duel::decide(observation, input_memory)
                           : policy_open::decide(observation, input_memory);
        result.second["duel"] = duel;
        return result;
    });
}
