// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-bbd: the Galaxy S9's sensor hub (Broadcom BCM4773: GNSS receiver and
 * hub for the light, proximity, motion, magnetic and pressure sensors) on a
 * mainline kernel.
 *
 * The rest of this module is Broadcom's SPI transport and "bbd" device nodes
 * as in Samsung's exynos9810 kernel (GPL), built against today's kernel
 * through zrn_compat.h. This file is what that code got from the board and
 * from other Samsung drivers, none of which exist here:
 *
 *  - the SPI bus. The controller the chip hangs off (spi@13ac0000) is not in
 *    the mainline device tree, so a bit-banged one is registered here on its
 *    four pins (gph0-0 clock, gph0-1 MOSI, gph0-2 MISO, gph0-3 chip select)
 *    and the chip declared on it. It writes the pins' data register itself:
 *    through gpiolib (spi-gpio) the firmware patch took longer to send than
 *    lhd is prepared to wait (about 3 s; the stock controller needs 0.8 s);
 *  - the handshake GPIOs: host request (gpa1-7, the chip's interrupt to us),
 *    MCU request (gph0-5) and response (gph0-4), and "AP sleep" (gpa0-5),
 *    which Samsung's sensor driver holds high while the phone is awake;
 *  - the two files Samsung's hub daemon (lhd) switches power with:
 *    /sys/class/sec/sensorhub/mcu_power (the chip's 1.8 V supply, LDO37 of
 *    the S2MPS18 PMIC, which the bootloader leaves off; switched through
 *    this kernel's ACPM driver) and
 *    /sys/class/sec/gps/GPS_PWR_EN/value (gpg0-2, the chip's enable pin);
 *  - somewhere for sensor packets to go. On the stock kernel Samsung's SSP
 *    driver takes them. Here they are queued on /dev/zrn_ssp for a user-space
 *    client, and what is written there is sent to the hub;
 *  - Samsung's ssp_suspend / ssp_resume: the AP_SLEEP pulse at each suspend
 *    (ap_sleep_pulse) and a resume counter (resume_seq) that tells the
 *    user-space client to send the hub MSG2SSP_AP_STATUS_RESUME (0xd3).
 *
 * The chip does nothing until lhd, run from the phone's own vendor partition,
 * has loaded its patch through /dev/bbd_patch and /dev/ttyBCM.
 */
#include <linux/delay.h>
#include <linux/gpio/machine.h>
#include <linux/irq.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spi/spi.h>
#include <linux/io.h>
#include "bbd.h"

struct gpio_desc *zrn_gpios[ZRN_GPIO_COUNT];

int zrn_named_gpio(const char *name)
{
	if (!strcmp(name, "ssp-host-req"))
		return ZRN_GPIO_HOST_REQ;
	if (!strcmp(name, "ssp-mcu-req"))
		return ZRN_GPIO_MCU_REQ;
	if (!strcmp(name, "ssp-mcu-resp"))
		return ZRN_GPIO_MCU_RESP;
	return -ENOENT;
}

static struct gpiod_lookup_table hub_gpios = {
	.dev_id = "zrn-bbd",
	.table = {
		GPIO_LOOKUP("gpa1", 7, "host-req", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gph0", 5, "mcu-req", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gph0", 4, "mcu-resp", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpg0", 2, "gps-pwr", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpa0", 5, "ap-sleep", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpa4", 0, "shub-int", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gph0", 0, "sck", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gph0", 1, "mosi", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gph0", 2, "miso", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gph0", 3, "cs", GPIO_ACTIVE_HIGH),
		{ }
	},
};

/*
 * The gph0 bank, written directly. gpiolib still owns the pins (it set their
 * directions), but every level on this bank is changed here, under one lock,
 * so that the SPI clocking and the MCU-request line cannot undo each other.
 */
#define GPH0_BASE	0x13a80000
#define GPH0_DAT	0x04
#define GPH0_PUD	0x08
#define PIN_SCK		BIT(0)
#define PIN_MOSI	BIT(1)
#define PIN_MISO	BIT(2)
#define PIN_CS		BIT(3)
#define PIN_MCU_REQ	BIT(5)

static void __iomem *gph0;
static DEFINE_RAW_SPINLOCK(gph0_lock);

static void gph0_set(u32 pin, bool high)
{
	unsigned long flags;
	u32 v;

	raw_spin_lock_irqsave(&gph0_lock, flags);
	v = readl(gph0 + GPH0_DAT);
	writel(high ? v | pin : v & ~pin, gph0 + GPH0_DAT);
	raw_spin_unlock_irqrestore(&gph0_lock, flags);
}

void zrn_mcu_req(int high)
{
	gph0_set(PIN_MCU_REQ, high);
}

/* One byte each way, SPI mode 3: the clock rests high, data moves on its
 * falling edge and is read on the rising one, most significant bit first. */
static u8 spi_byte(u8 out)
{
	unsigned long flags;
	u32 v;
	u8 in = 0;
	int bit;

	raw_spin_lock_irqsave(&gph0_lock, flags);
	v = readl(gph0 + GPH0_DAT) & ~(PIN_SCK | PIN_MOSI);
	for (bit = 7; bit >= 0; bit--) {
		u32 d = v | ((out >> bit) & 1 ? PIN_MOSI : 0);

		writel(d, gph0 + GPH0_DAT);		/* clock low, data out */
		writel(d | PIN_SCK, gph0 + GPH0_DAT);	/* clock high: both sides sample */
		in = (in << 1) | !!(readl(gph0 + GPH0_DAT) & PIN_MISO);
	}
	raw_spin_unlock_irqrestore(&gph0_lock, flags);
	return in;
}

/*
 * The real controller (USI00 of the CHUB block, spi@13ac0000 in the stock
 * device tree), driven here by polling its FIFOs. Bit-banging the pins works
 * but every access to this block's registers takes about 1.7 us, which makes
 * it 25 KB/s: the firmware patch then takes 7 s and lhd gives up after 3.
 * Through the FIFO each access carries a whole byte.
 */
#define SPI_BASE	0x13ac0000
#define SPI_CH_CFG	0x00
#define SPI_CLK_CFG	0x04
#define SPI_MODE_CFG	0x08
#define SPI_CS_REG	0x0c
#define SPI_STATUS	0x14
#define SPI_TX_DATA	0x18
#define SPI_RX_DATA	0x1c
#define SPI_FB_CLK	0x2c
#define USI_CON		0xc4
#define USI_OPTION	0xc8
#define USI_SW_CONF	0x13a13000	/* in the CHUB system registers: 2 selects SPI */
#define GPH0_CON	0x00

static void __iomem *spi_regs, *usi_sw_conf;
static bool bitbang;
module_param(bitbang, bool, 0444);
MODULE_PARM_DESC(bitbang, "toggle the pins by hand instead of using the SPI controller (slow)");
static int prescaler = 3;
module_param(prescaler, int, 0444);
MODULE_PARM_DESC(prescaler, "SPI clock = source / (2 * (prescaler + 1))");

/* Configures the controller; safe to call again (the reset path does, since
 * the pins are taken away from it while the chip is held in standby). */
static int hw_spi_setup(void)
{
	u32 v;

	void __iomem *cmu_chub = ioremap(0x13a00000, 0x4000);

	if (!spi_regs)
		spi_regs = ioremap(SPI_BASE, 0x100);
	if (!usi_sw_conf)
		usi_sw_conf = ioremap(USI_SW_CONF, 4);
	if (!spi_regs || !usi_sw_conf || !cmu_chub) {
		if (cmu_chub)
			iounmap(cmu_chub);
		return -ENOMEM;
	}
	/* its clock: from the block's bus clock, undivided, gates held open
	 * (left to themselves they stay shut) */
	writel(0x00300000, cmu_chub + 0x2010);
	writel(0x00300000, cmu_chub + 0x2090);
	writel(0x00300000, cmu_chub + 0x20a8);
	iounmap(cmu_chub);
	writel(2, usi_sw_conf);
	v = readl(spi_regs + USI_CON);
	writel(v & ~1U, spi_regs + USI_CON);		/* out of reset */
	udelay(10);
	v = readl(spi_regs + USI_OPTION);
	writel((v & ~4U) | 2, spi_regs + USI_OPTION);	/* keep its clock running */
	writel(BIT(5), spi_regs + SPI_CH_CFG);		/* software reset */
	udelay(10);
	writel(0, spi_regs + SPI_CH_CFG);
	writel(BIT(3) | BIT(2), spi_regs + SPI_CH_CFG);	/* mode 3 */
	writel(BIT(8) | (prescaler & 0xff), spi_regs + SPI_CLK_CFG);
	writel(0, spi_regs + SPI_MODE_CFG);		/* bytes, no DMA */
	writel(0, spi_regs + SPI_FB_CLK);
	/* its own select held active, or it shifts nothing; the chip's select
	 * is a GPIO, as on the stock kernel */
	writel(0, spi_regs + SPI_CS_REG);
	/* clock, MOSI and MISO pins to the controller (function 2) */
	writel((readl(gph0 + GPH0_CON) & ~0xfffU) | 0x222, gph0 + GPH0_CON);
	pr_info("zrn-bbd: SPI controller: ch %08x clk %08x usi %08x/%08x\n", readl(spi_regs + SPI_CH_CFG),
		readl(spi_regs + SPI_CLK_CFG), readl(spi_regs + USI_CON), readl(spi_regs + USI_OPTION));
	return 0;
}

static int hw_spi_xfer(const u8 *tx, u8 *rx, unsigned int len)
{
	const u32 cfg = BIT(3) | BIT(2);

	writel(cfg | 3, spi_regs + SPI_CH_CFG);		/* both directions on */
	while (len) {
		unsigned int n = min(len, 32U), i, spins = 0;

		for (i = 0; i < n; i++)
			writel(tx ? tx[i] : 0, spi_regs + SPI_TX_DATA);
		while (((readl(spi_regs + SPI_STATUS) >> 15) & 0x1ff) < n) {
			if (++spins > 20000) {
				writel(cfg | BIT(5), spi_regs + SPI_CH_CFG);	/* empty the FIFOs */
				udelay(10);
				writel(cfg, spi_regs + SPI_CH_CFG);
				pr_err_ratelimited("zrn-bbd: SPI stuck (status %08x)\n", readl(spi_regs + SPI_STATUS));
				return -ETIMEDOUT;
			}
		}
		for (i = 0; i < n; i++) {
			u8 in = readl(spi_regs + SPI_RX_DATA);

			if (rx)
				rx[i] = in;
		}
		if (tx)
			tx += n;
		if (rx)
			rx += n;
		len -= n;
	}
	writel(cfg, spi_regs + SPI_CH_CFG);
	return 0;
}

static int hub_spi_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
		struct spi_transfer *t)
{
	const u8 *tx = t->tx_buf;
	u8 *rx = t->rx_buf;
	unsigned int i;

	if (!bitbang)
		return hw_spi_xfer(tx, rx, t->len);
	for (i = 0; i < t->len; i++) {
		u8 in = spi_byte(tx ? tx[i] : 0);

		if (rx)
			rx[i] = in;
	}
	return 0;
}

/* called with true to release the chip (the line is active low) */
static void hub_spi_set_cs(struct spi_device *spi, bool release)
{
	if (bitbang)
		gph0_set(PIN_SCK, true);
	gph0_set(PIN_CS, release);
}

static struct platform_device *hub_pdev;
static struct spi_controller *hub_ctlr;
static struct spi_device *hub_spi;
static struct class *sec_class;
static struct device *sec_hub, *sec_gps;
static struct kobject *pwr_kobj;

/* ---- the power files lhd expects ------------------------------------------ */

/* from this kernel's ACPM driver (s9p-acpm): the PMIC's registers */
int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);
#define S2MPS18_L35CTRL		0x6c
#define S2MPS18_L37CTRL		(S2MPS18_L35CTRL + 2)
#define LDO_ON_MODE		0xc0

static ssize_t mcu_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 v = 0;

	s9p_acpm_pmic_read(S2MPS18_L37CTRL, &v);
	return sysfs_emit(buf, "%d\n", (v & LDO_ON_MODE) == LDO_ON_MODE);
}

static ssize_t mcu_power_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	long long on;
	u8 v = 0;
	int ret;

	if (kstrtoll(buf, 8, &on))
		return -EINVAL;
	/*
	 * On the stock kernel this regulator is "always on", so lhd's "off, on"
	 * at start-up never actually cuts the supply there. Doing it for real
	 * (20 ms off, then straight into the enable pin) leaves the chip dead,
	 * so a request to switch it off is acknowledged and not carried out.
	 */
	if (!on) {
		pr_info("zrn-bbd: hub supply: asked to switch off, left on\n");
		return size;
	}
	ret = s9p_acpm_pmic_update(S2MPS18_L37CTRL, on ? LDO_ON_MODE : 0, LDO_ON_MODE);
	msleep(20);
	s9p_acpm_pmic_read(S2MPS18_L37CTRL, &v);
	pr_info("zrn-bbd: hub supply %s (LDO37 control %02x, %d)\n", on ? "on" : "off", v, ret);
	return size;
}
static DEVICE_ATTR_RW(mcu_power);

/*
 * Resetting the chip after a warm reboot (see sensors/HUB-COLDBOOT-REA.md).
 *
 * Samsung's kernel never cuts LDO37 (always-on in its device tree) and never
 * resets the chip itself: lhd's BrcmGpsHalGpio::Normal() only drops the enable
 * pin (gpg0-2, "nSTANDBY") for 20 ms if it reads high and raises it again.
 * What a TWRP pass or a power-off add is time with the enable pin low and
 * every other line to the chip low too. A warm reboot of ztOS leaves the chip
 * running (MCU_RESP high with MCU_REQ low, SHUB_INT low) and a 100 ms pulse
 * did not clear that, nor did a 3 s supply cut made while chip select, the
 * SPI clock, MCU_REQ and AP_SLEEP were still driven high (those can keep the
 * chip's I/O ring and core partly powered through its input clamp diodes).
 *
 * So: park every line we drive towards the chip low (or as a pulled-down
 * input), hold it in standby, then bring it up in Samsung's order: AP_SLEEP
 * high (ssp_dev.c probe), pins back, enable pin high, 100 ms. If MCU_RESP is
 * still high afterwards the hold is repeated, longer.
 */
static unsigned int standby_ms = 1000;
module_param(standby_ms, uint, 0644);
MODULE_PARM_DESC(standby_ms, "how long the reset path holds the chip in standby with its lines parked");
static bool supply_cut;
module_param(supply_cut, bool, 0644);
MODULE_PARM_DESC(supply_cut, "also switch LDO37 off during the hold (off by default: the stock kernel never does)");

static bool mcu_resp_high(void)
{
	return gpiod_get_value(zrn_gpios[ZRN_GPIO_MCU_RESP]) > 0;
}

static void hub_pins_park(void)
{
	zrn_mcu_req(0);
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 0);
	if (bitbang) {
		gph0_set(PIN_SCK, false);
		gph0_set(PIN_MOSI, false);
	} else {
		/* clock, MOSI, MISO from the controller to inputs; their pull-downs
		 * (gph0 PUD 0x..111) hold them low */
		writel(readl(gph0 + GPH0_CON) & ~0xfffU, gph0 + GPH0_CON);
	}
	gph0_set(PIN_CS, false);
}

static int hub_pins_restore(void)
{
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 1);
	gph0_set(PIN_CS, true);
	if (bitbang) {
		gph0_set(PIN_SCK, true);
		return 0;
	}
	return hw_spi_setup();
}

static int hub_cycle(unsigned int hold_ms)
{
	int ret;

	hub_pins_park();
	gpiod_set_value(zrn_gpios[ZRN_GPIO_GPS_PWR], 0);
	if (supply_cut)
		s9p_acpm_pmic_update(S2MPS18_L37CTRL, 0, LDO_ON_MODE);
	msleep(hold_ms);
	ret = s9p_acpm_pmic_update(S2MPS18_L37CTRL, LDO_ON_MODE, LDO_ON_MODE);
	if (ret)
		return ret;
	msleep(20);
	ret = hub_pins_restore();
	if (ret)
		return ret;
	/* Enable the hub after its supply stabilizes; gpg0-4 is an unrelated,
	 * unpulled input used by the disabled USB PHY port. */
	gpiod_set_value(zrn_gpios[ZRN_GPIO_GPS_PWR], 1);
	msleep(100);
	return 0;
}

static int hub_power_sequence(unsigned int hold_ms)
{
	unsigned int hold = hold_ms;
	bool stale = mcu_resp_high();
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = hub_cycle(hold);
		pr_info("zrn-bbd: hub standby %u ms%s: MCU_RESP %d (was %d), SHUB_INT %d\n", hold,
			supply_cut ? " with supply cut" : "", mcu_resp_high(), stale,
			gpiod_get_value(zrn_gpios[ZRN_GPIO_SHUB_INT]));
		if (ret || !mcu_resp_high())
			return ret;
		hold = max(hold * 3, standby_ms);
	}
	pr_warn("zrn-bbd: MCU_RESP still high with MCU_REQ low; the chip did not reset\n");
	return 0;
}

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	int reset;

	if (kstrtoint(buf, 0, &reset) || reset != 1)
		return -EINVAL;
	reset = hub_power_sequence(standby_ms);
	return reset ? reset : size;
}
static DEVICE_ATTR_WO(reset);

/*
 * For bring-up: the PMIC's LDO control registers. Reading lists LDO1..45
 * (top two bits: 0 off, 3 on, 1 and 2 follow the PMIC's power-enable pins);
 * writing "N MODE" sets LDO N's two mode bits and leaves its voltage alone.
 */
/* LDO1 is at 0x49, LDO2 has two registers (0x4a, 0x4b), LDO3 onwards follow */
static u8 ldo_reg(unsigned int n)
{
	return n == 1 ? 0x49 : n == 2 ? 0x4a : 0x49 + n;
}

static ssize_t ldo_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i, n = 0;

	for (i = 1; i <= 45; i++) {
		u8 v = 0;

		s9p_acpm_pmic_read(ldo_reg(i), &v);
		n += sysfs_emit_at(buf, n, "LDO%d %02x mode %d\n", i, v, v >> 6);
	}
	return n;
}

static ssize_t ldo_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	unsigned int n, mode;
	int ret;

	if (sscanf(buf, "%u %u", &n, &mode) != 2 || n < 1 || n > 45 || mode > 3)
		return -EINVAL;
	ret = s9p_acpm_pmic_update(ldo_reg(n), mode << 6, LDO_ON_MODE);
	pr_info("zrn-bbd: LDO%u mode %u (%d)\n", n, mode, ret);
	return ret ? ret : size;
}
static DEVICE_ATTR_RW(ldo);

/* Also for bring-up: any PMIC register. Write "RR" to choose one to read,
 * "RR VV MM" (hex) to change the bits in MM to those in VV. */
static u8 pmic_reg;

static ssize_t pmic_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 v = 0;
	int ret = s9p_acpm_pmic_read(pmic_reg, &v);

	return sysfs_emit(buf, "%02x: %02x (%d)\n", pmic_reg, v, ret);
}

static ssize_t pmic_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	unsigned int reg, val, mask;
	int n = sscanf(buf, "%x %x %x", &reg, &val, &mask);

	if (n < 1 || reg > 0xff)
		return -EINVAL;
	pmic_reg = reg;
	if (n == 3) {
		int ret = s9p_acpm_pmic_update(reg, val, mask);

		pr_info("zrn-bbd: PMIC %02x: %02x under %02x (%d)\n", reg, val, mask, ret);
	}
	return size;
}
static DEVICE_ATTR_RW(pmic);

static ssize_t value_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", gpiod_get_value(zrn_gpios[ZRN_GPIO_GPS_PWR]));
}

static ssize_t value_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size)
{
	int on;

	if (kstrtoint(buf, 0, &on))
		return -EINVAL;
	gpiod_set_value(zrn_gpios[ZRN_GPIO_GPS_PWR], !!on);
	pr_info("zrn-bbd: hub enable pin %d\n", !!on);
	return size;
}
static struct kobj_attribute value_attr = __ATTR(value, 0664, value_show, value_store);

/* ---- sensor packets, to and from user space ------------------------------- */

static DEFINE_KFIFO(ssp_fifo, unsigned char, 16384);
static DEFINE_SPINLOCK(ssp_lock);
static DECLARE_WAIT_QUEUE_HEAD(ssp_wait);
static bool hub_ready;

/* each packet from the hub is queued as a two-byte length and its bytes */
static int ssp_on_packet(void *data, const char *buf, size_t size)
{
	unsigned char len[2] = { size & 0xff, size >> 8 };
	unsigned long flags;

	spin_lock_irqsave(&ssp_lock, flags);
	if (size < 0x10000 && kfifo_avail(&ssp_fifo) >= size + 2) {
		kfifo_in(&ssp_fifo, len, 2);
		kfifo_in(&ssp_fifo, buf, size);
	}
	spin_unlock_irqrestore(&ssp_lock, flags);
	wake_up_interruptible(&ssp_wait);
	return 0;
}

static int ssp_on_control(void *data, const char *str)
{
	pr_info("zrn-bbd: control: %s\n", str);
	return 0;
}

static int ssp_on_mcu_ready(void *data, bool ready)
{
	pr_info("zrn-bbd: hub %s\n", ready ? "ready" : "not ready");
	hub_ready = ready;
	wake_up_interruptible(&ssp_wait);
	return 0;
}

static int ssp_on_mcu_reset(void *data, bool no_response)
{
	pr_info("zrn-bbd: hub reset%s\n", no_response ? " (it had stopped answering)" : "");
	return 0;
}

static bbd_callbacks ssp_callbacks = {
	.on_packet = ssp_on_packet,
	.on_control = ssp_on_control,
	.on_mcu_ready = ssp_on_mcu_ready,
	.on_mcu_reset = ssp_on_mcu_reset,
};

static ssize_t ssp_read(struct file *file, char __user *buf, size_t size, loff_t *pos)
{
	unsigned int copied;
	int ret;

	if (kfifo_is_empty(&ssp_fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(ssp_wait, !kfifo_is_empty(&ssp_fifo)))
			return -ERESTARTSYS;
	}
	ret = kfifo_to_user(&ssp_fifo, buf, size, &copied);
	return ret ? ret : copied;
}

static ssize_t ssp_write(struct file *file, const char __user *buf, size_t size, loff_t *pos)
{
	unsigned char pkt[1024];

	if (size > sizeof(pkt))
		return -EMSGSIZE;
	if (copy_from_user(pkt, buf, size))
		return -EFAULT;
	return bbd_send_packet(pkt, size);
}

static __poll_t ssp_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &ssp_wait, wait);
	return kfifo_is_empty(&ssp_fifo) ? 0 : EPOLLIN | EPOLLRDNORM;
}

static const struct file_operations ssp_fops = {
	.owner = THIS_MODULE,
	.read = ssp_read,
	.write = ssp_write,
	.poll = ssp_poll,
};

static struct miscdevice ssp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "zrn_ssp",
	.fops = &ssp_fops,
	.mode = 0660,
};

static ssize_t ready_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", hub_ready);
}
static DEVICE_ATTR_RO(ready);

/* ---- what Samsung's ssp_suspend / ssp_resume do ----------------------------- */

/*
 * Stock (ssp_dev.c, and ssp_suspend at 0xa83560 in stock's Image): at each
 * suspend AP_SLEEP (gpa0-5) goes low and straight back high (set 0, a read
 * and a printk, set 1), and no command is sent; at resume AP_SLEEP is set
 * high and MSG2SSP_AP_STATUS_RESUME (0xd3) is sent with ssp_send_cmd
 * (ssp_resume, 0xa839a4). Here the command comes from ztoscsf-sensord, the
 * only writer on the link, when resume_seq moves.
 *
 * Off by default (ap_sleep_pulse): it pulses only while host_req is a wake
 * IRQ, since a hub told the AP sleeps asks to be woken (SSP_suspend_*
 * strings in its firmware) and with host_req masked nobody would answer.
 */
static bool ap_sleep_pulse;
module_param(ap_sleep_pulse, bool, 0644);
MODULE_PARM_DESC(ap_sleep_pulse, "pulse AP_SLEEP low at each suspend and count the resumes for 0xd3 (as stock)");
static unsigned int ap_sleep_pulse_us = 100;
module_param(ap_sleep_pulse_us, uint, 0644);
MODULE_PARM_DESC(ap_sleep_pulse_us, "how long AP_SLEEP is held low");
static bool pulsed;
static unsigned long resume_seq, ap_suspends;

void zrn_ssp_suspend(void)
{
	struct irq_data *d = hub_spi && hub_spi->irq > 0 ? irq_get_irq_data(hub_spi->irq) : NULL;
	unsigned int us = ap_sleep_pulse_us;
	const char *why = NULL;
	int level;

	if (!ap_sleep_pulse)
		why = "off";
	else if (!hub_ready)
		why = "hub not ready";
	else if (!d || !irqd_is_wakeup_set(d))
		why = "host_req is not a wake IRQ";
	if (why) {
		pr_info("zrn-bbd: no AP_SLEEP pulse (%s)\n", why);
		return;
	}
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 0);
	usleep_range(us, us + 50);
	level = gpiod_get_value(zrn_gpios[ZRN_GPIO_AP_SLEEP]);
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 1);
	pr_info("zrn-bbd: AP_SLEEP pulse, read %d\n", level);
	pulsed = true;
	ap_suspends++;
}

void zrn_ssp_resume(void)
{
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 1);
	if (!pulsed)
		return;
	pulsed = false;
	resume_seq++;
	if (!IS_ERR_OR_NULL(sec_hub))
		sysfs_notify(&sec_hub->kobj, NULL, "resume_seq");
}

static ssize_t resume_seq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", resume_seq);
}
static DEVICE_ATTR_RO(resume_seq);

static ssize_t ap_suspends_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", ap_suspends);
}
static DEVICE_ATTR_RO(ap_suspends);

/* ---- putting it together ---------------------------------------------------- */

static void board_exit(void)
{
	misc_deregister(&ssp_misc);
	if (hub_spi)
		spi_unregister_device(hub_spi);
	zrn_bcm_spi_exit();
	if (hub_ctlr)
		spi_unregister_controller(hub_ctlr);
	if (pwr_kobj) {
		sysfs_remove_file(pwr_kobj, &value_attr.attr);
		kobject_put(pwr_kobj);
	}
	if (!IS_ERR_OR_NULL(sec_gps))
		device_unregister(sec_gps);
	if (!IS_ERR_OR_NULL(sec_hub))
		device_unregister(sec_hub);
	if (!IS_ERR_OR_NULL(sec_class))
		class_destroy(sec_class);
	if (!IS_ERR_OR_NULL(hub_pdev)) {
		int i;

		for (i = 0; i < ZRN_GPIO_COUNT; i++)
			if (!IS_ERR_OR_NULL(zrn_gpios[i]))
				gpiod_put(zrn_gpios[i]);
		platform_device_unregister(hub_pdev);
	}
	gpiod_remove_lookup_table(&hub_gpios);
	if (spi_regs) {
		writel(BIT(5), spi_regs + SPI_CH_CFG);
		writel(0, spi_regs + SPI_CH_CFG);
		iounmap(spi_regs);
	}
	if (usi_sw_conf)
		iounmap(usi_sw_conf);
	if (gph0)
		iounmap(gph0);
}

static int __init board_init(void)
{
	static const char * const names[ZRN_GPIO_COUNT] = { "host-req", "mcu-req", "mcu-resp", "gps-pwr",
		"ap-sleep", "shub-int", "sck", "mosi", "miso", "cs" };
	/* "AP sleep" is held high (the AP is awake) from the start, as Samsung's
	 * sensor driver does at probe: with it low the chip does not come up */
	static const enum gpiod_flags flags[ZRN_GPIO_COUNT] = { GPIOD_IN, GPIOD_OUT_LOW, GPIOD_IN, GPIOD_OUT_LOW,
		GPIOD_OUT_HIGH, GPIOD_IN, GPIOD_OUT_HIGH, GPIOD_OUT_LOW, GPIOD_IN, GPIOD_OUT_HIGH };
	struct spi_board_info info = {
		.modalias = "ssp-spi",
		.max_speed_hz = 2000000,
		.mode = SPI_MODE_3,
	};
	int i, ret;

	gph0 = ioremap(GPH0_BASE, 0x100);
	if (!gph0)
		return -ENOMEM;
	gpiod_add_lookup_table(&hub_gpios);
	hub_pdev = platform_device_register_simple("zrn-bbd", -1, NULL, 0);
	if (IS_ERR(hub_pdev)) {
		ret = PTR_ERR(hub_pdev);
		goto err;
	}
	for (i = 0; i < ZRN_GPIO_COUNT; i++) {
		/* with the controller in use its three pins are not GPIOs */
		if (!bitbang && (i == ZRN_GPIO_SCK || i == ZRN_GPIO_MOSI || i == ZRN_GPIO_MISO))
			continue;
		zrn_gpios[i] = gpiod_get(&hub_pdev->dev, names[i], flags[i]);
		if (IS_ERR(zrn_gpios[i])) {
			ret = PTR_ERR(zrn_gpios[i]);
			pr_err("zrn-bbd: no %s GPIO: %d\n", names[i], ret);
			goto err;
		}
	}

	sec_class = class_create(THIS_MODULE, "sec");
	if (IS_ERR(sec_class)) {
		ret = PTR_ERR(sec_class);
		goto err;
	}
	sec_hub = device_create(sec_class, NULL, MKDEV(0, 0), NULL, "sensorhub");
	sec_gps = device_create(sec_class, NULL, MKDEV(0, 0), NULL, "gps");
	ret = -ENOMEM;
	if (IS_ERR(sec_hub) || IS_ERR(sec_gps))
		goto err;
	ret = device_create_file(sec_hub, &dev_attr_mcu_power);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_ready);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_resume_seq);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_ap_suspends);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_reset);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_ldo);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_pmic);
	if (ret)
		goto err;
	pwr_kobj = kobject_create_and_add("GPS_PWR_EN", &sec_gps->kobj);
	ret = pwr_kobj ? sysfs_create_file(pwr_kobj, &value_attr.attr) : -ENOMEM;
	if (ret)
		goto err;

	ret = zrn_bcm_spi_init();
	if (ret)
		goto err;

	/* No pulls on the handshake pair, as on the stock kernel: the chip's
	 * drive on MCU_RESP does not overcome the SoC's pull-down, and with it
	 * in place the chip never appears to answer. */
	writel(readl(gph0 + GPH0_PUD) & ~0x00ff0000, gph0 + GPH0_PUD);

	if (!bitbang) {
		ret = hw_spi_setup();
		if (ret)
			goto err;
	}
	hub_ctlr = spi_alloc_host(&hub_pdev->dev, 0);
	if (!hub_ctlr) {
		ret = -ENOMEM;
		goto err;
	}
	hub_ctlr->bus_num = 97;
	hub_ctlr->num_chipselect = 1;
	hub_ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	hub_ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	hub_ctlr->transfer_one = hub_spi_transfer_one;
	hub_ctlr->set_cs = hub_spi_set_cs;
	ret = spi_register_controller(hub_ctlr);
	if (ret) {
		spi_controller_put(hub_ctlr);
		hub_ctlr = NULL;
		goto err;
	}
	ret = -ENODEV;
	hub_spi = spi_new_device(hub_ctlr, &info);

	if (!hub_spi) {
		pr_err("zrn-bbd: could not declare the hub on the SPI bus\n");
		goto err;
	}
	/* after the probe: it clears the whole bbd state, callbacks included */
	bbd_register(NULL, &ssp_callbacks);
	/* Match Samsung's power-up order on every probe, including module reload;
	 * a chip left running by a warm reboot gets the long hold */
	ret = hub_power_sequence(mcu_resp_high() ? standby_ms : 100);
	if (ret)
		goto err;
	{
		u8 a = 0, b = 0;

		s9p_acpm_pmic_read(S2MPS18_L35CTRL, &a);
		s9p_acpm_pmic_read(S2MPS18_L37CTRL, &b);
		pr_info("zrn-bbd: PMIC LDO35 (touch, on) %02x, LDO37 (hub) %02x\n", a, b);
	}
	ret = misc_register(&ssp_misc);
	if (ret)
		goto err;
	pr_info("zrn-bbd: transport up; the hub wakes when lhd loads its patch\n");
	return 0;

err:
	board_exit();
	return ret;
}

module_init(board_init);
module_exit(board_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 sensor hub (BCM4773) transport: board glue for Broadcom's bbd driver");
