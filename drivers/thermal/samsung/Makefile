#
# Samsung thermal specific Makefile
#
ccflags-y += -I$(srctree)/include/soc/samsung -I$(srctree)/arch/arm/mach-exynos/include -I$(srctree)/arch/arm/plat-samsung/include
obj-$(CONFIG_EXYNOS_THERMAL)	+= exynos_thermal.o cal_tmu.o
exynos_thermal-y				:= exynos_tmu.o