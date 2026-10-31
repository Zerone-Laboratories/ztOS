/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zrn-cam, live preview through /dev/zrn-cam. Included by zrn-cam.c.
 *
 *	write "rear" | "front"	power up and stream; "stop" ends it
 *	write "rearfull" | "frontfull"	the same at the sensor's full size, for a still
 *	write "af N"		focus lens position 0-511, by stock's first-position steps
 *	write "exp C G"		exposure lines and gain code
 *	write "ap15" | "ap24"	the aperture, rear only, inside the module's move limit
 *	read			struct zrn_cam_frame, the newest finished frame
 *	mmap			frames of 16-bit Bayer, one after another, info.area bytes apart
 *	ioctl ZRN_CAM_DMABUF	the same memory as a DMA-BUF, for the GPU to read directly
 *
 * It stops by itself: when the last reader closes or stops reading for
 * LIVE_IDLE_S seconds, after LIVE_MAX_S seconds, at LIVE_HOT_MC on the battery
 * sensor, after repeated receiver errors, and at reboot. A thread polls the
 * receiver; no interrupt is requested. Nothing loads this module at boot.
 */
#include <linux/dma-buf.h>
#define LIVE_BUFS	4
#define LIVE_FULL_BUFS	2
#define LIVE_FULL_AREA	((size_t)FULL_W * 2 * FULL_H)
#define LIVE_SIZE	(LIVE_FULL_BUFS * LIVE_FULL_AREA)	/* preview's four fit in this */
#define ZRN_CAM_DMABUF	_IO('Z', 1)
#define LIVE_IDLE_S	3
#define LIVE_MAX_S	600
#define LIVE_HOT_MC	45000
#define LIVE_ERR_MAX	30	/* in LIVE_ERR_WINDOW_S: a rare overflow under load is not fatal */
#define LIVE_ERR_WINDOW_S	5
#define LIVE_ERR_LOGS	8

struct zrn_cam_frame {
	__u32 seq, index, w, h, stride, bayer;	/* bayer: 0 GB/RG (rear), 1 GR/BG (front) */
	__u32 errors, stopped;			/* stopped: why, once it has: LS_* */
	__u32 area;				/* bytes from one frame's start to the next */
};
enum { LS_RUNNING, LS_ASKED, LS_IDLE, LS_TIME, LS_HOT, LS_ERRORS, LS_FAILED };

static struct task_struct *live_task;
static DECLARE_COMPLETION(live_done);
static void *live_buf;
static dma_addr_t live_dma;
static const struct camera *live_cam;
static struct zrn_cam_frame live_info;
static DEFINE_SPINLOCK(live_slock);
static unsigned long live_read_at;
static bool live_stop_asked;
static bool live_tp;
static size_t live_area = FRAME_AREA;
static unsigned int live_bufs = LIVE_BUFS;
static bool af_up;
static int af_at;
module_param(live_tp, bool, 0644);	/* the sensor's colour bars instead of the room */

static void live_addr(unsigned int i)
{
	unsigned int n;

	for (n = 0; n < 8; n++)
		writel(lower_32_bits(live_dma) + i * live_area, csis_vc + VC_ADDR(n));
}

static int battery_mc(void)
{
	static struct thermal_zone_device *tz;
	int t = 0;

	if (!tz) {
		tz = thermal_zone_get_zone_by_name("max170xx_battery");
		if (IS_ERR(tz)) {
			tz = NULL;
			return 0;
		}
	}
	return thermal_zone_get_temp(tz, &t) ? 0 : t;
}

/* Land the lens as fimc_is_sensor_peri_actuator_softlanding does, in steps. */
static void af_down(void)
{
	if (!af_up)
		return;
	while (af_at > 40) {
		af_at -= 40;
		af_position(af_at);
		msleep(10);
	}
	af_position(0);
	msleep(30);
	act_bus_off();
	af_up = false;
}

/* Under the lock, camera streaming. Stock's order: first position, then active mode. */
static int af_goto(int pos)
{
	int ret;

	if (!af_up) {
		act_select(live_cam == &cameras[1]);
		ret = act_bus_on();
		if (ret) {
			act_bus_off();
			return ret;
		}
		if (!af_known()) {
			act_bus_off();
			return -ENODEV;
		}
		ret = af_position(AF_FIRST);
		usleep_range(2000, 2100);
		if (!ret)
			ret = act_write(af_cl, 0x02, 0x00);
		if (ret) {
			act_bus_off();
			return ret;
		}
		af_at = AF_FIRST;
		af_up = true;
	}
	af_at = pos;
	return af_position(pos);
}

static void live_power_down(void)
{
	power_off(live_cam);
	gpiod_put(reset_gpio);
	reset_gpio = NULL;
}

static int live_thread(void *d)
{
	unsigned long start = jiffies, last_temp = jiffies, err_from = jiffies;
	unsigned int cur = 0, fs = 0, why = LS_RUNNING, errs = 0, recent = 0, logged = 0;
	bool armed = false;

	while (why == LS_RUNNING) {
		u32 s0 = readl(csis + CSIS_SRC0), s1 = readl(csis + CSIS_SRC1), dm;

		if (s0)
			writel(s0, csis + CSIS_SRC0);
		if (s1)
			writel(s1, csis + CSIS_SRC1);
		if (s0 & BIT(24))
			fs++;
		dm = readl(csis_vc + CMN_SRC);
		if (dm)
			writel(dm, csis_vc + CMN_SRC);
		if ((s0 & 0x000fff1f) || (s1 & 0xfff0) || (dm & BIT(12)) || (dm & 0x00f00000)) {
			errs++;
			recent++;
			if (logged++ < LIVE_ERR_LOGS)
				pr_info("zrn-cam: error %u at frame %u: src0 %08x src1 %08x dma %08x\n",
					errs, live_info.seq, s0, s1, dm);
		}
		if (time_after(jiffies, err_from + LIVE_ERR_WINDOW_S * HZ)) {
			err_from = jiffies;
			recent = 0;
		}
		if (!armed && fs >= 2) {
			writel(BIT(0) | BIT(1), csis_vc + VC_CTRL);
			armed = true;
		}
		if (dm & BIT(8)) {	/* a frame finished: publish it, the next goes elsewhere */
			spin_lock(&live_slock);
			live_info.seq++;
			live_info.index = cur;
			live_info.errors = errs;
			spin_unlock(&live_slock);
			cur = (cur + 1) % live_bufs;
			live_addr(cur);
		}
		if (READ_ONCE(live_stop_asked) || stop_now)
			why = LS_ASKED;
		else if (time_after(jiffies, READ_ONCE(live_read_at) + LIVE_IDLE_S * HZ))
			why = LS_IDLE;
		else if (time_after(jiffies, start + LIVE_MAX_S * HZ))
			why = LS_TIME;
		else if (recent > LIVE_ERR_MAX)
			why = LS_ERRORS;
		else if (time_after(jiffies, last_temp + 2 * HZ)) {
			last_temp = jiffies;
			if (battery_mc() >= LIVE_HOT_MC)
				why = LS_HOT;
		}
		usleep_range(2000, 3000);
	}
	dma_stop();
	mutex_lock(&lock);
	af_down();
	cis_write8(0x0100, 0);
	msleep(50);
	csis_off();
	live_power_down();
	__pm_relax(stream_ws);
	pr_info("zrn-cam: live stopped: %u, %u frames, %u errors\n", why, live_info.seq, errs);
	spin_lock(&live_slock);
	live_info.stopped = why;
	spin_unlock(&live_slock);
	live_task = NULL;
	mutex_unlock(&lock);
	complete_all(&live_done);
	return 0;
}

static int live_start(bool front, bool full)
{
	const struct csi_path *p = front ? (full ? &path_front_full : &path_front)
					 : (full ? &path_rear_full : &path_rear);
	struct task_struct *t;
	int ret;

	mutex_lock(&lock);
	if (live_task) {
		ret = -EBUSY;
		goto out;
	}
	result_len = 0;
	result[0] = 0;
	if (!top_cmu)
		top_cmu = ioremap(0x1a240000, 0x3000);
	ret = pmic_buses();
	if (!ret)
		ret = csis_map(p);
	if (ret)
		goto out;
	if (readl(csis + CSIS_VERSION) != 0x05010000) {
		ret = -ENODEV;
		goto out;
	}
	if (!live_buf) {
		live_buf = dma_alloc_coherent(ctl_dev, LIVE_SIZE, &live_dma, GFP_KERNEL);
		if (!live_buf) {
			ret = -ENOMEM;
			goto out;
		}
	}
	ret = apm_set(7, INTCAM_KHZ);
	if (ret)
		goto out;
	live_cam = front ? &cameras[1] : &cameras[0];
	reset_gpio = gpiod_get(ctl_dev, front ? "reset-front" : "reset-rear", GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio)) {
		ret = PTR_ERR(reset_gpio);
		reset_gpio = NULL;
		goto out;
	}
	ret = power_on(live_cam, false);
	if (ret)
		goto off;
	live_area = full ? LIVE_FULL_AREA : FRAME_AREA;
	live_bufs = full ? LIVE_FULL_BUFS : LIVE_BUFS;
	if (front && full) {
		ret = cis_table(tb_front_3264x2448_30, ARRAY_SIZE(tb_front_3264x2448_30));
	} else if (front) {
		ret = cis_table(tb_front_1632x1224_30, ARRAY_SIZE(tb_front_1632x1224_30));
	} else {
		ret = cis_table(tb_reset_tnp, ARRAY_SIZE(tb_reset_tnp));
		if (!ret)
			ret = cis_table(tb_global, ARRAY_SIZE(tb_global));
		if (!ret)
			ret = full ? cis_table(tb_mode_4032x3024_30, ARRAY_SIZE(tb_mode_4032x3024_30))
				   : cis_table(tb_mode_2016x1512_30, ARRAY_SIZE(tb_mode_2016x1512_30));
	}
	if (!ret && live_tp)
		ret = cis_write8(0x0601, 2);
	if (ret)
		goto off;
	__pm_stay_awake(stream_ws);
	writel(0, csis_vc + VC_FMT);
	writel(p->w, csis_vc + VC_RESOL);
	live_addr(0);
	writel(0, csis_vc + CMN_OTF);
	writel(0, csis_vc + CMN_DATA);
	writel(0x00f11f00, csis_vc + CMN_MASK);
	writel(readl(csis_vc + CMN_SRC), csis_vc + CMN_SRC);
	csis_on();
	cis_write8(0x0104, 1);
	if (front) {
		cis_write16(0x6028, 0x4000);
		/* the full tables leave WDR on: line by line a long exposure stock sets, then
		 * sensor_3h1_cis_set_exposure_time turns it off for each frame. */
		cis_write16(0x0216, 0x0000);
		ret = cis_write8(0x0100, 1);
	} else {
		ret = cis_write16(0x0100, 0x0103);
	}
	cis_write8(0x0104, 0);
	if (ret) {
		csis_off();
		__pm_relax(stream_ws);
		goto off;
	}
	memset(&live_info, 0, sizeof(live_info));
	live_info.w = p->w;
	live_info.h = p->h;
	live_info.stride = p->w * 2;
	live_info.bayer = front;
	live_info.area = live_area;
	live_stop_asked = false;
	live_read_at = jiffies;
	reinit_completion(&live_done);
	t = kthread_create(live_thread, NULL, "zrn-cam-live");
	if (IS_ERR(t)) {
		ret = PTR_ERR(t);
		cis_write8(0x0100, 0);
		csis_off();
		__pm_relax(stream_ws);
		goto off;
	}
	live_task = t;
	wake_up_process(t);
	goto out;
off:
	live_power_down();
	live_info.stopped = LS_FAILED;
out:
	mutex_unlock(&lock);
	return ret;
}

static void live_stop(void)
{
	WRITE_ONCE(live_stop_asked, true);
	if (READ_ONCE(live_task))
		wait_for_completion_timeout(&live_done, 3 * HZ);
}

static ssize_t live_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	char cmd[32];
	size_t n = min(len, sizeof(cmd) - 1);
	u32 c, g;
	int ret = 0;

	if (copy_from_user(cmd, ubuf, n))
		return -EFAULT;
	cmd[n] = 0;
	strim(cmd);
	if (!strcmp(cmd, "rear") || !strcmp(cmd, "front") ||
	    !strcmp(cmd, "rearfull") || !strcmp(cmd, "frontfull")) {
		live_stop();
		ret = live_start(cmd[0] == 'f', strstr(cmd, "full") != NULL);
	} else if (sscanf(cmd, "af %u", &c) == 1) {
		mutex_lock(&lock);
		if (live_task && live_cam && c <= 511)
			ret = af_goto(c);
		else
			ret = -EINVAL;
		mutex_unlock(&lock);
	} else if (!strcmp(cmd, "stop")) {
		live_stop();
	} else if (sscanf(cmd, "exp %u %u", &c, &g) == 2) {
		mutex_lock(&lock);
		if (live_task && sensor && c && c < 0x8000 && g >= 0x20 && g <= 0x200) {
			cis_write8(0x0104, 1);
			cis_write16(0x0202, c);
			cis_write16(0x0204, g);
			cis_write8(0x0104, 0);
		} else {
			ret = -EINVAL;
		}
		mutex_unlock(&lock);
	} else if (!strcmp(cmd, "ap15") || !strcmp(cmd, "ap24")) {
		mutex_lock(&lock);
		if (live_task && live_cam == &cameras[0]) {
			if (!af_up) {
				act_select(false);
				ret = act_bus_on();
			}
			if (!ret)
				ret = ap_init();
			if (!ret)
				ret = ap_move(cmd[2] == '1' ? AP_F1_5 : AP_F2_4, cmd);
			if (!af_up)
				act_bus_off();
		} else {
			ret = -EINVAL;
		}
		mutex_unlock(&lock);
	} else {
		ret = -EINVAL;
	}
	return ret ? ret : len;
}

static ssize_t live_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	struct zrn_cam_frame i;

	if (len < sizeof(i))
		return -EINVAL;
	WRITE_ONCE(live_read_at, jiffies);
	spin_lock(&live_slock);
	i = live_info;
	spin_unlock(&live_slock);
	return copy_to_user(ubuf, &i, sizeof(i)) ? -EFAULT : sizeof(i);
}

static int live_mmap(struct file *f, struct vm_area_struct *vma)
{
	if (!live_buf || vma->vm_pgoff || vma->vm_end - vma->vm_start > PAGE_ALIGN(LIVE_SIZE))
		return -EINVAL;
	return dma_mmap_coherent(ctl_dev, vma, live_buf, live_dma, vma->vm_end - vma->vm_start);
}

static int live_release(struct inode *i, struct file *f)
{
	live_stop();
	return 0;
}

static struct sg_table *zc_map(struct dma_buf_attachment *at, enum dma_data_direction dir)
{
	struct sg_table *sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	int ret;

	if (!sgt)
		return ERR_PTR(-ENOMEM);
	ret = dma_get_sgtable(ctl_dev, sgt, live_buf, live_dma, LIVE_SIZE);
	if (!ret)
		ret = dma_map_sgtable(at->dev, sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}
	return sgt;
}

static void zc_unmap(struct dma_buf_attachment *at, struct sg_table *sgt, enum dma_data_direction dir)
{
	dma_unmap_sgtable(at->dev, sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

static void zc_release(struct dma_buf *b)
{
}

static int zc_mmap(struct dma_buf *b, struct vm_area_struct *vma)
{
	return dma_mmap_coherent(ctl_dev, vma, live_buf, live_dma, vma->vm_end - vma->vm_start);
}

static const struct dma_buf_ops zc_ops = {
	.map_dma_buf = zc_map, .unmap_dma_buf = zc_unmap, .release = zc_release, .mmap = zc_mmap,
};

static long live_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct dma_buf *b;
	int fd;

	if (cmd != ZRN_CAM_DMABUF)
		return -ENOTTY;
	if (!live_buf)
		return -EINVAL;
	info.ops = &zc_ops;
	info.priv = live_buf;
	info.size = LIVE_SIZE;
	info.flags = O_RDWR | O_CLOEXEC;
	b = dma_buf_export(&info);
	if (IS_ERR(b))
		return PTR_ERR(b);
	fd = dma_buf_fd(b, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(b);
	return fd;
}

static const struct file_operations live_fops = {
	.owner = THIS_MODULE, .write = live_write, .read = live_read,
	.mmap = live_mmap, .release = live_release, .unlocked_ioctl = live_ioctl,
};
static struct miscdevice live_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "zrn-cam", .fops = &live_fops, .mode = 0660,
};
