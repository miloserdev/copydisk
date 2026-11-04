// SPDX-License-Identifier: GPL-2.0-only
/* log.c - step-by-step message output */
#include <stdarg.h>
#include <stdio.h>
#include "copydisk.h"

int g_verbose;

static void vout(FILE *f, const char *prefix, const char *fmt, va_list ap)
{
	fputs(prefix, f);
	vfprintf(f, fmt, ap);
	fputc('\n', f);
	fflush(f);
}

void log_step(int n, int total, const char *fmt, ...)
{
	char pfx[32];
	va_list ap;

	snprintf(pfx, sizeof(pfx), "\n[%d/%d] ", n, total);
	va_start(ap, fmt);
	vout(stdout, pfx, fmt, ap);
	va_end(ap);
}

void log_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vout(stdout, "      ", fmt, ap);
	va_end(ap);
}

void log_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vout(stderr, "  [!] ", fmt, ap);
	va_end(ap);
}

void log_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vout(stderr, "[ERROR] ", fmt, ap);
	va_end(ap);
}

void log_vrb(const char *fmt, ...)
{
	va_list ap;
	if (!g_verbose)
		return;
	va_start(ap, fmt);
	vout(stdout, "        . ", fmt, ap);
	va_end(ap);
}

const char *human(uint64_t b, char *buf, size_t n)
{
	static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = (double)b;
	int i = 0;

	while (v >= 1024.0 && i < 4) {
		v /= 1024.0;
		i++;
	}
	if (i == 0)
		snprintf(buf, n, "%llu B", (unsigned long long)b);
	else
		snprintf(buf, n, "%.2f %s", v, u[i]);
	return buf;
}
