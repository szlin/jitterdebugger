// SPDX-License-Identifier: MIT

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "jitterdebugger.h"

struct ringbuffer_sample {
	struct timespec ts;
	uint64_t val;
};

struct ringbuffer {
	uint32_t size;
	uint32_t overflow;
	uint32_t read;
	uint32_t write;
	struct ringbuffer_sample *data;
};

static int ringbuffer_full(uint32_t size, uint32_t read, uint32_t write)
{
	return write - read == size;
}

static int ringbuffer_empty(uint32_t read, uint32_t write)
{
	return write == read;
}

static uint32_t ringbuffer_mask(uint32_t size, uint32_t idx)
{
	return idx & (size - 1);
}

struct ringbuffer *ringbuffer_create(unsigned int size)
{
	struct ringbuffer *rb;

	/* Size needs to be of power of 2 */
	if ((size & (size - 1)) != 0)
		return NULL;

	rb = calloc(1, sizeof(*rb));
	if (!rb)
		return NULL;

	rb->size = size;
	rb->data = calloc(rb->size, sizeof(struct ringbuffer_sample));

	return rb;
}

int ringbuffer_write(struct ringbuffer *rb, struct timespec ts, uint64_t val)
{
	uint32_t read, idx;

	read = READ_ONCE(rb->read);
	if (ringbuffer_full(rb->size, read, rb->write)) {
		rb->overflow++;
		return 1;
	}

	idx = ringbuffer_mask(rb->size, rb->write + 1);
	rb->data[idx].ts = ts;
	rb->data[idx].val = val;

	WRITE_ONCE(rb->write, rb->write + 1);
	return 0;
}

int ringbuffer_read(struct ringbuffer *rb, struct timespec *ts, uint64_t *val)
{
	uint32_t write, idx;

	write = READ_ONCE(rb->write);
	if (ringbuffer_empty(rb->read, write))
		return 1;

	idx = ringbuffer_mask(rb->size, rb->read + 1);
	*ts = rb->data[idx].ts;
	*val = rb->data[idx].val;

	WRITE_ONCE(rb->read, rb->read + 1);
	return 0;
}

void _err_handler(int error, char *fmt, ...)
{
	va_list ap;
	char *msg;

	va_start(ap, fmt);
	vasprintf(&msg, fmt, ap);
	va_end(ap);

	fprintf(stderr, "%s: %s\n", msg, strerror(error));

	free(msg);
	exit(1);
}

void _warn_handler(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
}

/*
 * Returns parsed zero or positive number, or a negative error value.
 * len optionally stores the length of the parsed string, may be NULL.
 */
long int parse_num(const char *str, int base, size_t *len)
{
	long int ret;
	char *endptr;

	errno = 0;
	ret = strtol(str, &endptr, base);
	if (errno)
		return -errno;
	if (ret < 0)
		return -ERANGE;

	if (len)
		*len = endptr - str;

	return ret;
}

/* cpu_set_t helpers */

void cpuset_from_bits(cpu_set_t *set, unsigned long bits)
{
	unsigned i;

	for (i = 0; i < sizeof(bits) * 8; i++)
		if ((1UL << i) & bits)
			CPU_SET(i, set);
}

unsigned long cpuset_to_bits(cpu_set_t *set)
{
	unsigned long bits = 0;
	unsigned int i, t, bit;

	for (i = 0, t = 0; t < CPU_COUNT(set); i++) {
		bit = CPU_ISSET(i, set);
		bits |= bit << i;
		t += bit;
	}

	return bits;
}

static inline void _cpuset_fprint_end(FILE *f, unsigned long i, unsigned long r)
{
	if (r > 1)
		fprintf(f, "%c%lu", r > 2 ? '-' : ',', i - 1);
}

/* Prints cpu_set_t as an affinity specification. */
void cpuset_fprint(FILE *f, cpu_set_t *set)
{
	unsigned long bit = 0, range = 0;
	unsigned long i, t, comma = 0;

	for (i = 0, t = 0; t < CPU_COUNT(set); t += bit, i++) {
		bit = CPU_ISSET(i, set);
		if (!range && bit) {
			if (comma)
				fputc(',', f);

			comma = 1;
			fprintf(f, "%lu", i);
		} else if (!bit) {
			_cpuset_fprint_end(f, i, range);
			range = 0;
		}
		range += bit;
	}

	_cpuset_fprint_end(f, i, range);
	fprintf(f, " = %u [0x%lX]", CPU_COUNT(set), cpuset_to_bits(set));
}

static long int _cpuset_parse_num(const char *str, int base, size_t *len)
{
	long int ret;

	ret = parse_num(str, base, len);
	if (ret < 0)
		err_abort("cpuset: unable to parse string %s", str);

	return ret;
}

/*
 * Parses affinity specification (i.e. 0,2-3,7) into a cpu_set_t.
 * Returns parsed string length or -errno.
 */
ssize_t cpuset_parse(cpu_set_t *set, const char *str)
{
	unsigned int i, first, last;
	ssize_t len_next;
	long int num = 0;
	size_t len = 0;

	if (!strncmp(str, "0x", 2)) {
		num = _cpuset_parse_num(str, 16, &len);
		cpuset_from_bits(set, (unsigned long) num);
		return len;
	}

	num = _cpuset_parse_num(str, 10, &len);

	str += len;
	first = num;

	if (str[0] == '-') {
		num = _cpuset_parse_num(str + 1, 10, &len);
		str += 1 + len;
		last = len ? num + 1 : CPU_SETSIZE; /* "x-" means x-CPU_SIZE */
	} else
		last = first + 1;

	if (CPU_SETSIZE < last) {
		warn_handler("cpu num %d bigger than CPU_SETSIZE(%u), reducing",
			     last, CPU_SETSIZE);
		last = CPU_SETSIZE;
	}

	for (i = first; i < last; i++)
		CPU_SET(i, set);

	if (str[0] == ',') {
		len_next = cpuset_parse(set, str + 1);
		if (len_next < 0)
			return len_next;

		len += len_next;
	}

	return len;
}
