/* USB2XB - RXDK/XDK FAT32 <-> FATX transfer utility */
#include <xtl.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "font.h"
#include "usb2xb.h"

static USB2XBApp g_app;

void __cdecl main(void)
{
    if(!Gfx_Init())return;

    UI_Init(Gfx_Width(),Gfx_Height());

    if(!Font_Init(Gfx_Device())){
        Gfx_Shutdown();
        return;
    }

    if(!USB2XB_Init(&g_app)){
        Font_Shutdown();
        Gfx_Shutdown();
        return;
    }

    while(g_app.running){
        USB2XB_Update(&g_app);
        Gfx_BeginFrame(0xFF050509);
        USB2XB_Render(&g_app);
        Gfx_EndFrame();
    }

    USB2XB_Shutdown(&g_app);
    Font_Shutdown();
    Gfx_Shutdown();
}
