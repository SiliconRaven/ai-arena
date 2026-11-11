// The public baseline as a contestant program: the policy in include/baseline_policy.hpp, run through the step
// adapter in include/step_worker.hpp. This is baseline/bot.cpp in the contestant package.
#include "include/baseline_policy.hpp"
#include "include/step_worker.hpp"
int main(){return kb_step_worker::run_policy(step);}
