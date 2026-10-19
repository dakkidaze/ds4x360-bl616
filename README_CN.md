# ds4x360-bl616 —— DualShock 4 → Xbox 360（XUSB）适配器

[English](README.md) · [编译教程](BUILD_CN.md) · [刷固件教程](FLASHING_CN.md)

基于 BL616/BL618 的适配器：把 **DualShock 4（DS4）** 通过蓝牙经典（BR/EDR）HID 接入，
再向主机呈现为 **Xbox 360 有线手柄**（XUSB，VID:PID `1209:D1CE`），并带一个 **UAC1 音频复合**
功能。Windows 由 `usbccgp` 拆分为 `xusb22`（XInput）+ `usbaudio`，无需装驱动。

打补丁的 Bouffalo SDK 已 vendor 在 `third_party/bouffalo_sdk`，克隆后不需要另外下载 SDK。
裁剪/vendor 的做法见 [docs/SDK_VENDORING_CN.md](docs/SDK_VENDORING_CN.md)，每一处本地补丁见
`SDK_CHANGES.md`。

> **非官方项目。** 与索尼、微软均无关联，也未获其认可。"DualShock"、"Xbox" 是各自权利人的商标。
> `1209:D1CE` 是本地/实验用的 pid.codes 风格 VID/PID；正式分发前请申请正式分配。

## 变体 / 编译 / 烧录

本仓库交付的是 XInput persona。发布两个变体；DS4 persona 仅作**参考**（回归对照），不发布。

| 变体 | 编译 | 主机看到 |
|---|---|---|
| `xinput`（**默认**） | `./build.sh` | XUSB 手柄 `1209:D1CE` + UAC1 音频 |
| `xinput-noaudio` | `XINPUT=1 AUDIO=0 ./build.sh` | 仅 XUSB 手柄，无音频功能 |

- **编译**：[BUILD_CN.md](BUILD_CN.md) / [BUILD.md](BUILD.md) —— vendor SDK、
  `./setup_toolchain.sh` 自动下载 Xuantie 工具链、Linux/WSL2 命令、原生 Windows 受限说明。
- **烧录**：[FLASHING_CN.md](FLASHING_CN.md) / [FLASHING.md](FLASHING.md) —— ROM ISP
  `349b:6160`、`BLFlashCommand --config=flash_prog_cfg-<variant>.ini`、Dev Cube 备选。
- `./release.sh` 把两个变体构建到 `release/`（固件、烧录配置、许可文件、`SOURCE.txt`、
  `SHA256SUMS`）。
- DS4 参考构建：`XINPUT=0 ./build.sh` —— 可以编译，但不是产品目标。

## 支持的开发板

| 开发板 | 芯片 | 状态 |
|---|---|---|
| **LCTech BL616**（默认） | BL616 QFN32 | **已做硬件验收**（XUSB + UAC1 复合） |
| Ai-M61-32S-Kit | BL618 QFN56 | 可编译；硬件未测 |
| Sipeed M0S Dock | BL616 QFN32 | 可编译；硬件未测 |

只有 LCTech BL616 在真实硬件上验收过。其他板型能编译，但 USB 接线、LED 行为和音频时序
未经验证。

### USB 接线（仅 Ai-M61-32S-Kit）

板载 Type-C 是 UART 桥（调试/烧录用）。原生 USB 需要从排针飞线：

```
Pin 37 (USB_DM) → USB D-（白）
Pin 38 (USB_DP) → USB D+（绿）
GND             → USB GND（黑）
```

LCTech BL616 / M0S Dock 直接用板载 Type-C 做原生 USB，无需接线。

## 快速开始

### 1. 依赖

```bash
# Linux / WSL2（Ubuntu/Debian）
sudo apt install -y build-essential cmake python3 curl xz-utils git

# macOS
brew install cmake make xz   # GNU Make >= 4.0（用 gmake）；xz 用于解压工具链
```

需要 CMake >= 3.15、GNU Make >= 4.0。

### 2. RISC-V 工具链

SDK 需要 T-Head 扩展（`xtheade`、`zpsfoperand`），发行版或 Homebrew 的 `riscv64-elf-gcc`
无法编译。`build.sh` 会查找已有的 Xuantie 工具链，找不到就通过 `./setup_toolchain.sh`
下载一次（约 95 MB，SHA-256 校验）到 `third_party/toolchain/`（已 gitignore）。手动执行：

```bash
./setup_toolchain.sh          # 幂等
./setup_toolchain.sh --force  # 重装
```

也可以指定自己的工具链：`CROSS_COMPILE=/path/to/riscv64-unknown-elf- ./build.sh`。

### 3. 编译

```bash
./build.sh                    # 默认：XUSB + UAC1 音频复合
XINPUT=1 AUDIO=0 ./build.sh   # 仅 XUSB（诊断）
XINPUT=0 ./build.sh           # 参考：DS4 persona（不发布）

./build.sh rebuild            # 清理后重编
./build.sh clean              # 只清理
./build.sh strip              # 删中间文件，保留固件
```

- 默认板型 `BOARD_TYPE=lctech616`；可改为 `aim61` / `m0sdock`。
- **USB 速度：只支持 Full-Speed（12 Mbps）**，`USB_SPEED=hs` 会被拒绝。
- 产物：`firmware/lctech616/ds4dongle-lctech616-xinput.bin`（复合）和
  `firmware/lctech616/ds4dongle-lctech616-noaudio-xinput.bin`，以及 `boot2_*.bin`、
  `partition.bin`、`flash_prog_cfg.ini`。构建产物不进 git。
- `DEBUG_CONSOLE=1` / `MIC_DIAG=1` 改变的是**行为**：`DEBUG_CONSOLE` 增加 UART 日志镜像 /
  HID 0xB4 日志环，`MIC_DIAG` 启用麦克风诊断（并改变音频节拍）。它们同时生成 `-dbg` /
  `-diag` 变体，不会覆盖发布文件名。`DEBUG_CONSOLE` 仅在非空且不等于 `0` 时生效。

### 4. 原生 Windows

原生 Windows **不是开箱即用**：裁剪后的 SDK 不再包含 `tools/make`、`tools/cmake`、
`tools/ninja`，而 `project.build` 在 Windows 分支硬编码 `tools/cmake/bin/cmake.exe`。
请用 **WSL2**（推荐），或自备系统 make/CMake 并传 `CMAKE=cmake`，或把 `BL_SDK_BASE`
指向完整 SDK。详见 [BUILD_CN.md](BUILD_CN.md) §2.2。

### 5. 烧录

简版（已验证路径）：拔下板子 → 按住 **BOOT** → 插入板子的 USB 口 → 等 `349b:6160` 串口出现 →
用对应变体的 ini 运行 `BLFlashCommand`。完整说明（含 WSL2/usbipd 与 Dev Cube 备选）见
[FLASHING_CN.md](FLASHING_CN.md) / [FLASHING.md](FLASHING.md)。

## 使用

1. 把板子的 Type-C 接到主机（供电 + 数据）。
2. DS4 进入配对模式：按住 **PS + Share/Create**，直到灯条闪烁。
3. 主机出现 "Xbox 360 Controller for Windows" 以及一个音频设备（`1209:D1CE` 复合设备）。
4. 板载 LED 显示连接状态（见下表）。

### LED 状态（LCTech BL616，GPIO27 单蓝灯）

| 状态 | 表现 |
|---|---|
| 空闲 / 等待配对 | 慢闪（约 1 Hz） |
| 扫描中 | 快闪（约 3 Hz） |
| 已连接 | 常亮 |
| 刚断开 | 闪约 3 秒（约 1 Hz），随后回到空闲 |
| 电量 ≤20%（放电中） | 中速闪（约 1.7 Hz） |
| 电量 ≤10%（放电中） | 快闪（约 3 Hz） |
| 自动熄灭 | 1 分钟后熄灭 |
| 事件确认 | 单闪 |
| 清除配对 | 三连闪 |

### BOOT 按键手势

| 手势 | 动作 |
|---|---|
| 单击 | 切换到下一个已记忆的手柄（最多 8 个） |
| 双击 | 断开当前手柄并扫描新柄（保留配对） |
| 长按 3 秒 | 清除全部配对并开始扫描（三连闪） |

## 功能

### 已实现（XInput persona）

- 蓝牙经典 HID Host：inquiry、SDP、L2CAP、SSP 配对、重连看门狗。
- DS4 输入 → XUSB 20 字节报告：按键、十字键、摇杆（径向死区、Y 轴取反）、模拟扳机、
  Guide（= PS 键）。
- XUSB 输出 → DS4：震动（强/弱）和 LED 命令经 BT `0x05` 下发。
- 触摸板修饰键连发：按住触摸板 + 目标键（A/B/X/Y/LB/RB）≥400 ms → 15 Hz 脉冲；
  短按原样透传。
- 断连发中立报告（不会卡键）；4 ms 节拍（主机实测约 126 Hz）。
- 复合构建提供双向 UAC1 音频：32 kHz 立体声扬声器输出、16 kHz 麦克风输入。
- 多手柄记忆（最多 8 个配对）与 BOOT 手势切换。
- 编译期选择板型（LCTech BL616 / Ai-M61-32S-Kit / M0S Dock）。

### 已知限制

| 项 | 说明 |
|---|---|
| 单活动手柄 | 同一时间只连接一个 DS4；最多记忆 8 个配对。 |
| XUSB 报告集 | XUSB 报告不含触摸坐标、陀螺仪、电量；触摸板只作连发修饰键。 |
| 音频 | 只有 UAC1 复合；无 HD 触觉、无自适应扳机音频通路。 |
| `1209:D1CE` | 开发用 VID/PID；正式分发前请申请正式分配。 |

### DS4 persona（仅参考）

本仓库也能编译原始 DS4 HID persona（`XINPUT=0`），用于回归对比。它**不是产品**：
`release.sh` 不构建它，本仓库也不对它做硬件验收；保留它只是为了验证共享 USB/BT 改动。
上游的 DS4 HID 报告布局、Feature report 以及 DualSense 时代的材料都不适用于 XInput 构建。

## 技术说明

### XUSB 输入报告（20 字节）

| 偏移 | 内容 |
|---|---|
| 0 | `0x00` |
| 1 | `0x14`（报告长度） |
| 2–3 | 按钮字，`uint16` LE（`& 0xF7FF`） |
| 4 | LT（0–255） |
| 5 | RT（0–255） |
| 6–7 | LX `int16` LE |
| 8–9 | LY `int16` LE（向上为正） |
| 10–11 | RX `int16` LE |
| 12–13 | RY `int16` LE |
| 14–19 | 保留（`0`） |

按钮位：十字键 `0x0001–0x0008`、Start `0x0010`、Back `0x0020`、L3/R3 `0x0040/0x0080`、
LB/RB `0x0100/0x0200`、Guide `0x0400`、A/B/X/Y `0x1000/0x2000/0x4000/0x8000`。

### 音频路径

复合构建中 interface 0 是 XUSB，interface 1–3 是 UAC1（audio control、speaker AS OUT、
mic AS IN）。扬声器 32 kHz 立体声，麦克风 16 kHz 单声道。mic 端点是 `0x84`
（FIFO index 4 → `FIBC3`，`0x1BC`）；扬声器是 ISO OUT `0x01`（FIFO0）。
端点映射见 `docs/phase1-audio-spec.md`。

### 为什么 mic FIFO 诊断按端点推导

`ds4_usb_audio.c` 的 mic FIFO 占用由端点推导：`0x1B0 + 4 * ((ep & 0x7F) - 1)`
→ DS4 `0x1B4`、XInput `0x1BC`。如果硬编码 `FIBC1`，在复合构建里读到的是 XUSB IN 的 FIFO。

## 仓库结构

```
src/                        应用（BT host、USB、XInput 映射、音频）
tools/                      host 侧工具（hidlog、描述符自检、单测）
docs/                       设计与验收记录
third_party/bouffalo_sdk/   vendor 的补丁 SDK（Apache-2.0）
build.sh                    构建入口
build_macos.sh              Linux/macOS/WSL 构建实现
release.sh                  生成 release/
setup_toolchain.sh          下载 Xuantie 工具链
SDK_CHANGES.md              SDK 补丁说明
misc/                       本地笔记/日志（gitignore）
```

## 文档

| 文档 | 内容 |
|---|---|
| [BUILD_CN.md](BUILD_CN.md) / [BUILD.md](BUILD.md) | 工具链、编译命令、Windows 限制 |
| [FLASHING_CN.md](FLASHING_CN.md) / [FLASHING.md](FLASHING.md) | ISP 进入、ini 布局、BLFlashCommand、Dev Cube |
| [docs/SDK_VENDORING_CN.md](docs/SDK_VENDORING_CN.md) | SDK 如何裁剪与 vendor |
| `SDK_CHANGES.md` | 每一处本地 SDK 补丁（base `42c20811` → local `ec8c6e9d`） |
| `docs/phase1-verification.md` | 复合构建的硬件验收记录 |
| `docs/phase2-gamepad-spec.md` | DS4 → XUSB 映射与连发语义 |

## 致谢

- [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) —— 本项目的 BT/USB 骨架来源（MIT）。
- [bouffalolab/bouffalo_sdk](https://github.com/bouffalolab/bouffalo_sdk) —— BL616 SDK，
  已 vendor 并打补丁。
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) —— USB device 栈。
- `dakkidaze/ds4dongle-bl618` —— DS4 persona 的参考项目。

## 许可

GPL-3.0，见 [LICENSE](LICENSE)。第三方声明见 [NOTICE](NOTICE)；vendor SDK 为 Apache-2.0
（`third_party/bouffalo_sdk/LICENSE`）。
