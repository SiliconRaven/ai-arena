# AI Arena · 本地对战

在同一个控制台选择游戏和 C++ 程序，本机运行对局并观看回放。

- **擂台乱斗**支持 4–8 人。在逐渐缩小的方形擂台上冲刺、挥锤、投弹，把对手撞下去。内置 10 份 EP1 R2 正式赛交卷和三种陪练。[规则](games/ai-knockback/workspace/RULES.md) · [选手名单](games/ai-knockback/entrants/roster.json) · [美术说明](games/ai-knockback/assets/CREDITS.md)
- **AI Smash · 平台混战**每局 4 人。跳上平台，用攻击、防御、抓投和钩索把对手击飞出界。内置 11 份 EP2 R2 正式赛交卷和三种陪练，使用该轮冻结的 sm-1.3 规则。[规则](games/ai-smash/workspace/RULES.md) · [选手名单](games/ai-smash/entrants/roster.json) · [美术说明](games/ai-smash/assets/CREDITS.md)

我的B站空间：https://space.bilibili.com/3691002512541717?


HTML 回放内嵌该局所需的素材，可单独分享、离线播放。源码采用 [MIT 许可证](LICENSE)。

两款游戏分别位于 `games/ai-knockback/` 和 `games/ai-smash/`。每个游戏的 `workspace/` 存放规则说明、基准、自己的程序和陪练，`prompts/` 存放 AI 提示词。两款游戏共用 `arena/` 的编译与对战代码。

## 开始

### Windows x64

准备 Python 3.10+ 和 MinGW-w64 的 g++（GCC 11 或更新版本）。本版用 [w64devkit 2.11.0](https://github.com/skeeto/w64devkit/releases/tag/v2.11.0) 随附的 GCC 16.2 验证。将工具链解压到英文路径，例如 `C:\w64devkit`；项目也放在英文路径下，例如 `C:\arena`。你自己的程序文件夹可以叫“我的程序”，其中的源码和头文件请使用 ASCII 文件名。

在项目文件夹打开 PowerShell，按实际解压位置设置编译器路径并启动：

```powershell
$env:Path = "C:\w64devkit\bin;$env:Path"
.\start.cmd
```

如果 g++ 已在 PATH 中，可以直接双击 `start.cmd`。保留启动窗口以查看运行状态。

### Linux / WSL

准备 Python 3.10+ 和支持 C++17 的 g++（GCC 11 或更新版本）。在项目根目录运行：

```bash
python3 server.py
```

### 开始对战

打开 <http://127.0.0.1:8786>，选择游戏和参赛选手，设置场数（1–100000）和起始种子，点「开始对战」。AI Smash 默认选好四位内置选手。点「用自己的程序和陪练打」会加入当前游戏的 `workspace/my-bot/` 和三位陪练，也可以填写其他程序目录。

加入自己的程序后，可以在「你的小人」里选择回放形象。

每场的种子递增 1，结束后可查看累计得分、第一名次数、击杀和每局回放。可在启动命令末尾加 `--port 端口号` 更改端口。

程序会在本机直接编译运行，没有 Docker 的网络、内存或 CPU 隔离。只运行你信任的源码；本地服务只监听 127.0.0.1。对局耗时取决于程序及场数，第一次需要编译。

## 写自己的程序

AI Smash 的程序使用 `sm-step-json-1` 协议。直接编辑 [AI Smash 的 bot.cpp](games/ai-smash/workspace/my-bot/bot.cpp)，或把 [基准目录](games/ai-smash/workspace/baseline/) 复制到自己的文件夹。观察与指令见 [AI Smash 规则](games/ai-smash/workspace/RULES.md)。

### 擂台乱斗

在 [擂台乱斗的 bot.cpp](games/ai-knockback/workspace/my-bot/bot.cpp) 中编写自己的策略。默认的 `main` 把基准策略 `step` 传给 `kb_step_worker::run_policy`；可以在同一文件里定义自己的策略函数，将它传给 `run_policy`。[基准目录](games/ai-knockback/workspace/baseline/) 保留了初始示例，方便对照。程序目录必须有 `bot.cpp`；每个顶层 `.cpp` 都参与编译，所需的头文件放在程序目录内。基准示例提供了 `include/step_worker.hpp` 和 `include/json.hpp`，[裁判头文件](games/ai-knockback/native/include/) 中还有可复制到程序目录的游戏引擎。对局使用 `kb-step-json-1` 协议，每回合输出指令，详见 [擂台乱斗规则](games/ai-knockback/workspace/RULES.md)。编译参数为 `-std=c++17 -O2 -fno-fast-math -ffp-contract=off`。

## 让自己的 AI 写 bot

在控制台选择游戏，点「下载选手包」，解压并用自己的 AI 编程工具打开 `ai-arena-player/`。把对应游戏 `prompts/PROMPT.md` 的内容交给它，AI 就可以编辑该游戏 `workspace/my-bot/bot.cpp`、编译并和三个陪练试打。双击 `start.cmd` 也可以在网页试打。已经克隆完整仓库的用户可以直接点「查看 AI 提示词」，在相同的目录工作。

- **擂台乱斗**的陪练会近身冲撞、守内圈反击或游走投弹。[提示词](games/ai-knockback/prompts/PROMPT.md) · [选手包说明](games/ai-knockback/PLAYER_PACKAGE.md)
- **AI Smash**的陪练会近身抓投、守中路防御反打或拉开距离打光波。[提示词](games/ai-smash/prompts/PROMPT.md) · [选手包说明](games/ai-smash/PLAYER_PACKAGE.md)

在完整仓库中用自己的程序和三种陪练打两局：

```powershell
py -3 -m arena.public_match --game knockback --bot games/ai-knockback/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
py -3 -m arena.public_match --game smash --bot games/ai-smash/workspace/my-bot --opponents striker warden roamer --games 2 --seed 1
```

也可以从命令行打包：

```powershell
py -3 -m arena.package --out output/player-package.zip
py -3 -m arena.package --game smash --out output/smash-player-package.zip
```

每个包用对应游戏的基准程序初始化 `workspace/my-bot/`。写好后可以把整个程序目录带回完整仓库，挑战视频里的正式选手。

Linux / WSL 将上述命令开头的 `py -3` 换成 `python3`。

## 命令行

省略 `--game` 时运行擂台乱斗。用自己的程序对阵三位选手，运行两局：

```powershell
py -3 -m arena.public_match --bot games/ai-knockback/workspace/my-bot --opponents astra terra opus --games 2 --seed 1
```

用四份正式赛交卷跑一局 AI Smash：

```powershell
py -3 -m arena.public_match --game smash --opponents argon-r2 astra-r2 claude-fable-r2 doubao-r2 --games 1 --seed 1
```

加 `--bot 文件夹` 时，AI Smash 选三位内置选手。两款游戏的 ID 分别见各自的选手名单。

Linux / WSL 将上面命令开头的 `py -3` 换成 `python3`。

每批开始时编译一次，整批使用相同的程序。每次运行在 `output/` 下创建新目录，保存逐局 `.html`、完整 `.json` 和 `summary.json`。编译缓存也在 `output/`。汇总将每局原始得分相加。结果与缓存可随时删除，不纳入 Git。

## 打包公开版

在仓库根目录运行以下命令，生成的 ZIP 只包含当前提交中的文件：

```sh
git archive --format=zip --prefix=ai-arena/ --output=../ai-arena-source.zip HEAD
```
