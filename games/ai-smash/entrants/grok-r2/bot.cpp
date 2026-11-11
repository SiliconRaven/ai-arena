#include "include/ai.hpp"
#include "include/step_worker.hpp"

int main() {
    return sm_step_worker::run([](const mini_json::Json& obs, mini_json::Json& memory) {
        return std::make_pair(ai::decide(obs, memory), memory);
    });
}
