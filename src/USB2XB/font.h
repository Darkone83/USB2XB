#ifndef USB2XB_FONT_H
#define USB2XB_FONT_H

/* DarkDash-style baked atlas font renderer. */

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_SIZE_SMALL   0
#define FONT_SIZE_MEDIUM  1
#define FONT_SIZE_LARGE   2

#define FONT_RGBA(r,g,b,a) (((DWORD)(a)<<24)|((DWORD)(r)<<16)|((DWORD)(g)<<8)|(DWORD)(b))
#define FONT_WHITE          FONT_RGBA(255,255,255,255)
#define FONT_GRAY           FONT_RGBA(170,170,170,255)
#define FONT_DARKGRAY       FONT_RGBA(80,80,80,255)
#define FONT_BLACK          FONT_RGBA(0,0,0,255)
#define FONT_RED            FONT_RGBA(220,50,50,255)

    int  Font_Init(IDirect3DDevice8* pDevice);
    void Font_Shutdown(void);

    int  Font_MeasureText(const char* str, int size);
    int  Font_GlyphHeight(int size);
    int  Font_LineHeight(int size);

    void Font_DrawText(IDirect3DDevice8* pDevice,
        float x, float y, const char* str, int size, DWORD colour, int max_w);

    void Font_DrawTextCentered(IDirect3DDevice8* pDevice,
        float cx, float y, float width, const char* str, int size, DWORD colour);

    void Font_DrawTextRight(IDirect3DDevice8* pDevice,
        float x, float y, const char* str, int size, DWORD colour);

    void Font_DrawTextEllipsis(IDirect3DDevice8* pDevice,
        float x, float y, const char* str, int size, DWORD colour, int max_w);

    void Font_DrawTextIso(IDirect3DDevice8* pDevice,
        float vx, float vy, const char* str, int size, DWORD colour);

    void Font_DrawTextIsoClip(IDirect3DDevice8* pDevice,
        float vx, float vy, const char* str, int size, DWORD colour, float max_w);

#ifdef __cplusplus
}
#endif
#endif
