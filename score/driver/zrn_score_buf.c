// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-score: dma-bufs for SCORE's packets and buffers. Samsung's userspace
 * got them from ION; this kernel has no ION and no dma-buf heaps.
 *
 * They are DMA-coherent memory the DSP can address itself (its SYSMMUs are
 * off: it works in physical addresses), mapped into user space as they are.
 * So the driver needs no copy for them: a request names the buffer, and the
 * DSP reads and writes it where it lies (zrn_score_buf_dvaddr, used by
 * score_memory.c). Other dma-bufs still go through the copy pool. The first
 * version was vmalloc memory, copied in and out on every request: 0.42 ms of
 * processor time a prediction for ztOSIntelligentBoost, eighty times what the
 * network costs on the processor itself (2026-10-25).
 */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "zrn_score_buf.h"

struct zrn_buf {
	struct device *dev;
	void *vaddr;
	dma_addr_t dma;
	size_t size;
};

static struct sg_table *zrn_map(struct dma_buf_attachment *a, enum dma_data_direction dir)
{
	return ERR_PTR(-EOPNOTSUPP);	/* never attached: the driver asks for the address */
}

static void zrn_unmap(struct dma_buf_attachment *a, struct sg_table *sgt, enum dma_data_direction dir)
{
}

static void zrn_release(struct dma_buf *dbuf)
{
	struct zrn_buf *b = dbuf->priv;

	dma_free_coherent(b->dev, b->size, b->vaddr, b->dma);
	kfree(b);
}

static int zrn_mmap(struct dma_buf *dbuf, struct vm_area_struct *vma)
{
	struct zrn_buf *b = dbuf->priv;

	return dma_mmap_coherent(b->dev, vma, b->vaddr, b->dma, b->size);
}

static int zrn_vmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	struct zrn_buf *b = dbuf->priv;

	iosys_map_set_vaddr(map, b->vaddr);
	return 0;
}

static const struct dma_buf_ops zrn_buf_ops = {
	.map_dma_buf = zrn_map,
	.unmap_dma_buf = zrn_unmap,
	.release = zrn_release,
	.mmap = zrn_mmap,
	.vmap = zrn_vmap,
};

/* a new buffer of `size` bytes the DSP can reach, zeroed; returns its fd */
int zrn_score_buf_alloc(struct device *dev, size_t size)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct zrn_buf *b;
	struct dma_buf *dbuf;
	int fd;

	if (!size || size > SZ_16M)
		return -EINVAL;
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->size = PAGE_ALIGN(size);
	b->vaddr = dma_alloc_coherent(dev, b->size, &b->dma, GFP_KERNEL);
	if (!b->vaddr) {
		kfree(b);
		return -ENOMEM;
	}
	memset(b->vaddr, 0, b->size);
	info.ops = &zrn_buf_ops;
	info.size = b->size;
	info.flags = O_RDWR;
	info.priv = b;
	dbuf = dma_buf_export(&info);
	if (IS_ERR(dbuf)) {
		dma_free_coherent(dev, b->size, b->vaddr, b->dma);
		kfree(b);
		return PTR_ERR(dbuf);
	}
	fd = dma_buf_fd(dbuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dbuf);
	return fd;
}

/* one of ours: the address the DSP uses for it (true), or not ours (false) */
bool zrn_score_buf_dvaddr(struct dma_buf *dbuf, dma_addr_t *dvaddr)
{
	struct zrn_buf *b;

	if (!dbuf || dbuf->ops != &zrn_buf_ops)
		return false;
	b = dbuf->priv;
	*dvaddr = b->dma;
	return true;
}
