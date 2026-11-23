/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ZRN_SLEEP_SEQUENCE_H
#define ZRN_SLEEP_SEQUENCE_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 zrn_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t zrn_u32;
#endif

enum zrn_seq_op {
	PMUCAL_READ, PMUCAL_WRITE, PMUCAL_COND_READ, PMUCAL_COND_WRITE,
	PMUCAL_SAVE_RESTORE, PMUCAL_COND_SAVE_RESTORE, PMUCAL_WAIT,
	PMUCAL_CHECK_SKIP, PMUCAL_WRITE_WAIT,
};
enum zrn_seq_phase { ZRN_SAVE, ZRN_RESTORE, ZRN_HANDLE };
struct zrn_seq {
	enum zrn_seq_op op;
	const char *name;
	zrn_u32 base, offset, mask, value, cond_base, cond_offset;
	zrn_u32 cond_mask, cond_value, saved;
	bool valid;
};
struct zrn_seq_io {
	void *context;
	zrn_u32 (*read)(void *, zrn_u32, zrn_u32);
	void (*write)(void *, zrn_u32, zrn_u32, zrn_u32);
	void (*delay_us)(void *, unsigned int);
};
int zrn_sequence_run(struct zrn_seq *, unsigned int, enum zrn_seq_phase,
		     const struct zrn_seq_io *, unsigned int *);
#endif
