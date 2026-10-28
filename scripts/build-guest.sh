#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require make gcc cpio gzip file python3
[[ -f "$KDIR/Module.symvers" ]] || { echo 'Run build-kernel.sh first.' >&2; exit 1; }
busybox="${BUSYBOX:-/bin/busybox}"
[[ -x "$busybox" ]] || { echo 'Install busybox-static or set BUSYBOX.' >&2; exit 1; }
file "$busybox" | grep -Eq 'statically linked|static-pie linked' || {
    echo 'The initramfs requires a statically linked BusyBox.' >&2; exit 1;
}
# Keep kbuild output in build/ so source files remain clean.
mkdir -p "$BUILD/module"
cp "$ROOT/kernel/Makefile" "$ROOT/kernel/edufs.c" "$BUILD/module/"
cp "$ROOT/include/edufs_disk.h" "$BUILD/module/"
make -C "$KDIR" M="$BUILD/module" modules
guest="$BUILD/initramfs"
mkdir -p "$guest"/{bin,sbin,proc,sys,dev,tmp,mnt,modules}
cp "$busybox" "$guest/bin/busybox"
for app in sh mount umount insmod rmmod grep ls dmesg poweroff mkdir cat; do
    ln -sfn busybox "$guest/bin/$app"
done
cp "$BUILD/module/edufs.ko" "$guest/modules/"
gcc -static -std=c11 -Wall -Wextra -Werror -O2 -I"$ROOT/include" \
    "$ROOT/tests/guest_tests.c" -o "$guest/bin/guest-tests"
gcc -static -std=c11 -Wall -Wextra -Werror -O2 -I"$ROOT/include" \
    "$ROOT/tools/edufs.c" -o "$guest/bin/edufs"
cp "$ROOT/guest/init" "$guest/init"
chmod +x "$guest/init"
(cd "$guest"; find . -print0 | cpio --null -o --format=newc | gzip -n > "$BUILD/initramfs.cpio.gz")
if [[ ! -e "$BUILD/disk.img" ]]; then
    "$guest/bin/edufs" mkfs "$BUILD/disk.img" 16384
fi
"$guest/bin/edufs" check "$BUILD/disk.img"
python3 "$ROOT/scripts/corrupt-fixtures.py" "$BUILD"
