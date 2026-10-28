#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require gdb
cd "$KDIR"
exec gdb -iex "add-auto-load-safe-path $KDIR" \
    -ex 'target remote 127.0.0.1:1234' -ex 'break start_kernel' vmlinux
