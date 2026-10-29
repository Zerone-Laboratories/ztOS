/* SPDX-License-Identifier: GPL-2.0 */
/*
 * What Broadcom's and Samsung's 2018 Android-kernel sources expect and a
 * mainline kernel no longer has (or never had), so that they build here with
 * as few edits as possible. Force-included ahead of every file.
 */
#ifndef ZRN_COMPAT_H
#define ZRN_COMPAT_H

#include <linux/version.h>
#include <linux/device.h>
#include <linux/pm_wakeup.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/sched/signal.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/gpio/consumer.h>

/* which phone, and which generation of the chip's patch */
#define CONFIG_SENSORS_SSP_STAR 1
#define CONFIG_SENSORS_SSP_BBD 1
#define CONFIG_BCM_GPS_SPI_DRIVER 1
#define ANDROID_VERSION 90000

/* Android wakelocks: nothing here suspends the system, so they are no-ops */
struct wake_lock { int unused; };
#define WAKE_LOCK_SUSPEND 0
static inline void wake_lock_init(struct wake_lock *l, int type, const char *name) { }
static inline void wake_lock_destroy(struct wake_lock *l) { }
static inline void wake_lock(struct wake_lock *l) { }
static inline void wake_unlock(struct wake_lock *l) { }
static inline void wake_lock_timeout(struct wake_lock *l, long timeout) { }
static inline int wake_lock_active(struct wake_lock *l) { return 0; }


/* time: the 2038-unsafe types and helpers these sources use */
struct timeval { long tv_sec; long tv_usec; };
static inline void do_gettimeofday(struct timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
}
#define timespec timespec64
#define ktime_to_timespec ktime_to_timespec64
#define timespec_sub timespec64_sub
#define timespec_to_ns timespec64_to_ns
#define getnstimeofday ktime_get_real_ts64
#define get_monotonic_boottime ktime_get_boottime_ts64

/* timers: the old callback took an unsigned long */
#include <linux/timer.h>
#define setup_timer(t, fn, data) timer_setup(t, (void (*)(struct timer_list *))(fn), 0)
#define del_timer_sync timer_delete_sync

#define class_create(owner, name) class_create(name)

/*
 * GPIOs. The sources keep them as legacy numbers read from the device tree;
 * here the "numbers" index a small table the board file fills with
 * descriptors (zrn_bbd_board.c).
 */
enum { ZRN_GPIO_HOST_REQ, ZRN_GPIO_MCU_REQ, ZRN_GPIO_MCU_RESP, ZRN_GPIO_GPS_PWR, ZRN_GPIO_AP_SLEEP,
       ZRN_GPIO_SHUB_INT, ZRN_GPIO_SCK, ZRN_GPIO_MOSI, ZRN_GPIO_MISO, ZRN_GPIO_CS, ZRN_GPIO_COUNT };
void zrn_mcu_req(int high);
extern struct gpio_desc *zrn_gpios[ZRN_GPIO_COUNT];
int zrn_named_gpio(const char *name);
#define of_get_named_gpio(node, name, index) zrn_named_gpio(name)
#define gpio_get_value(n) gpiod_get_value(zrn_gpios[n])
/* MCU_REQ shares a data register with the bit-banged SPI pins: one writer */
#define gpio_set_value(n, v) ((n) == ZRN_GPIO_MCU_REQ ? zrn_mcu_req(v) : gpiod_set_value(zrn_gpios[n], v))
#define gpio_to_irq(n) gpiod_to_irq(zrn_gpios[n])
#define gpio_request(n, label) 0
#define gpio_free(n) do { } while (0)
#define gpio_is_valid(n) ((n) >= 0 && (n) < ZRN_GPIO_COUNT)
#define gpio_direction_output(n, v) gpiod_direction_output(zrn_gpios[n], v)
#define gpio_direction_input(n) gpiod_direction_input(zrn_gpios[n])

/* Samsung's ssp_suspend / ssp_resume (not built here), in the board file:
 * the AP_SLEEP pulse, and the resume counter ztoscsf-sensord follows */
void zrn_ssp_suspend(void);
void zrn_ssp_resume(void);

/* the SPI driver's registration, called from the board file */
void bbd_exit(void);
int zrn_bcm_spi_init(void);
void zrn_bcm_spi_exit(void);

#endif
