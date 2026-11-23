obj-m += zrn-system-sleep.o
zrn-system-sleep-y := platform.o sequence.o
obj-m += zrn-sleep-table-check.o
zrn-sleep-table-check-y := table-check.o sequence.o

# Separate temporary backend for iterating without replacing pinned ops.
obj-m += zrn-sleep-init.o
zrn-sleep-init-y := platform.o sequence.o

# Standalone Exynos9810 media/audio power-domain driver (loaded and driven by
# hand; not wired into the sleep path). Shares the sequence engine.
obj-m += zrn-pd.o
zrn-pd-y := pd.o sequence.o
