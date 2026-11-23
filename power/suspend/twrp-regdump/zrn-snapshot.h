/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared register snapshot for comparing Samsung's kernel (TWRP, 4.9) with
 * ztOS at the moment each hands the SoC to the firmware for SYS_SLEEP.
 * Read-only. Registers come from zrn-snapshot-list.h (the ones the sleep
 * tables already touch); a conditional one is read only when its domain
 * status condition holds, as the sequence engine does. Plus the 64 KiB APM
 * SRAM (ACPM firmware state). Kernel-version neutral: no proc/seq glue here.
 */
#ifndef ZRN_SNAPSHOT_H
#define ZRN_SNAPSHOT_H

struct zrn_snap_reg { u32 addr, cond, cmask, cval; const char *name; };
#include "zrn-snapshot-list.h"

#define ZRN_SNAP_N ARRAY_SIZE(zrn_snap_regs)
/* flexpmu debug block only: other APM SRAM ranges are secure and abort */
#define ZRN_SNAP_SRAM_BASE 0x0204f800
#define ZRN_SNAP_SRAM_SIZE 0x200
#define ZRN_SNAP_MAX_PAGES 256

struct zrn_snap {
	u32 regs[ZRN_SNAP_N];
	u8 read[ZRN_SNAP_N];
	u32 *sram;		/* vmalloc'ed, ZRN_SNAP_SRAM_SIZE */
	bool taken;
	unsigned long count;
};

static struct { u32 page; void __iomem *va; } zrn_snap_pages[ZRN_SNAP_MAX_PAGES];
static int zrn_snap_npages;
static void __iomem *zrn_snap_sram_va;

static void __iomem *zrn_snap_va(u32 addr)
{
	int i;

	for (i = 0; i < zrn_snap_npages; i++)
		if (zrn_snap_pages[i].page == (addr & ~0xfffU))
			return zrn_snap_pages[i].va + (addr & 0xfff);
	return NULL;
}

static int zrn_snap_map_one(u32 addr)
{
	if (!addr || zrn_snap_va(addr))
		return 0;
	if (zrn_snap_npages == ZRN_SNAP_MAX_PAGES)
		return -ENOSPC;
	zrn_snap_pages[zrn_snap_npages].va = ioremap(addr & ~0xfffU, 0x1000);
	if (!zrn_snap_pages[zrn_snap_npages].va)
		return -ENOMEM;
	zrn_snap_pages[zrn_snap_npages++].page = addr & ~0xfffU;
	return 0;
}

/* call from process context (module init) */
static int zrn_snap_map(void)
{
	size_t i;
	int ret;

	for (i = 0; i < ZRN_SNAP_N; i++) {
		ret = zrn_snap_map_one(zrn_snap_regs[i].addr);
		if (!ret)
			ret = zrn_snap_map_one(zrn_snap_regs[i].cond);
		if (ret)
			return ret;
	}
	zrn_snap_sram_va = ioremap(ZRN_SNAP_SRAM_BASE, ZRN_SNAP_SRAM_SIZE);
	return zrn_snap_sram_va ? 0 : -ENOMEM;
}

/* safe in atomic context: readl only */
static void zrn_snap_take(struct zrn_snap *s)
{
	size_t i;

	for (i = 0; i < ZRN_SNAP_N; i++) {
		const struct zrn_snap_reg *r = &zrn_snap_regs[i];

		s->read[i] = 0;
		if (r->cond && (readl(zrn_snap_va(r->cond)) & r->cmask) != r->cval)
			continue;
		s->regs[i] = readl(zrn_snap_va(r->addr));
		s->read[i] = 1;
	}
	if (s->sram)
		for (i = 0; i < ZRN_SNAP_SRAM_SIZE / 4; i++)
			s->sram[i] = readl(zrn_snap_sram_va + 4 * i);
	s->taken = true;
	s->count++;
}

/* emit through any printf-like sink (seq_printf on both kernels) */
#define ZRN_SNAP_SHOW(m, s, label) do {						\
	size_t _i;								\
	seq_printf(m, "# %s taken=%d count=%lu regs=%zu\n", label,		\
		   (s)->taken, (s)->count, (size_t)ZRN_SNAP_N);			\
	if ((s)->taken) {							\
		for (_i = 0; _i < ZRN_SNAP_N; _i++)				\
			if ((s)->read[_i])					\
				seq_printf(m, "%08x %08x %s\n", zrn_snap_regs[_i].addr, \
					   (s)->regs[_i], zrn_snap_regs[_i].name); \
			else							\
				seq_printf(m, "%08x off      %s\n",		\
					   zrn_snap_regs[_i].addr, zrn_snap_regs[_i].name); \
		for (_i = 0; (s)->sram && _i < ZRN_SNAP_SRAM_SIZE / 4; _i += 4) \
			if ((s)->sram[_i] | (s)->sram[_i + 1] | (s)->sram[_i + 2] | (s)->sram[_i + 3]) \
				seq_printf(m, "sram %08zx %08x %08x %08x %08x\n", \
					   ZRN_SNAP_SRAM_BASE + 4 * _i, (s)->sram[_i], \
					   (s)->sram[_i + 1], (s)->sram[_i + 2], (s)->sram[_i + 3]); \
	}									\
} while (0)

#endif
