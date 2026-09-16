#include <xtl.h>
#include "usb2xb.h"
#include "input.h"
#include "fileman.h"
#include "usb2xb_usb.h"
#include "usb2xb_storage.h"

int USB2XB_Init(USB2XBApp* app)
{
    if (!app)return 0;
    ZeroMemory(app, sizeof(*app));

    InitInput();
    DDStorage_InitFatx(&app->xbox, "XBOX");
    USB2XB_USB_Init();
    USB2XB_StorageInitUsb(&app->usb);

    /* USB left, Xbox E: right. Once FAT32 callbacks are plugged into the USB
       backend, FileMan requires no architectural change. */
    FileMan_Init(&app->usb, "/", &app->xbox, "E:\\");
    app->running = 1;
    return 1;
}

void USB2XB_Update(USB2XBApp* app)
{
    WORD now, pressed;
    if (!app)return;

    PumpInput();
    USB2XB_USB_Pump();

    now = GetButtons();
    pressed = (WORD)(now & ~app->prevButtons);
    app->prevButtons = now;

    if (FileMan_Update(pressed, now))
        app->running = 0;
}

void USB2XB_Render(USB2XBApp* app)
{
    (void)app;
    FileMan_Render();
}

void USB2XB_Shutdown(USB2XBApp* app)
{
    (void)app;
    USB2XB_USB_Shutdown();
    ShutdownInput();
}
