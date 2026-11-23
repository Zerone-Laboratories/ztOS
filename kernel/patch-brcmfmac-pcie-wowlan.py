#!/usr/bin/env python3
"""Native OOB host-wake-doorbell WoWLAN for the BCM4361 (brcmfmac PCIe) on
Exynos9810 (Galaxy S9).

Proven (prior RE of fw 13.50.31.5): the BCM4361 firmware has NO 'wowl'/'tko'/
'tcpka' iovar, so the standard cfg80211 WOWLAN path is dead (iw ... wowlan
enable -> -95). It HAS the PCIe OOB doorbell instead: hostwake_oob,
HOSTWAKE_ASSRT_EVENT, pciedev_enable_device_wake, useoobgaint, apf, arp/nd
offload (arp_ol/arpoe/ndoe), mkeep_alive. nvram carries host_wake_opt /
#device_wake_opt. Wake is signalled out-of-band on WL_HOST_WAKE (gpa0-7, the
alive/PMU EINT bank), independent of the PCIe link, because the Exynos RC is
INTx-only with no PCIe-PME path.

This patch (all behind the brcmfmac module param `wol_doorbell`, default off):

  1. pcie.c: bind the OOB host-wake IRQ correctly. The enumerated PCIe endpoint
     (0000:01:00.0) has NO DT of_node, so gpiod_get(&pdev->dev,...) fails. The
     host-wake-gpios prop lives on the PCIe *controller* node (pcie@116a0000),
     which is the PCI host bridge's parent device. Resolve it via
     pci_find_host_bridge(pdev->bus)->dev.parent and gpiod_get() there.
     request_irq WITHOUT IRQF_NO_SUSPEND (that conflicts with enable_irq_wake);
     wake is armed with enable_irq_wake() which programs the Exynos PMU EINT.

  2. pcie.c brcmf_pcie_pm_enter_D3: never abort s2idle when the doorbell is
     armed. Try the normal D3_INFORM handshake; if the dongle still declines
     (the live -EIO/-5), DON'T return -EIO -- keep the link in D0/L1 (the chip
     stays powered by WL_REG_ON and keeps the BSS) and arm the OOB line. If the
     dongle DOES ACK D3, arm the OOB line from D3 (deep-sleep capable). Either
     way suspend proceeds and the EINT wakes the AP.

  3. cfg80211.c brcmf_cfg80211_suspend: the no-wowl path disassociates to save
     power. When the doorbell is armed, keep the BSS instead and arm the dongle
     with what the fw HAS: arp/nd offload + keepalive (absorb background
     chatter, hold the association) + pciedev_enable_device_wake/useoobgaint +
     a unicast-to-us pkt_filter. brcmf_cfg80211_resume unwinds it.

  4. device tree (exynos9810-starlte-zt.dts): host-wake-gpios = <&gpa0 7> on the
     pcie@116a0000 controller node + the wlan_host_wake EINT pinctrl entry.

PROVEN-FROM-SOURCE vs INFERRED is called out in comments. Steps 1/2/4 are
structural and source-grounded. Step 3's exact fw wake-trigger (which iovar the
BCM4361 honours to raise the doorbell on a packet) is INFERRED; every arming
iovar is best-effort (failure logged, suspend continues) so an A/B test can
narrow it from live dumps.

Apply to a CLEAN kernel tree (anchors match unpatched sources). Idempotent.
Takes the kernel tree root as argv[1]. Rebuild the brcmfmac module for
pcie.c/cfg80211.c/bus.h and the dtb for the DT change.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
pcie = root / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c'
bush = root / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/bus.h'
cfg = root / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/cfg80211.c'
dts = root / 'arch/arm64/boot/dts/exynos/exynos9810-starlte-zt.dts'

SENTINEL = 'brcmf_pcie_wol_doorbell_arm'  # new-version marker


def apply(path, edits, done_marker, pre=None):
    """Apply v2 edits. `pre(s)` runs first (used to revert an old v1 patch) and
    returns the transformed text. Idempotent: done_marker present -> no-op."""
    s = path.read_text()
    if done_marker in s:
        print('%s: already patched (v2)' % path.name)
        return
    if pre is not None:
        s = pre(s)
    for name, anchor, _ in edits:
        if s.count(anchor) != 1:
            raise SystemExit('brcmfmac-wowlan: %s anchor "%s" not unique (%d); inspect'
                             % (path.name, name, s.count(anchor)))
    for _, anchor, new in edits:
        s = s.replace(anchor, new, 1)
    path.write_text(s)
    print('%s: patched' % path.name)


# ---- revert an already-applied v1 brcmfmac-wowlan patch (pcie.c only) --------
# v1 touched pcie.c and the dts; it never touched cfg80211.c or bus.h. The dts
# blocks v1 wrote are byte-identical to v2's, so only pcie.c needs reverting:
# we undo each v1 insertion (replace the v1-inserted text with the original
# upstream anchor) to recover a clean pcie.c, then the normal v2 edits apply.
def revert_v1_pcie(s):
    if 'brcmf_pcie_hostwake_isr' not in s and 'brcmf_pcie_hostwake_attach' not in s:
        return s  # not a v1 tree; nothing to revert
    for name, new, anchor in V1_PCIE_BLOCKS:
        if s.count(new) != 1:
            raise SystemExit('brcmfmac-wowlan: v1-revert block "%s" not found uniquely '
                             '(%d); pcie.c is not a recognised v1 tree -- inspect'
                             % (name, s.count(new)))
        s = s.replace(new, anchor, 1)
    if 'hostwake' in s or 'wowl_supported = true' in s:
        raise SystemExit('brcmfmac-wowlan: v1 revert incomplete (residual markers); inspect')
    print('pcie.c: reverted v1')
    return s


# ================================================================= pcie.c
inc_anchor = '#include <linux/random.h>\n#include <linux/unaligned.h>\n'
inc_new = (inc_anchor +
           '#include <linux/gpio/consumer.h>\n'
           '#include <linux/pm_wakeup.h>\n')

struct_anchor = '\tbool irq_allocated;\n\tbool wowl_enabled;\n'
struct_new = ('\tbool irq_allocated;\n\tbool wowl_enabled;\n'
              '\t/* OOB host-wake (WL_HOST_WAKE): the Exynos RC is INTx-only and has no\n'
              '\t * PCIe-PME path, so wake-on-wireless is signalled out-of-band on a GPIO.\n'
              '\t */\n'
              '\tstruct gpio_desc *hostwake_gpio;\n'
              '\tint hostwake_irq;\n'
              '\tbool wol_active;\t\t/* OOB wake armed for this suspend */\n')

probe_anchor = ('/* Forward declaration for pci_match_id() call */\n'
                'static const struct pci_device_id brcmf_pcie_devid_table[];\n\n'
                'static int\n'
                'brcmf_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)')
helpers = '''/* Forward declaration for pci_match_id() call */
static const struct pci_device_id brcmf_pcie_devid_table[];

/* Native OOB host-wake-doorbell WoWLAN (BCM4361). Default off; A/B testable.
 * Non-static: cfg80211.c references it via extern to arm the dongle.
 */
bool brcmf_wol_doorbell;
module_param_named(wol_doorbell, brcmf_wol_doorbell, bool, 0644);
MODULE_PARM_DESC(wol_doorbell,
		 "Native OOB host-wake doorbell WoWLAN for BCM4361 (default off)");

/* OOB host-wake ISR: the dongle pulses WL_HOST_WAKE to pull the AP out of
 * system suspend. We only need to report the wakeup here; the actual wake
 * traffic is serviced once the PCIe link is resumed and brcmf_pcie_isr runs.
 */
static irqreturn_t brcmf_pcie_hostwake_isr(int irq, void *arg)
{
\tstruct brcmf_pciedev_info *devinfo = arg;

\tpm_wakeup_event(&devinfo->pdev->dev, 0);
\treturn IRQ_HANDLED;
}

/* The brcmfmac PCIe endpoint (e.g. 0000:01:00.0) has NO DT of_node on the
 * Exynos DWC root complex, so the host-wake GPIO cannot be looked up on
 * &pdev->dev (this is why the first attempt failed). host-wake-gpios lives on
 * the PCIe *controller* node (pcie@116a0000, "samsung,exynos9810-pcie"), which
 * is the PCI host bridge's parent device. Resolve that device here.
 */
static struct device *brcmf_pcie_ctrl_dev(struct pci_dev *pdev)
{
\tstruct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);

\tif (bridge && bridge->dev.parent && dev_of_node(bridge->dev.parent))
\t\treturn bridge->dev.parent;
\t/* Fallback: a future DT may describe the wifi function as a child of the
\t * RC (compatible "brcm,bcm4329-fmac") carrying host-wake-gpios, in which
\t * case the endpoint pci_dev gets an of_node of its own.
\t */
\tif (dev_of_node(&pdev->dev))
\t\treturn &pdev->dev;
\treturn NULL;
}

/* Grab the optional host-wake GPIO (on the controller node) and wire it to the
 * ISR. A missing GPIO is non-fatal: the driver runs without OOB wake.
 */
static void brcmf_pcie_hostwake_attach(struct brcmf_pciedev_info *devinfo)
{
\tstruct device *ctrl;
\tstruct gpio_desc *gpio;
\tint irq, ret;

\tdevinfo->hostwake_gpio = NULL;
\tdevinfo->hostwake_irq = -1;
\tdevinfo->wol_active = false;

\tctrl = brcmf_pcie_ctrl_dev(devinfo->pdev);
\tif (!ctrl) {
\t\tbrcmf_dbg(PCIE, "no DT node for host-wake; OOB wake disabled\\n");
\t\treturn;
\t}

\tgpio = gpiod_get(ctrl, "host-wake", GPIOD_IN);
\tif (IS_ERR(gpio)) {
\t\tbrcmf_dbg(PCIE, "no host-wake gpio (%ld); OOB wake disabled\\n",
\t\t\t  PTR_ERR(gpio));
\t\treturn;
\t}

\tirq = gpiod_to_irq(gpio);
\tif (irq <= 0) {
\t\tbrcmf_err(NULL, "host-wake gpiod_to_irq failed (%d)\\n", irq);
\t\tgpiod_put(gpio);
\t\treturn;
\t}

\t/* Rising edge on assert (short doorbell pulse). No IRQF_NO_SUSPEND: wake
\t * across suspend is armed with enable_irq_wake() in pm_enter_D3, which also
\t * programs the Exynos PMU EINT so the line survives SoC (MIF-down) sleep.
\t * NO_SUSPEND + enable_irq_wake on the same IRQ triggers a kernel WARN.
\t */
\tret = request_irq(irq, brcmf_pcie_hostwake_isr, IRQF_TRIGGER_RISING,
\t\t\t  "brcmf_hostwake", devinfo);
\tif (ret) {
\t\tbrcmf_err(NULL, "host-wake request_irq %d failed (%d)\\n", irq, ret);
\t\tgpiod_put(gpio);
\t\treturn;
\t}

\tdevinfo->hostwake_gpio = gpio;
\tdevinfo->hostwake_irq = irq;
\tdevice_init_wakeup(&devinfo->pdev->dev, true);
\tbrcmf_dbg(PCIE, "host-wake OOB IRQ %d registered (via %s)\\n",
\t\t  irq, dev_name(ctrl));
}

static void brcmf_pcie_hostwake_detach(struct brcmf_pciedev_info *devinfo)
{
\tif (devinfo->hostwake_irq > 0) {
\t\tfree_irq(devinfo->hostwake_irq, devinfo);
\t\tdevinfo->hostwake_irq = -1;
\t}
\tif (devinfo->hostwake_gpio) {
\t\tgpiod_put(devinfo->hostwake_gpio);
\t\tdevinfo->hostwake_gpio = NULL;
\t}
}

/* True when the OOB doorbell is wired and enabled by module param. */
static bool brcmf_pcie_wol_doorbell_arm(struct brcmf_pciedev_info *devinfo)
{
\treturn brcmf_wol_doorbell && devinfo->hostwake_irq > 0;
}

static int
brcmf_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)'''

wowl_anchor = ('\tbus->wowl_supported = pci_pme_capable(pdev, PCI_D3hot);\n'
               '\tbus->fwvid = drvdata[id->driver_data].vendor;\n'
               '\tdevinfo->fwseed = drvdata[id->driver_data].fw_seed;\n'
               '\tdev_set_drvdata(&pdev->dev, bus);\n')
wowl_new = ("\t/* Exynos RC is INTx-only; PME-in-D3hot isn't advertised. Wake is OOB via\n"
            "\t * WL_HOST_WAKE (gpa0-7), not PME. Let feature.c probe the fw 'wowl' iovar;\n"
            "\t * it self-gates (BCM4361 lacks it, so FEAT_WOWL stays clear and the\n"
            "\t * doorbell path below is used instead).\n"
            '\t */\n'
            '\tbus->wowl_supported = true;\n'
            '\tbus->fwvid = drvdata[id->driver_data].vendor;\n'
            '\tdevinfo->fwseed = drvdata[id->driver_data].fw_seed;\n'
            '\tdev_set_drvdata(&pdev->dev, bus);\n\n'
            '\t/* Optional out-of-band host-wake GPIO (non-fatal if absent). */\n'
            '\tbrcmf_pcie_hostwake_attach(devinfo);\n'
            '\tbus->wowl_doorbell = (devinfo->hostwake_irq > 0);\n')

fail_anchor = ('fail:\n'
               '\tbrcmf_err(NULL, "failed %x:%x\\n", pdev->vendor, pdev->device);\n'
               '\tbrcmf_pcie_release_resource(devinfo);\n')
fail_new = ('fail:\n'
            '\tbrcmf_err(NULL, "failed %x:%x\\n", pdev->vendor, pdev->device);\n'
            '\tbrcmf_pcie_hostwake_detach(devinfo);\n'
            '\tbrcmf_pcie_release_resource(devinfo);\n')

rem_anchor = ('\tkfree(bus);\n\n'
              '\tbrcmf_pcie_release_irq(devinfo);\n'
              '\tbrcmf_pcie_release_scratchbuffers(devinfo);\n')
rem_new = ('\tkfree(bus);\n\n'
           '\tbrcmf_pcie_hostwake_detach(devinfo);\n'
           '\tbrcmf_pcie_release_irq(devinfo);\n'
           '\tbrcmf_pcie_release_scratchbuffers(devinfo);\n')

# pm_enter_D3: never abort suspend when the doorbell is armed.
d3_anchor = ('\tdevinfo->mbdata_completed = false;\n'
             '\tbrcmf_pcie_send_mb_data(devinfo, BRCMF_H2D_HOST_D3_INFORM);\n\n'
             '\twait_event_timeout(devinfo->mbdata_resp_wait, devinfo->mbdata_completed,\n'
             '\t\t\t   BRCMF_PCIE_MBDATA_TIMEOUT);\n'
             '\tif (!devinfo->mbdata_completed) {\n'
             '\t\tbrcmf_err(bus, "Timeout on response for entering D3 substate\\n");\n'
             '\t\tbrcmf_bus_change_state(bus, BRCMF_BUS_UP);\n'
             '\t\treturn -EIO;\n'
             '\t}\n\n'
             '\tdevinfo->state = BRCMFMAC_PCIE_STATE_DOWN;\n\n'
             '\treturn 0;\n'
             '}')
d3_new = ('\tdevinfo->mbdata_completed = false;\n'
          '\tbrcmf_pcie_send_mb_data(devinfo, BRCMF_H2D_HOST_D3_INFORM);\n\n'
          '\twait_event_timeout(devinfo->mbdata_resp_wait, devinfo->mbdata_completed,\n'
          '\t\t\t   BRCMF_PCIE_MBDATA_TIMEOUT);\n'
          '\tif (!devinfo->mbdata_completed) {\n'
          '\t\t/* The BCM4361 fw declines the D3 handshake unless an OOB wake\n'
          '\t\t * method is armed (this is the live -EIO/-5). When the doorbell\n'
          '\t\t * is armed we must NOT abort system suspend: keep the link in\n'
          '\t\t * D0/L1 -- the chip stays powered by WL_REG_ON and holds the BSS\n'
          '\t\t * -- and rely on the OOB host-wake line to pull the AP out of\n'
          '\t\t * s2idle. The in-band ring is serviced after resume.\n'
          '\t\t */\n'
          '\t\tif (brcmf_pcie_wol_doorbell_arm(devinfo)) {\n'
          '\t\t\tbrcmf_err(bus, "D3 not ACKed; keeping link up for OOB host-wake doorbell\\n");\n'
          '\t\t\tbrcmf_bus_change_state(bus, BRCMF_BUS_UP);\n'
          '\t\t\tdevinfo->wol_active = true;\n'
          '\t\t\tenable_irq_wake(devinfo->hostwake_irq);\n'
          '\t\t\treturn 0;\n'
          '\t\t}\n'
          '\t\tbrcmf_err(bus, "Timeout on response for entering D3 substate\\n");\n'
          '\t\tbrcmf_bus_change_state(bus, BRCMF_BUS_UP);\n'
          '\t\treturn -EIO;\n'
          '\t}\n\n'
          '\tdevinfo->state = BRCMFMAC_PCIE_STATE_DOWN;\n\n'
          '\t/* D3 ACKed: arm the OOB host-wake line so the dongle can raise it\n'
          '\t * from D3 on a matching packet (deep-sleep / MIF-down capable path).\n'
          '\t */\n'
          '\tif (brcmf_pcie_wol_doorbell_arm(devinfo)) {\n'
          '\t\tdevinfo->wol_active = true;\n'
          '\t\tenable_irq_wake(devinfo->hostwake_irq);\n'
          '\t}\n\n'
          '\treturn 0;\n'
          '}')

# pm_leave_D3: disarm the OOB wake before the resume/cleanup paths.
leave_anchor = ('\tdevinfo = bus->bus_priv.pcie->devinfo;\n'
                '\tbrcmf_dbg(PCIE, "Enter, dev=%p, bus=%p\\n", dev, bus);\n\n'
                '\t/* Check if device is still up and running, if so we are ready */')
leave_new = ('\tdevinfo = bus->bus_priv.pcie->devinfo;\n'
             '\tbrcmf_dbg(PCIE, "Enter, dev=%p, bus=%p\\n", dev, bus);\n\n'
             '\t/* Disarm the OOB host-wake before either resume path; the cleanup\n'
             '\t * path frees the IRQ via brcmf_pcie_remove(), which must not run\n'
             '\t * with wake still enabled. Matches enable_irq_wake() in enter_D3.\n'
             '\t */\n'
             '\tif (devinfo->wol_active) {\n'
             '\t\tdisable_irq_wake(devinfo->hostwake_irq);\n'
             '\t\tdevinfo->wol_active = false;\n'
             '\t}\n\n'
             '\t/* Check if device is still up and running, if so we are ready */')

# ---- exact v1 insertions, for reverting an already-v1-patched pcie.c ---------
# Each triple is (name, v1_inserted_text, upstream_anchor_to_restore). The
# upstream anchors below are the same clean text v2 expects, so after the revert
# the normal v2 edits apply. These v1 strings are the verbatim output of the
# v1 patch (proven to match the flashed trees).
v1_struct_new = ('\tbool irq_allocated;\n\tbool wowl_enabled;\n'
                 '\t/* OOB host-wake (WL_HOST_WAKE): the Exynos RC is INTx-only and has no\n'
                 '\t * PCIe-PME path, so wake-on-wireless is signalled out-of-band on a GPIO.\n'
                 '\t */\n'
                 '\tstruct gpio_desc *hostwake_gpio;\n'
                 '\tint hostwake_irq;\n')
v1_helpers = '''/* Forward declaration for pci_match_id() call */
static const struct pci_device_id brcmf_pcie_devid_table[];

/* OOB host-wake ISR: the dongle pulses WL_HOST_WAKE to pull the AP out of
 * system suspend. We only need to abort/interrupt suspend here; the actual
 * wake traffic is serviced once the PCIe link is resumed and brcmf_pcie_isr
 * runs. Report a wakeup event and acknowledge the line.
 */
static irqreturn_t brcmf_pcie_hostwake_isr(int irq, void *arg)
{
\tstruct brcmf_pciedev_info *devinfo = arg;

\tpm_wakeup_event(&devinfo->pdev->dev, 0);
\treturn IRQ_HANDLED;
}

/* Grab the optional host-wake GPIO and wire it to the ISR. A missing GPIO is
 * not fatal: the driver simply runs without OOB wake (suspend-while-associated
 * then falls back to whatever PME path, if any, exists).
 */
static void brcmf_pcie_hostwake_attach(struct brcmf_pciedev_info *devinfo)
{
\tstruct device *dev = &devinfo->pdev->dev;
\tstruct gpio_desc *gpio;
\tint irq, ret;

\tdevinfo->hostwake_gpio = NULL;
\tdevinfo->hostwake_irq = -1;

\tgpio = gpiod_get(dev, "host-wake", GPIOD_IN);
\tif (IS_ERR(gpio)) {
\t\tbrcmf_dbg(PCIE, "no host-wake gpio (%ld); OOB wake disabled\\n",
\t\t\t  PTR_ERR(gpio));
\t\treturn;
\t}

\tirq = gpiod_to_irq(gpio);
\tif (irq <= 0) {
\t\tbrcmf_err(NULL, "host-wake gpiod_to_irq failed (%d)\\n", irq);
\t\tgpiod_put(gpio);
\t\treturn;
\t}

\tret = request_irq(irq, brcmf_pcie_hostwake_isr,
\t\t\t  IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND,
\t\t\t  "brcmf_hostwake", devinfo);
\tif (ret) {
\t\tbrcmf_err(NULL, "host-wake request_irq %d failed (%d)\\n",
\t\t\t  irq, ret);
\t\tgpiod_put(gpio);
\t\treturn;
\t}

\tdevinfo->hostwake_gpio = gpio;
\tdevinfo->hostwake_irq = irq;
\tdevice_init_wakeup(dev, true);
\tbrcmf_dbg(PCIE, "host-wake OOB IRQ %d registered\\n", irq);
}

static void brcmf_pcie_hostwake_detach(struct brcmf_pciedev_info *devinfo)
{
\tif (devinfo->hostwake_irq > 0) {
\t\tfree_irq(devinfo->hostwake_irq, devinfo);
\t\tdevinfo->hostwake_irq = -1;
\t}
\tif (devinfo->hostwake_gpio) {
\t\tgpiod_put(devinfo->hostwake_gpio);
\t\tdevinfo->hostwake_gpio = NULL;
\t}
}

static int
brcmf_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)'''
v1_wowl_new = ('\t/* Exynos RC is INTx-only; PME-in-D3hot isn\'t advertised. Wake is OOB via\n'
               '\t * WL_HOST_WAKE (gpa0-7), not PME. Let feature.c probe the fw \'wowl\' iovar;\n'
               '\t * it self-gates if the firmware lacks WOWL.\n'
               '\t */\n'
               '\tbus->wowl_supported = true;\n'
               '\tbus->fwvid = drvdata[id->driver_data].vendor;\n'
               '\tdevinfo->fwseed = drvdata[id->driver_data].fw_seed;\n'
               '\tdev_set_drvdata(&pdev->dev, bus);\n\n'
               '\t/* Optional out-of-band host-wake GPIO (non-fatal if absent). */\n'
               '\tbrcmf_pcie_hostwake_attach(devinfo);\n')
v1_en_anchor = ('\tdevinfo->state = BRCMFMAC_PCIE_STATE_DOWN;\n\n'
                '\treturn 0;\n}\n\n\n'
                'static int brcmf_pcie_pm_leave_D3(struct device *dev)')
v1_en_new = ('\tdevinfo->state = BRCMFMAC_PCIE_STATE_DOWN;\n\n'
             '\t/* Arm the OOB host-wake line so the dongle can pull the AP out of\n'
             '\t * system suspend while the link is in D3.\n'
             '\t */\n'
             '\tif (devinfo->wowl_enabled && devinfo->hostwake_irq > 0)\n'
             '\t\tenable_irq_wake(devinfo->hostwake_irq);\n\n'
             '\treturn 0;\n}\n\n\n'
             'static int brcmf_pcie_pm_leave_D3(struct device *dev)')
v1_dis_new = ('\tdevinfo = bus->bus_priv.pcie->devinfo;\n'
              '\tbrcmf_dbg(PCIE, "Enter, dev=%p, bus=%p\\n", dev, bus);\n\n'
              '\t/* Disarm the OOB host-wake before either resume path; the cleanup path\n'
              '\t * frees the IRQ via brcmf_pcie_remove(), which must not run with wake\n'
              '\t * still enabled. Matches the enable_irq_wake() in pm_enter_D3.\n'
              '\t */\n'
              '\tif (devinfo->wowl_enabled && devinfo->hostwake_irq > 0)\n'
              '\t\tdisable_irq_wake(devinfo->hostwake_irq);\n\n'
              '\t/* Check if device is still up and running, if so we are ready */')

V1_PCIE_BLOCKS = [
    # name, v1 inserted text, upstream anchor to restore
    ('includes', inc_new, inc_anchor),
    ('devinfo struct', v1_struct_new, struct_anchor),
    ('helpers', v1_helpers, probe_anchor),
    ('wowl_supported/attach', v1_wowl_new, wowl_anchor),
    ('probe fail detach', fail_new, fail_anchor),
    ('remove detach', rem_new, rem_anchor),
    ('enter_D3 enable_irq_wake', v1_en_new, v1_en_anchor),
    ('leave_D3 disable_irq_wake', v1_dis_new, leave_anchor),
]

apply(pcie, [
    ('includes', inc_anchor, inc_new),
    ('devinfo struct', struct_anchor, struct_new),
    ('helpers', probe_anchor, helpers),
    ('wowl_supported/attach', wowl_anchor, wowl_new),
    ('probe fail detach', fail_anchor, fail_new),
    ('remove detach', rem_anchor, rem_new),
    ('enter_D3 doorbell', d3_anchor, d3_new),
    ('leave_D3 disarm', leave_anchor, leave_new),
], SENTINEL, pre=revert_v1_pcie)

# ================================================================= bus.h
bus_anchor = '\tbool always_use_fws_queue;\n\tbool wowl_supported;\n'
bus_new = ('\tbool always_use_fws_queue;\n\tbool wowl_supported;\n'
           '\tbool wowl_doorbell;\t/* OOB host-wake line wired (BCM4361) */\n')
apply(bush, [('bus struct', bus_anchor, bus_new)], 'wowl_doorbell')

# ================================================================= cfg80211.c
# Arming/disarming helpers, inserted before brcmf_cfg80211_suspend.
sus_fn_anchor = ('static s32 brcmf_cfg80211_suspend(struct wiphy *wiphy,\n'
                 '\t\t\t\t  struct cfg80211_wowlan *wowl)')
helpers_cfg = '''extern bool brcmf_wol_doorbell;

/* WL_PKT_FILTER_TYPE_PATTERN_MATCH */
#define BRCMF_PKT_FILTER_PATTERN\t0

/* Install a pattern filter that matches frames whose destination MAC is our
 * own (offset 0, 6-byte match). Combined with pciedev_enable_device_wake this
 * narrows OOB wakes to unicast-to-us and skips broadcast/multicast noise.
 * Best-effort: the BCM4361 has wlc_pkt_filter_attach/apf, but the exact wake
 * coupling is INFERRED, so a failure here is logged and ignored.
 */
static void brcmf_doorbell_pktfilter(struct brcmf_if *ifp, u32 id, bool add)
{
\tstruct brcmf_pkt_filter_enable_le en = {0};
\tu8 buf[sizeof(struct brcmf_pkt_filter_le) + 2 * ETH_ALEN];
\tstruct brcmf_pkt_filter_le *pf = (struct brcmf_pkt_filter_le *)buf;
\tint ret;

\tif (!add) {
\t\tbrcmf_fil_iovar_int_set(ifp, "pkt_filter_delete", id);
\t\treturn;
\t}

\tmemset(buf, 0, sizeof(buf));
\tpf->id = cpu_to_le32(id);
\tpf->type = cpu_to_le32(BRCMF_PKT_FILTER_PATTERN);
\tpf->negate_match = cpu_to_le32(0);
\tpf->u.pattern.offset = cpu_to_le32(0);
\tpf->u.pattern.size_bytes = cpu_to_le32(ETH_ALEN);
\t/* mask (all ones) then pattern (our MAC) in mask_and_pattern. */
\tmemset(&pf->u.pattern.mask_and_pattern[0], 0xff, ETH_ALEN);
\tmemcpy(&pf->u.pattern.mask_and_pattern[ETH_ALEN], ifp->mac_addr, ETH_ALEN);

\tret = brcmf_fil_iovar_data_set(ifp, "pkt_filter_add", buf, sizeof(buf));
\tif (ret) {
\t\tbrcmf_dbg(TRACE, "pkt_filter_add failed (%d); OOB wakes unfiltered\\n", ret);
\t\treturn;
\t}
\ten.id = cpu_to_le32(id);
\ten.enable = cpu_to_le32(1);
\tbrcmf_fil_iovar_data_set(ifp, "pkt_filter_enable", &en, sizeof(en));
}

#define BRCMF_DOORBELL_PKTFILTER_ID\t101

/* Arm the dongle to raise the OOB host-wake doorbell on an incoming packet,
 * while keeping the association up. Mirrors what the BCM4361 fw actually has
 * (no 'wowl' iovar). Each iovar is best-effort.
 */
static void brcmf_doorbell_arm(struct brcmf_if *ifp)
{
\tbrcmf_dbg(TRACE, "Arming OOB host-wake doorbell (keep BSS)\\n");

\t/* Offload ARP/ND so background broadcast chatter is answered by the fw and
\t * never wakes the AP. PROVEN: fw has arp_ol/arpoe/ndoe. */
\tbrcmf_configure_arp_nd_offload(ifp, true);

\t/* Hold the link up with null-data keepalive (30 s). PROVEN: mkeep_alive. */
\tbrcmf_keepalive_start(ifp, 30);

\t/* Only wake on unicast addressed to us. */
\tbrcmf_doorbell_pktfilter(ifp, BRCMF_DOORBELL_PKTFILTER_ID, true);

\t/* Tell the dongle to use the OOB doorbell for host wake. PROVEN iovars
\t * exist; their exact effect here is INFERRED. */
\tbrcmf_fil_iovar_int_set(ifp, "useoobgaint", 1);
\tbrcmf_fil_iovar_int_set(ifp, "pciedev_enable_device_wake", 1);
}

static void brcmf_doorbell_disarm(struct brcmf_if *ifp)
{
\tbrcmf_dbg(TRACE, "Disarming OOB host-wake doorbell\\n");
\tbrcmf_fil_iovar_int_set(ifp, "pciedev_enable_device_wake", 0);
\tbrcmf_doorbell_pktfilter(ifp, BRCMF_DOORBELL_PKTFILTER_ID, false);
}

/* True when the no-wowl suspend path should arm the doorbell instead of
 * disassociating: module param on AND the OOB line is wired on this bus.
 */
static bool brcmf_doorbell_active(struct brcmf_cfg80211_info *cfg)
{
\treturn brcmf_wol_doorbell && cfg->pub->bus_if->wowl_doorbell;
}

static s32 brcmf_cfg80211_suspend(struct wiphy *wiphy,
\t\t\t\t  struct cfg80211_wowlan *wowl)'''

# suspend: branch the wowl==NULL path to keep the BSS and arm the doorbell.
sus_body_anchor = ('''\tif (wowl == NULL) {
\t\tbrcmf_bus_wowl_config(cfg->pub->bus_if, false);
\t\tlist_for_each_entry(vif, &cfg->vif_list, list) {
\t\t\tif (!test_bit(BRCMF_VIF_STATUS_READY, &vif->sme_state))
\t\t\t\tcontinue;
\t\t\t/* While going to suspend if associated with AP
\t\t\t * disassociate from AP to save power while system is
\t\t\t * in suspended state
\t\t\t */
\t\t\tbrcmf_link_down(vif, WLAN_REASON_UNSPECIFIED, true);
\t\t\t/* Make sure WPA_Supplicant receives all the event
\t\t\t * generated due to DISASSOC call to the fw to keep
\t\t\t * the state fw and WPA_Supplicant state consistent
\t\t\t */
\t\t\tbrcmf_delay(500);
\t\t}
\t\t/* Configure MPC */
\t\tbrcmf_set_mpc(ifp, 1);

\t} else {''')
sus_body_new = ('''\tif (wowl == NULL && brcmf_doorbell_active(cfg)) {
\t\t/* Native OOB host-wake doorbell: KEEP the association (do not
\t\t * disassociate) and arm the dongle to raise WL_HOST_WAKE on an
\t\t * incoming packet. The BCM4361 fw has no 'wowl' iovar, so this
\t\t * replaces the dead cfg80211 WOWLAN path.
\t\t */
\t\tbrcmf_bus_wowl_config(cfg->pub->bus_if, false);
\t\tbrcmf_doorbell_arm(ifp);

\t} else if (wowl == NULL) {
\t\tbrcmf_bus_wowl_config(cfg->pub->bus_if, false);
\t\tlist_for_each_entry(vif, &cfg->vif_list, list) {
\t\t\tif (!test_bit(BRCMF_VIF_STATUS_READY, &vif->sme_state))
\t\t\t\tcontinue;
\t\t\t/* While going to suspend if associated with AP
\t\t\t * disassociate from AP to save power while system is
\t\t\t * in suspended state
\t\t\t */
\t\t\tbrcmf_link_down(vif, WLAN_REASON_UNSPECIFIED, true);
\t\t\t/* Make sure WPA_Supplicant receives all the event
\t\t\t * generated due to DISASSOC call to the fw to keep
\t\t\t * the state fw and WPA_Supplicant state consistent
\t\t\t */
\t\t\tbrcmf_delay(500);
\t\t}
\t\t/* Configure MPC */
\t\tbrcmf_set_mpc(ifp, 1);

\t} else {''')

# resume: unwind the doorbell arming (runs before the wowl.active block).
res_anchor = ('''\tbrcmf_dbg(TRACE, "Enter\\n");

\tif (cfg->wowl.active) {''')
res_new = ('''\tbrcmf_dbg(TRACE, "Enter\\n");

\tif (brcmf_doorbell_active(cfg))
\t\tbrcmf_doorbell_disarm(ifp);

\tif (cfg->wowl.active) {''')

apply(cfg, [
    ('cfg helpers', sus_fn_anchor, helpers_cfg),
    ('suspend branch', sus_body_anchor, sus_body_new),
    ('resume unwind', res_anchor, res_new),
], 'brcmf_doorbell_arm')

# ================================================================= device tree
d = dts.read_text()
if 'wlan_host_wake' in d:
    print('%s: already patched' % dts.name)
else:
    node_anchor = ('\t\tperst-gpios = <&gpf2 2 GPIO_ACTIVE_LOW>;\n'
                   '\t\tenable-gpios = <&gpg0 5 GPIO_ACTIVE_HIGH>;\t/* WL_REG_ON, 100 ms before power-on */\n'
                   '\t\tpinctrl-names = "default";\n'
                   '\t\tpinctrl-0 = <&pcie0_clkreq &pcie0_perst>;\n')
    node_new = ('\t\tperst-gpios = <&gpf2 2 GPIO_ACTIVE_LOW>;\n'
                '\t\tenable-gpios = <&gpg0 5 GPIO_ACTIVE_HIGH>;\t/* WL_REG_ON, 100 ms before power-on */\n'
                '\t\t/* WL_HOST_WAKE: dongle pulses this to wake the AP out of suspend.\n'
                '\t\t * INTx RC has no PCIe-PME path, so wake is OOB on this alive GPIO. */\n'
                '\t\thost-wake-gpios = <&gpa0 7 GPIO_ACTIVE_HIGH>;\n'
                '\t\tpinctrl-names = "default";\n'
                '\t\tpinctrl-0 = <&pcie0_clkreq &pcie0_perst &wlan_host_wake>;\n')

    pin_anchor = ('\tkey_wink: key-wink-pins {\n'
                  '\t\tsamsung,pins = "gpa0-6";\n'
                  '\t\tsamsung,pin-function = <EXYNOS_PIN_FUNC_EINT>;\n'
                  '\t\tsamsung,pin-pud = <EXYNOS_PIN_PULL_UP>;\n'
                  '\t\tsamsung,pin-drv = <EXYNOS5420_PIN_DRV_LV1>;\n'
                  '\t};\n'
                  '};')
    pin_new = ('\tkey_wink: key-wink-pins {\n'
               '\t\tsamsung,pins = "gpa0-6";\n'
               '\t\tsamsung,pin-function = <EXYNOS_PIN_FUNC_EINT>;\n'
               '\t\tsamsung,pin-pud = <EXYNOS_PIN_PULL_UP>;\n'
               '\t\tsamsung,pin-drv = <EXYNOS5420_PIN_DRV_LV1>;\n'
               '\t};\n\n'
               '\t/* WL_HOST_WAKE (BCM4361 OOB host-wake), gpa0-7, EINT function.\n'
               '\t * Keep the input alive across SoC power-down (con-pdn = previous state)\n'
               '\t * so the dongle can assert it while the AP is suspended. */\n'
               '\twlan_host_wake: wlan-host-wake-pins {\n'
               '\t\tsamsung,pins = "gpa0-7";\n'
               '\t\tsamsung,pin-function = <EXYNOS_PIN_FUNC_EINT>;\n'
               '\t\tsamsung,pin-pud = <EXYNOS_PIN_PULL_DOWN>;\n'
               '\t\tsamsung,pin-con-pdn = <3>;\n'
               '\t\tsamsung,pin-pud-pdn = <1>;\n'
               '\t};\n'
               '};')

    for name, anchor in [('pcie_wlan node', node_anchor), ('alive pinctrl', pin_anchor)]:
        if d.count(anchor) != 1:
            raise SystemExit('brcmfmac-wowlan: dts anchor "%s" not unique (%d); inspect'
                             % (name, d.count(anchor)))
    d = d.replace(node_anchor, node_new, 1)
    d = d.replace(pin_anchor, pin_new, 1)
    dts.write_text(d)
    print('%s: patched' % dts.name)
