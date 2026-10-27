// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-bt: the board side of the Galaxy S9's Bluetooth, for a kernel whose
 * device tree does not describe it (see ../README.md).
 *
 * The BCM4361's Bluetooth sits on UART_BT (0x10840000, a plain Samsung UART
 * in PERIC1, 4-wire on gpq0-0..3). This module:
 *
 *   1. opens its clock gates in CMU_PERIC1, takes the UART out of its USI
 *      reset (USI_CON +0xc4 = 0, USI_OPTION +0xc8 = CLKREQ_ON), divider 1:1
 *      (200 MHz);
 *   2. puts gpq0-0..3 in their UART function, pulled up as the stock tree has
 *      them;
 *   3. registers the UART with samsung_tty as an "exynos850-uart" platform
 *      device, with a 200 MHz fixed clock and its interrupt (SPI 422): it
 *      becomes /dev/ttySAC0;
 *   4. powers the chip's Bluetooth: BT_EN is pin 7 of the PCAL6524 expander
 *      (I2C 0x22, reached over a bit-banged bus on gpp6-2/gpp6-3, as
 *      zrn_btprobe did), and BT_WAKE (gpg2-0) is held high.
 *
 * The host stack then attaches to the UART from userspace:
 *
 *	btattach -B /dev/ttySAC0 -P bcm -S 115200
 *
 * which loads brcm/BCM4361B2.hcd through btbcm. Unloading powers it off.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>
#include <linux/syscore_ops.h>
#include <linux/suspend.h>

#define CMU_PERIC1	0x10800000
#define DIV_UART_BT	0x1814
#define UART_BT		0x10840000
#define PINCTRL_ALIVE	0x14050000
#define GPQ0		0x0a0		/* CON +0, DAT +4, PUD +8 */
#define UART_SPI	0x1a6
#define UART_HZ		200000000
#define EXP_BUS		96
#define EXP_ADDR	0x22
#define EXP_OUT0	0x04
#define EXP_CFG0	0x0c
#define BT_EN		BIT(7)

static const u32 gates[] = { 0x20c4, 0x20c8, 0x201c, 0x2034 };

static struct gpiod_lookup_table bus_gpios = {
	.dev_id = "i2c-gpio.96",
	.table = {
		GPIO_LOOKUP_IDX("gpp6", 3, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp6", 2, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};
static struct gpiod_lookup_table wake_gpios = {
	.dev_id = "zrn-bt",
	.table = {
		GPIO_LOOKUP("gpg2", 0, "bt-wake", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpa2", 3, "host-wake", GPIO_ACTIVE_HIGH),
		{ }
	},
};
static struct i2c_gpio_platform_data bus_pdata = { .udelay = 5 };

static struct platform_device *bus_pdev, *uart_pdev, *self;
static struct i2c_client *exp;
static struct clk_hw *uart_clk;
static struct clk_lookup *cl_uart, *cl_baud;
static struct gpio_desc *bt_wake, *host_wake;
static int host_wake_irq = -1;

/* what the gates and USI_OPTION were before we forced them, for sleep */
static u32 saved_gates[ARRAY_SIZE(gates)], saved_option;
static bool saved;

static void clocks_on(void)
{
	void __iomem *cmu = ioremap(CMU_PERIC1, 0x4000);

	if (!cmu)
		return;
	for (int i = 0; i < ARRAY_SIZE(gates); i++) {
		if (!saved)
			saved_gates[i] = readl(cmu + gates[i]);
		writel(readl(cmu + gates[i]) | 0x300000, cmu + gates[i]);	/* MANUAL | CG_VAL */
	}
	/* CLKCMU_PERIC1_IP arrives at 200 MHz (SHARED2 800 / 2 / 2): divider
	 * ratio 0, as the bootloader leaves it */
	writel(readl(cmu + DIV_UART_BT) & ~0xf, cmu + DIV_UART_BT);
	pr_info("zrn-bt: UART_BT clock open, divider %#x\n", readl(cmu + DIV_UART_BT));
	iounmap(cmu);
	/* the UART sits behind a USI v2 wrapper (stock: samsung,usi-serial-v2):
	 * held in reset until its USI_CON is cleared, as exynos-usi does */
	cmu = ioremap(UART_BT, 0x100);
	if (cmu) {
		if (!saved)
			saved_option = readl(cmu + 0xc8);
		saved = true;
		writel(0x2, cmu + 0xc8);	/* USI_OPTION: CLKREQ_ON */
		writel(0x0, cmu + 0xc4);	/* USI_CON: reset off */
		iounmap(cmu);
	}
}

static void pins_uart(void)
{
	void __iomem *p = ioremap(PINCTRL_ALIVE, 0x1000);
	u32 v;

	if (!p)
		return;
	v = readl(p + GPQ0);
	writel((v & ~0xffff) | 0x2222, p + GPQ0);		/* function 2 on 0..3 */
	v = readl(p + GPQ0 + 8);
	writel((v & ~0xffff) | 0x3333, p + GPQ0 + 8);		/* pull-up, as stock */
	pr_info("zrn-bt: gpq0 con %#x pud %#x\n", readl(p + GPQ0), readl(p + GPQ0 + 8));
	iounmap(p);
}

/*
 * In SYS_SLEEP the non-alive banks drive each pin by its power-down config
 * (CONPDN, PUDPDN) instead. Left at their reset values the expander's RESETn
 * (gpg3-6) is an input pulled down: the PCAL6524 resets in the sleep, BT_EN
 * goes with it and the chip comes back unpowered, its firmware gone. The
 * stock tree has expander_reset_active { pin-con-pdn = <1> } (drive high) and
 * bt-btwake { pin-con-pdn = <3> } (hold the level it had), both with no pull.
 * The pinctrl driver saves and restores these registers across the sleep.
 */
#define GPG2		0x104300c0	/* PERIC0 */
#define GPG3		0x108300c0	/* PERIC1 */
#define CONPDN		0x10		/* 2 bits a pin: 0 low, 1 high, 2 input, 3 previous */
#define PUDPDN		0x14		/* 4 bits a pin */

static void pin_pdn(phys_addr_t bank, unsigned int pin, u32 con)
{
	void __iomem *p = ioremap(bank, 0x20);

	if (!p)
		return;
	writel((readl(p + CONPDN) & ~(3U << pin * 2)) | con << pin * 2, p + CONPDN);
	writel(readl(p + PUDPDN) & ~(0xfU << pin * 4), p + PUDPDN);
	pr_info("zrn-bt: %pa pin %u in sleep: conpdn %#x pudpdn %#x\n", &bank, pin,
		readl(p + CONPDN), readl(p + PUDPDN));
	iounmap(p);
}

static void pins_sleep(void)
{
	pin_pdn(GPG3, 6, 1);	/* expander RESETn: held high */
	pin_pdn(GPG2, 0, 3);	/* BT_WAKE: keeps its level */
}

static int uart_register(void)
{
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, UART_SPI, IRQ_TYPE_LEVEL_HIGH } };
	struct resource res[2] = {
		DEFINE_RES_MEM(UART_BT, 0x100),
	};
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	if (irq <= 0)
		return -EINVAL;
	res[1] = (struct resource)DEFINE_RES_IRQ(irq);

	uart_clk = clk_hw_register_fixed_rate(NULL, "zrn-uart-bt", NULL, 0, UART_HZ);
	if (IS_ERR(uart_clk))
		return PTR_ERR(uart_clk);
	cl_uart = clkdev_hw_create(uart_clk, "uart", "exynos850-uart.0");
	cl_baud = clkdev_hw_create(uart_clk, "clk_uart_baud0", "exynos850-uart.0");
	uart_pdev = platform_device_register_simple("exynos850-uart", 0, res, 2);
	if (IS_ERR(uart_pdev))
		return PTR_ERR(uart_pdev);
	pr_info("zrn-bt: UART_BT registered (irq %d), as ttySAC0 if samsung_tty takes it\n", irq);
	return 0;
}

static int exp_update(u8 reg, u8 mask, u8 val)
{
	int v = i2c_smbus_read_byte_data(exp, reg);

	if (v < 0)
		return v;
	return i2c_smbus_write_byte_data(exp, reg, (v & ~mask) | (val & mask));
}

static int bt_power(bool on)
{
	int ret = exp_update(EXP_CFG0, BT_EN, 0);	/* an output */

	if (!ret)
		ret = exp_update(EXP_OUT0, BT_EN, on ? BT_EN : 0);
	pr_info("zrn-bt: BT_EN %s (%d)\n", on ? "high" : "low", ret);
	return ret;
}

/*
 * System sleep. The forced-open gates and CLKREQ_ON are a standing clock
 * request on PERIC1: the firmware gave up SoC sleep 21 ms in, every time,
 * while this module was loaded (SoC sleep worked with it unloaded). Give the
 * gates and the request back to the hardware for the sleep, as late as can
 * be: syscore runs after every device, the UART included, has suspended, so
 * nothing touches UART_BT with its clock gated (on this SoC that hangs the
 * bus). BT_WAKE low lets the chip sleep too. Resume opens them again.
 */
static bool sleep_hook = true;
module_param(sleep_hook, bool, 0644);
static bool hooked;

static int bt_syscore_suspend(void *data)
{
	void __iomem *cmu;

	hooked = saved && sleep_hook;
	if (!hooked)
		return 0;
	if (bt_wake)
		gpiod_set_value(bt_wake, 0);
	cmu = ioremap(UART_BT, 0x100);
	if (cmu) {
		writel(saved_option, cmu + 0xc8);
		iounmap(cmu);
	}
	cmu = ioremap(CMU_PERIC1, 0x4000);
	if (cmu) {
		for (int i = 0; i < ARRAY_SIZE(gates); i++) {
			u32 was = readl(cmu + gates[i]);

			writel(saved_gates[i], cmu + gates[i]);
			pr_info("zrn-bt: sleep: gate %#x %08x -> %08x (now %08x)\n", gates[i], was,
				saved_gates[i], readl(cmu + gates[i]));
		}
		pr_info("zrn-bt: sleep: q-channel %#x = %08x, option -> %08x\n", 0x3028,
			readl(cmu + 0x3028), saved_option);
		iounmap(cmu);
	}
	return 0;
}

/*
 * UART_BT comes out of SYS_SLEEP at its reset values: UINTM 0, every
 * interrupt unmasked. The chip moves CTS as it wakes, which latches MODEM in
 * UINTP; samsung_tty's resume masks it again but never clears it, and its
 * handler acks only RXD and TXD. The level line then stays up for good, cpu0
 * spins in the UART handler, and every SPI numbered above it starves, the
 * MCT broadcast timer among them (measured: UINTP 8, UINTM e, hard lockup on
 * the core waiting for the broadcast). Mask everything and clear what latched
 * before the driver unmasks RX and TX.
 */
static void uart_quiet(void)
{
	void __iomem *uart = ioremap(UART_BT, 0x100);

	if (!uart)
		return;
	writel(0xf, uart + 0x38);	/* UINTM: all masked */
	writel(0xf, uart + 0x30);	/* UINTP: clear on 1 */
	iounmap(uart);
}

static void bt_syscore_resume(void *data)
{
	if (!hooked)
		return;
	clocks_on();
	uart_quiet();
	if (bt_wake)
		gpiod_set_value(bt_wake, 1);
}

static const struct syscore_ops bt_syscore_ops = {
	.suspend = bt_syscore_suspend,
	.resume = bt_syscore_resume,
};
static struct syscore bt_syscore = { .ops = &bt_syscore_ops };

/* The chip's Bluetooth side, powered, also kept the SoC from sleeping:
 * off for the sleep (process context: BT_EN is on the I2C expander), on
 * again after. The host stack loses the link; btattach is restarted by its
 * service when the tty goes away. */
/* Off: the chip stays powered and its links up through the sleep; with the
 * resume fixes above SoC sleep works so, and HOST_WAKE can wake it. */
static bool sleep_power_off;
module_param(sleep_power_off, bool, 0644);
static bool powered_off_for_sleep;

static bool entering_sleep;	/* see host_wake_isr */

static int bt_pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	if (event == PM_SUSPEND_PREPARE)
		WRITE_ONCE(entering_sleep, true);
	else if (event == PM_POST_SUSPEND)
		WRITE_ONCE(entering_sleep, false);
	if (!sleep_power_off)
		return NOTIFY_DONE;
	if (event == PM_SUSPEND_PREPARE) {
		powered_off_for_sleep = !bt_power(false);
		pr_info("zrn-bt: sleep: Bluetooth off (%d)\n", powered_off_for_sleep);
	} else if (event == PM_POST_SUSPEND && powered_off_for_sleep) {
		powered_off_for_sleep = false;
		pr_info("zrn-bt: sleep: Bluetooth on (%d)\n", bt_power(true));
	}
	return NOTIFY_DONE;
}
static struct notifier_block bt_pm_nb = { .notifier_call = bt_pm_notify };

/*
 * HOST_WAKE (gpa2-3, alive EINT 19): the chip pulses it low when it has
 * something for the host, once Write_Sleep_Mode (vendor 0xfc27) has turned
 * its UART sleep on with host_wake_active 0 and pulsed host wake, as stock
 * (bcm43xx_bluetooth: the bt_host_wake IRQ, wakeup source BT_host_wake). A
 * wake IRQ: in system sleep it wakes the SoC (with EINT_WAKEUP_MASK bit 19
 * open in zrn-system-sleep's eint_wake), and keeps it up a second for the
 * stack to read what came.
 */
/* Between PM_SUSPEND_PREPARE and PM_POST_SUSPEND the kernel's own Bluetooth
 * suspend notifier talks to the chip (scan off, event filters), and every
 * answer pulses HOST_WAKE: reported as a wakeup event, that aborted every
 * sleep at its freeze. Not reported then; in the sleep itself the PMU wakes
 * the SoC on EINT 19 by itself, Linux or not. */
static irqreturn_t host_wake_isr(int irq, void *data)
{
	if (!READ_ONCE(entering_sleep))
		pm_wakeup_dev_event(&self->dev, 1000, true);
	return IRQ_HANDLED;
}

static void host_wake_setup(void)
{
	int ret;

	host_wake = gpiod_get(&self->dev, "host-wake", GPIOD_IN);
	if (IS_ERR(host_wake)) {
		pr_warn("zrn-bt: no HOST_WAKE: %ld\n", PTR_ERR(host_wake));
		host_wake = NULL;
		return;
	}
	host_wake_irq = gpiod_to_irq(host_wake);
	ret = host_wake_irq < 0 ? host_wake_irq :
		request_irq(host_wake_irq, host_wake_isr, IRQF_TRIGGER_FALLING, "bt-host-wake", NULL);
	if (ret) {
		pr_warn("zrn-bt: HOST_WAKE irq: %d\n", ret);
		host_wake_irq = -1;
		return;
	}
	device_init_wakeup(&self->dev, true);
	enable_irq_wake(host_wake_irq);
	pr_info("zrn-bt: HOST_WAKE on irq %d, a wake source\n", host_wake_irq);
}

static void host_wake_release(void)
{
	if (host_wake_irq >= 0) {
		disable_irq_wake(host_wake_irq);
		free_irq(host_wake_irq, NULL);
	}
	if (host_wake)
		gpiod_put(host_wake);
}

/* bisecting what keeps the SoC from sleeping: 1 no UART device, 2 chip left
 * off, 4 pins left alone, 8 clocks left alone, 16 no I2C bus at all */
static unsigned int test_skip;
module_param(test_skip, uint, 0444);

static int __init zrn_bt_init(void)
{
	struct i2c_adapter *bus;
	int ret;

	if (!(test_skip & 8))
		clocks_on();
	if (!(test_skip & 4)) {
		pins_uart();
		pins_sleep();
	}
	if (test_skip & 16) {
		pr_info("zrn-bt: test_skip %#x: nothing else\n", test_skip);
		return 0;
	}

	gpiod_add_lookup_table(&bus_gpios);
	bus_pdev = platform_device_register_data(NULL, "i2c-gpio", EXP_BUS, &bus_pdata, sizeof(bus_pdata));
	if (IS_ERR(bus_pdev)) {
		ret = PTR_ERR(bus_pdev);
		goto err_table;
	}
	bus = i2c_get_adapter(EXP_BUS);
	if (!bus) {
		ret = -EPROBE_DEFER;
		goto err_bus;
	}
	exp = i2c_new_dummy_device(bus, EXP_ADDR);
	i2c_put_adapter(bus);
	if (IS_ERR(exp)) {
		ret = PTR_ERR(exp);
		goto err_bus;
	}

	gpiod_add_lookup_table(&wake_gpios);
	self = platform_device_register_simple("zrn-bt", -1, NULL, 0);
	if (!IS_ERR(self)) {
		bt_wake = gpiod_get(&self->dev, "bt-wake", GPIOD_OUT_HIGH);
		if (IS_ERR(bt_wake)) {
			pr_warn("zrn-bt: no BT_WAKE: %ld\n", PTR_ERR(bt_wake));
			bt_wake = NULL;
		}
		host_wake_setup();
	}

	ret = bt_power(false);	/* a clean reset of its Bluetooth side */
	if (ret)
		goto err_exp;
	msleep(30);
	if (!(test_skip & 2)) {
		ret = bt_power(true);
		if (ret)
			goto err_exp;
		msleep(100);
	}

	if (!(test_skip & 1)) {
		ret = uart_register();
		if (ret)
			goto err_power;
	}
	pr_info("zrn-bt: test_skip %#x\n", test_skip);
	register_syscore(&bt_syscore);
	register_pm_notifier(&bt_pm_nb);
	return 0;

err_power:
	bt_power(false);
err_exp:
	host_wake_release();
	if (bt_wake)
		gpiod_put(bt_wake);
	if (!IS_ERR_OR_NULL(self))
		platform_device_unregister(self);
	gpiod_remove_lookup_table(&wake_gpios);
	i2c_unregister_device(exp);
err_bus:
	platform_device_unregister(bus_pdev);
err_table:
	gpiod_remove_lookup_table(&bus_gpios);
	return ret;
}

static void __exit zrn_bt_exit(void)
{
	unregister_pm_notifier(&bt_pm_nb);
	unregister_syscore(&bt_syscore);
	platform_device_unregister(uart_pdev);
	clkdev_drop(cl_baud);
	clkdev_drop(cl_uart);
	clk_hw_unregister_fixed_rate(uart_clk);
	bt_power(false);
	host_wake_release();
	if (bt_wake)
		gpiod_put(bt_wake);
	platform_device_unregister(self);
	gpiod_remove_lookup_table(&wake_gpios);
	i2c_unregister_device(exp);
	platform_device_unregister(bus_pdev);
	gpiod_remove_lookup_table(&bus_gpios);
}

module_init(zrn_bt_init);
module_exit(zrn_bt_exit);
MODULE_SOFTDEP("pre: i2c-gpio");
MODULE_DESCRIPTION("Galaxy S9 Bluetooth board glue: UART_BT, BT_EN, BT_WAKE");
MODULE_LICENSE("GPL");
