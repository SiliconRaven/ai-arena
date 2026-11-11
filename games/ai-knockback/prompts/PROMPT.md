# 编写擂台击退 bot

你是一名参加擂台击退乱斗的程序员。请在当前项目的 `games/ai-knockback/workspace/my-bot/` 中编写尽可能强的 C++ bot。它将操控一个小人，利用移动、普通攻击、蓄力冲刺、重锤、定时炸弹和墙体，把对手击落，尽量存活并获得更多击杀。每局擂台会分阶段缩小，同一份程序需要适配 `games/ai-knockback/rules.json` 规定的全部人数。

先读 `README.md` 和 `games/ai-knockback/workspace/RULES.md`、`games/ai-knockback/rules.json`，再查看 `games/ai-knockback/workspace/my-bot/bot.cpp` 和 `games/ai-knockback/workspace/my-bot/include/step_worker.hpp`。规则、每回合时间份额、记忆上限和计分参数均以这些文件为准。比赛表现看存活名次和击杀；公开版的批次结果汇总每局原始得分。

## 实现

- 交付文件夹为 `games/ai-knockback/workspace/my-bot/`，入口是 `games/ai-knockback/workspace/my-bot/bot.cpp`。需要的头文件和辅助源码一起放在此文件夹内；编译器会编译其中所有顶层 `.cpp`。
- 使用现成 SDK 的 `kb_step_worker::run_policy`，或者需要显式 memory 时使用 `kb_step_worker::run`。遵守 `kb-step-json-1` 输入输出协议，标准输出专供协议使用，调试内容写到标准错误。
- 观察包含全场玩家、炸弹、墙体和缩圈信息。移动、攻击和释放技能使用方向向量；各项技能的冷却、前后摇、蓄力和过蓄规则请逐项查看 `games/ai-knockback/workspace/RULES.md`。
- 程序常驻，全局状态能延续；超时后进程可能重启，SDK memory 使用上一次成功答复的快照。初始化也占用第一次决策的时间份额。
- 可以将 `games/ai-knockback/native/include/` 中需要的引擎头文件复制到 `games/ai-knockback/workspace/my-bot/include/`，做前向模拟。算法须在裁判给定的时间份额内完成。
- 按项目固定的 C++ 编译参数实现策略，修改范围集中在 `games/ai-knockback/workspace/my-bot/`。保留规则、裁判、陪练和 SDK 的原有接口。

## 试打与改进

项目提供三种风格的陪练，`striker` 主动近身冲撞，`warden` 守内圈并用重锤反击，`roamer` 游走投弹。阅读或对战都可以。先跑通两局，再根据具体失败原因改进。

Windows 在项目根目录运行：

```powershell
py -3 -m arena.public_match --bot games/ai-knockback/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
```

Linux / WSL 在项目根目录运行：

```bash
python3 -m arena.public_match --bot games/ai-knockback/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
```

命令会自动编译并输出比赛结果；回放和汇总保存在新建的 `output/<批次>/` 文件夹里。检查 `.json` 的 `result` 中的 `rank`、`kills`、`faults`、`program_errors` 和出局时刻，打开 `.html` 看自己是如何被击落的。需要增加样本时，可以改场数和起始种子，或在网页里选择其他对手。

先解决编译失败、协议故障和决策超时，再改进走位和战术。结合回放找具体原因，例如靠边失控、蓄力过满、撞上弹簧墙、误入爆炸范围或攻击落空。对陪练的成绩只作为调试依据，之后还可以挑战视频里的正式选手。

完成后保留可运行的 `games/ai-knockback/workspace/my-bot/`，简要说明采用的策略、实际运行的试打命令和结果、仍存在的问题。只报告实际执行过的测试。
