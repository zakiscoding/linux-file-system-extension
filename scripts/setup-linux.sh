#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require sudo apt-get
sudo apt-get update
sudo apt-get install -y build-essential flex bison bc libssl-dev libelf-dev \
    libncurses-dev curl xz-utils cpio busybox-static qemu-system-x86 gdb git file python3
echo 'Dependencies ready. Run: bash scripts/validate-and-boot.sh'
