/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EDUFS_DISK_H
#define EDUFS_DISK_H

/* On-disk fields are little-endian integers, never native C structs. */
#define EDUFS_MAGIC 0x45445546u
#define EDUFS_VERSION 1u
#define EDUFS_BLOCK_SIZE 4096u
#define EDUFS_INODES 128u
#define EDUFS_INODE_SIZE 128u
#define EDUFS_DIRENT_SIZE 64u
#define EDUFS_NAME_MAX 59u
#define EDUFS_DIRECT 12u
#define EDUFS_INDIRECT (EDUFS_BLOCK_SIZE / 4u)
#define EDUFS_MAX_BLOCKS (EDUFS_DIRECT + EDUFS_INDIRECT)
#define EDUFS_MAX_FILE (EDUFS_MAX_BLOCKS * EDUFS_BLOCK_SIZE)
#define EDUFS_BITMAP_BLOCK 1u
#define EDUFS_IMAP_BLOCK 2u
#define EDUFS_ITABLE_BLOCK 3u
#define EDUFS_DIRECTORY_BLOCK 7u
#define EDUFS_DATA_BLOCK 9u
#define EDUFS_MIN_BLOCKS 10u
#define EDUFS_VOLUME_MAX_BLOCKS (EDUFS_BLOCK_SIZE * 8u)

/* Superblock u32 offsets: magic, version, block size, block count, inode count,
 * block bitmap, inode bitmap, inode table, root directory, data start. */
/* Inode offsets: type:u32 @0 (0 unused, 1 file, 2 root), size:u32 @4,
 * direct[12]:u32 @8, indirect:u32 @56; remaining bytes reserved, zero. */
/* Directory entry: inode:u32 @0, NUL-terminated name[60] @4.
 * Slot equals inode number. Slots 0 and 1 are reserved and empty. */
#endif
