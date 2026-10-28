# EduFS — educational Linux file system

A C Linux kernel module implementing a small disk-backed file system with
twelve direct block pointers, a single-indirect block, allocation rollback,
and a flat root directory. Built and tested on **Linux 6.12.114** in QEMU,
with GDB inspection of the syscall-to-allocation path.

## Verified behavior

- Create, list, read, write, append, seek, truncate, fsync, and unlink regular files.
- Zero-fill file extensions and gaps; reclaim data and indirect blocks on shrink
  or deletion; preserve an open file until its last close after unlink.
- Reject files larger than 4,243,456 bytes and invalid block references.
- Roll back inode pointers, the allocation bitmap, and existing indirect metadata
  when a block allocation or preparatory buffer read fails.
- Reject inconsistent images at mount, including duplicate block ownership,
  leaked allocations, malformed directories, and out-of-range pointers.

Eight offline integration tests pass on Windows and Ubuntu. The QEMU suite passes
boundary tests, 160 seeded VFS operations, clean remount persistence, allocation
failure injection, actual disk exhaustion, inode exhaustion/reuse, append, and
invalid-user-buffer checks. GDB captures a real `pwrite64` stack and verifies that
an injected indirect-allocation failure returns `-ENOSPC` without changing size.
See [validation](docs/validation.md) and [disk format](docs/disk-format.md).

## Run from Windows

Ubuntu WSL2 and the required compiler, QEMU, GDB, and BusyBox dependencies are
installed on the development machine. From this repository in PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/wsl-run.ps1 -Task All
```

The execution-policy option applies only to that process. The wrapper copies
sources to `/root/projects/edufs` in Ubuntu, preserves Linux build products there,
and copies test logs back to `build/linux-evidence/` in this Windows workspace.
`All` runs offline tests, builds the kernel and guest, runs QEMU tests, and runs
GDB rollback inspection. Use `-Task Test`, `Build`, `Qemu`, or `Gdb` for one stage.
Qemu and Gdb rebuild the guest module; Build or All must build the kernel first.
The wrapper uses Ubuntu's root account to avoid an interactive first-run setup.

## Run from Linux

Build on the Linux filesystem; kernel builds under `/mnt/c` or OneDrive are slow.
Copy or clone this source into a Linux directory, then run:

```sh
bash scripts/setup-linux.sh
make test
JOBS=8 bash scripts/build-kernel.sh
bash scripts/build-guest.sh
bash scripts/test-qemu.sh
bash scripts/test-gdb.sh
```

Allow several GiB for the kernel build. Reduce JOBS if memory is limited.
The kernel download is checked against kernel.org's SHA-256 list over HTTPS;
detached-signature verification is not implemented. CI currently runs offline
tests and sanitizers; the kernel/QEMU/GDB workflow is validated locally.

Automated QEMU and GDB runs create a fresh 64 MiB disposable image each time.
They retain those images under `build/test-run.*` and `build/gdb-run.*` for
inspection. The guest also receives two read-only corrupted test images.
Inspect `build/qemu-test.log`, `build/gdb-guest.log`, and `build/gdb-rollback.log`.

## Interactive guest and debugger

```sh
bash scripts/run-qemu.sh
```

After the module reload smoke test, the guest opens a shell. Run:

```sh
insmod /modules/edufs.ko trace=1
mount -t edufs /dev/vda /mnt
echo hello > /mnt/example
cat /mnt/example
umount /mnt
poweroff -f
```

Exit QEMU with Ctrl-a then x if necessary. `build/disk.img` is the persistent
interactive disk. Automated tests use separate disposable images.

For manual debugging, start `bash scripts/run-qemu.sh --debug`, then run
`bash scripts/debug.sh` in another terminal. QEMU waits on loopback port 1234.
Continue to `start_kernel`, then continue again to boot. With the module loaded,
interrupt GDB and run `lx-symbols ../module`, then set `break edufs_update`.
Perform a write in the guest to inspect inode pointers and the VFS stack.

Set `/sys/module/edufs/parameters/fail_alloc_after` to 0 to fail the next block
allocation, N to fail after N successful allocations, or -1 to disable injection.
This fault injection affects block allocation, not storage-device I/O errors.

## Offline tools

```sh
make
build/edufs mkfs build/example.img 16384
build/edufs put build/example.img hello README.md
build/edufs ls build/example.img
build/edufs get build/example.img hello build/exported.txt
build/edufs check build/example.img
build/edufs rm build/example.img hello
```

On Windows, `scripts/test-windows.ps1` compiles and tests `build/edufs.exe` using
gcc and Python. Run it with the same PowerShell process execution-policy option
shown above if your script policy requires it.

Formatting and export refuse to overwrite existing files. Offline tools require
exclusive access to unmounted images. The checker detects allocation/metadata
inconsistency; file-content integrity requires comparison with a reference copy.

## Scope and limitations

This teaching implementation supports 126 regular files in one directory, on
volumes up to 128 MiB. A superblock mutex serializes operations. The tests use one
QEMU CPU; concurrent workloads have not been qualified. File I/O uses buffer heads
directly, without mmap, direct I/O, hard links, rename, or nested directories.

Modes, ownership, and timestamps are initialized in memory and are not persisted
in version 1. There is no journal or recovery for interrupted metadata updates.
Allocation rollback finishes before changing existing file content; persistence
is guaranteed by the tested clean fsync/unmount path, not by crash recovery.
Storage write failures can leave inconsistent images and are outside the tested
rollback contract. No kernel crash was observed in the passing tests; the GDB
evidence demonstrates failure-path inspection, not diagnosis of a kernel crash.

## References

- [Linux kernel releases](https://www.kernel.org/)
- [External module build interface](https://docs.kernel.org/kbuild/modules.html)
- [QEMU GDB stub](https://www.qemu.org/docs/master/system/gdb.html)

C sources use GPL-2.0-only as specified by their SPDX headers.
