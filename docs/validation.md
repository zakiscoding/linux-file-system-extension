# Validation record

The portable C image tool was compiled on Windows using the installed
LLVM-MinGW compiler with C11, optimization, and `-Wall -Wextra -Werror -pedantic`.
The standard-library Python integration suite passed all eight test methods.
Run `powershell -File scripts/test-windows.ps1` to reproduce on Windows with
gcc and Python on PATH, or `make test` on Linux.

The tests cover zero-length files, partial blocks, exact block boundaries,
49,152-byte direct mapping, its first indirect byte, and the 4,243,456-byte
maximum. Oversized imports must preserve the original image.

Disk-full tests exercise failed replacement of an existing file and failure
after allocating an indirect block and twelve data blocks. They compare the
entire image before and after the failure and then check allocation consistency.
Deleting another file and retrying verifies block reuse. A separate test fills
all 126 file inodes, rejects another import, then frees and reuses a slot.

The randomized test executes 160 operations with seed 4523 and up to twelve
files. After each operation it launches a fresh checker process and exports all
remaining files into a Python reference model comparison. This tests reopening
images, not kernel remounts.

Corruption fixtures include bad magic/version/volume size, pointers into metadata,
out-of-range blocks, duplicate data ownership, indirect corruption, impossible
file length, missing indirect metadata, bitmap corruption, and truncated images.
Invalid images must be rejected before an attempted mutation changes them.

## Linux and QEMU validation — October 28, 2026

Ubuntu WSL2 was installed and initialized. The pinned Linux 6.12.114 kernel and
EduFS module compiled using Ubuntu GCC 15.2.0. QEMU 10.2.1 booted the guest using
TCG emulation with 512 MiB RAM and one CPU. All eight offline integration tests
also passed with the Linux compiler.

The mounted C test program exercises ten boundary lengths from zero through
4,243,456 bytes, fsyncs and remounts each case, and rejects a write beyond the
maximum. Allocation injection fails at zero, one, and two successful allocations
while extending a twelve-block file across the indirect boundary. Every failure
preserves its bytes, size, and available block count, including after remount.

The VFS randomized test uses xorshift32 seed 4523 for 160 operations across eight
files: positional writes, grow/shrink truncation, reads, deletion, and periodic
remounts. It compares every live file against a user-space byte model after each
operation. A separate test checks reading an open-unlinked file and block reuse
after the last close. Additional checks fill all 126 file inodes, reuse a freed
inode, verify append behavior, and reject an invalid user pointer with EFAULT.

Actual disk-full testing writes 1 MiB chunks into files until ENOSPC on a 64 MiB
volume. A failed allocation leaves the free-block count unchanged. Deleting all
fill files restores the original count; remount and the offline checker pass.
Two read-only corrupted guest disks are rejected at mount: bad superblock with
EINVAL, and an invalid data pointer with EUCLEAN. No kernel panic or oops was
observed in passing runs.

## GDB evidence

The automated debugger loads module symbols and stops at `edufs_update` during
a real `pwrite64` extending the file from 49,152 to 57,344 bytes. The stack passes
through `edufs_write`, `vfs_write`, and the syscall entry path. With allocation
injection set to 1, GDB observes a return value of -28 (ENOSPC), and inode size
remains 49,152 bytes. The guest then completes the remaining tests and powers off.
This demonstrates inspecting a real kernel allocation failure, not a crash.

Reproduce with `bash scripts/test-qemu.sh` and `bash scripts/test-gdb.sh`, or use
the PowerShell WSL wrapper documented in the README. Each automated run creates
a fresh disposable image, so interruption cannot contaminate a later test.
The Windows wrapper copies logs to `build/linux-evidence/`. Logs include guest
PASS markers and the GDB stack/values; the runner checks the offline image after
shutdown. The eight offline tests also passed locally under GCC address and
undefined-behavior sanitizers; the result is saved in `sanitizer-test.log`.

Limits: only clean remount persistence, serialized operations, and allocation
failure rollback were tested. Power-loss recovery, concurrent workloads, and
device-write-error recovery are not established. Version 1 does not persist
permissions, ownership, or timestamps.
