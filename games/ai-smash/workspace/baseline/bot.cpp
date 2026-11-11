#include "include/baseline_policy.hpp"
#include "include/step_worker.hpp"

int main() { return sm_step_worker::run_policy(smash_baseline::step); }
