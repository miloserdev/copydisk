// SPDX-License-Identifier: GPL-2.0-only
/*
 * copydisk - a "mini slice" of a GPT disk: same GPT/UUID/PARTUUID/LABEL, but a smaller rootfs.
 * Details: README.md, TZ.md
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "copydisk.h"

#define TOTAL_STEPS 7
#define MAX_RETRIES 3

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] SOURCE IMAGE\n"
		"  SOURCE     block device (/dev/nvme0n1) or a disk image file with a GPT\n"
		"  IMAGE      output sparse file (+ IMAGE.sfdisk and IMAGE.layout)\n\n"
		"Options:\n"
		"  -r N, --root N    shrink partition number N (as in fdisk, 1-based); default: the\n"
		"                    largest ext2/3/4 partition\n"
		"  -n, --no-shrink   shrink nothing (all partitions are copied 1:1)\n"
		"  -m MIB, --margin MIB   free-space margin in the new filesystem (default 256)\n"
		"  -f, --force       continue even if the source journal was not replayed\n"
		"  -y, --overwrite   overwrite IMAGE if it exists\n"
		"  -p, --plan        only show the plan, write nothing\n"
		"  -v, --verbose     print every copied file\n"
		"  -h, --help\n", prog);
}

static int is_ext(const char *t)
{
	return !strcmp(t, "ext4") || !strcmp(t, "ext3") || !strcmp(t, "ext2");
}

static void print_table(const struct disk *d)
{
	size_t i;
	char a[32];

	for (i = 0; i < d->nparts; i++) {
		const struct part *p = &d->parts[i];
		log_info("#%zu  %-8s %-10s %12s  PARTUUID=%s  PARTLABEL='%s'  FS-UUID=%s LABEL='%s'",
			 p->partno + 1, p->fstype[0] ? p->fstype : "-",
			 "", human(p->size * d->sector, a, sizeof(a)), p->uuid, p->name,
			 p->fsuuid[0] ? p->fsuuid : "-", p->fslabel);
	}
}

/* Layout: partitions before the shrunk one keep their original LBA, the ones after follow back-to-back, 1 MiB aligned. */
static uint64_t plan_layout(struct disk *d)
{
	uint64_t align = MIB / d->sector, cursor = 0, end = 0;
	int shifted = 0;
	size_t i;

	for (i = 0; i < d->nparts; i++) {
		struct part *p = &d->parts[i];

		p->new_start = shifted ? (cursor + align - 1) / align * align : p->start;
		if (p->new_size != p->size)
			shifted = 1;
		cursor = p->new_start + p->new_size;
		if (cursor > end)
			end = cursor;
	}
	/* 1 MiB tail for the backup GPT */
	return ((end * d->sector + MIB - 1) / MIB + 1) * MIB;
}

static int write_layout_report(const char *path, const struct disk *d, uint64_t total)
{
	FILE *f = fopen(path, "w");
	size_t i;

	if (!f)
		return RC_ERR;
	fprintf(f, "# copydisk layout\nsource: %s\nsector: %u\ndisk-guid: %s\nimage-bytes: %llu\n"
		   "# slot  action  orig_start  orig_size  new_start  new_size  partuuid  type  fstype  fs-uuid  label  name\n",
		d->path, d->sector, d->guid, (unsigned long long)total);
	for (i = 0; i < d->nparts; i++) {
		const struct part *p = &d->parts[i];
		fprintf(f, "%zu  %s  %llu  %llu  %llu  %llu  %s  %s  %s  %s  '%s'  '%s'\n", p->partno + 1,
			p->action == ACT_SHRINK ? "shrink" : "raw", (unsigned long long)p->start,
			(unsigned long long)p->size, (unsigned long long)p->new_start,
			(unsigned long long)p->new_size, p->uuid, p->type_guid,
			p->fstype[0] ? p->fstype : "-", p->fsuuid[0] ? p->fsuuid : "-", p->fslabel, p->name);
	}
	return fclose(f) ? RC_ERR : RC_OK;
}

/* One full run. Returns RC_NOSPACE if the shrunk filesystem turned out too small. */
static int run(const struct options *o, uint64_t margin_mib, int attempt)
{
	struct disk d;
	struct ext_est est;
	char a[32], b[32], dump[4096], rep[4096];
	size_t i, shrink = (size_t)-1;
	uint64_t total, blocks = 0;
	int srcfd = -1, dstfd = -1, rc = RC_ERR;

	memset(&est, 0, sizeof(est));
	if (attempt)
		log_info("=== retry #%d with margin %llu MiB ===", attempt, (unsigned long long)margin_mib);

	/* ---- 1 ---- */
	log_step(1, TOTAL_STEPS, "Reading the source GPT via libfdisk: %s", o->src);
	if (disk_read(o->src, &d) != RC_OK)
		return RC_ERR;
	log_info("disk %s, sector %u B, disk GUID %s, GPT slots %zu, partitions %zu",
		 human(d.size_bytes, a, sizeof(a)), d.sector, d.guid, d.nents, d.nparts);
	if (d.hybrid)
		log_warn("hybrid MBR detected - it is not carried into the image (a protective MBR is used)");
	if (!d.nparts) {
		log_err("the GPT has no partitions");
		goto out;
	}

	/* ---- 2 ---- */
	log_step(2, TOTAL_STEPS, "Detecting filesystems via libblkid");
	if (disk_probe_fs(&d) != RC_OK)
		goto out;
	print_table(&d);

	/* ---- 3 ---- */
	log_step(3, TOTAL_STEPS, "Choosing the partition to shrink and estimating the really used space");
	if (o->shrink_part > 0) {
		for (i = 0; i < d.nparts; i++)
			if (d.parts[i].partno + 1 == (size_t)o->shrink_part)
				shrink = i;
		if (shrink == (size_t)-1) {
			log_err("there is no partition #%d", o->shrink_part);
			goto out;
		}
		if (!is_ext(d.parts[shrink].fstype)) {
			log_err("partition #%d: filesystem '%s' - shrinking is supported only for ext2/3/4",
				o->shrink_part, d.parts[shrink].fstype);
			goto out;
		}
	} else if (o->shrink_part < 0) {
		for (i = 0; i < d.nparts; i++)
			if (is_ext(d.parts[i].fstype) &&
			    (shrink == (size_t)-1 || d.parts[i].size > d.parts[shrink].size))
				shrink = i;
	}
	if (shrink == (size_t)-1) {
		log_info("nothing to shrink - all partitions are copied 1:1");
	} else {
		struct part *p = &d.parts[shrink];
		uint64_t off = p->start * d.sector;

		log_info("shrinking partition #%zu (%s, %s)", p->partno + 1, p->fstype,
			 human(p->size * d.sector, a, sizeof(a)));
		if (ext_estimate(d.path, off, &est, o->force) != RC_OK)
			goto out;
		blocks = ext_plan_blocks(&est, margin_mib);
		log_info("data in use: %s (%llu inodes), journal %s",
			 human(est.data_blocks * est.bs, a, sizeof(a)),
			 (unsigned long long)est.used_inodes,
			 human(est.journal_blocks * est.bs, b, sizeof(b)));
		log_info("new filesystem size: %s (incl. %llu MiB margin) vs. original %s",
			 human(blocks * est.bs, a, sizeof(a)), (unsigned long long)margin_mib,
			 human(est.src_blocks * est.bs, b, sizeof(b)));
		if (blocks * est.bs >= p->size * d.sector) {
			log_warn("nothing to gain - the partition is already not larger than the estimate; copying 1:1");
			shrink = (size_t)-1;
		} else {
			p->action = ACT_SHRINK;
			/* the partition must hold the whole filesystem */
			p->new_size = (blocks * est.bs + d.sector - 1) / d.sector;
		}
	}

	/* ---- 4 ---- */
	log_step(4, TOTAL_STEPS, "Planning the image layout");
	total = plan_layout(&d);
	for (i = 0; i < d.nparts; i++) {
		struct part *p = &d.parts[i];
		log_info("#%zu %-6s LBA %llu+%llu -> %llu+%llu (%s)", p->partno + 1,
			 p->action == ACT_SHRINK ? "SHRINK" : "raw", (unsigned long long)p->start,
			 (unsigned long long)p->size, (unsigned long long)p->new_start,
			 (unsigned long long)p->new_size, human(p->new_size * d.sector, a, sizeof(a)));
		if (p->new_start != p->start && p->action != ACT_SHRINK)
			log_warn("partition #%zu is shifted (it follows the shrunk one) - when restoring "
				 "it must be moved back to its original LBA", p->partno + 1);
	}
	log_info("image file size: %s (apparent); on disk it takes roughly as much as "
		 "the actual data", human(total, a, sizeof(a)));
	if (o->dry_run) {
		log_info("--plan mode: nothing more to do");
		rc = RC_OK;
		goto out;
	}

	/* ---- 5 ---- */
	log_step(5, TOTAL_STEPS, "Creating the sparse image and writing the GPT (libfdisk)");
	if (!o->overwrite && access(o->out, F_OK) == 0) {
		log_err("%s already exists (use -y)", o->out);
		goto out;
	}
	dstfd = open(o->out, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (dstfd < 0 || ftruncate(dstfd, (off_t)total) < 0) {
		log_err("%s: %s", o->out, strerror(errno));
		goto out;
	}
	if (disk_write_image_gpt(o->out, &d) != RC_OK)
		goto out;
	if (disk_verify_image(o->out, &d) != RC_OK)
		goto out;

	/* ---- 6 ---- */
	log_step(6, TOTAL_STEPS, "Copying partition contents");
	srcfd = open(o->src, O_RDONLY | O_CLOEXEC);
	if (srcfd < 0) {
		log_err("%s: %s", o->src, strerror(errno));
		goto out;
	}
	for (i = 0; i < d.nparts; i++) {
		struct part *p = &d.parts[i];
		int r;

		if (p->action == ACT_SHRINK) {
			log_info("--- partition #%zu: recreating the filesystem and copying files ---", p->partno + 1);
			r = ext_shrink_copy(o->src, p->start * d.sector, o->out, p->new_start * d.sector,
					    blocks, &est, o->force);
		} else {
			log_info("--- partition #%zu: raw copy of %s (%s) ---", p->partno + 1,
				 p->fstype[0] ? p->fstype : "no fs / unknown",
				 human(p->size * d.sector, a, sizeof(a)));
			r = raw_copy(srcfd, p->start * d.sector, dstfd, p->new_start * d.sector,
				     p->size * d.sector);
		}
		if (r != RC_OK) {
			rc = r;
			goto out;
		}
	}
	if (fsync(dstfd) < 0)
		log_warn("fsync: %s", strerror(errno));

	/* ---- 7 ---- */
	log_step(7, TOTAL_STEPS, "Saving restore data");
	snprintf(dump, sizeof(dump), "%s.sfdisk", o->out);
	snprintf(rep, sizeof(rep), "%s.layout", o->out);
	if (disk_write_sfdisk_dump(o->src, dump) != RC_OK) {
		log_err("failed to write %s", dump);
		goto out;
	}
	log_info("original GPT (sfdisk dump): %s", dump);
	if (write_layout_report(rep, &d, total) != RC_OK) {
		log_err("failed to write %s", rep);
		goto out;
	}
	log_info("before -> after table: %s", rep);
	{
		struct stat st;
		if (stat(o->out, &st) == 0)
			log_info("done: %s, apparent %s, on disk %s", o->out,
				 human((uint64_t)st.st_size, a, sizeof(a)),
				 human((uint64_t)st.st_blocks * 512, b, sizeof(b)));
	}
	rc = RC_OK;
out:
	if (srcfd >= 0)
		close(srcfd);
	if (dstfd >= 0)
		close(dstfd);
	disk_free(&d);
	return rc;
}

int main(int argc, char **argv)
{
	static const struct option lo[] = {
		{ "root", 1, 0, 'r' }, { "no-shrink", 0, 0, 'n' }, { "margin", 1, 0, 'm' },
		{ "force", 0, 0, 'f' }, { "overwrite", 0, 0, 'y' }, { "plan", 0, 0, 'p' },
		{ "verbose", 0, 0, 'v' }, { "help", 0, 0, 'h' }, { 0, 0, 0, 0 }
	};
	struct options o = { .shrink_part = -1, .margin_mib = 256 };
	uint64_t margin;
	int c, rc, attempt;

	while ((c = getopt_long(argc, argv, "r:nm:fypvh", lo, NULL)) != -1) {
		switch (c) {
		case 'r': o.shrink_part = atoi(optarg); if (o.shrink_part <= 0) { usage(argv[0]); return 2; } break;
		case 'n': o.shrink_part = 0; break;
		case 'm': o.margin_mib = strtoull(optarg, NULL, 10); break;
		case 'f': o.force = 1; break;
		case 'y': o.overwrite = 1; break;
		case 'p': o.dry_run = 1; break;
		case 'v': g_verbose = 1; break;
		default: usage(argv[0]); return c == 'h' ? 0 : 2;
		}
	}
	if (argc - optind != 2) {
		usage(argv[0]);
		return 2;
	}
	o.src = argv[optind];
	o.out = argv[optind + 1];
	if (!strcmp(o.src, o.out)) {
		log_err("source and image are the same file");
		return 2;
	}

	margin = o.margin_mib;
	for (attempt = 0;; attempt++) {
		rc = run(&o, margin, attempt);
		if (rc != RC_NOSPACE)
			break;
		if (attempt >= MAX_RETRIES) {
			log_err("still not enough space; increase -m");
			break;
		}
		margin *= 2;
		o.overwrite = 1;
		log_warn("not enough space - removing the incomplete image and retrying with margin %llu MiB",
			 (unsigned long long)margin);
	}
	if (rc != RC_OK) {
		if (!o.dry_run)
			unlink(o.out);	/* an incomplete image is worse than none */
		return 1;
	}
	printf("\nDone.\n");
	return 0;
}
