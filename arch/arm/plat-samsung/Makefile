# SPDX-License-Identifier: GPL-2.0
#
# Copyright 2009 Simtec Electronics

ccflags-$(CONFIG_ARCH_S3C64XX) := -I$(srctree)/arch/arm/mach-s3c64xx/include
ccflags-$(CONFIG_ARCH_MULTIPLATFORM) += -I$(srctree)/$(src)/include
ccflags-y += -I$(srctree)/arch/arm/mach-s3c64xx/include -I$(srctree)/arch/arm/mach-exynos/include 
# Objects we always build independent of SoC choice

obj-y				+= init.o cpu.o

# ADC

obj-$(CONFIG_S3C_ADC)	+= adc.o

# devices

obj-y	+= platformdata.o

obj-y	+= devs.o
obj-y	+= dev-uart.o

obj-$(CONFIG_GPIO_SAMSUNG)     += gpio-samsung.o

# PM support

obj-$(CONFIG_PM_SLEEP)		+= pm-common.o
obj-$(CONFIG_EXYNOS_CPU_SUSPEND) += pm-common.o
obj-$(CONFIG_SAMSUNG_PM)	+= pm.o pm-gpio.o s5p-pm.o
#obj-$(CONFIG_SAMSUNG_PM_GPIO)	+= pm-gpio.o
obj-$(CONFIG_SAMSUNG_PM_CHECK)	+= pm-check.o
obj-$(CONFIG_SAMSUNG_PM_DEBUG)	+= pm-debug.o
obj-$(CONFIG_S5P_SLEEP)		+= s5p-sleep.o
obj-$(CONFIG_SAMSUNG_WAKEMASK)	+= wakeup-mask.o
obj-$(CONFIG_SAMSUNG_WDT_RESET)	+= watchdog-reset.o
