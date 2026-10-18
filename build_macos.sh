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
# XINPUT=1 is the XUSB (Xbox 360) persona. Phase 0 is XUSB-only, so it must
# be built with AUDIO=0; the tag makes the output name and the .board_type
# stamp distinct from every DS4 build (a persona switch forces a clean).
if [[ "${XINPUT:-}" == "1" && "${AUDIO:-}" != "0" ]]; then
    echo "ERROR: XINPUT=1 Phase 0 is XUSB-only - run with AUDIO=0 (Phase 1 re-adds audio)." >&2
    exit 1
fi
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
    # only gamepad in this build.
    FEATURE_TAG="${FEATURE_TAG}-xinput"
    FEATURE_DESC="XInput (XUSB 045E:028E) persona, no DS4 HID"
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
    echo "[build] Persona: XInput / XUSB (VID 045E PID 028E), DS4 HID disabled"
fi

# ── 环境变量（仅脚本执行期间生效） ──
export BL_SDK_BASE="${SCRIPT_DIR}/../bouffalo_sdk"
export CHIP=bl616
export BOARD=bl616dk

# ── 工具链查找 ──
TOOLCHAIN_PREFIX=""

find_toolchain() {
    local search_dirs=(
        "$HOME/riscv-toolchain/toolchain/bin"
        "/opt/riscv-toolchain/bin"
        "/opt/riscv-toolchain/xuantie/bin"
        "${BL_SDK_BASE}/tools/toolchain_gcc_t-head_linux/bin"
        "${BL_SDK_BASE}/tools/toolchain/bin"
    )
    local prefixes=("riscv64-unknown-elf-" "riscv64-elf-")

    for dir in "${search_dirs[@]}"; do
        for prefix in "${prefixes[@]}"; do
            if [[ -x "${dir}/${prefix}gcc" ]]; then
                export PATH="${dir}:${PATH}"
                TOOLCHAIN_PREFIX="$prefix"
                echo "[build] Toolchain found: ${dir}"
                return 0
            fi
        done
    done

    for prefix in "${prefixes[@]}"; do
        if command -v "${prefix}gcc" &>/dev/null; then
            TOOLCHAIN_PREFIX="$prefix"
            return 0
        fi
    done

    return 1
}

if ! find_toolchain; then
    echo "============================================"
    echo " ERROR: RISC-V toolchain not found"
    echo ""
    echo " 请安装 riscv64-unknown-elf-gcc 或 riscv64-elf-gcc："
    echo ""
    echo "   macOS (Homebrew):"
    echo "     brew install riscv64-elf-gcc"
    echo ""
    echo "   安装后重新执行: ./build_macos.sh"
    echo "============================================"
    exit 1
fi

export CROSS_COMPILE="$TOOLCHAIN_PREFIX"

# macOS 自带 Make 3.81 不支持 $(file >) 语法，SDK 需要 4.0+
if command -v gmake &>/dev/null; then
    MAKE_CMD="gmake"
elif [[ "$(make --version 2>/dev/null | head -1)" == *"4."* ]]; then
    MAKE_CMD="make"
else
    echo "ERROR: GNU Make >= 4.0 required (macOS default is 3.81)"
    echo "  brew install make   # 安装后可用 gmake 命令"
    exit 1
fi

echo "[build] SDK:       ${BL_SDK_BASE}"
echo "[build] Toolchain: $(which ${TOOLCHAIN_PREFIX}gcc)"
echo "[build] Prefix:    ${CROSS_COMPILE}"
echo "[build] Make:      $(${MAKE_CMD} --version | head -1)"
echo "[build] Chip:      ${CHIP} / ${BOARD}"
echo ""

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

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

# ── 构建 ──
ACTION="${1:-build}"

BIN_PATH="build/build_out/ds4dongle_bl618_bl616.bin"

# Board-type + USB-speed + feature guard: auto-clean when switching config
BUILD_KEY="${BOARD_TYPE}-${USB_SPEED}${FEATURE_TAG}"
BOARD_STAMP="build/.board_type"
if [[ -f "$BOARD_STAMP" ]]; then
    PREV_KEY=$(cat "$BOARD_STAMP")
    if [[ "$PREV_KEY" != "$BUILD_KEY" ]]; then
        echo "[build] Config changed (${PREV_KEY} → ${BUILD_KEY}), forcing clean..."
        ${MAKE_CMD} clean 2>/dev/null || true
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
    ${MAKE_CMD} CROSS_COMPILE="${CROSS_COMPILE}" -j${NPROC}
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
    clean)
        ${MAKE_CMD} clean
        echo "[build] Cleaned."
        ;;
    flash)
        COMX="${2:-/dev/tty.usbserial*}"
        ${MAKE_CMD} flash COMX="$COMX"
        ;;
    rebuild)
        ${MAKE_CMD} clean
        rm -rf build/build_out/CMakeCache.txt build/build_out/cmake_cache 2>/dev/null || true
        do_build
        check_debug_tag || exit 1
        do_output
        echo ""
        echo "[build] Rebuilt (${BOARD_TYPE}, usb=fs Full-Speed only). Output: ${OUT_DIR}/"
        ;;
    strip)
        strip_intermediates
        ;;
    *)
        echo "Usage: [BOARD_TYPE=...] ./build_macos.sh [build|clean|flash|rebuild|strip]"
        echo ""
        echo "  build   - 编译项目（默认）"
        echo "  clean   - 清除构建产物"
        echo "  flash   - 烧录固件 (可选: ./build_macos.sh flash /dev/tty.usbserial-xxx)"
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
        echo ""
        echo "Examples:"
        echo "  ./build_macos.sh rebuild                    # 发布候选：audio + gamepad"
        echo "  DEBUG_CONSOLE=1 ./build_macos.sh rebuild    # 日志镜像 → -dbg.bin"
        echo "  MIC_DIAG=1 ./build_macos.sh rebuild         # 麦克风诊断 → -diag.bin"
        echo "  GAMEPAD=0 ./build_macos.sh rebuild          # 仅音频 → -nogp.bin"
        echo "  AUDIO=0 ./build_macos.sh rebuild            # 诊断：无音频 → -noaudio.bin"
        exit 1
        ;;
esac
