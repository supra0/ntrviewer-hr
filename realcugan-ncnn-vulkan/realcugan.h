// realcugan implemented with ncnn library

#ifndef REALCUGAN_H
#define REALCUGAN_H

#include <string>
#include <array>

// ncnn
#include "ncnn/net.h"
#include "ncnn/gpu.h"
#include "ncnn/layer.h"

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi1_2.h>
#endif
#include "glad/glad.h"

extern "C" {
#include "../const.h"
}

class RealCUGAN;

class OutVkImageMat : public ncnn::VkImageMat {
public:
    OutVkImageMat(int index) : ncnn::VkImageMat(), index(index) {}
    const int index = 0;

    void create_like(const RealCUGAN* cugan, const ncnn::VkMat& m, const ncnn::Option& opt);
    void create(const RealCUGAN* cugan, int _w, int _h, size_t _elemsize, int _elempack, const ncnn::Option& opt);
    void create(const RealCUGAN* cugan, int _w, int _h, int _c, size_t _elemsize, int _elempack, const ncnn::Option& opt);
    void release(const RealCUGAN* cugan);

    void create_handles(const RealCUGAN* cugan);
    void release_handles();

    size_t totalsize = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;

#ifdef _WIN32
    HANDLE memory = 0;
#else
    int memory = 0;
#endif

#ifdef _WIN32
    ID3D11Resource *d3d_resource = NULL;
    IDXGIKeyedMutex *dxgi_mutex = NULL;
    ID3D11ShaderResourceView *d3d_srv = NULL;
    IDXGIResource1 *dxgi_res = NULL;
    HANDLE d3d_handle = NULL;
#endif
    GLuint gl_memory = 0;
    GLuint gl_texture = 0;

    bool dedicated = 0;
    VkExternalMemoryHandleTypeFlagBits memory_type = (VkExternalMemoryHandleTypeFlagBits)0;

    VkSemaphore vk_sem = 0, vk_sem_next = 0;
#ifdef _WIN32
    HANDLE sem = 0, sem_next = 0;
#else
    int sem = 0, sem_next = 0;
#endif
    GLuint gl_sem = 0, gl_sem_next = 0;
    void create_sem(const RealCUGAN* cugan);
    void destroy_sem(const RealCUGAN* cugan);

    bool first_subseq = 0;
    bool need_wait = 0;
    ncnn::VkCompute *cmd = 0;
    VkFence fence = 0;

    ncnn::VkImageMemory* out_create(const RealCUGAN* cugan,
        int w, int h, int c, size_t elemsize, int elempack,
        size_t& totalsize, bool& dedicated, VkExternalMemoryHandleTypeFlagBits &compatible_memory_type
    );
};

class RealCUGAN
{
public:
#ifdef _WIN32
    RealCUGAN(int gpuid, ID3D11Device **dev, ID3D11DeviceContext **ctx, bool tta_mode = false, int num_threads = 1);

    ID3D11Device **const dev = NULL;
    ID3D11DeviceContext **const ctx = NULL;
#endif
    RealCUGAN(int gpuid, bool tta_mode = false, int num_threads = 1);

    bool d3d11 = false;
    void init(int gpuid, bool tta_mode, int num_threads) {
        vkdev = gpuid == -1 ? 0 : ncnn::get_gpu_device(gpuid);

        net.opt.num_threads = num_threads;

        realcugan_preproc = 0;
        realcugan_postproc = 0;
        realcugan_4x_postproc = 0;
        bicubic_2x = 0;
        bicubic_3x = 0;
        bicubic_4x = 0;
        this->tta_mode = tta_mode;

        if (vkdev) {
            blob_vkallocator = vkdev->acquire_blob_allocator();
            staging_vkallocator = vkdev->acquire_staging_allocator();
        }
    }

    ~RealCUGAN();

#if _WIN32
    int load(const std::wstring& parampath, const std::wstring& modelpath);
#else
    int load(const std::string& parampath, const std::string& modelpath);
#endif

    int process(int index, const ncnn::Mat& inimage, ncnn::Mat& outimage) const;

public:
    // realcugan parameters
    int noise = 0;
    int scale = 0;
    int tilesize = 0;
    int prepadding = 0;
    int syncgap = 0;

// private:
    ncnn::VulkanDevice* vkdev = 0;
    ncnn::Net net;
    ncnn::Pipeline* realcugan_preproc = 0;
    ncnn::Pipeline* realcugan_postproc = 0;
    ncnn::Pipeline* realcugan_4x_postproc = 0;
    ncnn::Layer* bicubic_2x = 0;
    ncnn::Layer* bicubic_3x = 0;
    ncnn::Layer* bicubic_4x = 0;
    bool tta_mode = 0;

    mutable std::array<OutVkImageMat*, SCREEN_COUNT> out_gpu_tex = {};
    bool support_ext_mem = 0;
    bool tiling_linear = 0;

    ncnn::VkAllocator* blob_vkallocator = 0;
    ncnn::VkAllocator* staging_vkallocator = 0;
};

#endif // REALCUGAN_H
