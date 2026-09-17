#pragma once
#ifndef USB2XB_CHECKSUM_H
#define USB2XB_CHECKSUM_H

#include <xtl.h>
#include "dd_fileops.h"

int  ChecksumViewer_Open(DDStorage* fs, const char* path, const char* filename);
int  ChecksumViewer_IsActive(void);
void ChecksumViewer_Update(WORD pressed, WORD held);
void ChecksumViewer_Render(void);
void ChecksumViewer_Close(void);

#endif
