// SPDX-License-Identifier: GPL-2.0
/*
 * The Galaxy S9's vibration motor under mainline, as a rumble input device.
 *
 * The motor is a linear resonant actuator driven by the MAX77705's motor
 * driver, which takes a PWM from the SoC and divides it by 128: 20.48 kHz in,
 * the motor's 160 Hz out. The power chip's core driver (MFD_MAX77705) already
 * makes a "max77705-haptic" platform device; mainline's own driver for it
 * (max77693-haptic) wants a PWM and a regulator from a device tree that
 * describes neither here (there is no PWM controller in ours), so this one
 * does the little there is to do by itself, from the stock tree's values
 * (stock/twrp-live.dts: haptic, vib-pwm, motor-boost-en):
 *
 *  - the PWM is the SoC's timer 1 (0x10510000), out on gpp3-1 (function 2);
 *    its clock is the 26 MHz oscillator (the boot loader leaves a prescaler
 *    of 26 there), so 1270 counts are the stock period of 48828 ns;
 *  - the motor's supply is a boost switched on by gpg1-7;
 *  - MCONFIG (0x10) = 0x82: LRA, PWM divided by 128; bit 6 runs the motor;
 *    MAINCTRL1 (0x02) bit 7 is the driver's bias.
 *
 * The stock duty is 70 % of the period; a rumble's strength scales it.
 *
 * Timing and shape (v2). The first version ran on the kernel's ff-memless,
 * which times a pulse in jiffies (4 ms at HZ=250) from a timer and starts the
 * motor from an ordinary work item: a 7 ms key tick came out 4 to 16 ms long
 * and the motor, a mass on a spring, was still ringing 30 ms after it. This
 * version plays a pulse itself, from one hard hrtimer that only touches the
 * timer's registers, in three parts the way the stock driver's "haptic
 * engine" does (stock drivers/motor/sec_haptic.c, max77705_haptic.c):
 *
 *  - overdrive: the first od_ms at od_boost % of the strength, to bring the
 *    mass up to speed (stock's overdrive_ratio is 95 against 70 normal);
 *  - sustain: the strength asked for, until the pulse's length is up;
 *  - brake: a negative intensity in stock's terms, the duty mirrored about
 *    one half so that the drive opposes the motion, for brake_ms.
 *
 * The chip stays enabled (MCONFIG bit 6) for idle_ms after a pulse so that the
 * next one is started without an I2C transaction. A pulse is a rumble effect
 * whose replay.length is its length in ms; the strength is the larger of its
 * two magnitudes. Playing it again restarts it; stopping it brakes at once.
 * The five knobs are module parameters, writable under /sys/module, and
 * /sys/bus/platform/devices/.../stats tells how long each pulse really was.
 */
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#ifdef ZH_DEV
#define ZH_DRIVER	"zrn-haptic-dev"	/* a second driver, to try a build beside the loaded one */
#else
#define ZH_DRIVER	"zrn-haptic"
#endif

#define PWM_BASE	0x10510000
#define TCFG0		0x00
#define TCFG1		0x04
#define TCON		0x08
#define TCNTB1		0x18
#define TCMPB1		0x1c
#define TCON_T1_START	BIT(8)
#define TCON_T1_MANUAL	BIT(9)
#define TCON_T1_INVERT	BIT(10)
#define TCON_T1_RELOAD	BIT(11)
#define PERIOD_COUNTS	1270	/* 26 MHz / 1270 = 20.47 kHz; / 128 = 160 Hz */
#define DUTY_PERCENT	95

#define PINS_BASE	0x10430000
#define GPP3		0x060
#define GPG1		0x0a0
#define PIN_CON		0x0
#define PIN_DAT		0x4
#define PIN_PUD		0x8

#define REG_MAINCTRL1	0x02
#define MAINCTRL1_BIASEN BIT(7)
#define REG_MCONFIG	0x10
#define MCONFIG_LRA_DIV128 0x82
#define MCONFIG_MEN	BIT(6)

#define MAG_MAX		0xffff
#define LOG_N		8

static unsigned int od_ms = 8;		/* overdrive at the start of a pulse */
static unsigned int od_boost = 150;	/* percent of the pulse's strength, at most full */
static unsigned int brake_ms = 10;	/* reverse drive after it; 0 for none */
static unsigned int brake_pct = 70;	/* percent of the pulse's strength (at most) */
static unsigned int idle_ms = 300;	/* the chip stays enabled this long after a pulse */
module_param(od_ms, uint, 0644);
module_param(od_boost, uint, 0644);
module_param(brake_ms, uint, 0644);
module_param(brake_pct, uint, 0644);
module_param(idle_ms, uint, 0644);
MODULE_PARM_DESC(od_ms, "overdrive time at a pulse's start, ms");
MODULE_PARM_DESC(od_boost, "overdrive strength, percent of the pulse's");
MODULE_PARM_DESC(brake_ms, "braking time after a pulse, ms (0: none)");
MODULE_PARM_DESC(brake_pct, "braking strength, percent of the pulse's");
MODULE_PARM_DESC(idle_ms, "time the motor chip stays enabled after a pulse, ms");

enum zh_phase { ZH_IDLE, ZH_OD, ZH_SUSTAIN, ZH_BRAKE };

struct zh_log {
	u32 req_ms, od_us, drive_us, brake_us;
	s64 latency_us;
	s64 total_us;		/* first drive to the end of the brake */
	u32 mag;
};

struct zrn_haptic {
	struct device *dev;
	struct regmap *regmap;
	void __iomem *pwm, *pins;
	struct input_dev *input;

	struct kthread_worker *worker;
	struct kthread_work start_work;
	struct kthread_delayed_work off_work;
	struct hrtimer timer;
	raw_spinlock_t lock;		/* the fields below; taken in hard irq */
	enum zh_phase phase;
	bool chip_on;			/* MCONFIG's MEN is set */
	bool start_queued;
	u32 mag, od_mag, brake_mag;	/* 0..MAG_MAX */
	u32 req_ms;
	u32 t_od, t_sus, t_brake;	/* us */
	ktime_t phase_end, requested, started, idle_since;
	struct zh_log log[LOG_N];
	unsigned int log_n;
	unsigned long pulses;
};

static const struct regmap_config zrn_haptic_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

/* level: -MAG_MAX..MAG_MAX. Zero is a half duty, where the chip is still; a
 * negative level is the same distance from a half on the other side, which
 * drives the motor the other way. */
static void zrn_haptic_pwm(struct zrn_haptic *h, int level)
{
	u32 tcon = readl(h->pwm + TCON) & ~(TCON_T1_START | TCON_T1_MANUAL | TCON_T1_RELOAD);
	u32 half = PERIOD_COUNTS / 2;
	u32 span = PERIOD_COUNTS * DUTY_PERCENT / 100 - half;
	u32 duty;

	/* as pwm-samsung does it: the count, and where the output changes; with
	 * the inverter on, the time below the compare value is the high time.
	 * The chip divides this PWM by 128 for the motor's 160 Hz and takes its
	 * strength from how far the duty is from one half. */
	if (level >= 0)
		duty = half + span * (u32)min(level, MAG_MAX) / MAG_MAX;
	else
		duty = half - span * (u32)min(-level, MAG_MAX) / MAG_MAX;
	writel(readl(h->pwm + TCFG0) & ~0xffU, h->pwm + TCFG0);	/* timers 0 and 1: no prescaler */
	writel(readl(h->pwm + TCFG1) & ~0xf0U, h->pwm + TCFG1);	/* timer 1: no divider */
	writel(PERIOD_COUNTS - 1, h->pwm + TCNTB1);
	writel(PERIOD_COUNTS - 1 - duty, h->pwm + TCMPB1);
	writel(tcon | TCON_T1_INVERT | TCON_T1_MANUAL, h->pwm + TCON);
	writel(tcon | TCON_T1_INVERT | TCON_T1_RELOAD | TCON_T1_START, h->pwm + TCON);
}

static void zrn_haptic_pwm_off(struct zrn_haptic *h)
{
	u32 tcon = readl(h->pwm + TCON) & ~(TCON_T1_START | TCON_T1_MANUAL | TCON_T1_RELOAD);

	writel(tcon | TCON_T1_INVERT, h->pwm + TCON);	/* stopped, the output low */
}

static void zh_arm(struct zrn_haptic *h, u32 us)
{
	h->phase_end = ktime_add_us(ktime_get(), us);
	hrtimer_start(&h->timer, ns_to_ktime(us * 1000ULL), HRTIMER_MODE_REL_HARD);
}

static void zh_finish_log(struct zrn_haptic *h, ktime_t now)
{
	struct zh_log *l = &h->log[(h->log_n - 1) % LOG_N];

	l->total_us = ktime_us_delta(now, h->started);
}

/* hrtimer, hard irq: the next part of the pulse, or the end of it */
static enum hrtimer_restart zh_timer(struct hrtimer *t)
{
	struct zrn_haptic *h = container_of(t, struct zrn_haptic, timer);
	ktime_t now = ktime_get();
	unsigned long flags;

	raw_spin_lock_irqsave(&h->lock, flags);
	if (h->phase == ZH_IDLE || ktime_before(now, h->phase_end))
		goto out;	/* a pulse started again meanwhile: its own timer is queued */
	switch (h->phase) {
	case ZH_OD:
		if (h->t_sus) {
			h->phase = ZH_SUSTAIN;
			zrn_haptic_pwm(h, h->mag);
			zh_arm(h, h->t_sus);
			break;
		}
		fallthrough;
	case ZH_SUSTAIN:
		if (h->t_brake) {
			h->phase = ZH_BRAKE;
			zrn_haptic_pwm(h, -(int)h->brake_mag);
			zh_arm(h, h->t_brake);
			break;
		}
		fallthrough;
	default:
		h->phase = ZH_IDLE;
		zrn_haptic_pwm(h, 0);
		zh_finish_log(h, now);
		h->idle_since = now;
		kthread_queue_delayed_work(h->worker, &h->off_work, msecs_to_jiffies(idle_ms));
	}
out:
	raw_spin_unlock_irqrestore(&h->lock, flags);
	return HRTIMER_NORESTART;
}

/* with h->lock held and the chip enabled: drive the planned pulse */
static void zh_begin(struct zrn_haptic *h)
{
	struct zh_log *l = &h->log[h->log_n % LOG_N];
	ktime_t now = ktime_get();

	h->pulses++;
	h->log_n++;
	h->started = now;
	l->req_ms = h->req_ms;
	l->od_us = h->t_od;
	l->drive_us = h->t_sus;
	l->brake_us = h->t_brake;
	l->mag = h->mag;
	l->latency_us = ktime_us_delta(now, h->requested);
	l->total_us = 0;
	if (h->t_od) {
		h->phase = ZH_OD;
		zrn_haptic_pwm(h, h->od_mag);
		zh_arm(h, h->t_od);
	} else {
		h->phase = ZH_SUSTAIN;
		zrn_haptic_pwm(h, h->mag);
		zh_arm(h, h->t_sus);
	}
}

/* what the pulse is made of: input_event_lock held, may not sleep */
static void zh_plan(struct zrn_haptic *h, u32 mag, u32 len_ms)
{
	u32 total = len_ms * 1000, od = min(od_ms * 1000, total);
	u32 boosted = min_t(u64, (u64)mag * max(od_boost, 100U) / 100, MAG_MAX);
	u32 brake = brake_ms * 1000;

	/* nothing brakes for longer than the pulse drove: a tick the mass has
	 * barely begun to follow needs only a moment of it */
	brake = min(brake, max(total / 2, 3000U));
	if (!brake_pct)
		brake = 0;
	h->req_ms = len_ms;
	h->mag = mag;
	h->od_mag = boosted;
	h->brake_mag = min_t(u64, (u64)mag * brake_pct / 100, MAG_MAX);
	h->t_od = od;
	h->t_sus = total - od;
	h->t_brake = brake;
}

/* the worker: enable the chip if it is not, then start what is planned */
static void zh_start_work(struct kthread_work *work)
{
	struct zrn_haptic *h = container_of(work, struct zrn_haptic, start_work);
	unsigned long flags;
	bool need;

	raw_spin_lock_irqsave(&h->lock, flags);
	need = !h->chip_on;
	raw_spin_unlock_irqrestore(&h->lock, flags);
	if (need)
		regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128 | MCONFIG_MEN);

	raw_spin_lock_irqsave(&h->lock, flags);
	h->chip_on = true;
	h->start_queued = false;
	if (h->mag)
		zh_begin(h);
	raw_spin_unlock_irqrestore(&h->lock, flags);
}

static void zh_off_work(struct kthread_work *work)
{
	struct zrn_haptic *h = container_of(work, struct zrn_haptic, off_work.work);
	unsigned long flags;
	bool off;

	raw_spin_lock_irqsave(&h->lock, flags);
	off = h->phase == ZH_IDLE && h->chip_on && !h->start_queued;
	if (off) {
		s64 left = (s64)idle_ms * 1000 - ktime_us_delta(ktime_get(), h->idle_since);

		if (left > 1000) {	/* a pulse ended since this was queued */
			kthread_queue_delayed_work(h->worker, &h->off_work, usecs_to_jiffies(left));
			off = false;
		} else {
			h->chip_on = false;
		}
	}
	raw_spin_unlock_irqrestore(&h->lock, flags);
	if (off) {
		regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
		zrn_haptic_pwm_off(h);
	}
}

static int zrn_haptic_upload(struct input_dev *input, struct ff_effect *effect,
			     struct ff_effect *old)
{
	return effect->type == FF_RUMBLE ? 0 : -EINVAL;
}

/* input event_lock held (irqs off): start or stop an effect */
static int zrn_haptic_playback(struct input_dev *input, int id, int value)
{
	struct zrn_haptic *h = input_get_drvdata(input);
	struct ff_effect *e = &input->ff->effects[id];
	unsigned long flags;
	u32 mag = max(e->u.rumble.strong_magnitude, e->u.rumble.weak_magnitude);
	u32 len = e->replay.length ? e->replay.length : 20;

	raw_spin_lock_irqsave(&h->lock, flags);
	if (!value) {
		/* stopped: brake now if it is running */
		if (h->phase == ZH_OD || h->phase == ZH_SUSTAIN) {
			if (h->t_brake && h->brake_mag) {
				h->phase = ZH_BRAKE;
				zrn_haptic_pwm(h, -(int)h->brake_mag);
				zh_arm(h, h->t_brake);
			} else {
				h->phase = ZH_IDLE;
				zrn_haptic_pwm(h, 0);
				h->idle_since = ktime_get();
				kthread_queue_delayed_work(h->worker, &h->off_work,
							   msecs_to_jiffies(idle_ms));
			}
		}
	} else if (mag) {
		zh_plan(h, mag, min(len, 5000U));
		h->requested = ktime_get();
		if (h->chip_on) {
			zh_begin(h);	/* already enabled: within this call */
		} else if (!h->start_queued) {
			h->start_queued = true;
			kthread_queue_work(h->worker, &h->start_work);
		}
	}
	raw_spin_unlock_irqrestore(&h->lock, flags);
	return 0;
}

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zrn_haptic *h = dev_get_drvdata(dev);
	unsigned long flags;
	int n = 0, i;

	raw_spin_lock_irqsave(&h->lock, flags);
	n += sysfs_emit_at(buf, n, "pulses %lu  od_ms %u od_boost %u brake_ms %u brake_pct %u idle_ms %u\n",
			   h->pulses, od_ms, od_boost, brake_ms, brake_pct, idle_ms);
	n += sysfs_emit_at(buf, n, "newest first: asked_ms mag od_us drive_us brake_us start_latency_us first_drive_to_end_us\n");
	for (i = 1; i <= LOG_N && i <= h->log_n; i++) {
		struct zh_log *l = &h->log[(h->log_n - i) % LOG_N];

		n += sysfs_emit_at(buf, n, "%u %u %u %u %u %lld %lld\n", l->req_ms, l->mag, l->od_us,
				   l->drive_us, l->brake_us, l->latency_us, l->total_us);
	}
	raw_spin_unlock_irqrestore(&h->lock, flags);
	return n;
}
static DEVICE_ATTR_RO(stats);

static void zrn_haptic_close(struct input_dev *input)
{
	/* the last one to close it leaves the motor still: flush has stopped the effects */
}

static int zrn_haptic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct i2c_client *i2c = to_i2c_client(dev->parent);
	struct zrn_haptic *h;
	u32 v;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	h->regmap = devm_regmap_init_i2c(i2c, &zrn_haptic_regmap);
	if (IS_ERR(h->regmap))
		return dev_err_probe(dev, PTR_ERR(h->regmap), "no regmap\n");
	h->pwm = devm_ioremap(dev, PWM_BASE, 0x100);
	h->pins = devm_ioremap(dev, PINS_BASE, 0x100);
	if (!h->pwm || !h->pins)
		return -ENOMEM;
	raw_spin_lock_init(&h->lock);
	hrtimer_setup(&h->timer, zh_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	h->worker = kthread_run_worker(0, "zrn-haptic");
	if (IS_ERR(h->worker))
		return PTR_ERR(h->worker);
	sched_set_fifo(h->worker->task);	/* a pulse starts the moment it is asked for */
	kthread_init_work(&h->start_work, zh_start_work);
	kthread_init_delayed_work(&h->off_work, zh_off_work);

	/* gpg1-7: an output, high (the motor's boost); gpp3-1: the timer's output */
	writel(readl(h->pins + GPG1 + PIN_DAT) | BIT(7), h->pins + GPG1 + PIN_DAT);
	v = readl(h->pins + GPG1 + PIN_CON);
	writel((v & ~(0xfU << 28)) | (1U << 28), h->pins + GPG1 + PIN_CON);
	zrn_haptic_pwm_off(h);
	v = readl(h->pins + GPP3 + PIN_CON);
	writel((v & ~(0xfU << 4)) | (2U << 4), h->pins + GPP3 + PIN_CON);
	v = readl(h->pins + GPP3 + PIN_PUD);
	writel((v & ~(0xfU << 4)) | (1U << 4), h->pins + GPP3 + PIN_PUD);	/* pulled down */

	ret = regmap_update_bits(h->regmap, REG_MAINCTRL1, MAINCTRL1_BIASEN, MAINCTRL1_BIASEN);
	if (!ret)
		ret = regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
	if (ret) {
		kthread_destroy_worker(h->worker);
		return dev_err_probe(dev, ret, "the power chip did not take the motor's settings\n");
	}

	h->input = devm_input_allocate_device(dev);
	if (!h->input) {
		kthread_destroy_worker(h->worker);
		return -ENOMEM;
	}
	h->input->name = "zrn-haptic";
	h->input->id.bustype = BUS_I2C;
	h->input->close = zrn_haptic_close;
	input_set_drvdata(h->input, h);
	input_set_capability(h->input, EV_FF, FF_RUMBLE);
	ret = input_ff_create(h->input, 16);
	if (ret)
		goto err;
	h->input->ff->upload = zrn_haptic_upload;
	h->input->ff->playback = zrn_haptic_playback;
	ret = input_register_device(h->input);
	if (ret)
		goto err;
	platform_set_drvdata(pdev, h);
	device_create_file(dev, &dev_attr_stats);
	dev_info(dev, "vibration motor ready (160 Hz, %d %% duty, overdrive %u ms, brake %u ms)\n",
		 DUTY_PERCENT, od_ms, brake_ms);
	return 0;
err:
	kthread_destroy_worker(h->worker);
	return dev_err_probe(dev, ret, "no rumble device\n");
}

static void zrn_haptic_remove(struct platform_device *pdev)
{
	struct zrn_haptic *h = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_stats);
	hrtimer_cancel(&h->timer);
	kthread_cancel_delayed_work_sync(&h->off_work);
	kthread_destroy_worker(h->worker);
	regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
	zrn_haptic_pwm_off(h);
}

static const struct platform_device_id zrn_haptic_ids[] = {
	{ "max77705-haptic" },
	{ }
};
MODULE_DEVICE_TABLE(platform, zrn_haptic_ids);

static struct platform_driver zrn_haptic_driver = {
	.driver = { .name = ZH_DRIVER },
	.probe = zrn_haptic_probe,
	.remove = zrn_haptic_remove,
	.id_table = zrn_haptic_ids,
};
module_platform_driver(zrn_haptic_driver);

MODULE_DESCRIPTION("Galaxy S9 vibration motor (MAX77705 LRA driver, SoC timer PWM)");
MODULE_LICENSE("GPL");
