// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-etspi: the Egistec ET510 rear fingerprint sensor of the Galaxy S9
 * (starlte) for ztOS, secure (TrustZone) mode only.
 *
 * Port of Samsung's drivers/fingerprint/et5xx-spi.c (GPL, LineageOS
 * android_kernel_samsung_universal9810, lineage-17.1) built with
 * ENABLE_SENSORS_FPRINT_SECURE. In that mode the normal world never talks
 * SPI to the sensor: the Kinibi fingerprint trustlet does, through the
 * secure SPI driver. Linux only
 *   - powers the sensor (LDO gpm43-0, then SLEEP/RESETn = PCAL6524 P0.6),
 *   - muxes the SPI1 pins (gpp0-0..3) to the SPI function for the TA,
 *   - turns the SPI1 clocks on at the speed the HAL asks (FP_SET_SPI_CLOCK),
 *   - forwards the finger-detect interrupt (DRDY, gpa1-3) by poll(),
 *   - tells EL3 about chip select and suspend (SiP SMCs 0x8300002x),
 * behind the /dev/esfp0 ioctls libbauthserver.so uses (EGIS_IOC_MESSAGE).
 *
 * No device-tree node is needed, like zrn-bt: the pins come from gpiod
 * lookup tables and direct bank writes, and the expander is reached on the
 * bit-banged bus zrn-bt registers (i2c-gpio 96, address 0x22). zrn-bt holds
 * that address as a dummy client, so this module talks to the adapter
 * itself, read-modify-write under the adapter lock.
 *
 * Stock board facts (G960N boot.img DTB, rev16-26): spi@10450000 (SPI1,
 * "secure-mode"), etspi-spi@0, 13 MHz max, chipid ET510, orient 0,
 * etspi-sleepPin = <&expander 6>, etspi-drdyPin = <&gpa1 3>,
 * etspi-ldoPin = <&gpm43 0>, cs-gpio = <&gpp0 3>.
 */
#include <linux/arm-smccc.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/* ---- ABI with libbauthserver.so (et5xx.h, unchanged) ---- */
#define ET5XX_MAJOR		152
#define VENDOR			"EGISTEC"
#define CHIPID			"ET510"

#define FP_SENSOR_RESET		0x04
#define FP_POWER_CONTROL	0x05
#define FP_SET_SPI_CLOCK	0x06
#define FP_RESET_SET		0x07
#define FP_DISABLE_SPI_CLOCK	0x10
#define FP_CPU_SPEEDUP		0x11
#define FP_SET_SENSOR_TYPE	0x14
#define FP_SET_LOCKSCREEN	0x16
#define FP_SET_WAKE_UP_SIGNAL	0x17
#define FP_POWER_CONTROL_ET5XX	0x18
#define FP_SENSOR_ORIENT	0x19
#define FP_SPI_VALUE		0x1a
#define FP_IOCTL_RESERVED_01	0x1b
#define FP_IOCTL_RESERVED_02	0x1c
#define INT_TRIGGER_INIT	0xa4
#define INT_TRIGGER_CLOSE	0xa5
#define INT_TRIGGER_ABORT	0xa8

struct egis_ioc_transfer {
	u8 *tx_buf;
	u8 *rx_buf;
	__u32 len;
	__u32 speed_hz;
	__u16 delay_usecs;
	__u8 bits_per_word;
	__u8 cs_change;
	__u8 opcode;
	__u8 pad[3];
};
#define EGIS_IOC_MAGIC		'k'

/* fingerprint.h: sensor type values (type_check, FP_SET_SENSOR_TYPE) */
enum { SENSOR_OOO = -2, SENSOR_UNKNOWN, SENSOR_FAILED, SENSOR_VIPER,
	SENSOR_RAPTOR, SENSOR_EGIS, SENSOR_VIPER_WOG, SENSOR_NAMSAN,
	SENSOR_MAXIMUM };
#define DETECT_ADM		1

/* EL3 SiP calls the stock driver makes (fingerprint.h) */
#define MC_FC_FP_PM_SUSPEND		0x83000021
#define MC_FC_FP_CS_SET			0x83000027
#define MC_FC_FP_PM_SUSPEND_CS_HIGH	0x83000028

/* ---- board ---- */
#define EXP_BUS		96
#define EXP_ADDR	0x22
#define EXP_OUT0	0x04
#define EXP_CFG0	0x0c
#define FP_SLEEP	BIT(6)		/* PCAL6524 P0.6: ET510 SLEEP/RESETn */

#define PINCTRL_PERIC0	0x10430000	/* gpp0 is bank 0: CON DAT PUD DRV */
#define GPP0_CON	0x00
#define GPP0_PUD	0x08
#define GPP0_DRV	0x0c

/* SPI1 clocks: CMU_PERIC0 gates. No mainline clock provider exposes them
 * (/sys/kernel/debug/clk has 12 clocks), and the register offsets are not
 * pinned yet, so clk_mode 0 leaves the clocks as the bootloader left them.
 * See fingerprint/README.md, unknown U3. */
static unsigned int clk_mode;
module_param(clk_mode, uint, 0444);
MODULE_PARM_DESC(clk_mode, "0: do not touch SPI1 clocks (default)");

static bool smc_cs = true;
module_param(smc_cs, bool, 0644);
MODULE_PARM_DESC(smc_cs, "send the EL3 CS_SET / PM_SUSPEND SiP calls as stock does");

static struct gpiod_lookup_table fp_gpios = {
	.dev_id = "zrn-etspi",
	.table = {
		GPIO_LOOKUP("gpa1", 3, "drdy", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpm43", 0, "ldo", GPIO_ACTIVE_HIGH),
		{ }
	},
};

struct etspi {
	struct platform_device *pdev;
	struct gpio_desc *drdy, *ldo;
	struct i2c_adapter *bus;
	void __iomem *peric0;
	struct mutex lock;		/* ioctl serialisation */
	wait_queue_head_t waitq;
	struct wakeup_source *ws;
	int irq;
	bool irq_on, finger_on, powered, clk_on;
	int sensortype;
	u32 spi_value, speed_hz;
	unsigned int users, int_count, interrupt_count, reset_count;
	struct class *cls, *fp_cls;
	struct device *node, *fp_dev;
};
static struct etspi *g;

static unsigned long sip(u32 fn)
{
	struct arm_smccc_res res;

	if (!smc_cs)
		return 0;
	arm_smccc_smc(fn, 0, 0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

/* PCAL6524 read-modify-write on the adapter zrn-bt registered. The adapter
 * lock covers both transfers; zrn-bt's own RMW does not take it across, so a
 * BT_EN change at the same instant could still be lost (see README, R6). */
static int exp_update(u8 reg, u8 mask, u8 val)
{
	struct i2c_client c = { .addr = EXP_ADDR, .adapter = g->bus };
	union i2c_smbus_data d;
	int ret;

	i2c_lock_bus(g->bus, I2C_LOCK_SEGMENT);
	ret = __i2c_smbus_xfer(g->bus, c.addr, 0, I2C_SMBUS_READ, reg,
			       I2C_SMBUS_BYTE_DATA, &d);
	if (!ret) {
		d.byte = (d.byte & ~mask) | (val & mask);
		ret = __i2c_smbus_xfer(g->bus, c.addr, 0, I2C_SMBUS_WRITE, reg,
				       I2C_SMBUS_BYTE_DATA, &d);
	}
	i2c_unlock_bus(g->bus, I2C_LOCK_SEGMENT);
	return ret;
}

static int sleep_pin(bool high)
{
	int ret = exp_update(EXP_CFG0, FP_SLEEP, 0);	/* an output */

	if (!ret)
		ret = exp_update(EXP_OUT0, FP_SLEEP, high ? FP_SLEEP : 0);
	return ret;
}

/* stock pins_poweron: gpp0-0..3 function 2 (SPI1), pull-down, drive 2;
 * pins_poweroff: CLK/MOSI/CS outputs (function 1, no pull), MISO input,
 * pull-down. 4-bit fields per pin in each register. */
static void spi_pins(bool on)
{
	u32 con = readl(g->peric0 + GPP0_CON) & ~0xffff;
	u32 pud = readl(g->peric0 + GPP0_PUD) & ~0xffff;
	u32 drv = readl(g->peric0 + GPP0_DRV) & ~0xffff;

	if (on) {
		con |= 0x2222;
		pud |= 0x1111;
	} else {
		con |= 0x1011;		/* pin 2 (MISO) input */
		pud |= 0x0100;
	}
	writel(pud, g->peric0 + GPP0_PUD);
	writel(drv | 0x2222, g->peric0 + GPP0_DRV);
	writel(con, g->peric0 + GPP0_CON);
}

static void power(bool on)
{
	if (on) {
		spi_pins(true);
		gpiod_set_value_cansleep(g->ldo, 1);
		usleep_range(1100, 1150);
		sleep_pin(true);
		usleep_range(10000, 10050);
	} else {
		pr_info("zrn-etspi: cs_set smc ret = %lu\n", sip(MC_FC_FP_CS_SET));
		sleep_pin(false);
		gpiod_set_value_cansleep(g->ldo, 0);
		spi_pins(false);
	}
	g->powered = on;
	pr_info("zrn-etspi: power %d\n", on);
}

static void reset(void)
{
	sleep_pin(false);
	usleep_range(1050, 1100);
	sleep_pin(true);
	g->reset_count++;
}

static irqreturn_t drdy_irq(int irq, void *data)
{
	g->int_count++;
	g->interrupt_count++;
	g->finger_on = true;
	disable_irq_nosync(irq);
	wake_up_interruptible(&g->waitq);
	pm_wakeup_ws_event(g->ws, 1000, false);
	return IRQ_HANDLED;
}

static int irq_init(unsigned long flags)
{
	int ret;

	g->finger_on = false;
	g->int_count = 0;
	if (g->irq_on)
		return 0;
	ret = request_irq(g->irq, drdy_irq, flags, "etspi_irq", g);
	if (ret)
		return -EBUSY;
	enable_irq_wake(g->irq);
	g->irq_on = true;
	return 0;
}

static void irq_free(void)
{
	if (g->irq_on) {
		if (!g->int_count)
			disable_irq_nosync(g->irq);
		disable_irq_wake(g->irq);
		free_irq(g->irq, g);
		g->irq_on = false;
	}
	g->finger_on = false;
	g->int_count = 0;
}

static long etspi_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct egis_ioc_transfer *ioc;
	u32 size = _IOC_SIZE(cmd);
	long ret = 0;

	if (_IOC_TYPE(cmd) != EGIS_IOC_MAGIC || _IOC_NR(cmd) != 0 ||
	    _IOC_DIR(cmd) != _IOC_WRITE)
		return -ENOTTY;
	if (!size || size % sizeof(*ioc))
		return -EINVAL;
	ioc = memdup_user((void __user *)arg, size);
	if (IS_ERR(ioc))
		return PTR_ERR(ioc);

	mutex_lock(&g->lock);
	switch (ioc->opcode) {
	case FP_SENSOR_RESET:
		reset();
		break;
	case FP_RESET_SET:
		break;
	case FP_POWER_CONTROL:
	case FP_POWER_CONTROL_ET5XX:
		if (ioc->len == 1 || ioc->len == 0)
			power(ioc->len);
		break;
	case FP_SET_SPI_CLOCK:
		/* stock: clk_set_rate(sclk, speed * 4), pclk+sclk enabled */
		g->speed_hz = ioc->speed_hz;
		g->clk_on = true;
		pr_info("zrn-etspi: SPI clock %u Hz requested (clk_mode %u)\n",
			ioc->speed_hz, clk_mode);
		break;
	case FP_DISABLE_SPI_CLOCK:
		g->clk_on = false;
		break;
	case INT_TRIGGER_INIT:
		ret = irq_init(ioc->pad[0]);
		break;
	case INT_TRIGGER_CLOSE:
		irq_free();
		break;
	case INT_TRIGGER_ABORT:
		wake_up_interruptible(&g->waitq);
		break;
	case FP_CPU_SPEEDUP:		/* no secure-OS booster on ztOS */
	case FP_SET_LOCKSCREEN:
	case FP_SET_WAKE_UP_SIGNAL:
	case FP_IOCTL_RESERVED_01:
	case FP_IOCTL_RESERVED_02:
		break;
	case FP_SET_SENSOR_TYPE:
		if ((int)ioc->len >= SENSOR_OOO && (int)ioc->len < SENSOR_MAXIMUM) {
			if (!((int)ioc->len == SENSOR_OOO && g->sensortype == SENSOR_FAILED))
				g->sensortype = (int)ioc->len;
		} else {
			g->sensortype = SENSOR_UNKNOWN;
		}
		break;
	case FP_SENSOR_ORIENT:
		ret = put_user((u8)0, (u8 __user *)ioc->rx_buf);
		break;
	case FP_SPI_VALUE:
		g->spi_value = ioc->len;
		break;
	default:
		/* register/frame opcodes are the non-secure build's: refused */
		ret = -EFAULT;
	}
	mutex_unlock(&g->lock);
	kfree(ioc);
	return ret;
}

static __poll_t etspi_poll(struct file *f, struct poll_table_struct *w)
{
	__poll_t mask = 0;

	if (!g->finger_on)
		poll_wait(f, &g->waitq, w);
	if (g->finger_on) {
		mask |= EPOLLIN | EPOLLRDNORM;
		g->finger_on = false;
	}
	return mask;
}

static int etspi_open(struct inode *i, struct file *f)
{
	mutex_lock(&g->lock);
	g->users++;
	mutex_unlock(&g->lock);
	return nonseekable_open(i, f);
}

static int etspi_release(struct inode *i, struct file *f)
{
	mutex_lock(&g->lock);
	g->users--;
	mutex_unlock(&g->lock);
	return 0;
}

static const struct file_operations etspi_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = etspi_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.poll = etspi_poll,
	.open = etspi_open,
	.release = etspi_release,
};

/* /sys/class/fingerprint/fingerprint/: what the stock driver exposes */
static ssize_t bfs_values_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "\"FP_SPICLK\":\"%u\"\n", g->speed_hz); }
static ssize_t type_check_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%d\n", g->sensortype); }
static ssize_t vendor_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%s\n", VENDOR); }
static ssize_t name_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%s\n", CHIPID); }
static ssize_t adm_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%d\n", DETECT_ADM); }
static ssize_t intcnt_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", g->interrupt_count); }
static ssize_t intcnt_store(struct device *d, struct device_attribute *a,
			    const char *b, size_t n)
{ if (sysfs_streq(b, "c")) g->interrupt_count = 0; return n; }
static ssize_t resetcnt_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", g->reset_count); }
static ssize_t resetcnt_store(struct device *d, struct device_attribute *a,
			      const char *b, size_t n)
{ if (sysfs_streq(b, "c")) g->reset_count = 0; return n; }

static DEVICE_ATTR_RO(bfs_values);
static DEVICE_ATTR_RO(type_check);
static DEVICE_ATTR_RO(vendor);
static DEVICE_ATTR_RO(name);
static DEVICE_ATTR_RO(adm);
static DEVICE_ATTR(intcnt, 0664, intcnt_show, intcnt_store);
static DEVICE_ATTR(resetcnt, 0664, resetcnt_show, resetcnt_store);
static struct attribute *fp_attrs[] = {
	&dev_attr_bfs_values.attr, &dev_attr_type_check.attr,
	&dev_attr_vendor.attr, &dev_attr_name.attr, &dev_attr_adm.attr,
	&dev_attr_intcnt.attr, &dev_attr_resetcnt.attr, NULL,
};
ATTRIBUTE_GROUPS(fp);

static int etspi_suspend(struct device *dev)
{
	if (g->powered)
		pr_info("zrn-etspi: suspend smc ret = %lu\n",
			sip(g->irq_on ? MC_FC_FP_PM_SUSPEND_CS_HIGH : MC_FC_FP_PM_SUSPEND));
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(etspi_pm, etspi_suspend, NULL);

static int etspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	g->pdev = pdev;
	g->sensortype = SENSOR_UNKNOWN;
	mutex_init(&g->lock);
	init_waitqueue_head(&g->waitq);

	g->bus = i2c_get_adapter(EXP_BUS);
	if (!g->bus) {
		dev_err(dev, "i2c-%d (zrn-bt's expander bus) missing\n", EXP_BUS);
		return -EPROBE_DEFER;
	}
	g->peric0 = devm_ioremap(dev, PINCTRL_PERIC0, 0x1000);
	if (!g->peric0) {
		ret = -ENOMEM;
		goto err_bus;
	}
	/* Powered off, as stock probe leaves it: LDO low, SLEEP low. */
	g->ldo = devm_gpiod_get(dev, "ldo", GPIOD_OUT_LOW);
	if (IS_ERR(g->ldo)) {
		ret = PTR_ERR(g->ldo);
		goto err_bus;
	}
	g->drdy = devm_gpiod_get(dev, "drdy", GPIOD_IN);
	if (IS_ERR(g->drdy)) {
		ret = PTR_ERR(g->drdy);
		goto err_bus;
	}
	g->irq = gpiod_to_irq(g->drdy);
	if (g->irq < 0) {
		ret = g->irq;
		goto err_bus;
	}
	ret = sleep_pin(false);
	if (ret)
		dev_warn(dev, "expander P0.6 low failed: %d\n", ret);
	g->ws = wakeup_source_register(dev, "etspi_sigwake_lock");

	ret = register_chrdev(ET5XX_MAJOR, "egis_fingerprint", &etspi_fops);
	if (ret < 0)
		goto err_ws;
	g->cls = class_create("egis_fingerprint");
	if (IS_ERR(g->cls)) {
		ret = PTR_ERR(g->cls);
		goto err_chr;
	}
	g->node = device_create(g->cls, dev, MKDEV(ET5XX_MAJOR, 0), g, "esfp0");
	if (IS_ERR(g->node)) {
		ret = PTR_ERR(g->node);
		goto err_cls;
	}
	g->fp_cls = class_create("fingerprint");
	if (IS_ERR(g->fp_cls)) {
		ret = PTR_ERR(g->fp_cls);
		goto err_node;
	}
	g->fp_dev = device_create_with_groups(g->fp_cls, NULL, 0, g, fp_groups,
					      "fingerprint");
	if (IS_ERR(g->fp_dev)) {
		ret = PTR_ERR(g->fp_dev);
		goto err_fpcls;
	}
	dev_info(dev, "ET510 /dev/esfp0 ready (secure mode, irq %d)\n", g->irq);
	return 0;

err_fpcls:
	class_destroy(g->fp_cls);
err_node:
	device_destroy(g->cls, MKDEV(ET5XX_MAJOR, 0));
err_cls:
	class_destroy(g->cls);
err_chr:
	unregister_chrdev(ET5XX_MAJOR, "egis_fingerprint");
err_ws:
	wakeup_source_unregister(g->ws);
err_bus:
	i2c_put_adapter(g->bus);
	return ret;
}

static void etspi_remove(struct platform_device *pdev)
{
	irq_free();
	if (g->powered)
		power(false);
	device_destroy(g->fp_cls, 0);
	class_destroy(g->fp_cls);
	device_destroy(g->cls, MKDEV(ET5XX_MAJOR, 0));
	class_destroy(g->cls);
	unregister_chrdev(ET5XX_MAJOR, "egis_fingerprint");
	wakeup_source_unregister(g->ws);
	i2c_put_adapter(g->bus);
}

static struct platform_driver etspi_driver = {
	.driver = { .name = "zrn-etspi", .pm = pm_sleep_ptr(&etspi_pm) },
	.probe = etspi_probe,
	.remove = etspi_remove,
};

static struct platform_device *self;

static int __init zrn_etspi_init(void)
{
	int ret;

	gpiod_add_lookup_table(&fp_gpios);
	ret = platform_driver_register(&etspi_driver);
	if (ret)
		goto err_table;
	self = platform_device_register_simple("zrn-etspi", -1, NULL, 0);
	if (IS_ERR(self)) {
		ret = PTR_ERR(self);
		goto err_drv;
	}
	return 0;
err_drv:
	platform_driver_unregister(&etspi_driver);
err_table:
	gpiod_remove_lookup_table(&fp_gpios);
	return ret;
}

static void __exit zrn_etspi_exit(void)
{
	platform_device_unregister(self);
	platform_driver_unregister(&etspi_driver);
	gpiod_remove_lookup_table(&fp_gpios);
}

module_init(zrn_etspi_init);
module_exit(zrn_etspi_exit);
MODULE_DESCRIPTION("ztOS: Egistec ET510 fingerprint sensor, secure mode (/dev/esfp0)");
MODULE_LICENSE("GPL");
