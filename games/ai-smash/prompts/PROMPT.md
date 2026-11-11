# 编写 AI Smash 平台混战 bot

请为 AI Smash 写一个 C++17 bot，并在本地和三种陪练试打。每局四人，每人三条命。伤害越高越容易被击飞出界。策略需要兼顾存活、回场和击杀。

先读 `games/ai-smash/workspace/RULES.md` 与 `games/ai-smash/rules.json`，再看起始程序和 `include/step_worker.hpp`。编辑 `games/ai-smash/workspace/my-bot/bot.cpp`。把辅助源码和头文件放在同一个程序目录里，目录顶层的所有 `.cpp` 都会参与编译。

## 实现

- 沿用 `sm_step_worker::run_policy`。需要跨重启保留状态时，使用 `sm_step_worker::run` 的 JSON `memory`。标准输出留给 `sm-step-json-1` 协议，调试输出写到标准错误。
- 从观察的 `rules` 读取招式参数。观察包括全场玩家、技能条、招式阶段和飞行道具。移动用标量 `move`，招式方向用向量 `dir`。
- 考虑台边、缩圈、二段跳和上勾拳回场。进攻时结合对手距离、无敌时间和招式阶段，选择追击、抓投或防御。
- 如需做前向模拟，可以复制 `games/ai-smash/native/include/` 中所需的引擎头文件。把每回合的决策控制在规则规定的时间内。

## 试打

`striker` 近身攻击并在对手防御时抓投，`warden` 守中路并用防御和蓄力应对，`roamer` 拉开距离打光波。

在完整仓库或解压后的选手包根目录运行以下命令。

```powershell
py -3 -m arena.public_match --game smash --bot games/ai-smash/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
```

Linux / WSL 将 `py -3` 换成 `python3`。在 `output/<批次>/` 查看 HTML 回放和 JSON 结果，检查名次、击杀、出局原因和 `program_errors`。先修复编译与协议问题，再根据实际对局改进策略。完成后保留能运行的程序，简要说明策略、试打命令和实测结果。
