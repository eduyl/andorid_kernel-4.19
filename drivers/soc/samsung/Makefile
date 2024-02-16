# SPDX-License-Identifier: GPL-2.0

ccflags-y := -I$(srctree)/$(src)/include -I$(srctree)/arch/arm/plat-samsung/include
ccflags-y += -I$(srctree)/drivers/soc/samsung/include

obj-y	+= common.o
obj-$(CONFIG_EXYNOS_PMU)	+= exynos-pmu.o

obj-$(CONFIG_EXYNOS_PMU_ARM_DRIVERS)	+= exynos3250-pmu.o exynos4-pmu.o \
					exynos5250-pmu.o exynos5420-pmu.o
obj-$(CONFIG_EXYNOS_PM_DOMAINS) += pm_domains.o
obj-$(CONFIG_SOC_EXYNOS5433)	+= asv-exynos.o asv-exynos5433.o pm_domains-exynos5433.o asv-exynos5433_cal.o otp.o
obj-$(CONFIG_EXYNOS5433_BTS)	+= bts-exynos5433.o
ifeq ($(CONFIG_SOC_EXYNOS5433),y)
obj-$(CONFIG_SUSPEND)		+= pm-exynos5433.o
obj-$(CONFIG_CPU_IDLE)		+= cpuidle-exynos5433.o
obj-$(CONFIG_CPU_IDLE)		+= cpuidle_profiler.o
endif
obj-$(CONFIG_ARCH_EXYNOS)	+= firmware.o
obj-$(CONFIG_SOC_EXYNOS5433)	+= pmu-exynos5433.o
obj-$(CONFIG_CAL_SYS_PWRDOWN)	+= pmu_cal_sys.o
ifeq ($(CONFIG_SOC_EXYNOS5433),y)
obj-$(CONFIG_CAL_SYS_PWRDOWN)	+= pmu_cal_sys_exynos5433.o
endif
obj-$(CONFIG_EXYNOS_CORESIGHT)	+= exynos-coresight.o
obj-$(CONFIG_MACH_EXYNOS5_DT)		+= mach-exynos5-dt.o
obj-$(CONFIG_MACH_UNIVERSAL5433)	+= mach-universal5433.o
obj-$(CONFIG_VIDEO_EXYNOS_FIMC_IS)	+= setup-fimc-is.o setup-fimc-is-sensor.o
obj-$(CONFIG_VIDEO_EXYNOS_FIMG2D)	+= dev-fimg2d.o

obj-y					+= dev-uart.o

obj-$(CONFIG_ARCH_EXYNOS)		+= setup-i2c0.o
obj-$(CONFIG_EXYNOS5_SETUP_MIPIPHY)	+= setup-exynos5-mipiphy.o
obj-$(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION) += secmem.o

# HMP
obj-$(CONFIG_SCHED_HMP)			+= core_sysfs.o
