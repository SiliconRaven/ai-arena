# 擂台乱斗 · 本地对战

带上自己的 C++ 程序，与比赛中选手的程序打一场擂台乱斗。每局 4–8 人：在逐渐缩小的方形擂台上移动、冲刺、挥锤、投掷炸弹，把对手撞下去。细则、观察和指令见 [RULES.md](RULES.md)，参数见 [game/rules.json](game/rules.json)。

内置 10 位选手，取自视频第一期第二轮正式比赛（EP1 R2）实际使用的交卷。每份程序的版本和完整 SHA-256 记录在 [选手名单](entrants/roster.json)。选手在擂台上使用各自的头像、小人和动作；你自己的程序默认显示为圆球。每个 HTML 回放都内嵌了该局用到的角色图，可单独分享、离线播放。源码采用 [MIT 许可证](LICENSE)。角色图片的已知来源见[美术说明](assets/CREDITS.md)。

**写自己的 bot，只需编辑 [my-bot/bot.cpp](my-bot/bot.cpp) 并保存。** 文件夹里已经放好了能直接运行的基准程序和头文件。按下面的步骤启动网页，就能用刚保存的程序对战。

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

打开 <http://127.0.0.1:8786>。网页的「你的程序文件夹」默认填好 `my-bot`。选 3–7 位对手，设置场数（1–100000）和起始种子，点「开始对战」。如果已有其他程序，也可以在这里填它的文件夹路径。每场的种子递增 1，结束后可查看累计得分、第一名次数、击杀和每局回放。可在启动命令末尾加 `--port 端口号` 更改端口。

程序会在本机直接编译运行，没有 Docker 的网络、内存或 CPU 隔离。只运行你信任的源码；本地服务只监听 127.0.0.1。对局耗时取决于程序及场数，第一次需要编译。

## 写自己的程序

在 [my-bot/bot.cpp](my-bot/bot.cpp) 中编写自己的策略。默认的 `main` 把基准策略 `step` 传给 `kb_step_worker::run_policy`；可以在同一文件里定义自己的策略函数，将它传给 `run_policy`。`baseline/` 保留了初始示例，方便对照。程序目录必须有 `bot.cpp`；每个顶层 `.cpp` 都参与编译，所需的头文件放在程序目录内。基准示例提供了 `include/step_worker.hpp` 和 `include/json.hpp`，`native/include/` 还包含可复制到程序目录的游戏引擎头文件。对局使用 `kb-step-json-1` 协议，每回合输出指令，详见 [RULES.md](RULES.md)。编译参数为 `-std=c++17 -O2 -fno-fast-math -ffp-contract=off`。

## 让自己的 AI 写 bot

把 [PROMPT.md](PROMPT.md) 的内容交给能读取本项目目录、编辑文件和运行命令的 AI 编程工具。提示词会引导它阅读规则，在 `my-bot/bot.cpp` 中实现策略，再和三种陪练试打。

- `striker`（突击手）主动接近、轻锤和冲刺。
- `warden`（守卫）偏向守内圈，近身蓄重锤反击。
- `roamer`（游击手）边移动边投弹，保持距离。

这些是 EP1 使用过的三种陪练。它们的源码在 `entrants/striker/`、`entrants/warden/` 和 `entrants/roamer/`，网页的对手列表也能选择。基准与陪练适合检查程序能否正常对局，并尝试不同打法。

用当前的 `my-bot` 和三种陪练打两局：

```powershell
py -3 -m arena.public_match --bot my-bot --opponents striker warden roamer --games 2 --seed 1
```

也可以在网页下载独立的本地选手包，或运行：

```powershell
py -3 -m arena.package --out output/player-package.zip
```

解压 ZIP 后进入 `ai-arena-player/`，打开 `PROMPT.md` 交给自己的 AI。包内带齐规则、引擎与 SDK、基准模板、初始化好的 `my-bot/`、三种陪练、编译器调用入口和离线回放。运行环境与上面的 Windows / Linux 安装要求相同。每次打包都用 `baseline/` 初始化包内的 `my-bot/`。完成后将自己写好的整个 `my-bot/` 文件夹带回完整仓库，即可挑战视频里的十位选手。使用方法见 [选手包说明](player-package/README.md)。

Linux / WSL 将上述命令开头的 `py -3` 换成 `python3`。

## 命令行

对手 ID 见 `entrants/roster.json`。用自己的程序对阵三位选手，运行两局：

```powershell
py -3 -m arena.public_match --bot my-bot --opponents astra terra opus --games 2 --seed 1
```

Linux / WSL 将上面命令开头的 `py -3` 换成 `python3`。

每批开始时编译一次，整批使用相同的程序；中途编辑源码不影响已开始的对战。每次运行在 `output/` 下创建新目录，保存逐局 `.html`、完整 `.json` 和 `summary.json`；HTML 可直接用浏览器离线打开。编译缓存也在 `output/`。汇总将每局原始得分相加，不是正式赛的跨人数归一化排名。结果与缓存可随时删除，不纳入 Git。

## 打包公开版

在仓库根目录运行以下命令，生成的 ZIP 只包含当前提交中的文件：

```sh
git archive --format=zip --prefix=ai-arena/ --output=../ai-arena-source.zip HEAD
```
