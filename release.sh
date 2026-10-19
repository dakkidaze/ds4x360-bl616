#!/usr/bin/env bash
# Build the two XInput release variants and assemble a flashable bundle in
# release/.
#
#   xinput           XINPUT=1            XUSB pad + UAC1 audio (default build)
#   xinput-noaudio   XINPUT=1 AUDIO=0    XUSB pad only, no audio function
#
# The boot2/partition payload is identical for both variants; only the
# application image and the per-variant flash_prog_cfg-*.ini differ.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BOARD_TYPE="${BOARD_TYPE:-lctech616}"
if [[ "$BOARD_TYPE" != "lctech616" ]]; then
    echo "ERROR: release assets are defined for lctech616 only (BOARD_TYPE=${BOARD_TYPE})." >&2
    echo "       Unset BOARD_TYPE or set BOARD_TYPE=lctech616." >&2
    exit 1
fi

OUT_DIR="firmware/${BOARD_TYPE}"
REL_DIR="release"

# ── Source provenance ──
# A bundle is only reproducible if the tree is clean AND the source URL is
# public. Anything else is a development bundle and must say so.
GIT_TREE=0
COMMIT=""
TRACKED_DIRTY="unknown"
UNTRACKED_LIST=""
UNTRACKED_COUNT=0

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    GIT_TREE=1
    COMMIT="$(git rev-parse HEAD 2>/dev/null || true)"
    if git diff-index --quiet HEAD -- 2>/dev/null; then
        TRACKED_DIRTY=0
    else
        TRACKED_DIRTY=1
    fi
    # sed (not head) reads the whole stream: no SIGPIPE upstream under pipefail.
    UNTRACKED_LIST="$(git ls-files --others --exclude-standard | sed -n '1,20p')"
    UNTRACKED_COUNT="$(git ls-files --others --exclude-standard | wc -l | tr -d ' ')"
fi

SOURCE_URL="${SOURCE_URL:-$(git config --get remote.origin.url 2>/dev/null || true)}"
# Syntax check only: http(s):// or git@host: looks publishable. This says
# nothing about reachability or anonymous access, which the script cannot test.
is_public_url() {
    [[ "$1" =~ ^https?://[^/]+ ]] || [[ "$1" =~ ^git@[^:]+: ]]
}

PUBLIC_URL=0
[[ -n "$SOURCE_URL" ]] && is_public_url "$SOURCE_URL" && PUBLIC_URL=1

REPRODUCIBLE=0
if [[ $GIT_TREE -eq 1 && "$TRACKED_DIRTY" == "0" && "$UNTRACKED_COUNT" == "0" && $PUBLIC_URL -eq 1 ]]; then
    REPRODUCIBLE=1
fi

if [[ "${STRICT_SOURCE:-0}" == "1" && $REPRODUCIBLE -ne 1 ]]; then
    echo "ERROR: STRICT_SOURCE=1 requires a clean git tree and a source URL that looks" >&2
    echo "       public (http/https or git@). Reachability is NOT checked by this script." >&2
    echo "  git tree:          $([[ $GIT_TREE -eq 1 ]] && echo "yes" || echo "no (not a git checkout)")" >&2
    echo "  tracked changes:   $([[ "$TRACKED_DIRTY" == "1" ]] && echo "yes" || echo "no")" >&2
    echo "  untracked files:   ${UNTRACKED_COUNT}" >&2
    if [[ $PUBLIC_URL -eq 1 ]]; then
        echo "  source URL:        ${SOURCE_URL} (looks public; availability not verified)" >&2
    elif [[ -n "$SOURCE_URL" ]]; then
        echo "  source URL:        ${SOURCE_URL} (not public-looking: use http(s):// or git@...)" >&2
    else
        echo "  source URL:        <none: set SOURCE_URL or 'git remote add origin <url>'>" >&2
    fi
    echo "Commit/stash the changes and configure a public-looking remote, or drop STRICT_SOURCE." >&2
    exit 1
fi

if [[ $REPRODUCIBLE -ne 1 ]]; then
    echo "" >&2
    echo "!! DEVELOPMENT BUNDLE: this tree is not clean/reproducible." >&2
    echo "!!   tracked changes: $([[ "$TRACKED_DIRTY" == "1" ]] && echo yes || echo no);" \
         "untracked: ${UNTRACKED_COUNT};" \
         "public URL: $([[ $PUBLIC_URL -eq 1 ]] && echo yes || echo no)" >&2
    echo "!!   SOURCE.txt will record this and will NOT claim the commit reproduces the bundle." >&2
    echo "" >&2
fi

rm -rf "$REL_DIR"
mkdir -p "$REL_DIR"

# Track the boot2 image name (same file for every variant, but read it from the
# build output so a version bump does not silently break the flash configs).
BOOT2_NAME=""

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "ERROR: expected build output missing: $1" >&2
        exit 1
    fi
}

# Copy boot2 + partition into release/ (idempotent, identical for all variants).
copy_common() {
    local boot2_files=()
    shopt -s nullglob
    boot2_files=("${OUT_DIR}"/boot2_*.bin)
    shopt -u nullglob

    if [[ ${#boot2_files[@]} -ne 1 ]]; then
        echo "ERROR: expected exactly one ${OUT_DIR}/boot2_*.bin, found ${#boot2_files[@]}." >&2
        exit 1
    fi
    BOOT2_NAME="$(basename "${boot2_files[0]}")"
    cp -f "${boot2_files[0]}" "${REL_DIR}/${BOOT2_NAME}"
    require_file "${OUT_DIR}/partition.bin"
    cp -f "${OUT_DIR}/partition.bin" "${REL_DIR}/partition.bin"
}

# build_variant <variant> <source bin> [env assignments...]
#
# Every inherited feature switch is stripped first (env -u), so a contaminated
# shell (`AUDIO=0 DEBUG_CONSOLE=1 ... ./release.sh`) cannot change what a
# variant means. The explicit assignments come last and win.
build_variant() {
    local variant="$1"
    local src="$2"
    shift 2

    echo ""
    echo "============================================================"
    echo "[release] Building ${variant}"
    echo "============================================================"

    # A stale bin must never be copied: remove it, then require the build to
    # produce a fresh one.
    rm -f "$src"

    env -u AUDIO -u DEBUG_CONSOLE -u MIC_DIAG -u TOUCHPAD -u GYRO -u GAMEPAD \
        -u XINPUT -u USB_SPEED -u BOARD_TYPE \
        BOARD_TYPE=lctech616 USB_SPEED=fs "$@" ./build.sh

    require_file "$src"
    cp -f "$src" "${REL_DIR}/ds4x360-bl616-${variant}.bin"
    copy_common

    cat > "${REL_DIR}/flash_prog_cfg-${variant}.ini" <<FLASH_CFG
[cfg]
erase = 1
skip_mode = 0x0, 0x0
boot2_isp_mode = 0

[boot2]
filedir = ./${BOOT2_NAME}
address = 0x000000

[partition]
filedir = ./partition.bin
address = 0xE000

[FW]
filedir = ./ds4x360-bl616-${variant}.bin
address = @partition
FLASH_CFG

    echo "[release] ${variant} -> ${REL_DIR}/ds4x360-bl616-${variant}.bin"
}

echo "[release] Board: ${BOARD_TYPE} (LCTech BL616)"

build_variant xinput \
    "${OUT_DIR}/ds4dongle-lctech616-xinput.bin" \
    XINPUT=1
build_variant xinput-noaudio \
    "${OUT_DIR}/ds4dongle-lctech616-noaudio-xinput.bin" \
    XINPUT=1 AUDIO=0

# ── Licensing materials ──
require_file LICENSE
cp -f LICENSE "${REL_DIR}/LICENSE"
require_file NOTICE
cp -f NOTICE "${REL_DIR}/NOTICE"
require_file third_party/bouffalo_sdk/LICENSE
cp -f third_party/bouffalo_sdk/LICENSE "${REL_DIR}/LICENSE-bouffalo_sdk"

# Standalone third-party license texts that exist in the vendored tree.
require_file third_party/bouffalo_sdk/components/crypto/mbedtls/mbedtls_v3/LICENSE
cp -f third_party/bouffalo_sdk/components/crypto/mbedtls/mbedtls_v3/LICENSE \
    "${REL_DIR}/LICENSE-mbedtls"
# FreeRTOS keeps its MIT text under the POSIX port; the kernel sources carry
# the same grant (see THIRD_PARTY.md).
require_file third_party/bouffalo_sdk/components/os/freertos/posix/LICENSE
cp -f third_party/bouffalo_sdk/components/os/freertos/posix/LICENSE \
    "${REL_DIR}/LICENSE-freertos"

# Components without a standalone license file in the trimmed tree.
cat > "${REL_DIR}/THIRD_PARTY.md" <<'THIRD_PARTY_MD'
# Third-party components in this bundle

The application firmware links against components from the vendored Bouffalo
SDK. Where the trimmed tree has no standalone license file, this table records
the license, the upstream project, and the in-tree source that carries the
notice. No license text is reproduced here.

| Component | License | Upstream | In-tree notice |
|---|---|---|---|
| CherryUSB | Apache-2.0 | https://github.com/cherry-embedded/CherryUSB | `third_party/bouffalo_sdk/components/usb/cherryusb/core/usbd_core.c` (SPDX header), `.../common/usb_def.h` |
| FreeRTOS kernel | MIT | https://github.com/FreeRTOS/FreeRTOS-Kernel | `third_party/bouffalo_sdk/components/os/freertos/tasks.c` (full MIT text), copy shipped as `LICENSE-freertos` |
| Zephyr Bluetooth host (blestack) | Apache-2.0 | https://github.com/zephyrproject-rtos/zephyr | `third_party/bouffalo_sdk/components/wireless/bluetooth/blestack/src/host/conn.c` (SPDX header) |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later (used as Apache-2.0) | https://github.com/Mbed-TLS/mbedtls | `third_party/bouffalo_sdk/components/crypto/mbedtls/mbedtls_v3/LICENSE`, shipped as `LICENSE-mbedtls` |
| Bouffalo SDK | Apache-2.0 | https://github.com/bouffalolab/bouffalo_sdk | `third_party/bouffalo_sdk/LICENSE`, shipped as `LICENSE-bouffalo_sdk` |

The vendored SDK also contains other bundled libraries; their notices remain
inside `third_party/bouffalo_sdk/`.
THIRD_PARTY_MD

# ── GPL-3.0 Corresponding Source pointer ──
DISPLAY_URL="${SOURCE_URL}"
if [[ -z "$DISPLAY_URL" ]]; then
    DISPLAY_URL="<repository URL not configured: set SOURCE_URL or add a git remote>"
fi
DISPLAY_COMMIT="${COMMIT:-<not a git checkout: commit hash unavailable>}"

{
    cat <<'SOURCE_HEAD'
ds4x360-bl616 - XInput firmware release bundle (LCTech BL616)
=============================================================

Corresponding Source (GPL-3.0)
------------------------------
SOURCE_HEAD

    if [[ $REPRODUCIBLE -eq 1 ]]; then
        cat <<EOF
Bundle status: CLEAN SOURCE REVISION - clean tree at this commit, source URL
looks public (http/https or git@). Public availability is NOT verified.

The complete Corresponding Source for these binaries is the ds4x360-bl616
repository at the revision below; the clean tree at this commit contains
everything needed to rebuild these binaries.

Repository: ${DISPLAY_URL}
Commit:     ${DISPLAY_COMMIT}

Public availability is not verified by this script. Before publishing, confirm
that an anonymous clone (no credentials) can retrieve this exact commit.
EOF
    else
        cat <<EOF
Bundle status: DEVELOPMENT BUNDLE - NOT reproducible from the commit below.

This bundle was built from a working tree that does not correspond to a single
published commit. The repository/commit below identify the base revision only;
they do NOT contain everything needed to rebuild these exact binaries.

Repository: ${DISPLAY_URL}
Commit:     ${DISPLAY_COMMIT}
Tracked changes at build time: $([[ "$TRACKED_DIRTY" == "1" ]] && echo "yes" || echo "no")
Untracked files at build time: ${UNTRACKED_COUNT}
Public source URL: $([[ $PUBLIC_URL -eq 1 ]] && echo "yes" || echo "no")
EOF
        if [[ "$UNTRACKED_COUNT" != "0" ]]; then
            echo ""
            echo "Untracked files (first 20):"
            printf '%s\n' "$UNTRACKED_LIST" | sed 's/^/  /'
        fi
        cat <<'EOF'

To produce a reproducible bundle, commit all changes, configure a public
remote (or SOURCE_URL=https://...), and run with STRICT_SOURCE=1.
EOF
    fi

    cat <<'SOURCE_TAIL'

Source layout (for rebuilding):
  src/                        application sources (BT host, USB, XInput)
  tools/                      host-side tools and self-checks
  CMakeLists.txt, Makefile    build definition
  build.sh, build_macos.sh    build entry point and implementation
  release.sh                  this release script
  setup_toolchain.sh          downloads the official Xuantie toolchain
  third_party/bouffalo_sdk/   vendored patched Bouffalo SDK (Apache-2.0,
                              see LICENSE-bouffalo_sdk)
  SDK_CHANGES.md              every local SDK patch and why it exists

Build instructions: BUILD.md (English) / BUILD_CN.md (Chinese).

Third-party components
----------------------
See NOTICE and THIRD_PARTY.md. Standalone license texts shipped here:
  LICENSE-bouffalo_sdk, LICENSE-freertos, LICENSE-mbedtls.
SOURCE_TAIL
} > "${REL_DIR}/SOURCE.txt"

# ── Bundle README ──
cat > "${REL_DIR}/README.txt" <<README_TXT
ds4x360-bl616 - XInput release bundle (LCTech BL616)
====================================================

Source provenance
-----------------
Status:    $([[ $REPRODUCIBLE -eq 1 ]] && echo "CLEAN SOURCE REVISION (clean tree, public-looking URL; availability not verified)" || echo "DEVELOPMENT BUNDLE (NOT reproducible from the commit below)")
Repository: ${DISPLAY_URL}
Commit:     ${DISPLAY_COMMIT}
Tracked changes at build time: $([[ "$TRACKED_DIRTY" == "1" ]] && echo "yes" || echo "no")
Untracked files at build time: ${UNTRACKED_COUNT}
Public source URL: $([[ $PUBLIC_URL -eq 1 ]] && echo "yes" || echo "no")

A clean source revision requires a clean tree and a public-looking source URL
(http/https or git@); this script does not verify reachability. Public
availability is not verified by this script. Before publishing, confirm that an
anonymous clone (no credentials) can retrieve this exact commit.
See SOURCE.txt for the full statement and THIRD_PARTY.md for license details.

Variants
--------
xinput           ds4x360-bl616-xinput.bin
                 XINPUT=1 (default build): Xbox 360 wired pad (XUSB,
                 VID:PID 1209:D1CE) plus the UAC1 audio function.

xinput-noaudio   ds4x360-bl616-xinput-noaudio.bin
                 XINPUT=1 AUDIO=0: XUSB pad only, no UAC1 audio function.

Each variant ships with its own flash configuration:
  flash_prog_cfg-xinput.ini
  flash_prog_cfg-xinput-noaudio.ini
boot2_*.bin and partition.bin are identical for both variants.

Verify the download
-------------------
  cd release
  sha256sum -c SHA256SUMS
  # macOS:
  shasum -a 256 -c SHA256SUMS

What the .ini does
------------------
The .ini is a flash layout manifest, not firmware. It tells BLFlashCommand:
  boot2     -> 0x000000
  partition -> 0xE000
  app       -> @partition (resolved from partition.bin; 0x10000 on this layout)
Keep the files together: the ini uses relative paths. If you rename a bin,
edit the matching [FW] filedir line.

Flashing (validated path: BLFlashCommand over the board's USB ISP port)
-----------------------------------------------------------------------
1. Unplug the board, hold BOOT, plug the board's own USB port into the PC.
   The ROM ISP enumerates as USB CDC 349b:6160 (COMx on Windows,
   /dev/ttyACM0 on Linux/WSL2). Release BOOT when the port appears.
2. Run BLFlashCommand from this directory:

   BLFlashCommand-ubuntu --interface=uart --baudrate=2000000 \
     --port=/dev/ttyACM0 --chipname=bl616 \
     --config=flash_prog_cfg-<variant>.ini --reset

   (macOS: BLFlashCommand-macos; Windows: BLFlashCommand.exe with COMx.)
3. The tool's --reset may leave the chip in ISP mode. If the app does not
   start, unplug and replug without holding BOOT.

BLFlashCommand is NOT shipped in this repository (the vendored SDK drops
tools/bflb_tools/bouffalo_flash_cube). Copy it from a full Bouffalo SDK.

Bouffalo Dev Cube is an alternative, but it does not import BLFlashCommand's
.ini; configure it manually with the addresses above:
  boot2     0x000000
  partition 0xE000
  app       0x10000

Hardware validation: LCTech BL616 only.
README_TXT

# ── Checksums (bundle-relative names, not including SHA256SUMS itself) ──
(
    cd "$REL_DIR"
    shopt -s nullglob
    files=()
    files=()
    for f in *; do
        [[ "$f" == "SHA256SUMS" ]] && continue
        [[ -f "$f" ]] && files+=("$f")
    done
    shopt -u nullglob

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "${files[@]}" > SHA256SUMS
    else
        shasum -a 256 "${files[@]}" > SHA256SUMS
    fi
)

echo ""
echo "[release] Contents:"
ls -lh release

echo ""
echo "[release] SHA256SUMS:"
cat "${REL_DIR}/SHA256SUMS"

echo ""
echo "[release] Done: two variants in ${REL_DIR}/"
