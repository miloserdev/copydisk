// SPDX-License-Identifier: GPL-2.0-only
/*
 * extshrink.c - builds a shrunken copy of an ext2/3/4 filesystem via libext2fs.
 *
 *  1. ext_estimate()   - scans source inodes, computes how much is really used;
 *  2. ext_plan_blocks()- decides how large the new filesystem should be;
 *  3. ext_shrink_copy()- "mke2fs" (initialize + allocate_tables + journal) and "rsync -aHAX"
 *                        (recursive copy of inodes/data/xattr/ACL/links).
 *
 * Nothing is mounted: both filesystems are opened directly as "file?offset=N".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <ext2fs/ext2_err.h>
#include <ext2fs/ext2fs.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "copydisk.h"

/* inode flags that are carried over (the rest - extents/index/compr/... - are decided by the fs itself) */
#define KEEP_FLAGS (0x00000010 /*IMMUTABLE*/ | 0x00000020 /*APPEND*/ | 0x00000040 /*NODUMP*/ | \
		    0x00000080 /*NOATIME*/ | 0x00000008 /*SYNC*/ | 0x00010000 /*DIRSYNC*/ | \
		    0x00800000 /*NOCOW*/ | 0x20000000 /*PROJINHERIT*/ | 0x00000800 /*ENCRYPT*/ | \
		    0x40000000 /*CASEFOLD*/)

#define COPY_CHUNK (1024 * 1024)

static int errcode_to_rc(errcode_t e)
{
	switch (e) {
	case EXT2_ET_BLOCK_ALLOC_FAIL:
	case EXT2_ET_INODE_ALLOC_FAIL:
	case EXT2_ET_EA_NO_SPACE:
		return RC_NOSPACE;
	default:
		return RC_ERR;
	}
}

static errcode_t open_fs(const char *path, uint64_t off, ext2_filsys *fs)
{
	char opt[64];
	snprintf(opt, sizeof(opt), "offset=%llu", (unsigned long long)off);
	return ext2fs_open2(path, opt, EXT2_FLAG_64BITS, 0, 0, unix_io_manager, fs);
}

/* ---------------------------------------------------------------- estimate */

static int check_source_features(ext2_filsys fs, int force)
{
	struct ext2_super_block *sb = fs->super;

	if (ext2fs_has_feature_inline_data(sb)) {
		log_err("filesystems with inline_data are not supported");
		return RC_ERR;
	}
	if (ext2fs_has_feature_bigalloc(sb)) {
		log_err("filesystems with bigalloc are not supported");
		return RC_ERR;
	}
	if (ext2fs_has_feature_mmp(sb)) {
		log_err("filesystems with MMP are not supported");
		return RC_ERR;
	}
	if (ext2fs_has_feature_journal_dev(sb)) {
		log_err("this is an external journal device, not a filesystem");
		return RC_ERR;
	}
	if (ext2fs_has_feature_journal_needs_recovery(sb)) {
		if (!force) {
			log_err("source journal was not replayed (filesystem was not cleanly unmounted) - "
				"pending transactions would be missing from the copy. Unmount/replay the "
				"journal (e2fsck) or pass --force");
			return RC_ERR;
		}
		log_warn("source journal not replayed; continuing because of --force");
	}
	if (!(sb->s_state & EXT2_VALID_FS))
		log_warn("source is marked as not clean (s_state=%u)", sb->s_state);
	if (ext2fs_has_feature_quota(sb) || ext2fs_has_feature_project(sb))
		log_warn("quota/project are disabled in the copy (quota files are not carried over); "
			 "after restoring: tune2fs -O quota ... && quotacheck");
	if (ext2fs_has_feature_orphan_file(sb))
		log_warn("orphan_file feature is not enabled in the copy (the kernel allows that)");
	return RC_OK;
}

int ext_estimate(const char *dev, uint64_t off, struct ext_est *e, int force)
{
	ext2_filsys fs;
	ext2_inode_scan scan;
	struct ext2_inode in;
	ext2_ino_t ino;
	errcode_t err;
	uint64_t sectors = 0;

	memset(e, 0, sizeof(*e));
	initialize_ext2_error_table();	/* so that error_message() knows the ext2 error texts */
	err = open_fs(dev, off, &fs);
	if (err) {
		log_err("cannot open ext filesystem at %s+%llu: %s", dev,
			(unsigned long long)off, error_message(err));
		return RC_ERR;
	}
	if (check_source_features(fs, force) != RC_OK) {
		ext2fs_close_free(&fs);
		return RC_ERR;
	}
	e->bs = fs->blocksize;
	e->src_blocks = ext2fs_blocks_count(fs->super);
	e->inode_size = EXT2_INODE_SIZE(fs->super);
	e->blocks_per_group = fs->super->s_blocks_per_group;
	e->gdt_entry = EXT2_DESC_SIZE(fs->super);

	err = ext2fs_open_inode_scan(fs, 0, &scan);
	if (err) {
		log_err("inode scan: %s", error_message(err));
		ext2fs_close_free(&fs);
		return RC_ERR;
	}
	for (;;) {
		err = ext2fs_get_next_inode(scan, &ino, &in);
		if (err == EXT2_ET_BAD_BLOCK_IN_INODE_TABLE)
			continue;
		if (err) {
			log_err("reading inode: %s", error_message(err));
			ext2fs_close_inode_scan(scan);
			ext2fs_close_free(&fs);
			return RC_ERR;
		}
		if (!ino)
			break;
		if (in.i_links_count == 0 || in.i_mode == 0)
			continue;
		if (ino == EXT2_JOURNAL_INO) {
			e->journal_blocks = EXT2_I_SIZE(&in) / e->bs;
			continue;
		}
		if (ino < EXT2_FIRST_INODE(fs->super) && ino != EXT2_ROOT_INO)
			continue;
		e->used_inodes++;
		sectors += ext2fs_get_stat_i_blocks(fs, &in);
	}
	ext2fs_close_inode_scan(scan);
	e->data_blocks = (sectors * 512 + e->bs - 1) / e->bs;
	if (!ext2fs_has_feature_journal(fs->super))
		e->journal_blocks = 0;
	ext2fs_close_free(&fs);
	return RC_OK;
}

static uint64_t inodes_wanted(const struct ext_est *e)
{
	uint64_t extra = e->used_inodes / 8;
	return e->used_inodes + (extra < 2048 ? 2048 : extra);
}

/* whether group g holds a superblock backup (classic sparse_super) */
static int group_has_backup(uint64_t g)
{
	uint64_t b;
	if (g <= 1)
		return 1;
	if (!(g & 1))
		return 0;
	for (b = 3; b <= 7; b += 2) {
		uint64_t x = b;
		while (x < g)
			x *= b;
		if (x == g)
			return 1;
	}
	return 0;
}

uint64_t ext_plan_blocks(const struct ext_est *e, uint64_t margin_mib)
{
	uint64_t inodes = inodes_wanted(e);
	uint64_t itable = (inodes * e->inode_size + e->bs - 1) / e->bs;
	uint64_t base = e->data_blocks + e->journal_blocks + itable;
	uint64_t margin = margin_mib * MIB / e->bs;
	uint64_t align = MIB / e->bs;
	uint64_t S = base + margin;
	int it;

	for (it = 0; it < 6; it++) {
		uint64_t groups = (S + e->blocks_per_group - 1) / e->blocks_per_group;
		uint64_t gdt = (groups * e->gdt_entry + e->bs - 1) / e->bs;
		uint64_t backups = 0, g, overhead, ns;

		for (g = 0; g < groups; g++)
			backups += group_has_backup(g);
		/* bitmaps + superblock/GDT/reserved GDT in all copies */
		overhead = groups * 2 + backups * (1 + gdt + e->bs / 4);
		ns = (base + overhead) + (base + overhead) / 10 + margin;
		if (ns < inodes)	/* inodes_per_group <= blocks_per_group */
			ns = inodes;
		ns = (ns + align - 1) / align * align;
		if (ns == S)
			break;
		S = ns;
	}
	return S;
}

/* ------------------------------------------------------------ copying */

struct cx {
	ext2_filsys src, dst;
	ext2_ino_t *hl;		/* src ino -> dst ino for hardlinks */
	unsigned char *buf;
	errcode_t err;
	int rc;
	uint64_t n_files, n_dirs, n_links, n_special, n_hard, n_xattr, bytes;
	char errpath[PATH_MAX];
	time_t now;
};

struct frame {
	struct cx *c;
	ext2_ino_t dst_dir;
	const char *path;
};

static int is_zero_blk(const unsigned char *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

static int ft_of(unsigned mode)
{
	if (LINUX_S_ISREG(mode)) return EXT2_FT_REG_FILE;
	if (LINUX_S_ISDIR(mode)) return EXT2_FT_DIR;
	if (LINUX_S_ISCHR(mode)) return EXT2_FT_CHRDEV;
	if (LINUX_S_ISBLK(mode)) return EXT2_FT_BLKDEV;
	if (LINUX_S_ISLNK(mode)) return EXT2_FT_SYMLINK;
	if (LINUX_S_ISFIFO(mode)) return EXT2_FT_FIFO;
	if (LINUX_S_ISSOCK(mode)) return EXT2_FT_SOCK;
	return EXT2_FT_UNKNOWN;
}

static void fail(struct cx *c, errcode_t e, const char *path, const char *what)
{
	if (c->err)
		return;
	c->err = e;
	c->rc = errcode_to_rc(e);
	snprintf(c->errpath, sizeof(c->errpath), "%s (%s)", path, what);
}

/* read the whole inode (including extra fields) */
static errcode_t rd_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode_large *in)
{
	size_t sz = EXT2_INODE_SIZE(fs->super);
	memset(in, 0, sizeof(*in));
	return ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)in,
				      sz < sizeof(*in) ? sz : sizeof(*in));
}

static errcode_t wr_inode(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode_large *in)
{
	size_t sz = EXT2_INODE_SIZE(fs->super);
	return ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)in,
				       sz < sizeof(*in) ? sz : sizeof(*in));
}

#define HAS_EXTRA(fs, in, f) \
	((size_t)EXT2_INODE_SIZE((fs)->super) > EXT2_GOOD_OLD_INODE_SIZE && \
	 (size_t)EXT2_GOOD_OLD_INODE_SIZE + (in)->i_extra_isize >= offsetof(struct ext2_inode_large, f) + sizeof((in)->f) && \
	 offsetof(struct ext2_inode_large, f) + sizeof((in)->f) <= (size_t)EXT2_INODE_SIZE((fs)->super))

/* Carries over owner, mode, timestamps (including nanoseconds and crtime) and flags. */
static errcode_t fix_meta(struct cx *c, ext2_ino_t sino, ext2_ino_t dino)
{
	struct ext2_inode_large s, d;
	errcode_t e;

	if ((e = rd_inode(c->src, sino, &s)) || (e = rd_inode(c->dst, dino, &d)))
		return e;
	d.i_mode = s.i_mode;
	d.i_uid = s.i_uid;
	d.i_gid = s.i_gid;
	d.osd2.linux2.l_i_uid_high = s.osd2.linux2.l_i_uid_high;
	d.osd2.linux2.l_i_gid_high = s.osd2.linux2.l_i_gid_high;
	d.i_atime = s.i_atime;
	d.i_mtime = s.i_mtime;
	d.i_ctime = s.i_ctime;
	d.i_dtime = 0;
	d.i_generation = s.i_generation;
	d.i_flags = (d.i_flags & ~KEEP_FLAGS) | (s.i_flags & KEEP_FLAGS);
#define COPY_X(f) do { if (HAS_EXTRA(c->src, &s, f) && HAS_EXTRA(c->dst, &d, f)) d.f = s.f; } while (0)
	COPY_X(i_ctime_extra);
	COPY_X(i_mtime_extra);
	COPY_X(i_atime_extra);
	COPY_X(i_crtime);
	COPY_X(i_crtime_extra);
	if (ext2fs_has_feature_project(c->dst->super))
		COPY_X(i_projid);
#undef COPY_X
	return wr_inode(c->dst, dino, &d);
}

struct xnames {
	char **v;
	size_t n, cap;
};

static int xattr_collect(char *name, char *value, size_t len, void *data)
{
	struct xnames *x = data;
	(void)value; (void)len;
	if (x->n == x->cap) {
		x->cap = x->cap ? x->cap * 2 : 8;
		x->v = realloc(x->v, x->cap * sizeof(char *));
	}
	x->v[x->n++] = strdup(name);
	return 0;
}

/* xattr / POSIX ACL / security.capability — via get/set (the library converts ACLs itself) */
static errcode_t copy_xattrs(struct cx *c, ext2_ino_t sino, ext2_ino_t dino)
{
	struct ext2_xattr_handle *sh = NULL, *dh = NULL;
	struct xnames xn = { 0 };
	errcode_t e;
	size_t i, cnt = 0;

	e = ext2fs_xattrs_open(c->src, sino, &sh);
	if (e)
		return e;
	if ((e = ext2fs_xattrs_read(sh)) || (e = ext2fs_xattrs_count(sh, &cnt)))
		goto out;
	if (!cnt)
		goto out;
	if ((e = ext2fs_xattrs_iterate(sh, xattr_collect, &xn)))
		goto out;
	if ((e = ext2fs_xattrs_open(c->dst, dino, &dh)))
		goto out;
	for (i = 0; i < xn.n; i++) {
		void *val = NULL;
		size_t vl = 0;

		e = ext2fs_xattr_get(sh, xn.v[i], &val, &vl);
		if (!e) {
			e = ext2fs_xattr_set(dh, xn.v[i], val, vl);
			ext2fs_free_mem(&val);
		}
		if (e)
			goto out;
		c->n_xattr++;
	}
out:
	for (i = 0; i < xn.n; i++)
		free(xn.v[i]);
	free(xn.v);
	if (dh)
		ext2fs_xattrs_close(&dh);
	if (sh)
		ext2fs_xattrs_close(&sh);
	return e;
}

/* ext2fs_mkdir/ext2fs_symlink do not grow a full parent directory: expand it and retry */
static errcode_t mkdir_retry(struct cx *c, ext2_ino_t parent, ext2_ino_t ino, const char *name)
{
	errcode_t e = ext2fs_mkdir(c->dst, parent, ino, name);
	if (e == EXT2_ET_DIR_NO_SPACE && !(e = ext2fs_expand_dir(c->dst, parent)))
		e = ext2fs_mkdir(c->dst, parent, ino, name);
	return e;
}

static errcode_t symlink_retry(struct cx *c, ext2_ino_t parent, const char *name, const char *target)
{
	errcode_t e = ext2fs_symlink(c->dst, parent, 0, name, target);
	if (e == EXT2_ET_DIR_NO_SPACE && !(e = ext2fs_expand_dir(c->dst, parent)))
		e = ext2fs_symlink(c->dst, parent, 0, name, target);
	return e;
}

static errcode_t link_entry(struct cx *c, ext2_ino_t dir, const char *name, ext2_ino_t ino, int ft)
{
	return ext2fs_link(c->dst, dir, name, ino, ft | EXT2FS_LINK_EXPAND);
}

/* file contents; zero blocks are skipped -> holes, as in a sparse original */
static errcode_t copy_data(struct cx *c, ext2_ino_t sino, ext2_ino_t dino, uint64_t size)
{
	ext2_file_t sf = NULL, df = NULL;
	unsigned bs = c->dst->blocksize;
	uint64_t pos = 0;
	errcode_t e;

	if ((e = ext2fs_file_open(c->src, sino, 0, &sf)))
		return e;
	if ((e = ext2fs_file_open(c->dst, dino, EXT2_FILE_WRITE, &df)))
		goto out;
	while (pos < size) {
		unsigned want = (size - pos > COPY_CHUNK) ? COPY_CHUNK : (unsigned)(size - pos);
		unsigned got = 0, o = 0;

		if ((e = ext2fs_file_read(sf, c->buf, want, &got)))
			goto out;
		if (!got)
			break;
		while (o < got) {
			unsigned b = (got - o > bs) ? bs : got - o, end, w = 0;

			if (is_zero_blk(c->buf + o, b)) {
				o += b;
				continue;
			}
			end = o + b;
			while (end < got) {
				unsigned bb = (got - end > bs) ? bs : got - end;
				if (is_zero_blk(c->buf + end, bb))
					break;
				end += bb;
			}
			if ((e = ext2fs_file_llseek(df, pos + o, EXT2_SEEK_SET, NULL)))
				goto out;
			if ((e = ext2fs_file_write(df, c->buf + o, end - o, &w)))
				goto out;
			o = end;
		}
		pos += got;
	}
	e = ext2fs_file_set_size2(df, size);
	c->bytes += size;
out:
	if (df) {
		errcode_t e2 = ext2fs_file_close(df);
		if (!e)
			e = e2;
	}
	if (sf)
		ext2fs_file_close(sf);
	return e;
}

static int copy_dir_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *de, int offset,
		       int blocksize, char *buf, void *priv);

static errcode_t copy_tree(struct cx *c, ext2_ino_t sdir, ext2_ino_t ddir, const char *path)
{
	struct frame f = { c, ddir, path };
	errcode_t e = ext2fs_dir_iterate2(c->src, sdir, 0, NULL, copy_dir_cb, &f);
	if (e && !c->err)
		fail(c, e, path, "reading directory");
	return c->err;
}

static void progress(struct cx *c)
{
	uint64_t n = c->n_files + c->n_dirs + c->n_links + c->n_special + c->n_hard;
	if (!g_verbose && n && n % 20000 == 0) {
		char b[32];
		log_info("  ... %llu objects, %s of data", (unsigned long long)n,
			 human(c->bytes, b, sizeof(b)));
	}
}

static int copy_dir_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry *de, int offset,
		       int blocksize, char *buf, void *priv)
{
	struct frame *f = priv;
	struct cx *c = f->c;
	ext2_ino_t sino = de->inode, dino = 0;
	struct ext2_inode_large si;
	char name[EXT2_NAME_LEN + 1];
	char *path = NULL;
	unsigned mode;
	int nl = ext2fs_dirent_name_len(de), ft;
	errcode_t e;

	(void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
	if (c->err)
		return DIRENT_ABORT;
	memcpy(name, de->name, nl);
	name[nl] = 0;
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return 0;
	if (asprintf(&path, "%s%s%s", f->path, f->path[1] ? "/" : "", name) < 0)
		return DIRENT_ABORT;

	if ((e = rd_inode(c->src, sino, &si))) {
		fail(c, e, path, "reading inode");
		goto abort;
	}
	mode = si.i_mode;
	ft = ft_of(mode);
	log_vrb("%s", path);

	/* hardlink to an already copied inode */
	if (!LINUX_S_ISDIR(mode) && si.i_links_count > 1 && c->hl[sino]) {
		struct ext2_inode_large di;

		dino = c->hl[sino];
		if ((e = link_entry(c, f->dst_dir, name, dino, ft)) ||
		    (e = rd_inode(c->dst, dino, &di))) {
			fail(c, e, path, "hardlink");
			goto abort;
		}
		di.i_links_count++;
		if ((e = wr_inode(c->dst, dino, &di))) {
			fail(c, e, path, "hardlink links_count");
			goto abort;
		}
		c->n_hard++;
		goto done;
	}

	if (LINUX_S_ISDIR(mode)) {
		if ((e = ext2fs_new_inode(c->dst, f->dst_dir, LINUX_S_IFDIR | 0755, NULL, &dino)) ||
		    (e = mkdir_retry(c, f->dst_dir, dino, name))) {
			fail(c, e, path, "mkdir");
			goto abort;
		}
		if ((e = copy_xattrs(c, sino, dino))) {
			fail(c, e, path, "xattr");
			goto abort;
		}
		if (copy_tree(c, sino, dino, path))
			goto abort;
		if ((e = fix_meta(c, sino, dino))) {
			fail(c, e, path, "directory metadata");
			goto abort;
		}
		c->n_dirs++;
	} else if (LINUX_S_ISLNK(mode)) {
		char *target = NULL;
		size_t len = EXT2_I_SIZE(&si);

		target = calloc(1, len + 1);
		if (ext2fs_is_fast_symlink((struct ext2_inode *)&si)) {
			memcpy(target, si.i_block, len < sizeof(si.i_block) ? len : sizeof(si.i_block));
		} else {
			ext2_file_t sf;
			unsigned got = 0;

			e = ext2fs_file_open(c->src, sino, 0, &sf);
			if (!e) {
				e = ext2fs_file_read(sf, target, len, &got);
				ext2fs_file_close(sf);
			}
			if (e) {
				free(target);
				fail(c, e, path, "reading symlink");
				goto abort;
			}
		}
		e = symlink_retry(c, f->dst_dir, name, target);
		free(target);
		if (!e)
			e = ext2fs_lookup(c->dst, f->dst_dir, name, nl, NULL, &dino);
		if (e || (e = copy_xattrs(c, sino, dino)) || (e = fix_meta(c, sino, dino))) {
			fail(c, e, path, "symlink");
			goto abort;
		}
		c->n_links++;
	} else {
		struct ext2_inode_large di;
		int is_reg = LINUX_S_ISREG(mode);

		if ((e = ext2fs_new_inode(c->dst, f->dst_dir, mode, NULL, &dino))) {
			fail(c, e, path, "new_inode");
			goto abort;
		}
		memset(&di, 0, sizeof(di));
		di.i_mode = mode;
		di.i_links_count = 1;
		di.i_atime = di.i_ctime = di.i_mtime = (uint32_t)c->now;
		if (is_reg) {
			if (ext2fs_has_feature_extents(c->dst->super))
				di.i_flags |= EXT4_EXTENTS_FL;
		} else {	/* chr/blk: device numbers live in i_block[0..1] */
			di.i_block[0] = si.i_block[0];
			di.i_block[1] = si.i_block[1];
		}
		if ((e = link_entry(c, f->dst_dir, name, dino, ft))) {
			fail(c, e, path, "link");
			goto abort;
		}
		ext2fs_inode_alloc_stats2(c->dst, dino, +1, 0);
		if ((e = ext2fs_write_new_inode(c->dst, dino, (struct ext2_inode *)&di))) {
			fail(c, e, path, "write_new_inode");
			goto abort;
		}
		if (is_reg) {
			if (di.i_flags & EXT4_EXTENTS_FL) {
				ext2_extent_handle_t h;
				e = ext2fs_extent_open2(c->dst, dino, (struct ext2_inode *)&di, &h);
				if (e) {
					fail(c, e, path, "extent_open");
					goto abort;
				}
				ext2fs_extent_free(h);
			}
			if ((e = copy_data(c, sino, dino, EXT2_I_SIZE(&si)))) {
				fail(c, e, path, "data");
				goto abort;
			}
			c->n_files++;
		} else {
			c->n_special++;
		}
		if ((e = copy_xattrs(c, sino, dino)) || (e = fix_meta(c, sino, dino))) {
			fail(c, e, path, "xattr/metadata");
			goto abort;
		}
		if (si.i_links_count > 1)
			c->hl[sino] = dino;
	}
done:
	progress(c);
	free(path);
	return 0;
abort:
	free(path);
	return DIRENT_ABORT;
}

/* ------------------------------------------------------------ filesystem creation */

static void log_fs_summary(const char *title, ext2_filsys fs)
{
	char a[32], b[32];
	uint64_t bs = fs->blocksize;
	log_info("%s: block size %u, %llu blocks (%s), free %s, inodes %u (free %u)", title,
		 fs->blocksize, (unsigned long long)ext2fs_blocks_count(fs->super),
		 human(ext2fs_blocks_count(fs->super) * bs, a, sizeof(a)),
		 human(ext2fs_free_blocks_count(fs->super) * bs, b, sizeof(b)),
		 fs->super->s_inodes_count, fs->super->s_free_inodes_count);
}

static errcode_t make_fs(ext2_filsys src, const char *dst_path, uint64_t dst_off, uint64_t dst_blocks,
			 const struct ext_est *est, ext2_filsys *out)
{
	struct ext2_super_block param, *ss = src->super;
	ext2_filsys fs;
	errcode_t e;
	dgrp_t g;
	ext2_ino_t i;

	memset(&param, 0, sizeof(param));
	param.s_rev_level = ss->s_rev_level;
	ext2fs_blocks_count_set(&param, dst_blocks);
	param.s_log_block_size = ss->s_log_block_size;
	param.s_log_cluster_size = ss->s_log_cluster_size;
	param.s_blocks_per_group = ss->s_blocks_per_group;
	param.s_clusters_per_group = ss->s_clusters_per_group;
	param.s_inodes_count = (__u32)inodes_wanted(est);
	param.s_first_ino = ss->s_first_ino;
	param.s_inode_size = ss->s_inode_size;
	param.s_desc_size = ss->s_desc_size;
	param.s_log_groups_per_flex = ss->s_log_groups_per_flex;
	param.s_creator_os = ss->s_creator_os;
	param.s_feature_compat = ss->s_feature_compat;
	param.s_feature_incompat = ss->s_feature_incompat;
	param.s_feature_ro_compat = ss->s_feature_ro_compat;
	/* things that are created/recomputed from scratch */
	ext2fs_clear_feature_journal(&param);
	ext2fs_clear_feature_fast_commit(&param);
	ext2fs_clear_feature_orphan_file(&param);
	ext2fs_clear_feature_journal_needs_recovery(&param);
	ext2fs_clear_feature_quota(&param);
	ext2fs_clear_feature_project(&param);
	ext2fs_clear_feature_shared_blocks(&param);
	if (ext2fs_has_feature_sparse_super2(&param)) {
		ext2fs_clear_feature_sparse_super2(&param);
		ext2fs_set_feature_sparse_super(&param);
	}

	e = ext2fs_initialize(dst_path, EXT2_FLAG_64BITS, &param, unix_io_manager, &fs);
	if (e)
		return e;
	{	/* initialize does not understand "path?offset=", so set the partition offset explicitly */
		char opt[64];
		snprintf(opt, sizeof(opt), "offset=%llu", (unsigned long long)dst_off);
		if ((e = io_channel_set_options(fs->io, opt)))
			goto err;
	}

	/* filesystem identity: UUID, label, etc. - before creating the journal and tables (checksums!) */
	memcpy(fs->super->s_uuid, ss->s_uuid, sizeof(ss->s_uuid));
	memcpy(fs->super->s_volume_name, ss->s_volume_name, sizeof(ss->s_volume_name));
	memcpy(fs->super->s_last_mounted, ss->s_last_mounted, sizeof(ss->s_last_mounted));
	memcpy(fs->super->s_hash_seed, ss->s_hash_seed, sizeof(ss->s_hash_seed));
	memcpy(fs->super->s_mount_opts, ss->s_mount_opts, sizeof(ss->s_mount_opts));
	fs->super->s_def_hash_version = ss->s_def_hash_version;
	fs->super->s_flags = (fs->super->s_flags & ~3U) | (ss->s_flags & 3U);	/* signed/unsigned hash */
	fs->super->s_default_mount_opts = ss->s_default_mount_opts;
	fs->super->s_errors = ss->s_errors;
	fs->super->s_max_mnt_count = ss->s_max_mnt_count;
	fs->super->s_checkinterval = ss->s_checkinterval;
	fs->super->s_encoding = ss->s_encoding;
	fs->super->s_encoding_flags = ss->s_encoding_flags;
	fs->super->s_mkfs_time = ss->s_mkfs_time;
	fs->super->s_min_extra_isize = ss->s_min_extra_isize;
	fs->super->s_want_extra_isize = ss->s_want_extra_isize;
	if (ext2fs_has_feature_csum_seed(fs->super))
		fs->super->s_checksum_seed = ss->s_checksum_seed;
	/* scale the root-reserved blocks proportionally */
	if (ext2fs_blocks_count(ss))
		ext2fs_r_blocks_count_set(fs->super,
			(unsigned long long)((long double)ext2fs_r_blocks_count(ss) /
					     ext2fs_blocks_count(ss) * dst_blocks));
	if (ext2fs_has_feature_metadata_csum(fs->super))
		fs->super->s_checksum_type = EXT2_CRC32C_CHKSUM;	/* mke2fs does the same */
	ext2fs_init_csum_seed(fs);

	if ((e = ext2fs_allocate_tables(fs)))
		goto err;
	/* the image is freshly created and zeroed - inode tables are already "zeroed" */
	if (ext2fs_has_group_desc_csum(fs))
		for (g = 0; g < fs->group_desc_count; g++)
			ext2fs_bg_flags_set(fs, g, ext2fs_bg_flags(fs, g) | EXT2_BG_INODE_ZEROED);

	/* as in mke2fs: reserved inodes, bad blocks inode, resize inode, root */
	ext2fs_mark_inode_bitmap2(fs->inode_map, EXT2_BAD_INO);
	ext2fs_inode_alloc_stats2(fs, EXT2_BAD_INO, +1, 0);
	if ((e = ext2fs_update_bb_inode(fs, NULL)))
		goto err;
	for (i = EXT2_ROOT_INO + 1; i < EXT2_FIRST_INODE(fs->super); i++)
		ext2fs_inode_alloc_stats2(fs, i, +1, 0);
	ext2fs_mark_ib_dirty(fs);
	if (ext2fs_has_feature_resize_inode(fs->super) && (e = ext2fs_create_resize_inode(fs)))
		goto err;
	if ((e = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, 0)))
		goto err;

	/* journal: same size as before (if it fits) */
	if (ext2fs_has_feature_journal(ss)) {
		struct ext2fs_journal_params jp;
		uint64_t jb = est->journal_blocks;

		if (jb < 1024 || jb > dst_blocks / 4) {
			int d = ext2fs_default_journal_size(dst_blocks);
			jb = d > 0 ? (uint64_t)d : 0;
		}
		if (jb) {
			memset(&jp, 0, sizeof(jp));
			jp.num_journal_blocks = (blk_t)jb;
			log_info("creating journal: %llu blocks", (unsigned long long)jb);
			if ((e = ext2fs_add_journal_inode3(fs, &jp, ~0ULL, 0)))
				goto err;
		} else {
			log_warn("filesystem too small for a journal - created without one");
		}
	}
	*out = fs;
	return 0;
err:
	ext2fs_free(fs);
	return e;
}

int ext_shrink_copy(const char *src, uint64_t src_off, const char *dst, uint64_t dst_off,
		    uint64_t dst_blocks, const struct ext_est *est, int force)
{
	ext2_filsys sfs = NULL, dfs = NULL, vfs = NULL;
	struct cx c;
	char a[32];
	errcode_t e;
	int rc = RC_ERR;

	memset(&c, 0, sizeof(c));
	c.now = time(NULL);

	log_info("opening source read-only: %s, offset %llu", src, (unsigned long long)src_off);
	if ((e = open_fs(src, src_off, &sfs))) {
		log_err("opening source: %s", error_message(e));
		return RC_ERR;
	}
	if (check_source_features(sfs, force) != RC_OK)
		goto out;
	{	/* Reproducible output: every "now" the library would use (reserved inodes,
		 * s_wtime, ...) becomes the source's last write time. */
		char tb[32];

		c.now = sfs->super->s_wtime ? (time_t)sfs->super->s_wtime : time(NULL);
		snprintf(tb, sizeof(tb), "%lld", (long long)c.now);
		setenv("E2FSPROGS_FAKE_TIME", tb, 1);
	}
	log_fs_summary("source", sfs);

	log_info("creating filesystem (like mke2fs) in the image: %llu blocks (%s)",
		 (unsigned long long)dst_blocks, human(dst_blocks * sfs->blocksize, a, sizeof(a)));
	log_info("carrying over UUID=%s LABEL='%.16s', features, block/inode size, hash seed",
		 "<same as original>", sfs->super->s_volume_name);
	if ((e = make_fs(sfs, dst, dst_off, dst_blocks, est, &dfs))) {
		log_err("filesystem creation: %s", error_message(e));
		rc = errcode_to_rc(e);
		goto out;
	}
	log_fs_summary("new fs   ", dfs);

	c.src = sfs;
	c.dst = dfs;
	c.hl = calloc((size_t)sfs->super->s_inodes_count + 1, sizeof(ext2_ino_t));
	c.buf = malloc(COPY_CHUNK);
	if (!c.hl || !c.buf) {
		log_err("out of memory");
		goto out;
	}

	log_info("copying tree (like rsync -aHAX): inodes, data, hardlinks, symlinks, "
		 "device nodes, xattr, ACL, capabilities, timestamps");
	if ((e = copy_xattrs(&c, EXT2_ROOT_INO, EXT2_ROOT_INO)))
		fail(&c, e, "/", "root xattr");
	if (!c.err && (e = fix_meta(&c, EXT2_ROOT_INO, EXT2_ROOT_INO)))
		fail(&c, e, "/", "root metadata");
	if (!c.err)
		copy_tree(&c, EXT2_ROOT_INO, EXT2_ROOT_INO, "/");

	if (c.err) {
		if (c.rc == RC_NOSPACE)
			log_warn("new filesystem ran out of space at %s: %s", c.errpath, error_message(c.err));
		else
			log_err("%s: %s", c.errpath, error_message(c.err));
		rc = c.rc;
		goto out;
	}
	log_info("copied: files %llu, dirs %llu, symlinks %llu, special %llu, hardlinks %llu, "
		 "xattr %llu, logical data size %s", (unsigned long long)c.n_files, (unsigned long long)c.n_dirs,
		 (unsigned long long)c.n_links, (unsigned long long)c.n_special,
		 (unsigned long long)c.n_hard, (unsigned long long)c.n_xattr,
		 human(c.bytes, a, sizeof(a)));

	/* finalize: statistics, backup superblocks/GDT, flags */
	ext2fs_calculate_summary_stats(dfs, 0);
	dfs->super->s_state = EXT2_VALID_FS;
	dfs->super->s_lastcheck = (__u32)c.now;
	dfs->flags &= ~EXT2_FLAG_MASTER_SB_ONLY;
	ext2fs_mark_super_dirty(dfs);
	ext2fs_mark_bb_dirty(dfs);
	ext2fs_mark_ib_dirty(dfs);
	log_fs_summary("result   ", dfs);
	if ((e = ext2fs_close_free(&dfs))) {
		log_err("writing filesystem: %s", error_message(e));
		goto out;
	}

	/* verification: reopen and compare identity */
	if ((e = open_fs(dst, dst_off, &vfs))) {
		log_err("new filesystem does not open: %s", error_message(e));
		goto out;
	}
	if (memcmp(vfs->super->s_uuid, sfs->super->s_uuid, 16) ||
	    memcmp(vfs->super->s_volume_name, sfs->super->s_volume_name, 16)) {
		log_err("UUID/LABEL of the new filesystem differ from the original");
		goto out;
	}
	{
		char u[40];
		const unsigned char *b = vfs->super->s_uuid;
		snprintf(u, sizeof(u), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
			 b[12], b[13], b[14], b[15]);
		log_info("verification: filesystem opens, UUID=%s LABEL='%.16s' match the original", u,
			 vfs->super->s_volume_name);
	}
	rc = RC_OK;
out:
	if (vfs)
		ext2fs_close_free(&vfs);
	if (dfs)
		ext2fs_free(dfs);	/* on error: do not write a half-built filesystem */
	if (sfs)
		ext2fs_close_free(&sfs);
	free(c.hl);
	free(c.buf);
	return rc;
}
