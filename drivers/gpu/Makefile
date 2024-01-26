# drm/tegra depends on host1x, so if both drivers are built-in care must be
# taken to initialize them in the correct order. Link order is the only way
# to ensure this currently.
obj-$(CONFIG_TEGRA_HOST1X)	+= host1x/
obj-y			+= drm/ vga/
obj-$(CONFIG_IMX_IPUV3_CORE)	+= ipu-v3/
obj-$(CONFIG_TRACE_GPU_MEM)		+= trace/

#
# T7XX configs
# (5433)
#
ifeq ($(CONFIG_MALI_T7XX),y)
	ifeq ($(CONFIG_MALI_R5P0),y)
		obj-y += t7xx/r5p0/
	endif
	ifeq ($(CONFIG_MALI_R7P0),y)
		obj-y += t7xx/r7p0/
	endif
	ifeq ($(CONFIG_MALI_R12P0),y)
		obj-y += t7xx/r12p0/
	endif
	ifeq ($(CONFIG_MALI_R15P0),y)
		obj-y += t7xx/r15p0/
	endif
	ifeq ($(CONFIG_MALI_R22P0),y)
		obj-y += t7xx/r22p0/
	endif
endif
