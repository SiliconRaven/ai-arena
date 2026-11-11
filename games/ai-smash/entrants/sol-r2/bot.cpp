#include "include/planner.hpp"
#include "include/step_worker.hpp"
#include <optional>

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::optional<ring::Rules> rules;
    ring::Readback readback;
    return sm_step_worker::run([&](const ring::J& obs, ring::J& memory) {
        const auto deadline = ring::Clock::now() + std::chrono::milliseconds(14);
        if (!rules) rules.emplace(obs.at("rules"));
        const ring::World world = readback.read(obs, *rules);
        const ring::Order order = ring::Planner(world, readback.habit, readback.me, deadline).choose();
        readback.record(order);
        return std::make_pair(ring::encode(order), memory);
    });
}
