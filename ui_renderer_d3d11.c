#include "ui_compositor_csc.h"
#include "ui_common_sdl.h"
#include "ui_renderer_d3d11.h"
#include "ui_main_nk.h"
#include "main.h"

static SDL_Window *sdl_win[SCREEN_COUNT];
static struct nk_context *nk_ctx;

#include "nuklear_d3d11.h"
#include "realcugan-ncnn-vulkan/lib.h"
#include "magpie/lib.h"

#include <versionhelpers.h>

#define MAX_VERTEX_BUFFER 512 * 1024
#define MAX_INDEX_BUFFER 128 * 1024

struct d3d_vertex_t {
    float pos[2];
    float uv[2];
};
static ID3D11Buffer *d3d_child_vb[SCREEN_COUNT][SCREEN_COUNT];
static ID3D11Buffer *d3d_vb[SCREEN_COUNT];
static ID3D11Buffer *d3d_ib[SCREEN_COUNT];
static ID3D11InputLayout *d3d_il[SCREEN_COUNT];
static ID3D11BlendState *d3d_ui_bs[SCREEN_COUNT];
static ID3D11VertexShader *d3d_vs[SCREEN_COUNT];
static ID3D11PixelShader *d3d_ps[SCREEN_COUNT];
static ID3D11SamplerState *d3d_ss_point[SCREEN_COUNT];
static ID3D11SamplerState *d3d_ss_linear[SCREEN_COUNT];
static struct magpie_t *magpie;
static int magpie_count;
static rp_lock_t magpie_update_lock;
static struct magpie_render_t *magpie_render[SCREEN_COUNT][SCREEN_COUNT];
static int magpie_render_mode[SCREEN_COUNT][SCREEN_COUNT];

static void d3d11_upscaling_update(int ctx_top_bot) {
    int i = ctx_top_bot;

    rp_lock_wait(upscaling_update_lock);

    if (i == SCREEN_TOP) {
        if (ui_upscaling_selected == ui_upscaling_post_index(UI_UPSCALING_FILTER_REAL_CUGAN)) {
            if (!upscaling_filter_realcugan_created) {
                int ret = realcugan_d3d11_create(d3d11device, d3d11device_context, dxgi_adapter);
                if (ret < 0) {
                    err_log("Real-CUGAN init failed\n");
                    upscaling_filter_realcugan = 0;
                    ui_upscaling_selected = UI_UPSCALING_FILTER_NONE;
                } else {
                    upscaling_filter_realcugan = 1;
                    upscaling_filter_realcugan_created = 1;
                }
            } else {
                upscaling_filter_realcugan = 1;
            }
        } else {
            upscaling_filter_realcugan = 0;
        }
    }

    rp_lock_rel(upscaling_update_lock);
}

static int d3d11_upscaling_init(void) {
    rp_lock_init(upscaling_update_lock);
    rp_lock_init(magpie_update_lock);

    ui_upscaling_filter_count = UI_UPSCALING_FILTER_EXTRA_COUNT;

    magpie_startup();

    magpie = magpie_load();
    if (magpie) {
        magpie_count = magpie_mode_count(magpie);
        if (magpie_count >= MAGPIE_MODE_START) {
            ui_upscaling_filter_count += magpie_count - MAGPIE_MODE_START;
        }
    }

    ui_upscaling_filter_options = malloc(ui_upscaling_filter_count * sizeof(*ui_upscaling_filter_options));
    if (!ui_upscaling_filter_options) {
        return -1;
    }

    ui_upscaling_filter_options[UI_UPSCALING_FILTER_NONE] = "None";
    ui_upscaling_filter_options[ui_upscaling_post_index(UI_UPSCALING_FILTER_REAL_CUGAN)] = "Real-CUGAN";

    for (int i = 0; i < magpie_count - MAGPIE_MODE_START; ++i) {
        ui_upscaling_filter_options[UI_UPSCALING_FILTER_PRE_COUNT + i] = magpie_mode_name(magpie, i + MAGPIE_MODE_START);
    }

    ui_upscaling_selected = 0;

    return 0;
}

static void magpie_cleanup_aux(void);
static void d3d11_upscaling_close(void) {
    if (magpie) {
        magpie_unload(magpie);
        magpie = 0;
    }
    magpie_count = 0;

    magpie_cleanup_aux();

    if (ui_upscaling_filter_options) {
        free(ui_upscaling_filter_options);
        ui_upscaling_filter_options = 0;
    }
    ui_upscaling_filter_count = 0;

    rp_lock_close(magpie_update_lock);
    rp_lock_close(upscaling_update_lock);
}

static const char *d3d_vs_src =
    "struct VSInput\n"
    "{\n"
    " float2 position: POSITION;\n"
    " float2 uv: TEXCOORD;\n"
    "};\n"
    "struct VSOutput\n"
    "{\n"
    " float4 position: SV_Position;\n"
    " float2 uv: TEXCOORD;\n"
    "};\n"
    "VSOutput Main(VSInput input)\n"
    "{\n"
    " VSOutput output = (VSOutput)0;\n"
    " output.position = float4(input.position, 0.0, 1.0);\n"
    " output.uv = input.uv;\n"
    " return output;\n"
    "}\n";
static const char *d3d_ps_src =
    "SamplerState my_samp: register(s0);\n"
    "Texture2D my_tex: register(t0);\n"
    "struct PSInput\n"
    "{\n"
    " float4 position: SV_Position;\n"
    " float2 uv: TEXCOORD;\n"
    "};\n"
    "struct PSOutput\n"
    "{\n"
    " float4 color: SV_Target0;\n"
    "};\n"
    "PSOutput Main(PSInput input)\n"
    "{\n"
    " PSOutput output = (PSOutput)0;\n"
    " float4 color = my_tex.Sample(my_samp, input.uv);\n"
    " if (any(color != float4(0.0, 0.0, 0.0, 0.0)))\n"
    "  color = float4(color.rgb * (15.0 / 16.0), 15.0 / 16.0);\n"
    " output.color = color;\n"
    " return output;\n"
    "}\n";

static ID3DBlob *compile_shader(const char *src, const char *target)
{
    HRESULT hr;
    ID3DBlob *code;
    ID3DBlob *err_msg;
    hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, "Main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &err_msg);
    if (hr) {
        err_log("D3DCompile failed: %d\n", (int)hr);
        if (err_msg) {
            err_log("%s\n", (const char *)err_msg->lpVtbl->GetBufferPointer(err_msg));
            IUnknown_Release(err_msg);
        }
        return NULL;
    }
    if (err_msg)
        IUnknown_Release(err_msg);
    return code;
}

static ID3D11VertexShader *load_vs(ID3D11Device *dev, const char *src, ID3DBlob **compiled)
{
    ID3DBlob *code = compile_shader(src, "vs_4_0");
    if (!code) {
        return NULL;
    }

    ID3D11VertexShader *vs;
    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(dev, code->lpVtbl->GetBufferPointer(code), code->lpVtbl->GetBufferSize(code), NULL, &vs);
    if (hr) {
        err_log("CreateVertexShader failed: %d\n", (int)hr);
        IUnknown_Release(code);
        return NULL;
    }
    *compiled = code;
    return vs;
}

static ID3D11PixelShader *load_ps(ID3D11Device *dev, const char *src)
{
    ID3DBlob *code = compile_shader(src, "ps_4_0");
    if (!code) {
        return NULL;
    }

    ID3D11PixelShader *ps;
    HRESULT hr;
    hr = ID3D11Device_CreatePixelShader(dev, code->lpVtbl->GetBufferPointer(code), code->lpVtbl->GetBufferSize(code), NULL, &ps);
    if (hr) {
        err_log("CreatePixelShader failed: %d\n", (int)hr);
        IUnknown_Release(code);
        return NULL;
    }
    IUnknown_Release(code);
    return ps;
}

static int d3d11_init(void) {
    for (int j = 0; j < SCREEN_COUNT; ++j) {
        HRESULT hr;

        ID3DBlob *vs_code;
        d3d_vs[j] = load_vs(d3d11device[j], d3d_vs_src, &vs_code);
        if (!d3d_vs[j]) {
            return -1;
        }
        d3d_ps[j] = load_ps(d3d11device[j], d3d_ps_src);
        if (!d3d_ps[j]) {
            return -1;
        }

        D3D11_INPUT_ELEMENT_DESC input_desc[] =
            {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            };

        hr = ID3D11Device_CreateInputLayout(
            d3d11device[j],
            input_desc,
            ARRAYSIZE(input_desc),
            vs_code->lpVtbl->GetBufferPointer(vs_code),
            vs_code->lpVtbl->GetBufferSize(vs_code),
            &d3d_il[j]);
        if (hr) {
            err_log("CreateInputLayout failed: %d\n", (int)hr);
            return -1;
        }
        CHECK_AND_RELEASE(vs_code);

        const struct d3d_vertex_t vb_data[] = {
            {{-1.0f, 1.0f}, {0.0f, 0.0f}},
            {{-1.0f, -1.0f}, {0.0f, 1.0f}},
            {{1.0f, 1.0f}, {1.0f, 0.0f}},
            {{1.0f, -1.0f}, {1.0f, 1.0f}},
        };
        D3D11_BUFFER_DESC vb_desc = {};
        vb_desc.ByteWidth = sizeof(vb_data);
        vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vb_srd = {.pSysMem = vb_data};
        hr = ID3D11Device_CreateBuffer(d3d11device[j], &vb_desc, &vb_srd, &d3d_vb[j]);
        if (hr) {
            err_log("CreateBuffer failed: %d\n", (int)hr);
            return -1;
        }

        const unsigned ib_data[] =
            {0, 2, 1, 1, 2, 3};
        D3D11_BUFFER_DESC ib_desc = {};
        ib_desc.ByteWidth = sizeof(ib_data);
        ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
        ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA ib_srd = {.pSysMem = ib_data};
        hr = ID3D11Device_CreateBuffer(d3d11device[j], &ib_desc, &ib_srd, &d3d_ib[j]);
        if (hr) {
            err_log("CreateBuffer failed: %d\n", (int)hr);
            return -1;
        }

        D3D11_BLEND_DESC blend_desc = {
            .RenderTarget = {
                {
                    .BlendEnable = FALSE,
                    .SrcBlend = D3D11_BLEND_ONE,
                    .DestBlend = D3D11_BLEND_ZERO,
                    .BlendOp = D3D11_BLEND_OP_ADD,
                    .SrcBlendAlpha = D3D11_BLEND_ONE,
                    .DestBlendAlpha = D3D11_BLEND_ZERO,
                    .BlendOpAlpha = D3D11_BLEND_OP_ADD,
                    .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
                }}};
        hr = ID3D11Device_CreateBlendState(d3d11device[j], &blend_desc, &d3d_ui_bs[j]);
        if (hr) {
            err_log("CreateBlendState failed: %d\n", (int)hr);
            return -1;
        }

        D3D11_SAMPLER_DESC sampler_desc = {};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        hr = ID3D11Device_CreateSamplerState(d3d11device[j], &sampler_desc, &d3d_ss_point[j]);
        if (hr) {
            err_log("CreateSamplerState failed: %d\n", (int)hr);
            return -1;
        }

        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        hr = ID3D11Device_CreateSamplerState(d3d11device[j], &sampler_desc, &d3d_ss_linear[j]);
        if (hr) {
            err_log("CreateSamplerState failed: %d\n", (int)hr);
            return -1;
        }

        for (int i = 0; i < SCREEN_COUNT; ++i) {
            D3D11_BUFFER_DESC child_vb_desc = {};
            child_vb_desc.ByteWidth = sizeof(struct d3d_vertex_t) * 4;
            child_vb_desc.Usage = D3D11_USAGE_DYNAMIC;
            child_vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            child_vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = ID3D11Device_CreateBuffer(d3d11device[j], &child_vb_desc, NULL, &d3d_child_vb[j][i]);
            if (hr) {
                err_log("CreateBuffer failed: %d\n", (int)hr);
                return -1;
            }

            D3D11_TEXTURE2D_DESC tex_desc = {};
            tex_desc.Width = 240;
            tex_desc.Height = i == SCREEN_TOP ? 400 : 320;
            tex_desc.MipLevels = 1;
            tex_desc.ArraySize = 1;
            tex_desc.Format = D3D_FORMAT;
            tex_desc.SampleDesc.Count = 1;
            tex_desc.SampleDesc.Quality = 0;
            tex_desc.Usage = D3D11_USAGE_DYNAMIC;
            tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            tex_desc.MiscFlags = 0;
            tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            hr = ID3D11Device_CreateTexture2D(d3d11device[j], &tex_desc, NULL, &rp_buffer_ctx[i].d3d_tex[j]);
            if (hr) {
                err_log("CreateTexture2D failed: %d\n", (int)hr);
                return -1;
            }

            hr = ID3D11Device_CreateShaderResourceView(d3d11device[j], (ID3D11Resource *)rp_buffer_ctx[i].d3d_tex[j], NULL, &rp_buffer_ctx[i].d3d_srv[j]);
            if (hr) {
                err_log("CreateShaderResourceView failed: %d\n", (int)hr);
                return -1;
            }
        }
    }

    if (d3d11_ui_init())
        return -1;

    return 0;
}

static void d3d11_close(void)
{
    d3d11_ui_close();

    for (int j = 0; j < SCREEN_COUNT; ++j) {
        for (int i = 0; i < SCREEN_COUNT; ++i) {
            CHECK_AND_RELEASE(rp_buffer_ctx[j].d3d_srv[i]);
            CHECK_AND_RELEASE(rp_buffer_ctx[j].d3d_tex[i]);
            CHECK_AND_RELEASE(d3d_child_vb[j][i]);
        }

        CHECK_AND_RELEASE(d3d_ui_bs[j]);
        CHECK_AND_RELEASE(d3d_ib[j]);
        CHECK_AND_RELEASE(d3d_vb[j]);
        CHECK_AND_RELEASE(d3d_ss_point[j]);
        CHECK_AND_RELEASE(d3d_ss_linear[j]);
        CHECK_AND_RELEASE(d3d_il[j]);
        CHECK_AND_RELEASE(d3d_vs[j]);
        CHECK_AND_RELEASE(d3d_ps[j]);
    }
}

static int d3d11_renderer_init(void) {
    if (dxgi_init())
        return -1;

    if (is_renderer_csc()) {
        if (composition_swapchain_init(ui_hwnd)) {
            return -1;
        }
        ui_compositing = 1;
    } else {
        HRESULT hr;
        for (int i = 0; i < SCREEN_COUNT; ++i) {
            D3D_FEATURE_LEVEL featureLevelSupported;
            hr = D3D11CreateDevice(
                (IDXGIAdapter *)dxgi_adapter,
                dxgi_adapter ? 0 : D3D_DRIVER_TYPE_HARDWARE,
                NULL,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                NULL,
                0,
                D3D11_SDK_VERSION,
                &d3d11device[i],
                &featureLevelSupported,
                &d3d11device_context[i]);
            if (hr) {
                err_log("D3D11CreateDevice failed: %d\n", (int)hr);
                return -1;
            }

            DXGI_SWAP_CHAIN_DESC sc_desc = {};
            sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sc_desc.SampleDesc.Count = 1;
            sc_desc.BufferCount = COMPAT_PRESENATTION_BUFFER_COUNT_PER_SCREEN;
            sc_desc.OutputWindow = ui_hwnd[i];
            sc_desc.Windowed = TRUE;
            sc_desc.SwapEffect = IsWindows10OrGreater() ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

            hr = IDXGIFactory2_CreateSwapChain(dxgi_factory, (IUnknown *)d3d11device[i], &sc_desc, &dxgi_sc[i]);
            if (hr) {
                err_log("CreateSwapChain failed: %d\n", (int)hr);
                return -1;
            }
        }
    }

    nk_ctx = nk_d3d11_init(d3d11device[SCREEN_TOP], WIN_WIDTH_DEFAULT, WIN_HEIGHT_DEFAULT, MAX_VERTEX_BUFFER, MAX_INDEX_BUFFER);
    if (!nk_ctx)
        return -1;

    if (d3d11_init()) {
        return -1;
    }

    if (d3d11_upscaling_init())
        return -1;

    ui_upscaling_filters = 1;

    err_log("d3d11 %s\n", is_renderer_csc() ? "composition swapchain" : "");

    return 0;
}

static void d3d11_renderer_destroy(void) {
    ui_upscaling_filters = 0;

    d3d11_upscaling_close();

    d3d11_close();

    if (nk_ctx) {
        nk_d3d11_shutdown();
        nk_ctx = NULL;
    }

    if (is_renderer_csc()) {
        ui_compositing = 0;
        composition_swapchain_close();
    } else {
        for (int i = 0; i < SCREEN_COUNT; ++i) {
            CHECK_AND_RELEASE(dxgi_sc[i]);
            CHECK_AND_RELEASE(d3d11device_context[i]);
            CHECK_AND_RELEASE(d3d11device[i]);
        }
    }

    dxgi_close();
}

int ui_renderer_d3d11_init(void) {
    if (sdl_win_init(sdl_win, 0)) {
        return -1;
    }

    for (int i = 0; i < SCREEN_COUNT; ++i)
        ui_sdl_win[i] = sdl_win[i];

    sdl_set_wminfo();

    for (int i = 0; i < SCREEN_COUNT; ++i) {
        ui_win_width_drawable[i] = WIN_WIDTH_DEFAULT;
        ui_win_height_drawable[i] = WIN_HEIGHT_DEFAULT;
        ui_win_scale[i] = 1.0f;
    }

    if (d3d11_renderer_init()) {
        return -1;
    }

    ui_nk_ctx = nk_ctx;

    return 0;
}

void ui_renderer_d3d11_destroy(void) {
    ui_nk_ctx = NULL;

    d3d11_renderer_destroy();

    sdl_reset_wminfo();

    for (int i = 0; i < SCREEN_COUNT; ++i)
        ui_sdl_win[i] = NULL;

    sdl_win_destroy(sdl_win);
}

static struct ID3D11RenderTargetView *d3d_rtv[SCREEN_COUNT]; // Non-owning

static ID3D11Texture2D *sc_tex[SCREEN_COUNT];
static ID3D11RenderTargetView *sc_rtv[SCREEN_COUNT];

static struct presentation_buffer_t *d3d_pres_buf[SCREEN_COUNT];

static ID3D11Texture2D *magpie_in_tex[SCREEN_COUNT][SCREEN_COUNT];
static SIZE magpie_in_size[SCREEN_COUNT][SCREEN_COUNT];
static SIZE magpie_out_size[SCREEN_COUNT][SCREEN_COUNT];
static ID3D11ShaderResourceView *magpie_out_srv[SCREEN_COUNT][SCREEN_COUNT];

static void magpie_free_aux(int ctx_top_bot, int screen_top_bot) {
    CHECK_AND_RELEASE(magpie_in_tex[ctx_top_bot][screen_top_bot]);
    CHECK_AND_RELEASE(magpie_out_srv[ctx_top_bot][screen_top_bot]);
}

static void magpie_cleanup_aux(void) {
    for (int j = 0; j < SCREEN_COUNT; ++j) {
        for (int i = 0; i < SCREEN_COUNT; ++i) {
            magpie_free_aux(j, i);
            if (magpie_render[j][i]) {
                magpie_render_close(magpie_render[j][i]);
                magpie_render[j][i] = NULL;
            }
        }
    }
}

static void magpie_upscaling_update(int selected, int ctx_top_bot, int screen_top_bot, int in_width, int in_height, int out_width, int out_height) {
    int i = ctx_top_bot;

    int render_mode = -1;
    if (selected == ui_upscaling_post_index(UI_UPSCALING_FILTER_REAL_CUGAN)) {
        render_mode = MAGPIE_MODE_REAL_CUGAN_RESERVED;
        in_width *= SCREEN_UPSCALE_FACTOR;
        in_height *= SCREEN_UPSCALE_FACTOR;
    } else if (selected >= UI_UPSCALING_FILTER_PRE_COUNT && selected < ui_upscaling_post_index(0)) {
        render_mode = MAGPIE_MODE_START + (selected - UI_UPSCALING_FILTER_PRE_COUNT);
    }

    SIZE in_size = { .cx = in_width, .cy = in_height };
    SIZE out_size = { .cx = out_width, .cy = out_height };

    if (
        magpie_render[i][screen_top_bot] && (
            magpie_render_mode[i][screen_top_bot] != render_mode ||
            memcmp(&magpie_out_size[i][screen_top_bot], &out_size, sizeof(SIZE)) != 0 ||
            memcmp(&magpie_in_size[i][screen_top_bot], &in_size, sizeof(SIZE)) != 0
        )
    ) {
        magpie_render_close(magpie_render[i][screen_top_bot]);
        magpie_render[i][screen_top_bot] = 0;

        magpie_free_aux(i, screen_top_bot);
    }
    if (!magpie_render[i][screen_top_bot] && render_mode >= 0) {
        rp_lock_wait(upscaling_update_lock);

        magpie_free_aux(i, screen_top_bot);

        D3D11_TEXTURE2D_DESC tex_desc = {};
        tex_desc.Width = in_width;
        tex_desc.Height = in_height;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = D3D_FORMAT;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.SampleDesc.Quality = 0;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tex_desc.MiscFlags = 0;
        tex_desc.CPUAccessFlags = 0;

        HRESULT hr;
        hr = ID3D11Device_CreateTexture2D(d3d11device[i], &tex_desc, NULL, &magpie_in_tex[i][screen_top_bot]);

        if (!magpie_in_tex[i][screen_top_bot]) {
            err_log("CreateTexture2D failed: %d\n", (int)hr);
            ui_upscaling_selected = UI_UPSCALING_FILTER_NONE;
            goto fail;
        }

        magpie_render[i][screen_top_bot] = magpie_render_init(magpie, render_mode, d3d11device[i], d3d11device_context[i], magpie_in_tex[i][screen_top_bot], &out_size);
        ID3D11Texture2D *tex = magpie_render[i][screen_top_bot] ? magpie_render_output(magpie_render[i][screen_top_bot]) : NULL;
        if (!tex) {
            err_log("magpie_render_init failed\n");

            if (magpie_render[i][screen_top_bot]) {
                magpie_render_close(magpie_render[i][screen_top_bot]);
                magpie_render[i][screen_top_bot] = NULL;
            }

            magpie_free_aux(i, screen_top_bot);
            ui_upscaling_selected = UI_UPSCALING_FILTER_NONE;
            goto fail;
        }

        hr = ID3D11Device_CreateShaderResourceView(d3d11device[i], (ID3D11Resource *)tex, NULL, &magpie_out_srv[i][screen_top_bot]);
        if (hr) {
            err_log("CreateShaderResourceView failed: %d\n", (int)hr);

            magpie_render_close(magpie_render[i][screen_top_bot]);
            magpie_render[i][screen_top_bot] = NULL;

            magpie_free_aux(i, screen_top_bot);
            ui_upscaling_selected = UI_UPSCALING_FILTER_NONE;
            goto fail;
        }

        magpie_render_mode[i][screen_top_bot] = render_mode;
        magpie_out_size[i][screen_top_bot] = out_size;
        magpie_in_size[i][screen_top_bot] = in_size;

fail:
        rp_lock_rel(upscaling_update_lock);
    }
}

void ui_renderer_d3d11_main(int screen_top_bot, int ctx_top_bot, view_mode_t view_mode, bool win_shared, float bg[4]) {
    int i = ctx_top_bot;
    HRESULT hr;

    d3d11_upscaling_update(i);

    int p = win_shared ? screen_top_bot : i;
    sc_fail[p] = 0;

    if (is_renderer_csc()) {
        ui_compositor_csc_main(screen_top_bot, i, win_shared);
        if (sc_fail[p]) {
            return;
        }

        struct presentation_buffer_t *bufs = presentation_buffers[i][screen_top_bot];
        int index_sc;
        if (presentation_buffer_get(bufs, p, win_shared, COMPAT_PRESENATTION_BUFFER_COUNT_PER_SCREEN, ui_ctx_width[p], ui_ctx_height[p], &index_sc) != 0) {
            sc_fail[p] = 1;
            return;
        }
        d3d_pres_buf[p] = &bufs[index_sc];
        d3d_rtv[p] = d3d_pres_buf[p]->rtv;
        ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 1, &d3d_rtv[p], NULL);
    } else {
        if (i == SCREEN_TOP)
            rp_lock_wait(comp_lock);

        if (ui_ctx_width[i] != ui_win_width_drawable[i] || ui_ctx_height[i] != ui_win_height_drawable[i]) {
            ui_ctx_width[i] = ui_win_width_drawable[i];
            ui_ctx_height[i] = ui_win_height_drawable[i];
            hr = IDXGISwapChain_ResizeBuffers(dxgi_sc[i], 0, 0, 0, 0, 0);
            if (hr) {
                err_log("ResizeBuffers failed: %d\n", (int)hr);
                sc_fail[p] = 1;
                return;
            }

            if (i == SCREEN_TOP) {
                if (nk_d3d11_resize(d3d11device_context[i], ui_ctx_width[i], ui_ctx_height[i], ui_win_scale[i])) {
                    err_log("nk_d3d11_resize failed\n");
                    sc_fail[p] = 1;
                    return;
                }
            }
        }

        hr = IDXGISwapChain_GetBuffer(dxgi_sc[i], 0, &IID_ID3D11Texture2D, (void **)&sc_tex[i]);
        if (hr) {
            err_log("GetBuffer failed: %d\n", (int)hr);
            sc_fail[p] = 1;
            return;
        }

        hr = ID3D11Device_CreateRenderTargetView(d3d11device[i], (ID3D11Resource *)sc_tex[i], NULL, &sc_rtv[i]);
        if (hr) {
            err_log("CreateRenderTargetView failed: %d\n", (int)hr);
            IUnknown_Release(sc_tex[i]);
            sc_fail[p] = 1;
            return;
        }

        d3d_rtv[i] = sc_rtv[i];
        ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 1, &d3d_rtv[i], NULL);
    }

    D3D11_VIEWPORT vp = { .Width = ui_ctx_width[p], .Height = ui_ctx_height[p] };
    ID3D11DeviceContext_RSSetViewports(d3d11device_context[i], 1, &vp);
    if (!win_shared) {
        ID3D11DeviceContext_ClearRenderTargetView(d3d11device_context[i], d3d_rtv[p], bg);
    }

    if (view_mode == VIEW_MODE_TOP_BOT && !win_shared) {
        draw_screen(&rp_buffer_ctx[SCREEN_TOP], SCREEN_HEIGHT0, SCREEN_WIDTH, SCREEN_TOP, i, view_mode, 0);
        draw_screen(&rp_buffer_ctx[SCREEN_BOT], SCREEN_HEIGHT1, SCREEN_WIDTH, SCREEN_BOT, i, view_mode, 0);
    } else if (view_mode == VIEW_MODE_BOT) {
        draw_screen(&rp_buffer_ctx[SCREEN_BOT], SCREEN_HEIGHT1, SCREEN_WIDTH, SCREEN_BOT, i, view_mode, win_shared);
    } else {
        if (!draw_screen(&rp_buffer_ctx[screen_top_bot], screen_top_bot == SCREEN_TOP ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1, SCREEN_WIDTH, screen_top_bot, i, view_mode, win_shared)) {
            ID3D11DeviceContext_ClearRenderTargetView(d3d11device_context[i], d3d_rtv[p], bg);
        }
    }
}

static int ctx_width[SCREEN_COUNT];
static int ctx_height[SCREEN_COUNT];
static int win_width_drawable[SCREEN_COUNT];
static int win_height_drawable[SCREEN_COUNT];

static void d3d11_draw_screen(int ctx_top_bot, int screen_top_bot, int p, struct d3d_vertex_t *vertices, ID3D11Resource *in_tex, ID3D11ShaderResourceView *in_srv)
{
    int i = ctx_top_bot;

    if (in_tex) {
        int width = SCREEN_WIDTH;
        int height = (screen_top_bot == SCREEN_TOP ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1);
        magpie_upscaling_update(ui_upscaling_selected, i, screen_top_bot, width, height, ctx_width[screen_top_bot], ctx_height[screen_top_bot]);

        if (magpie_render[i][screen_top_bot]) {
            ID3D11DeviceContext_CopyResource(d3d11device_context[i], (ID3D11Resource *)magpie_in_tex[i][screen_top_bot], in_tex);
            magpie_render_run(magpie_render[i][screen_top_bot]);
            in_srv = magpie_out_srv[i][screen_top_bot];
            ID3D11DeviceContext_ClearState(d3d11device_context[i]);

            ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 1, &d3d_rtv[p], NULL);
            D3D11_VIEWPORT vp = { .Width = ui_ctx_width[p], .Height = ui_ctx_height[p] };
            ID3D11DeviceContext_RSSetViewports(d3d11device_context[i], 1, &vp);
        }
    }

    {
        HRESULT hr;
        D3D11_MAPPED_SUBRESOURCE tex_mapped = {};
        hr = ID3D11DeviceContext_Map(d3d11device_context[i], (ID3D11Resource *)d3d_child_vb[i][screen_top_bot], 0, D3D11_MAP_WRITE_DISCARD, 0, &tex_mapped);
        if (hr) {
            err_log("Map failed: %d", (int)hr);
            return;
        }
        memcpy(tex_mapped.pData, vertices, sizeof(struct d3d_vertex_t) * 4);

        ID3D11DeviceContext_Unmap(d3d11device_context[i], (ID3D11Resource *)d3d_child_vb[i][screen_top_bot], 0);
    }

    ID3D11DeviceContext_IASetPrimitiveTopology(d3d11device_context[i], D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    unsigned vb_stride = sizeof(struct d3d_vertex_t);
    unsigned vb_offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(d3d11device_context[i], 0, 1, &d3d_child_vb[i][screen_top_bot], &vb_stride, &vb_offset);
    ID3D11DeviceContext_IASetIndexBuffer(d3d11device_context[i], d3d_ib[i], DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetInputLayout(d3d11device_context[i], d3d_il[i]);
    ID3D11DeviceContext_OMSetBlendState(d3d11device_context[i], d3d_ui_bs[i], NULL, 0xffffffff);
    ID3D11DeviceContext_VSSetShader(d3d11device_context[i], d3d_vs[i], NULL, 0);
    ID3D11DeviceContext_PSSetShader(d3d11device_context[i], d3d_ps[i], NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(d3d11device_context[i], 0, 1, &d3d_ss_linear[i]);
    ID3D11DeviceContext_RSSetState(d3d11device_context[i], NULL);
    ID3D11DeviceContext_PSSetShaderResources(d3d11device_context[i], 0, 1, &in_srv);
    ID3D11DeviceContext_DrawIndexed(d3d11device_context[i], 6, 0, 0);
    ID3D11ShaderResourceView *ptr_null = NULL;
    ID3D11DeviceContext_PSSetShaderResources(d3d11device_context[i], 0, 1, &ptr_null);
}

void ui_renderer_d3d11_draw(struct rp_buffer_ctx_t *ctx, uint8_t *data, int width, int height, int screen_top_bot, int ctx_top_bot, int index, view_mode_t view_mode, int win_shared) {
    double ctx_left_f;
    double ctx_top_f;
    double ctx_right_f;
    double ctx_bot_f;
    bool upscaled;
    draw_screen_get_dims(
        screen_top_bot, ctx_top_bot, win_shared, view_mode, width, height,
        &ctx_left_f, &ctx_top_f, &ctx_right_f, &ctx_bot_f, &ctx_width[screen_top_bot], &ctx_height[screen_top_bot], &win_width_drawable[screen_top_bot], &win_height_drawable[screen_top_bot], &upscaled);

    struct d3d_vertex_t vertices[] = {
        {{ctx_left_f, ctx_bot_f}, {0.0f, 0.0f}},
        {{ctx_right_f, ctx_bot_f}, {0.0f, 1.0f}},
        {{ctx_left_f, ctx_top_f}, {1.0f, 0.0f}},
        {{ctx_right_f, ctx_top_f}, {1.0f, 1.0f}},
    };

    HRESULT hr;

    if (width != (screen_top_bot == SCREEN_TOP ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1) || height != SCREEN_WIDTH) {
        err_log("Invalid size\n");
        return;
    }

    int i = ctx_top_bot;
    int p = win_shared ? screen_top_bot : i;
    if (view_mode == VIEW_MODE_BOT) {
        p = SCREEN_TOP;
    }

    if (upscaled) {
        if (!data) {
            if (!ctx->d3d_srv_upscaled[i]) {
                data = ctx->data_prev;
            }
        }
        if (data) {
            bool dim3;
            bool success;
            ctx->d3d_mutex_upscaled[i] = NULL;
            ctx->d3d_srv_upscaled[i] = NULL;
            ctx->d3d_res_upscaled[i] = realcugan_d3d11_run(i, screen_top_bot, index, height, width, GL_CHANNELS_N, data, ctx->screen_upscaled, &ctx->d3d_mutex_upscaled[i], &ctx->d3d_srv_upscaled[i], &dim3, &success);
            if (ctx->d3d_res_upscaled[i]) {
                hr = IDXGIKeyedMutex_AcquireSync(ctx->d3d_mutex_upscaled[i], 1, 2000);
                if (hr) {
                    err_log("AcquireSync failed: %d\n", (int)hr);
                    return;
                }

                d3d11_draw_screen(i, screen_top_bot, p, vertices, ctx->d3d_res_upscaled[i], ctx->d3d_srv_upscaled[i]);

                hr = IDXGIKeyedMutex_ReleaseSync(ctx->d3d_mutex_upscaled[i], 0);
                if (hr) {
                    err_log("ReleaseSync failed: %d\n", (int)hr);
                    return;
                }
                return;
            } else {
                if (success) {
                    int scale = SCREEN_UPSCALE_FACTOR;
                    width *= scale;
                    height *= scale;
                    if (!ctx->d3d_srv_upscaled_prev[i]) {
                        CHECK_AND_RELEASE(ctx->d3d_tex_upscaled_prev[i]);

                        D3D11_TEXTURE2D_DESC tex_desc = {};
                        tex_desc.Width = height;
                        tex_desc.Height = width;
                        tex_desc.MipLevels = 1;
                        tex_desc.ArraySize = 1;
                        tex_desc.Format = D3D_FORMAT;
                        tex_desc.SampleDesc.Count = 1;
                        tex_desc.SampleDesc.Quality = 0;
                        tex_desc.Usage = D3D11_USAGE_DYNAMIC;
                        tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        tex_desc.MiscFlags = 0;
                        tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                        hr = ID3D11Device_CreateTexture2D(d3d11device[i], &tex_desc, NULL, &ctx->d3d_tex_upscaled_prev[i]);
                        if (hr) {
                            err_log("CreateTexture2D failed: %d\n", (int)hr);
                            return;
                        }

                        hr = ID3D11Device_CreateShaderResourceView(d3d11device[i], (ID3D11Resource *)ctx->d3d_tex_upscaled_prev[i], NULL, &ctx->d3d_srv_upscaled_prev[i]);
                        if (hr) {
                            err_log("CreateShaderResourceView failed: %d\n", (int)hr);
                            CHECK_AND_RELEASE(ctx->d3d_tex_upscaled_prev[i]);
                            return;
                        }
                    }

                    D3D11_MAPPED_SUBRESOURCE tex_mapped = {};
                    hr = ID3D11DeviceContext_Map(d3d11device_context[i], (ID3D11Resource *)ctx->d3d_tex_upscaled_prev[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &tex_mapped);
                    if (hr) {
                        err_log("Map failed: %d", (int)hr);
                        return;
                    }
                    for (int i = 0; i < width; ++i) {
                        memcpy(tex_mapped.pData + i * tex_mapped.RowPitch, ctx->screen_upscaled + i * height * 4, height * 4);
                    }

                    ID3D11DeviceContext_Unmap(d3d11device_context[i], (ID3D11Resource *)ctx->d3d_tex_upscaled_prev[i], 0);

                    d3d11_draw_screen(i, screen_top_bot, p, vertices, (ID3D11Resource *)ctx->d3d_tex_upscaled_prev[i], ctx->d3d_srv_upscaled_prev[i]);
                } else {
                    upscaling_filter_realcugan = 0;
                    err_log("upscaling failed; filter disabled\n");
                }
                return;
            }
        } else if (ctx->d3d_srv_upscaled[i]) {
            hr = IDXGIKeyedMutex_AcquireSync(ctx->d3d_mutex_upscaled[i], 0, 2000);
            if (hr) {
                err_log("AcquireSync failed: %d\n", (int)hr);
                return;
            }

            d3d11_draw_screen(i, screen_top_bot, p, vertices, ctx->d3d_res_upscaled[i], ctx->d3d_srv_upscaled[i]);

            hr = IDXGIKeyedMutex_ReleaseSync(ctx->d3d_mutex_upscaled[i], 0);
            if (hr) {
                err_log("ReleaseSync failed: %d\n", (int)hr);
                return;
            }
            return;
        } else if (ctx->d3d_srv_upscaled_prev[i]) {
            d3d11_draw_screen(i, screen_top_bot, p, vertices, (ID3D11Resource *)ctx->d3d_tex_upscaled_prev[i], ctx->d3d_srv_upscaled_prev[i]);
        } else {
            err_log("no data\n");
        }
        return;
    }

    if (data) {
        D3D11_MAPPED_SUBRESOURCE tex_mapped = {};
        hr = ID3D11DeviceContext_Map(d3d11device_context[i], (ID3D11Resource *)ctx->d3d_tex[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &tex_mapped);
        if (hr) {
            err_log("Map failed: %d", (int)hr);
            return;
        }
        for (int i = 0; i < width; ++i) {
            memcpy(tex_mapped.pData + i * tex_mapped.RowPitch, data + i * height * 4, height * 4);
        }

        ID3D11DeviceContext_Unmap(d3d11device_context[i], (ID3D11Resource *)ctx->d3d_tex[i], 0);
    }

    d3d11_draw_screen(i, screen_top_bot, p, vertices, (ID3D11Resource *)ctx->d3d_tex[i], ctx->d3d_srv[i]);
}

static ID3D11Texture2D *d3d_ui_tex;
static ID3D11RenderTargetView *d3d_ui_rtv;
static ID3D11ShaderResourceView *d3d_ui_srv;

int d3d11_ui_init(void)
{
    int i = SCREEN_TOP;

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = ui_win_width_drawable[i];
    tex_desc.Height = ui_win_height_drawable[i];
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    tex_desc.MiscFlags = 0;
    tex_desc.CPUAccessFlags = 0;

    HRESULT hr;
    hr = ID3D11Device_CreateTexture2D(d3d11device[i], &tex_desc, NULL, &d3d_ui_tex);
    if (hr) {
        err_log("CreateTexture2D failed: %d\n", (int)hr);
        return -1;
    } else {
        hr = ID3D11Device_CreateRenderTargetView(d3d11device[i], (ID3D11Resource *)d3d_ui_tex, NULL, &d3d_ui_rtv);
        if (hr) {
            err_log("CreateRenderTargetView failed: %d\n", (int)hr);
            CHECK_AND_RELEASE(d3d_ui_tex);
            return -1;
        } else {
            hr = ID3D11Device_CreateShaderResourceView(d3d11device[i], (ID3D11Resource *)d3d_ui_tex, NULL, &d3d_ui_srv);
            if (hr) {
                err_log("CreateShaderResourceView failed: %d\n", (int)hr);
                CHECK_AND_RELEASE(d3d_ui_rtv);
                CHECK_AND_RELEASE(d3d_ui_tex);
                return -1;
            } else {
                if (nk_d3d11_resize(d3d11device_context[i], ui_win_width_drawable[i], ui_win_height_drawable[i], ui_win_scale[i])) {
                    err_log("nk_d3d11_resize failed\n");
                    CHECK_AND_RELEASE(d3d_ui_srv);
                    CHECK_AND_RELEASE(d3d_ui_rtv);
                    CHECK_AND_RELEASE(d3d_ui_tex);
                    return -1;
                }
            }
        }
    }

    return 0;
}

void d3d11_ui_close(void)
{
    CHECK_AND_RELEASE(d3d_ui_srv);
    CHECK_AND_RELEASE(d3d_ui_rtv);
    CHECK_AND_RELEASE(d3d_ui_tex);
}

void ui_renderer_d3d11_present(int screen_top_bot, int ctx_top_bot, bool win_shared) {
    int i = ctx_top_bot;
    int p = win_shared ? screen_top_bot : i;

    if (is_renderer_csc()) {
        if (!sc_fail[p]) {
            if (p == SCREEN_TOP) {
                int width = ui_win_width_drawable_prev[p];
                int height = ui_win_height_drawable_prev[p];
                struct presentation_buffer_t *bufs = ui_pres_bufs;
                int j = SURFACE_UTIL_UI;
                int index_sc;
                if (presentation_buffer_get(bufs, j, -1, COMPAT_PRESENATTION_BUFFER_COUNT_PER_SCREEN, width, height, &index_sc) != 0) {
                    sc_fail[p] = 1;
                    goto fail;
                }
                struct presentation_buffer_t *buf = &bufs[index_sc];
                ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 1, &d3d_ui_rtv, NULL);
                float clearColor[4] = {};
                ID3D11DeviceContext_ClearRenderTargetView(d3d11device_context[i], d3d_ui_rtv, clearColor);
                nk_d3d11_render(d3d11device_context[i], NK_ANTI_ALIASING_ON, ui_win_scale[i]);
                nk_gui_next = 0;

                ID3D11DeviceContext_IASetPrimitiveTopology(d3d11device_context[i], D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                unsigned vb_stride = sizeof(struct d3d_vertex_t);
                unsigned vb_offset = 0;
                ID3D11DeviceContext_IASetVertexBuffers(d3d11device_context[i], 0, 1, &d3d_vb[i], &vb_stride, &vb_offset);
                ID3D11DeviceContext_IASetIndexBuffer(d3d11device_context[i], d3d_ib[i], DXGI_FORMAT_R32_UINT, 0);
                ID3D11DeviceContext_IASetInputLayout(d3d11device_context[i], d3d_il[i]);
                ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 1, &buf->rtv, NULL);
                ID3D11DeviceContext_OMSetBlendState(d3d11device_context[i], d3d_ui_bs[i], NULL, 0xffffffff);
                ID3D11DeviceContext_VSSetShader(d3d11device_context[i], d3d_vs[i], NULL, 0);
                ID3D11DeviceContext_PSSetShader(d3d11device_context[i], d3d_ps[i], NULL, 0);
                ID3D11DeviceContext_PSSetShaderResources(d3d11device_context[i], 0, 1, &d3d_ui_srv);
                ID3D11DeviceContext_PSSetSamplers(d3d11device_context[i], 0, 1, &d3d_ss_point[i]);

                D3D11_VIEWPORT vp = {.Width = width, .Height = height};
                ID3D11DeviceContext_RSSetViewports(d3d11device_context[i], 1, &vp);
                ID3D11DeviceContext_DrawIndexed(d3d11device_context[i], 6, 0, 0);

                ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 0, NULL, NULL);
                ID3D11ShaderResourceView *ptr_null = NULL;
                ID3D11DeviceContext_PSSetShaderResources(d3d11device_context[i], 0, 1, &ptr_null);

                if (update_hide_ui()) {
                    sc_fail[p] = 1;
                    goto fail;
                }

                if (!ui_hide_nk_windows && ui_buffer_present(buf, width, height)) {
                    sc_fail[p] = 1;
                    goto fail;
                }
            }
            if (presentation_buffer_present(d3d_pres_buf[p], i, screen_top_bot, win_shared, ui_ctx_width[p], ui_ctx_height[p])) {
            }
        }
fail:
        ui_compositor_csc_present(i);
    } else {
        if (!sc_fail[p]) {
            HRESULT hr;

            if (p == SCREEN_TOP) {
                nk_d3d11_render(d3d11device_context[i], NK_ANTI_ALIASING_ON, ui_win_scale[i]);
                nk_gui_next = 0;
            }
            hr = IDXGISwapChain_Present(dxgi_sc[i], 1, 0);
            if (hr) {
                err_log("Present failed: %d\n", (int)hr);
            }

            IUnknown_Release(sc_rtv[i]);
            IUnknown_Release(sc_tex[i]);
        }
        if (i == SCREEN_TOP)
            rp_lock_rel(comp_lock);
    }

    if (sc_fail[p]) {
        Sleep(REST_EVERY_MS);
        sc_fail[p] = 0;
    }
}
