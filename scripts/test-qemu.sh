#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require timeout tee grep mktemp
run_dir=$(mktemp -d "$BUILD/test-run.XXXXXX")
export EDUFS_DISK_IMAGE="$run_dir/disk.img"
"$BUILD/initramfs/bin/edufs" mkfs "$EDUFS_DISK_IMAGE" 16384
# The VFS suite uses and cleans its own files. Never attach valuable images.
timeout 600 bash "$ROOT/scripts/run-qemu.sh" --test 2>&1 | tee "$BUILD/qemu-test.log"
grep -q 'EDUFS AUTOMATED PASS' "$BUILD/qemu-test.log"
if grep -Eq 'BUG:|Oops:|Kernel panic|EDUFS AUTOMATED FAIL' "$BUILD/qemu-test.log"; then
    echo 'Guest failure detected; inspect build/qemu-test.log.' >&2
    exit 1
fi
"$BUILD/initramfs/bin/edufs" check "$EDUFS_DISK_IMAGE"
echo "Validated disposable image: $EDUFS_DISK_IMAGE"
