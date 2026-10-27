# Samsung's SCORE vision DSP driver (Exynos 9810, SCORE v2.0; GPL, from
# drivers/vision/score of the vendor kernel) as one module for ztOS's kernel.
obj-m += zrn-score.o
zrn-score-y := score_device.o score_core.o score_ioctl.o score_frame.o score_context.o \
	score_util.o score_mmu.o score_system.o score_interface.o score_firmware.o \
	score_scq.o score_sm.o score_sr.o score_print.o score_dpmu.o score_profiler.o \
	score_sysfs.o score_memory.o score_clk.o score_pm.o zrn_score_board.o zrn_score_buf.o
ccflags-y += -I$(src) -include $(src)/zrn_score_compat.h -DCONFIG_EXYNOS_SCORE -DCONFIG_EXYNOS_SCORE_V2 -DSCORE_EVT1 \
	-Wno-missing-prototypes -Wno-missing-declarations -Wno-unused-function -Wno-unused-variable
