#ifndef USB2XB_TEXTVIEWER_H
#define USB2XB_TEXTVIEWER_H

#include <xtl.h>
#include "dd_fileops.h"

/*
    Read-only text viewer derived from the XbDiag FileViewer behavior.
    USB2XB routes reads through DDStorage so the same viewer works on FATX
    and the custom FAT32 USB backend.
*/
int  TextViewer_CanOpen(const char* filename);
int  TextViewer_Open(DDStorage* fs, const char* path, const char* filename);
int  TextViewer_IsActive(void);
void TextViewer_Close(void);
void TextViewer_Update(WORD pressed, WORD held);
void TextViewer_Render(void);

#endif
#pragma once
