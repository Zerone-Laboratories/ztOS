#ifndef ZRN_SCORE_BUF_H
#define ZRN_SCORE_BUF_H
#include <linux/ioctl.h>
#include <linux/types.h>
/* ztOS: a buffer for SCORE (see zrn_score_buf.c): in the size, out the fd */
struct zrn_score_alloc {
	unsigned long long	size;
	int			fd;
	int			reserved;
};
#define ZRN_SCORE_IOC_ALLOC	_IOWR('S', 32, struct zrn_score_alloc)
struct device;
struct dma_buf;
int zrn_score_buf_alloc(struct device *dev, size_t size);
bool zrn_score_buf_dvaddr(struct dma_buf *dbuf, dma_addr_t *dvaddr);
#endif
