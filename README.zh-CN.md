# E.T. 外星人 — 雅达利 2600 原生 C++ 移植

[English README](README.md)

本项目将雅达利 2600 游戏 **E.T. the Extra-Terrestrial**（Atari，1982）的
6502 汇编代码静态转换为 C++，并配套实现了一个精简的硬件模型，使游戏可以脱离
模拟器，直接在现代平台上原生运行。

需要说明的是：这**不是**从零开始的重制、重新设计或手工复刻，而是通过汇编到
C++ 的转换来保留原版游戏逻辑，从而实现原版游戏的跨平台运行。

## 项目内容

- **静态翻译的游戏逻辑**：原版 6502 汇编被逐条转换为 C++（`game/game.cpp`）。
- **硬件兼容层**：只实现游戏实际用到的硬件——TIA（视频、音频、碰撞检测）、
  RIOT（RAM、输入、间隔定时器）以及 F8 卡带切换（`src/hardware.cpp`）。
- **前端程序**：负责画面、声音和键盘输入（`src/main.cpp`）。

## 这不是什么

- **不是 6502 模拟器**：没有指令取指/译码循环，也没有在运行时解释 ROM；
  6502 指令就是 C++ 语句。
- **不是重新设计的游戏**：玩法、时序、数据表和细节行为均来自原版汇编。
- **不是通用雅达利 2600 模拟器**：不能运行其他 ROM。

## 功能

- 原版游戏逻辑与时序，原生 60 FPS 运行。
- 原生视频输出、键盘操作和 PulseAudio 声音。
- 可选 NTSC 调色板（RGB565）。
- 支持彩色/黑白模式以及左右难度开关。
- 构建自包含：不需要外部模拟器；除窗口和音频库外无运行时依赖。

## 编译与运行

已在 Ubuntu 20.04 上使用以下命令测试通过。

```sh
sudo apt update
sudo apt install make g++ libpulse-dev
```

```sh
make          # 生成 ./et
make run      # 编译并启动游戏
```

## 操作方式

| 雅达利 2600 | 键盘 |
| --- | --- |
| 摇杆方向 | 方向键或 `W` `A` `S` `D` |
| 开火键 | `Space`、`Z` 或 `Ctrl` |
| Select | `F1` 或 `Tab` |
| Reset | `F2` 或 `Enter` |
| 彩色/黑白开关 | `F3`（切换） |
| 左难度开关 | `F4`（切换 A/B） |
| 右难度开关 | `F5`（切换 A/B） |
| 退出 | `Esc` |

## 移植到其他平台

游戏核心与平台无关，只有 `src/main.cpp` 是 Linux 专用前端。移植时只需实现三件事：

1. **视频**：调用一次 `et::start_game()`，然后以 60 Hz 调用
   `et::wait_frame(et::Frame&)`。`Frame::pixels` 是 160 × 210 的 TIA 颜色索引，
   可用 `et::ntsc_palette[256]`（RGB565）转换后显示。
2. **音频**：播放 `Frame::audio`（`audio_samples` 个交错的立体声 `s16` 采样，
   采样率 31,400 Hz）。
3. **输入**：设置全局 `et::input` 的字段（`up`、`down`、`left`、`right`、
   `fire`、`reset`、`select`、`color`、`left_diff_a`、`right_diff_a`）。
   退出时调用 `et::stop_game()`。

这样即可构建 SDL2、Win32、macOS、Web 或主机平台前端，而无需改动翻译后的
游戏逻辑和硬件模型。

## 精确性与限制

- 这是功能性转换，不是周期精确的硬件仿真。TIA 的建模足以还原本作的显示内核
  和音频，但它不是通用的 TIA 实现。
- 支持 F8 卡带切换，不支持其他卡带方案。
- 仅实现 NTSC 制式和调色板。
- 游戏按 60 Hz 设计；帧输出由定时器节流，不依赖垂直同步。

## 致谢

- 原版游戏：**E.T. the Extra-Terrestrial**（Atari，1982），设计者
  Howard Scott Warshaw。
- 转换所用的 6502 反汇编来源：
  [Retro-Replay 的 E.T. 反汇编项目](https://retro-replay.com/romhacking/documents/e-t-the-extra-terrestrial-disassembly/#credits)。
  感谢其作者与贡献者对原版代码的整理和注释。

## 法律说明

- 本仓库中转换后的源代码与硬件模型采用 **GNU GPL v3.0** 许可（见 `LICENSE`）。
- **E.T. the Extra-Terrestrial** 及原版 ROM 的版权归 Atari 所有。内嵌 ROM 数据
  仅用于保存和研究原版游戏。本项目与 Atari 无关联，也未获得其认可。
- 本项目未创作任何原版游戏内容；所有游戏内容均来自原版卡带及社区反汇编。
