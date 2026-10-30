// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-cam: power one camera of the Galaxy S9 at a time, read its chip ID over
 * I2C and power it down again, in Samsung's own order.
 *
 * Nothing happens at load. Each action is one write to
 * /sys/kernel/debug/zrn-cam/run and is finished when the write returns:
 *
 *	pmic		read both camera PMICs and the iris LDOs, no writes
 *	rear front iris	full cycle: on, chip ID, off
 *	rear-rails front-rails iris-rails
 *			the rails only, no reset, clock or sensor I2C
 *	act-read	rear up; read the aperture and focus drivers, no pulse, no move
 *	act-move	rear up; drive the aperture both ways and the focus a little;
 *			ends at F1.5 with the focus landed, then rear off
 *	stream-*, intcam-*	RAW through CSIS0: see zrn-cam-csis.h
 *
 * Results are in .../result and dmesg. Rails are never left on: the off
 * sequence runs on every path and from a reboot notifier. There is no remove
 * path on purpose: try a new build by rebooting, never rmmod.
 *
 * Sources, all from the vendor kernel:
 *  fimc-is-device-module-{2l3,3h1,5f1}.c	sequences and delays
 *  s2mpb03.c, s2mpb02-regulator.c, s2mps18-private.h	registers
 *  twrp-live.dts				rails, pads, addresses
 *
 * The clock is not touched. Observed at boot: MUX_CLKCMU_CIS_CLKn selects
 * OSCCLK, the divider is /1 and the gate is open, which is the 26 MHz stock
 * asks for. Only the pad changes function, so no CMU register is written.
 * The S2MPB PMICs and sensors are reached by bit-banged I2C, which cannot hang
 * the bus. The iris rails live on the main PMIC and go only through the
 * kernel's ACPM calls, and only the three iris registers are ever written.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/uaccess.h>

int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);

/*
 * The vendor tree has no S2MPS18 regulator table, so the voltage code of the
 * iris LDOs is not known from source and this module never writes it. Read at
 * boot on 2026-10-31: 0x1c, 0x28, 0x2c, off; they decode to 1.05, 2.8 and 1.8 V
 * on the pattern of the other LDOs. Off unless asked for: on that run the iris
 * sensor did not answer, because gpg0-7 (gpio_iris_en) ignored the write.
 */
static bool iris_allowed;
module_param(iris_allowed, bool, 0644);

enum { PB03, PB02, PMIC_N };

#define BUS_PB03	99
#define BUS_PB02	100
#define BUS_SENSOR	101
#define BUS_ACT		102
#define ADDR_APERTURE	0x4c
#define ADDR_FOCUS	0x0c
#define ADDR_PB03	0x56
#define ADDR_PB02	0x59

#define PERI_PINCTRL	0x10830000
#define PERI_SIZE	0x100
#define BANK_GPP4	0x00
#define BANK_GPP5	0x20
#define BANK_GPC0	0x60
#define BANK_GPC1	0x80
#define REG_CON		0x0
#define REG_DAT		0x4
#define REG_PUD		0x8
#define REG_DRV		0xc

#define PB_ON		0x80
#define MPS_ON		0xc0

/* one LDO or buck: PMIC, control register, voltage register and code */
struct rail {
	const char *name;
	u8 pmic, en_reg, en_mask, v_reg, v_mask, code;
};

#define PB03_LDO(n, nm, c) { nm, PB03, 2 + (n), PB_ON, 2 + (n), 0x3f, c }
#define PB02_LDO(n, nm, c) { nm, PB02, 0x1d + (n), PB_ON, 0x1d + (n), 0x7f, c }

/*
 * Codes from the vendor regulator tables and the voltage stock asks for:
 * S2MPB03 LDO1,2,4 0.7 V + 12.5 mV, LDO3 0.7 V + 25 mV, LDO5-7 1.8 V + 25 mV.
 * S2MPB02 LDO1-5 0.6 V + 12.5 mV, LDO6-18 0.6 V + 25 mV.
 * BUCK2 0.4 V + 6.25 mV.
 */
static const struct rail ret_1v0  = PB03_LDO(4, "VDDD_RET_1.0V_CAM", 26);
static const struct rail a_2v95   = PB03_LDO(6, "VDDA_2.95V_CAM", 46);
static const struct rail a_2v15   = PB03_LDO(5, "VDDA_2.15V_CAM", 14);
static const struct rail d_1v0    = PB03_LDO(1, "VDDD_1.0V_CAM", 26);
static const struct rail af_com   = PB02_LDO(14, "VDDAF_COMMON_CAM", 108);
static const struct rail io_1v8   = PB03_LDO(3, "VDDIO_1.8V_CAM", 44);
static const struct rail q_1v1    = PB03_LDO(2, "VDDQ_1.1V_CAM", 32);
static const struct rail ois_1v8  = PB02_LDO(10, "VDDD_1.8V_OIS", 48);
static const struct rail vm_2v8   = PB03_LDO(7, "VDD_VM_2.8V_OIS", 40);
static const struct rail ois_2v8  = PB02_LDO(15, "VDDD_2.8V_OIS", 88);

static const struct rail ext_1v3  = { "VDD_EXT_1P3_PB02", PB02, 0x0d, 0xc0, 0x0e, 0xff, 144 };
static const struct rail af_vt    = PB02_LDO(16, "VDDAF_2.8V_VT", 88);
static const struct rail a_vt     = PB02_LDO(13, "VDDA_2.8V_VT", 88);
static const struct rail io_vt    = PB02_LDO(6, "VDDIO_1.8V_VT", 48);
static const struct rail d_vt     = PB02_LDO(5, "VDDD_1.05V_VT", 36);

/* a step of a sequence; the delay is stock's, in microseconds */
struct step {
	const struct rail *rail;	/* NULL: a marker below */
	bool on;
	unsigned int us;
	int marker;
};

enum { M_NONE, M_RESET_LOW, M_RESET_HIGH, M_BUS_ON, M_BUS_OFF, M_CLK_ON, M_CLK_OFF, M_IRIS_EN, M_IRIS_DIS,
	M_MPS_D_ON, M_MPS_A_ON, M_MPS_IO_ON, M_MPS_D_OFF, M_MPS_A_OFF, M_MPS_IO_OFF };

#define R(r, o, d) { .rail = &(r), .on = (o), .us = (d) }
#define M(m, d) { .marker = (m), .us = (d) }

/* module-2l3.c NORMAL ON, seq 0; bus, clock, reset, then NORMAL OFF */
static const struct step rear_on[] = {
	M(M_RESET_LOW, 0), R(ret_1v0, 1, 1), R(a_2v95, 1, 0), R(a_2v15, 1, 1), R(d_1v0, 1, 1),
	R(af_com, 1, 1), R(io_1v8, 1, 1), R(q_1v1, 1, 1000), R(ois_1v8, 1, 100), R(vm_2v8, 1, 0),
	R(ois_2v8, 1, 1000), M(M_BUS_ON, 0), M(M_CLK_ON, 1500), M(M_RESET_HIGH, 10000),
};
static const struct step rear_off[] = {
	R(af_com, 0, 10), R(q_1v1, 0, 1), R(d_1v0, 0, 10), R(a_2v95, 0, 0), R(a_2v15, 0, 0),
	M(M_CLK_OFF, 0), M(M_RESET_LOW, 10), M(M_BUS_OFF, 0), R(ois_2v8, 0, 0), R(vm_2v8, 0, 0),
	R(ois_1v8, 0, 0), R(ret_1v0, 0, 100), R(io_1v8, 0, 10),
};
/* module-3h1.c; star defines USE_BUCK2_REGULATOR_CONTROL. VDDD goes to 1.05 V
 * directly: stock's non-factory path has only that one voltage step. */
static const struct step front_on[] = {
	M(M_RESET_LOW, 0), R(ext_1v3, 1, 0), R(af_vt, 1, 0), R(a_vt, 1, 0), R(io_vt, 1, 0), R(d_vt, 1, 1000),
	M(M_BUS_ON, 10), M(M_RESET_HIGH, 0), M(M_CLK_ON, 9000),
};
static const struct step front_off[] = {
	M(M_CLK_OFF, 1), M(M_RESET_LOW, 0), M(M_BUS_OFF, 0), R(af_vt, 0, 0), R(d_vt, 0, 0), R(a_vt, 0, 0),
	R(io_vt, 0, 0), R(ext_1v3, 0, 0),
};
/* module-5f1.c SECURE */
static const struct step iris_on[] = {
	M(M_RESET_LOW, 0), M(M_IRIS_EN, 0), M(M_MPS_IO_ON, 0), M(M_MPS_A_ON, 0), M(M_MPS_D_ON, 1000),
	M(M_BUS_ON, 10), M(M_RESET_HIGH, 0), M(M_CLK_ON, 2000),
};
static const struct step iris_off[] = {
	M(M_CLK_OFF, 200), M(M_RESET_LOW, 0), M(M_BUS_OFF, 0), M(M_MPS_D_OFF, 0), M(M_MPS_A_OFF, 0),
	M(M_MPS_IO_OFF, 0), M(M_IRIS_DIS, 0),
};

#define MPS_L39	0x70	/* VDDD_1.05V_IRIS */
#define MPS_L40	0x71	/* VDDA_2.8V_IRIS */
#define MPS_L41	0x72	/* VDDIO_1.8V_IRIS */

struct camera {
	const char *name;
	const struct step *on, *off;
	unsigned int n_on, n_off;
	u8 addr;
	const char *i2c_chip;
	u8 scl, sda;
	u8 i2c_bank, i2c_pud;
	u8 clk_pin, clk_drv;
	const char *reset_chip;
	u8 reset_pin;
};

static const struct camera cameras[] = {
	{ "rear", rear_on, rear_off, ARRAY_SIZE(rear_on), ARRAY_SIZE(rear_off), 0x10,
	  "gpc0", 0, 1, BANK_GPC0, 3, 4, 0, "gpg2", 6 },
	{ "front", front_on, front_off, ARRAY_SIZE(front_on), ARRAY_SIZE(front_off), 0x35,
	  "gpp5", 2, 3, BANK_GPP5, 0, 5, 0, "gpg2", 5 },
	{ "iris", iris_on, iris_off, ARRAY_SIZE(iris_on), ARRAY_SIZE(iris_off), 0x10,
	  "gpp4", 6, 7, BANK_GPP4, 0, 6, 1, "gpg2", 3 },
};

static DEFINE_MUTEX(lock);
static char result[16384];
static size_t result_len;
static void __iomem *peri;
static struct i2c_client *pmic[PMIC_N];
static struct i2c_gpio_platform_data i2c_100k = { .udelay = 5 };

static struct gpiod_lookup_table pb03_gpios = {
	.dev_id = "i2c-gpio.99",
	.table = {
		GPIO_LOOKUP_IDX("gpm1", 0, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpm0", 0, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};
static struct gpiod_lookup_table pb02_gpios = {
	.dev_id = "i2c-gpio.100",
	.table = {
		GPIO_LOOKUP_IDX("gpp6", 1, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp6", 0, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};

static const struct camera *up_bus;
static struct platform_device *sensor_pdev;
static struct {
	struct gpiod_lookup_table t;
	struct gpiod_lookup e[3];
} sensor_tbl = { .t = { .dev_id = "i2c-gpio.101" } };
#define sensor_gpios sensor_tbl.t
#define sensor_lookup sensor_tbl.e
static struct i2c_client *sensor;
static struct gpio_desc *reset_gpio, *iris_en_gpio;
static u32 saved_pud;

static void note(const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_info("zrn-cam: %pV\n", &vaf);
	result_len += scnprintf(result + result_len, sizeof(result) - result_len, "%pV\n", &vaf);
	va_end(args);
}

static int pmic_buses(void)
{
	static const struct { int bus; struct gpiod_lookup_table *t; u8 addr; int id; } b[] = {
		{ BUS_PB03, &pb03_gpios, ADDR_PB03, PB03 },
		{ BUS_PB02, &pb02_gpios, ADDR_PB02, PB02 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(b); i++) {
		struct platform_device *pd;
		struct i2c_adapter *ad;
		struct i2c_client *c;

		if (pmic[b[i].id])
			continue;
		gpiod_add_lookup_table(b[i].t);
		pd = platform_device_register_data(NULL, "i2c-gpio", b[i].bus, &i2c_100k, sizeof(i2c_100k));
		if (IS_ERR(pd))
			return PTR_ERR(pd);
		ad = i2c_get_adapter(b[i].bus);
		if (!ad)
			return -ENODEV;
		c = i2c_new_dummy_device(ad, b[i].addr);
		i2c_put_adapter(ad);
		if (IS_ERR(c))
			return PTR_ERR(c);
		pmic[b[i].id] = c;
	}
	return 0;
}

static int rail_set(const struct rail *r, bool on)
{
	struct i2c_client *c = pmic[r->pmic];
	int en, v, ret;

	en = i2c_smbus_read_byte_data(c, r->en_reg);
	if (en < 0)
		return en;
	if (on) {
		if (en & r->en_mask)
			note("%s: already on (0x%02x) before us", r->name, en);
		/* the voltage first, with the LDO off, and read back */
		v = i2c_smbus_read_byte_data(c, r->v_reg);
		if (v < 0)
			return v;
		if ((v & r->v_mask) != r->code) {
			ret = i2c_smbus_write_byte_data(c, r->v_reg, (v & ~r->v_mask) | r->code);
			if (ret)
				return ret;
			v = i2c_smbus_read_byte_data(c, r->v_reg);
			if (v < 0 || (v & r->v_mask) != r->code) {
				note("%s: voltage code %d did not stick (%d)", r->name, r->code, v);
				return -EIO;
			}
		}
		en = i2c_smbus_read_byte_data(c, r->en_reg);
		if (en < 0)
			return en;
		ret = i2c_smbus_write_byte_data(c, r->en_reg, en | r->en_mask);
	} else {
		ret = i2c_smbus_write_byte_data(c, r->en_reg, en & ~r->en_mask);
	}
	if (ret)
		return ret;
	en = i2c_smbus_read_byte_data(c, r->en_reg);
	if (en < 0)
		return en;
	if (!!(en & r->en_mask) != on) {
		note("%s: enable bit did not take (0x%02x)", r->name, en);
		return -EIO;
	}
	return 0;
}

static int mps_set(u8 reg, bool on)
{
	u8 v = 0;
	int ret = s9p_acpm_pmic_update(reg, on ? MPS_ON : 0, MPS_ON);

	if (ret)
		return ret;
	ret = s9p_acpm_pmic_read(reg, &v);
	if (ret)
		return ret;
	note("main PMIC 0x%02x = 0x%02x", reg, v);
	return !!(v & MPS_ON) == on ? 0 : -EIO;
}

static void field(unsigned int off, unsigned int pin, u32 val)
{
	u32 v = readl(peri + off);

	v &= ~(0xf << (pin * 4));
	writel(v | (val << (pin * 4)), peri + off);
}

static int bus_on(const struct camera *c)
{
	struct i2c_adapter *ad;
	struct platform_device *pd;
	int ret;

	sensor_lookup[0] = (struct gpiod_lookup)GPIO_LOOKUP_IDX(c->i2c_chip, c->sda, NULL, 0,
			GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN);
	sensor_lookup[1] = (struct gpiod_lookup)GPIO_LOOKUP_IDX(c->i2c_chip, c->scl, NULL, 1,
			GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN);
	memset(&sensor_lookup[2], 0, sizeof(sensor_lookup[2]));
	gpiod_add_lookup_table(&sensor_gpios);

	/* stock's on_i2c pull: the PUD field of both pads, restored at off */
	saved_pud = readl(peri + c->i2c_bank + REG_PUD);
	up_bus = c;
	field(c->i2c_bank + REG_PUD, c->scl, c->i2c_pud);
	field(c->i2c_bank + REG_PUD, c->sda, c->i2c_pud);

	pd = platform_device_register_data(NULL, "i2c-gpio", BUS_SENSOR, &i2c_100k, sizeof(i2c_100k));
	if (IS_ERR(pd)) {
		gpiod_remove_lookup_table(&sensor_gpios);
		return PTR_ERR(pd);
	}
	sensor_pdev = pd;
	ad = i2c_get_adapter(BUS_SENSOR);
	if (!ad)
		return -ENODEV;
	sensor = i2c_new_dummy_device(ad, c->addr);
	i2c_put_adapter(ad);
	if (IS_ERR(sensor)) {
		ret = PTR_ERR(sensor);
		sensor = NULL;
		return ret;
	}
	return 0;
}

static void bus_off(void)
{
	const struct camera *c = up_bus;

	if (sensor)
		i2c_unregister_device(sensor);
	sensor = NULL;
	if (sensor_pdev) {
		platform_device_unregister(sensor_pdev);
		sensor_pdev = NULL;
		gpiod_remove_lookup_table(&sensor_gpios);
	}
	if (c) {
		u32 mask = (0xf << (c->scl * 4)) | (0xf << (c->sda * 4));
		u32 v = readl(peri + c->i2c_bank + REG_PUD);

		writel((v & ~mask) | (saved_pud & mask), peri + c->i2c_bank + REG_PUD);
	}
	up_bus = NULL;
}

/* gpc1-n: stock's "pin2" is function 2, no pull; "pin1" is output, pull 1 */
static void clk_pad(const struct camera *c, bool on)
{
	if (on) {
		field(BANK_GPC1 + REG_PUD, c->clk_pin, 0);
		field(BANK_GPC1 + REG_DRV, c->clk_pin, c->clk_drv);
		field(BANK_GPC1 + REG_CON, c->clk_pin, 2);
	} else {
		u32 d = readl(peri + BANK_GPC1 + REG_DAT);

		writel(d & ~BIT(c->clk_pin), peri + BANK_GPC1 + REG_DAT);
		field(BANK_GPC1 + REG_CON, c->clk_pin, 1);
		field(BANK_GPC1 + REG_PUD, c->clk_pin, 1);
	}
}

static struct gpiod_lookup_table ctl_gpios = {
	.dev_id = "zrn-cam",
	.table = {
		GPIO_LOOKUP("gpg2", 6, "reset-rear", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpg2", 5, "reset-front", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpg2", 3, "reset-iris", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpg0", 7, "iris-en", GPIO_ACTIVE_HIGH),
		{ }
	},
};
static struct device *ctl_dev;

static int do_marker(const struct camera *c, int m)
{
	switch (m) {
	case M_RESET_LOW:
		gpiod_set_value_cansleep(reset_gpio, 0);
		return 0;
	case M_RESET_HIGH:
		gpiod_set_value_cansleep(reset_gpio, 1);
		return 0;
	case M_BUS_ON:
		return bus_on(c);
	case M_BUS_OFF:
		bus_off();
		return 0;
	case M_CLK_ON:
		clk_pad(c, true);
		return 0;
	case M_CLK_OFF:
		clk_pad(c, false);
		return 0;
	case M_IRIS_EN:
		gpiod_set_value_cansleep(iris_en_gpio, 1);
		return 0;
	case M_IRIS_DIS:
		gpiod_set_value_cansleep(iris_en_gpio, 0);
		return 0;
	case M_MPS_IO_ON:
		return mps_set(MPS_L41, true);
	case M_MPS_A_ON:
		return mps_set(MPS_L40, true);
	case M_MPS_D_ON:
		return mps_set(MPS_L39, true);
	case M_MPS_D_OFF:
		return mps_set(MPS_L39, false);
	case M_MPS_A_OFF:
		return mps_set(MPS_L40, false);
	case M_MPS_IO_OFF:
		return mps_set(MPS_L41, false);
	}
	return 0;
}

static void wait_us(unsigned int us)
{
	if (!us)
		return;
	if (us < 1000)
		usleep_range(us, us + 50);
	else
		usleep_range(us, us + 500);
}

static void power_off(const struct camera *c)
{
	unsigned int i;

	for (i = 0; i < c->n_off; i++) {
		const struct step *s = &c->off[i];
		int ret = s->rail ? rail_set(s->rail, false) : do_marker(c, s->marker);

		if (ret)
			note("off step %u failed: %d", i, ret);	/* carry on: cut the rest */
		wait_us(s->us);
	}
}

static int power_on(const struct camera *c, bool rails_only)
{
	unsigned int i;

	for (i = 0; i < c->n_on; i++) {
		const struct step *s = &c->on[i];
		int ret;

		if (rails_only && !s->rail && s->marker != M_RESET_LOW && s->marker != M_IRIS_EN &&
		    s->marker < M_MPS_D_ON)
			continue;
		ret = s->rail ? rail_set(s->rail, true) : do_marker(c, s->marker);
		if (ret) {
			note("%s: on step %u failed: %d", c->name, i, ret);
			return ret;
		}
		wait_us(s->us);
	}
	return 0;
}

static int read16(struct i2c_client *cl, u16 reg, void *buf, int len)
{
	u8 a[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg m[2] = {
		{ .addr = cl->addr, .len = 2, .buf = a },
		{ .addr = cl->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret = i2c_transfer(cl->adapter, m, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static void read_id(const struct camera *c)
{
	u8 id[2], rev;
	int ret = -EIO, t;

	for (t = 0; t < 3; t++) {
		ret = read16(sensor, 0x0000, id, 2);
		if (!ret)
			break;
		msleep(5);
	}
	if (ret) {
		note("%s: no answer at 0x%02x: %d", c->name, c->addr, ret);
		return;
	}
	if (read16(sensor, 0x0002, &rev, 1))
		rev = 0xff;
	note("%s: chip ID 0x%02x%02x revision 0x%02x", c->name, id[0], id[1], rev);
}

/*
 * Aperture (AK7372 at 0x4c) and focus (AK737x at 0x0c), on stock's hsi2c_1 pads
 * gpc0-2 SCL and gpc0-3 SDA. From fimc-is-aperture-ak7372.c, fimc-is-actuator-ak737x.c
 * and fimc_is_sensor_peri_actuator_softlanding(). OIS at 0x24 is left alone:
 * star's USE_OIS_SHIFT_FOR_APERTURE needs LDU or factory, which stock.config has off.
 *
 * Only the registers stock's normal path writes can be written here. Register
 * 0x03 stores to the chip's own memory and must never be written.
 */
#define ACT_SCL		2
#define ACT_SDA		3
#define ACT_PUD		3	/* the same pull as the sensor's pads, from on_i2c */
#define AP_HOLD_US	10000
#define AP_HOLD_LIMIT_US 20000
#define AP_F1_5		0
#define AP_F2_4		1023
#define AP_IDLE		511
#define AF_FIRST	120
#define AF_TEST		200
#define MOVES_PER_LOAD	10

static int moves_left = MOVES_PER_LOAD;
static struct i2c_client *ap_cl, *af_cl;
static struct platform_device *act_pdev;
static u32 act_saved_pud;
static struct gpiod_lookup_table act_gpios = {
	.dev_id = "i2c-gpio.102",
	.table = {
		GPIO_LOOKUP_IDX("gpc0", ACT_SDA, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpc0", ACT_SCL, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};

static int act_write(struct i2c_client *cl, u8 reg, u8 val)
{
	bool ok = reg <= 0x02 || (cl == ap_cl && (reg == 0xa6 || reg == 0xae));

	if (!ok) {
		note("refused write 0x%02x to register 0x%02x", cl->addr, reg);
		return -EPERM;
	}
	return i2c_smbus_write_byte_data(cl, reg, val);
}

static void act_bus_off(void)
{
	u32 mask = (0xf << (ACT_SCL * 4)) | (0xf << (ACT_SDA * 4));

	if (ap_cl)
		i2c_unregister_device(ap_cl);
	if (af_cl)
		i2c_unregister_device(af_cl);
	ap_cl = af_cl = NULL;
	if (act_pdev) {
		u32 v;

		platform_device_unregister(act_pdev);
		act_pdev = NULL;
		gpiod_remove_lookup_table(&act_gpios);
		v = readl(peri + BANK_GPC0 + REG_PUD);
		writel((v & ~mask) | (act_saved_pud & mask), peri + BANK_GPC0 + REG_PUD);
	}
}

static int act_bus_on(void)
{
	struct platform_device *pd;
	struct i2c_adapter *ad;
	struct i2c_client *a, *f;

	gpiod_add_lookup_table(&act_gpios);
	act_saved_pud = readl(peri + BANK_GPC0 + REG_PUD);
	field(BANK_GPC0 + REG_PUD, ACT_SCL, ACT_PUD);
	field(BANK_GPC0 + REG_PUD, ACT_SDA, ACT_PUD);
	pd = platform_device_register_data(NULL, "i2c-gpio", BUS_ACT, &i2c_100k, sizeof(i2c_100k));
	if (IS_ERR(pd)) {
		u32 mask = (0xf << (ACT_SCL * 4)) | (0xf << (ACT_SDA * 4));
		u32 v = readl(peri + BANK_GPC0 + REG_PUD);

		writel((v & ~mask) | (act_saved_pud & mask), peri + BANK_GPC0 + REG_PUD);
		gpiod_remove_lookup_table(&act_gpios);
		return PTR_ERR(pd);
	}
	act_pdev = pd;
	ad = i2c_get_adapter(BUS_ACT);
	if (!ad)
		return -ENODEV;
	a = i2c_new_dummy_device(ad, ADDR_APERTURE);
	f = i2c_new_dummy_device(ad, ADDR_FOCUS);
	i2c_put_adapter(ad);
	if (!IS_ERR(a))
		ap_cl = a;
	if (!IS_ERR(f))
		af_cl = f;
	if (IS_ERR(a))
		return PTR_ERR(a);
	if (IS_ERR(f))
		return PTR_ERR(f);
	return 0;
}

/* Read-only snapshot. Registers 0x80-0x8f are read to look for a hall or status
 * value that follows the move; their meaning is not known from vendor source. */
static void act_snapshot(struct i2c_client *cl, const char *label)
{
	static const u8 regs[] = { 0x00, 0x01, 0x02, 0x03, 0x4b, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85,
		0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0xa6, 0xae, 0xf3 };
	char line[240];
	size_t n = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		int v = i2c_smbus_read_byte_data(cl, regs[i]);

		if (v < 0)
			n += scnprintf(line + n, sizeof(line) - n, " %02x=E", regs[i]);
		else
			n += scnprintf(line + n, sizeof(line) - n, " %02x=%02x", regs[i], v);
	}
	note("0x%02x %s:%s", cl->addr, label, line);
}

static int ap_position(int pos)
{
	int ret = act_write(ap_cl, 0x00, (pos >> 2) & 0xff);

	return ret ? ret : act_write(ap_cl, 0x01, (pos << 6) & 0xc0);
}

/* sensor_ak7372_aperture_init */
static int ap_init(void)
{
	int ret = act_write(ap_cl, 0xae, 0x3b);

	if (!ret)
		ret = ap_position(AP_IDLE);
	if (!ret)
		ret = act_write(ap_cl, 0x02, 0x00);
	return ret;
}

/*
 * sensor_ak7372_aperture_set_value without its same-value shortcut: 0xa6=0x7b,
 * the end position, 10 ms, 0xa6=0, back to 511, 15 ms. The pulse is timed, and
 * a pulse that ran long stops any further move. Power-off also ends a pulse.
 */
static int ap_move(int pos, const char *name)
{
	ktime_t t0, t1;
	int a6, h, l, ret, close_ret;
	s64 hold;

	if (moves_left <= 0) {
		note("aperture: no moves left in this load");
		return -EPERM;
	}
	moves_left--;
	t0 = ktime_get();
	ret = act_write(ap_cl, 0xa6, 0x7b);
	if (!ret)
		ret = ap_position(pos);
	a6 = i2c_smbus_read_byte_data(ap_cl, 0xa6);
	h = i2c_smbus_read_byte_data(ap_cl, 0x00);
	l = i2c_smbus_read_byte_data(ap_cl, 0x01);
	hold = ktime_us_delta(ktime_get(), t0);
	if (hold < AP_HOLD_US)
		usleep_range(AP_HOLD_US - hold, AP_HOLD_US - hold + 300);
	close_ret = act_write(ap_cl, 0xa6, 0x00);
	if (close_ret)
		close_ret = act_write(ap_cl, 0xa6, 0x00);
	t1 = ktime_get();
	hold = ktime_to_us(ktime_sub(t1, t0));
	if (!close_ret)
		close_ret = ap_position(AP_IDLE);
	usleep_range(15000, 16000);
	note("aperture %s: pulse %lld us, during it a6=0x%02x pos=0x%02x 0x%02x, step %d, close %d",
	     name, hold, a6 & 0xff, h & 0xff, l & 0xff, ret, close_ret);
	act_snapshot(ap_cl, name);
	if (ret)
		return ret;
	if (close_ret)
		return close_ret;
	if (hold > AP_HOLD_LIMIT_US) {
		note("aperture: pulse over the limit, stopping");
		return -ETIMEDOUT;
	}
	if ((a6 & 0xff) != 0x7b)
		note("aperture: 0xa6 did not read back as written, or is not readable");
	return 0;
}

static int af_position(int pos)
{
	int ret = act_write(af_cl, 0x00, (pos & 0x1ff) >> 1);

	return ret ? ret : act_write(af_cl, 0x01, (pos & 1) << 7);
}

/* Stock's ID check against the DT's vendor_product_id pairs */
static bool af_known(void)
{
	int f3 = i2c_smbus_read_byte_data(af_cl, 0xf3);
	int r3 = i2c_smbus_read_byte_data(af_cl, 0x03);

	note("focus: id f3=%d 03=%d", f3, r3);
	return f3 == 0x0e || r3 == 0x09 || r3 == 0x0e;
}

/* Init at vendor_first_pos, a small move and back, then stock's landing: to 0, 200 ms */
static int af_move(void)
{
	int pos, ret;

	ret = af_position(AF_FIRST);
	if (ret)
		return ret;
	usleep_range(2000, 2100);
	ret = act_write(af_cl, 0x02, 0x00);
	if (ret)
		return ret;
	msleep(30);
	act_snapshot(af_cl, "focus 120");
	for (pos = AF_FIRST + 20; pos <= AF_TEST && !ret; pos += 20) {
		ret = af_position(pos);
		msleep(10);
	}
	msleep(30);
	act_snapshot(af_cl, "focus 200");
	for (pos = AF_TEST - 20; pos >= AF_FIRST && !ret; pos -= 20) {
		ret = af_position(pos);
		msleep(10);
	}
	msleep(30);
	act_snapshot(af_cl, "focus 120 again");
	/* land on every path, whatever failed above */
	af_position(0);
	msleep(200);
	act_snapshot(af_cl, "focus landed");
	return ret;
}

static void act_run(bool move)
{
	int ret = act_bus_on();

	if (ret) {
		note("actuator bus: %d", ret);
		act_bus_off();
		return;
	}
	act_snapshot(ap_cl, "aperture before init");
	ret = i2c_smbus_read_byte_data(ap_cl, 0x02);
	if (ret < 0) {
		note("aperture: no answer at 0x4c: %d", ret);
	} else {
		ret = ap_init();
		note("aperture init: %d", ret);
		act_snapshot(ap_cl, "aperture after init");
		if (!ret && move) {
			static const int seq[] = { AP_F2_4, AP_F1_5, AP_F2_4, AP_F1_5 };
			int i;

			for (i = 0; i < ARRAY_SIZE(seq) && !ret; i++)
				ret = ap_move(seq[i], seq[i] == AP_F1_5 ? "F1.5" : "F2.4");
			/* a stop on the way still ends at stock's closing value, if allowed */
			if (ret && moves_left > 0)
				ap_move(AP_F1_5, "F1.5 after stop");
			note("aperture moves left in this load: %d", moves_left);
		}
	}
	act_snapshot(af_cl, "focus before");
	if (!af_known()) {
		note("focus: no known product ID, not moved");
	} else if (move) {
		ret = af_move();
		note("focus moves: %d", ret);
	}
	act_bus_off();
}

#include "zrn-cam-csis.h"
#include "zrn-cam-live.h"

static void pmic_dump(void)
{
	static const u8 pb03[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	static const u8 pb02[] = { 0, 3, 4, 0x0d, 0x0e, 0x22, 0x23, 0x27, 0x2a, 0x2b, 0x2c, 0x2d };
	static const u8 mps[] = { MPS_L39, MPS_L40, MPS_L41 };
	u8 v;
	int i, r;

	for (i = 0; i < ARRAY_SIZE(pb03); i++) {
		r = i2c_smbus_read_byte_data(pmic[PB03], pb03[i]);
		note("S2MPB03 %02x = %d (0x%02x)", pb03[i], r, r & 0xff);
	}
	for (i = 0; i < ARRAY_SIZE(pb02); i++) {
		r = i2c_smbus_read_byte_data(pmic[PB02], pb02[i]);
		note("S2MPB02 %02x = %d (0x%02x)", pb02[i], r, r & 0xff);
	}
	for (i = 0; i < ARRAY_SIZE(mps); i++) {
		v = 0;
		r = s9p_acpm_pmic_read(mps[i], &v);
		note("S2MPS18 %02x = 0x%02x (%d)", mps[i], v, r);
	}
}

static ssize_t run_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	char cmd[32];
	size_t n = min(len, sizeof(cmd) - 1);
	bool rails = false;
	const struct camera *c = NULL;
	bool act = false, act_moves = false, stream = false, st_frame = false, st_tp = false, st_front = false;
	int i, ret;

	if (copy_from_user(cmd, ubuf, n))
		return -EFAULT;
	cmd[n] = 0;
	strim(cmd);

	mutex_lock(&lock);
	if (live_task) {
		mutex_unlock(&lock);
		return -EBUSY;
	}
	result_len = 0;
	result[0] = 0;
	ret = pmic_buses();
	if (ret) {
		note("PMIC buses: %d", ret);
		goto done;
	}
	if (!strcmp(cmd, "pmic")) {
		pmic_dump();
		goto done;
	}
	if (!strncmp(cmd, "dvfs-", 5)) {
		u32 dom = 0, khz = 0;

		if (!top_cmu)
			top_cmu = ioremap(0x1a240000, 0x3000);
		bus_note("before");
		/* only the two camera domains of the stock DT */
		if (sscanf(cmd, "dvfs-%u-%u", &dom, &khz) == 2 && (dom == 5 || dom == 7) &&
		    khz >= 100000 && khz <= INTCAM_KHZ) {
			ret = apm_set(dom, khz);
			note("APM domain %u at %u kHz: %d", dom, khz, ret);
			usleep_range(1000, 2000);
			bus_note("after");
		} else {
			ret = -EINVAL;
		}
		goto done;
	}
	if (!strncmp(cmd, "stream-", 7) || !strncmp(cmd, "fstream-", 8)) {
		if (!top_cmu)
			top_cmu = ioremap(0x1a240000, 0x3000);
		stream = true;
		st_front = cmd[0] == 'f';
		st_frame = strstr(cmd, "-frame");
		st_tp = strstr(cmd, "-tp");
		c = st_front ? &cameras[1] : &cameras[0];
	}
	if (!strcmp(cmd, "act-read") || !strcmp(cmd, "act-move")) {
		act = true;
		act_moves = !strcmp(cmd, "act-move");
		c = &cameras[0];
	}
	for (i = 0; !c && i < ARRAY_SIZE(cameras); i++) {
		size_t l = strlen(cameras[i].name);

		if (!strncmp(cmd, cameras[i].name, l)) {
			if (!cmd[l] || !strcmp(cmd + l, "-rails")) {
				c = &cameras[i];
				rails = cmd[l];
			}
			break;
		}
	}
	if (!c) {
		ret = -EINVAL;
		goto done;
	}
	if (!strcmp(c->name, "iris") && !iris_allowed) {
		note("iris: not allowed in this load");
		ret = -EPERM;
		goto done;
	}
	/* a PMIC that does not answer refuses the whole cycle */
	if (strcmp(c->name, "iris") &&
	    (i2c_smbus_read_byte_data(pmic[PB03], 0) < 0 || i2c_smbus_read_byte_data(pmic[PB02], 0) < 0)) {
		note("PMIC not answering; nothing switched");
		ret = -EIO;
		goto done;
	}
	reset_gpio = gpiod_get(ctl_dev, c == &cameras[0] ? "reset-rear" : c == &cameras[1] ? "reset-front" : "reset-iris",
			GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio)) {
		ret = PTR_ERR(reset_gpio);
		note("reset pin: %d", ret);
		goto done;
	}
	if (c == &cameras[2]) {
		iris_en_gpio = gpiod_get(ctl_dev, "iris-en", GPIOD_OUT_LOW);
		if (IS_ERR(iris_en_gpio)) {
			ret = PTR_ERR(iris_en_gpio);
			iris_en_gpio = NULL;
			gpiod_put(reset_gpio);
			note("iris enable pin: %d", ret);
			goto done;
		}
	}
	ret = power_on(c, rails);
	if (!ret && stream)
		ret = stream_run(st_front, st_frame, st_tp);
	else if (!ret && !rails)
		read_id(c);
	if (!ret && act)
		act_run(act_moves);
	if (!ret && rails)
		note("%s: rails up", c->name);
	power_off(c);
	note("%s: off", c->name);
	gpiod_put(reset_gpio);
	reset_gpio = NULL;
	if (iris_en_gpio) {
		gpiod_put(iris_en_gpio);
		iris_en_gpio = NULL;
	}
done:
	mutex_unlock(&lock);
	return ret ? ret : len;
}

static ssize_t result_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	ssize_t r;

	mutex_lock(&lock);
	r = simple_read_from_buffer(ubuf, len, pos, result, strnlen(result, sizeof(result)));
	mutex_unlock(&lock);
	return r;
}

/* the last frame's area, for a root reader; pictures go to /run and no further */
static ssize_t frame_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	ssize_t r = 0;

	mutex_lock(&lock);
	if (frame)
		r = simple_read_from_buffer(ubuf, len, pos, frame + FRAME_FRONT, FRAME_AREA);
	mutex_unlock(&lock);
	return r;
}

static const struct file_operations frame_fops = { .owner = THIS_MODULE, .read = frame_read };
static const struct file_operations run_fops = { .owner = THIS_MODULE, .write = run_write };
static const struct file_operations result_fops = { .owner = THIS_MODULE, .read = result_read };

static int reboot_cb(struct notifier_block *nb, unsigned long a, void *d)
{
	/* a cycle in progress holds the lock until it is off; wait out to one second */
	unsigned int i;

	stop_now = true;
	live_stop();
	for (i = 0; i < 100 && !mutex_trylock(&lock); i++)
		msleep(10);
	if (i < 100)
		mutex_unlock(&lock);
	return NOTIFY_DONE;
}
static struct notifier_block reboot_nb = { .notifier_call = reboot_cb };

static struct platform_device *ctl_pdev;

static int __init zrn_cam_init(void)
{
	struct dentry *d;

	/* gpiod lookups need a device; nothing is requested until a write */
	ctl_pdev = platform_device_register_simple("zrn-cam", -1, NULL, 0);
	if (IS_ERR(ctl_pdev))
		return PTR_ERR(ctl_pdev);
	ctl_dev = &ctl_pdev->dev;
	gpiod_add_lookup_table(&ctl_gpios);
	peri = ioremap(PERI_PINCTRL, PERI_SIZE);
	if (!peri)
		return -ENOMEM;
	register_reboot_notifier(&reboot_nb);
	d = debugfs_create_dir("zrn-cam", NULL);
	debugfs_create_file("run", 0200, d, NULL, &run_fops);
	debugfs_create_file("result", 0400, d, NULL, &result_fops);
	debugfs_create_file("frame", 0400, d, NULL, &frame_fops);
	ctl_pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	ctl_pdev->dev.dma_mask = &ctl_pdev->dev.coherent_dma_mask;
	stream_ws = wakeup_source_register(NULL, "zrn-cam");
	if (misc_register(&live_misc))
		pr_warn("zrn-cam: no /dev/zrn-cam\n");
	pr_info("zrn-cam: loaded, idle\n");
	return 0;
}
module_init(zrn_cam_init);
MODULE_SOFTDEP("pre: i2c-gpio");
MODULE_DESCRIPTION("Galaxy S9 cameras: power, chip ID, power down");
MODULE_LICENSE("GPL");
