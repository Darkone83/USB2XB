#ifndef USB2XB_H
#define USB2XB_H
#include <xtl.h>
#include "dd_fileops.h"

typedef struct
{
    DDStorage usb;
    DDStorage xbox;
    WORD prevButtons;
    int running;
} USB2XBApp;

int  USB2XB_Init(USB2XBApp* app);
void USB2XB_Update(USB2XBApp* app);
void USB2XB_Render(USB2XBApp* app);
void USB2XB_Shutdown(USB2XBApp* app);

#endif
