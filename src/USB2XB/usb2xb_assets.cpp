/*---------------------------------------------------------------------------
    usb2xb_assets.cpp -- UI image assets for USB2XB.

    D:\dat\logo.dat  = PNG logo
    D:\dat\sd.dat    = PNG 480-line background
    D:\dat\hd.dat    = PNG 720p background

    The .dat extension is intentional; stb_image decodes from PNG magic bytes.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <xgraphics.h>
#include <stdlib.h>
#include <string.h>

#include "dd_gfx.h"
#include "dd_ui.h"
#include "usb2xb_stbi.h"
#include "usb2xb_assets.h"

typedef struct
{
    IDirect3DTexture8* tex;
    int w;
    int h;
    int pw;
    int ph;
} U2xTexture;

typedef struct
{
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
} U2xAssetVert;

#define U2X_ASSET_FVF \
    (D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1)

static U2xTexture s_bg;
static U2xTexture s_logo;
static int s_init = 0;


static unsigned u2x_np2(unsigned v)
{
    unsigned p = 1;

    while (p < v)
        p <<= 1;

    return p;
}


static void tex_zero(U2xTexture* t)
{
    if (!t)
        return;

    t->tex = 0;
    t->w = 0;
    t->h = 0;
    t->pw = 0;
    t->ph = 0;
}


static int upload_rgba(
    const unsigned char* rgba,
    unsigned w,
    unsigned h,
    U2xTexture* out)
{
    unsigned pw;
    unsigned ph;
    unsigned x;
    unsigned y;
    unsigned char* pad;
    IDirect3DTexture8* tex = 0;
    D3DLOCKED_RECT lr;
    HRESULT hr;

    if (!rgba || !out || !w || !h)
        return 0;

    pw = u2x_np2(w);
    ph = u2x_np2(h);

    pad = (unsigned char*)malloc(
        (size_t)pw * ph * 4);

    if (!pad)
        return 0;

    memset(
        pad,
        0,
        (size_t)pw * ph * 4);

    /*
        stb gives RGBA. Xbox A8R8G8B8 upload memory is BGRA byte order.
    */
    for (y = 0; y < h; ++y)
    {
        const unsigned char* src =
            rgba + (size_t)y * w * 4;

        unsigned char* dst =
            pad + (size_t)y * pw * 4;

        for (x = 0; x < w; ++x)
        {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }

    hr = Gfx_Device()->CreateTexture(
        pw,
        ph,
        1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &tex);

    if (FAILED(hr))
    {
        free(pad);
        return 0;
    }

    hr = tex->LockRect(
        0,
        &lr,
        0,
        0);

    if (FAILED(hr))
    {
        tex->Release();
        free(pad);
        return 0;
    }

    XGSwizzleRect(
        pad,
        pw * 4,
        0,
        lr.pBits,
        pw,
        ph,
        0,
        4);

    tex->UnlockRect(0);
    free(pad);

    out->tex = tex;
    out->w = (int)w;
    out->h = (int)h;
    out->pw = (int)pw;
    out->ph = (int)ph;

    return 1;
}


static int upload_logo_rgba_trimmed(
    const unsigned char* rgba,
    unsigned w,
    unsigned h,
    U2xTexture* out)
{
    unsigned minX = w;
    unsigned minY = h;
    unsigned maxX = 0;
    unsigned maxY = 0;
    unsigned x;
    unsigned y;
    int found = 0;
    unsigned char* crop;
    unsigned cw;
    unsigned ch;

    if (!rgba || !out || !w || !h)
        return 0;

    /*
        logo.dat may be generated on a much larger black canvas.  Detect the
        visible badge instead of treating the whole PNG bounds as artwork.
        A deliberately low threshold keeps dark gunmetal/black badge pixels
        while discarding the empty near-black surround.
    */
    for (y = 0; y < h; ++y)
    {
        const unsigned char* row =
            rgba + (size_t)y * w * 4;

        for (x = 0; x < w; ++x)
        {
            unsigned char r = row[x * 4 + 0];
            unsigned char g = row[x * 4 + 1];
            unsigned char b = row[x * 4 + 2];
            unsigned char a = row[x * 4 + 3];
            unsigned char hi = r;

            if (g > hi)hi = g;
            if (b > hi)hi = b;

            if (a > 16 && hi > 14)
            {
                if (!found)
                {
                    minX = maxX = x;
                    minY = maxY = y;
                    found = 1;
                }
                else
                {
                    if (x < minX)minX = x;
                    if (x > maxX)maxX = x;
                    if (y < minY)minY = y;
                    if (y > maxY)maxY = y;
                }
            }
        }
    }

    if (!found)
        return upload_rgba(rgba, w, h, out);

    /*
        Small safety pad around the detected artwork so glows/rounded edges
        are not clipped.
    */
    if (minX > 4)minX -= 4; else minX = 0;
    if (minY > 4)minY -= 4; else minY = 0;

    if (maxX + 4 < w)maxX += 4; else maxX = w - 1;
    if (maxY + 4 < h)maxY += 4; else maxY = h - 1;

    cw = maxX - minX + 1;
    ch = maxY - minY + 1;

    crop = (unsigned char*)malloc(
        (size_t)cw * ch * 4);

    if (!crop)
        return 0;

    for (y = 0; y < ch; ++y)
    {
        memcpy(
            crop + (size_t)y * cw * 4,
            rgba +
            ((size_t)(minY + y) * w + minX) * 4,
            (size_t)cw * 4);
    }

    {
        int ok = upload_rgba(
            crop,
            cw,
            ch,
            out);

        free(crop);
        return ok;
    }
}


static int load_png_dat(
    const char* path,
    U2xTexture* out,
    int trimLogo)
{
    HANDLE h;
    DWORD size;
    DWORD got = 0;
    unsigned char* file = 0;
    unsigned char* rgba = 0;
    int w = 0;
    int hh = 0;
    int ok = 0;

    if (!path || !out)
        return 0;

    tex_zero(out);

    h = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        0);

    if (h == INVALID_HANDLE_VALUE)
        return 0;

    size = GetFileSize(h, 0);

    if (size == 0 ||
        size == 0xFFFFFFFF)
    {
        CloseHandle(h);
        return 0;
    }

    file = (unsigned char*)malloc(size);

    if (!file)
    {
        CloseHandle(h);
        return 0;
    }

    if (!ReadFile(
        h,
        file,
        size,
        &got,
        0) ||
        got != size)
    {
        free(file);
        CloseHandle(h);
        return 0;
    }

    CloseHandle(h);

    rgba = USB2XB_StbLoadImageMem(
        file,
        (int)size,
        &w,
        &hh);

    free(file);

    if (!rgba)
        return 0;

    if (trimLogo)
    {
        ok = upload_logo_rgba_trimmed(
            rgba,
            (unsigned)w,
            (unsigned)hh,
            out);
    }
    else
    {
        ok = upload_rgba(
            rgba,
            (unsigned)w,
            (unsigned)hh,
            out);
    }

    USB2XB_StbFree(rgba);
    return ok;
}


static void draw_tex(
    U2xTexture* t,
    float x,
    float y,
    float w,
    float h,
    DWORD colour,
    int pointFilter)
{
    IDirect3DDevice8* d = Gfx_Device();
    U2xAssetVert v[4];
    float x0;
    float y0;
    float x1;
    float y1;
    float umax;
    float vmax;

    if (!d ||
        !t ||
        !t->tex ||
        t->pw <= 0 ||
        t->ph <= 0)
    {
        return;
    }

    x0 = UI_Sx(x);
    y0 = UI_Sy(y);
    x1 = UI_Sx(x + w);
    y1 = UI_Sy(y + h);

    umax = (float)t->w / (float)t->pw;
    vmax = (float)t->h / (float)t->ph;

    v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].rhw = 1;
    v[0].colour = colour; v[0].u = 0;    v[0].v = 0;

    v[1].x = x1; v[1].y = y0; v[1].z = 0; v[1].rhw = 1;
    v[1].colour = colour; v[1].u = umax; v[1].v = 0;

    v[2].x = x0; v[2].y = y1; v[2].z = 0; v[2].rhw = 1;
    v[2].colour = colour; v[2].u = 0;    v[2].v = vmax;

    v[3].x = x1; v[3].y = y1; v[3].z = 0; v[3].rhw = 1;
    v[3].colour = colour; v[3].u = umax; v[3].v = vmax;

    d->SetTexture(0, t->tex);

    d->SetRenderState(
        D3DRS_ALPHABLENDENABLE,
        TRUE);

    d->SetRenderState(
        D3DRS_SRCBLEND,
        D3DBLEND_SRCALPHA);

    d->SetRenderState(
        D3DRS_DESTBLEND,
        D3DBLEND_INVSRCALPHA);

    d->SetRenderState(
        D3DRS_ZENABLE,
        FALSE);

    d->SetRenderState(
        D3DRS_LIGHTING,
        FALSE);

    d->SetTextureStageState(
        0,
        D3DTSS_COLOROP,
        D3DTOP_MODULATE);

    d->SetTextureStageState(
        0,
        D3DTSS_COLORARG1,
        D3DTA_TEXTURE);

    d->SetTextureStageState(
        0,
        D3DTSS_COLORARG2,
        D3DTA_DIFFUSE);

    d->SetTextureStageState(
        0,
        D3DTSS_ALPHAOP,
        D3DTOP_MODULATE);

    d->SetTextureStageState(
        0,
        D3DTSS_ALPHAARG1,
        D3DTA_TEXTURE);

    d->SetTextureStageState(
        0,
        D3DTSS_ALPHAARG2,
        D3DTA_DIFFUSE);

    d->SetTextureStageState(
        0,
        D3DTSS_MINFILTER,
        pointFilter ?
        D3DTEXF_POINT :
        D3DTEXF_LINEAR);

    d->SetTextureStageState(
        0,
        D3DTSS_MAGFILTER,
        pointFilter ?
        D3DTEXF_POINT :
        D3DTEXF_LINEAR);

    d->SetVertexShader(U2X_ASSET_FVF);

    d->DrawPrimitiveUP(
        D3DPT_TRIANGLESTRIP,
        2,
        v,
        sizeof(U2xAssetVert));

    d->SetTexture(0, 0);

    d->SetRenderState(
        D3DRS_ALPHABLENDENABLE,
        FALSE);

    d->SetRenderState(
        D3DRS_ZENABLE,
        TRUE);
}


void USB2XB_AssetsInit(void)
{
    const char* bgPath;

    if (s_init)
        return;

    s_init = 1;

    tex_zero(&s_bg);
    tex_zero(&s_logo);

    bgPath =
        Gfx_Height() >= 720 ?
        "D:\\dat\\hd.dat" :
        "D:\\dat\\sd.dat";

    load_png_dat(
        bgPath,
        &s_bg,
        0);

    load_png_dat(
        "D:\\dat\\logo.dat",
        &s_logo,
        1);
}


void USB2XB_AssetsDrawBackground(void)
{
    if (!s_init)
        USB2XB_AssetsInit();

    if (!s_bg.tex)
        return;

    draw_tex(
        &s_bg,
        0.0f,
        0.0f,
        UI_Width(),
        UI_Height(),
        0xFFFFFFFF,
        0);
}


void USB2XB_AssetsDrawLogo(void)
{
    /*
        Fill the header title area rather than fitting into the old text's
        literal glyph box.  The logo is cropped to its visible artwork at load
        time, then scaled proportionally here.

        Logical dimensions automatically scale to the physical output through
        UI_Sx/UI_Sy:
          480-line modes -> native logical size
          720p           -> ~1.5x physical size
    */
    float targetH = 42.0f;
    float maxW = 178.0f;
    float drawW;
    float drawH;
    float x = 18.0f;
    float y;

    if (!s_init)
        USB2XB_AssetsInit();

    if (!s_logo.tex ||
        s_logo.w <= 0 ||
        s_logo.h <= 0)
    {
        return;
    }

    drawH = targetH;
    drawW =
        ((float)s_logo.w /
            (float)s_logo.h) *
        drawH;

    if (drawW > maxW)
    {
        drawW = maxW;
        drawH =
            ((float)s_logo.h /
                (float)s_logo.w) *
            drawW;
    }

    /*
        Vertically center inside the 58px header.  A 42px target height makes
        the badge visually dominant without colliding with the header rails.
    */
    y = (58.0f - drawH) * 0.5f;

    draw_tex(
        &s_logo,
        x,
        y,
        drawW,
        drawH,
        0xFFFFFFFF,
        1);
}
