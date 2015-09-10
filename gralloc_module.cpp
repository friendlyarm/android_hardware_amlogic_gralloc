/*
 * Copyright (C) 2010 ARM Limited. All rights reserved.
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "Gralloc"

#include <errno.h>
#include <pthread.h>
#include <cutils/log.h>
#include <cutils/atomic.h>
#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <hardware/hwcomposer_defs.h>

#include <linux/ion.h>
#include <ion/ion.h>
#include <sys/mman.h>

#include "gralloc_priv.h"
#include "alloc_device.h"
#include "framebuffer_device.h"

static pthread_mutex_t s_map_lock = PTHREAD_MUTEX_INITIALIZER;

static int gralloc_device_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
    int status = -EINVAL;

    if (!strncmp(name, GRALLOC_HARDWARE_GPU0, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
    {
        status = alloc_device_open(module, name, device);
    }
    else if (!strncmp(name, GRALLOC_HARDWARE_FB0, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
    {
        status = framebuffer_device_open(module, name, device);
    }

    return status;
}

static int gralloc_register_buffer(gralloc_module_t const *module, buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
    {
        AERR("Registering invalid buffer 0x%p, returning error", handle);
        return -EINVAL;
    }

    // if this handle was created in this process, then we keep it as is.
    private_handle_t *hnd = (private_handle_t *)handle;

    int retval = -EINVAL;

    pthread_mutex_lock(&s_map_lock);

#if GRALLOC_ARM_UMP_MODULE

    if (!s_ump_is_open)
    {
        ump_result res = ump_open(); // MJOLL-4012: UMP implementation needs a ump_close() for each ump_open

        if (res != UMP_OK)
        {
            pthread_mutex_unlock(&s_map_lock);
            AERR("Failed to open UMP library with res=%d", res);
            return retval;
        }

        s_ump_is_open = 1;
    }

#endif

    hnd->pid = getpid();

    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
    {
        ALOGD("gralloc_register_buffer register framebuffer");
        hw_module_t * pmodule = NULL;
        private_module_t *m = NULL;
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **)&pmodule) == 0)
        {
            m = reinterpret_cast<private_module_t *>(pmodule);
        }
        else
        {
            AERR("Could not get gralloc module for handle: %p", hnd);
            retval = -errno;
            goto cleanup;
        }

        framebuffer_mapper_t* fbMaper = &(m->fb_primary);
        if(hnd->usage & GRALLOC_USAGE_EXTERNAL_DISP){
            ALOGD("register external display");
            fbMaper = &(m->fb_external);
        }
        if(!fbMaper->framebuffer) {
            fbMaper->framebuffer = new private_handle_t(hnd->flags, hnd->usage, hnd->size, hnd->base,0, dup(hnd->fd), 0);
            fbMaper->bufferSize = hnd->offset;
            fbMaper->numBuffers = fbMaper->framebuffer->size / fbMaper->bufferSize;
            fbMaper->bufferMask = 0;

            /*
             * map the framebuffer
             */
            void* vaddr = mmap(0, fbMaper->framebuffer->size, PROT_READ|PROT_WRITE, MAP_SHARED, fbMaper->framebuffer->fd, 0);
            if (vaddr == MAP_FAILED)
            {
                AERR( "Error mapping the framebuffer (%s)", strerror(errno) );
                return -errno;
            }
            memset(vaddr, 0, fbMaper->framebuffer->size);
            fbMaper->framebuffer->base = vaddr;

#if GRALLOC_ARM_UMP_MODULE
#ifdef IOCTL_GET_FB_UMP_SECURE_ID
            ioctl(fbMaper->framebuffer->fd, IOCTL_GET_FB_UMP_SECURE_ID, &fbMaper->framebuffer->ump_id);
#endif
            if ((int)UMP_INVALID_SECURE_ID != fbMaper->framebuffer->ump_id )
            {
                AINF("framebuffer accessed with UMP secure ID %i\n", fbMaper->framebuffer->ump_id);
            }
#endif
            ALOGD("register frame buffer count %d ",fbMaper->numBuffers );
        } else {
            ALOGE("ERROR::register frambuffer again!!!");
        }

    }else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP)
    {
#if GRALLOC_ARM_UMP_MODULE
        hnd->ump_mem_handle = (int)ump_handle_create_from_secure_id(hnd->ump_id);

        if (UMP_INVALID_MEMORY_HANDLE != (ump_handle)hnd->ump_mem_handle)
        {
            hnd->base = ump_mapped_pointer_get((ump_handle)hnd->ump_mem_handle);

            if (0 != hnd->base)
            {
                hnd->lockState = private_handle_t::LOCK_STATE_MAPPED;
                hnd->writeOwner = 0;
                hnd->lockState = 0;

                pthread_mutex_unlock(&s_map_lock);
                return 0;
            }
            else
            {
                AERR("Failed to map UMP handle 0x%x", hnd->ump_mem_handle);
            }

            ump_reference_release((ump_handle)hnd->ump_mem_handle);
        }
        else
        {
            AERR("Failed to create UMP handle 0x%x", hnd->ump_mem_handle);
        }

#else
        AERR("Gralloc does not support UMP. Unable to register UMP memory for handle 0x%p", hnd);
#endif
    }
    else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
    {
#if GRALLOC_ARM_DMA_BUF_MODULE
        int ret;
        unsigned char *mappedAddress;
        size_t size = hnd->size;
        hw_module_t *pmodule = NULL;
        private_module_t *m = NULL;

        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **)&pmodule) == 0)
        {
            m = reinterpret_cast<private_module_t *>(pmodule);
        }
        else
        {
            AERR("Could not get gralloc module for handle: 0x%p", hnd);
            retval = -errno;
            goto cleanup;
        }

        /* the test condition is set to m->ion_client <= 0 here, because:
         * 1) module structure are initialized to 0 if no initial value is applied
         * 2) a second user process should get a ion fd greater than 0.
         */
        if (m->ion_client <= 0)
        {
            /* a second user process must obtain a client handle first via ion_open before it can obtain the shared ion buffer*/
            m->ion_client = ion_open();

            if (m->ion_client < 0)
            {
                AERR("Could not open ion device for handle: 0x%p", hnd);
                retval = -errno;
                goto cleanup;
            }
        }

        mappedAddress = (unsigned char *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, hnd->share_fd, 0);

        if (MAP_FAILED == mappedAddress)
        {
            AERR("mmap( share_fd:%d ) failed with %s",  hnd->share_fd, strerror(errno));
            retval = -errno;
            goto cleanup;
        }

        hnd->base = mappedAddress + hnd->offset;
        pthread_mutex_unlock(&s_map_lock);
        return 0;
#endif
    }
    else
    {
        AERR("registering non-UMP buffer not supported. flags = %d", hnd->flags);
    }

cleanup:
    pthread_mutex_unlock(&s_map_lock);
    return retval;
}

static int gralloc_unregister_buffer(gralloc_module_t const *module, buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
    {
        AERR("unregistering invalid buffer 0x%p, returning error", handle);
        return -EINVAL;
    }

    private_handle_t *hnd = (private_handle_t *)handle;

    AERR_IF(hnd->lockState & private_handle_t::LOCK_STATE_READ_MASK, "[unregister] handle %p still locked (state=%08x)", hnd, hnd->lockState);

    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
    {
        pthread_mutex_lock(&s_map_lock);

        ALOGD("unregister framebuffer ");
        //AERR( "Can't unregister buffer 0x%x as it is a framebuffer", (unsigned int)handle );
        hw_module_t * pmodule = NULL;
        private_module_t *m = NULL;
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **)&pmodule) == 0)
        {
            m = reinterpret_cast<private_module_t *>(pmodule);
            framebuffer_mapper_t* fbMaper = &(m->fb_primary);
            if(hnd->usage & GRALLOC_USAGE_EXTERNAL_DISP){
                ALOGD("unregister external display");
                fbMaper = &(m->fb_external);
            }

            if(fbMaper->framebuffer) {
                munmap((void*)fbMaper->framebuffer->base,fbMaper->framebuffer->size);
                close(fbMaper->framebuffer->fd);
                //reset framebuffer info
                delete fbMaper->framebuffer;
                fbMaper->framebuffer = 0;
                fbMaper->bufferMask = 0;
                fbMaper->numBuffers = 0;
            } else {
                AERR("Can't unregister a not exist buffers: %p", hnd);
            }

        }
        else
        {
            AERR("Could not get gralloc module for handle: %p", hnd);
        }
    }
    else if (hnd->pid == getpid()) // never unmap buffers that were not registered in this process
    {
        pthread_mutex_lock(&s_map_lock);

        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP)
        {
#if GRALLOC_ARM_UMP_MODULE
            ump_mapped_pointer_release((ump_handle)hnd->ump_mem_handle);
            ump_reference_release((ump_handle)hnd->ump_mem_handle);
            hnd->ump_mem_handle = (int)UMP_INVALID_MEMORY_HANDLE;
#else
            AERR("Can't unregister UMP buffer for handle 0x%p. Not supported", handle);
#endif
        }
        else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
        {
#if GRALLOC_ARM_DMA_BUF_MODULE
            void *base = (void *)hnd->base;
            size_t size = hnd->size;

            if (munmap(base, size) < 0)
            {
                AERR("Could not munmap base:0x%p size:%d '%s'", base, size, strerror(errno));
            }

#else
            AERR("Can't unregister DMA_BUF buffer for hnd %p. Not supported", hnd);
#endif

        }
        else
        {
            AERR("Unregistering unknown buffer is not supported. Flags = %d", hnd->flags);
        }

        hnd->base = 0;
        hnd->lockState  = 0;
        hnd->writeOwner = 0;

        pthread_mutex_unlock(&s_map_lock);
    }
    else
    {
        AERR("Trying to unregister buffer 0x%p from process %d that was not created in current process: %d", hnd, hnd->pid, getpid());
    }

    return 0;
}

static int gralloc_lock(gralloc_module_t const *module, buffer_handle_t handle, int usage, int l, int t, int w, int h, void **vaddr)
{
    if (private_handle_t::validate(handle) < 0)
    {
        AERR("Locking invalid buffer 0x%p, returning error", handle);
        return -EINVAL;
    }

    private_handle_t *hnd = (private_handle_t *)handle;

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP || hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
    {
        hnd->writeOwner = usage & GRALLOC_USAGE_SW_WRITE_MASK;
    }

    if ((usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK))
            || (usage & GRALLOC_USAGE_HW_CAMERA_MASK)
            || (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER))
    {
        *vaddr = (void *)hnd->base;
    }

    return 0;
}

static int gralloc_lock_ycbcr(gralloc_module_t const* module,
                        buffer_handle_t handle, int usage,
                        int l, int t, int w, int h,
                        android_ycbcr *ycbcr)
{

    if (!ycbcr) {
        ALOGE("gralloc_lock_ycbcr got NULL ycbcr struct");
        return -EINVAL;
    }

    private_module_t *gr = (private_module_t *)module;
    private_handle_t *hnd = (private_handle_t *)handle;
    if (!gr || (private_handle_t::validate(hnd) < 0)) {
        ALOGE("gralloc_lock_ycbcr bad handle\n");
        return -EINVAL;
    }

    // Validate usage
    // For now, only allow camera write, software read.
    bool sw_read = (0 != (usage & GRALLOC_USAGE_SW_READ_MASK));
    bool hw_cam_write = (usage & GRALLOC_USAGE_HW_CAMERA_WRITE);
    bool sw_read_allowed = (0 != (hnd->usage & GRALLOC_USAGE_SW_READ_MASK));

    if ( (!hw_cam_write && !sw_read) ||
            (sw_read && !sw_read_allowed) ) {
        ALOGE("gralloc_lock_ycbcr usage mismatch usage:0x%x cb->usage:0x%x\n",
                usage, hnd->usage);
        return -EINVAL;
    }

    uint8_t *cpu_addr = NULL;
    cpu_addr = (uint8_t *)hnd->base;

    // Calculate offsets to underlying YUV data
    size_t yStride;
    size_t cStride;
    size_t yOffset;
    size_t uOffset;
    size_t vOffset;
    size_t cStep;
    switch (hnd->format) {
        case HAL_PIXEL_FORMAT_YCrCb_420_SP: //this is NV21
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            yStride = hnd->width;
            cStride = hnd->width;
            yOffset = 0;
            vOffset = yStride * hnd->height;
            uOffset = vOffset + 1;
            cStep = 2;
            break;
        default:
            ALOGE("gralloc_lock_ycbcr unexpected internal format %x",
                    hnd->format);
            return -EINVAL;
    }

    ycbcr->y = cpu_addr + yOffset;
    ycbcr->cb = cpu_addr + uOffset;
    ycbcr->cr = cpu_addr + vOffset;
    ycbcr->ystride = yStride;
    ycbcr->cstride = cStride;
    ycbcr->chroma_step = cStep;

    // Zero out reserved fields
    memset(ycbcr->reserved, 0, sizeof(ycbcr->reserved));

#if 0
    ALOGV("gralloc_lock_ycbcr success. usage: %x, ycbcr.y: %p, .cb: %p, .cr: %p, "
            ".ystride: %d , .cstride: %d, .chroma_step: %d", usage,
            ycbcr->y, ycbcr->cb, ycbcr->cr, ycbcr->ystride, ycbcr->cstride,
            ycbcr->chroma_step);
#endif

    return 0;
}

static int gralloc_unlock(gralloc_module_t const *module, buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
    {
        AERR("Unlocking invalid buffer 0x%p, returning error", handle);
        return -EINVAL;
    }

    private_handle_t *hnd = (private_handle_t *)handle;
    int32_t current_value;
    int32_t new_value;
    int retry;

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP && hnd->writeOwner)
    {
#if GRALLOC_ARM_UMP_MODULE
        ump_cpu_msync_now((ump_handle)hnd->ump_mem_handle, UMP_MSYNC_CLEAN_AND_INVALIDATE, (void *)hnd->base, hnd->size);
#else
        AERR("Buffer 0x%p is UMP type but it is not supported", hnd);
#endif
    }
    else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION && hnd->writeOwner)
    {
#if GRALLOC_ARM_DMA_BUF_MODULE
        hw_module_t *pmodule = NULL;
        private_module_t *m = NULL;

        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **)&pmodule) == 0)
        {
            m = reinterpret_cast<private_module_t *>(pmodule);
            ion_sync_fd(m->ion_client, hnd->share_fd);
        }
        else
        {
            AERR("Couldnot get gralloc module for handle 0x%p\n", handle);
        }

#endif
    }

    return 0;
}

// There is one global instance of the module

static struct hw_module_methods_t gralloc_module_methods =
{
open:
    gralloc_device_open
};

private_module_t::private_module_t()
{
#define INIT_ZERO(obj) (memset(&(obj),0,sizeof((obj))))

    base.common.tag = HARDWARE_MODULE_TAG;
    base.common.version_major = 1;
    base.common.version_minor = 0;
    base.common.id = GRALLOC_HARDWARE_MODULE_ID;
    base.common.name = "Graphics Memory Allocator Module";
    base.common.author = "ARM Ltd.";
    base.common.methods = &gralloc_module_methods;
    base.common.dso = NULL;
    INIT_ZERO(base.common.reserved);

    base.registerBuffer = gralloc_register_buffer;
    base.unregisterBuffer = gralloc_unregister_buffer;
    base.lock = gralloc_lock;
    base.lock_ycbcr = gralloc_lock_ycbcr;
    base.unlock = gralloc_unlock;
    base.perform = NULL;
    INIT_ZERO(base.reserved_proc);

    INIT_ZERO(fb_primary);
    INIT_ZERO(fb_external);

    pthread_mutex_init(&(lock), NULL);

#undef INIT_ZERO
};

/*
 * HAL_MODULE_INFO_SYM will be initialized using the default constructor
 * implemented above
 */
struct private_module_t HAL_MODULE_INFO_SYM;

