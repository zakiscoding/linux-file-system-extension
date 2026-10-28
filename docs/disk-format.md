# EduFS image format, version 1

All integers are unsigned 32-bit little-endian. Blocks are 4096 bytes. Tools
encode fields explicitly, so host struct padding and byte order cannot change
the image format. An image contains 10–32768 blocks (40 KiB–128 MiB).

Block 0 contains the superblock. Its first ten integers are magic `0x45445546`,
version `1`, block size `4096`, total block count, inode count `128`, block bitmap
location `1`, inode bitmap location `2`, inode-table start `3`, directory start
`7`, and data-region start `9`. Remaining bytes are reserved.

Block 1 is the block bitmap. Block 2 is the inode bitmap. Bit n is bit n modulo
8 in byte n divided by 8, with the least-significant bit first. Set bits mean
allocated. Metadata blocks and bitmap padding beyond the usable range are set.

Blocks 3–6 contain 128 inodes, each 128 bytes. Inode 0 is reserved and zero;
inode 1 is the root directory; inodes 2–127 hold regular files. Each inode has
type at byte 0 (0 unused, 1 regular file, 2 root), byte size at byte 4, twelve
direct block pointers at bytes 8–55, and a single-indirect block pointer at byte
56. Bytes 60–127 are reserved and zero. Unused inodes are entirely zero.

Blocks 7–8 hold 128 directory entries, each 64 bytes. Slot n names inode n.
Slots 0 and 1 are empty; the kernel synthesizes dot entries. Each allocated
entry contains its inode number followed by a 60-byte NUL-terminated name.
Names contain 1–59 bytes, cannot contain `/`, and cannot be `.` or `..`.
An unused entry is entirely zero. The directory supports 126 files.

The root inode has type 2, size 8192, direct pointers 7 and 8, and otherwise
zero pointers. Data allocation starts at block 9. A file's first twelve logical
blocks use direct pointers. A larger file uses an additional metadata block
containing 1024 data-block pointers. Pointer zero means unused; sparse files
are not supported. Every block within a file's size must exist. Pointers after
the end of the file must be zero. The largest file is 4,243,456 bytes:
`(12 + 4096 / 4) * 4096`. The direct-to-indirect boundary is 49,152 bytes.

The checker reconstructs block ownership from the inode table and compares it
with the allocation bitmap. Duplicate ownership, dangling references, leaked
blocks, invalid lengths, malformed names, and orphan directory entries fail.
It is an allocation-consistency checker, not a data-integrity checksum: a
changed byte in valid file content is detected only by comparison with a
reference copy.

## Kernel allocation transaction

The kernel keeps references to the nine metadata buffer heads for a mounted
volume. Its superblock mutex serializes file and directory operations. Before
extending or truncating a file, it saves the inode, allocation bitmap, and any
existing indirect block. It allocates required blocks and acquires all data
buffers before modifying existing file content. Failure restores those saved
metadata values and releases all buffer references. Only after preparation
succeeds does it zero gaps/tails, copy data, update size, and dirty buffers.

An unlinked open inode retains its blocks until eviction on the last close.
Clean unmount flushes metadata and data through the block layer. This sequence
does not provide atomicity against power loss or block-device write errors.

## Offline transaction behavior

The offline tool loads and validates the entire image before modifying it.
Allocation, replacement, and reclamation happen in that private memory image.
No disk image changes are written until all allocations and input reads succeed
and the resulting image passes the checker. On allocation failure, discarding
the private image provides operation rollback, including replacement of an old
file. This differs from the kernel allocator's explicit metadata snapshots.

The final image write is in place. An interrupted write, storage error, or power
loss can leave a partial image; this format has no journal or crash-recovery
guarantee. Tools require exclusive offline access and must never run against a
mounted or concurrently edited image.
