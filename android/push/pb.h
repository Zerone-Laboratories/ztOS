// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The little of protobuf that MCS needs: varints, length-delimited fields and
 * a reader that walks a message field by field. Unknown fields are skipped,
 * so a newer server's additions do not break us.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { PB_VARINT = 0, PB_I64 = 1, PB_LEN = 2, PB_I32 = 5 };

struct pb_out {
	uint8_t *b;
	size_t n, cap;
	bool oom;
};

static inline void pb_free(struct pb_out *o)
{
	free(o->b);
	*o = (struct pb_out){0};
}

static inline void pb_raw(struct pb_out *o, const void *p, size_t n)
{
	if (o->oom)
		return;
	if (o->n + n > o->cap) {
		size_t cap = o->cap ? o->cap : 256;
		while (cap < o->n + n)
			cap *= 2;
		uint8_t *b = realloc(o->b, cap);
		if (!b) {
			o->oom = true;
			return;
		}
		o->b = b;
		o->cap = cap;
	}
	memcpy(o->b + o->n, p, n);
	o->n += n;
}

static inline void pb_varint(struct pb_out *o, uint64_t v)
{
	uint8_t b[10];
	size_t n = 0;
	do {
		b[n] = v & 0x7f;
		v >>= 7;
		if (v)
			b[n] |= 0x80;
		n++;
	} while (v);
	pb_raw(o, b, n);
}

static inline void pb_key(struct pb_out *o, unsigned field, unsigned wire)
{
	pb_varint(o, (uint64_t)field << 3 | wire);
}

static inline void pb_uint(struct pb_out *o, unsigned field, uint64_t v)
{
	pb_key(o, field, PB_VARINT);
	pb_varint(o, v);
}

static inline void pb_bytes(struct pb_out *o, unsigned field, const void *p, size_t n)
{
	pb_key(o, field, PB_LEN);
	pb_varint(o, n);
	pb_raw(o, p, n);
}

static inline void pb_str(struct pb_out *o, unsigned field, const char *s)
{
	pb_bytes(o, field, s, strlen(s));
}

static inline void pb_sub(struct pb_out *o, unsigned field, const struct pb_out *sub)
{
	if (sub->oom)
		o->oom = true;
	pb_bytes(o, field, sub->b, sub->n);
}

struct pb_in {
	const uint8_t *p, *end;
};

struct pb_field {
	unsigned num, wire;
	uint64_t v;		/* PB_VARINT, PB_I64, PB_I32 */
	const uint8_t *data;	/* PB_LEN */
	size_t len;
};

/* 1 for a varint read into *v, 0 when the input ran out or it was too long */
static inline int pb_read_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
	uint64_t r = 0;
	for (int shift = 0; shift < 64 && *p < end; shift += 7) {
		uint8_t b = *(*p)++;
		r |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80)) {
			*v = r;
			return 1;
		}
	}
	return 0;
}

/* 1: a field in *f; 0: the end of the message; -1: it is malformed */
static inline int pb_next(struct pb_in *in, struct pb_field *f)
{
	uint64_t key;
	if (in->p >= in->end)
		return 0;
	if (!pb_read_varint(&in->p, in->end, &key))
		return -1;
	f->num = (unsigned)(key >> 3);
	f->wire = key & 7;
	f->data = NULL;
	f->len = 0;
	f->v = 0;
	switch (f->wire) {
	case PB_VARINT:
		return pb_read_varint(&in->p, in->end, &f->v) ? 1 : -1;
	case PB_I64:
		if (in->end - in->p < 8)
			return -1;
		memcpy(&f->v, in->p, 8);
		in->p += 8;
		return 1;
	case PB_I32: {
		uint32_t v;
		if (in->end - in->p < 4)
			return -1;
		memcpy(&v, in->p, 4);
		f->v = v;
		in->p += 4;
		return 1;
	}
	case PB_LEN: {
		uint64_t n;
		if (!pb_read_varint(&in->p, in->end, &n) || n > (uint64_t)(in->end - in->p))
			return -1;
		f->data = in->p;
		f->len = (size_t)n;
		in->p += n;
		return 1;
	}
	default:
		return -1;	/* groups: not in MCS */
	}
}

/* a length-delimited field as a new NUL-terminated string */
static inline char *pb_strdup(const struct pb_field *f)
{
	char *s = malloc(f->len + 1);
	if (s) {
		memcpy(s, f->data, f->len);
		s[f->len] = 0;
	}
	return s;
}
