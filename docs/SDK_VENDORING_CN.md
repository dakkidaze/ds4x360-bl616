# Bouffalo SDK 裁剪与 vendor 指南

本文说明 `third_party/bouffalo_sdk` 是怎么来的、怎么更新，以及怎么把同样的做法用到 XInput
（Xbox 360）固件仓库。

一句话：SDK 是**普通 tracked 目录**，不是 submodule；它是打过补丁、保守裁剪过的完整 SDK 副本。
克隆后不需要另外 checkout SDK 就能编译；工具链单独下载。

## 1. 目标和约束

- **克隆即可编译**：`git clone` + `./build.sh`，不需要额外下载 SDK。
- **保留补丁**：固件依赖本地 USB/BT 修复，见 [SDK_CHANGES.md](SDK_CHANGES.md)。
- **可复现**：SDK 的确切状态在 app 仓库历史里。
- **体积可接受**：app 仓库 pack 约 122.5 MiB；SDK 工作区约 410 MB。
- **许可证**：保留 SDK 的 Apache-2.0 `LICENSE` 和 app 的 `NOTICE`。

代价是仓库体积。当前是**保守裁剪**：只删除构建根本不读的内容（examples、docs、VCS 历史、
大部分 tools），components/drivers/bsp 全部保留。更深的裁剪可以做，但要单独、逐步、带构建
验证地进行。

## 2. 当前数字

| 指标 | 完整上游 SDK | vendored 副本 |
|---|---:|---:|
| 工作区 | 约 1.6 GB | 约 410 MB |
| `.git/` | 455 MB | 已删除 |
| `examples/` | 342 MB | 已删除 |
| `docs/` | 11 MB | 已删除 |
| `tools/` | 384 MB | 39 MB |
| `components/` | 约 318 MB | 约 320 MB |
| `drivers/` | 35 MB | 35 MB |
| `bsp/` | 15 MB | 15 MB |
| 文件数 | 磁盘 17,201（tracked 17,121） | 磁盘 11,782（tracked 11,639） |
| tracked 内容（git tree） | — | 约 376.6 MiB |
| app 仓库 pack | — | 约 122.5 MiB |

`du -sh` 看到的是工作区，不是分发体积。Git 会压缩 SDK，克隆下来是约 123 MiB（pack），
checkout 后才是约 410 MB。

## 3. 保留了什么

| 路径 | 大小 | 原因 |
|---|---:|---|
| `CMakeLists.txt`、`project.build`、`board.build`、`cmake/`、`Kconfig`、`VERSION` | 小 | SDK 构建系统；app 的 `Makefile` 会 include `project.build`。 |
| `bsp/` | 15 MB | 板级定义、链接脚本、boot2/分区配置。所有板型都保留。 |
| `drivers/` | 35 MB | HAL/SoC 驱动。所有 SoC 都保留。 |
| `components/` | 约 320 MB | BT 协议栈、CherryUSB、FreeRTOS、libc、crypto、分区工具等。整体保留。 |
| `tools/bflb_tools/bflb_fw_post_proc/` | 29 MB | 把 ELF 处理成可烧录 `.bin` 的必要工具。 |
| 小型开发工具（`genromfs`、`genlfs`、`lpfw`、`byai`、`footprint`、gdb 脚本） | 约 10 MB | 本 app 不需要；暂时保留，严格裁剪时可删。 |
| `.github/`、`.claude/`、`.cproject`、`.project`、`.clang-format`、`.gitignore` | 小 | 当前保留了部分开发元数据；可选删除。 |

主要体积来自 `components/` 里的预编译无线/crypto 库：整个 `components/wireless` 约 160 MB，
其中 Bluetooth 约 41 MB。

## 4. 删掉了什么

| 路径 | 大小 | 原因 |
|---|---:|---|
| `.git/` | 455 MB | app 仓库已有自己的历史；嵌套仓库会让体积翻倍、`git add` 也麻烦。 |
| `examples/` | 342 MB | 本 app 编译不需要。 |
| `docs/` | 11 MB | 上游文档；改成链接即可。 |
| `tools/bflb_tools/bouffalo_flash_cube/` | 250 MB | 烧录 GUI/CLI。**代价**：`./build.sh flash` 需要从完整 SDK 拷 `BLFlashCommand`，见 [FLASHING_CN.md](FLASHING_CN.md)。 |
| `tools/bflb_tools/bouffalo_mfg_tool/` | 37 MB | 产线工具，本 app 不用。 |
| `tools/cmake/`、`tools/make/`、`tools/ninja/` | 约 34 MB | 使用系统 `cmake` 和 GNU Make（>= 4.0）。 |
| `tools/openocd/`、`tools/cklink_firmware/` | 约 26 MB | 调试器工具；编译和 UART 烧录不需要。 |

加上 `.git`，总共删掉约 1.15 GB；vendored 工作区约 410 MB。

## 5. 可复现的裁剪流程

起点必须是**已打补丁**的完整 SDK（见第 6 节），不要拿任意上游 checkout 直接裁。

```bash
SRC=/path/to/patched/bouffalo_sdk
DST=/path/to/app/third_party/bouffalo_sdk

# 1. 拷贝已打补丁的树（目标只能是 SDK 目录）
mkdir -p "$DST"
rsync -a --delete "$SRC/" "$DST/"

# 2. 删除 app 构建不读的内容
rm -rf "$DST/.git" \
       "$DST/examples" \
       "$DST/docs" \
       "$DST/tools/bflb_tools/bouffalo_flash_cube" \
       "$DST/tools/bflb_tools/bouffalo_mfg_tool" \
       "$DST/tools/cmake" \
       "$DST/tools/make" \
       "$DST/tools/ninja" \
       "$DST/tools/openocd" \
       "$DST/tools/cklink_firmware"

# 3. 可选：更严格的裁剪（开发元数据，构建不需要）
# rm -rf "$DST/.github" "$DST/.claude" "$DST/.cproject" "$DST/.project" \
#        "$DST/.clang-format" "$DST/BouffaloSDK.png" \
#        "$DST/bouffalo_sdk.code-workspace" "$DST/CLAUDE.md"

# 4. 检查
du -sh "$DST"
find "$DST" -type f | wc -l
find "$DST" -type l            # 应为空；有断链就修
```

不要让 `rsync --delete` 作用到 app 仓库根目录，否则会把 app 删掉；只同步 SDK 目录。

拷贝完先按第 8 节验证，再提交。必须保留 `LICENSE`、`project.build`、`cmake/`、`Kconfig`、
`bsp/`、`drivers/`、`components/`、`tools/bflb_tools/bflb_fw_post_proc/`。

## 6. 补丁：哪些不是上游

当前 fork 基于上游 `42c20811`，本地 commit `ec8c6e9d`
（`feat(usb,bt): P0 audio session hardening, F0 guard rework, BT TX ownership`，
15 个文件，+2028/−91）。[SDK_CHANGES.md](SDK_CHANGES.md) 记录了每项修改、原版问题和
用原版会出现的现象。

保持可维护的做法：

1. 补丁保存在**完整 SDK 的 git 提交**里，不要只在 vendored 副本里手改。
2. 导出 patch series（`git format-patch 42c20811..ec8c6e9d`），或保留完整 SDK 提交。
   vendored 副本本身无法 rebase。
3. 更新上游时：rebase 本地提交、解冲突、重新裁剪，然后重新编译所有板型和变体。
4. 更新 `SDK_CHANGES.md` 的 base 和 local commit。
5. app 侧代码（USB 描述符、XUSB 映射、DS4 HID 描述符）留在 app 仓库，不要放进 SDK。
   只有共享栈本身的修改才进 SDK。
6. 建议加 `third_party/bouffalo_sdk/VENDORED_SDK_COMMIT`，写明 upstream/local 两个 hash。

P0 修复大部分是通用 USB 设备控制器/CherryUSB/BT host 加固。XInput 用的是同一套栈，
所以应优先复用同一份已打补丁的 SDK，除非能证明某个补丁只对 DS4 有意义。

## 7. 构建集成

- `build.sh` 导出 `BL_SDK_BASE="${SCRIPT_DIR}/third_party/bouffalo_sdk"`。
- `CMakeLists.txt` 用 `find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})`。
- `Makefile` include `$(BL_SDK_BASE)/project.build`。
- 工具链**不 vendor**：`setup_toolchain.sh` 下载官方 Xuantie 工具链到
  `third_party/toolchain/`（gitignore）。SDK 需要
  `-march=rv32imafcpzpsfoperand_xtheade`，发行版 GCC 编译不了。
- `.gitignore` 排除 `/build/`、`/firmware/`、`/misc/`、`/release/`、`/third_party/toolchain/`。

## 8. 验证清单

必须在**全新 clone**里跑，而不只是当前工作区：

```bash
git clone <app-repo> /tmp/app-check
cd /tmp/app-check
./build.sh                     # 默认板型/变体
AUDIO=0 ./build.sh             # 仅手柄
SPEAKER=1 ./build.sh           # speaker 路由（DS4 专用）
./release.sh                   # 发布包
git status --short             # 必须干净：不能把 build 产物带进 git
```

再检查：

```bash
git ls-files third_party/bouffalo_sdk | wc -l
git ls-tree -r -l HEAD third_party/bouffalo_sdk | awk '{sum+=$4} END {printf "%.1f MiB\n", sum/1048576}'
git check-ignore -v third_party/bouffalo_sdk/components/wireless/macsw/lib/libmacsw_bl616.a
```

最后一条必须**没有输出**（文件必须被 tracked）。全新 clone 能编译，才能证明没有漏掉被
ignore 的必要文件。

## 9. 坑

### 9.1 SDK 自带的 `.gitignore`

vendored SDK 里保留了它自己的 `.gitignore`。`git add third_party/bouffalo_sdk` 时 Git 会应用
它，于是磁盘上存在的一些文件**不会**被 tracked。本仓库有 143 个这种文件（例如
`bouffalo_sdk.code-workspace`、`.clangd`、部分第三方 build-info 头和 CMake 文件）。
本仓库的 BL616 构建已用当前 tracked vendor 验证，所以本配置不需要它们。
新 app 要以 fresh-clone 构建作为门槛。

新 app 要重新验证 fresh-clone 构建。如果发现某个必要文件被跳过，用 `git add -f` 单独加，
或调整/删除 SDK 的 `.gitignore`。不要盲目 `-f` 加几百个构建产物。

### 9.2 全局 `*.a` / `*.bin` 会把 SDK 挡掉

这是把 vendor 复制到新仓库时最大的坑。app 的 `.gitignore` 如果写：

```gitignore
*.a
*.bin
```

会把 SDK 里预编译的 Bluetooth/USB 库、boot2/分区镜像全部静默忽略。本地能编译，fresh clone
就挂。加 SDK 之前先查：

```bash
git check-ignore -v third_party/bouffalo_sdk/components/wireless/macsw/lib/libmacsw_bl616.a
git check-ignore -v third_party/bouffalo_sdk/bsp/board/bl616dk/config/boot2_bl616_isp_release_v8.1.8.bin
```

两条都必须**没有输出**。建议用路径限定的 ignore（`/build/**/*.a`、`/lib/*.a`），不要用全局
`*.a`；`*.bin` 同理。

### 9.3 其它

- **烧录工具被删了**：`bouffalo_flash_cube` 不在 vendor 里；用 Dev Cube，或从完整 SDK 拷
  `BLFlashCommand`。`build.sh flash` 找不到工具会直接报错。
- **工具链不进 git**：`third_party/toolchain/` 解压后约 700 MB，绝不提交。
- **绝对路径**：不要在别处 configure/build 过的树上裁；build cache 里是绝对路径，不属于 vendor。
- **symlink**：上游可能有符号链接；裁剪后 `find -type l` 检查断链。
- **许可证**：保留 SDK 的 Apache-2.0 `LICENSE`，保留 app 的 `NOTICE` 和 GPL-3.0 `LICENSE`。
- **Kconfig/CMake 引用**：删掉一个 component 可能让 configure 或链接失败，即使 app 没直接调用。
  每删一组都要重新构建。
- **保留 `bflb_fw_post_proc`**：没有它编译能过，但生成 `.bin` 会失败。

## 10. 可选的深度裁剪

当前 vendor 保留了所有 components/drivers/BSP。要更小，就小步、可验证地删。候选和风险：

| 候选 | 大约节省 | 风险 |
|---|---:|---|
| `bsp/board/*` 只留 `bl616dk` | ~4 MB | 板级/Kconfig/链接脚本引用；构建用 `BOARD=bl616dk`。 |
| `drivers/soc/*` 只留 `bl616` | ~20 MB | 芯片选择/CMake；构建用 `CHIP=bl616`。 |
| `components/wireless/wifi6`、`wl80211`、`thread`、`wifi4`、`bl_wpa_supplicant` | ~100 MB | BT 可能仍引用 `macsw`/`coex`/`lpfw_private`；看 link map。 |
| `components/wireless/macsw` | ~30 MB | BT controller/macsw 库可能依赖它。必须测。 |
| `components/graphics` | ~42 MB | 本 app 不用，但 Kconfig 可能引用。 |
| `components/crypto` | ~73 MB | BT SSP/AES 可能要用，别盲删。 |
| `components/net`、`multimedia`、`fs`、`ipc` | ~35 MB | 可能被 Kconfig/其它 component 引用。 |
| `tools/` 里除 `bflb_fw_post_proc` 外的小工具 | ~10 MB | 对本构建无影响。 |
| `.github`、`.claude`、IDE 文件 | <1 MB | 无。 |

深度裁剪有可能做到 150–250 MB，但预编译无线/crypto 库占大头，链接依赖不直观。
**建议不要把深度裁剪和协议开发混在一起**：先用当前 410 MB vendor 把 XInput 跑通，再一组一组
删，每删一组都做完整构建 + fresh-clone 验证。

## 11. 用到 XInput 仓库

XInput 仓库（`ds4x360-bl616`）目前用的是同级 `../bouffalo_sdk`。要改成 vendored 模式：

1. 拷贝已打补丁、保守裁剪的 SDK：

   ```bash
   mkdir -p third_party
   rsync -a /path/to/ds4dongle-bl618/third_party/bouffalo_sdk/ \
             third_party/bouffalo_sdk/
   ```

2. **先改 `.gitignore` 再 `git add`**。XInput 目前有全局 `*.a` / `*.bin`，必须先限定路径或删除，
   并加上工具链忽略：

   ```gitignore
   /build/
   /firmware/
   /release/
   /third_party/toolchain/

   # 不要在这里用全局 *.a / *.bin：会把 vendored SDK 的预编译库
   # 和 boot2/分区镜像一起忽略掉。
   ```

   然后验证：

   ```bash
   git check-ignore -v third_party/bouffalo_sdk/components/wireless/macsw/lib/libmacsw_bl616.a
   git check-ignore -v third_party/bouffalo_sdk/bsp/board/bl616dk/config/boot2_bl616_isp_release_v8.1.8.bin
   ```

3. 改 SDK 路径默认值：

   - `build_macos.sh`：`BL_SDK_BASE="${SCRIPT_DIR}/third_party/bouffalo_sdk"`
   - `Makefile`：`BL_SDK_BASE ?= $(abspath third_party/bouffalo_sdk)`
   - `build_windows.bat`：`set "BL_SDK_BASE=%~dp0third_party\bouffalo_sdk"`
   - `CMakeLists.txt` 已经用 `$ENV{BL_SDK_BASE}`，不用改。
   - 更新 `README.md` / `README_CN.md` 里 `../bouffalo_sdk` 的说明。

4. 加 `setup_toolchain.sh` 和 `/third_party/toolchain/` 忽略项。

5. 暂存并核对 vendor：

   ```bash
   git add third_party/bouffalo_sdk
   git ls-files third_party/bouffalo_sdk | wc -l          # 约 11639
   git ls-tree -r -l HEAD third_party/bouffalo_sdk | \
     awk '{sum+=$4} END {printf "%.1f MiB\n", sum/1048576}'   # 约 376.6
   ```

6. 编译两个 XInput 变体：

   ```bash
   XINPUT=1 ./build_macos.sh
   XINPUT=1 AUDIO=0 ./build_macos.sh
   ```

7. fresh-clone 验证：把 XInput 仓库 clone 到 `/tmp`，在那里构建，确认没有缺库/缺 boot2。

8. vendor 和协议改动分开提交。加一份 XInput 的 `SDK_CHANGES.md`（复制/改写本仓库的），
   记录 `42c20811` 和 `ec8c6e9d`，以及 XInput 特有的 SDK 补丁（如果有）。

9. `build/`、`firmware/`、`release/`、`third_party/toolchain/` 不要进 Git。

XUSB 描述符、MS OS 1.0 extended properties、XInput 映射都属于 app 仓库，不属于 SDK。
只有共享 USB/BT 栈本身需要改时，才动 SDK，并写进 XInput 的 `SDK_CHANGES.md`。

## 12. 最终检查

- [ ] SDK 来自**已打补丁**的 checkout，且记录 commit。
- [ ] 删除了 `.git`、`examples`、`docs` 和不需要的 tools。
- [ ] 保留 `tools/bflb_tools/bflb_fw_post_proc`。
- [ ] `LICENSE`、`NOTICE` 存在。
- [ ] 没有全局 `*.a` / `*.bin` 把 SDK 挡掉。
- [ ] `git ls-files third_party/bouffalo_sdk | wc -l` 符合预期。
- [ ] SDK 内没有 `.git`、构建产物、工具链。
- [ ] fresh clone 能编译所有支持板型/变体。
- [ ] 烧录路径已写清楚（Dev Cube 或拷 `BLFlashCommand`）。
- [ ] 更新了 `SDK_CHANGES.md` / `VENDORED_SDK_COMMIT`。
