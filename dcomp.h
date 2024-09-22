#ifndef RP_DCOMP_H
#define RP_DCOMP_H

#define COBJMACROS
#include <d3d11.h>
#include <d2d1_1.h>
#include "Presentation.h"
#include "dcomptypes.h"
#define COMPOSITIONSURFACE_READ 0x0001L
#define COMPOSITIONSURFACE_WRITE 0x0002L
#define COMPOSITIONSURFACE_ALL_ACCESS (COMPOSITIONSURFACE_READ | COMPOSITIONSURFACE_WRITE)

STDAPI DCompositionCreateDevice(IDXGIDevice *dxgiDevice, REFIID iid, void **dcompositionDevice);
STDAPI DCompositionCreateSurfaceHandle(DWORD desiredAccess, SECURITY_ATTRIBUTES *securityAttributes, HANDLE *surfaceHandle);
STDAPI DCompositionGetStatistics(COMPOSITION_FRAME_ID frameId, COMPOSITION_FRAME_STATS *frameStats, UINT targetIdCount, COMPOSITION_TARGET_ID *targetIds, UINT *actualTargetIdCount);
STDAPI DCompositionGetTargetStatistics(COMPOSITION_FRAME_ID frameId, const COMPOSITION_TARGET_ID *targetId, COMPOSITION_TARGET_STATS *targetStats);

const IID IID_IPresentationFactory = { 0x8fb37b58, 0x1d74, 0x4f64, { 0xa4, 0x9c, 0x1f, 0x97, 0xa8, 0x0a, 0x2e, 0xc0 } };
const IID IID_IDCompositionDevice = { 0xc37ea93a, 0xe7aa, 0x450d, { 0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3 } };

typedef interface IDCompositionAffineTransform2DEffect   IDCompositionAffineTransform2DEffect;
typedef interface IDCompositionAnimation                 IDCompositionAnimation;
typedef interface IDCompositionArithmeticCompositeEffect IDCompositionArithmeticCompositeEffect;
typedef interface IDCompositionBlendEffect               IDCompositionBlendEffect;
typedef interface IDCompositionBrightnessEffect          IDCompositionBrightnessEffect;
typedef interface IDCompositionClip                      IDCompositionClip;
typedef interface IDCompositionColorMatrixEffect         IDCompositionColorMatrixEffect;
typedef interface IDCompositionCompositeEffect           IDCompositionCompositeEffect;
typedef interface IDCompositionDevice                    IDCompositionDevice;
typedef interface IDCompositionEffect                    IDCompositionEffect;
typedef interface IDCompositionEffectGroup               IDCompositionEffectGroup;
typedef interface IDCompositionFilterEffect              IDCompositionFilterEffect;
typedef interface IDCompositionGaussianBlurEffect        IDCompositionGaussianBlurEffect;
typedef interface IDCompositionHueRotationEffect         IDCompositionHueRotationEffect;
typedef interface IDCompositionLinearTransferEffect      IDCompositionLinearTransferEffect;
typedef interface IDCompositionMatrixTransform           IDCompositionMatrixTransform;
typedef interface IDCompositionMatrixTransform3D         IDCompositionMatrixTransform3D;
typedef interface IDCompositionRectangleClip             IDCompositionRectangleClip;
typedef interface IDCompositionRotateTransform           IDCompositionRotateTransform;
typedef interface IDCompositionRotateTransform3D         IDCompositionRotateTransform3D;
typedef interface IDCompositionSaturationEffect          IDCompositionSaturationEffect;
typedef interface IDCompositionScaleTransform            IDCompositionScaleTransform;
typedef interface IDCompositionScaleTransform3D          IDCompositionScaleTransform3D;
typedef interface IDCompositionShadowEffect              IDCompositionShadowEffect;
typedef interface IDCompositionSkewTransform             IDCompositionSkewTransform;
typedef interface IDCompositionSurface                   IDCompositionSurface;
typedef interface IDCompositionTableTransferEffect       IDCompositionTableTransferEffect;
typedef interface IDCompositionTarget                    IDCompositionTarget;
typedef interface IDCompositionTransform                 IDCompositionTransform;
typedef interface IDCompositionTransform3D               IDCompositionTransform3D;
typedef interface IDCompositionTranslateTransform        IDCompositionTranslateTransform;
typedef interface IDCompositionTranslateTransform3D      IDCompositionTranslateTransform3D;
typedef interface IDCompositionTurbulenceEffect          IDCompositionTurbulenceEffect;
typedef interface IDCompositionVirtualSurface            IDCompositionVirtualSurface;
typedef interface IDCompositionVisual                    IDCompositionVisual;

#define IUNKNOWN_METHODS \
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppvObject); \
    STDMETHOD_(ULONG, AddRef)(THIS); \
    STDMETHOD_(ULONG, Release)(THIS);

#undef INTERFACE
#define INTERFACE IDCompositionDevice
DECLARE_INTERFACE(IDCompositionDevice)
{
    IUNKNOWN_METHODS
    STDMETHOD(Commit)(THIS);
    STDMETHOD(WaitForCommitCompletion)(THIS);
    STDMETHOD(GetFrameStatistics)(THIS_ DCOMPOSITION_FRAME_STATISTICS *statistics);
    STDMETHOD(CreateTargetForHwnd)(THIS_ HWND hwnd, BOOL topmost, IDCompositionTarget **target);
    STDMETHOD(CreateVisual)(THIS_ IDCompositionVisual **visual);
    STDMETHOD(CreateSurface)(THIS_ UINT width, UINT height, DXGI_FORMAT pixelFormat, DXGI_ALPHA_MODE alphaMode, IDCompositionSurface **surface);
    STDMETHOD(CreateVirtualSurface)(THIS_ UINT initialWidth, UINT initialHeight, DXGI_FORMAT pixelFormat, DXGI_ALPHA_MODE alphaMode, IDCompositionVirtualSurface **virtualSurface);
    STDMETHOD(CreateSurfaceFromHandle)(THIS_ HANDLE handle, IUnknown **surface);
    STDMETHOD(CreateSurfaceFromHwnd)(THIS_ HWND hwnd, IUnknown **surface);
    STDMETHOD(CreateTranslateTransform)(THIS_ IDCompositionTranslateTransform **translateTransform);
    STDMETHOD(CreateScaleTransform)(THIS_ IDCompositionScaleTransform **scaleTransform);
    STDMETHOD(CreateRotateTransform)(THIS_ IDCompositionRotateTransform **rotateTransform);
    STDMETHOD(CreateSkewTransform)(THIS_ IDCompositionSkewTransform **skewTransform);
    STDMETHOD(CreateMatrixTransform)(THIS_ IDCompositionMatrixTransform **matrixTransform);
    STDMETHOD(CreateTransformGroup)(THIS_ IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transformGroup);
    STDMETHOD(CreateTranslateTransform3D)(THIS_ IDCompositionTranslateTransform3D **translateTransform3D);
    STDMETHOD(CreateScaleTransform3D)(THIS_ IDCompositionScaleTransform3D **scaleTransform3D);
    STDMETHOD(CreateRotateTransform3D)(THIS_ IDCompositionRotateTransform3D **rotateTransform3D);
    STDMETHOD(CreateMatrixTransform3D)(THIS_ IDCompositionMatrixTransform3D **matrixTransform3D);
    STDMETHOD(CreateTransform3DGroup)(THIS_ IDCompositionTransform3D **transforms3D, UINT elements, IDCompositionTransform3D **transform3DGroup);
    STDMETHOD(CreateEffectGroup)(THIS_ IDCompositionEffectGroup **effectGroup);
    STDMETHOD(CreateRectangleClip)(THIS_ IDCompositionRectangleClip **clip);
    STDMETHOD(CreateAnimation)(THIS_ IDCompositionAnimation **animation);
    STDMETHOD(CheckDeviceState)(THIS_ BOOL *pfValid);
};

#undef INTERFACE
#define INTERFACE IDCompositionVisual
DECLARE_INTERFACE(IDCompositionVisual)
{
    IUNKNOWN_METHODS
    STDMETHOD(SetOffsetX1)(THIS_ IDCompositionAnimation*);
    STDMETHOD(SetOffsetX2)(THIS_ float);
    STDMETHOD(SetOffsetY1)(THIS_ IDCompositionAnimation*);
    STDMETHOD(SetOffsetY2)(THIS_ float);
    STDMETHOD(SetTransform1)(THIS_ IDCompositionTransform*);
    STDMETHOD(SetTransform2)(THIS_ D2D_MATRIX_3X2_F*);
    STDMETHOD(SetTransformParent)(THIS_ IDCompositionVisual*);
    STDMETHOD(SetEffect)(THIS_ IDCompositionEffect*);
    STDMETHOD(SetBitmapInterpolationMode)(THIS_ enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE);
    STDMETHOD(SetBorderMode)(THIS_ enum DCOMPOSITION_BORDER_MODE);
    STDMETHOD(SetClip1)(THIS_ IDCompositionClip*);
    STDMETHOD(SetClip2)(THIS_ D2D_RECT_F*);
    STDMETHOD(SetContent)(THIS_ IUnknown*);
    STDMETHOD(AddVisual)(THIS_ IDCompositionVisual*,BOOL,IDCompositionVisual*);
    STDMETHOD(RemoveVisual)(THIS_ IDCompositionVisual*);
    STDMETHOD(RemoveAllVisuals)(THIS);
    STDMETHOD(SetCompositeMode)(THIS_ enum DCOMPOSITION_COMPOSITE_MODE);
};

#undef INTERFACE
#define INTERFACE IDCompositionTarget
DECLARE_INTERFACE(IDCompositionTarget)
{
    IUNKNOWN_METHODS
    STDMETHOD(SetRoot)(THIS_ IDCompositionVisual*);
};

#endif
