/*---------------------------------------------------------------------------
    font.cpp -- USB2XB font renderer, retaining DarkDash's atlas/UI/iso model.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <xgraphics.h>
#include "font.h"
#include "font_atlas.h"
#include "dd_ui.h"

static IDirect3DTexture8* s_tex = NULL;
static const GlyphMetrics* s_metrics[3] =
{
    g_glyphsSmall, g_glyphsMedium, g_glyphsLarge
};

#define FONT_FVF     (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FONT_ISO_FVF (D3DFVF_XYZ    | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
} FontVert;

typedef struct {
    float x, y, z;
    DWORD colour;
    float u, v;
} FontIsoVert;

static int FontSizePx(int size)
{
    if (size == FONT_SIZE_LARGE) return FONT_LARGE_SIZE;
    if (size == FONT_SIZE_MEDIUM) return FONT_MEDIUM_SIZE;
    return FONT_SMALL_SIZE;
}

int Font_Init(IDirect3DDevice8* d)
{
    D3DLOCKED_RECT lr;
    HRESULT hr;

    if (s_tex) return 1;
    if (!d) return 0;

    hr = d->CreateTexture(FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_tex);
    if (FAILED(hr)) {
        s_tex = NULL;
        return 0;
    }

    hr = s_tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) {
        s_tex->Release();
        s_tex = NULL;
        return 0;
    }

    XGSwizzleRect((void*)g_fontAtlasData, FONT_ATLAS_WIDTH * 4, NULL,
        lr.pBits, FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT, NULL, 4);
    s_tex->UnlockRect(0);
    return 1;
}

void Font_Shutdown(void)
{
    if (s_tex) {
        s_tex->Release();
        s_tex = NULL;
    }
}

int Font_MeasureText(const char* str, int size)
{
    int w = 0;
    unsigned char c;

    if (!str || size < 0 || size > 2) return 0;
    while ((c = (unsigned char)*str++) != 0) {
        if (c >= 32 && c <= 126)
            w += s_metrics[size][c - 32].advance;
    }
    return w;
}

int Font_GlyphHeight(int size)
{
    return FontSizePx(size);
}

int Font_LineHeight(int size)
{
    int h = FontSizePx(size);
    return h + h / 5 + 2;
}

static void FontStates(IDirect3DDevice8* d, DWORD fvf)
{
    d->SetTexture(0, s_tex);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    /*
        Native-size 480-line rendering is sharper with point filtering.
        720p scales the atlas ~1.5x, where linear filtering avoids uneven
        stair-stepping while remaining substantially sharper than ISO text.
    */
    if (UI_ScaleY(1.0f) <= 1.01f)
    {
        d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    }
    else
    {
        d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    }
    d->SetVertexShader(fvf);
}

void Font_DrawText(IDirect3DDevice8* d,
    float x, float y, const char* str, int size, DWORD colour, int max_w)
{
    float cur, end;
    unsigned char c;

    if (!d || !s_tex || !str || size < 0 || size > 2) return;

    FontStates(d, FONT_FVF);
    cur = UI_Sx(x);
    end = (max_w > 0) ? UI_Sx(x + (float)max_w) : 100000.0f;

    while ((c = (unsigned char)*str++) != 0) {
        const GlyphMetrics* gm;
        FontVert v[4];
        float gw, gh, gx, gy, u0, v0, u1, v1, adv;

        if (c < 32 || c > 126) continue;
        gm = &s_metrics[size][c - 32];
        adv = UI_ScaleX((float)gm->advance);

        if (cur + adv > end) break;

        gw = UI_ScaleX((float)gm->w);
        gh = UI_ScaleY((float)gm->h);
        gx = cur;

        /*
            The atlas stores the glyph's vertical bearing.  The old renderer
            ignored it and placed every bitmap at the same top Y, which made
            lowercase, capitals, punctuation and descenders sit on different
            visual baselines.  Apply bear_y inside the design-size line box.
        */
        gy = UI_Sy(y + (float)gm->bear_y);

        u0 = (float)gm->x / (float)FONT_ATLAS_WIDTH;
        v0 = (float)gm->y / (float)FONT_ATLAS_HEIGHT;
        u1 = (float)(gm->x + gm->w) / (float)FONT_ATLAS_WIDTH;
        v1 = (float)(gm->y + gm->h) / (float)FONT_ATLAS_HEIGHT;

        v[0].x = gx;    v[0].y = gy;    v[0].z = 0; v[0].rhw = 1; v[0].colour = colour; v[0].u = u0; v[0].v = v0;
        v[1].x = gx + gw; v[1].y = gy;    v[1].z = 0; v[1].rhw = 1; v[1].colour = colour; v[1].u = u1; v[1].v = v0;
        v[2].x = gx;    v[2].y = gy + gh; v[2].z = 0; v[2].rhw = 1; v[2].colour = colour; v[2].u = u0; v[2].v = v1;
        v[3].x = gx + gw; v[3].y = gy + gh; v[3].z = 0; v[3].rhw = 1; v[3].colour = colour; v[3].u = u1; v[3].v = v1;

        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(FontVert));
        cur += adv;
    }

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void Font_DrawTextCentered(IDirect3DDevice8* d,
    float cx, float y, float width, const char* str, int size, DWORD colour)
{
    int w = Font_MeasureText(str, size);
    /*
        Centering does not need a clipping cast. Avoid a runtime float->int
        conversion here so RXDK never pulls in __ftol2_sse for this helper.
    */
    Font_DrawText(d, cx + (width - (float)w) * 0.5f, y,
        str, size, colour, 0);
}

void Font_DrawTextRight(IDirect3DDevice8* d,
    float x, float y, const char* str, int size, DWORD colour)
{
    int w = Font_MeasureText(str, size);
    Font_DrawText(d, x - (float)w, y, str, size, colour, 0);
}

void Font_DrawTextEllipsis(
    IDirect3DDevice8* d,
    float x,
    float y,
    const char* str,
    int size,
    DWORD colour,
    int max_w)
{
    char tmp[260];
    const char* dots = "...";
    int dotsW;
    int n = 0;
    int w = 0;

    if (!str || max_w <= 0)
        return;

    if (Font_MeasureText(str, size) <= max_w)
    {
        Font_DrawText(d, x, y, str, size, colour, max_w);
        return;
    }

    dotsW = Font_MeasureText(dots, size);

    while (str[n] && n < (int)sizeof(tmp) - 4)
    {
        unsigned char c = (unsigned char)str[n];
        int adv = 0;

        if (c >= 32 && c <= 126)
            adv = s_metrics[size][c - 32].advance;

        if (w + adv + dotsW > max_w)
            break;

        tmp[n] = str[n];
        w += adv;
        ++n;
    }

    tmp[n++] = '.';
    tmp[n++] = '.';
    tmp[n++] = '.';
    tmp[n] = 0;

    Font_DrawText(d, x, y, tmp, size, colour, max_w);
}


static void Font_DrawIsoInternal(IDirect3DDevice8* d,
    float vx, float vy, const char* str, int size, DWORD colour, float max_w)
{
    float cx = UI_VIRT_W * 0.5f;
    float cy = UI_VIRT_H * 0.5f;
    float cur = vx;
    float end = (max_w > 0.0f) ? vx + max_w : 100000.0f;
    unsigned char c;

    if (!d || !s_tex || !str || size < 0 || size > 2) return;

    FontStates(d, FONT_ISO_FVF);

    while ((c = (unsigned char)*str++) != 0) {
        const GlyphMetrics* gm;
        FontIsoVert v[4];
        float gw, gh, x0, x1, y0, y1, u0, v0, u1, v1;

        if (c < 32 || c > 126) continue;
        gm = &s_metrics[size][c - 32];

        if (cur + (float)gm->advance > end) break;

        gw = (float)gm->w;
        gh = (float)gm->h;
        x0 = cur - cx;
        x1 = cur + gw - cx;

        /*
            Keep ISO text on the same baseline model as screen-space text.
            bear_y is the glyph top offset inside the nominal font-size box.
        */
        y0 = cy - (vy + (float)gm->bear_y);
        y1 = cy - (vy + (float)gm->bear_y + gh);

        u0 = (float)gm->x / (float)FONT_ATLAS_WIDTH;
        v0 = (float)gm->y / (float)FONT_ATLAS_HEIGHT;
        u1 = (float)(gm->x + gm->w) / (float)FONT_ATLAS_WIDTH;
        v1 = (float)(gm->y + gm->h) / (float)FONT_ATLAS_HEIGHT;

        v[0].x = x0; v[0].y = y0; v[0].z = -0.2f; v[0].colour = colour; v[0].u = u0; v[0].v = v0;
        v[1].x = x1; v[1].y = y0; v[1].z = -0.2f; v[1].colour = colour; v[1].u = u1; v[1].v = v0;
        v[2].x = x0; v[2].y = y1; v[2].z = -0.2f; v[2].colour = colour; v[2].u = u0; v[2].v = v1;
        v[3].x = x1; v[3].y = y1; v[3].z = -0.2f; v[3].colour = colour; v[3].u = u1; v[3].v = v1;

        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(FontIsoVert));
        cur += (float)gm->advance;
    }

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void Font_DrawTextIso(IDirect3DDevice8* d,
    float vx, float vy, const char* str, int size, DWORD colour)
{
    Font_DrawIsoInternal(d, vx, vy, str, size, colour, 0.0f);
}

void Font_DrawTextIsoClip(IDirect3DDevice8* d,
    float vx, float vy, const char* str, int size, DWORD colour, float max_w)
{
    Font_DrawIsoInternal(d, vx, vy, str, size, colour, max_w);
}
