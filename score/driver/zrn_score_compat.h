/* What Samsung's vendor kernel (4.9) had and ztOS's 7.1 kernel does not,
 * filled in here so the driver's own files stay close to Samsung's.
 * struct timespec: 64-bit time on arm64, so timespec64 has the same layout
 * the ioctl ABI (score_ioctl.h timestamps) always had. */
#ifndef ZRN_SCORE_COMPAT_H
#define ZRN_SCORE_COMPAT_H
#include <linux/types.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#define timespec		timespec64
#define getrawmonotonic		ktime_get_raw_ts64
#define timespec_sub		timespec64_sub
#define timespec_to_ns		timespec64_to_ns
#ifndef del_timer_sync
#define del_timer_sync		timer_delete_sync
#endif

/* dma_buf_kmap() went away: the whole buffer mapped instead (page 0 was the
 * only one Samsung's driver ever asked for, of buffers it then used whole) */
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
static inline void *zrn_dma_buf_kmap(struct dma_buf *dbuf, unsigned long page)
{
	struct iosys_map map;

	if (dma_buf_vmap_unlocked(dbuf, &map))
		return NULL;
	return map.vaddr;
}
static inline void zrn_dma_buf_kunmap(struct dma_buf *dbuf, unsigned long page, void *vaddr)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(vaddr);

	dma_buf_vunmap_unlocked(dbuf, &map);
}
#define dma_buf_kmap	zrn_dma_buf_kmap
#define dma_buf_kunmap	zrn_dma_buf_kunmap
#endif
