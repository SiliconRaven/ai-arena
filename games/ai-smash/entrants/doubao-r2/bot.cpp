#include "include/policy.hpp"
#include "include/step_worker.hpp"
using J = mini_json::Json;

int main() {
  return sm_step_worker::run([](const J &obs, J &memory) {
    J order = mybot::decide(obs);
    if (memory.is_null() || !memory.is_object()) memory = J::object{};
    memory["v"] = 1;
    return std::make_pair(order, memory);
  });
}
