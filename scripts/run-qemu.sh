#!/usr/bin/env bash
source "$(dirname -- "$0")/env.sh"
require qemu-system-x86_64
disk="${EDUFS_DISK_IMAGE:-$BUILD/disk.img}"
for path in "$KDIR/arch/x86/boot/bzImage" "$BUILD/initramfs.cpio.gz" "$disk" "$BUILD/corrupt-super.img" "$BUILD/corrupt-pointer.img"; do
    [[ -f "$path" ]] || { echo "Missing $path; build kernel and guest first." >&2; exit 1; }
done
debug=()
cmdline='console=ttyS0 rdinit=/init nokaslr panic=0'
for option in "$@"; do
    case "$option" in
        --debug) debug=(-gdb tcp:127.0.0.1:1234 -S) ;;
        --test) cmdline+=' edufs.test=1' ;;
        *) echo "Usage: $0 [--debug] [--test]" >&2; exit 1 ;;
    esac
done
exec qemu-system-x86_64 -accel tcg -m 512M -smp 1 -nographic -no-reboot \
    -kernel "$KDIR/arch/x86/boot/bzImage" \
    -initrd "$BUILD/initramfs.cpio.gz" \
    -append "$cmdline" \
    -drive "file=$disk,format=raw,if=virtio" \
    -drive "file=$BUILD/corrupt-super.img,format=raw,if=virtio,readonly=on" \
    -drive "file=$BUILD/corrupt-pointer.img,format=raw,if=virtio,readonly=on" "${debug[@]}"
