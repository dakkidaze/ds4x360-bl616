#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ── 板型选择 ──
#   lctech616  — LCTech BL616, 默认
#   aim61      — Ai-M61-32S-Kit (BL618)
#   m0sdock    — Sipeed M0S Dock (BL616)
BOARD_TYPE="${BOARD_TYPE:-lctech616}"

# ── USB 速度 ──
#   fs  — Full-Speed 12Mbps（唯一支持的模式）
#   R09: Full-Speed only. USB_SPEED=hs is rejected; HS is not supported.
USB_SPEED="${USB_SPEED:-fs}"
if [[ "$USB_SPEED" != "fs" ]]; then
    echo "ERROR: USB_SPEED=${USB_SPEED} is not supported - Full-Speed only (12 Mbps); HS is not supported." >&2
    exit 1
fi

# ── 默认 persona ──
# 本仓库交付 XInput（XUSB）persona：不设置 XINPUT 时默认构建 XUSB + UAC1 音频。
# DS4 persona 仅作参考/回归，用 XINPUT=0 构建。必须 export：CMakeLists.txt 读 $ENV{XINPUT}。
export XINPUT="${XINPUT:-1}"

# ── 功能开关（环境变量，正向可选） ──
#   音频       — 默认启用（默认构建即发布候选，真实有线 DS4 带音频）。
#                AUDIO=0 仅用于诊断排障时关闭 UAC1 音频。
#   GAMEPAD=0  — 去掉手柄 HID 功能（默认包含，无需声明）
#   TOUCHPAD=1 — 预留：触摸板→鼠标（暂无功能）
#   GYRO=1     — 预留：陀螺仪映射（暂无功能）
#   DEBUG_CONSOLE=1 / MIC_DIAG=1 — 开发变体，产物加 -dbg / -diag 后缀，
#                不会覆盖发布候选的 ds4dongle-<board>.bin
FEATURE_TAG=""
FEATURE_DESC="audio"
# DEBUG_CONSOLE predicate must be identical to CMakeLists.txt: non-empty AND
# not "0". `DEBUG_CONSOLE= ./build_macos.sh` must build the release variant,
# not enable debug in CMake while this script names the file as release.
DEBUG_CONSOLE_ON=0
if [[ "${AUDIO:-}" == "0" ]]; then
    FEATURE_TAG="${FEATURE_TAG}-noaudio"
    FEATURE_DESC="no audio (AUDIO=0 diagnostic)"
fi
# XINPUT=1 is the XUSB (Xbox 360) persona. XUSB must be interface 0, so the
# audio function (when enabled) follows at 1/2/3. The tag makes the output
# name and the .board_type stamp distinct from every DS4 build (a persona
# switch forces a clean).
if [[ -n "${DEBUG_CONSOLE:-}" && "${DEBUG_CONSOLE}" != "0" ]]; then
    DEBUG_CONSOLE_ON=1
    FEATURE_TAG="${FEATURE_TAG}-dbg"
    FEATURE_DESC="${FEATURE_DESC} + UART/0xB4 log mirror"
fi
if [[ "${MIC_DIAG:-}" == "1" ]]; then
    FEATURE_TAG="${FEATURE_TAG}-diag"
    FEATURE_DESC="${FEATURE_DESC} + mic diagnostics"
fi
if [[ -n "${TOUCHPAD:-}" ]]; then
    FEATURE_TAG="${FEATURE_TAG}-tp"
    FEATURE_DESC="${FEATURE_DESC} + touchpad stub"
fi
if [[ -n "${GYRO:-}" ]]; then
    FEATURE_TAG="${FEATURE_TAG}-gyro"
    FEATURE_DESC="${FEATURE_DESC} + gyro stub"
fi
if [[ "${XINPUT:-}" == "1" ]]; then
    # Mutually exclusive with the DS4 HID gamepad: the XUSB interface is the
    # only gamepad in this build. Audio follows AUDIO (enabled by default,
    # so the default XINPUT build is the XUSB + UAC1 composite).
    FEATURE_TAG="${FEATURE_TAG}-xinput"
    if [[ "${AUDIO:-}" == "0" ]]; then
        FEATURE_DESC="XInput (XUSB 1209:D1CE) persona, XUSB-only (no audio)"
    else
        FEATURE_DESC="XInput (XUSB 1209:D1CE) persona + audio, no DS4 HID"
    fi
elif [[ "${GAMEPAD:-}" == "0" ]]; then
    FEATURE_TAG="${FEATURE_TAG}-nogp"
    FEATURE_DESC="${FEATURE_DESC}, no gamepad"
else
    FEATURE_DESC="${FEATURE_DESC} + gamepad"
fi

# Clear all board env vars first
unset BOARD_M0S_DOCK 2>/dev/null || true
unset BOARD_LCTECH_616 2>/dev/null || true

case "$BOARD_TYPE" in
    aim61)
        echo "[build] Target: Ai-M61-32S-Kit (BL618)"
        ;;
    m0sdock)
        export BOARD_M0S_DOCK=1
        echo "[build] Target: Sipeed M0S Dock (BL616)"
        ;;
    lctech616|*)
        BOARD_TYPE="lctech616"
        export BOARD_LCTECH_616=1
        echo "[build] Target: LCTech BL616"
        ;;
esac

echo "[build] USB:  Full-Speed only (12 Mbps, enforced; HS not supported)"
echo "[build] Audio: default (AUDIO=0 to disable)"
echo "[build] Features: ${FEATURE_DESC}"
if [[ "${XINPUT:-}" == "1" ]]; then
    echo "[build] Persona: XInput / XUSB (VID 1209 PID D1CE), DS4 HID disabled"
fi

# ── 环境变量（仅脚本执行期间生效） ──
export BL_SDK_BASE="${SCRIPT_DIR}/third_party/bouffalo_sdk"
export CHIP=bl616
export BOARD=bl616dk

# ── GNU Make（SDK 需要 4.0+；macOS 自带 3.81，需用 gmake） ──
if command -v gmake &>/dev/null; then
    MAKE_CMD="gmake"
elif [[ "$(make --version 2>/dev/null | head -1)" == *"4."* ]]; then
    MAKE_CMD="make"
else
    echo "ERROR: GNU Make >= 4.0 required (macOS default is 3.81)"
    echo "  brew install make   # 安装后可用 gmake 命令"
    exit 1
fi

# ── 清理中间文件（保留最终固件） ──
strip_intermediates() {
    echo "[build] Stripping intermediate files..."
    find build/build_out/components build/build_out/drivers \( -name "*.obj" -o -name "*.o" \) -delete 2>/dev/null || true
    rm -rf build/build_out/lib/*.a
    rm -rf build/build_out/components/*/CMakeFiles
    rm -rf build/build_out/drivers/*/CMakeFiles
    rm -rf build/CMakeFiles
    BEFORE_SIZE=$(du -sh build/ 2>/dev/null | cut -f1)
    echo "[build] Build dir: ${BEFORE_SIZE}"
}

usage() {
    echo "Usage: [BOARD_TYPE=...] ./build.sh [build|clean|flash|rebuild|strip]"
    echo ""
    echo "  build   - 编译项目（默认）"
    echo "  clean   - 清除构建产物"
    echo "  flash   - 烧录固件 (可选: ./build.sh flash /dev/tty.usbserial-xxx)"
    echo "  rebuild - 清除后重新编译"
    echo "  strip   - 仅清理中间文件（保留固件）"
    echo ""
    echo "Board types:  lctech616 (默认) | aim61 | m0sdock"
    echo ""
    echo "USB speed:"
    echo "  fs  — Full-Speed 12Mbps（唯一支持的模式）"
    echo ""
    echo "Features:"
    echo "  音频默认启用（AUDIO=0 仅供诊断时关闭）"
    echo "  GAMEPAD=0  去掉手柄 HID；TOUCHPAD=1 / GYRO=1 为预留开关"
    echo "  XINPUT=1   默认。Xbox 360 (XUSB) persona，带音频；AUDIO=0 仅 XUSB"
    echo "  XINPUT=0   参考：DS4 persona（本仓库不发布）"
    echo ""
    echo "Examples:"
    echo "  ./build.sh rebuild                          # 发布候选：XInput + 音频 → -xinput.bin"
    echo "  XINPUT=1 AUDIO=0 ./build.sh rebuild         # 仅 XUSB → -noaudio-xinput.bin"
    echo "  XINPUT=0 ./build.sh rebuild                 # 参考：DS4 persona"
    echo "  DEBUG_CONSOLE=1 ./build.sh rebuild          # 日志镜像 → -dbg.bin"
    echo "  MIC_DIAG=1 ./build.sh rebuild               # 麦克风诊断 → -diag.bin"
}

# ── 非编译动作：先于工具链探测/自动下载和配置切换处理 ──
# clean/flash/strip 不能依赖编译器，也不能触发 setup_toolchain.sh 或自动 clean。
ACTION="${1:-build}"

case "$ACTION" in
    clean)
        ${MAKE_CMD} CMAKE=cmake clean
        echo "[build] Cleaned."
        exit 0
        ;;
    flash)
        COMX="${2:-/dev/tty.usbserial*}"
        FLASH_TOOL="${BL_SDK_BASE}/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand-ubuntu"
        if [[ ! -x "$FLASH_TOOL" ]]; then
            echo "ERROR: flash tool is not vendored."
            echo "  Missing: ${FLASH_TOOL}"
            echo "  The vendored SDK drops tools/bflb_tools/bouffalo_flash_cube/ to save space."
            echo "  Use Bouffalo Dev Cube, or copy BLFlashCommand* from a full SDK checkout into that directory."
            exit 1
        fi
        ${MAKE_CMD} CMAKE=cmake flash COMX="$COMX"
        exit $?
        ;;
    strip)
        strip_intermediates
        exit 0
        ;;
    build|rebuild)
        : # handled below, after toolchain discovery
        ;;
    *)
        usage
        exit 1
        ;;
esac

# ── 工具链查找 ──
TOOLCHAIN_PREFIX=""


# The SDK needs the T-Head extensions (xtheade/zpsfoperand). Distro and
# Homebrew riscv64-elf-gcc cannot compile it, so actually compile a trivial
# translation unit with the required ISA before accepting a candidate.
probe_isa() {
    local gcc="$1" tmp rc
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/bl-isa.XXXXXX")" || return 1
    printf 'int main(void){return 0;}\n' > "$tmp/t.c"
    "$gcc" -march=rv32imafcpzpsfoperand_xtheade -mabi=ilp32f \
        -c "$tmp/t.c" -o "$tmp/t.o" >/dev/null 2>&1
    rc=$?
    rm -rf "$tmp"
    return $rc
}

find_toolchain() {
    local search_dirs=(
        "${SCRIPT_DIR}/third_party/toolchain"/*/bin
        "$HOME/riscv-toolchain/toolchain/bin"
        "/opt/riscv-toolchain/bin"
        "/opt/riscv-toolchain/xuantie/bin"
        "${BL_SDK_BASE}/tools/toolchain_gcc_t-head_linux/bin"
        "${BL_SDK_BASE}/tools/toolchain/bin"
    )
    local prefixes=("riscv64-unknown-elf-" "riscv64-elf-")
    local dir prefix gcc

    for dir in "${search_dirs[@]}"; do
        for prefix in "${prefixes[@]}"; do
            gcc="${dir}/${prefix}gcc"
            if [[ -x "$gcc" ]] && probe_isa "$gcc"; then
                export PATH="${dir}:${PATH}"
                TOOLCHAIN_PREFIX="$prefix"
                echo "[build] Toolchain found: ${dir}"
                return 0
            fi
        done
    done

    for prefix in "${prefixes[@]}"; do
        gcc="$(command -v "${prefix}gcc" 2>/dev/null || true)"
        if [[ -n "$gcc" ]] && probe_isa "$gcc"; then
            TOOLCHAIN_PREFIX="$prefix"
            echo "[build] Toolchain found: ${gcc}"
            return 0
        fi
    done

    return 1
}

# An explicit CROSS_COMPILE wins: never download a toolchain over it. Resolve
# it with command -v so both a bare PATH prefix (`riscv64-unknown-elf-`) and an
# absolute prefix (`/opt/x/bin/riscv64-unknown-elf-`) work, then verify the
# resolved compiler supports the required ISA.
if [[ -n "${CROSS_COMPILE:-}" ]]; then
    CROSS_COMPILE_GCC="$(command -v "${CROSS_COMPILE}gcc" 2>/dev/null || true)"
    if [[ -z "$CROSS_COMPILE_GCC" ]]; then
        echo "ERROR: CROSS_COMPILE=${CROSS_COMPILE}, but ${CROSS_COMPILE}gcc is not executable." >&2
        echo "       Point CROSS_COMPILE at the toolchain prefix (including the trailing dash)," >&2
        echo "       or unset it to let build.sh discover/download the Xuantie toolchain." >&2
        exit 1
    fi
    if ! probe_isa "$CROSS_COMPILE_GCC"; then
        echo "ERROR: CROSS_COMPILE=${CROSS_COMPILE}: ${CROSS_COMPILE_GCC} cannot compile the" >&2
        echo "       required T-Head ISA (-march=rv32imafcpzpsfoperand_xtheade -mabi=ilp32f)." >&2
        echo "       Use the official Xuantie toolchain (./setup_toolchain.sh) or fix CROSS_COMPILE." >&2
        exit 1
    fi
    TOOLCHAIN_PREFIX="$CROSS_COMPILE"
    echo "[build] Toolchain (CROSS_COMPILE): ${CROSS_COMPILE_GCC}"
else
    if ! find_toolchain; then
        # First run: the toolchain is not vendored (third_party/toolchain/ is
        # git-ignored). Download it once, then retry the search.
        if [[ "${SKIP_TOOLCHAIN_SETUP:-0}" != "1" \
              && ! -d "${SCRIPT_DIR}/third_party/toolchain" \
              && -x "${SCRIPT_DIR}/setup_toolchain.sh" ]]; then
            echo "[build] RISC-V toolchain not found — running ./setup_toolchain.sh (once)"
            if ! "${SCRIPT_DIR}/setup_toolchain.sh"; then
                echo "[build] ERROR: toolchain download failed." >&2
            fi
            find_toolchain || true
        fi
    fi
fi

if [[ -z "${TOOLCHAIN_PREFIX}" ]]; then
    echo "============================================"
    echo " ERROR: RISC-V toolchain not found"
    echo ""
    echo " SDK 需要 T-Head 扩展（xtheade / zpsfoperand），发行版或 Homebrew 的"
    echo " riscv64-elf-gcc 无法编译本项目。"
    echo ""
    echo "   推荐（自动下载官方 Xuantie 工具链）:"
    echo "     ./setup_toolchain.sh"
    echo ""
    echo "   或显式指定已有工具链:"
    echo "     CROSS_COMPILE=/path/to/riscv64-unknown-elf- ./build.sh"
    echo "============================================"
    exit 1
fi

export CROSS_COMPILE="$TOOLCHAIN_PREFIX"

echo "[build] SDK:       ${BL_SDK_BASE}"
echo "[build] Toolchain: $(command -v "${TOOLCHAIN_PREFIX}gcc" 2>/dev/null || echo "${TOOLCHAIN_PREFIX}gcc")"
echo "[build] Prefix:    ${CROSS_COMPILE}"
echo "[build] Make:      $(${MAKE_CMD} --version | head -1)"
echo "[build] Chip:      ${CHIP} / ${BOARD}"
echo ""

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# ── 构建 ──
BIN_PATH="build/build_out/ds4dongle_bl618_bl616.bin"


# Board-type + USB-speed + feature guard: auto-clean when switching config
BUILD_KEY="${BOARD_TYPE}-${USB_SPEED}${FEATURE_TAG}"
BOARD_STAMP="build/.board_type"
if [[ -f "$BOARD_STAMP" ]]; then
    PREV_KEY=$(cat "$BOARD_STAMP")
    if [[ "$PREV_KEY" != "$BUILD_KEY" ]]; then
        echo "[build] Config changed (${PREV_KEY} → ${BUILD_KEY}), forcing clean..."
        ${MAKE_CMD} CMAKE=cmake clean 2>/dev/null || true
        rm -rf build/build_out/CMakeCache.txt build/build_out/cmake_cache 2>/dev/null || true
    fi
fi

BIN_NAME="ds4dongle-${BOARD_TYPE}${FEATURE_TAG}.bin"
OUT_DIR="firmware/${BOARD_TYPE}"

# ── 输出 .bin（线刷用） ──
gen_bin() {
    if [[ ! -f "$BIN_PATH" ]]; then
        echo "[build] ERROR: bin not found at ${BIN_PATH}"
        return 1
    fi

    mkdir -p "${OUT_DIR}"
    cp "$BIN_PATH" "${OUT_DIR}/${BIN_NAME}"

    # Flash package: boot2 + partition + flash config
    cp build/build_out/boot2_*.bin "${OUT_DIR}/"
    cp build/build_out/partition.bin "${OUT_DIR}/"

    cat > "${OUT_DIR}/flash_prog_cfg.ini" <<FLASH_CFG
[cfg]
# 0: no erase, 1:programmed section erase, 2: chip erase
erase = 1
# skip mode set first para is skip addr, second para is skip len, multi-segment region with ; separated
skip_mode = 0x0, 0x0
# 0: not use isp mode, #1: isp mode
boot2_isp_mode = 0

[boot2]
filedir = ./boot2_*.bin
address = 0x000000

[partition]
filedir = ./partition*.bin
address = 0xE000

[FW]
filedir = ./${BIN_NAME}
address = @partition
FLASH_CFG

    echo "[build] .bin → ${OUT_DIR}/${BIN_NAME}"
    echo "[build] Flash package → ${OUT_DIR}/"
}

# ── 执行 ──
do_build() {
    ${MAKE_CMD} CMAKE=cmake CROSS_COMPILE="${CROSS_COMPILE}" -j${NPROC}
    mkdir -p build && echo "$BUILD_KEY" > "$BOARD_STAMP"
}

# ── DEBUG_CONSOLE consistency check (CMake macro ⇔ script -dbg tag) ──
# CMakeLists.txt writes this marker only when it enabled the DEBUG_CONSOLE
# macro. Belt and braces: the script refuses to copy a debug firmware to the
# release file name (or vice versa) if the two disagree.
DEBUG_MARKER="build/ds4_debug_console.enabled"
check_debug_tag() {
    if [[ -f "$DEBUG_MARKER" && "$DEBUG_CONSOLE_ON" != "1" ]]; then
        echo "[build] ERROR: CMake enabled DEBUG_CONSOLE but this script did not select the -dbg tag." >&2
        echo "[build]        Refusing to overwrite the release firmware with a debug build." >&2
        echo "[build]        DEBUG_CONSOLE is enabled by any non-empty value except 0." >&2
        return 1
    fi
    if [[ ! -f "$DEBUG_MARKER" && "$DEBUG_CONSOLE_ON" == "1" ]]; then
        echo "[build] ERROR: -dbg was selected but CMake did not enable DEBUG_CONSOLE." >&2
        echo "[build]        DEBUG_CONSOLE must be non-empty and not \"0\" (empty or 0 = release)." >&2
        return 1
    fi
    return 0
}

do_output() {
    gen_bin
}

case "$ACTION" in
    build)
        do_build
        check_debug_tag || exit 1
        do_output
        echo ""
        echo "[build] Done (${BOARD_TYPE}, usb=fs Full-Speed only). Output: ${OUT_DIR}/"
        ;;
    rebuild)
        ${MAKE_CMD} CMAKE=cmake clean
        rm -rf build/build_out/CMakeCache.txt build/build_out/cmake_cache 2>/dev/null || true
        do_build
        check_debug_tag || exit 1
        do_output
        echo ""
        echo "[build] Rebuilt (${BOARD_TYPE}, usb=fs Full-Speed only). Output: ${OUT_DIR}/"
        ;;
esac
