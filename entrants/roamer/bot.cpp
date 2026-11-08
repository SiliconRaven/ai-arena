#include "practice/policies.hpp"
#include "include/step_worker.hpp"

int main() {
    return kb_step_worker::run_policy([](const practice::J& observation) {
        return practice::Policy(observation, "roamer").step();
    });
}
