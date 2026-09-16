#ifndef USB2XB_UI_PRIMS_H
#define USB2XB_UI_PRIMS_H

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_ARGB(a,r,g,b) (((DWORD)(a)<<24)|((DWORD)(r)<<16)|((DWORD)(g)<<8)|(DWORD)(b))

    /*
        USB2XB logical canvas:
          4:3        640 x 480
          widescreen 854 x 480

        480 widescreen is anamorphic: logical 854 maps to the 640-pixel Xbox
        framebuffer horizontally and the display stretches it back to 16:9.
        720p maps the same 854x480 layout almost exactly 1.5x.
    */
    void  UI_Init(int backW, int backH);
    void  UI_SetStretch(int stretch); /* retained for source compatibility */
    void  UI_SetCalibration(float l, float r, float t, float b);

    float UI_Width(void);
    float UI_Height(void);
    int   UI_IsWide(void);

#define UI_VIRT_W (UI_Width())
#define UI_VIRT_H (UI_Height())

    float UI_Sx(float x);
    float UI_Sy(float y);
    float UI_ScaleX(float d);
    float UI_ScaleY(float d);

    void UI_FillRect(float vx, float vy, float vw, float vh, DWORD colour);
    void UI_FillRectAdd(float vx, float vy, float vw, float vh, DWORD colour);
    void UI_FillTri(float ax, float ay, float bx, float by, float cx, float cy,
        DWORD colour);

#ifdef __cplusplus
}
#endif

#endif
