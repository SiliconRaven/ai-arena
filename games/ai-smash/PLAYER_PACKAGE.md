# AI Smash · 本地选手包

解压后进入 `ai-arena-player/`。你的程序在 `games/ai-smash/workspace/my-bot/bot.cpp`，其中已经放好可直接参赛的基准实现。

## 交给 AI

用你自己的 AI 编程工具打开解压目录，把 `games/ai-smash/prompts/PROMPT.md` 交给它。AI 可以在本地阅读规则、编辑 `games/ai-smash/workspace/my-bot/`、运行下面的试打命令并查看结果。

## 启动

Windows 需要 Python 3.10+ 和加入 PATH 的 MinGW-w64 g++（GCC 11 或更新版本）。把项目放在英文路径下，双击 `start.cmd`。

Linux / WSL 需要 Python 3.10+ 和 g++（GCC 11 或更新版本）。在解压目录运行 `python3 server.py`。

打开 <http://127.0.0.1:8786>，默认是 `games/ai-smash/workspace/my-bot` 对阵三种陪练。可以在「你的小人」里选择自己程序的回放形象。也可以在终端运行两局：

```powershell
py -3 -m arena.public_match --game smash --bot games/ai-smash/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
```

Linux / WSL 将 `py -3` 换成 `python3`。对局汇总、JSON 和离线 HTML 回放保存在 `output/`。

## 程序与陪练

- `games/ai-smash/workspace/my-bot/` 是你的工作目录。编辑并保存后，下批对战会编译新版本。
- `games/ai-smash/` 包含规则、SDK、裁判、基准示例和回放。
- `games/ai-smash/workspace/practice/` 包含突击手 `striker`、守卫 `warden` 和游击手 `roamer`。

写好后可把整个 `games/ai-smash/workspace/my-bot/` 文件夹带回完整仓库，在 AI Smash 的「你的程序文件夹」中选择它，再和视频里的正式选手对打。

程序在本机以当前用户权限运行。仅运行你信任的代码。
