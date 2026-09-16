/*---------------------------------------------------------------------------
    dd_ui.cpp -- DarkDash virtual coordinate scaling + simple primitives.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_ui.h"
#include "dd_gfx.h"

static int   s_backW = 640;
static int   s_backH = 480;
static int   s_stretch = 0;
static int   s_wide = 0;
static float s_virtW = 640.0f;
static float s_virtH = 480.0f;
static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;
static float s_off_x = 0.0f;
static float s_off_y = 0.0f;
static float s_calL = 0.0f;
static float s_calR = 0.0f;
static float s_calT = 0.0f;
static float s_calB = 0.0f;

#define UI_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
} UiVert;

static void ui_recalc(void)
{
    float sx = (float)s_backW / s_virtW;
    float sy = (float)s_backH / s_virtH;

    /*
        Do not letterbox the logical canvas.  On 480 widescreen the Xbox
        framebuffer is anamorphic, so x and y intentionally use different
        framebuffer scales.  They appear uniform after display aspect
        correction.  720p is effectively uniform already.
    */
    s_scale_x = sx;
    s_scale_y = sy;
    s_off_x = 0.0f;
    s_off_y = 0.0f;

    {
        float availW = s_virtW - s_calL - s_calR;
        float availH = s_virtH - s_calT - s_calB;
        float fxr = (availW > 1.0f) ? availW / s_virtW : 1.0f;
        float fyr = (availH > 1.0f) ? availH / s_virtH : 1.0f;

        s_off_x += s_calL * s_scale_x;
        s_off_y += s_calT * s_scale_y;
        s_scale_x *= fxr;
        s_scale_y *= fyr;
    }
}


void UI_Init(int backW, int backH)
{
    s_backW = backW;
    s_backH = backH;
    s_wide = Gfx_IsWidescreen() ? 1 : 0;
    s_virtW = s_wide ? 854.0f : 640.0f;
    s_virtH = 480.0f;
    ui_recalc();
}

void UI_SetStretch(int stretch)
{
    s_stretch = stretch ? 1 : 0;
    ui_recalc();
}

void UI_SetCalibration(float l, float r, float t, float b)
{
    s_calL = l;
    s_calR = r;
    s_calT = t;
    s_calB = b;
    ui_recalc();
}

float UI_Width(void) { return s_virtW; }
float UI_Height(void) { return s_virtH; }
int UI_IsWide(void) { return s_wide; }

float UI_Sx(float x) { return s_off_x + x * s_scale_x; }
float UI_Sy(float y) { return s_off_y + y * s_scale_y; }
float UI_ScaleX(float d) { return d * s_scale_x; }
float UI_ScaleY(float d) { return d * s_scale_y; }

static void ui_quad(float vx, float vy, float vw, float vh,
    DWORD colour, int additive)
{
    IDirect3DDevice8* d = Gfx_Device();
    UiVert v[4];
    float x0, y0, x1, y1;

    if (!d) return;

    x0 = UI_Sx(vx);
    y0 = UI_Sy(vy);
    x1 = UI_Sx(vx + vw);
    y1 = UI_Sy(vy + vh);

    v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].rhw = 1; v[0].colour = colour; v[0].u = 0; v[0].v = 0;
    v[1].x = x1; v[1].y = y0; v[1].z = 0; v[1].rhw = 1; v[1].colour = colour; v[1].u = 0; v[1].v = 0;
    v[2].x = x0; v[2].y = y1; v[2].z = 0; v[2].rhw = 1; v[2].colour = colour; v[2].u = 0; v[2].v = 0;
    v[3].x = x1; v[3].y = y1; v[3].z = 0; v[3].rhw = 1; v[3].colour = colour; v[3].u = 0; v[3].v = 0;

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND,
        additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(UiVert));

    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
}

void UI_FillRect(float vx, float vy, float vw, float vh, DWORD colour)
{
    ui_quad(vx, vy, vw, vh, colour, 0);
}

void UI_FillRectAdd(float vx, float vy, float vw, float vh, DWORD colour)
{
    ui_quad(vx, vy, vw, vh, colour, 1);
}

void UI_FillTri(float ax, float ay, float bx, float by, float cx, float cy,
    DWORD colour)
{
    IDirect3DDevice8* d = Gfx_Device();
    UiVert t[3];

    if (!d) return;

    t[0].x = UI_Sx(ax); t[0].y = UI_Sy(ay); t[0].z = 0; t[0].rhw = 1; t[0].colour = colour; t[0].u = 0; t[0].v = 0;
    t[1].x = UI_Sx(bx); t[1].y = UI_Sy(by); t[1].z = 0; t[1].rhw = 1; t[1].colour = colour; t[1].u = 0; t[1].v = 0;
    t[2].x = UI_Sx(cx); t[2].y = UI_Sy(cy); t[2].z = 0; t[2].rhw = 1; t[2].colour = colour; t[2].u = 0; t[2].v = 0;

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t, sizeof(UiVert));
}
