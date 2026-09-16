/*---------------------------------------------------------------------------
    dd_gfx.cpp -- USB2XB D3D8 bring-up.

    Based on DarkDash's proven device/frame ownership. Dashboard-only config
    and watchdog dependencies were deliberately removed from the base package.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_gfx.h"

static IDirect3D8* s_d3d = NULL;
static IDirect3DDevice8* s_device = NULL;
static int               s_width = 640;
static int               s_height = 480;
static int               s_widescreen = 0;
static char              s_videoMode[8] = "480i";

static void GfxSetMode(const char* m)
{
    int i = 0;
    while (m[i] && i < (int)sizeof(s_videoMode) - 1) {
        s_videoMode[i] = m[i];
        ++i;
    }
    s_videoMode[i] = 0;
}


/*
    Prefer Xbox 2x linear multisampling. If a video/depth combination rejects
    it, retry the exact same mode without AA before the normal resolution
    fallback path is allowed to run.
*/
static HRESULT GfxCreateDeviceWithAA(D3DPRESENT_PARAMETERS* pp)
{
    HRESULT hr;

    if (!pp || !s_d3d)
        return E_FAIL;

    pp->MultiSampleType =
        D3DMULTISAMPLE_2_SAMPLES_MULTISAMPLE_LINEAR;

    hr = s_d3d->CreateDevice(
        0,
        D3DDEVTYPE_HAL,
        NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        pp,
        &s_device);

    if (FAILED(hr)) {
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;

        hr = s_d3d->CreateDevice(
            0,
            D3DDEVTYPE_HAL,
            NULL,
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            pp,
            &s_device);
    }

    return hr;
}


int Gfx_Init(void)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    DWORD vflags;
    DWORD vstd;
    int palI;
    int pal60;
    int has480p;
    int has720;

    if (s_device) return 1;

    s_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!s_d3d) return 0;

    ZeroMemory(&pp, sizeof(pp));

    vflags = XGetVideoFlags();
    vstd = XGetVideoStandard();

    palI = (vstd == XC_VIDEO_STANDARD_PAL_I) ? 1 : 0;
    pal60 = (vflags & XC_VIDEO_FLAGS_PAL_60Hz) ? 1 : 0;
    has480p = (vflags & XC_VIDEO_FLAGS_HDTV_480p) ? 1 : 0;
    has720 = (vflags & XC_VIDEO_FLAGS_HDTV_720p) ? 1 : 0;

    pp.FullScreen_RefreshRateInHz = 60;

    /*
       Keep DarkDash's safe mode ordering for now:
         720p -> 480p -> PAL 576i -> NTSC/PAL60 480i.
       USB2XB can later add a preference setting if it actually needs one.
    */
    if (has720) {
        pp.BackBufferWidth = 1280;
        pp.BackBufferHeight = 720;
        pp.Flags = D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
        GfxSetMode("720p");
    }
    else if (has480p) {
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = 480;
        pp.Flags = D3DPRESENTFLAG_PROGRESSIVE;
        if (vflags & XC_VIDEO_FLAGS_WIDESCREEN)
            pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;
        GfxSetMode("480p");
    }
    else if (palI && !pal60) {
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = 576;
        pp.Flags = D3DPRESENTFLAG_INTERLACED;
        if (vflags & XC_VIDEO_FLAGS_WIDESCREEN)
            pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;
        pp.FullScreen_RefreshRateInHz = 50;
        GfxSetMode("576i");
    }
    else {
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = 480;
        pp.Flags = D3DPRESENTFLAG_INTERLACED;
        if (vflags & XC_VIDEO_FLAGS_WIDESCREEN)
            pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;
        GfxSetMode("480i");
    }

    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    hr = GfxCreateDeviceWithAA(&pp);

    /* Same idea as DarkDash: never turn a rejected HD mode into a black screen. */
    if (FAILED(hr)) {
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = (palI && !pal60) ? 576 : 480;
        pp.Flags = D3DPRESENTFLAG_INTERLACED;
        if (vflags & XC_VIDEO_FLAGS_WIDESCREEN)
            pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;
        pp.FullScreen_RefreshRateInHz = (palI && !pal60) ? 50 : 60;
        GfxSetMode((palI && !pal60) ? "576i" : "480i");

        hr = GfxCreateDeviceWithAA(&pp);
    }

    if (FAILED(hr)) {
        s_d3d->Release();
        s_d3d = NULL;
        return 0;
    }

    s_width = pp.BackBufferWidth;
    s_height = pp.BackBufferHeight;
    s_widescreen =
        (pp.Flags & D3DPRESENTFLAG_WIDESCREEN) ? 1 : 0;

    s_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    s_device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    return 1;
}

void Gfx_Shutdown(void)
{
    if (s_device) {
        s_device->Release();
        s_device = NULL;
    }
    if (s_d3d) {
        s_d3d->Release();
        s_d3d = NULL;
    }
}

IDirect3DDevice8* Gfx_Device(void) { return s_device; }
int Gfx_Width(void) { return s_width; }
int Gfx_Height(void) { return s_height; }
int Gfx_IsWidescreen(void) { return s_widescreen; }
const char* Gfx_VideoModeStr(void) { return s_videoMode; }

void Gfx_BeginFrame(DWORD clearColour)
{
    if (!s_device) return;

    s_device->Clear(
        0, NULL,
        D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        clearColour,
        1.0f,
        0);

    s_device->BeginScene();
}

void Gfx_EndFrame(void)
{
    if (!s_device) return;
    s_device->EndScene();
    s_device->Present(NULL, NULL, NULL, NULL);
}
