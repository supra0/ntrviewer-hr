// realcugan implemented with ncnn library

#include <stdio.h>
#include <algorithm>
#include <queue>
#include <vector>
#include <clocale>
#include <mutex>
#include <memory>
#include <functional>

#include "ncnn/cpu.h"
#include "ncnn/gpu.h"
#include "ncnn/platform.h"

#include "realcugan.h"
#include "lib.h"

#include "filesystem_utils.h"

#define REALCUGAN_WORK_COUNT (2)

int opt_testing_no_ext_mem, opt_testing_no_shared_sem, opt_testing_no_fp16;

static RealCUGAN* realcugan[SCREEN_COUNT * REALCUGAN_WORK_COUNT];
static int realcugan_indices[SCREEN_COUNT * FBI_COUNT];
static int realcugan_work_indices[SCREEN_COUNT];
static std::array<std::mutex, sizeof(realcugan) / sizeof(realcugan[0])> realcugan_locks;
static bool realcugan_support_ext_mem;

#ifdef _WIN32
// Non-owning
static ID3D11Device **d3d_device;
static ID3D11DeviceContext **d3d_context;
#endif

static int realcugan_size()
{
    return realcugan_support_ext_mem ? sizeof(realcugan) / sizeof(realcugan[0]) : 1;
}

static int realcugan_index(int screen_top_bot, int index, int next)
{
    index = screen_top_bot * FBI_COUNT + index;
    if (next) {
        ++realcugan_work_indices[screen_top_bot];
        realcugan_work_indices[screen_top_bot] %= REALCUGAN_WORK_COUNT;

        realcugan_indices[index] = screen_top_bot * REALCUGAN_WORK_COUNT + realcugan_work_indices[screen_top_bot];
    }
    return realcugan_support_ext_mem ? realcugan_indices[index] : 0;
}

#define print_uuid(uuid, name, ...) ({ \
  fprintf(stderr, name "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x\n", \
    ##__VA_ARGS__, \
    (int)*(uint32_t *)(char *)(uuid), (int)*(uint16_t *)&((char *)uuid)[4], (int)*(uint16_t *)&((char *)uuid)[6], (int)*(uint16_t *)&((char *)uuid)[8], \
    (int)*(uint8_t *)&(uuid)[10], (int)*(uint8_t *)&(uuid)[11], (int)*(uint8_t *)&(uuid)[12], (int)*(uint8_t *)&(uuid)[13], (int)*(uint8_t *)&(uuid)[14], (int)*(uint8_t *)&(uuid)[15] \
  ); \
})

#define print_luid(luid, name, ...) ({ \
  fprintf(stderr, name "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\n", \
    ##__VA_ARGS__, \
    (int)*(uint8_t *)&(luid)[0], (int)*(uint8_t *)&(luid)[1], (int)*(uint8_t *)&(luid)[2], (int)*(uint8_t *)&(luid)[3], (int)*(uint8_t *)&(luid)[4], (int)*(uint8_t *)&(luid)[5], (int)*(uint8_t *)&(luid)[6], (int)*(uint8_t *)&(luid)[7] \
  ); \
})

static void realcugan_close() {
    for (int k = 0; k < SCREEN_COUNT; ++k) {
        for (int j = 0; j < SCREEN_COUNT; ++j) {
            for (int i = 0; i < FBI_COUNT; ++i) {
                realcugan_next(k, j, i);
            }
        }
    }

    for (int j = 0; j < realcugan_size(); ++j) {
        if (realcugan[j]) {
            delete realcugan[j];
            realcugan[j] = nullptr;
        }
    }
}

static int noise = -1;
static std::vector<int> tilesize;
static path_t model = PATHSTR("models-se");
static int syncgap = 0;
static int tta_mode = 0;
static int scale = REALCUGAN_SCALE;
static path_t paramfullpath;
static path_t modelfullpath;
static int prepadding;

static int use_gpu_count;
static int use_gpu_i;
static ncnn::VulkanDevice *vkdev;
int supported_ext_context;

static int realcugan_open(
    std::function<int(VkPhysicalDeviceIDProperties &, int)> support_ext
) {
    supported_ext_context = 0;
    vkdev = 0;

    int default_i = -1;

    int i = 0;

    for (; i<use_gpu_count; i++)
    {
        if (tilesize[i] != 0)
            continue;

        ncnn::VulkanDevice *vkdev = ncnn::get_gpu_device(i);
        uint32_t heap_budget = vkdev->get_heap_budget();

        // more fine-grained tilesize policy here
        if (model.find(PATHSTR("models-nose")) != path_t::npos || model.find(PATHSTR("models-se")) != path_t::npos || model.find(PATHSTR("models-pro")) != path_t::npos)
        {
            if (scale == 2)
            {
                if (heap_budget > 1300)
                    tilesize[i] = 400;
                else if (heap_budget > 800)
                    tilesize[i] = 300;
                else if (heap_budget > 400)
                    tilesize[i] = 200;
                else if (heap_budget > 200)
                    tilesize[i] = 100;
                else
                    tilesize[i] = 32;
            }
            if (scale == 3)
            {
                if (heap_budget > 3300)
                    tilesize[i] = 400;
                else if (heap_budget > 1900)
                    tilesize[i] = 300;
                else if (heap_budget > 950)
                    tilesize[i] = 200;
                else if (heap_budget > 320)
                    tilesize[i] = 100;
                else
                    tilesize[i] = 32;
            }
            if (scale == 4)
            {
                if (heap_budget > 1690)
                    tilesize[i] = 400;
                else if (heap_budget > 980)
                    tilesize[i] = 300;
                else if (heap_budget > 530)
                    tilesize[i] = 200;
                else if (heap_budget > 240)
                    tilesize[i] = 100;
                else
                    tilesize[i] = 32;
            }
        }

        // We have images as large as 400x240; if exceeding tilesize, sync gap will be used which destroys performance
        if (tilesize[i] < 400) {
            continue;
        }

        VkPhysicalDeviceIDProperties dev_id_props {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
        };
        VkPhysicalDeviceProperties2 dev_props2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            &dev_id_props
        };
        ncnn::vkGetPhysicalDeviceProperties2KHR(vkdev->info.physical_device(), &dev_props2);
        if (support_ext(dev_id_props, i)) {
            supported_ext_context = 1;
            break;
        }

        if (default_i < 0) {
            default_i = i;
        }
    }

    if (i == use_gpu_count) {
        if (default_i < 0) {
            i = ncnn::get_default_gpu_index();

            if (i < 0) {
                fprintf(stderr, "no suitable gpu device\n");
                return -1;
            }
        } else {
            i = default_i;
        }
    }

    vkdev = ncnn::get_gpu_device(i);
    if (!vkdev) {
        fprintf(stderr, "no gpu vulkan device found\n");
        return -1;
    }
    if (tilesize[i] < 400) {
        fprintf(stderr, "insufficient vram\n");
        return -1;
    }

    use_gpu_i = i;
    return 0;
}


#ifdef _WIN32
static int realcugan_d3d11_open(ID3D11Device *device[SCREEN_COUNT], ID3D11DeviceContext *context[SCREEN_COUNT], IDXGIAdapter1 *adapter) {
    use_gpu_count = ncnn::get_gpu_count();

    tilesize.clear();
    tilesize.resize(use_gpu_count, 0);

    DXGI_ADAPTER_DESC1 adapter_desc;
    HRESULT hr;
    hr = adapter->GetDesc1(&adapter_desc);
    if (hr) {
      fprintf(stderr, "GetDesc1 failed: %d\n", (int)hr);
      return hr;
    }
    print_luid((const char *)&adapter_desc.AdapterLuid, "Adapter LUID: ");

    if (realcugan_open([&](VkPhysicalDeviceIDProperties &dev_id_props, int i) {
        print_luid(dev_id_props.deviceLUID, "Vk Device %d LUID: ", i);
        if (
            dev_id_props.deviceLUIDValid &&
            memcmp(&adapter_desc.AdapterLuid, dev_id_props.deviceLUID, sizeof(dev_id_props.deviceLUID)) == 0
        ) {
            fprintf(stderr, "matching gpu device found\n");
            return 1;
        }
        return 0;
    }))
        return -1;

    // Tested on AMD and NVIDIA for now
    bool supported_gpu_vendor = vkdev->info.vendor_id() == 0x1002 || vkdev->info.vendor_id() == 0x10de;
    supported_gpu_vendor = 1;
    realcugan_support_ext_mem = supported_ext_context && supported_gpu_vendor && ncnn::support_VK_KHR_external_memory_capabilities &&
        vkdev->info.support_VK_KHR_external_memory() && vkdev->info.support_VK_KHR_external_memory_win32();

    if (!(vkdev->info.support_fp16_packed() && vkdev->info.support_fp16_storage() && vkdev->info.support_int8_storage())) {
        realcugan_support_ext_mem = false;
        fprintf(stderr, "no float16 and int8 support, using slow path\n");
    }

    if (opt_testing_no_ext_mem) {
        if (realcugan_support_ext_mem) {
            fprintf(stderr, "D3D/Vk interop masked\n");
            realcugan_support_ext_mem = 0;
        }
    }

    if (realcugan_support_ext_mem) {
        fprintf(stderr, "using D3D/Vk interop\n");
        d3d_device = device;
        d3d_context = context;
    }

    for (int j = 0; j < realcugan_size(); ++j) {
        if (realcugan[j]) {
            delete realcugan[j];
        }

        realcugan[j] = new RealCUGAN(use_gpu_i, d3d_device, d3d_context, tta_mode);

        realcugan[j]->support_ext_mem = realcugan_support_ext_mem;
        if (realcugan[j]->load(paramfullpath, modelfullpath) != 0) {
            realcugan_destroy();
            return -1;
        }
        if (realcugan_support_ext_mem) {
            if (!realcugan[j]->support_ext_mem) {
                realcugan_support_ext_mem = false;
            }
        }

        realcugan[j]->noise = noise;
        realcugan[j]->scale = scale;
        realcugan[j]->tilesize = tilesize[use_gpu_i];
        realcugan[j]->prepadding = prepadding;
        realcugan[j]->syncgap = syncgap;
        realcugan[j]->tiling_linear = false;
    }

    return 0;
}
#endif
static int realcugan_ogl_open() {
    use_gpu_count = ncnn::get_gpu_count();

    tilesize.clear();
    tilesize.resize(use_gpu_count, 0);

    if (GLAD_GL_EXT_memory_object) {
        GLint gl_num_device_uuids = 0;
        glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &gl_num_device_uuids);
        GLubyte gl_device_uuids[gl_num_device_uuids][GL_UUID_SIZE_EXT];
        for (int i = 0; i < gl_num_device_uuids; ++i) {
            glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, i, gl_device_uuids[i]);
            print_uuid(gl_device_uuids[i], "GL Device %d UUID: ", i);
        }
        GLubyte gl_driver_uuid[GL_UUID_SIZE_EXT];
        glGetUnsignedBytevEXT(GL_DRIVER_UUID_EXT, gl_driver_uuid);
        print_uuid(gl_driver_uuid, "GL Driver UUID: ");

        if (realcugan_open([&](VkPhysicalDeviceIDProperties &dev_id_props, int i) {
            print_uuid(dev_id_props.deviceUUID, "Vk Device %d UUID: ", i);
            print_uuid(dev_id_props.driverUUID, "Vk Driver %d UUID: ", i);
            if (
                gl_num_device_uuids == 1 &&
                memcmp(gl_device_uuids[0], dev_id_props.deviceUUID, sizeof(dev_id_props.deviceUUID)) == 0 &&
                memcmp(gl_driver_uuid, dev_id_props.driverUUID, sizeof(dev_id_props.driverUUID)) == 0
            ) {
                fprintf(stderr, "matching gpu device found\n");
                return 1;
            }
            return 0;
        }))
            return -1;
    } else {
        if (realcugan_open([&](VkPhysicalDeviceIDProperties &, int) {
            return 0;
        }))
            return -1;
    }

    // Tested on AMD and NVIDIA for now
    bool supported_gpu_vendor = vkdev->info.vendor_id() == 0x1002 || vkdev->info.vendor_id() == 0x10de;
    supported_gpu_vendor = 1;
    realcugan_support_ext_mem = supported_ext_context && supported_gpu_vendor && ncnn::support_VK_KHR_external_memory_capabilities &&
        vkdev->info.support_VK_KHR_external_memory() && GLAD_GL_EXT_memory_object &&
#if _WIN32
        vkdev->info.support_VK_KHR_external_memory_win32() && GLAD_GL_EXT_memory_object_win32;
#else
        vkdev->info.support_VK_KHR_external_memory_fd() && GLAD_GL_EXT_memory_object_fd;
#endif

    if (!(vkdev->info.support_fp16_packed() && vkdev->info.support_fp16_storage() && vkdev->info.support_int8_storage())) {
        fprintf(stderr, "no float16 and int8 support, using slow path\n");
    }

    if (opt_testing_no_ext_mem) {
        if (realcugan_support_ext_mem) {
            fprintf(stderr, "OGL/Vk interop masked\n");
            realcugan_support_ext_mem = 0;
        }
    }

    if (realcugan_support_ext_mem) {
        fprintf(stderr, "using OGL/Vk interop\n");
    }

    for (int j = 0; j < realcugan_size(); ++j) {
        if (realcugan[j]) {
            delete realcugan[j];
        }

        realcugan[j] = new RealCUGAN(use_gpu_i, tta_mode);

        realcugan[j]->support_ext_mem = realcugan_support_ext_mem;
        if (realcugan[j]->load(paramfullpath, modelfullpath) != 0) {
            realcugan_destroy();
            return -1;
        }
        if (realcugan_support_ext_mem) {
            if (!realcugan[j]->support_ext_mem) {
                realcugan_support_ext_mem = false;
            }
        }

        realcugan[j]->noise = noise;
        realcugan[j]->scale = scale;
        realcugan[j]->tilesize = tilesize[use_gpu_i];
        realcugan[j]->prepadding = prepadding;
        realcugan[j]->syncgap = syncgap;
        realcugan[j]->tiling_linear = false;
    }

    return 0;
}

#ifdef _WIN32
extern "C"
int realcugan_d3d11_reset(ID3D11Device *device[SCREEN_COUNT], ID3D11DeviceContext *context[SCREEN_COUNT], IDXGIAdapter1 *adapter) {
    realcugan_close();
    return realcugan_d3d11_open(device, context, adapter);
}
#endif

static int realcugan_create() {
    if (noise < -1 || noise > 3)
    {
        fprintf(stderr, "invalid noise argument\n");
        return -1;
    }

    if (!(scale == 1 || scale == 2 || scale == 3 || scale == 4))
    {
        fprintf(stderr, "invalid scale argument\n");
        return -1;
    }

    if (!(syncgap == 0 || syncgap == 1 || syncgap == 2 || syncgap == 3))
    {
        fprintf(stderr, "invalid syncgap argument\n");
        return -1;
    }

    for (int i=0; i<(int)tilesize.size(); i++)
    {
        if (tilesize[i] != 0 && tilesize[i] < 32)
        {
            fprintf(stderr, "invalid tilesize argument\n");
            return -1;
        }
    }

    if (model.find(PATHSTR("models-se")) != path_t::npos
        || model.find(PATHSTR("models-nose")) != path_t::npos
        || model.find(PATHSTR("models-pro")) != path_t::npos)
    {
        if (scale == 2)
        {
            prepadding = 18;
        }
        if (scale == 3)
        {
            prepadding = 14;
        }
        if (scale == 4)
        {
            prepadding = 19;
        }
    }
    else
    {
        fprintf(stderr, "unknown model dir type\n");
        return -1;
    }

    if (model.find(PATHSTR("models-nose")) != path_t::npos)
    {
        // force syncgap off for nose models
        syncgap = 0;
    }

#if _WIN32
    wchar_t parampath[256];
    wchar_t modelpath[256];
    if (noise == -1)
    {
        swprintf(parampath, 256, L"%ls/up%dx-conservative.param", model.c_str(), scale);
        swprintf(modelpath, 256, L"%ls/up%dx-conservative.bin", model.c_str(), scale);
    }
    else if (noise == 0)
    {
        swprintf(parampath, 256, L"%ls/up%dx-no-denoise.param", model.c_str(), scale);
        swprintf(modelpath, 256, L"%ls/up%dx-no-denoise.bin", model.c_str(), scale);
    }
    else
    {
        swprintf(parampath, 256, L"%ls/up%dx-denoise%dx.param", model.c_str(), scale, noise);
        swprintf(modelpath, 256, L"%ls/up%dx-denoise%dx.bin", model.c_str(), scale, noise);
    }
#else
    char parampath[256];
    char modelpath[256];
    if (noise == -1)
    {
        sprintf(parampath, "%s/up%dx-conservative.param", model.c_str(), scale);
        sprintf(modelpath, "%s/up%dx-conservative.bin", model.c_str(), scale);
    }
    else if (noise == 0)
    {
        sprintf(parampath, "%s/up%dx-no-denoise.param", model.c_str(), scale);
        sprintf(modelpath, "%s/up%dx-no-denoise.bin", model.c_str(), scale);
    }
    else
    {
        sprintf(parampath, "%s/up%dx-denoise%dx.param", model.c_str(), scale, noise);
        sprintf(modelpath, "%s/up%dx-denoise%dx.bin", model.c_str(), scale, noise);
    }
#endif

    paramfullpath = sanitize_filepath(parampath);
    modelfullpath = sanitize_filepath(modelpath);

    ncnn::create_gpu_instance();

    return 0;
}

#ifdef _WIN32
extern "C"
int realcugan_d3d11_create(ID3D11Device *device[SCREEN_COUNT], ID3D11DeviceContext *context[SCREEN_COUNT], IDXGIAdapter1 *adapter) {
    if (realcugan_create())
        return -1;

    if (realcugan_d3d11_open(device, context, adapter) != 0) {
        ncnn::destroy_gpu_instance();
        return -1;
    }

    return 0;
}
#endif

extern "C"
int realcugan_ogl_create() {
    if (realcugan_create())
        return -1;

    if (realcugan_ogl_open() != 0) {
        ncnn::destroy_gpu_instance();
        return -1;
    }

    return 0;
}

extern "C"
int realcugan_run(int ctx_top_bot, int locks_index, int w, int h, int c, const unsigned char *indata, unsigned char *outdata)
{
    ncnn::Mat inimage = ncnn::Mat(w, h, (void*)indata, (size_t)c, c);
    ncnn::Mat outimage = ncnn::Mat(w * REALCUGAN_SCALE, h * REALCUGAN_SCALE, (void*)outdata, (size_t)c, c);

    if (locks_index >= (int)realcugan_locks.size() || ctx_top_bot >= (int)realcugan[locks_index]->out_gpu_tex.size()) {
        return -2;
    }

    realcugan_locks[locks_index].lock();
    if (
        realcugan[locks_index]->process(ctx_top_bot, inimage, outimage)
        != 0
    ) {
        realcugan_locks[locks_index].unlock();
        return -1;
    }
    realcugan_locks[locks_index].unlock();
    return 0;
}

#ifdef _WIN32
extern "C"
ID3D11Resource *realcugan_d3d11_run(int ctx_top_bot, int screen_top_bot, int index, int w, int h, int c, const unsigned char *indata, unsigned char *outdata, IDXGIKeyedMutex **mutex, ID3D11ShaderResourceView **srv, bool *dim3, bool *success)
{
    int locks_index = realcugan_index(screen_top_bot, index, 1);

    if (realcugan_run(ctx_top_bot, locks_index, w, h, c, indata, outdata)) {
        *success = 0;
        return 0;
    }

    OutVkImageMat *out = realcugan[locks_index]->out_gpu_tex[ctx_top_bot];
    *mutex = out->dxgi_mutex;
    *srv = out->d3d_srv;
    *dim3 = out->depth > 1;
    *success = true;
    return out->d3d_resource;
}
#endif

extern "C"
GLuint realcugan_ogl_run(int ctx_top_bot, int screen_top_bot, int index, int w, int h, int c, const unsigned char *indata, unsigned char *outdata, GLuint *gl_sem, GLuint *gl_sem_next, bool *dim3, bool *success)
{
    int locks_index = realcugan_index(screen_top_bot, index, 1);

    if (realcugan_run(ctx_top_bot, locks_index, w, h, c, indata, outdata)) {
        *success = 0;
        return 0;
    }

    OutVkImageMat *out = realcugan[locks_index]->out_gpu_tex[ctx_top_bot];
    GLuint tex = out->gl_texture;
    *gl_sem = out->gl_sem;
    *gl_sem_next = out->gl_sem_next;
    *dim3 = out->depth > 1;
    *success = true;
    return tex;
}

extern "C" void realcugan_next(int ctx_top_bot, int screen_top_bot, int index)
{
    int locks_index = realcugan_index(screen_top_bot, index, 0);
    if (!realcugan[locks_index] || ctx_top_bot >= (int)realcugan[locks_index]->out_gpu_tex.size()) {
        return;
    }
    OutVkImageMat *out = realcugan[locks_index]->out_gpu_tex[ctx_top_bot];
    if (!out || !out->first_subseq) {
        return;
    }

    if (out->need_wait) {
        realcugan_locks[locks_index].lock();
        if (out->need_wait) {
            VkResult ret = ncnn::vkWaitForFences(realcugan[locks_index]->vkdev->vkdevice(), 1, &out->fence, VK_TRUE, (uint64_t)-1);
            if (ret != VK_SUCCESS) {
                NCNN_LOGE("vkWaitForFences failed %d", ret);
            }
            out->need_wait = false;
        }
        realcugan_locks[locks_index].unlock();
    }
}

extern "C" void realcugan_destroy()
{
    realcugan_close();
    ncnn::destroy_gpu_instance();
#ifdef _WIN32
    d3d_context = NULL;
    d3d_device = NULL;
#endif
}
