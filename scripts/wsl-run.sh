#!/usr/bin/env bash
set -euo pipefail
source_root="$1"
task="${2:-all}"
linux_root=/root/projects/edufs
mkdir -p "$linux_root" "$source_root/build/linux-evidence"
# Preserve build products in the Linux filesystem while refreshing source files.
(cd "$source_root"; tar --exclude=./build --exclude=./.git -cf - .) | tar -xf - -C "$linux_root"
cd "$linux_root"
export JOBS="${JOBS:-8}"
copy_evidence() {
    for log in offline-test.log qemu-test.log gdb-rollback.log gdb-guest.log; do
        if [[ -f "build/$log" ]]; then cp "build/$log" "$source_root/build/linux-evidence/"; fi
    done
}
trap copy_evidence EXIT
case "$task" in
    test) mkdir -p build; make test 2>&1 | tee build/offline-test.log ;;
    build) bash scripts/build-kernel.sh; bash scripts/build-guest.sh ;;
    qemu) bash scripts/build-guest.sh; bash scripts/test-qemu.sh ;;
    gdb) bash scripts/build-guest.sh; bash scripts/test-gdb.sh ;;
    all)
        mkdir -p build
        make test 2>&1 | tee build/offline-test.log
        bash scripts/build-kernel.sh
        bash scripts/build-guest.sh
        bash scripts/test-qemu.sh
        bash scripts/test-gdb.sh
        ;;
    *) echo 'Task must be test, build, qemu, gdb, or all.' >&2; exit 1 ;;
esac
