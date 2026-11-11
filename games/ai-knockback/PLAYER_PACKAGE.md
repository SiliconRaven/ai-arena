# 擂台击退 · 本地选手包

解压后进入 `ai-arena-player/`。自己的程序在 `games/ai-knockback/workspace/my-bot/bot.cpp`，已放好能直接运行的基准实现及头文件。

## 交给 AI

将 `games/ai-knockback/prompts/PROMPT.md` 的内容发送给能够读取这个目录、编辑文件和执行命令的 AI 编程工具。它可以阅读`games/ai-knockback/workspace/RULES.md`、查看 SDK 和引擎、修改 `games/ai-knockback/workspace/my-bot/`，再调用本地裁判试打。

## 环境与启动

Windows 需要 Python 3.10+ 和 MinGW-w64 g++（GCC 11 或更新版本），把编译器的 `bin` 加入 PATH。项目和编译缓存放在英文路径下。双击 `start.cmd` 启动网页，或在此目录的 PowerShell 中运行：

```powershell
.\start.cmd
```

Linux / WSL 需要 Python 3.10+ 和 g++（GCC 11 或更新版本），在此目录运行：

```bash
python3 server.py
```

打开 <http://127.0.0.1:8786>，默认使用 `games/ai-knockback/workspace/my-bot` 并选中三种陪练，可设置场数和种子。可以在启动命令后加 `--port 端口号`。

## 和陪练打两局

Windows：

```powershell
py -3 -m arena.public_match --bot games/ai-knockback/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
```

Linux / WSL：

```bash
python3 -m arena.public_match --bot games/ai-knockback/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
```

裁判会自动编译，在 `output/` 中保存逐局 JSON、可离线打开的 HTML 回放及 `summary.json`。每批开始时编译一次，编辑并保存程序后，在下一批对战中生效。

## 包含的文件

- `games/ai-knockback/workspace/my-bot/` 是你的工作文件夹。
- `games/ai-knockback/` 包含提示词、规则、基准示例、C++ 裁判和回放模板。
- `games/ai-knockback/native/include/` 包含 SDK 和可用于模拟的引擎头文件。
- `games/ai-knockback/workspace/practice/` 包含三种陪练。突击手 `striker` 主动近身，守卫 `warden` 防守内圈，游击手 `roamer` 游走投弹。

在控制台的「你的小人」里可以选择自己程序的回放形象。完成程序后，把整个 `games/ai-knockback/workspace/my-bot/` 文件夹放回完整的公开版项目，就可以挑战视频里的十位选手。

程序在本机以当前用户权限运行。仅运行你信任的代码。源码采用随包附带的 MIT 许可证。
