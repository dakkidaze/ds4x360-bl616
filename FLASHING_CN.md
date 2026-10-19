# 刷固件教程

本教程覆盖 **Release 发布包** 和 **本地编译产物**，说明 `.ini` 的作用、ISP 进入方式，
以及 Linux / WSL2 / Windows 下可用的烧录路径。

> dongle 是 BL616 板子。烧录走芯片的 **ROM ISP over USB**，与 dongle 正常工作时呈现的
> XUSB 复合设备是**两个不同的 USB 设备**。LCTech BL616 上已验证的路径是**板子自己的 USB 口**：
> 进入 ROM ISP 后会枚举为 USB CDC **`349b:6160`**（Windows 下 `COMx`，Linux/WSL2 下
> `/dev/ttyACM0`）。UART 排针外接 USB-TTL 只是**备选**传输方式，不是必需条件。

## 1. 发布包里都有什么？

`./release.sh` 产出：

| 文件 | 作用 |
|---|---|
| `ds4x360-bl616-xinput.bin` | XInput 变体（XUSB 手柄 + UAC1 音频）的应用固件。 |
| `ds4x360-bl616-xinput-noaudio.bin` | XInput 仅手柄变体（无音频）的应用固件。 |
| `boot2_bl616_isp_release_v8.1.8.bin` | 二级 bootloader，固定写到 `0x000000`。 |
| `partition.bin` | 分区表，固定写到 `0xE000`，里面定义了应用分区的位置。 |
| `flash_prog_cfg-xinput.ini` | `xinput` 变体的**烧录布局清单**。 |
| `flash_prog_cfg-xinput-noaudio.ini` | `xinput-noaudio` 变体的布局清单。 |
| `LICENSE`、`NOTICE`、`LICENSE-bouffalo_sdk` | 许可材料（GPL-3.0 应用、第三方声明、Apache-2.0 SDK）。 |
| `SOURCE.txt` | GPL-3.0 Corresponding Source 指引（仓库地址 + commit）。 |
| `SHA256SUMS` | 包内其余文件的校验和。 |
| `README.txt` | 包内简要说明与已验证的烧录命令。 |

刷之前先校验下载：

```bash
cd release
sha256sum -c SHA256SUMS
# macOS:
shasum -a 256 -c SHA256SUMS
```

这块 4 MB BL616 布局把 `FW` 分区放在 `0x10000`。ini 里写的是 `address = @partition`，
烧录工具会读 `partition.bin` 自动解析出这个地址。

ini 里还有：

- `erase = 1` —— 写入前擦除本次要编程的区块（不是整片擦除）；
- `skip_mode = 0x0, 0x0` —— 不跳过任何区域；
- `boot2_isp_mode = 0` —— 使用正常 boot2，不是 ISP boot2。

每个变体一个 ini，是因为应用固件在发布包里被重命名了。**如果你改了文件名，记得同步修改
ini 里对应的 `filedir = ./...`。**

## 2. 到底需不需要 ini？

| 烧录方式 | 是否用 ini？ | 说明 |
|---|---|---|
| `BLFlashCommand ... --config=...` | **需要——已验证路径** | 一次性给出 boot2/分区/应用地址。 |
| `./build.sh flash /dev/ttyACM0`（Linux） | **需要，但脚本自动处理** | 脚本调用 SDK 的 `make flash`，使用仓库根目录的 `flash_prog_cfg.ini`（指向 `build/build_out/`）。 |
| Bouffalo Dev Cube（图形界面） | **不导入本 ini** | 需要手动填地址（见第 4 节）。 |
| 直接 `--firmware` + `--addr` | 不用 | 理论上可以，但还要自己烧 boot2 和分区表。不推荐。 |

## 3. 准备：进入 ROM ISP

1. 拔下板子。
2. 按住 **BOOT**。
3. 把**板子自己的 USB 口**插到 PC（就是平时用的 Type-C；进入 ROM ISP 后芯片会重新枚举为
   `349b:6160`）。
4. 等串口出现后松开 **BOOT**。

找到串口：

```bash
# Linux / WSL2
lsusb | grep -i 349b          # 349b:6160 = BL616 ROM ISP
ls /dev/ttyACM* /dev/ttyUSB*
# macOS
ls /dev/tty.usbserial* /dev/tty.usbmodem*
```

如果无法使用板子的 USB 口，UART 排针外接 USB-TTL（3.3V）是**备选**方案；ISP 进入步骤相同，
设备名会是 `/dev/ttyUSB*`（或 `COMx`）。

Linux 权限：

```bash
sudo usermod -aG dialout "$USER"   # 重新登录生效
# 或临时：
sudo chmod 666 /dev/ttyACM0
```

### BLFlashCommand 没有 vendor

裁剪后的 SDK 删掉了 `tools/bflb_tools/bouffalo_flash_cube/`，所以仓库里**没有**
`BLFlashCommand`。请从完整 Bouffalo SDK 拷贝：

```bash
mkdir -p third_party/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube
cp -r <full-sdk>/tools/bflb_tools/bouffalo_flash_cube/* \
      third_party/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/
```

里面有 `BLFlashCommand-ubuntu`、`BLFlashCommand-macos`、`BLFlashCommand.exe` 等。

`./build.sh flash` 会先检查 `BLFlashCommand-ubuntu`；缺失时直接报错并提示
“use Dev Cube or copy BLFlashCommand from a full SDK”。它不会 clean、不会重新 configure，
也不会下载工具链。

## 4. 方式 A —— BLFlashCommand（已验证路径）

这是硬件验收时实际使用的流程（`docs/phase1-verification.md`）。

### 4.1 Release 发布包

```bash
cd release
/path/to/BLFlashCommand-ubuntu \
  --interface=uart --baudrate=2000000 \
  --port=/dev/ttyACM0 --chipname=bl616 \
  --config=flash_prog_cfg-xinput.ini --reset
```

换变体时把 `xinput` 换成 `xinput-noaudio`；macOS 用 `BLFlashCommand-macos`，Windows 用
`BLFlashCommand.exe --port=COMx`。请在发布包目录里执行，ini 的相对路径才能找到文件。

`--reset` 让工具在烧完后复位芯片，但**可能仍停留在 ISP 模式**。如果之后主机看不到
`1209:d1ce`，拔下板子、**不要按 BOOT** 重新插上即可启动应用。

### 4.2 本地编译

```bash
./build.sh
./build.sh flash /dev/ttyACM0
```

`build.sh flash` 调用 SDK 的 `make flash`，在仓库根目录使用 `flash_prog_cfg.ini`
（`filedir` 指向 `build/build_out/`）。工具缺失时见第 3 节。

## 5. 方式 B —— Bouffalo Dev Cube（备选）

Dev Cube 是图形工具，**不会导入 BLFlashCommand 的 `.ini`**。需要手动配置布局：

1. 按第 3 节让板子进入 ISP。
2. 打开 **Bouffalo Dev Cube**，芯片选 **BL616**，接口选 **UART**，串口选 `349b:6160`
   对应的 `COMx`。
3. 手动选择文件并填写地址：

   | 区域 | 文件 | 地址 |
   |---|---|---|
   | boot2 | `boot2_bl616_isp_release_v8.1.8.bin` | `0x000000` |
   | partition | `partition.bin` | `0xE000` |
   | application | `ds4x360-bl616-<variant>.bin` | `0x10000` |

4. 开始下载；如果烧完仍停在 ISP，断电重插。

Dev Cube 是可用的备选方案，但本项目验证的是 BLFlashCommand。

## 6. 方式 C —— WSL2（本仓库常用路径）

在 Windows 上开发、在 WSL2 里编译时，用 `usbipd-win` 把 ISP 设备透传给 WSL
（硬件验收就是这么做的：`usbipd attach --wsl --busid 6-15`）：

```powershell
# Windows PowerShell（管理员）
winget install usbipd
usbipd list                       # 找到 349b:6160 的 BUSID
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

WSL 里确认设备节点：

```bash
ls /dev/ttyACM0
```

然后按 4.1/4.2 烧录：

```bash
cd release
/path/to/BLFlashCommand-ubuntu \
  --interface=uart --baudrate=2000000 \
  --port=/dev/ttyACM0 --chipname=bl616 \
  --config=flash_prog_cfg-xinput.ini --reset
```

注意：`usbipd attach` 是整设备绑定，挂进 WSL 后 Windows 侧（包括 Dev Cube）会暂时看不到它；
用完 `usbipd detach --busid <BUSID>` 归还。

## 7. 方式 D —— 原生 Windows

优先用 WSL2（第 6 节）。原生 Windows 可以用 `BLFlashCommand.exe`（或 Dev Cube）烧录，但
**原生构建不是开箱即用的**——已知限制见 [BUILD_CN.md](BUILD_CN.md) 第 2.2 节。

本仓库的 SDK 未 vendor 烧录工具，`build_windows.bat flash COMx` 依赖的
`BLFlashCommand.exe` 也不存在。请用 Dev Cube，或把完整 SDK 的
`bouffalo_flash_cube` 拷到 `third_party\bouffalo_sdk\tools\bflb_tools\` 下再手动执行。

## 8. 该刷哪个变体？

| 变体 | 适用场景 |
|---|---|
| `xinput` | 默认。XBOX 360 有线手柄（XUSB `1209:D1CE`）+ UAC1 音频。 |
| `xinput-noaudio` | 只要 XUSB 手柄，不需要音频；用于排除音频相关问题。 |

DS4 persona 构建（`XINPUT=0 ./build.sh`）只作参考/回归，不随 `release.sh` 发布。

## 9. 常见问题

| 现象 | 检查 |
|---|---|
| 没有 `349b:6160` / 串口 | 板子没进 ISP、USB 线只能充电、或设备被 Windows 占用（先 usbipd detach）。 |
| Linux 报 `Permission denied` | 加入 `dialout` 组，或 `sudo chmod 666 /dev/ttyACM0`。 |
| 找不到 `BLFlashCommand` | 从完整 SDK 拷贝（第 3 节），或改用 Dev Cube；`./build.sh flash` 会直接报错提示。 |
| WSL 里没有 `/dev/ttyACM0` | `usbipd list` 确认已 bind/attach；attach 后重新插拔一次设备。 |
| 烧录成功但主机不识别为手柄 | `--reset` 可能把芯片留在 ISP；拔下后不按 BOOT 重新插上。`lsusb` 应看到 `1209:d1ce`。 |
| 音频设备不出现 | 烧的是 `xinput-noaudio`；换 `xinput` 变体。 |
| `SHA256SUMS` 校验失败 | 重新下载；不要烧写损坏的镜像。 |

烧录后主机应看到 `1209:d1ce`（XUSB）。Linux 下可以查：

```bash
lsusb | grep -i 1209
dmesg | tail -n 40
```

## 10. 相关文档

- 编译选项：[BUILD.md](BUILD.md) / [BUILD_CN.md](BUILD_CN.md)
- 变体说明：[README.md](README.md) / [README_CN.md](README_CN.md)
- 硬件验收记录：`docs/phase1-verification.md`
