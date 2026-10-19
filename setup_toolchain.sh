#!/usr/bin/env bash
# Download the official Bouffalo/Xuantie RISC-V toolchain into
# third_party/toolchain/ (git-ignored). The SDK needs the T-Head extensions
# (-march=...xtheade, -march=...zpsfoperand), which the upstream
# gcc-riscv64-unknown-elf package does not support.
#
# Usage: ./setup_toolchain.sh [--force]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST="${SCRIPT_DIR}/third_party/toolchain"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

uname_s="$(uname -s)"
uname_m="$(uname -m)"

case "${uname_s}-${uname_m}" in
    Linux-x86_64)
        NAME="xuantie-gcc-riscv-v2.6.1-linux-x86_64.tar.xz"
        SHA256="83817bd7cab693fc249bb98869de5e475c1ede577f4a188869fed96c8dbb8e1a"
        ;;
    Linux-aarch64|Linux-arm64)
        NAME="xuantie-gcc-riscv-v2.10.2-linux-arm64.tar.xz"
        SHA256="9f19c300979d5e7189b9630c4480f5690b93c8e840a767dd4ef61bf5a6565c8e"
        ;;
    Darwin-arm64)
        NAME="xuantie-gcc-riscv-v2.10.2-macos-arm64.tar.xz"
        SHA256="3a950464d6f93cd17af27401e4156ccc9e743d4dc5f39010440af4abefd8705b"
        ;;
    *)
        echo "ERROR: no prebuilt Xuantie toolchain for ${uname_s}-${uname_m}." >&2
        echo "       Download one from https://github.com/bouffalolab/toolchain/releases/tag/default" >&2
        echo "       and extract it into ${DEST}/, or set CROSS_COMPILE." >&2
        exit 1
        ;;
esac

URL="https://github.com/bouffalolab/toolchain/releases/download/default/${NAME}"

# Already installed?
if [[ ${FORCE} -eq 0 ]] && compgen -G "${DEST}/*/bin/riscv64-unknown-elf-gcc" >/dev/null; then
    echo "[toolchain] already installed:"
    compgen -G "${DEST}/*/bin/riscv64-unknown-elf-gcc" | sed 's#/bin/riscv64-unknown-elf-gcc##' | sed 's/^/  /'
    exit 0
fi

for tool in curl tar xz; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "ERROR: '${tool}' is required." >&2
        if [[ "${tool}" == "xz" ]]; then
            echo "       Debian/Ubuntu: sudo apt install xz-utils" >&2
            echo "       macOS:         brew install xz" >&2
        else
            echo "       Debian/Ubuntu: sudo apt install curl" >&2
            echo "       macOS:         brew install curl" >&2
        fi
        exit 1
    fi
done

mkdir -p "${DEST}"
TMP="$(mktemp -d "${DEST}/.download.XXXXXX")"
trap 'rm -rf "${TMP}"' EXIT

echo "[toolchain] downloading ${NAME} (~95 MB)..."
if ! curl -L --retry 3 --retry-delay 2 -o "${TMP}/${NAME}" "${URL}"; then
    echo "ERROR: download failed." >&2
    echo "       Retry, or download manually: ${URL}" >&2
    exit 1
fi

echo "[toolchain] verifying sha256..."
if command -v sha256sum >/dev/null 2>&1; then
    SHA256_CMD=(sha256sum -c -)
elif command -v shasum >/dev/null 2>&1; then
    # macOS ships shasum (Perl) but not sha256sum.
    SHA256_CMD=(shasum -a 256 -c -)
else
    echo "ERROR: no SHA-256 tool found (need sha256sum or shasum)." >&2
    echo "       Debian/Ubuntu: sudo apt install coreutils" >&2
    echo "       macOS:         shasum ships with the base system" >&2
    exit 1
fi
if ! echo "${SHA256}  ${TMP}/${NAME}" | "${SHA256_CMD[@]}" >/dev/null; then
    echo "ERROR: sha256 mismatch for ${NAME}." >&2
    exit 1
fi

echo "[toolchain] extracting into ${DEST}..."
tar -xJf "${TMP}/${NAME}" -C "${DEST}"

echo "[toolchain] installed:"
compgen -G "${DEST}/*/bin/riscv64-unknown-elf-gcc" | sed 's#/bin/riscv64-unknown-elf-gcc##' | sed 's/^/  /'
echo "[toolchain] done."
