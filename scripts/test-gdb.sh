#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require gdb timeout
run_dir=$(mktemp -d "$BUILD/gdb-run.XXXXXX")
export EDUFS_DISK_IMAGE="$run_dir/disk.img"
"$BUILD/initramfs/bin/edufs" mkfs "$EDUFS_DISK_IMAGE" 16384
bash "$ROOT/scripts/run-qemu.sh" --debug --test > "$BUILD/gdb-guest.log" 2>&1 &
guest_pid=$!
trap 'kill "$guest_pid" 2>/dev/null || true' EXIT
# Retry connection briefly while QEMU starts its loopback stub.
for attempt in {1..50}; do
    if (echo > /dev/tcp/127.0.0.1/1234) 2>/dev/null; then break; fi
    sleep 0.1
done
(cd "$KDIR"; timeout 120 gdb -batch -iex "add-auto-load-safe-path $KDIR" \
    -x "$ROOT/scripts/rollback.gdb" vmlinux) > "$BUILD/gdb-rollback.log" 2>&1
grep -q 'EDUFS GDB ROLLBACK PASS' "$BUILD/gdb-rollback.log"
wait "$guest_pid"
grep -q 'EDUFS AUTOMATED PASS' "$BUILD/gdb-guest.log"
"$BUILD/initramfs/bin/edufs" check "$EDUFS_DISK_IMAGE"
cat "$BUILD/gdb-rollback.log"
