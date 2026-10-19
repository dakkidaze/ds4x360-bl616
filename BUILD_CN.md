# 编译教程（Linux / WSL2 / Windows）

本文面向**全新系统**：从零安装依赖、克隆仓库、编译出可烧录的 XInput 固件。
仓库已经 vendor 了打过补丁的 Bouffalo SDK（`third_party/bouffalo_sdk`），不需要另外下载 SDK。

本仓库只交付 **XInput（XUSB）persona**：默认构建是 XBOX 360 有线手柄 + UAC1 音频，
DS4 persona 仅作参考/回归。**Linux / WSL2 是受支持的构建路径**；原生 Windows 见第 2.2 节
的已知限制。

---

## 0. 你需要什么

| 项 | 说明 |
|---|---|
| 磁盘 | SDK + 工具链约 1.5 GB（工具链解压后约 700 MB，可删） |
| 网络 | 首次编译需要下载官方 RISC-V 工具链（约 95 MB）；之后可离线 |
| 硬件 | LCTech BL616（唯一在真实硬件上验收过的板型） |

系统依赖（Debian / Ubuntu / WSL2）：

```bash
sudo apt update
sudo apt install -y build-essential cmake python3 curl xz-utils git
```

- `build-essential`：gcc、`make`（需 GNU Make **>= 4.0**，Ubuntu 20.04+ 满足）
- `cmake`：**>= 3.15**
- `python3`：SDK 的镜像生成脚本
- `curl` + `xz-utils`：下载并解压工具链
- `git`：克隆仓库

Fedora 对应：

```bash
sudo dnf install -y gcc gcc-c++ make cmake python3 curl xz git
```

macOS 依赖（Homebrew）：

```bash
brew install cmake make xz
# curl 与 git 随 macOS 自带；xz 用于解压工具链
```

> **为什么不能只用发行版的 `gcc-riscv64-unknown-elf`？**
> SDK 用 `-march=rv32imafcpzpsfoperand_xtheade` 编译，`xtheade` / `zpsfoperand` 是 T-Head 扩展，
> 上游 GCC 不支持。必须用官方 Xuantie 工具链；`build.sh` 会自动下载。

---

## 1. Linux / WSL2（推荐）

### 1.1 克隆

```bash
git clone <仓库地址> ds4x360-bl616
cd ds4x360-bl616
```

### 1.2 首次编译（自动下载工具链）

```bash
./build.sh
```

第一次运行会：

1. 在 `third_party/toolchain/*/bin`、`~/riscv-toolchain/toolchain/bin` 等处找
   `riscv64-unknown-elf-gcc`（或 `riscv64-elf-gcc`）；
2. 都找不到、且 `third_party/toolchain/` 不存在时，自动执行一次 `./setup_toolchain.sh`，
   从官方 release 下载 `xuantie-gcc-riscv-v2.6.1-linux-x86_64.tar.xz`（约 95 MB，SHA-256 校验）
   到 `third_party/toolchain/`（已 gitignore，不会进仓库）；
3. 用系统 `cmake` + 该工具链编译。

产物在：

```
firmware/lctech616/ds4dongle-lctech616-xinput.bin          # 默认：XUSB 手柄 + UAC1 音频
firmware/lctech616/ds4dongle-lctech616-noaudio-xinput.bin  # XUSB 仅手柄（AUDIO=0）
firmware/lctech616/flash_prog_cfg.ini                      # 烧录配置（指向当前编译输出）
firmware/lctech616/boot2_*.bin, partition.bin              # 烧录包
```

### 1.3 构建命令

```bash
./build.sh                        # 默认：XInput + 音频复合变体（推荐）
XINPUT=1 AUDIO=0 ./build.sh       # 仅 XUSB（无 UAC1 音频），诊断用
BOARD_TYPE=lctech616 USB_SPEED=fs ./build.sh  # 显式板型/速率；fs 是唯一支持的 USB 速度
XINPUT=0 ./build.sh               # 参考：DS4 persona（本仓库不发布该形态）
```

- `USB_SPEED` 只支持 `fs`（Full-Speed 12 Mbps）；`hs` 会被拒绝。
- 板型默认 `BOARD_TYPE=lctech616`。`aim61` / `m0sdock` 仍可编译，但**未在真实硬件上验证**。
- `DEBUG_CONSOLE=1` 与 `MIC_DIAG=1` 改变的是**行为**，不只是文件名：
  `DEBUG_CONSOLE` 增加 UART 日志镜像 / HID 0xB4 日志环，`MIC_DIAG` 启用麦克风诊断行为
  （会改变音频节拍）。两者同时给产物加 `-dbg` / `-diag` 后缀，不会覆盖发布镜像。
  详见 `./build.sh` 的 usage 输出。

其他动作：

```bash
./release.sh          # 构建两个 XInput 变体到 release/（固件 + 烧录配置）
./build.sh clean      # 清理
./build.sh rebuild    # 清理后重编
./build.sh strip      # 只删中间文件，保留固件
```

### 1.4 手动 / 离线安装工具链

```bash
./setup_toolchain.sh          # 单独下载（已安装则直接返回）
./setup_toolchain.sh --force  # 强制重装
SKIP_TOOLCHAIN_SETUP=1 ./build.sh   # 禁止自动下载
```

离线时：从 <https://github.com/bouffalolab/toolchain/releases/tag/default> 下载对应包，
解压到 `third_party/toolchain/`，确保存在
`third_party/toolchain/<目录>/bin/riscv64-unknown-elf-gcc`。

### 1.5 发布包

```bash
./release.sh
```

产出 `release/`：

```
release/ds4x360-bl616-xinput.bin
release/ds4x360-bl616-xinput-noaudio.bin
release/boot2_bl616_isp_release_v8.1.8.bin
release/partition.bin
release/flash_prog_cfg-xinput.ini
release/flash_prog_cfg-xinput-noaudio.ini
release/README.txt
```

### 1.6 烧录

> 完整教程：**[FLASHING_CN.md](FLASHING_CN.md)** —— ISP 进入方式、ini 的作用、
> Dev Cube 与 BLFlashCommand 两条路径、WSL2/usbipd 串口透传。

---

## 2. Windows

### 2.1 方案 A：WSL2（推荐）

WSL2 里就是标准 Linux，按第 1 节做：

```powershell
wsl --install -d Ubuntu
```

然后在 Ubuntu 里安装依赖、克隆、`./build.sh`。

**烧录**：发布包和 Windows 路径见 **[FLASHING_CN.md](FLASHING_CN.md)**。
已验证的传输方式是板子自己的 USB 口进入 ROM ISP（`349b:6160`），用 `usbipd-win` 把这个设备
挂进 WSL（外接 USB-TTL 只是备选）：

```powershell
winget install usbipd
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

WSL 里出现 `/dev/ttyACM0` 后即可用 `BLFlashCommand` 烧录。
注意：`usbipd attach` 是整设备绑定，挂进 WSL 后 Windows 侧会暂时看不到它。

### 2.2 方案 B：原生 Windows（已知限制）

**原生 Windows 目前不是开箱即用的路径。** 本仓库 vendor 的 SDK 是保守裁剪版，
**不再包含** `tools/make`、`tools/cmake`、`tools/ninja`；而 SDK 的 `project.build`
在 Windows 分支里把 CMake 硬编码为 `$(BL_SDK_BASE)/tools/cmake/bin/cmake.exe`，
`build_windows.bat` 也把这些目录加进了 `PATH`。因此原生 Windows 需要：

- 系统里自备 **GNU Make** 与 **CMake**（在 `PATH` 中）；
- 给 make 传 `CMAKE=cmake`，覆盖 `project.build` 的硬编码路径；
- 或改用未裁剪的**完整 Bouffalo SDK**（`BL_SDK_BASE` 指过去）。

`build_windows.bat` 只把 `BL_SDK_BASE` 指到 `third_party\bouffalo_sdk`，没有自动下载工具链；
Windows 版 Xuantie 工具链需手动下载解压（<https://github.com/bouffalolab/toolchain/releases/tag/default>），
并把 `TOOLCHAIN_PATH` 指到包含 `bin` 的那一层。

结论：**优先用 WSL2 构建**，原生 Windows 仅在你能自行补齐 make/CMake 时使用。

---

## 3. 常见问题

| 现象 | 处理 |
|---|---|
| `ERROR: RISC-V toolchain not found` | 运行 `./setup_toolchain.sh`；或检查 `third_party/toolchain/*/bin/riscv64-unknown-elf-gcc` |
| 工具链下载失败/超时 | 重试；或按 1.4 手动下载并解压 |
| `cmake` 版本过低 | 升级到 >= 3.15（Ubuntu 20.04+ 即可） |
| `GNU Make >= 4.0 required` | macOS 用 `brew install make` 后跑 `gmake`；Ubuntu 20.04+ 自带满足 |
| `USB_SPEED=hs is not supported` | 只支持 `fs`；去掉该变量或用 `USB_SPEED=fs` |
| 切换变体/板型后编译异常 | `./build.sh clean` 或 `./build.sh rebuild`（脚本也会按构建键自动清理） |
| 烧录时找不到 `BLFlashCommand` | 使用 Dev Cube，或从完整 SDK 拷贝，见 [FLASHING_CN.md](FLASHING_CN.md) |
| 想用官方 SDK 复现 | 本仓库的 SDK 已打补丁，必须用 `third_party/bouffalo_sdk`；改用官方 SDK 会丢失 P0 修复（见 `SDK_CHANGES.md`） |

---

## 4. 仓库结构（构建相关）

```
ds4x360-bl616/
  src/                        应用源码
  tools/                      工具源码
  third_party/bouffalo_sdk/   vendor 的补丁 SDK（Apache-2.0）
  build.sh                    构建入口（委托给 build_macos.sh）
  build_macos.sh              Linux/macOS/WSL 构建实现
  release.sh                  生成 release/ 发布包
  setup_toolchain.sh          下载官方 Xuantie 工具链
  build_windows.bat           原生 Windows 构建（需手动准备工具链，见 2.2）
  SDK_CHANGES.md              SDK 修改说明
  misc/                       本地日志/旧产物（gitignore，克隆后不存在）
```
