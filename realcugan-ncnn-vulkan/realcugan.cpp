// realcugan implemented with ncnn library

#include "realcugan.h"

#include <algorithm>
#include <vector>
#include <map>

// ncnn
#include "ncnn/cpu.h"

#include "realcugan_preproc.comp.hex.h"
#include "realcugan_postproc.comp.hex.h"
#include "realcugan_4x_postproc.comp.hex.h"
#include "realcugan_preproc_tta.comp.hex.h"
#include "realcugan_postproc_tta.comp.hex.h"
#include "realcugan_4x_postproc_tta.comp.hex.h"

#include <stdio.h>

#ifndef _WIN32
#include <unistd.h>
#endif

typedef struct VkWin32KeyedMutexAcquireReleaseInfoKHR {
    VkStructureType sType;
    const void *pNext;
    uint32_t acquireCount;
    const VkDeviceMemory *pAcquireSyncs;
    const uint64_t *pAcquireKeys;
    const uint32_t *pAcquireTimeouts;
    uint32_t releaseCount;
    const VkDeviceMemory *pReleaseSyncs;
    const uint64_t *pReleaseKeys;
} VkWin32KeyedMutexAcquireReleaseInfoKHR;

static const VkStructureType VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR = (VkStructureType)1000075000;

#ifdef _WIN32
RealCUGAN::RealCUGAN(int gpuid, ID3D11Device **dev, ID3D11DeviceContext **ctx, bool _tta_mode, int num_threads) : dev(dev), ctx(ctx), d3d11(true) {
    init(gpuid, tta_mode, num_threads);
}
#endif
RealCUGAN::RealCUGAN(int gpuid, bool tta_mode, int num_threads)
{
    init(gpuid, tta_mode, num_threads);
}

RealCUGAN::~RealCUGAN()
{
    if (vkdev) {
        vkdev->reclaim_blob_allocator(blob_vkallocator);
        vkdev->reclaim_staging_allocator(staging_vkallocator);
    }

    // cleanup preprocess and postprocess pipeline
    if (realcugan_preproc)
        delete realcugan_preproc;

    if (realcugan_postproc)
        delete realcugan_postproc;

    if (realcugan_4x_postproc)
        delete realcugan_4x_postproc;

    if (bicubic_2x) {
        bicubic_2x->destroy_pipeline(net.opt);
        delete bicubic_2x;
    }

    if (bicubic_3x) {
        bicubic_3x->destroy_pipeline(net.opt);
        delete bicubic_3x;
    }

    if (bicubic_4x) {
        bicubic_4x->destroy_pipeline(net.opt);
        delete bicubic_4x;
    }

    for (int i = 0; i < out_gpu_tex.size(); ++i) {
        if (out_gpu_tex[i]) {
            delete out_gpu_tex[i]->cmd;
            out_gpu_tex[i]->release(this);
            out_gpu_tex[i]->destroy_sem(this);
            delete out_gpu_tex[i];
        }
    }
}

#if _WIN32
int RealCUGAN::load(const std::wstring& parampath, const std::wstring& modelpath)
#else
int RealCUGAN::load(const std::string& parampath, const std::string& modelpath)
#endif
{
    net.opt.use_vulkan_compute = vkdev ? true : false;
    if (!vkdev || (vkdev->info.support_fp16_packed() && vkdev->info.support_fp16_storage() && vkdev->info.support_int8_storage())) {
        net.opt.use_fp16_packed = true;
        net.opt.use_fp16_storage = vkdev ? true : false;
        net.opt.use_fp16_arithmetic = false;
        net.opt.use_int8_storage = true;
    } else {
        if (d3d11)
            support_ext_mem = false;
        net.opt.use_fp16_packed = false;
        net.opt.use_fp16_storage = false;
        net.opt.use_fp16_arithmetic = false;
        net.opt.use_int8_storage = false;
    }
    net.set_vulkan_device(vkdev);

#if _WIN32
    {
        FILE* fp = _wfopen(parampath.c_str(), L"rb");
        if (!fp)
        {
            fwprintf(stderr, L"_wfopen %ls failed\n", parampath.c_str());
        }

        net.load_param(fp);

        fclose(fp);
    }
    {
        FILE* fp = _wfopen(modelpath.c_str(), L"rb");
        if (!fp)
        {
            fwprintf(stderr, L"_wfopen %ls failed\n", modelpath.c_str());
        }

        net.load_model(fp);

        fclose(fp);
    }
#else
    net.load_param(parampath.c_str());
    net.load_model(modelpath.c_str());
#endif

    // initialize preprocess and postprocess pipeline
    if (vkdev)
    {
        std::vector<ncnn::vk_specialization_type> specializations(1);
#if _WIN32
        specializations[0].i = 1;
#else
        specializations[0].i = 0;
#endif

        {
            static std::vector<uint32_t> spirv;
            static ncnn::Mutex lock;
            {
                ncnn::MutexLockGuard guard(lock);
                if (spirv.empty())
                {
                    if (tta_mode)
                        compile_spirv_module(realcugan_preproc_tta_comp_data, sizeof(realcugan_preproc_tta_comp_data), net.opt, spirv);
                    else
                        compile_spirv_module(realcugan_preproc_comp_data, sizeof(realcugan_preproc_comp_data), net.opt, spirv);
                }
            }

            realcugan_preproc = new ncnn::Pipeline(vkdev);
            realcugan_preproc->set_optimal_local_size_xyz(8, 8, 3);
            realcugan_preproc->create(spirv.data(), spirv.size() * 4, specializations);
        }

        {
            static std::vector<uint32_t> spirv;
            static ncnn::Mutex lock;
            {
                ncnn::MutexLockGuard guard(lock);
                if (spirv.empty())
                {
                    if (tta_mode)
                        compile_spirv_module(realcugan_postproc_tta_comp_data, sizeof(realcugan_postproc_tta_comp_data), net.opt, spirv);
                    else
                        compile_spirv_module(realcugan_postproc_comp_data, sizeof(realcugan_postproc_comp_data), net.opt, spirv);
                }
            }

            realcugan_postproc = new ncnn::Pipeline(vkdev);
            realcugan_postproc->set_optimal_local_size_xyz(8, 8, 3);
            realcugan_postproc->create(spirv.data(), spirv.size() * 4, specializations);
        }

        {
            static std::vector<uint32_t> spirv;
            static ncnn::Mutex lock;
            {
                ncnn::MutexLockGuard guard(lock);
                if (spirv.empty())
                {
                    if (tta_mode)
                        compile_spirv_module(realcugan_4x_postproc_tta_comp_data, sizeof(realcugan_4x_postproc_tta_comp_data), net.opt, spirv);
                    else
                        compile_spirv_module(realcugan_4x_postproc_comp_data, sizeof(realcugan_4x_postproc_comp_data), net.opt, spirv);
                }
            }

            realcugan_4x_postproc = new ncnn::Pipeline(vkdev);
            realcugan_4x_postproc->set_optimal_local_size_xyz(8, 8, 3);
            realcugan_4x_postproc->create(spirv.data(), spirv.size() * 4, specializations);
        }
    }

    // bicubic 2x/3x/4x for alpha channel
    {
        bicubic_2x = ncnn::create_layer("Interp");
        bicubic_2x->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 3);// bicubic
        pd.set(1, 2.f);
        pd.set(2, 2.f);
        bicubic_2x->load_param(pd);

        bicubic_2x->create_pipeline(net.opt);
    }
    {
        bicubic_3x = ncnn::create_layer("Interp");
        bicubic_3x->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 3);// bicubic
        pd.set(1, 3.f);
        pd.set(2, 3.f);
        bicubic_3x->load_param(pd);

        bicubic_3x->create_pipeline(net.opt);
    }
    {
        bicubic_4x = ncnn::create_layer("Interp");
        bicubic_4x->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 3);// bicubic
        pd.set(1, 4.f);
        pd.set(2, 4.f);
        bicubic_4x->load_param(pd);

        bicubic_4x->create_pipeline(net.opt);
    }

    return 0;
}

int RealCUGAN::process(int index, const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    bool syncgap_needed = tilesize < std::max(inimage.w, inimage.h);

    if (!vkdev)
    {
#if 1
        fprintf(stderr, "not a gpu vulkan device\n");
        return -1;
#else
        // cpu only
        if (syncgap_needed && syncgap)
        {
            if (syncgap == 1)
                return process_cpu_se(inimage, outimage);
            if (syncgap == 2)
                return process_cpu_se_rough(inimage, outimage);
            if (syncgap == 3)
                return process_cpu_se_very_rough(inimage, outimage);
        }
        else
            return process_cpu(inimage, outimage);
#endif
    }

#if 1
    if (noise == -1 && scale == 1)
    {
        outimage = inimage;
        return 0;
    }
#endif

    if (syncgap_needed && syncgap)
    {
#if 1
        fprintf(stderr, "insufficient vram, syncgap not supported\n");
        return -1;
#else
        if (syncgap == 1)
            return process_se(inimage, outimage);
        if (syncgap == 2)
            return process_se_rough(inimage, outimage);
        if (syncgap == 3)
            return process_se_very_rough(inimage, outimage);
#endif
    }

    if (index + 1 > out_gpu_tex.size()) {
        out_gpu_tex.resize(index + 1);
    }
    if (!out_gpu_tex[index]) {
        out_gpu_tex[index] = new OutVkImageMat(index);
        if (support_ext_mem) {
            out_gpu_tex[index]->create_sem(this);
        }
        out_gpu_tex[index]->cmd = new ncnn::VkCompute(vkdev);
    }

    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    ncnn::Option opt = net.opt;
    opt.blob_vkallocator = blob_vkallocator;
    opt.workspace_vkallocator = blob_vkallocator;
    opt.staging_vkallocator = staging_vkallocator;

    // each tile 400x400
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    const size_t in_out_tile_elemsize = opt.use_fp16_storage ? 2u : 4u;

    //#pragma omp parallel for num_threads(2)
    for (int yi = 0; yi < ytiles; yi++)
    {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int prepadding_bottom = prepadding;
        if (scale == 1 || scale == 3)
        {
            prepadding_bottom += (tile_h_nopad + 3) / 4 * 4 - tile_h_nopad;
        }
        if (scale == 2 || scale == 4)
        {
            prepadding_bottom += (tile_h_nopad + 1) / 2 * 2 - tile_h_nopad;
        }

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding_bottom, h);

        ncnn::Mat in;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            in = ncnn::Mat(w, (in_tile_y1 - in_tile_y0), (unsigned char*)pixeldata + in_tile_y0 * w * channels, (size_t)channels, 1);
        }
        else
        {
            if (channels == 3)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGR2RGB, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGB, w, (in_tile_y1 - in_tile_y0));
#endif
            }
            if (channels == 4)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGRA2RGBA, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGBA, w, (in_tile_y1 - in_tile_y0));
#endif
            }
        }

        ncnn::VkCompute& cmd = *out_gpu_tex[index]->cmd;
        if (out_gpu_tex[index]->first_subseq) {
            if (out_gpu_tex[index]->need_wait) {
                VkResult ret = ncnn::vkWaitForFences(vkdev->vkdevice(), 1, &out_gpu_tex[index]->fence, VK_TRUE, (uint64_t)-1);
                if (ret != VK_SUCCESS)
                {
                    NCNN_LOGE("vkWaitForFences failed %d", ret);
                }
                out_gpu_tex[index]->need_wait = false;
            }

            cmd.reset();
        }

        // upload
        ncnn::VkMat in_gpu;
        {
            cmd.record_clone(in, in_gpu, opt);

            if (xtiles > 1)
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        int out_tile_y0 = std::max(yi * TILE_SIZE_Y, 0);
        int out_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h);

        ncnn::VkMat out_gpu;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, (size_t)channels, 1, opt.blob_vkallocator);
        }
        else
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, channels, (size_t)4u, 1, opt.blob_vkallocator);
        }

        for (int xi = 0; xi < xtiles; xi++)
        {
            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            int prepadding_right = prepadding;
            if (scale == 1 || scale == 3)
            {
                prepadding_right += (tile_w_nopad + 3) / 4 * 4 - tile_w_nopad;
            }
            if (scale == 2 || scale == 4)
            {
                prepadding_right += (tile_w_nopad + 1) / 2 * 2 - tile_w_nopad;
            }

            if (tta_mode)
            {
                // preproc
                ncnn::VkMat in_tile_gpu[8];
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu[0].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[1].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[2].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[3].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[4].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[5].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[6].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[7].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu[0];
                    bindings[2] = in_tile_gpu[1];
                    bindings[3] = in_tile_gpu[2];
                    bindings[4] = in_tile_gpu[3];
                    bindings[5] = in_tile_gpu[4];
                    bindings[6] = in_tile_gpu[5];
                    bindings[7] = in_tile_gpu[6];
                    bindings[8] = in_tile_gpu[7];
                    bindings[9] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu[0].w;
                    constants[4].i = in_tile_gpu[0].h;
                    constants[5].i = in_tile_gpu[0].cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu[0].w;
                    dispatcher.h = in_tile_gpu[0].h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu[8];
                for (int ti = 0; ti < 8; ti++)
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(blob_vkallocator);
                    ex.set_workspace_vkallocator(blob_vkallocator);
                    ex.set_staging_vkallocator(staging_vkallocator);

                    ex.input("in0", in_tile_gpu[ti]);

                    ex.extract("out0", out_tile_gpu[ti], cmd);
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                if (scale == 4)
                {
                    std::vector<ncnn::VkMat> bindings(11);
                    bindings[0] = in_gpu;
                    bindings[1] = out_tile_gpu[0];
                    bindings[2] = out_tile_gpu[1];
                    bindings[3] = out_tile_gpu[2];
                    bindings[4] = out_tile_gpu[3];
                    bindings[5] = out_tile_gpu[4];
                    bindings[6] = out_tile_gpu[5];
                    bindings[7] = out_tile_gpu[6];
                    bindings[8] = out_tile_gpu[7];
                    bindings[9] = out_alpha_tile_gpu;
                    bindings[10] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(16);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = out_tile_gpu[0].w;
                    constants[4].i = out_tile_gpu[0].h;
                    constants[5].i = out_tile_gpu[0].cstep;
                    constants[6].i = out_gpu.w;
                    constants[7].i = out_gpu.h;
                    constants[8].i = out_gpu.cstep;
                    constants[9].i = xi * TILE_SIZE_X;
                    constants[10].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[11].i = xi * TILE_SIZE_X * scale;
                    constants[12].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[13].i = channels;
                    constants[14].i = out_alpha_tile_gpu.w;
                    constants[15].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_4x_postproc, bindings, constants, dispatcher);
                }
                else
                {
                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = out_tile_gpu[0];
                    bindings[1] = out_tile_gpu[1];
                    bindings[2] = out_tile_gpu[2];
                    bindings[3] = out_tile_gpu[3];
                    bindings[4] = out_tile_gpu[4];
                    bindings[5] = out_tile_gpu[5];
                    bindings[6] = out_tile_gpu[6];
                    bindings[7] = out_tile_gpu[7];
                    bindings[8] = out_alpha_tile_gpu;
                    bindings[9] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(11);
                    constants[0].i = out_tile_gpu[0].w;
                    constants[1].i = out_tile_gpu[0].h;
                    constants[2].i = out_tile_gpu[0].cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = channels;
                    constants[9].i = out_alpha_tile_gpu.w;
                    constants[10].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_postproc, bindings, constants, dispatcher);
                }
            }
            else
            {
                // preproc
                ncnn::VkMat in_tile_gpu;
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding_right;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding_bottom;

                    in_tile_gpu.create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu;
                    bindings[2] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu.w;
                    constants[4].i = in_tile_gpu.h;
                    constants[5].i = in_tile_gpu.cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu.w;
                    dispatcher.h = in_tile_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_preproc, bindings, constants, dispatcher);
                }

                // realcugan
                ncnn::VkMat out_tile_gpu;
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(blob_vkallocator);
                    ex.set_workspace_vkallocator(blob_vkallocator);
                    ex.set_staging_vkallocator(staging_vkallocator);

                    ex.input("in0", in_tile_gpu);

                    ex.extract("out0", out_tile_gpu, cmd);
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                if (scale == 4)
                {
                    std::vector<ncnn::VkMat> bindings(4);
                    bindings[0] = in_gpu;
                    bindings[1] = out_tile_gpu;
                    bindings[2] = out_alpha_tile_gpu;
                    bindings[3] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(16);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = out_tile_gpu.w;
                    constants[4].i = out_tile_gpu.h;
                    constants[5].i = out_tile_gpu.cstep;
                    constants[6].i = out_gpu.w;
                    constants[7].i = out_gpu.h;
                    constants[8].i = out_gpu.cstep;
                    constants[9].i = xi * TILE_SIZE_X;
                    constants[10].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[11].i = xi * TILE_SIZE_X * scale;
                    constants[12].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[13].i = channels;
                    constants[14].i = out_alpha_tile_gpu.w;
                    constants[15].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_4x_postproc, bindings, constants, dispatcher);
                }
                else
                {
                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = out_tile_gpu;
                    bindings[1] = out_alpha_tile_gpu;
                    bindings[2] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(11);
                    constants[0].i = out_tile_gpu.w;
                    constants[1].i = out_tile_gpu.h;
                    constants[2].i = out_tile_gpu.cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = channels;
                    constants[9].i = out_alpha_tile_gpu.w;
                    constants[10].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realcugan_postproc, bindings, constants, dispatcher);
                }
            }

#if 1
            if (xtiles > 1)
#endif
            {
                cmd.submit_and_wait();
                cmd.reset();
            }
        }

        if (support_ext_mem) {
            out_gpu_tex[index]->create_like(this, out_gpu, opt);
            cmd.record_clone(out_gpu, *out_gpu_tex[index], opt);
            if (d3d11) {
#ifdef WIN32
                if (out_gpu_tex[index]->d3d_resource) {
                    const uint64_t acqKey = 0;
                    const uint64_t relKey = 1;
                    const uint32_t timeout = 2000;
                    VkDeviceMemory memory = out_gpu_tex[index]->data->memory;
                    VkWin32KeyedMutexAcquireReleaseInfoKHR keyedMutexInfo { VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR };
                    keyedMutexInfo.acquireCount = 1;
                    keyedMutexInfo.pAcquireSyncs = &memory;
                    keyedMutexInfo.pAcquireKeys = &acqKey;
                    keyedMutexInfo.pAcquireTimeouts = &timeout;
                    keyedMutexInfo.releaseCount = 1;
                    keyedMutexInfo.pReleaseSyncs = &memory;
                    keyedMutexInfo.pReleaseKeys = &relKey;

                    cmd.submit_and_wait(NULL, 0, NULL, &out_gpu_tex[index]->fence, &keyedMutexInfo);
                } else {
                    cmd.submit_and_wait(NULL, 0, NULL, &out_gpu_tex[index]->fence);
                }
#endif
            } else {
                cmd.submit_and_wait(out_gpu_tex[index]->first_subseq ? out_gpu_tex[index]->vk_sem_next : nullptr, VK_PIPELINE_STAGE_TRANSFER_BIT, out_gpu_tex[index]->vk_sem, &out_gpu_tex[index]->fence);
            }
                out_gpu_tex[index]->need_wait = out_gpu_tex[index]->first_subseq = true;
            }

        // download
#if 1
        else
        {
            ncnn::Mat out;

            if (opt.use_fp16_storage && opt.use_int8_storage)
            {
                out = ncnn::Mat(out_gpu.w, out_gpu.h, (unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, (size_t)channels, 1);
            }

            cmd.record_clone(out_gpu, out, opt);

            cmd.submit_and_wait();

            out_gpu_tex[index]->first_subseq = true;

            if (!(opt.use_fp16_storage && opt.use_int8_storage))
            {
                if (channels == 3)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB2BGR);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB);
#endif
                }
                if (channels == 4)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA2BGRA);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA);
#endif
                }
            }
        }
#endif
    }

    return 0;
}

using namespace ncnn;

typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceImageFormatProperties2)(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo, VkImageFormatProperties2 *pImageFormatProperties);

static PFN_vkGetPhysicalDeviceImageFormatProperties2 vkGetPhysicalDeviceImageFormatProperties2;

#ifdef _WIN32
typedef struct VkImportMemoryWin32HandleInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkExternalMemoryHandleTypeFlagBits handleType;
    HANDLE handle;
    LPCWSTR name;
} VkImportMemoryWin32HandleInfoKHR;

static const VkStructureType VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR = (VkStructureType)1000073000;

typedef struct VkMemoryGetWin32HandleInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkDeviceMemory memory;
    VkExternalMemoryHandleTypeFlagBits handleType;
} VkMemoryGetWin32HandleInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkGetMemoryWin32HandleKHR)(VkDevice device, const VkMemoryGetWin32HandleInfoKHR* pGetWin32HandleInfo, HANDLE* pHandle);

static PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR;

static const VkStructureType VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR = (VkStructureType)1000073003;
#else
typedef struct VkMemoryGetFdInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkDeviceMemory memory;
    VkExternalMemoryHandleTypeFlagBits handleType;
} VkMemoryGetFdInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkGetMemoryFdKHR)(VkDevice device, const VkMemoryGetFdInfoKHR* pGetWin32HandleInfo, int* pFd);

static PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;

static const VkStructureType VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR = (VkStructureType)1000074002;
#endif

typedef enum VkExternalSemaphoreHandleTypeFlagBits {
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT = 0x00000001,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT = 0x00000002,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT = 0x00000004,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT = 0x00000008,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT = 0x00000010,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_ZIRCON_EVENT_BIT_FUCHSIA = 0x00000080,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT_KHR = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT_KHR = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_FLAG_BITS_MAX_ENUM = 0x7FFFFFFF
} VkExternalSemaphoreHandleTypeFlagBits;
typedef VkFlags VkExternalSemaphoreHandleTypeFlags;

typedef enum VkExternalSemaphoreFeatureFlagBits {
    VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT = 0x00000001,
    VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT = 0x00000002,
    VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT_KHR = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT,
    VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT_KHR = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT,
    VK_EXTERNAL_SEMAPHORE_FEATURE_FLAG_BITS_MAX_ENUM = 0x7FFFFFFF
} VkExternalSemaphoreFeatureFlagBits;
typedef VkFlags VkExternalSemaphoreFeatureFlags;

typedef struct VkPhysicalDeviceExternalSemaphoreInfo {
    VkStructureType                          sType;
    const void*                              pNext;
    VkExternalSemaphoreHandleTypeFlagBits    handleType;
} VkPhysicalDeviceExternalSemaphoreInfo;

typedef struct VkExternalSemaphoreProperties {
    VkStructureType                       sType;
    void*                                 pNext;
    VkExternalSemaphoreHandleTypeFlags    exportFromImportedHandleTypes;
    VkExternalSemaphoreHandleTypeFlags    compatibleHandleTypes;
    VkExternalSemaphoreFeatureFlags       externalSemaphoreFeatures;
} VkExternalSemaphoreProperties;

typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo, VkExternalSemaphoreProperties* pExternalSemaphoreProperties);

static PFN_vkGetPhysicalDeviceExternalSemaphoreProperties vkGetPhysicalDeviceExternalSemaphorePropertiesKHR;

static const VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO = (VkStructureType)1000076000;
static const VkStructureType VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES = (VkStructureType)1000076001;

typedef struct VkExportSemaphoreCreateInfo {
    VkStructureType                       sType;
    const void*                           pNext;
    VkExternalSemaphoreHandleTypeFlags    handleTypes;
} VkExportSemaphoreCreateInfo;

static const VkStructureType VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO = (VkStructureType)1000077000;

#ifdef _WIN32
typedef struct VkSemaphoreGetWin32HandleInfoKHR {
    VkStructureType                          sType;
    const void*                              pNext;
    VkSemaphore                              semaphore;
    VkExternalSemaphoreHandleTypeFlagBits    handleType;
} VkSemaphoreGetWin32HandleInfoKHR;

static const VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR = (VkStructureType)1000078003;

typedef VkResult (VKAPI_PTR *PFN_vkGetSemaphoreWin32HandleKHR)(VkDevice device, const VkSemaphoreGetWin32HandleInfoKHR* pGetWin32HandleInfo, HANDLE* pHandle);
static PFN_vkGetSemaphoreWin32HandleKHR vkGetSemaphoreWin32HandleKHR;
#else
typedef struct VkSemaphoreGetFdInfoKHR {
    VkStructureType                          sType;
    const void*                              pNext;
    VkSemaphore                              semaphore;
    VkExternalSemaphoreHandleTypeFlagBits    handleType;
} VkSemaphoreGetFdInfoKHR;

static const VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR = (VkStructureType)1000079001;

typedef VkResult (VKAPI_PTR *PFN_vkGetSemaphoreFdKHR)(VkDevice device, const VkSemaphoreGetFdInfoKHR* pGetFdInfo, int* pFd);
static PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
#endif

static bool shared_sem_supported(const RealCUGAN* cugan)
{
    bool ret = cugan->vkdev->info.support_VK_KHR_external_semaphore() && GLAD_GL_EXT_semaphore &&
#ifdef _WIN32
        cugan->vkdev->info.support_VK_KHR_external_semaphore_win32() && GLAD_GL_EXT_semaphore_win32;
#else
        cugan->vkdev->info.support_VK_KHR_external_semaphore_fd() && GLAD_GL_EXT_semaphore_fd;
#endif
    if (!ret) {
        return false;
    }

    if (!vkGetPhysicalDeviceExternalSemaphorePropertiesKHR) {
        VkInstance instance = get_gpu_instance();
        vkGetPhysicalDeviceExternalSemaphorePropertiesKHR = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
    }

#ifdef _WIN32
    if (!vkGetSemaphoreWin32HandleKHR) {
        VkInstance instance = get_gpu_instance();
        vkGetSemaphoreWin32HandleKHR = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetInstanceProcAddr(instance, "vkGetSemaphoreWin32HandleKHR");
        if (!vkGetSemaphoreWin32HandleKHR) {
            return false;
        }
    }
#else
    if (!vkGetSemaphoreFdKHR) {
        VkInstance instance = get_gpu_instance();
        vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)vkGetInstanceProcAddr(instance, "vkGetSemaphoreFdKHR");
        if (!vkGetSemaphoreFdKHR) {
            return false;
        }
    }
#endif

    return true;
}

void OutVkImageMat::create_sem(const RealCUGAN* cugan)
{
    if (shared_sem_supported(cugan)) {
        VkExternalSemaphoreHandleTypeFlagBits compatible_semaphore_type;
        bool found = false;
        if (vkGetPhysicalDeviceExternalSemaphorePropertiesKHR) {
            VkExternalSemaphoreHandleTypeFlagBits flags[] = {
#ifdef _WIN32
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
#else
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
#endif
            };

            VkPhysicalDeviceExternalSemaphoreInfo extSemInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, nullptr};
            VkExternalSemaphoreProperties extSemProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
                                                nullptr};

            for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
            {
                extSemInfo.handleType = flags[i];
                vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(cugan->vkdev->info.physical_device(), &extSemInfo, &extSemProps);
                if (extSemProps.compatibleHandleTypes & flags[i] && extSemProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)
                {
                    compatible_semaphore_type = flags[i];
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
#ifdef _WIN32
            compatible_semaphore_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
            compatible_semaphore_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
        }

        VkExportSemaphoreCreateInfo exportSemaphoreCreateInfo{
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr,
            compatible_semaphore_type};
        VkSemaphoreCreateInfo semaphoreCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                                    &exportSemaphoreCreateInfo};
        VkResult ret;
        ret = vkCreateSemaphore(cugan->vkdev->vkdevice(), &semaphoreCreateInfo, nullptr, &vk_sem);
        if (ret != VK_SUCCESS) {
            vk_sem = 0;
            destroy_sem(cugan);
            return;
        }
        ret = vkCreateSemaphore(cugan->vkdev->vkdevice(), &semaphoreCreateInfo, nullptr, &vk_sem_next);
        if (ret != VK_SUCCESS) {
            vk_sem_next = 0;
            destroy_sem(cugan);
            return;
        }

#ifdef _WIN32
        VkSemaphoreGetWin32HandleInfoKHR semaphoreGetHandleInfo{
            VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr,
            VK_NULL_HANDLE, compatible_semaphore_type};
        semaphoreGetHandleInfo.semaphore = vk_sem;
        ret = vkGetSemaphoreWin32HandleKHR(cugan->vkdev->vkdevice(), &semaphoreGetHandleInfo, &sem);
#else
        VkSemaphoreGetFdInfoKHR semaphoreGetFdInfo{
            VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr,
            VK_NULL_HANDLE, compatible_semaphore_type};
        semaphoreGetFdInfo.semaphore = vk_sem;
        ret = vkGetSemaphoreFdKHR(cugan->vkdev->vkdevice(), &semaphoreGetFdInfo, &sem);
#endif
        if (ret != VK_SUCCESS) {
            sem = 0;
            destroy_sem(cugan);
            return;
        }
#ifdef _WIN32
        semaphoreGetHandleInfo.semaphore = vk_sem_next;
        ret = vkGetSemaphoreWin32HandleKHR(cugan->vkdev->vkdevice(), &semaphoreGetHandleInfo, &sem_next);
#else
        semaphoreGetFdInfo.semaphore = vk_sem_next;
        ret = vkGetSemaphoreFdKHR(cugan->vkdev->vkdevice(), &semaphoreGetFdInfo, &sem_next);
#endif
        if (ret != VK_SUCCESS) {
            sem_next = 0;
            destroy_sem(cugan);
            return;
        }

        glGenSemaphoresEXT(1, &gl_sem);
        glGenSemaphoresEXT(1, &gl_sem_next);
#ifdef _WIN32
        if (compatible_semaphore_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT) {
            glImportSemaphoreWin32HandleEXT(gl_sem, GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT, sem);
            glImportSemaphoreWin32HandleEXT(gl_sem_next, GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT, sem_next);
            sem = NULL;
            sem_next = NULL;
        } else {
            glImportSemaphoreWin32HandleEXT(gl_sem, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, sem);
            glImportSemaphoreWin32HandleEXT(gl_sem_next, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, sem_next);
        }
#else
        glImportSemaphoreFdEXT(gl_sem, GL_HANDLE_TYPE_OPAQUE_FD_EXT, sem);
        glImportSemaphoreFdEXT(gl_sem_next, GL_HANDLE_TYPE_OPAQUE_FD_EXT, sem_next);
        sem = 0;
        sem_next= 0;
#endif
    } else {
        vk_sem = 0;
        sem = 0;
        gl_sem = 0;

        vk_sem_next = 0;
        sem_next = 0;
        gl_sem_next = 0;
    }
}

void OutVkImageMat::destroy_sem(const RealCUGAN* cugan)
{
    if (shared_sem_supported(cugan)) {
        if (gl_sem) {
            glDeleteSemaphoresEXT(1, &gl_sem);
            gl_sem = 0;
            sem = 0;
        }

        if (sem) {
#ifdef _WIN32
            CloseHandle(sem);
#else
            close(sem);
#endif
            sem = 0;
        }

        if (vk_sem) {
            vkDestroySemaphore(cugan->vkdev->vkdevice(), vk_sem, nullptr);
            vk_sem = 0;
        }

        if (gl_sem_next) {
            glDeleteSemaphoresEXT(1, &gl_sem_next);
            gl_sem_next = 0;
            sem_next = 0;
        }

        if (sem_next) {
#ifdef _WIN32
            CloseHandle(sem_next);
#else
            close(sem_next);
#endif
            sem_next = 0;
        }

        if (vk_sem_next) {
            vkDestroySemaphore(cugan->vkdev->vkdevice(), vk_sem_next, nullptr);
            vk_sem_next = 0;
        }
    }
}

void OutVkImageMat::create_like(const RealCUGAN* cugan, const ncnn::VkMat& m, const ncnn::Option& opt) {
    int _dims = m.dims;
    if (_dims == 2)
        create(cugan, m.w, m.h, m.elemsize, m.elempack, opt);
    else if (_dims == 3)
        create(cugan, m.w, m.h, m.c, m.elemsize, m.elempack, opt);
    else
        release(cugan);
}

VkImageMemory* OutVkImageMat::out_create(const RealCUGAN* cugan,
    int w, int h, int c, size_t elemsize, int elempack,
    size_t& totalsize, bool& dedicated, VkExternalMemoryHandleTypeFlagBits &compatible_memory_type
) {
    if (elempack != 1 && elempack != 4 && elempack != 8)
    {
        NCNN_LOGE("elempack must be 1 4 8");
        return 0;
    }

    // resolve format
    VkFormat format = VK_FORMAT_UNDEFINED;

    if (c == 1 && elemsize / elempack == 4 && elempack == 1)
    {
        format = VK_FORMAT_R8G8B8A8_UNORM;
    }
    else if (c == 4 && elemsize / elempack == 4 && elempack == 1)
    {
        format = VK_FORMAT_R32_SFLOAT;
    }
    else
    {
        NCNN_LOGE("not supported params combination");
        return 0;
    }

    // resolve image width height depth
    int width = w;
    int height = h;
    int depth = c;

    // large elempack spills on image w
    if (elempack == 8) width *= 2;

    if (depth == 1) {
        if (width > (int)cugan->vkdev->info.max_image_dimension_2d() || height > (int)cugan->vkdev->info.max_image_dimension_2d())
        {
            NCNN_LOGE("image dimension too large %d %d > %d", width, height, (int)cugan->vkdev->info.max_image_dimension_2d());
            return 0;
        }
    } else {
        if (width > (int)cugan->vkdev->info.max_image_dimension_3d() || height > (int)cugan->vkdev->info.max_image_dimension_3d() || depth > (int)cugan->vkdev->info.max_image_dimension_3d())
        {
            NCNN_LOGE("image dimension too large %d %d %d > %d", width, height, depth, (int)cugan->vkdev->info.max_image_dimension_3d());
            return 0;
        }
    }

    VkResult ret;

    VkImageTiling tiling;
    VkImageUsageFlags usage;

    if (cugan->d3d11) {
#ifdef _WIN32
        compatible_memory_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        tiling = VK_IMAGE_TILING_OPTIMAL;
        usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VkPhysicalDeviceExternalImageFormatInfo extImgFmtInfo {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO
        };
        extImgFmtInfo.handleType = compatible_memory_type;

        VkPhysicalDeviceImageFormatInfo2 phyDevImgFmtInfo {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2
        };
        phyDevImgFmtInfo.pNext = &extImgFmtInfo;
        phyDevImgFmtInfo.format = format;
        phyDevImgFmtInfo.type = depth == 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
        phyDevImgFmtInfo.tiling = tiling;
        phyDevImgFmtInfo.usage = usage;

        VkExternalImageFormatProperties extImgFmtProps {
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES
        };

        VkImageFormatProperties2 imgFmtProps {
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2
        };
        imgFmtProps.pNext = &extImgFmtProps;

        if (!vkGetPhysicalDeviceImageFormatProperties2) {
            VkInstance instance = get_gpu_instance();
            vkGetPhysicalDeviceImageFormatProperties2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties2");
            if (!vkGetPhysicalDeviceImageFormatProperties2) {
                NCNN_LOGE("vkGetInstanceProcAddr vkGetPhysicalDeviceImageFormatProperties2 failed");
                return 0;
            }
        }

        ret = vkGetPhysicalDeviceImageFormatProperties2(cugan->vkdev->info.physical_device(), &phyDevImgFmtInfo, &imgFmtProps);
        if (ret != VK_SUCCESS) {
            NCNN_LOGE("vkGetPhysicalDeviceImageFormatProperties2 failed: %d", (int)ret);
            return 0;
        }

        if (!(extImgFmtProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            NCNN_LOGE("Texture type not importable");
            return 0;
        }

        dedicated = (bool)(extImgFmtProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);

        HRESULT hr;
        if (depth == 1) {
            D3D11_TEXTURE2D_DESC desc;
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            desc.CPUAccessFlags = 0;
            hr = cugan->dev[index]->CreateTexture2D(&desc, NULL, (ID3D11Texture2D **)&d3d_resource);
            if (hr) {
                NCNN_LOGE("CreateTexture2D failed: %d", (int)hr);
                return 0;
            }
        } else {
            // shared NT handle not supported for 3D textures..
            return 0;
        }

        hr = d3d_resource->QueryInterface(&dxgi_res);
        if (hr) {
            NCNN_LOGE("QueryInterface IDXGIResource1 failed: %d", (int)hr);
            return 0;
        }

        hr = dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &d3d_handle);
        if (hr) {
            NCNN_LOGE("CreateSharedHandle failed: %d", (int)hr);
            return 0;
        }

        hr = d3d_resource->QueryInterface(&dxgi_mutex);
        if (hr) {
            NCNN_LOGE("QueryInterface IDXGIKeyedMutex failed: %d\n", (int)hr);
            return 0;
        }

        hr = cugan->dev[index]->CreateShaderResourceView(d3d_resource, NULL, &d3d_srv);
        if (hr) {
            NCNN_LOGE("CreateShaderResourceView failed: %d\n", (int)hr);
            return 0;
        }
#endif
    } else {
        bool found = false;
        if (vkGetPhysicalDeviceExternalBufferPropertiesKHR) {
            VkExternalMemoryHandleTypeFlagBits flags[] = {
#ifdef _WIN32
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
#else
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
#endif
            };

            VkPhysicalDeviceExternalBufferInfo extBufInfo{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, nullptr};
            VkExternalBufferProperties extBufProps{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
                                                nullptr};

            for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
            {
                extBufInfo.handleType = flags[i];
                vkGetPhysicalDeviceExternalBufferPropertiesKHR(cugan->vkdev->info.physical_device(), &extBufInfo, &extBufProps);
                if (
                    extBufProps.externalMemoryProperties.compatibleHandleTypes & flags[i] &&
                    extBufProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT
                )
                {
                    compatible_memory_type = flags[i];
                    found = true;
                    // Default behavior for dedicated in case we can't test for it below
                    dedicated = (bool)(extBufProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
                    break;
                }
            }
        }
        if (!found) {
#ifdef _WIN32
            compatible_memory_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
            compatible_memory_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
            dedicated = true;
        }

        tiling = cugan->tiling_linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
        // usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    VkExternalMemoryImageCreateInfo extImageCreateInfo { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extImageCreateInfo.pNext = 0;
    extImageCreateInfo.handleTypes = compatible_memory_type;

    VkImageCreateInfo imageCreateInfo { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageCreateInfo.pNext = &extImageCreateInfo;
    imageCreateInfo.flags = 0;
    imageCreateInfo.imageType = depth == 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
    imageCreateInfo.format = format;
    imageCreateInfo.extent.width = width;
    imageCreateInfo.extent.height = height;
    imageCreateInfo.extent.depth = depth;
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = tiling;
    imageCreateInfo.usage = usage;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.queueFamilyIndexCount = 0;
    imageCreateInfo.pQueueFamilyIndices = 0;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image;
    ret = vkCreateImage(cugan->vkdev->vkdevice(), &imageCreateInfo, 0, &image);
    if (ret != VK_SUCCESS)
    {
        NCNN_LOGE("vkCreateImage failed %d %d %d %d %d %d %d", ret, width, height, depth, format, tiling, usage);
        return 0;
    }

    size_t aligned_size;
    uint32_t image_memory_type_index;
    if (cugan->vkdev->info.support_VK_KHR_get_memory_requirements2()) {
        VkMemoryDedicatedRequirements dedReqs;
        VkImageMemoryRequirementsInfo2 memReqsInfo2;
        VkMemoryRequirements2 memReqs2;

        memset(&dedReqs, 0, sizeof(dedReqs));
        dedReqs.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;

        /* VkImageMemoryRequirementsInfo2 */
        memset(&memReqsInfo2, 0, sizeof(memReqsInfo2));
        memReqsInfo2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
        memReqsInfo2.image = image;

        /* VkMemoryRequirements2 */
        memset(&memReqs2, 0, sizeof(memReqs2));
        memReqs2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        memReqs2.pNext = &dedReqs;

        cugan->vkdev->vkGetImageMemoryRequirements2KHR(cugan->vkdev->vkdevice(), &memReqsInfo2, &memReqs2);

        const size_t size = memReqs2.memoryRequirements.size;
        const size_t alignment = memReqs2.memoryRequirements.alignment;

        aligned_size = alignSize(size, alignment);
        image_memory_type_index = cugan->vkdev->find_memory_index(memReqs2.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

        if (!dedicated)
            dedicated = dedReqs.requiresDedicatedAllocation;
    } else {
        VkMemoryRequirements memoryRequirements;
        vkGetImageMemoryRequirements(cugan->vkdev->vkdevice(), image, &memoryRequirements);

        const size_t size = memoryRequirements.size;
        const size_t alignment = memoryRequirements.alignment;

        aligned_size = alignSize(size, alignment);
        image_memory_type_index = cugan->vkdev->find_memory_index(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }

    VkMemoryDedicatedAllocateInfoKHR dedAllocInfo { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedAllocInfo.image = image;

    VkMemoryAllocateInfo memoryAllocateInfo;
    memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryAllocateInfo.allocationSize = aligned_size;
    memoryAllocateInfo.memoryTypeIndex = image_memory_type_index;

#ifdef _WIN32
    VkImportMemoryWin32HandleInfoKHR d3dExtAllocInfo { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
#endif
    VkExportMemoryAllocateInfo oglExtAllocInfo {
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr,
        extImageCreateInfo.handleTypes
    };

    if (cugan->d3d11) {
#ifdef _WIN32
        d3dExtAllocInfo.handleType = compatible_memory_type;
        d3dExtAllocInfo.handle = d3d_handle;
        if (dedicated) {
            d3dExtAllocInfo.pNext = &dedAllocInfo;
        }
        memoryAllocateInfo.pNext = &d3dExtAllocInfo;
#endif
    } else {
        if (dedicated) {
            oglExtAllocInfo.pNext = &dedAllocInfo;
        }
        memoryAllocateInfo.pNext = &oglExtAllocInfo;
    }

    VkDeviceMemory memory = 0;
    ret = vkAllocateMemory(cugan->vkdev->vkdevice(), &memoryAllocateInfo, 0, &memory);
    if (ret != VK_SUCCESS)
    {
        NCNN_LOGE("vkAllocateMemory failed %d", ret);
        vkDestroyImage(cugan->vkdev->vkdevice(), image, 0);
        return 0;
    }

    VkImageMemory* ptr = new VkImageMemory;

    ptr->image = image;

    ptr->width = width;
    ptr->height = height;
    ptr->depth = depth;
    ptr->format = format;

    ptr->memory = memory;
    ptr->bind_offset = 0;
    ptr->bind_capacity = aligned_size;

    vkBindImageMemory(cugan->vkdev->vkdevice(), ptr->image, ptr->memory, ptr->bind_offset);

    ptr->mapped_ptr = 0;

    VkImageViewCreateInfo imageViewCreateInfo;
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.pNext = 0;
    imageViewCreateInfo.flags = 0;
    imageViewCreateInfo.image = image;
    imageViewCreateInfo.viewType = depth == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_3D;
    imageViewCreateInfo.format = format;
    imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = 1;

    VkImageView imageview;
    ret = vkCreateImageView(cugan->vkdev->vkdevice(), &imageViewCreateInfo, 0, &imageview);
    if (ret != VK_SUCCESS)
    {
        NCNN_LOGE("vkCreateImageView failed %d", ret);
        vkFreeMemory(cugan->vkdev->vkdevice(), memory, 0);
        vkDestroyImage(cugan->vkdev->vkdevice(), image, 0);
        return 0;
    }

    ptr->imageview = imageview;

    ptr->access_flags = 0;
    ptr->image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ptr->stage_flags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    ptr->command_refcount = 0;
    ptr->refcount = -1;

    totalsize = aligned_size;

    return ptr;
}

void OutVkImageMat::create(const RealCUGAN* cugan, int _w, int _h, size_t _elemsize, int _elempack, const ncnn::Option& opt)
{
    if (dims == 2 && w == _w && h == _h && elemsize == _elemsize && elempack == _elempack) {
        allocator = opt.blob_vkallocator;
        data->refcount = -1;
        return;
    }

    release(cugan);

    elemsize = _elemsize;
    elempack = _elempack;
    allocator = opt.blob_vkallocator;

    dims = 2;
    width = w = _w;
    height = h = _h;
    d = 1;
    depth = c = 1;

    if (total() > 0)
    {
        data = out_create(cugan, w, h, c, elemsize, elempack, totalsize, dedicated, memory_type);

        if (data) create_handles(cugan);
        else release(cugan);
    }
    else
    {
        release(cugan);
    }
}

void OutVkImageMat::create(const RealCUGAN* cugan, int _w, int _h, int _c, size_t _elemsize, int _elempack, const ncnn::Option& opt)
{
    if (dims == 3 && w == _w && h == _h && c == _c && elemsize == _elemsize && elempack == _elempack) {
        allocator = opt.blob_vkallocator;
        data->refcount = -1;
        return;
    }

    release(cugan);

    elemsize = _elemsize;
    elempack = _elempack;
    allocator = opt.blob_vkallocator;

    dims = 3;
    width = w = _w;
    height = h = _h;
    d = 1;
    depth = c = _c;

    if (total() > 0)
    {
        data = out_create(cugan, w, h, c, elemsize, elempack, totalsize, dedicated, memory_type);

        if (data) create_handles(cugan);
        else release(cugan);
    }
    else
    {
        release(cugan);
    }
}

void OutVkImageMat::release(const RealCUGAN* cugan)
{
    if (data)
    {
        release_handles();

#ifdef _WIN32
        if (d3d_handle) {
            CloseHandle(d3d_handle);
            d3d_handle = NULL;
        }

        if (dxgi_res) {
            dxgi_res->Release();
            dxgi_res = NULL;
        }

        if (d3d_srv) {
            d3d_srv->Release();
            d3d_srv = NULL;
        }

        if (dxgi_mutex) {
            dxgi_mutex->Release();
            dxgi_mutex = NULL;
        }

        if (d3d_resource) {
            d3d_resource->Release();
            d3d_resource = NULL;
        }
#endif

        vkDestroyImageView(cugan->vkdev->vkdevice(), data->imageview, 0);
        vkFreeMemory(cugan->vkdev->vkdevice(), data->memory, 0);
        vkDestroyImage(cugan->vkdev->vkdevice(), data->image, 0);

        delete data;

        data = 0;
    }

    allocator = NULL;
    elemsize = 0;
    elempack = 0;

    dims = 0;
    w = 0;
    h = 0;
    d = 0;
    c = 0;

    totalsize = 0;
    width = height = depth = 0;
}

void OutVkImageMat::create_handles(const RealCUGAN* cugan)
{
    release_handles();

#if _WIN32
    if (!(GLAD_GL_EXT_memory_object && GLAD_GL_EXT_memory_object_win32)) return;

    if (!vkGetMemoryWin32HandleKHR) {
        VkInstance instance = get_gpu_instance();
        vkGetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)vkGetInstanceProcAddr(instance, "vkGetMemoryWin32HandleKHR");
        if (!vkGetMemoryWin32HandleKHR) {
            return;
        }
    }

    VkMemoryGetWin32HandleInfoKHR memoryFdInfo{
        VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr,
        data->memory,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};

    VkResult ret = vkGetMemoryWin32HandleKHR(cugan->vkdev->vkdevice(), &memoryFdInfo, &memory);
    if (ret != VK_SUCCESS) {
        memory = 0;
        return;
    }
#else
    if (!(GLAD_GL_EXT_memory_object && GLAD_GL_EXT_memory_object_fd)) return;

    if (!vkGetMemoryFdKHR) {
        VkInstance instance = get_gpu_instance();
        vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetInstanceProcAddr(instance, "vkGetMemoryFdKHR");
        if (!vkGetMemoryFdKHR) {
            return;
        }
    }

    VkMemoryGetFdInfoKHR memoryFdInfo{
        VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr,
        data->memory,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};

    VkResult ret = vkGetMemoryFdKHR(cugan->vkdev->vkdevice(), &memoryFdInfo, &memory);
    if (ret != VK_SUCCESS) {
        memory = 0;
        return;
    }
#endif

    glCreateMemoryObjectsEXT(1, &gl_memory);
    GLint ded = dedicated ? GL_TRUE : GL_FALSE;
    glMemoryObjectParameterivEXT(gl_memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &ded);
#if _WIN32
    if (memory_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT) {
        glImportMemoryWin32HandleEXT(gl_memory, totalsize, GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT, memory);
        memory = NULL;
    } else {
        glImportMemoryWin32HandleEXT(gl_memory, totalsize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, memory);
    }
#else
    glImportMemoryFdEXT(gl_memory, totalsize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, memory);
    memory = 0;
#endif

    glGenTextures(1, &gl_texture);
    GLint tiling = cugan->tiling_linear ? GL_LINEAR_TILING_EXT : GL_OPTIMAL_TILING_EXT;
    if (depth == 1) {
        glBindTexture(GL_TEXTURE_2D, gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, tiling);
        glTextureStorageMem2DEXT(
            gl_texture, 1, GL_RGBA8, width,
            height, gl_memory, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        glBindTexture(GL_TEXTURE_3D, gl_texture);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_TILING_EXT, tiling);
        glTextureStorageMem3DEXT(
            gl_texture, 1, GL_R32F, width,
            height, depth, gl_memory, 0);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_3D, 0);
    }
}

void OutVkImageMat::release_handles()
{
    if (gl_texture) {
        glDeleteTextures(1, &gl_texture);
        gl_texture = 0;
    }
    if (gl_memory) {
        glDeleteMemoryObjectsEXT(1, &gl_memory);
        gl_memory = 0;
        memory = 0;
    }
    if (memory) {
#if _WIN32
        CloseHandle(memory);
#else
        close(memory);
#endif
        memory = 0;
    }
}
