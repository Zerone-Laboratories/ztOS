/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zrn-cam, live preview through /dev/zrn-cam. Included by zrn-cam.c.
 *
 *	write "rear" | "front"	power up and stream; "stop" ends it
 *	write "exp C G"		exposure lines and gain code
 *	write "ap15" | "ap24"	the aperture, rear only, inside the module's move limit
 *	read			struct zrn_cam_frame, the newest finished frame
 *	mmap			LIVE_BUFS frames of 16-bit Bayer, one after another
 *
 * It stops by itself: when the last reader closes or stops reading for
 * LIVE_IDLE_S seconds, after LIVE_MAX_S seconds, at LIVE_HOT_MC on the battery
 * sensor, after repeated receiver errors, and at reboot. A thread polls the
 * receiver; no interrupt is requested. Nothing loads this module at boot.
 */
#define LIVE_BUFS	4
#define LIVE_SIZE	(LIVE_BUFS * (size_t)FRAME_AREA)
#define LIVE_IDLE_S	3
#define LIVE_MAX_S	600
#define LIVE_HOT_MC	45000
#define LIVE_ERR_MAX	30

struct zrn_cam_frame {
	__u32 seq, index, w, h, stride, bayer;	/* bayer: 0 GB/RG (rear), 1 GR/BG (front) */
	__u32 errors, stopped;			/* stopped: why, once it has: LS_* */
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
module_param(live_tp, bool, 0644);	/* the sensor's colour bars instead of the room */

static void live_addr(unsigned int i)
{
	unsigned int n;

	for (n = 0; n < 8; n++)
		writel(lower_32_bits(live_dma) + i * FRAME_AREA, csis_vc + VC_ADDR(n));
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

static void live_power_down(void)
{
	power_off(live_cam);
	gpiod_put(reset_gpio);
	reset_gpio = NULL;
}

static int live_thread(void *d)
{
	unsigned long start = jiffies, last_temp = jiffies;
	unsigned int cur = 0, fs = 0, why = LS_RUNNING, errs = 0;
	bool armed = false;

	while (why == LS_RUNNING) {
		u32 s0 = readl(csis + CSIS_SRC0), s1 = readl(csis + CSIS_SRC1), dm;

		if (s0)
			writel(s0, csis + CSIS_SRC0);
		if (s1)
			writel(s1, csis + CSIS_SRC1);
		if (s0 & BIT(24))
			fs++;
		if ((s0 & 0x000fff1f) || (s1 & 0xfff0))
			errs++;
		dm = readl(csis_vc + CMN_SRC);
		if (dm)
			writel(dm, csis_vc + CMN_SRC);
		if (dm & BIT(12) || dm & 0x00f00000)
			errs++;
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
			cur = (cur + 1) % LIVE_BUFS;
			live_addr(cur);
		}
		if (READ_ONCE(live_stop_asked) || stop_now)
			why = LS_ASKED;
		else if (time_after(jiffies, READ_ONCE(live_read_at) + LIVE_IDLE_S * HZ))
			why = LS_IDLE;
		else if (time_after(jiffies, start + LIVE_MAX_S * HZ))
			why = LS_TIME;
		else if (errs > LIVE_ERR_MAX)
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

static int live_start(bool front)
{
	const struct csi_path *p = front ? &path_front : &path_rear;
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
	if (front) {
		ret = cis_table(tb_front_1632x1224_30, ARRAY_SIZE(tb_front_1632x1224_30));
	} else {
		ret = cis_table(tb_reset_tnp, ARRAY_SIZE(tb_reset_tnp));
		if (!ret)
			ret = cis_table(tb_global, ARRAY_SIZE(tb_global));
		if (!ret)
			ret = cis_table(tb_mode_2016x1512_30, ARRAY_SIZE(tb_mode_2016x1512_30));
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
	if (!strcmp(cmd, "rear") || !strcmp(cmd, "front")) {
		live_stop();
		ret = live_start(cmd[0] == 'f');
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
			ret = act_bus_on();
			if (!ret)
				ret = ap_init();
			if (!ret)
				ret = ap_move(cmd[2] == '1' ? AP_F1_5 : AP_F2_4, cmd);
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

static const struct file_operations live_fops = {
	.owner = THIS_MODULE, .write = live_write, .read = live_read,
	.mmap = live_mmap, .release = live_release,
};
static struct miscdevice live_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "zrn-cam", .fops = &live_fops, .mode = 0660,
};
