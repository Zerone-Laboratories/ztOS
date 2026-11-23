#!/usr/bin/env python3
"""Skip suspend/resume PM for the Exynos9810 VTS/CHUB pin controllers.

ztOS powers VTS (0x13880000) and CHUB (0x13a80000) OFF for MIF-down SYS_SLEEP
(the zrn-pd driver). Across that cycle those domains come back powered but
unclocked, so samsung_pinctrl's save/restore register access aborts with an
asynchronous SError (0xbe000011) in samsung_pinctrl_resume -> dpm_resume_early.
Stock leaves these banks to the sensorhub firmware and the AP pinctrl never
touches them; ztOS has no sensorhub consumer, so skip their PM entirely. Safe
because those banks hold no state we rely on across a sleep."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'drivers/pinctrl/samsung/pinctrl-samsung.c'
s = path.read_text()

if 'samsung_pinctrl_skip_pm' in s:
    print('pinctrl skip-vts-chub: already patched')
    sys.exit(0)

helper = '''/* ztOS: the VTS (0x13880000) and CHUB (0x13a80000) pin controllers sit in power
 * domains that zrn-pd turns OFF for MIF-down SYS_SLEEP. Across that cycle their
 * banks are powered-but-unclocked, so the save/restore register access below
 * aborts (async SError 0xbe000011 in samsung_pinctrl_resume). Stock leaves
 * these banks to the sensorhub firmware; ztOS has no sensorhub consumer. */
static bool __maybe_unused samsung_pinctrl_skip_pm(struct device *dev)
{
	const char *name = dev_name(dev);

	return name && (!strncmp(name, "13880000.", 9) ||
			!strncmp(name, "13a80000.", 9));
}

'''

sus_anchor = ('static int __maybe_unused samsung_pinctrl_suspend(struct device *dev)\n'
              '{\n'
              '\tstruct samsung_pinctrl_drv_data *drvdata = dev_get_drvdata(dev);\n'
              '\tstruct samsung_pin_bank *bank;\n'
              '\tint i;\n'
              '\n'
              '\ti = clk_enable(drvdata->pclk);')
sus_new = ('static int __maybe_unused samsung_pinctrl_suspend(struct device *dev)\n'
           '{\n'
           '\tstruct samsung_pinctrl_drv_data *drvdata = dev_get_drvdata(dev);\n'
           '\tstruct samsung_pin_bank *bank;\n'
           '\tint i;\n'
           '\n'
           '\tif (samsung_pinctrl_skip_pm(dev))\n'
           '\t\treturn 0;\n'
           '\n'
           '\ti = clk_enable(drvdata->pclk);')

res_anchor = ('static int __maybe_unused samsung_pinctrl_resume(struct device *dev)\n'
              '{\n'
              '\tstruct samsung_pinctrl_drv_data *drvdata = dev_get_drvdata(dev);\n'
              '\tstruct samsung_pin_bank *bank;\n'
              '\tint ret;\n'
              '\tint i;\n'
              '\n'
              '\t/*')
res_new = ('static int __maybe_unused samsung_pinctrl_resume(struct device *dev)\n'
           '{\n'
           '\tstruct samsung_pinctrl_drv_data *drvdata = dev_get_drvdata(dev);\n'
           '\tstruct samsung_pin_bank *bank;\n'
           '\tint ret;\n'
           '\tint i;\n'
           '\n'
           '\tif (samsung_pinctrl_skip_pm(dev))\n'
           '\t\treturn 0;\n'
           '\n'
           '\t/*')

if s.count(sus_anchor) != 1 or s.count(res_anchor) != 1:
    raise SystemExit('pinctrl suspend/resume anchors changed; inspect before patching')

s = s.replace(sus_anchor, helper + sus_new, 1)
s = s.replace(res_anchor, res_new, 1)
path.write_text(s)
print('pinctrl skip-vts-chub: patched')
