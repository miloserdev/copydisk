// SPDX-License-Identifier: GPL-2.0-only
/* rawcopy.c - byte-for-byte partition copy that skips zero blocks (sparse) */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "copydisk.h"

#define CHUNK (1024 * 1024)
#define BLK   4096

static int is_zero(const unsigned char *p, size_t n)
{
	size_t i;
	for (i = 0; i + 8 <= n; i += 8) {
		uint64_t v;
		memcpy(&v, p + i, 8);
		if (v)
			return 0;
	}
	for (; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

static int full_pread(int fd, void *buf, size_t n, uint64_t off)
{
	size_t done = 0;
	while (done < n) {
		ssize_t r = pread(fd, (char *)buf + done, n - done, off + done);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0) {
			errno = EIO;	/* source is shorter than the partition table claims */
			return -1;
		}
		done += (size_t)r;
	}
	return 0;
}

static int full_pwrite(int fd, const void *buf, size_t n, uint64_t off)
{
	size_t done = 0;
	while (done < n) {
		ssize_t r = pwrite(fd, (const char *)buf + done, n - done, off + done);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)r;
	}
	return 0;
}

/*
 * Copies len bytes; zero 4K blocks are not written - the image file
 * is pre-sized with ftruncate(), so those ranges are already zeros (holes).
 */
int raw_copy(int srcfd, uint64_t src_off, int dstfd, uint64_t dst_off, uint64_t len)
{
	unsigned char *buf = malloc(CHUNK);
	uint64_t pos = 0, written = 0, skipped = 0;
	int next_pct = 10;

	if (!buf)
		return RC_ERR;

	while (pos < len) {
		size_t n = (len - pos > CHUNK) ? CHUNK : (size_t)(len - pos);
		size_t o = 0;

		if (full_pread(srcfd, buf, n, src_off + pos) < 0) {
			log_err("reading source at offset %llu: %s",
				(unsigned long long)(src_off + pos), strerror(errno));
			free(buf);
			return RC_ERR;
		}
		while (o < n) {
			size_t b = (n - o > BLK) ? BLK : n - o;
			size_t e;

			if (is_zero(buf + o, b)) {
				skipped += b;
				o += b;
				continue;
			}
			/* merge consecutive non-zero blocks into a single write */
			e = o + b;
			while (e < n) {
				size_t bb = (n - e > BLK) ? BLK : n - e;
				if (is_zero(buf + e, bb))
					break;
				e += bb;
			}
			if (full_pwrite(dstfd, buf + o, e - o, dst_off + pos + o) < 0) {
				log_err("writing image: %s", strerror(errno));
				free(buf);
				return RC_ERR;
			}
			written += e - o;
			o = e;
		}
		pos += n;
		if (len >= 64 * MIB && (int)(pos * 100 / len) >= next_pct) {
			log_info("  ... %d%%", next_pct);
			next_pct += 10;
		}
	}
	{
		char a[32], b[32];
		log_info("written %s of data, skipped zero (holes) %s",
			 human(written, a, sizeof(a)), human(skipped, b, sizeof(b)));
	}
	free(buf);
	return RC_OK;
}
