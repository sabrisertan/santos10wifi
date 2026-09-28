# SPDX-License-Identifier: GPL-2.0-only
# Santos exact-1.12 Services4 skeleton: Samsung 1.12.2701748 sources
# (vendored under src/), golden PVRDEFS, modern-7.2 Kbuild.
# ABI headers (include4, services4/include, bridged dispatch) are
# FROZEN — compile errors inside them stop the port, they are never
# "fixed" to satisfy the compiler.

santos-pvr112-objs := \
	src/services4/gburst/gburstm.o \
	src/services4/gburst/gburst_stats.o \
	src/services4/gburst/utilf.o \
	src/services4/srvkm/env/linux/osfunc.o \
	src/services4/srvkm/env/linux/mutils.o \
	src/services4/srvkm/env/linux/mmap.o \
	src/services4/srvkm/env/linux/module.o \
	src/services4/srvkm/env/linux/pdump.o \
	src/services4/srvkm/env/linux/pvr_bridge_k.o \
	src/services4/srvkm/env/linux/pvr_debug.o \
	src/services4/srvkm/env/linux/mm.o \
	src/services4/srvkm/env/linux/mutex.o \
	src/services4/srvkm/env/linux/event.o \
	src/services4/srvkm/env/linux/osperproc.o \
	src/services4/srvkm/env/linux/pvr_drm.o \
	santos-stub-sync.o \
	santos-sync-provider.o \
	santos-pvr-s5d.o \
	src/services4/srvkm/common/buffer_manager.o \
	src/services4/srvkm/common/devicemem.o \
	src/services4/srvkm/common/deviceclass.o \
	src/services4/srvkm/common/handle.o \
	src/services4/srvkm/common/hash.o \
	src/services4/srvkm/common/lists.o \
	src/services4/srvkm/common/mem.o \
	src/services4/srvkm/common/mem_debug.o \
	src/services4/srvkm/common/metrics.o \
	src/services4/srvkm/common/osfunc_common.o \
	src/services4/srvkm/common/pdump_common.o \
	src/services4/srvkm/common/perproc.o \
	src/services4/srvkm/common/power.o \
	src/services4/srvkm/common/pvrsrv.o \
	src/services4/srvkm/common/queue.o \
	src/services4/srvkm/common/ra.o \
	src/services4/srvkm/common/refcount.o \
	src/services4/srvkm/common/resman.o \
	src/services4/srvkm/bridged/bridged_support.o \
	src/services4/srvkm/bridged/bridged_pvr_bridge.o \
	src/services4/srvkm/bridged/sgx/bridged_sgx_bridge.o \
	src/services4/system/intel_drm/gburst_interface.o \
	src/services4/system/intel_drm/sysconfig.o \
	src/services4/system/intel_drm/sysutils.o \
	src/services4/system/intel_drm/sys_pvr_drm_export.o \
	src/services4/srvkm/devices/sgx/gburst_hw.o \
	src/services4/srvkm/devices/sgx/sgxinit.o \
	src/services4/srvkm/devices/sgx/sgxpower.o \
	src/services4/srvkm/devices/sgx/sgxreset.o \
	src/services4/srvkm/devices/sgx/sgxutils.o \
	src/services4/srvkm/devices/sgx/sgxkick.o \
	src/services4/srvkm/devices/sgx/sgxtransfer.o \
	src/services4/srvkm/devices/sgx/mmu.o \
	src/services4/srvkm/devices/sgx/pb.o \
	src/services4/3rdparty/dc_nohw/dc_nohw_displayclass.o \
	src/services4/3rdparty/dc_nohw/dc_nohw_linux.o

obj-$(CONFIG_DRM_PVRSGX_SANTOS_112) += santos-pvr112.o
obj-$(CONFIG_DRM_PVRSGX_SANTOS_112) += santos-pvr-shell.o
obj-$(CONFIG_DRM_PVRSGX_SANTOS_112) += santos-pvr-power.o
obj-$(CONFIG_DRM_PVRSGX_SANTOS_112) += santos-pmu-preinit.o
obj-$(CONFIG_DRM_PVRSGX_SANTOS_112) += santos-sgx-one.o
obj-$(CONFIG_DRM_PVRSGX_SANTOS_112) += santos-gfx-pwr.o
obj-$(CONFIG_DRM_PVRSGX_SANTOS_112) += santos-sgx-wa.o

# Golden PVRDEFS (bld/clovertrail/pvrdefs), include paths remapped to
# the vendored src/ layout. ANDROID + release, exact ABI.
ccflags-y += -DANDROID -D_linux_ -D__KERNEL__ -DSANTOS10_SGX_EARLY_WA
# ccflags-y += -DPVRDEBUG
ccflags-y += -DCLOVERTRAIL_PHONE
ccflags-y += -DSGX544 -DSGX_CORE_REV=115 -DSGX_FEATURE_MP=1 -DSGX_FEATURE_MP_CORE_COUNT=2 -DSGX_FEATURE_SYSTEM_CACHE=1 -DSUPPORT_MEMORY_TILING
ccflags-y += -DPVR_LINUX_TIMERS_USING_WORKQUEUES -DLINUX -DSERVICES4 -D_XOPEN_SOURCE=600
ccflags-y += -DPVR2D_VALIDATE_INPUT_PARAMS  -UDEBUG_LOG_PATH_TRUNCATE
ccflags-y += -DSUPPORT_LIBDRM_LITE -DSUPPORT_ANDROID_PLATFORM -DPROFILE_COMM -DPVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE
ccflags-y += -DSUPPORT_GRAPHICS_HAL -DPVR_SECURE_FD_EXPORT -DSUPPORT_SRVINIT -DSUPPORT_SGX -DSUPPORT_PERCONTEXT_PB -DSUPPORT_LINUX_X86_WRITECOMBINE
ccflags-y += -DPVRSRV_MODNAME='"pvrsrvkm"'
ccflags-y += -DTRANSFER_QUEUE -DSUPPORT_DRI_DRM -DSUPPORT_DRI_DRM_EXT -DSUPPORT_DRI_DRM_NO_DROPMASTER
ccflags-y += -DSYS_USING_INTERRUPTS -DSUPPORT_HW_RECOVERY -DSYS_SUPPORTS_SGX_IDLE_CALLBACK -DSUPPORT_ACTIVE_POWER_MANAGEMENT
ccflags-y += -DPVR_SECURE_HANDLES -DLDM_PCI -DUSE_PTHREADS -DSUPPORT_SGX_EVENT_OBJECT -DSUPPORT_SGX_HWPERF -DGBURST_HW_PVRSCOPESERVICE_SUPPORT
ccflags-y += -DSUPPORT_LINUX_X86_PAT -DPVR_PROC_USE_SEQ_FILE -DSUPPORT_CPU_CACHED_BUFFERS
ccflags-y += -DDRM_PVR_RESERVED_INTEL_ORDER -DDRM_PVR_USE_INTEL_FB -DSUPPORT_MEMINFO_IDS -DSUPPORT_SGX_NEW_STATUS_VALS -DSUPPORT_LARGE_GENERAL_HEAP
ccflags-y += -DSUPPORT_DC_CMDCOMPLETE_WHEN_NO_LONGER_DISPLAYED -DSUPPORT_CUSTOM_SWAP_OPERATIONS
ccflags-y += -DPVRSRV_NEED_PVR_DPF -DPVRSRV_NEW_PVR_DPF -DPVRSRV_NEED_PVR_ASSERT
ccflags-y += -DPVR_ANDROID_NATIVE_WINDOW_HAS_SYNC -DPVR_LINUX_MEM_AREA_POOL_ALLOW_SHRINK
ccflags-y += -DPVRSRV_DUMP_MK_TRACE=1 -DSUPPORT_PVRSRV_DEVICE_CLASS -DPVRSRV_USSE_EDM_STATUS_DEBUG=1
ccflags-y += -DSUPPORT_SGX_LOW_LATENCY_SCHEDULING
# A137: headless Display Class device (golden dc_nohw) for PVR2D offscreen.
# Standalone: registered/unregistered by SysInitialise/SysDeinitialise,
# no module_init, no DRM hook, no panel/MDS involvement.
ccflags-y += -DSANTOS10_SGX_DC_NOHW -DSANTOS10_DC_NOHW_STANDALONE
ccflags-y += -DDC_NOHW_DISCONTIG_BUFFERS -DDC_NOHW_GET_BUFFER_DIMENSIONS
ccflags-y += -I$(src)/src/services4/3rdparty/dc_nohw
ccflags-y += 

ccflags-y += -I$(src)/src/include4
ccflags-y += -I$(src)/src/services4/include
ccflags-y += -I$(src)/src/services4/include/env/linux
ccflags-y += -I$(src)/src/services4/srvkm/env/linux
ccflags-y += -I$(src)/src/services4/srvkm/include
ccflags-y += -I$(src)/src/services4/srvkm/bridged
ccflags-y += -I$(src)/src/services4/system/include
ccflags-y += -I$(src)/src/services4/system/intel_drm
ccflags-y += -I$(src)/src/services4/srvkm/hwdefs
ccflags-y += -I$(src)/src/services4/srvkm/bridged/sgx
ccflags-y += -I$(src)/src/services4/srvkm/devices/sgx
ccflags-y += -I$(src)/shim
ccflags-y += -I$(src)/shim
# Golden 3.4 Kbuild provided the toolchain freestanding headers via
# isystem; modern Kbuild does not. Restore exactly that (stddef.h,
# stdarg.h), no source edits.
ccflags-y += -isystem $(shell $(CC) -print-file-name=include)
