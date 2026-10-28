// SPDX-License-Identifier: GPL-2.0-only
/* Small synchronous teaching filesystem. No mmap, journal, or crash recovery. */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/statfs.h>
#include "edufs_disk.h"

struct edufs_sb {
	struct mutex lock;
	u32 blocks;
	struct buffer_head *meta[EDUFS_DATA_BLOCK];
};
static int fail_alloc_after = -1;
module_param(fail_alloc_after, int, 0644);
MODULE_PARM_DESC(fail_alloc_after, "Fail block allocation after N successes; -1 disables");
static bool trace;
module_param(trace, bool, 0644);
MODULE_PARM_DESC(trace, "Log inode and logical block operations");
static const struct inode_operations edufs_dir_iops, edufs_file_iops;
static const struct file_operations edufs_dir_fops, edufs_file_fops;

static u32 rd(const void *p)
{
	const u8 *b = p;
	return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}
static void wr(void *p, u32 v)
{
	u8 *b = p;
	b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}
static u8 *meta(struct super_block *sb, u32 block)
{
	struct edufs_sb *s = sb->s_fs_info;
	return (u8 *)s->meta[block]->b_data;
}
static u8 *record(struct super_block *sb, u32 n)
{
	return meta(sb, EDUFS_ITABLE_BLOCK + n / 32) + (n % 32) * EDUFS_INODE_SIZE;
}
static u8 *entry(struct super_block *sb, u32 n)
{
	return meta(sb, EDUFS_DIRECTORY_BLOCK + n / 64) + (n % 64) * EDUFS_DIRENT_SIZE;
}
static bool used(const u8 *map, u32 n) { return (map[n / 8] >> (n % 8)) & 1; }
static void setused(u8 *map, u32 n, bool value)
{
	if (value) map[n / 8] |= 1U << (n % 8);
	else map[n / 8] &= ~(1U << (n % 8));
}
static bool valid_block(struct super_block *sb, u32 b)
{
	struct edufs_sb *s = sb->s_fs_info;
	return b >= EDUFS_DATA_BLOCK && b < s->blocks && used(meta(sb, 1), b);
}
static void dirty_metadata(struct super_block *sb)
{
	struct edufs_sb *s = sb->s_fs_info;
	u32 b;
	for (b = 1; b < EDUFS_DATA_BLOCK; b++) mark_buffer_dirty(s->meta[b]);
}
static u32 allocate(struct super_block *sb)
{
	struct edufs_sb *s = sb->s_fs_info;
	u32 b;
	if (READ_ONCE(fail_alloc_after) == 0) return 0;
	for (b = EDUFS_DATA_BLOCK; b < s->blocks; b++) {
		if (!used(meta(sb, 1), b)) {
			setused(meta(sb, 1), b, true);
			if (READ_ONCE(fail_alloc_after) > 0) fail_alloc_after--;
			return b;
		}
	}
	return 0;
}
static u32 pointer(u8 *ip, struct buffer_head *ind, u32 j)
{
	return j < EDUFS_DIRECT ? rd(ip + 8 + 4 * j) : rd(ind->b_data + 4 * (j - EDUFS_DIRECT));
}
static void setpointer(u8 *ip, struct buffer_head *ind, u32 j, u32 b)
{
	wr(j < EDUFS_DIRECT ? ip + 8 + 4 * j : (u8 *)ind->b_data + 4 * (j - EDUFS_DIRECT), b);
}

/* Validate ownership before exposing any disk inode to VFS. */
static int edufs_validate(struct super_block *sb)
{
	struct edufs_sb *s = sb->s_fs_info;
	u8 *seen = kzalloc(EDUFS_BLOCK_SIZE, GFP_KERNEL);
	u32 n, j, b;
	int err = -EUCLEAN;
	if (!seen) return -ENOMEM;
	for (b = 0; b < EDUFS_DATA_BLOCK; b++) setused(seen, b, true);
	if (!used(meta(sb, 2), 0) || !used(meta(sb, 2), 1)) goto out;
	if (memchr_inv(record(sb, 0), 0, EDUFS_INODE_SIZE) ||
	    memchr_inv(entry(sb, 0), 0, 2 * EDUFS_DIRENT_SIZE)) goto out;
	if (rd(record(sb, 1)) != 2 || rd(record(sb, 1) + 4) != 8192 ||
	    rd(record(sb, 1) + 8) != 7 || rd(record(sb, 1) + 12) != 8 ||
	    memchr_inv(record(sb, 1) + 16, 0, EDUFS_INODE_SIZE - 16)) goto out;
	for (n = 2; n < EDUFS_INODES; n++) {
		u8 *ip = record(sb, n), *ep = entry(sb, n);
		u32 count, indirect;
		struct buffer_head *ind = NULL;
		if (!used(meta(sb, 2), n)) {
			if (memchr_inv(ip, 0, EDUFS_INODE_SIZE) || memchr_inv(ep, 0, EDUFS_DIRENT_SIZE)) goto out;
			continue;
		}
		if (rd(ip) != 1 || rd(ip + 4) > EDUFS_MAX_FILE || rd(ep) != n ||
		    !ep[4] || !memchr(ep + 4, 0, 60) || strchr(ep + 4, '/') ||
		    !strcmp(ep + 4, ".") || !strcmp(ep + 4, "..") ||
		    memchr_inv(ip + 60, 0, EDUFS_INODE_SIZE - 60)) goto out;
		for (j = 2; j < n; j++)
			if (rd(entry(sb, j)) && !strcmp(entry(sb, j) + 4, ep + 4)) goto out;
		count = DIV_ROUND_UP(rd(ip + 4), EDUFS_BLOCK_SIZE);
		indirect = rd(ip + 56);
		if (count > EDUFS_DIRECT) {
			if (!valid_block(sb, indirect) || used(seen, indirect)) goto out;
			setused(seen, indirect, true);
			ind = sb_bread(sb, indirect);
			if (!ind) { err = -EIO; goto out; }
		} else if (indirect) goto out;
		for (j = 0; j < (ind ? EDUFS_MAX_BLOCKS : EDUFS_DIRECT); j++) {
			b = pointer(ip, ind, j);
			if (j < count) {
				if (!valid_block(sb, b) || used(seen, b)) { brelse(ind); goto out; }
				setused(seen, b, true);
			} else if (b) { brelse(ind); goto out; }
		}
		brelse(ind);
	}
	for (b = 0; b < EDUFS_VOLUME_MAX_BLOCKS; b++)
		if (used(meta(sb, 1), b) != (b < s->blocks ? used(seen, b) : true)) goto out;
	for (n = EDUFS_INODES; n < EDUFS_BLOCK_SIZE * 8; n++) if (!used(meta(sb, 2), n)) goto out;
	err = 0;
out:
	kfree(seen);
	return err;
}

struct edufs_tx {
	u8 inode_before[EDUFS_INODE_SIZE];
	u8 bitmap_before[EDUFS_BLOCK_SIZE];
	u8 indirect_before[EDUFS_BLOCK_SIZE];
	struct buffer_head *data[EDUFS_MAX_BLOCKS];
};

/* All allocations and reads finish before changing existing file contents.
 * On failure restore the inode, bitmap, and existing indirect block. */
static int edufs_update(struct inode *inode, const u8 *input, loff_t pos,
			size_t bytes, loff_t target)
{
	struct super_block *sb = inode->i_sb;
	u8 *ip = record(sb, inode->i_ino);
	u32 old_size = rd(ip + 4), old_count = DIV_ROUND_UP(old_size, EDUFS_BLOCK_SIZE);
	u32 new_count = DIV_ROUND_UP(target, EDUFS_BLOCK_SIZE), j, b, previous_ind = rd(ip + 56);
	struct edufs_tx *tx;
	struct buffer_head *ind = NULL;
	int err = -ENOMEM;

	if (target < 0 || target > EDUFS_MAX_FILE) return -EFBIG;
	tx = kvzalloc(sizeof(*tx), GFP_KERNEL);
	if (!tx) return -ENOMEM;
	memcpy(tx->inode_before, ip, EDUFS_INODE_SIZE);
	memcpy(tx->bitmap_before, meta(sb, 1), EDUFS_BLOCK_SIZE);
	if (previous_ind) {
		if (!valid_block(sb, previous_ind)) { err = -EUCLEAN; goto rollback; }
		ind = sb_bread(sb, previous_ind);
		if (!ind) { err = -EIO; goto rollback; }
		memcpy(tx->indirect_before, ind->b_data, EDUFS_BLOCK_SIZE);
	} else if (new_count > EDUFS_DIRECT) {
		b = allocate(sb);
		if (!b) { err = -ENOSPC; goto rollback; }
		wr(ip + 56, b);
		ind = sb_bread(sb, b);
		if (!ind) { err = -EIO; goto rollback; }
		memset(ind->b_data, 0, EDUFS_BLOCK_SIZE);
	}
	for (j = old_count; j < new_count; j++) {
		b = allocate(sb);
		if (!b) { err = -ENOSPC; goto rollback; }
		setpointer(ip, ind, j, b);
	}
	for (j = 0; j < max(old_count, new_count); j++) {
		b = pointer(ip, ind, j);
		if (!valid_block(sb, b)) { err = -EUCLEAN; goto rollback; }
		tx->data[j] = sb_bread(sb, b);
		if (!tx->data[j]) { err = -EIO; goto rollback; }
	}
	/* No fallible operations beyond this point. Zero gaps and truncated tails. */
	for (j = 0; j < new_count; j++) {
		u32 start = j * EDUFS_BLOCK_SIZE;
		u32 zero_from = min(old_size, (u32)target);
		if (j >= old_count) memset(tx->data[j]->b_data, 0, EDUFS_BLOCK_SIZE);
		else if (zero_from < start + EDUFS_BLOCK_SIZE)
			memset(tx->data[j]->b_data + (zero_from > start ? zero_from - start : 0),
			       0, EDUFS_BLOCK_SIZE - (zero_from > start ? zero_from - start : 0));
		if (input && bytes) {
			loff_t lo = max_t(loff_t, pos, start);
			loff_t hi = min_t(loff_t, pos + bytes, start + EDUFS_BLOCK_SIZE);
			if (hi > lo) memcpy(tx->data[j]->b_data + lo - start, input + lo - pos, hi - lo);
		}
		mark_buffer_dirty(tx->data[j]);
	}
	for (j = new_count; j < old_count; j++) {
		setused(meta(sb, 1), pointer(ip, ind, j), false);
		setpointer(ip, ind, j, 0);
	}
	if (new_count <= EDUFS_DIRECT && ind) {
		setused(meta(sb, 1), rd(ip + 56), false);
		wr(ip + 56, 0);
	} else if (ind) mark_buffer_dirty(ind);
	wr(ip + 4, target);
	i_size_write(inode, target);
	inode->i_blocks = (new_count + (new_count > EDUFS_DIRECT)) * (EDUFS_BLOCK_SIZE / 512);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	dirty_metadata(sb);
	err = 0;
	goto out;
rollback:
	memcpy(ip, tx->inode_before, EDUFS_INODE_SIZE);
	memcpy(meta(sb, 1), tx->bitmap_before, EDUFS_BLOCK_SIZE);
	if (previous_ind && ind) {
		memcpy(ind->b_data, tx->indirect_before, EDUFS_BLOCK_SIZE);
		mark_buffer_dirty(ind);
	}
	/* A prior dirty buffer may have entered writeback during preparation. */
	dirty_metadata(sb);
	if (trace) pr_info("edufs: rollback inode=%lu error=%d\n", inode->i_ino, err);
out:
	for (j = 0; j < EDUFS_MAX_BLOCKS; j++) brelse(tx->data[j]);
	brelse(ind);
	kvfree(tx);
	return err;
}

static struct inode *edufs_iget(struct super_block *sb, u32 n)
{
	struct inode *inode = iget_locked(sb, n);
	u8 *ip = record(sb, n);
	if (!inode) return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW)) return inode;
	inode_init_owner(&nop_mnt_idmap, inode, NULL, n == 1 ? S_IFDIR | 0755 : S_IFREG | 0644);
	simple_inode_init_ts(inode);
	i_size_write(inode, rd(ip + 4));
	inode->i_blocks = (DIV_ROUND_UP(rd(ip + 4), EDUFS_BLOCK_SIZE) + !!rd(ip + 56)) * 8;
	inode->i_op = n == 1 ? &edufs_dir_iops : &edufs_file_iops;
	inode->i_fop = n == 1 ? &edufs_dir_fops : &edufs_file_fops;
	if (n == 1) inc_nlink(inode);
	unlock_new_inode(inode);
	return inode;
}
static struct dentry *edufs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct edufs_sb *s = dir->i_sb->s_fs_info;
	struct inode *found = NULL;
	u32 n;
	if (dentry->d_name.len > EDUFS_NAME_MAX) return ERR_PTR(-ENAMETOOLONG);
	mutex_lock(&s->lock);
	for (n = 2; n < EDUFS_INODES; n++) {
		u8 *ep = entry(dir->i_sb, n);
		if (rd(ep) && strlen(ep + 4) == dentry->d_name.len &&
		    !memcmp(ep + 4, dentry->d_name.name, dentry->d_name.len)) {
			found = edufs_iget(dir->i_sb, n); break;
		}
	}
	mutex_unlock(&s->lock);
	return d_splice_alias(found, dentry);
}
static int edufs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct edufs_sb *s = sb->s_fs_info;
	struct inode *inode;
	u32 n;
	int err = -ENOSPC;
	if (!dentry->d_name.len || dentry->d_name.len > EDUFS_NAME_MAX) return -ENAMETOOLONG;
	mutex_lock(&s->lock);
	for (n = 2; n < EDUFS_INODES && used(meta(sb, 2), n); n++) {}
	if (n == EDUFS_INODES) goto out;
	memset(record(sb, n), 0, EDUFS_INODE_SIZE);
	wr(record(sb, n), 1);
	setused(meta(sb, 2), n, true);
	inode = edufs_iget(sb, n);
	if (IS_ERR(inode)) {
		memset(record(sb, n), 0, EDUFS_INODE_SIZE);
		setused(meta(sb, 2), n, false);
		err = PTR_ERR(inode); goto out;
	}
	inode->i_mode = S_IFREG | (mode & 0777);
	wr(entry(sb, n), n);
	memcpy(entry(sb, n) + 4, dentry->d_name.name, dentry->d_name.len);
	d_instantiate(dentry, inode);
	dirty_metadata(sb);
	err = 0;
out:
	mutex_unlock(&s->lock);
	return err;
}
static int edufs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct edufs_sb *s = dir->i_sb->s_fs_info;
	mutex_lock(&s->lock);
	memset(entry(dir->i_sb, d_inode(dentry)->i_ino), 0, EDUFS_DIRENT_SIZE);
	drop_nlink(d_inode(dentry));
	dirty_metadata(dir->i_sb);
	mutex_unlock(&s->lock);
	return 0;
}
static int edufs_iterate(struct file *file, struct dir_context *ctx)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct edufs_sb *s = sb->s_fs_info;
	if (!dir_emit_dots(file, ctx)) return 0;
	mutex_lock(&s->lock);
	while (ctx->pos < EDUFS_INODES) {
		u8 *ep = entry(sb, ctx->pos);
		if (rd(ep) && !dir_emit(ctx, ep + 4, strlen(ep + 4), rd(ep), DT_REG)) break;
		ctx->pos++;
	}
	mutex_unlock(&s->lock);
	return 0;
}
static ssize_t edufs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct edufs_sb *s = sb->s_fs_info;
	u8 *ip = record(sb, inode->i_ino);
	struct buffer_head *ind = NULL;
	ssize_t done = 0;
	int err = 0;
	if (*pos < 0) return -EINVAL;
	mutex_lock(&s->lock);
	if (*pos >= i_size_read(inode)) goto out;
	count = min_t(size_t, count, i_size_read(inode) - *pos);
	if (rd(ip + 56)) {
		if (!valid_block(sb, rd(ip + 56))) { err = -EUCLEAN; goto out; }
		ind = sb_bread(sb, rd(ip + 56));
		if (!ind) { err = -EIO; goto out; }
	}
	while (count) {
		u32 logical = *pos / EDUFS_BLOCK_SIZE, offset = *pos % EDUFS_BLOCK_SIZE;
		u32 b;
		size_t chunk = min_t(size_t, count, EDUFS_BLOCK_SIZE - offset), missed;
		struct buffer_head *bh;
		if (logical >= EDUFS_MAX_BLOCKS || (logical >= EDUFS_DIRECT && !ind)) { err = -EUCLEAN; break; }
		b = pointer(ip, ind, logical);
		if (!valid_block(sb, b)) { err = -EUCLEAN; break; }
		if (trace) pr_info("edufs: read inode=%lu logical=%u physical=%u\n", inode->i_ino, logical, b);
		bh = sb_bread(sb, b);
		if (!bh) { err = -EIO; break; }
		missed = copy_to_user(buf + done, bh->b_data + offset, chunk);
		brelse(bh);
		done += chunk - missed; *pos += chunk - missed; count -= chunk - missed;
		if (missed) { err = -EFAULT; break; }
	}
out:
	brelse(ind);
	mutex_unlock(&s->lock);
	return done ? done : err;
}
static ssize_t edufs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct inode *inode = file_inode(file);
	struct edufs_sb *s = inode->i_sb->s_fs_info;
	u8 *input;
	int err;
	loff_t start;
	if (!count) return 0;
	if (count > EDUFS_MAX_FILE || *pos < 0) return -EFBIG;
	input = vmemdup_user(buf, count);
	if (IS_ERR(input)) return PTR_ERR(input);
	mutex_lock(&s->lock);
	start = file->f_flags & O_APPEND ? i_size_read(inode) : *pos;
	if (start > EDUFS_MAX_FILE || count > EDUFS_MAX_FILE - start) err = -EFBIG;
	else {
		if (trace) pr_info("edufs: write inode=%lu offset=%lld bytes=%zu\n", inode->i_ino, start, count);
		err = edufs_update(inode, input, start, count, max_t(loff_t, i_size_read(inode), start + count));
	}
	if (!err) *pos = start + count;
	mutex_unlock(&s->lock);
	kvfree(input);
	if (!err && (file->f_flags & (O_SYNC | O_DSYNC))) err = sync_blockdev(inode->i_sb->s_bdev);
	return err ? err : count;
}
static int edufs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct edufs_sb *s = inode->i_sb->s_fs_info;
	int err = setattr_prepare(idmap, dentry, attr);
	if (err) return err;
	mutex_lock(&s->lock);
	if (attr->ia_valid & ATTR_SIZE) err = edufs_update(inode, NULL, 0, 0, attr->ia_size);
	if (!err) setattr_copy(idmap, inode, attr);
	mutex_unlock(&s->lock);
	return err;
}
static int edufs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return sync_blockdev(file_inode(file)->i_sb->s_bdev);
}
static void edufs_evict_inode(struct inode *inode)
{
	struct edufs_sb *s = inode->i_sb->s_fs_info;
	truncate_inode_pages_final(&inode->i_data);
	if (!inode->i_nlink && inode->i_ino >= 2) {
		int err;
		mutex_lock(&s->lock);
		err = edufs_update(inode, NULL, 0, 0, 0);
		if (!err) {
			memset(record(inode->i_sb, inode->i_ino), 0, EDUFS_INODE_SIZE);
			setused(meta(inode->i_sb, 2), inode->i_ino, false);
			dirty_metadata(inode->i_sb);
		} else pr_err("edufs: failed to reclaim inode %lu: %d\n", inode->i_ino, err);
		mutex_unlock(&s->lock);
	}
	clear_inode(inode);
}
static void edufs_put_super(struct super_block *sb)
{
	struct edufs_sb *s = sb->s_fs_info;
	u32 b;
	if (!s) return;
	for (b = 0; b < EDUFS_DATA_BLOCK; b++) brelse(s->meta[b]);
	kfree(s);
}
static int edufs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct edufs_sb *s = sb->s_fs_info;
	u32 b;
	memset(buf, 0, sizeof(*buf));
	buf->f_type = EDUFS_MAGIC; buf->f_bsize = EDUFS_BLOCK_SIZE; buf->f_frsize = EDUFS_BLOCK_SIZE;
	buf->f_blocks = s->blocks - EDUFS_DATA_BLOCK; buf->f_files = EDUFS_INODES - 2;
	buf->f_namelen = EDUFS_NAME_MAX;
	mutex_lock(&s->lock);
	for (b = EDUFS_DATA_BLOCK; b < s->blocks; b++) if (!used(meta(sb, 1), b)) buf->f_bfree++;
	for (b = 2; b < EDUFS_INODES; b++) if (!used(meta(sb, 2), b)) buf->f_ffree++;
	buf->f_bavail = buf->f_bfree;
	mutex_unlock(&s->lock);
	return 0;
}
static const struct inode_operations edufs_dir_iops = {
	.lookup = edufs_lookup, .create = edufs_create, .unlink = edufs_unlink,
};
static const struct inode_operations edufs_file_iops = { .setattr = edufs_setattr };
static const struct file_operations edufs_dir_fops = {
	.owner = THIS_MODULE, .llseek = generic_file_llseek,
	.read = generic_read_dir, .iterate_shared = edufs_iterate, .fsync = edufs_fsync,
};
static const struct file_operations edufs_file_fops = {
	.owner = THIS_MODULE, .llseek = generic_file_llseek,
	.read = edufs_read, .write = edufs_write, .fsync = edufs_fsync,
};
static const struct super_operations edufs_super_ops = {
	.statfs = edufs_statfs, .evict_inode = edufs_evict_inode, .put_super = edufs_put_super,
};
static int edufs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct edufs_sb *s;
	struct inode *root;
	u32 b, expected[] = { EDUFS_MAGIC, EDUFS_VERSION, EDUFS_BLOCK_SIZE, 0, EDUFS_INODES, 1, 2, 3, 7, 9 };
	int err = -EINVAL;
	if (!sb_set_blocksize(sb, EDUFS_BLOCK_SIZE)) return -EINVAL;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) return -ENOMEM;
	mutex_init(&s->lock); sb->s_fs_info = s;
	for (b = 0; b < EDUFS_DATA_BLOCK; b++) {
		s->meta[b] = sb_bread(sb, b);
		if (!s->meta[b]) { err = -EIO; goto fail; }
	}
	s->blocks = rd(meta(sb, 0) + 12); expected[3] = s->blocks;
	for (b = 0; b < ARRAY_SIZE(expected); b++) if (rd(meta(sb, 0) + b * 4) != expected[b]) goto fail;
	if (s->blocks < EDUFS_MIN_BLOCKS || s->blocks > EDUFS_VOLUME_MAX_BLOCKS ||
	    bdev_nr_bytes(sb->s_bdev) != (u64)s->blocks * EDUFS_BLOCK_SIZE) goto fail;
	err = edufs_validate(sb);
	if (err) goto fail;
	sb->s_magic = EDUFS_MAGIC; sb->s_maxbytes = EDUFS_MAX_FILE;
	sb->s_op = &edufs_super_ops; sb->s_time_gran = 1;
	root = edufs_iget(sb, 1);
	if (IS_ERR(root)) { err = PTR_ERR(root); goto fail; }
	sb->s_root = d_make_root(root);
	if (!sb->s_root) { err = -ENOMEM; goto fail; }
	return 0;
fail:
	for (b = 0; b < EDUFS_DATA_BLOCK; b++) brelse(s->meta[b]);
	kfree(s); sb->s_fs_info = NULL;
	return err;
}
static struct dentry *edufs_mount(struct file_system_type *type, int flags, const char *device, void *data)
{
	return mount_bdev(type, flags, device, data, edufs_fill_super);
}
static struct file_system_type edufs_type = {
	.owner = THIS_MODULE, .name = "edufs", .mount = edufs_mount,
	.kill_sb = kill_block_super, .fs_flags = FS_REQUIRES_DEV,
};
static int __init edufs_init(void) { return register_filesystem(&edufs_type); }
static void __exit edufs_exit(void) { unregister_filesystem(&edufs_type); }
module_init(edufs_init);
module_exit(edufs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Educational disk filesystem with indirect blocks and allocation rollback");
