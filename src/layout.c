// SPDX-License-Identifier: GPL-2.0-only
/* layout.c - GPT (libfdisk) and filesystem detection (libblkid) */
#define _GNU_SOURCE
#include <blkid/blkid.h>
#include <errno.h>
#include <fcntl.h>
#include <libfdisk/libfdisk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "copydisk.h"

static int cmp_start(const void *a, const void *b)
{
	const struct part *x = a, *y = b;
	return (x->start > y->start) - (x->start < y->start);
}

static char *dupnn(const char *s)
{
	return strdup(s ? s : "");
}

void disk_free(struct disk *d)
{
	size_t i;
	for (i = 0; i < d->nparts; i++) {
		free(d->parts[i].type_guid);
		free(d->parts[i].uuid);
		free(d->parts[i].name);
	}
	free(d->parts);
	free(d->path);
	free(d->guid);
	memset(d, 0, sizeof(*d));
}

/* Reads the GPT. Everything needed for an exact reproduction is stored in struct disk. */
int disk_read(const char *path, struct disk *d)
{
	struct fdisk_context *cxt = NULL;
	struct fdisk_table *tb = NULL;
	struct stat st;
	char *id = NULL;
	size_t i, n;
	int rc = RC_ERR;

	memset(d, 0, sizeof(*d));
	d->path = strdup(path);

	if (stat(path, &st) < 0) {
		log_err("%s: %s", path, strerror(errno));
		goto out;
	}
	cxt = fdisk_new_context();
	if (!cxt)
		goto out;
	if (fdisk_assign_device(cxt, path, 1) != 0) {
		log_err("libfdisk could not open %s", path);
		goto out;
	}
	if (!fdisk_is_label(cxt, GPT)) {
		log_err("%s: partition table is not GPT", path);
		goto out;
	}
	d->sector = (uint32_t)fdisk_get_sector_size(cxt);
	d->size_bytes = (uint64_t)fdisk_get_nsectors(cxt) * d->sector;
	d->nents = fdisk_get_npartitions(cxt);
	d->hybrid = fdisk_gpt_is_hybrid(cxt) > 0;
	if (fdisk_get_disklabel_id(cxt, &id) == 0 && id)
		d->guid = id;
	else
		d->guid = strdup("");

	if (fdisk_get_partitions(cxt, &tb) != 0) {
		log_err("failed to read the partition list");
		goto out;
	}
	n = fdisk_table_get_nents(tb);
	d->parts = calloc(n ? n : 1, sizeof(*d->parts));
	for (i = 0; i < n; i++) {
		struct fdisk_partition *pa = fdisk_table_get_partition(tb, i);
		struct fdisk_parttype *t;
		struct part *p;

		if (!pa || !fdisk_partition_has_start(pa) || !fdisk_partition_has_size(pa))
			continue;
		p = &d->parts[d->nparts++];
		p->partno = fdisk_partition_get_partno(pa);
		p->start = fdisk_partition_get_start(pa);
		p->size = fdisk_partition_get_size(pa);
		t = fdisk_partition_get_type(pa);
		p->type_guid = dupnn(t ? fdisk_parttype_get_string(t) : NULL);
		p->uuid = dupnn(fdisk_partition_get_uuid(pa));
		p->name = dupnn(fdisk_partition_get_name(pa));
		fdisk_gpt_get_partition_attrs(cxt, p->partno, &p->attrs);
		p->new_start = p->start;
		p->new_size = p->size;
	}
	qsort(d->parts, d->nparts, sizeof(*d->parts), cmp_start);
	rc = RC_OK;
out:
	if (tb)
		fdisk_unref_table(tb);
	if (cxt) {
		fdisk_deassign_device(cxt, 1);
		fdisk_unref_context(cxt);
	}
	if (rc != RC_OK)
		disk_free(d);
	return rc;
}

/* Detects TYPE/UUID/LABEL inside every partition via libblkid. */
int disk_probe_fs(struct disk *d)
{
	int fd = open(d->path, O_RDONLY | O_CLOEXEC);
	size_t i;

	if (fd < 0) {
		log_err("%s: %s", d->path, strerror(errno));
		return RC_ERR;
	}
	for (i = 0; i < d->nparts; i++) {
		struct part *p = &d->parts[i];
		blkid_probe pr = blkid_new_probe();
		const char *v;

		if (!pr)
			continue;
		if (blkid_probe_set_device(pr, fd, (blkid_loff_t)(p->start * d->sector),
					   (blkid_loff_t)(p->size * d->sector)) == 0) {
			blkid_probe_enable_superblocks(pr, 1);
			blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE |
				BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID);
			if (blkid_do_safeprobe(pr) == 0) {
				if (!blkid_probe_lookup_value(pr, "TYPE", &v, NULL))
					snprintf(p->fstype, sizeof(p->fstype), "%s", v);
				if (!blkid_probe_lookup_value(pr, "UUID", &v, NULL))
					snprintf(p->fsuuid, sizeof(p->fsuuid), "%s", v);
				if (!blkid_probe_lookup_value(pr, "LABEL", &v, NULL))
					snprintf(p->fslabel, sizeof(p->fslabel), "%s", v);
			}
		}
		blkid_free_probe(pr);
	}
	close(fd);
	return RC_OK;
}

/*
 * Creates the GPT in the image file. The file must already have its final size
 * (otherwise libfdisk would put the backup GPT in the wrong place).
 */
int disk_write_image_gpt(const char *out, const struct disk *d)
{
	struct fdisk_context *cxt = fdisk_new_context();
	struct fdisk_label *lb;
	size_t i;
	int rc = RC_ERR, r;

	if (!cxt)
		return RC_ERR;
	fdisk_save_user_sector_size(cxt, d->sector, d->sector);
	if (fdisk_assign_device(cxt, out, 0) != 0) {
		log_err("libfdisk could not open image %s", out);
		goto out;
	}
	if (fdisk_create_disklabel(cxt, "gpt") != 0) {
		log_err("failed to create GPT");
		goto out;
	}
	if (d->nents && fdisk_gpt_set_npartitions(cxt, (uint32_t)d->nents) != 0)
		log_warn("failed to set the number of GPT slots = %zu", d->nents);
	if (d->guid && *d->guid && (r = fdisk_set_disklabel_id_from_string(cxt, d->guid)) != 0) {
		log_err("failed to set disk GUID %s (%d)", d->guid, r);
		goto out;
	}
	lb = fdisk_get_label(cxt, "gpt");

	for (i = 0; i < d->nparts; i++) {
		const struct part *p = &d->parts[i];
		struct fdisk_partition *pa = fdisk_new_partition();
		struct fdisk_parttype *t = fdisk_label_parse_parttype(lb, p->type_guid);

		fdisk_partition_set_partno(pa, p->partno);
		fdisk_partition_set_start(pa, p->new_start);
		fdisk_partition_set_size(pa, p->new_size);
		if (t) {
			fdisk_partition_set_type(pa, t);
			fdisk_unref_parttype(t);
		}
		if (*p->uuid)
			fdisk_partition_set_uuid(pa, p->uuid);
		fdisk_partition_set_name(pa, p->name);
		r = fdisk_add_partition(cxt, pa, NULL);
		fdisk_unref_partition(pa);
		if (r != 0) {
			log_err("failed to add partition #%zu (%d)", p->partno + 1, r);
			goto out;
		}
		if (p->attrs && fdisk_gpt_set_partition_attrs(cxt, p->partno, p->attrs) != 0)
			log_warn("partition #%zu: failed to set attributes", p->partno + 1);
		log_vrb("GPT: slot %zu start=%llu size=%llu uuid=%s name='%s'", p->partno + 1,
			(unsigned long long)p->new_start, (unsigned long long)p->new_size,
			p->uuid, p->name);
	}
	if (fdisk_write_disklabel(cxt) != 0) {
		log_err("failed to write GPT to the image");
		goto out;
	}
	rc = RC_OK;
out:
	fdisk_deassign_device(cxt, 0);
	fdisk_unref_context(cxt);
	return rc;
}

/* Dump of the ORIGINAL table in sfdisk format: needed to restore onto the source disk. */
int disk_write_sfdisk_dump(const char *src, const char *dumpfile)
{
	struct fdisk_context *cxt = fdisk_new_context();
	struct fdisk_script *sc;
	FILE *f;
	int rc = RC_ERR;

	if (!cxt)
		return RC_ERR;
	if (fdisk_assign_device(cxt, src, 1) != 0)
		goto out;
	sc = fdisk_new_script(cxt);
	if (!sc)
		goto out;
	f = fopen(dumpfile, "w");
	if (f) {
		if (fdisk_script_read_context(sc, NULL) == 0 && fdisk_script_write_file(sc, f) == 0)
			rc = RC_OK;
		if (fclose(f) != 0)
			rc = RC_ERR;
	}
	fdisk_unref_script(sc);
out:
	fdisk_deassign_device(cxt, 1);
	fdisk_unref_context(cxt);
	return rc;
}

/* Re-reads the image GPT and compares it with the plan. */
int disk_verify_image(const char *out, const struct disk *d)
{
	struct disk n;
	size_t i;
	int bad = 0;

	if (disk_read(out, &n) != RC_OK)
		return RC_ERR;
	if (strcasecmp(n.guid, d->guid) != 0) {
		log_err("disk GUID mismatch: %s != %s", n.guid, d->guid);
		bad++;
	}
	if (n.nparts != d->nparts) {
		log_err("partition count: %zu != %zu", n.nparts, d->nparts);
		bad++;
	}
	for (i = 0; i < n.nparts && i < d->nparts; i++) {
		const struct part *a = &n.parts[i], *b = &d->parts[i];
		if (a->partno != b->partno || a->start != b->new_start || a->size != b->new_size ||
		    strcasecmp(a->uuid, b->uuid) || strcasecmp(a->type_guid, b->type_guid) ||
		    strcmp(a->name, b->name) || a->attrs != b->attrs) {
			log_err("partition #%zu in the image does not match the plan", b->partno + 1);
			bad++;
		}
	}
	disk_free(&n);
	if (!bad)
		log_info("image GPT matches the plan: disk GUID, PARTUUID, types, names, attributes");
	return bad ? RC_ERR : RC_OK;
}
