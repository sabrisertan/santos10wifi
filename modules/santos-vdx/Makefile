# SPDX-License-Identifier: GPL-2.0-only
obj-m += santos-vdx-gate1.o
obj-m += santos-vdx-shell.o
obj-m += santos-pvr-shell-vdx.o
obj-m += santos-vdx.o
santos-vdx-objs := santos-vdx-core.o santos-vdx-mmu.o santos-vdx-engine.o
ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations

PVR112 := $(srctree)/drivers/gpu/drm/pvrsgx/santos-112

ccflags-y += -DANDROID -D_linux_ -D__KERNEL__
ccflags-y += -DPVRDEBUG
ccflags-y += -DCLOVERTRAIL_PHONE
ccflags-y += -DSGX544 -DSGX_CORE_REV=115 -DSGX_FEATURE_MP=1 -DSGX_FEATURE_MP_CORE_COUNT=2 -DSGX_FEATURE_SYSTEM_CACHE=1 -DSUPPORT_MEMORY_TILING
ccflags-y += -DPVR_LINUX_TIMERS_USING_WORKQUEUES -DLINUX -DSERVICES4 -D_XOPEN_SOURCE=600
ccflags-y += -DPVR2D_VALIDATE_INPUT_PARAMS -UDEBUG_LOG_PATH_TRUNCATE
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

ccflags-y += -I$(PVR112)/src/include4
ccflags-y += -I$(PVR112)/src/services4/include
ccflags-y += -I$(PVR112)/src/services4/include/env/linux
ccflags-y += -I$(PVR112)/src/services4/srvkm/env/linux
ccflags-y += -I$(PVR112)/src/services4/srvkm/include
ccflags-y += -I$(PVR112)/src/services4/srvkm/bridged
ccflags-y += -I$(PVR112)/src/services4/system/include
ccflags-y += -I$(PVR112)/src/services4/system/intel_drm
ccflags-y += -I$(PVR112)/src/services4/srvkm/hwdefs
ccflags-y += -I$(PVR112)/src/services4/srvkm/bridged/sgx
ccflags-y += -I$(PVR112)/src/services4/srvkm/devices/sgx
ccflags-y += -I$(PVR112)/shim
ccflags-y += -isystem $(shell $(CC) -print-file-name=include)
