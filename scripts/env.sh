#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_VERSION=6.12.114
BUILD="${EDUFS_BUILD_DIR:-$ROOT/build}"
KDIR="$BUILD/linux-$KERNEL_VERSION"
export ROOT BUILD KDIR KERNEL_VERSION
if [[ "$(uname -s)" != Linux ]]; then
    echo 'Run these scripts in Linux or WSL2, not a Windows shell.' >&2
    exit 1
fi
require() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null || { echo "Missing dependency: $tool" >&2; exit 1; }
    done
}
