/*
 * Samsung Exynos SoC series SCore driver
 *
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/dma-buf.h>

#include "score_log.h"
#include "score_regs.h"
#include "score_mem_table.h"
#include "score_system.h"
#include "score_mmu.h"
#include "score_memory.h"
#include "zrn_score_buf.h"

int score_memory_kmap_packet(struct score_memory *mem,
		struct score_mmu_packet *packet)
{
	int ret = 0;
	int fd;
	struct dma_buf *dbuf;
	void *kvaddr;

	score_enter();
	fd = packet->fd;
	if (fd <= 0) {
		score_err("fd(%d) is invalid\n", fd);
		ret = -EINVAL;
		goto p_err;
	}

	dbuf = dma_buf_get(fd);
	if (IS_ERR_OR_NULL(dbuf)) {
		score_err("dma_buf is invalid (fd:%d)\n", fd);
		ret =  -EINVAL;
		goto p_err;
	}
	packet->dbuf = dbuf;
	packet->size = dbuf->size;

	ret = dma_buf_begin_cpu_access(dbuf, DMA_NONE);
	if (ret) {
		score_err("accessing at cpu failed (dbuf:%p)\n", dbuf);
		goto p_err_begin_cpu_access;
	}

	kvaddr = dma_buf_kmap(dbuf, 0);
	if (!packet) {
		score_err("kmap failed (dbuf:%p)\n", dbuf);
		ret = -EFAULT;
		goto p_err_kmap;
	}

	packet->kvaddr = kvaddr;

	score_leave();
	return ret;
p_err_kmap:
	dma_buf_end_cpu_access(dbuf, 0);
p_err_begin_cpu_access:
	dma_buf_put(dbuf);
p_err:
	return ret;
}

void score_memory_kunmap_packet(struct score_memory *mem,
		struct score_mmu_packet *packet)
{
	struct dma_buf *dbuf;
	void *kvaddr;

	score_enter();
	dbuf = packet->dbuf;
	kvaddr = packet->kvaddr;

	dma_buf_kunmap(dbuf, 0, kvaddr);
	dma_buf_end_cpu_access(dbuf, DMA_NONE);
	dma_buf_put(dbuf);

	score_leave();
}

/**
 * score_memory_get_internal_mem_kvaddr - Get kvaddr of
 *	kernel space memory allocated for SCore device
 * @mem:	[in]	object about score_memory structure
 * @kvaddr:	[out]	kernel virtual address
 */
void *score_memory_get_internal_mem_kvaddr(struct score_memory *mem)
{
	score_enter();
	return mem->kvaddr;
}

/**
 * score_memory_get_internal_mem_dvaddr - Get dvaddr of
 *	kernel space memory allocated for SCore device
 * @mem:	[in]	object about score_memory structure
 * @dvaddr:	[out]	device virtual address
 */
dma_addr_t score_memory_get_internal_mem_dvaddr(struct score_memory *mem)
{
	score_check();
	return mem->dvaddr;
}

/**
 * score_memory_get_profiler_kvaddr - Get MC kvaddr and dvaddr of
 *	kernel space memory allocated for SCore device
 * @mem:	[in]	object about score_memory structure
 * @id:         [in]    id of requested core memory
 * @kvaddr:	[out]	kernel virtual address
 */
void *score_memory_get_profiler_kvaddr(struct score_memory *mem,
							unsigned int id)
{
	score_check();
	if (id == SCORE_MASTER)
		return mem->profiler_mc_kvaddr;
	else if (id == SCORE_KNIGHT1)
		return mem->profiler_kc1_kvaddr;
	else if (id == SCORE_KNIGHT2)
		return mem->profiler_kc2_kvaddr;
	return 0;
}

/**
 * score_memory_get_print_kvaddr - Get kvaddr of
 *	kernel space memory allocated for SCore device
 * @mem:	[in]	object about score_memory structure
 * @kvaddr:	[out]	kernel virtual address
 */
void *score_memory_get_print_kvaddr(struct score_memory *mem)
{
	score_check();
	return mem->print_kvaddr;
}

static int score_zynq_memory_map_dmabuf(struct score_mmu *mmu,
		struct score_mmu_buffer *buf)
{
	int ret = 0;
	struct score_memory *mem;
	struct dma_buf *dbuf;
	void *kvaddr;

	score_enter();
	mem = &mmu->mem;
	dbuf = buf->dbuf;

	/* ztOS: our own buffers (ZRN_SCORE_IOC_ALLOC) the DSP reads where they are */
	if (zrn_score_buf_dvaddr(dbuf, &buf->dvaddr)) {
		buf->kvaddr = NULL;	/* nothing to copy back either */
		return 0;
	}

	ret = dma_buf_begin_cpu_access(dbuf, DMA_BIDIRECTIONAL);
	if (ret) {
		score_err("accessing at cpu failed (dbuf:%p)\n", dbuf);
		goto p_err_begin_cpu_access;
	}

	kvaddr = dma_buf_kmap(dbuf, 0);
	if (!kvaddr) {
		score_err("kmap failed (dbuf:%p)\n", dbuf);
		ret = -EFAULT;
		goto p_err_kmap;
	}

	mutex_lock(&mem->lock);
	if ((mem->used_image_buf_size + buf->size) > mem->image_mem_size)
		mem->used_image_buf_size = 0;

	buf->dvaddr = mem->image_paddr + mem->used_image_buf_size;
	buf->kvaddr = mem->image_kvaddr + mem->used_image_buf_size;
	memcpy(buf->kvaddr, kvaddr, buf->size);

	mem->used_image_buf_size += buf->size;
	mutex_unlock(&mem->lock);
	dma_buf_kunmap(dbuf, 0, kvaddr);

	score_leave();
	return ret;
p_err_kmap:
	dma_buf_end_cpu_access(dbuf, DMA_BIDIRECTIONAL);
p_err_begin_cpu_access:
	return ret;
}

/* ztOS: the result went back at the frame's completion
 * (zrn_score_memory_sync_back). Copying again here, from the freelist worker
 * whenever it runs, wrote an old request's data over the next request's input;
 * and the pool is a ring (map_dmabuf wraps it), not a stack: freeing out of
 * order had new buffers overlap ones still in use. */
static int score_zynq_memory_unmap_dmabuf(struct score_mmu *mmu,
		struct score_mmu_buffer *buf)
{
	dma_addr_t ours;

	score_enter();
	if (!zrn_score_buf_dvaddr(buf->dbuf, &ours))
		dma_buf_end_cpu_access(buf->dbuf, DMA_BIDIRECTIONAL);
	buf->dvaddr = 0;
	buf->kvaddr = NULL;
	score_leave();
	return 0;
}

/* ztOS: copy a buffer's pool copy back to its dma-buf as soon as its frame is done;
 * the freelist worker unmaps it later, after user space has read it */
void zrn_score_memory_sync_back(struct score_mmu_buffer *buf)
{
	void *kvaddr;

	if (!buf->kvaddr || !buf->dbuf)
		return;
	kvaddr = dma_buf_kmap(buf->dbuf, 0);
	if (!kvaddr)
		return;
	memcpy(kvaddr, buf->kvaddr, buf->size);
	dma_buf_kunmap(buf->dbuf, 0, kvaddr);
}

const struct score_mmu_ops score_zynq_mmu_ops = {
	.map_dmabuf	= score_zynq_memory_map_dmabuf,
	.unmap_dmabuf	= score_zynq_memory_unmap_dmabuf,
//	.map_userptr	= score_zynq_memory_map_dmabuf,
//	.unmap_userptr	= score_zynq_memory_unmap_dmabuf,
};

/*
 * ztOS: SCORE's own IOMMUs are left off (the bootloader leaves them so), so
 * the DSP sees physical addresses: its memory is ordinary DMA memory below
 * 4 GB. One block for the firmware, heaps, log and profilers, one for the
 * images (user buffers are copied in and out, as on Samsung's FPGA board).
 */
#define ZRN_SCORE_IMAGE_SIZE	SZ_16M

static int __score_memory_alloc(struct score_memory *mem)
{
	int ret = 0;
	dma_addr_t image;

	score_enter();
	mem->size = SCORE_MEMORY_INTERNAL_SIZE;
	mem->kvaddr = dma_alloc_coherent(mem->dev, mem->size, &mem->dvaddr, GFP_KERNEL);
	if (!mem->kvaddr) {
		score_err("no memory for the firmware (%zu bytes)\n", (size_t)mem->size);
		ret = -ENOMEM;
		mem->image_kvaddr = NULL;
		goto p_err;
	}

	mem->image_mem_size = ZRN_SCORE_IMAGE_SIZE;
	mem->image_kvaddr = dma_alloc_coherent(mem->dev, mem->image_mem_size, &image, GFP_KERNEL);
	if (!mem->image_kvaddr) {
		score_err("no memory for images\n");
		ret = -ENOMEM;
		goto p_err_image;
	}
	mem->image_paddr = (unsigned int)image;
	mem->used_image_buf_size = 0;

	mem->malloc_dvaddr = mem->dvaddr + SCORE_FW_SIZE;
	mem->malloc_kvaddr = mem->kvaddr + SCORE_FW_SIZE;

	mem->print_dvaddr = mem->malloc_dvaddr + SCORE_MALLOC_SIZE;
	mem->print_kvaddr = mem->malloc_kvaddr + SCORE_MALLOC_SIZE;

	mem->profiler_mc_dvaddr = mem->print_dvaddr + SCORE_PRINT_SIZE;
	mem->profiler_mc_kvaddr = mem->print_kvaddr + SCORE_PRINT_SIZE;
	mem->profiler_kc1_dvaddr =
			mem->profiler_mc_dvaddr + SCORE_MC_PROFILER_SIZE;
	mem->profiler_kc1_kvaddr =
			mem->profiler_mc_kvaddr + SCORE_MC_PROFILER_SIZE;
	mem->profiler_kc2_dvaddr =
			mem->profiler_kc1_dvaddr + SCORE_KC_PROFILER_SIZE;
	mem->profiler_kc2_kvaddr =
			mem->profiler_kc1_kvaddr + SCORE_KC_PROFILER_SIZE;
	score_info("memory: firmware %pad (%zu), images %pad (%zu)\n", &mem->dvaddr,
		(size_t)mem->size, &image, (size_t)mem->image_mem_size);

	score_leave();
	return ret;
p_err_image:
	dma_free_coherent(mem->dev, mem->size, mem->kvaddr, mem->dvaddr);
	mem->kvaddr = NULL;
p_err:
	return ret;
}

static void __score_memory_free(struct score_memory *mem)
{
	score_enter();
	if (mem->image_kvaddr)
		dma_free_coherent(mem->dev, mem->image_mem_size, mem->image_kvaddr, mem->image_paddr);
	if (mem->kvaddr)
		dma_free_coherent(mem->dev, mem->size, mem->kvaddr, mem->dvaddr);
	mem->image_kvaddr = NULL;
	mem->kvaddr = NULL;
	score_leave();
}

void score_memory_init(struct score_memory *mem)
{
	void __iomem *sfr;
	dma_addr_t kc1_malloc, kc2_malloc;

	score_enter();
	sfr = mem->system->sfr;
	kc1_malloc = mem->malloc_dvaddr + SCORE_MC_MALLOC_SIZE;
	kc2_malloc = kc1_malloc + SCORE_KC_MALLOC_SIZE;

	writel(mem->malloc_dvaddr,   sfr + MC_MALLOC_BASE_ADDR);
	writel(SCORE_MC_MALLOC_SIZE, sfr + MC_MALLOC_SIZE);
	writel(kc1_malloc,           sfr + KC1_MALLOC_BASE_ADDR);
	writel(kc2_malloc,           sfr + KC2_MALLOC_BASE_ADDR);
	writel(SCORE_KC_MALLOC_SIZE, sfr + KC_MALLOC_SIZE);
	writel(mem->print_dvaddr,    sfr + PRINT_BASE_ADDR);
	writel(SCORE_PRINT_SIZE,     sfr + PRINT_SIZE);
	writel(SCORE_MC_PROFILER_SIZE,  sfr + PROFILER_MC_SIZE);
	writel(SCORE_KC_PROFILER_SIZE,  sfr + PROFILER_KC_SIZE);
	writel(mem->profiler_mc_dvaddr, sfr + PROFILER_MC_BASE_ADDR);
	writel(mem->profiler_kc1_dvaddr, sfr + PROFILER_KC1_BASE_ADDR);
	writel(mem->profiler_kc2_dvaddr, sfr + PROFILER_KC2_BASE_ADDR);

	score_leave();
}

int score_memory_open(struct score_memory *mem)
{
	int ret = 0;

	score_enter();
	ret = __score_memory_alloc(mem);
	score_leave();
	return ret;
}

void score_memory_close(struct score_memory *mem)
{
	score_enter();
	__score_memory_free(mem);
	score_leave();
}

int score_memory_probe(struct score_mmu *mmu)
{
	struct score_memory *mem;

	score_enter();
	mem = &mmu->mem;
	mem->dev = mmu->system->dev;
	mem->system = mmu->system;
	mmu->mmu_ops = &score_zynq_mmu_ops;
	mutex_init(&mem->lock);
	score_leave();
	return 0;
}

void score_memory_remove(struct score_memory *mem)
{
	score_enter();
	mutex_destroy(&mem->lock);
	score_leave();
}
