#define KILL_VALUE 140.0
#define TAG_VALUE 5.0
#define BUDGET_SAFE
#define STRONG_MODEL
#define TIME_DISCOUNT .94
#define BLINK_PENALTY 5.0
#define INVERSE_STATE
#define RECOVERY_V3
#define RECOVERY_BEAM
#define LATE_CENTER
#define DAMAGE_AVERSION 2.8
#define SEARCH_HORIZON 60
#define SEARCH_SHORT 21
#define SEARCH_KEEP 24
#include "include/step_worker.hpp"
#include "include/policy.hpp"
int main(){bot::Brain brain;return sm_step_worker::run([&](const mini_json::Json&obs,mini_json::Json&memory){return brain.step(obs,memory);});}
