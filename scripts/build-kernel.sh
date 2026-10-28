#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require make gcc curl tar xz sha256sum flex bison bc perl openssl
mkdir -p "$BUILD"
if [[ ! -d "$KDIR" ]]; then
    archive="linux-$KERNEL_VERSION.tar.xz"
    base=https://cdn.kernel.org/pub/linux/kernel/v6.x
    curl -fL "$base/$archive" -o "$BUILD/$archive"
    curl -fL "$base/sha256sums.asc" -o "$BUILD/sha256sums.asc"
    (cd "$BUILD"; awk -v name="$archive" '$2 == name {print}' sha256sums.asc > kernel.sha256
     test -s kernel.sha256; sha256sum -c kernel.sha256)
    tar -xJf "$BUILD/$archive" -C "$BUILD"
fi
make -C "$KDIR" x86_64_defconfig
"$KDIR/scripts/config" --file "$KDIR/.config" \
    -e MODULES -e BLK_DEV_INITRD -e DEVTMPFS -e DEVTMPFS_MOUNT \
    -e VIRTIO_PCI -e VIRTIO_BLK -e SERIAL_8250 -e SERIAL_8250_CONSOLE \
    -e DEBUG_KERNEL -d DEBUG_INFO_NONE -e DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT \
    -e GDB_SCRIPTS -e FRAME_POINTER -d RANDOMIZE_BASE -d DEBUG_INFO_BTF
make -C "$KDIR" olddefconfig
make -C "$KDIR" -j"${JOBS:-$(nproc)}" bzImage modules
make -C "$KDIR" scripts_gdb
ln -sfn scripts/gdb/vmlinux-gdb.py "$KDIR/vmlinux-gdb.py"
