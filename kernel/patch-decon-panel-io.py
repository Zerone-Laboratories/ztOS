#!/usr/bin/env python3
"""Panel reads, raw panel commands, and a full brightness table.

  patch-decon-panel-io.py <kernel tree>

The S6E3HA8 sets its brightness from several registers at once: gamma (CA,
34 bytes), AOR (B1), ELVSS (B5), VINT, IRC and more, latched together by
F7 03. Samsung's driver computes the gamma for every level from calibration
the factory burnt into each panel (MTP, register C8), so it cannot be a
fixed table. s9p-decon-backlight.patch sends AOR alone, and because AOR only
fine-tunes within a gamma step, its table saws up and down: the panel's
brightness jumped about between levels, which is why ztMETRO used four
levels and darkened the picture in between.

This adds what user space needs to do it properly, with no further kernel
changes:

  /sys/devices/.../panel_read   write "C8 34": read 34 bytes of register C8
                                (with the level-2 key on); read: the bytes
                                in hex
  /sys/devices/.../panel_cmd    write packets, hex bytes, ';' between
                                packets: sent as DCS writes, in one go
                                between frames
  /sys/devices/.../bl_table     write a brightness table (format below);
                                from then on the backlight device sends the
                                level's whole packet set, and its
                                max_brightness is the table's last level

bl_table: "S9BT", u16 levels, u16 stride (little-endian), then `levels`
entries of `stride` bytes; each entry is packets as [u8 length][bytes],
ended by a zero length, sent verbatim (the keys and F7 03 included). Bit 7
of a length sends that packet as a DCS long write even if it is short, as
Samsung's driver sends every 2-byte packet but its B0 offsets. All
three are root's only. Applied after patch-decon-panel-lock.py.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/gpu/drm/s9p/s9p-decon.c"
s = path.read_text()
if "s9p_dsim_read" in s:
    print("patch-decon-panel-io: already applied")
    sys.exit(0)

edits = [
    # registers
    ("""#define DSIM_PKTHDR			0x0058
#define DSIM_PAYLOAD			0x005C""",
     """#define  DSIM_INTSRC_RX_DATA_DONE	BIT(18)
#define DSIM_PKTHDR			0x0058
#define  DSIM_PKTHDR_BTA		BIT(24)
#define DSIM_PAYLOAD			0x005C
#define DSIM_RXFIFO			0x0060
#define DSIM_FIFOCTRL			0x0068
#define  DSIM_FIFOCTRL_EMPTY_RX		BIT(12)
#define  DSIM_FIFOCTRL_EMPTY_PH_SFR	BIT(10)
#define  DSIM_INTSRC_FRAME_DONE	BIT(24)
#define DSIM_DPHY_STATUS		0x001C
#define  DSIM_DPHY_STOP_STATE_DAT	0xf	/* all four data lanes idle */
#define DECON_GLOBAL_CONTROL		0x0000
#define  DECON_GLOBAL_IDLE_STATUS	BIT(5)
/* DSI packet types for reading (the MIPI ones; this driver has its own
 * MIPI_DCS_* names, which <video/mipi_display.h> would clash with) */
#define S9P_DSI_MAX_RETURN_SIZE		0x37
#define S9P_DSI_DCS_READ		0x06
#define S9P_DSI_RX_ERROR_REPORT		0x02
#define S9P_DSI_RX_EOT			0x08
#define S9P_DSI_RX_GEN_SHORT_1		0x11
#define S9P_DSI_RX_GEN_SHORT_2		0x12
#define S9P_DSI_RX_GEN_LONG		0x1A
#define S9P_DSI_RX_DCS_LONG		0x1C
#define S9P_DSI_RX_DCS_SHORT_1		0x21
#define S9P_DSI_RX_DCS_SHORT_2		0x22"""),
    # state
    ("""	struct mutex panel_lock;	/* flips and panel commands take turns */
};""",
     """	struct mutex panel_lock;	/* flips and panel commands take turns */
	u8 panel_rd[128];		/* the last panel_read */
	int panel_rd_len;
	u8 *bl_table;			/* see patch-decon-panel-io.py */
	unsigned int bl_levels, bl_stride;
	u8 *bl_upload;			/* bl_table as it arrives */
	size_t bl_upload_len;
	int bl_sent;			/* table level on the panel now; -1 none */
	unsigned long bl_last;		/* jiffies of the last brightness change */
	struct delayed_work bl_settle;	/* the whole set, once changes stop */
	unsigned long bl_on_at;		/* jiffies of the last display-on */
};"""),
]
for a, b in edits:
    assert a in s, a[:60]
    s = s.replace(a, b, 1)

# the read, the raw writes and the table: after the AOR brightness
anchor = "static int s9p_bl_update(struct backlight_device *bl)"
assert anchor in s
code = r'''/* ------------------------------------------------------- panel reads */

/* one packet header on its own (a short packet, or a read request) */
static int s9p_dsim_hdr(struct s9p_decon *priv, u8 id, u8 d0, u8 d1, bool bta)
{
	u32 v;

	writel_relaxed(DSIM_INTSRC_PH_FIFO_EMPTY, priv->dsim + DSIM_INTSRC);
	writel_relaxed((bta ? DSIM_PKTHDR_BTA : 0) | DSIM_PKTHDR_ID(id) |
		       DSIM_PKTHDR_DATA0(d0) | DSIM_PKTHDR_DATA1(d1),
		       priv->dsim + DSIM_PKTHDR);
	return readl_poll_timeout_atomic(priv->dsim + DSIM_INTSRC, v,
					 v & DSIM_INTSRC_PH_FIFO_EMPTY, 10, 20000);
}

/*
 * A DCS read, as the vendor's dsim_read_data does it: the most it may send
 * back (set maximum return packet size), the read request with a bus
 * turnaround, then the reply out of the RX FIFO. The caller holds
 * panel_lock and the frame trigger off.
 */
static int s9p_dsim_read(struct s9p_decon *priv, u8 reg, u8 *buf, size_t len)
{
	u32 v, rx;
	int got = 0, depth = 64, i;

	if (!priv->dsim || !len || len > sizeof(priv->panel_rd))
		return -EINVAL;
	writel_relaxed(DSIM_INTSRC_RX_DATA_DONE, priv->dsim + DSIM_INTSRC);
	if (s9p_dsim_hdr(priv, S9P_DSI_MAX_RETURN_SIZE,
			 len & 0xff, len >> 8, false) ||
	    s9p_dsim_hdr(priv, S9P_DSI_DCS_READ, reg, 0, true))
		return -EIO;
	if (readl_poll_timeout_atomic(priv->dsim + DSIM_INTSRC, v,
				      v & DSIM_INTSRC_RX_DATA_DONE, 20, 100000))
		return -ETIMEDOUT;
	do {
		rx = readl_relaxed(priv->dsim + DSIM_RXFIFO);
		switch (rx & 0xff) {
		case S9P_DSI_RX_ERROR_REPORT:
			/* the panel acknowledges with a report, before or after
			 * the data; Samsung counts only bits 0-5 and 8-13 as
			 * errors (MIPI_DSI_ERR_BIT_MASK 0x3f3f): bit 15 comes
			 * with every read on this panel */
			if ((rx >> 8) & 0x3f3f)
				dev_warn(priv->drm.dev, "panel read %02x: error report %04x\n",
					 reg, (rx >> 8) & 0xffff);
			break;
		case S9P_DSI_RX_EOT:
			break;
		case S9P_DSI_RX_DCS_SHORT_1:
		case S9P_DSI_RX_GEN_SHORT_1:
			buf[0] = (rx >> 8) & 0xff;
			got = 1;
			break;
		case S9P_DSI_RX_DCS_SHORT_2:
		case S9P_DSI_RX_GEN_SHORT_2:
			buf[0] = (rx >> 8) & 0xff;
			if (len > 1)
				buf[1] = (rx >> 16) & 0xff;
			got = min_t(int, 2, len);
			break;
		case S9P_DSI_RX_DCS_LONG:
		case S9P_DSI_RX_GEN_LONG:
			got = min_t(int, (rx >> 8) & 0xffff, len);
			for (i = 0; i < ((rx >> 8) & 0xffff); i += 4) {
				u32 w = readl_relaxed(priv->dsim + DSIM_RXFIFO);
				int j;

				for (j = 0; j < 4 && i + j < got; j++)
					buf[i + j] = (w >> (8 * j)) & 0xff;
			}
			break;
		default:
			dev_warn(priv->drm.dev, "panel read %02x: packet %08x\n", reg, rx);
			return -EIO;
		}
	} while (!(readl_relaxed(priv->dsim + DSIM_FIFOCTRL) & DSIM_FIFOCTRL_EMPTY_RX) &&
		 --depth);
	return got ? got : -EIO;
}

static ssize_t panel_read_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct s9p_decon *priv = dev_get_drvdata(dev);
	unsigned int reg, len;
	int ret = -ENODEV;

	if (sscanf(buf, "%x %u", &reg, &len) != 2 || reg > 0xff || !len ||
	    len > sizeof(priv->panel_rd))
		return -EINVAL;
	mutex_lock(&priv->panel_lock);
	if (priv->enabled) {
		s9p_hw_trigger_mask(priv);
		s9p_wait_shadow_idle(priv);
		s9p_dsim_write(priv, s9p_key2_on, sizeof(s9p_key2_on));
		ret = s9p_dsim_read(priv, reg, priv->panel_rd, len);
		s9p_dsim_write(priv, s9p_key2_off, sizeof(s9p_key2_off));
		s9p_hw_trigger_unmask(priv);
	}
	priv->panel_rd_len = ret > 0 ? ret : 0;
	mutex_unlock(&priv->panel_lock);
	return ret < 0 ? ret : count;
}

static ssize_t panel_read_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct s9p_decon *priv = dev_get_drvdata(dev);
	int i, n = 0;

	mutex_lock(&priv->panel_lock);
	for (i = 0; i < priv->panel_rd_len; i++)
		n += sysfs_emit_at(buf, n, "%02x%c", priv->panel_rd[i],
				   i == priv->panel_rd_len - 1 ? '\n' : ' ');
	mutex_unlock(&priv->panel_lock);
	return n;
}
static DEVICE_ATTR_ADMIN_RW(panel_read);

/* "F0 5A 5A; B1 00 0C; F7 03": DCS writes, all sent between two frames */
static ssize_t panel_cmd_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct s9p_decon *priv = dev_get_drvdata(dev);
	u8 *pkts, *p;
	size_t i = 0, n = 0, start = 0;
	unsigned int byte;
	int used;

	if (count > 4096)
		return -E2BIG;
	pkts = kmalloc(count + 2, GFP_KERNEL);	/* [len][bytes]..., 0 */
	if (!pkts)
		return -ENOMEM;
	p = pkts + 1;
	while (i < count) {
		if (buf[i] == ';' || buf[i] == '\n') {
			if (p - pkts - start > 1) {
				pkts[start] = p - pkts - start - 1;
				start = p - pkts;
				p++;
			}
			i++;
			continue;
		}
		if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == ',') {
			i++;
			continue;
		}
		if (sscanf(buf + i, "%2x%n", &byte, &used) != 1) {
			kfree(pkts);
			return -EINVAL;
		}
		*p++ = byte;
		i += used;
	}
	if (p - pkts - start > 1) {
		pkts[start] = p - pkts - start - 1;
		start = p - pkts;
	}
	pkts[start] = 0;

	mutex_lock(&priv->panel_lock);
	if (priv->enabled) {
		s9p_hw_trigger_mask(priv);
		s9p_wait_shadow_idle(priv);
		for (n = 0; pkts[n]; n += pkts[n] + 1)
			s9p_dsim_write(priv, pkts + n + 1, pkts[n]);
		s9p_hw_trigger_unmask(priv);
	}
	mutex_unlock(&priv->panel_lock);
	kfree(pkts);
	return count;
}
static DEVICE_ATTR_WO(panel_cmd);

/* ------------------------------------------------- the brightness table */

/* a DCS long write whatever its length (s9p_dsim_write sends 1-2 bytes short) */
static void s9p_dsim_write_long(struct s9p_decon *priv, const u8 *data, size_t len)
{
	u32 v, word = 0;
	size_t i;

	if (!priv->dsim || !s9p_panel_dcs || !len)
		return;
	writel_relaxed(DSIM_INTSRC_PH_FIFO_EMPTY, priv->dsim + DSIM_INTSRC);
	for (i = 0; i < len; i++) {
		word |= (u32)data[i] << (8 * (i % 4));
		if (i % 4 == 3 || i == len - 1) {
			writel_relaxed(word, priv->dsim + DSIM_PAYLOAD);
			word = 0;
		}
	}
	writel_relaxed(DSIM_PKTHDR_ID(MIPI_DCS_LONG_WRITE) |
		       DSIM_PKTHDR_DATA0(len & 0xff) | DSIM_PKTHDR_DATA1(len >> 8),
		       priv->dsim + DSIM_PKTHDR);
	if (readl_poll_timeout_atomic(priv->dsim + DSIM_INTSRC, v,
				      v & DSIM_INTSRC_PH_FIFO_EMPTY, 10, 20000))
		dev_warn_once(priv->drm.dev, "panel did not take DCS %02x\n", data[0]);
}

/*
 * the level's packets, verbatim; caller holds panel_lock, panel enabled.
 * False when nothing went: a frame was still on its way out.
 */
static bool s9p_panel_brightness_table(struct s9p_decon *priv, unsigned int level, bool whole)
{
	static int shown;
	const u8 *e;
	unsigned int n, len;
	ktime_t t0;
	u32 v;

	if (level >= priv->bl_levels)
		level = priv->bl_levels - 1;
	e = priv->bl_table + (size_t)level * priv->bl_stride;
	/*
	 * Between two frames, with the frame trigger held off: the mask stops
	 * the next frame, and the DECON going idle shows the one already on its
	 * way has gone. Sent while a frame was going out, the packets waited
	 * behind it (10 ms) and a B0 offset reset was lost now and then: the
	 * panel's offset stays until changed, so the gamma after it landed in
	 * the wrong place and black came out green. The set itself takes well
	 * under a millisecond, so the hold stays inside the DSIM's 18 ms TE
	 * timeout; the long gamma only goes when the level has settled (see
	 * bl_update), the rest are 11 bytes or less.
	 */
	s9p_hw_trigger_mask(priv);
	s9p_wait_shadow_idle(priv);
	/* a frame still going out: send nothing. Packets sent across a frame
	 * (a slider dragged while the screen redraws) broke into the pixels,
	 * and the panel showed stripes until it slept. The caller tries again. */
	if (readl_poll_timeout(priv->decon + DECON_GLOBAL_CONTROL, v,
			       v & DECON_GLOBAL_IDLE_STATUS, 20, 17000)) {
		s9p_hw_trigger_unmask(priv);
		return false;
	}
	if (readl_poll_timeout_atomic(priv->dsim + DSIM_DPHY_STATUS, v,
				      (v & DSIM_DPHY_STOP_STATE_DAT) == DSIM_DPHY_STOP_STATE_DAT,
				      10, 5000)) {
		s9p_hw_trigger_unmask(priv);
		return false;
	}
	t0 = ktime_get();
	for (n = 0; n < priv->bl_stride && e[n]; n += len + 1) {
		len = e[n] & 0x7f;
		if (n + 1 + len > priv->bl_stride)
			break;
		/* while the level keeps changing (a slider dragged) only AOR goes,
		 * between the keys and latched by F7 03, as the old AOR path sent
		 * it: sent over and over, the ELVSS, VINT and ACL writes and their
		 * B0 offsets left the panel's colours wrong (black came out green).
		 * The whole set follows once the level has settled (bl_update). */
		if (!whole && e[n + 1] != 0xF0 && e[n + 1] != 0xF7 &&
		    !(e[n + 1] == 0xB1 && len == 3))
			continue;
		if (e[n] & 0x80)
			s9p_dsim_write_long(priv, e + n + 1, len);
		else
			s9p_dsim_write(priv, e + n + 1, len);
		readl_poll_timeout_atomic(priv->dsim + DSIM_DPHY_STATUS, v,
					  (v & DSIM_DPHY_STOP_STATE_DAT) == DSIM_DPHY_STOP_STATE_DAT,
					  10, 5000);
	}
	readl_poll_timeout_atomic(priv->dsim + DSIM_DPHY_STATUS, v,
				  (v & DSIM_DPHY_STOP_STATE_DAT) == DSIM_DPHY_STOP_STATE_DAT,
				  10, 5000);
	s9p_hw_trigger_unmask(priv);
	if (shown < 5) {
		shown++;
		dev_info(priv->drm.dev, "brightness level %u sent in %lld us\n", level,
			 ktime_us_delta(ktime_get(), t0));
	}
	return true;
}

/* brightness has stopped changing: now the whole set, gamma included */
static void s9p_bl_settle(struct work_struct *work)
{
	struct s9p_decon *priv = container_of(to_delayed_work(work),
					      struct s9p_decon, bl_settle);

	mutex_lock(&priv->panel_lock);
	if (priv->enabled && priv->bl && priv->bl_table) {
		if (s9p_panel_brightness_table(priv, priv->bl->props.brightness, true))
			priv->bl_sent = priv->bl->props.brightness;
		else	/* the DECON was busy: again shortly */
			mod_delayed_work(system_wq, &priv->bl_settle, msecs_to_jiffies(20));
	}
	mutex_unlock(&priv->panel_lock);
}

static ssize_t bl_table_write(struct file *file, struct kobject *kobj,
			      const struct bin_attribute *attr, char *buf,
			      loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct s9p_decon *priv = dev_get_drvdata(dev);
	size_t total, need = off + count;
	unsigned int levels, stride;
	u8 *grown;

	if (need > SZ_512K)
		return -E2BIG;
	mutex_lock(&priv->panel_lock);
	if (off == 0) {
		kvfree(priv->bl_upload);
		priv->bl_upload = NULL;
		priv->bl_upload_len = 0;
	}
	if (off != priv->bl_upload_len) {
		mutex_unlock(&priv->panel_lock);
		return -EINVAL;
	}
	grown = kvrealloc(priv->bl_upload, need, GFP_KERNEL);
	if (!grown) {
		mutex_unlock(&priv->panel_lock);
		return -ENOMEM;
	}
	memcpy(grown + off, buf, count);
	priv->bl_upload = grown;
	priv->bl_upload_len = need;

	if (need >= 8 && memcmp(grown, "S9BT", 4) == 0) {
		levels = grown[4] | grown[5] << 8;
		stride = grown[6] | grown[7] << 8;
		total = 8 + (size_t)levels * stride;
		if (levels && stride && need == total) {
			/* whole: it becomes the table */
			kvfree(priv->bl_table);
			priv->bl_table = kvmalloc(total - 8, GFP_KERNEL);
			if (priv->bl_table) {
				memcpy(priv->bl_table, grown + 8, total - 8);
				priv->bl_levels = levels;
				priv->bl_stride = stride;
				priv->bl_sent = -1;
				if (priv->bl) {
					priv->bl->props.max_brightness = levels - 1;
					if (priv->bl->props.brightness >= levels)
						priv->bl->props.brightness = levels - 1;
				}
				dev_info(dev, "brightness table: %u levels of %u bytes\n",
					 levels, stride);
			}
			kvfree(priv->bl_upload);
			priv->bl_upload = NULL;
			priv->bl_upload_len = 0;
		}
	}
	mutex_unlock(&priv->panel_lock);
	return count;
}
static const struct bin_attribute bin_attr_bl_table = {
	.attr = { .name = "bl_table", .mode = 0200 },
	.write = bl_table_write,
};

static struct attribute *s9p_panel_attrs[] = {
	&dev_attr_panel_read.attr,
	&dev_attr_panel_cmd.attr,
	NULL
};
static const struct bin_attribute *const s9p_panel_bin_attrs[] = {
	&bin_attr_bl_table,
	NULL
};
static const struct attribute_group s9p_panel_group = {
	.attrs = s9p_panel_attrs,
	.bin_attrs = s9p_panel_bin_attrs,
};

'''
s = s.replace(anchor, code + anchor, 1)

edits = [
    # the backlight sends the table's level when there is one. Not a level
    # already there (the shell sets it again as the screen comes on), and
    # not within 100 ms of display-on: a full set (ELVSS above all) sent
    # while the panel powers up stopped its TE, and every flip after it.
    ("""	if (priv->enabled && !backlight_is_blank(bl))
		s9p_panel_brightness(priv, bl->props.brightness);""",
     """	if (priv->enabled && !backlight_is_blank(bl)) {
		if (priv->bl_table) {
			if (priv->bl_sent != bl->props.brightness) {
				unsigned long ready = priv->bl_on_at + msecs_to_jiffies(100);
				/*
				 * The gamma (CA) is one 35-byte command, and this
				 * panel takes it only whole; sent while frames are
				 * being drawn it can jam the DSIM. So while the level
				 * keeps changing (a slider dragged) only the short
				 * packets go, and the whole set follows 200 ms after
				 * the last change.
				 */
				if (time_before(jiffies, ready))
					msleep(jiffies_to_msecs(ready - jiffies) + 1);
				s9p_panel_brightness_table(priv, bl->props.brightness, false);

				priv->bl_sent = -1;
				priv->bl_last = jiffies;
				mod_delayed_work(system_wq, &priv->bl_settle,
						 msecs_to_jiffies(400));
			}
		} else {
			s9p_panel_brightness(priv, bl->props.brightness);
		}
	}"""),
    # waking: the level goes in before display-on, as Samsung's init does
    # (sleep-out reloads the panel's registers from its own memory)
    ("""	if (priv->bl)
		s9p_panel_brightness(priv, priv->bl->props.brightness);
	s9p_panel_on(priv);""",
     """	if (priv->bl && priv->bl_table) {
		/* no frames before display-on: this one goes */
		if (s9p_panel_brightness_table(priv, priv->bl->props.brightness, true))
			priv->bl_sent = priv->bl->props.brightness;
	} else if (priv->bl) {
		s9p_panel_brightness(priv, priv->bl->props.brightness);
	}
	s9p_panel_on(priv);
	priv->bl_on_at = jiffies;"""),
    # asleep: nothing is on the panel any more (and no settle to come)
    ("""	mutex_lock(&priv->panel_lock);
	priv->enabled = false;	/* no brightness to a panel going to sleep */""",
     """	cancel_delayed_work_sync(&priv->bl_settle);
	mutex_lock(&priv->panel_lock);
	priv->enabled = false;	/* no brightness to a panel going to sleep */"""),
    ("""	priv->enabled = false;	/* no brightness to a panel going to sleep */""",
     """	priv->enabled = false;	/* no brightness to a panel going to sleep */
	priv->bl_sent = -1;"""),
    ("""	mutex_init(&priv->panel_lock);
""", """	mutex_init(&priv->panel_lock);
	priv->bl_sent = -1;
	INIT_DELAYED_WORK(&priv->bl_settle, s9p_bl_settle);
"""),
    # the files, once the device is up
    ("""	s9p_backlight_init(priv, &pdev->dev);""",
     """	s9p_backlight_init(priv, &pdev->dev);
	platform_set_drvdata(pdev, priv);	/* for the panel files */
	if (devm_device_add_group(&pdev->dev, &s9p_panel_group))
		dev_warn(&pdev->dev, "no panel_read/panel_cmd/bl_table files\\n");"""),
]
for a, b in edits:
    assert a in s, a[:60]
    s = s.replace(a, b, 1)

if "<linux/sizes.h>" not in s:
    s = s.replace("#include <linux/iopoll.h>", "#include <linux/iopoll.h>\n#include <linux/sizes.h>", 1)
path.write_text(s)
print("patch-decon-panel-io: applied")
