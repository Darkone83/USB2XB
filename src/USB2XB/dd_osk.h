/*---------------------------------------------------------------------------
    dd_osk.h -- controller on-screen keyboard overlay.

    Ported from DarkDash for USB2XB.
---------------------------------------------------------------------------*/
#ifndef DD_OSK_H
#define DD_OSK_H

#include <xtl.h>
#include <d3d8.h>

#define OSK_MAX_LEN 255

enum
{
    OSK_TEXT = 0,
    OSK_NUMERIC = 1
};

void Osk_Open(int mode, const char* initial, int maxLen);
void Osk_Close(void);
int  Osk_IsOpen(void);
int  Osk_Update(WORD pressed); /* 0 open, 1 confirm, -1 cancel */
void Osk_Draw(IDirect3DDevice8* d);
void Osk_GetText(char* buf, int buflen);

#endif
