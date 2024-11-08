#include "ui_compositor_csc.h"
#include "ui_common_sdl.h"
#include "ui_renderer_d3d11.h"
#include "ui_main_nk.h"
#include "main.h"

static SDL_Window *sdl_win[SCREEN_COUNT];
static struct nk_context *nk_ctx;

#include "nuklear_d3d11.h"

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

    err_log("d3d11 %s\n", is_renderer_csc() ? "composition swapchain" : "");

    return 0;
}

static void d3d11_renderer_destroy(void) {
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

void ui_renderer_d3d11_main(int screen_top_bot, int ctx_top_bot, view_mode_t view_mode, bool win_shared, float bg[4]) {
    int i = ctx_top_bot;
    HRESULT hr;

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
        d3d_rtv[screen_top_bot] = d3d_pres_buf[p]->rtv;
        ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 1, &d3d_rtv[screen_top_bot], NULL);
    } else {
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

        d3d_rtv[screen_top_bot] = sc_rtv[i];
        ID3D11DeviceContext_OMSetRenderTargets(d3d11device_context[i], 1, &d3d_rtv[screen_top_bot], NULL);
    }

    D3D11_VIEWPORT vp = { .Width = ui_ctx_width[p], .Height = ui_ctx_height[p] };
    ID3D11DeviceContext_RSSetViewports(d3d11device_context[i], 1, &vp);
    if (!win_shared) {
        ID3D11DeviceContext_ClearRenderTargetView(d3d11device_context[i], d3d_rtv[screen_top_bot], bg);
    }

    if (view_mode == VIEW_MODE_TOP_BOT && !win_shared) {
        draw_screen(&rp_buffer_ctx[SCREEN_TOP], SCREEN_HEIGHT0, SCREEN_WIDTH, SCREEN_TOP, i, view_mode, 0);
        draw_screen(&rp_buffer_ctx[SCREEN_BOT], SCREEN_HEIGHT1, SCREEN_WIDTH, SCREEN_BOT, i, view_mode, 0);
    } else if (view_mode == VIEW_MODE_BOT) {
        draw_screen(&rp_buffer_ctx[SCREEN_BOT], SCREEN_HEIGHT1, SCREEN_WIDTH, SCREEN_BOT, i, view_mode, win_shared);
    } else {
        if (!draw_screen(&rp_buffer_ctx[screen_top_bot], screen_top_bot == SCREEN_TOP ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1, SCREEN_WIDTH, screen_top_bot, i, view_mode, win_shared)) {
            ID3D11DeviceContext_ClearRenderTargetView(d3d11device_context[i], d3d_rtv[screen_top_bot], bg);
        }
    }
}

static void d3d11_draw_screen(int ctx_top_bot, int screen_top_bot, struct d3d_vertex_t *vertices)
{
    int i = ctx_top_bot;
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
    ID3D11DeviceContext_DrawIndexed(d3d11device_context[i], 6, 0, 0);
}

void ui_renderer_d3d11_draw(struct rp_buffer_ctx_t *ctx, uint8_t *data, int width, int height, int screen_top_bot, int ctx_top_bot, int index, view_mode_t view_mode, int win_shared) {
    double ctx_left_f;
    double ctx_top_f;
    double ctx_right_f;
    double ctx_bot_f;
    int ctx_width;
    int ctx_height;
    int win_width_drawable;
    int win_height_drawable;
    bool upscaled;
    draw_screen_get_dims(
        screen_top_bot, ctx_top_bot, win_shared, view_mode, width, height,
        &ctx_left_f, &ctx_top_f, &ctx_right_f, &ctx_bot_f, &ctx_width, &ctx_height, &win_width_drawable, &win_height_drawable, &upscaled);

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

    if (upscaled) {
        // TODO
        (void)index;
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

    ID3D11DeviceContext_PSSetShaderResources(d3d11device_context[i], 0, 1, &ctx->d3d_srv[i]);
    d3d11_draw_screen(i, screen_top_bot, vertices);
    ID3D11ShaderResourceView *ptr_null = NULL;
    ID3D11DeviceContext_PSSetShaderResources(d3d11device_context[i], 0, 1, &ptr_null);
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
    }

    if (sc_fail[p]) {
        Sleep(REST_EVERY_MS);
        sc_fail[p] = 0;
    }
}
