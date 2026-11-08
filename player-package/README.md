# 擂台击退 · 本地选手包

解压后进入 `ai-arena-player/`。自己的程序在 `my-bot/bot.cpp`，已放好能直接运行的基准实现及头文件。

## 交给 AI

将 `PROMPT.md` 的内容发送给能够读取这个目录、编辑文件和执行命令的 AI 编程工具。它可以阅读 `RULES.md`、查看 SDK 和引擎、修改 `my-bot/`，再调用本地裁判试打。

## 环境与启动

Windows 需要 Python 3.10+ 和 MinGW-w64 g++（GCC 11 或更新版本），把编译器的 `bin` 加入 PATH。项目和编译缓存放在英文路径下。双击 `start.cmd` 启动网页，或在此目录的 PowerShell 中运行：

```powershell
.\start.cmd
```

Linux / WSL 需要 Python 3.10+ 和 g++（GCC 11 或更新版本），在此目录运行：

```bash
python3 server.py
```

打开 <http://127.0.0.1:8786>，默认使用 `my-bot` 并选中三种陪练，可设置场数和种子。可以在启动命令后加 `--port 端口号`。

## 和陪练打两局

Windows：

```powershell
py -3 -m arena.public_match --bot my-bot --opponents striker warden roamer --games 2 --seed 1
```

Linux / WSL：

```bash
python3 -m arena.public_match --bot my-bot --opponents striker warden roamer --games 2 --seed 1
```

裁判会自动编译，在 `output/` 中保存逐局 JSON、可离线打开的 HTML 回放及 `summary.json`。每批开始时编译一次，编辑并保存程序后，在下一批对战中生效。

## 包含的文件

- `PROMPT.md` 引导 AI 编程和测试。
- `RULES.md` 与 `game/rules.json` 给出规则和接口。
- `baseline/` 是基准示例，`my-bot/` 是你的工作文件夹。
- `native/include/` 包含 SDK 和可用于模拟的 C++ 引擎头文件。
- `entrants/striker/` 是主动近身的突击手，`entrants/warden/` 是守内圈的守卫，`entrants/roamer/` 是游走投弹的游击手。
- `arena/`、`native/`、`game/` 和网页入口负责编译、对局、汇总和回放。

这里的所有角色都用圆球表示。完成程序后，把整个 `my-bot/` 文件夹放回完整的公开版项目，就可以挑战视频里的十位选手。

程序在本机以当前用户权限运行。仅运行你信任的代码。源码采用随包附带的 MIT 许可证。
