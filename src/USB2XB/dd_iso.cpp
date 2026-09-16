/*---------------------------------------------------------------------------
    dd_iso.cpp -- DarkDash orthographic isometric camera, trimmed for USB2XB.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>
#include <d3dx8.h>
#include "dd_iso.h"
#include "dd_gfx.h"
#include "dd_ui.h"

#define ISO_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    float x, y, z;
    DWORD colour;
    float u, v;
} IsoVert;

static float s_pitch = 26.0f;
static float s_yaw = -16.0f;
static float s_breatheP = 0.0f;
static float s_breatheY = 0.0f;

static D3DXMATRIX s_world;
static D3DXMATRIX s_view;
static D3DXMATRIX s_proj;
static D3DXMATRIX s_combined;

static void iso_build(void)
{
    D3DXMATRIX rx, ry, tmp;
    D3DXVECTOR3 eye, at, up;

    D3DXMatrixRotationX(&rx, D3DXToRadian(s_pitch + s_breatheP));
    D3DXMatrixRotationY(&ry, D3DXToRadian(s_yaw + s_breatheY));
    D3DXMatrixMultiply(&s_world, &ry, &rx);

    eye.x=0.0f; eye.y=0.0f; eye.z=-800.0f;
    at.x=0.0f; at.y=0.0f; at.z=0.0f;
    up.x=0.0f; up.y=1.0f; up.z=0.0f;

    D3DXMatrixLookAtLH(&s_view, &eye, &at, &up);
    D3DXMatrixOrthoLH(&s_proj,
                      UI_VIRT_W * 1.30f,
                      UI_VIRT_H * 1.30f,
                      -2000.0f,
                      2000.0f);

    D3DXMatrixMultiply(&tmp, &s_world, &s_view);
    D3DXMatrixMultiply(&s_combined, &tmp, &s_proj);
}

void Iso_SetAngles(float p, float y)
{
    s_pitch = p;
    s_yaw = y;
}

void Iso_SetBreathe(float p, float y)
{
    s_breatheP = p;
    s_breatheY = y;
}

void Iso_GetAngles(float* p, float* y)
{
    if (p) *p = s_pitch;
    if (y) *y = s_yaw;
}

void Iso_NudgeAngles(float dp, float dy)
{
    s_pitch += dp;
    s_yaw += dy;

    if (s_pitch < 0.0f) s_pitch = 0.0f;
    if (s_pitch > 70.0f) s_pitch = 70.0f;
    if (s_yaw < -45.0f) s_yaw = -45.0f;
    if (s_yaw > 45.0f) s_yaw = 45.0f;
}

void Iso_Begin(void)
{
    IDirect3DDevice8* d = Gfx_Device();

    if (!d) return;

    iso_build();

    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&s_world);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&s_view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&s_proj);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
}

void Iso_End(void)
{
    IDirect3DDevice8* d = Gfx_Device();
    if (!d) return;
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void Iso_FillRect(float vx, float vy, float vw, float vh,
                  DWORD colour, int additive)
{
    IDirect3DDevice8* d = Gfx_Device();
    IsoVert v[4];
    float cx = UI_VIRT_W * 0.5f;
    float cy = UI_VIRT_H * 0.5f;
    float x0, x1, y0, y1;

    if (!d) return;

    x0 = vx - cx;
    x1 = vx + vw - cx;
    y0 = cy - vy;
    y1 = cy - (vy + vh);

    v[0].x=x0; v[0].y=y0; v[0].z=0; v[0].colour=colour; v[0].u=0; v[0].v=0;
    v[1].x=x1; v[1].y=y0; v[1].z=0; v[1].colour=colour; v[1].u=0; v[1].v=0;
    v[2].x=x0; v[2].y=y1; v[2].z=0; v[2].colour=colour; v[2].u=0; v[2].v=0;
    v[3].x=x1; v[3].y=y1; v[3].z=0; v[3].colour=colour; v[3].u=0; v[3].v=0;

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND,
                      additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(ISO_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(IsoVert));
}

#define ISO_STRIP_MAX 96

void Iso_DrawStrip(const IsoStripPt* pts, int n, int additive)
{
    IDirect3DDevice8* d = Gfx_Device();
    static IsoVert v[ISO_STRIP_MAX];
    float cx = UI_VIRT_W * 0.5f;
    float cy = UI_VIRT_H * 0.5f;
    int i;

    if (!d || !pts || n < 3) return;
    if (n > ISO_STRIP_MAX) n = ISO_STRIP_MAX;

    for (i = 0; i < n; ++i) {
        v[i].x = pts[i].vx - cx;
        v[i].y = cy - pts[i].vy;
        v[i].z = 0.0f;
        v[i].colour = pts[i].colour;
        v[i].u = 0.0f;
        v[i].v = 0.0f;
    }

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND,
                      additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(ISO_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, n - 2, v, sizeof(IsoVert));
}

void Iso_Project(float vx, float vy, float* outVx, float* outVy)
{
    float cx = UI_VIRT_W * 0.5f;
    float cy = UI_VIRT_H * 0.5f;
    D3DXVECTOR3 in, out;

    in.x = vx - cx;
    in.y = cy - vy;
    in.z = 0.0f;

    D3DXVec3TransformCoord(&out, &in, &s_combined);

    if (outVx)
        *outVx = (out.x * 0.5f + 0.5f) * UI_VIRT_W;
    if (outVy)
        *outVy = (1.0f - (out.y * 0.5f + 0.5f)) * UI_VIRT_H;
}
