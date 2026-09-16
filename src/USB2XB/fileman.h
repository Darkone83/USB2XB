#ifndef USB2XB_FILEMAN_H
#define USB2XB_FILEMAN_H

#include <xtl.h>
#include "dd_fileops.h"

void FileMan_Init(DDStorage* left, const char* leftRoot,
                  DDStorage* right, const char* rightRoot);
void FileMan_Refresh(void);
int  FileMan_Update(WORD pressed, WORD held); /* nonzero requests app exit */
void FileMan_Render(void);

#endif
