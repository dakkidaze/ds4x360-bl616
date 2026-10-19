#!/usr/bin/env bash
# Canonical build entry point. build_macos.sh is the Linux/macOS/WSL
# implementation; this wrapper exists so docs and CI can use ./build.sh.
exec "$(cd "$(dirname "$0")" && pwd)/build_macos.sh" "$@"
