/* SPDX-License-Identifier: GPL-2.0-only */
/* copydisk - shared definitions */
#ifndef COPYDISK_H
#define COPYDISK_H

#include <stddef.h>
#include <stdint.h>

#define MIB (1024ULL * 1024ULL)

/* internal step return codes */
enum { RC_OK = 0, RC_ERR = 1, RC_NOSPACE = 2 };

enum part_action { ACT_RAW = 0, ACT_SHRINK = 1 };

struct part {
	size_t   partno;               /* GPT slot, 0-based */
	uint64_t start, size;          /* original, in sectors */
	uint64_t new_start, new_size;  /* in the image, in sectors */
	char    *type_guid;
	char    *uuid;                 /* PARTUUID */
	char    *name;                 /* PARTLABEL */
	uint64_t attrs;
	char     fstype[32];
	char     fsuuid[64];
	char     fslabel[256];
	enum part_action action;
};

struct disk {
	char    *path;
	uint64_t size_bytes;
	uint32_t sector;
	char    *guid;
	size_t   nents;                /* number of slots in the GPT table */
	int      hybrid;
	size_t   nparts;
	struct part *parts;            /* sorted by start */
};

struct options {
	const char *src, *out;
	int      shrink_part;          /* -1 auto, 0 no shrink, N = slot N (1-based) */
	uint64_t margin_mib;
	int      force, overwrite, dry_run;
};

/* estimate of the source ext filesystem */
struct ext_est {
	uint32_t bs;
	uint64_t src_blocks;
	uint64_t data_blocks;          /* sum of i_blocks, in fs blocks */
	uint64_t used_inodes;
	uint64_t journal_blocks;
	uint32_t inode_size, blocks_per_group, gdt_entry;
};

/* log.c */
extern int g_verbose;
void log_step(int n, int total, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vrb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *human(uint64_t bytes, char *buf, size_t n);

/* layout.c */
int  disk_read(const char *path, struct disk *d);
void disk_free(struct disk *d);
int  disk_probe_fs(struct disk *d);
int  disk_write_image_gpt(const char *out, const struct disk *d);
int  disk_write_sfdisk_dump(const char *src, const char *dumpfile);
int  disk_verify_image(const char *out, const struct disk *d);

/* rawcopy.c */
int  raw_copy(int srcfd, uint64_t src_off, int dstfd, uint64_t dst_off, uint64_t len);

/* extshrink.c */
int  ext_estimate(const char *dev, uint64_t off, struct ext_est *e, int force);
uint64_t ext_plan_blocks(const struct ext_est *e, uint64_t margin_mib);
int  ext_shrink_copy(const char *src, uint64_t src_off, const char *dst, uint64_t dst_off,
		     uint64_t dst_blocks, const struct ext_est *e, int force);

#endif
