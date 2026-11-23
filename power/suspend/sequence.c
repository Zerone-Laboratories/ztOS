// SPDX-License-Identifier: GPL-2.0-only
/* Samsung PMUCAL SAVE / RESTORE / HANDLE semantics for the sleep tables. */
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
#include "sequence.h"

static bool condition(const struct zrn_seq *s, const struct zrn_seq_io *io)
{
	return (io->read(io->context, s->cond_base, s->cond_offset) &
		s->cond_mask) == s->cond_value;
}

static void write_value(const struct zrn_seq *s, zrn_u32 value,
			const struct zrn_seq_io *io)
{
	if (s->mask != (zrn_u32)~0U)
		value |= io->read(io->context, s->base, s->offset) & ~s->mask;
	io->write(io->context, s->base, s->offset, value);
}

int zrn_sequence_run(struct zrn_seq *seq, unsigned int size,
		     enum zrn_seq_phase phase, const struct zrn_seq_io *io,
		     unsigned int *failed)
{
	unsigned int i, retry;
	struct zrn_seq *s;
	bool conditional;
	bool skip_next = false;

	if ((!seq && size) || !io || !io->read || !io->write || !io->delay_us ||
	    (unsigned int)phase > ZRN_HANDLE)
		return -EINVAL;
	for (i = 0; i < size; i++) {
		s = &seq[i];
		if (failed)
			*failed = i;
		/* PMUCAL_CHECK_SKIP marks the following entry for skipping when
		 * its own value test matches (vendor pmucal_rae need_skip). */
		if (skip_next) {
			skip_next = false;
			continue;
		}
		if (phase == ZRN_SAVE)
			s->valid = false;
		conditional = s->op == PMUCAL_COND_READ ||
			s->op == PMUCAL_COND_WRITE ||
			s->op == PMUCAL_COND_SAVE_RESTORE;
		if (conditional && !condition(s, io))
			continue;
		switch (s->op) {
		case PMUCAL_SAVE_RESTORE:
		case PMUCAL_COND_SAVE_RESTORE:
			if (phase == ZRN_SAVE) {
				s->saved = io->read(io->context, s->base, s->offset) & s->mask;
				s->valid = true;
			} else if (phase == ZRN_RESTORE && s->valid) {
				write_value(s, s->saved, io);
				s->valid = false;
			} else if (phase == ZRN_HANDLE) {
				return -EINVAL;
			}
			break;
		case PMUCAL_READ:
		case PMUCAL_COND_READ:
			if (phase != ZRN_RESTORE)
				s->saved = io->read(io->context, s->base, s->offset) & s->mask;
			break;
		case PMUCAL_WRITE:
		case PMUCAL_COND_WRITE:
			if (phase != ZRN_SAVE)
				write_value(s, s->value, io);
			break;
		case PMUCAL_CHECK_SKIP:
			/* Vendor skips the next entry only on the restore pass
			 * (e.g. skip the PLL-lock wait when the PLL is off). */
			if (phase == ZRN_RESTORE &&
			    (io->read(io->context, s->base, s->offset) &
			     s->mask) == s->value)
				skip_next = true;
			break;
		case PMUCAL_WAIT:
			if (phase == ZRN_SAVE)
				break;
			for (retry = 0; retry <= 1000; retry++) {
				if ((io->read(io->context, s->base, s->offset) &
				     s->mask) == s->value)
					break;
				io->delay_us(io->context, 1);
			}
			if (retry > 1000)
				return -ETIMEDOUT;
			break;
		case PMUCAL_WRITE_WAIT:
			/* Vendor pmucal_rae: write the field, then poll the paired
			 * STATUS register (CONFIGURATION offset + 4) until it reads
			 * back the written value. CHUB bring-up uses it for the M4
			 * reset / clamp / sweeper handshakes. Not a save-pass op. */
			if (phase == ZRN_SAVE)
				break;
			write_value(s, s->value, io);
			for (retry = 0; retry <= 1000; retry++) {
				if ((io->read(io->context, s->base, s->offset + 0x4) &
				     s->mask) == s->value)
					break;
				io->delay_us(io->context, 1);
			}
			if (retry > 1000)
				return -ETIMEDOUT;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}
