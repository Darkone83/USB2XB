#pragma once
#ifndef USB2XB_HEXVIEWER_H
#define USB2XB_HEXVIEWER_H

#include <xtl.h>
#include "dd_fileops.h"

int  HexViewer_Open(DDStorage* fs, const char* path, const char* filename);
int  HexViewer_IsActive(void);
void HexViewer_Update(WORD pressed, WORD held);
void HexViewer_Render(void);
void HexViewer_Close(void);

#endif
