#ifndef USB2XB_GFX_H
#define USB2XB_GFX_H

/* DarkDash-derived D3D8 device + frame lifecycle, trimmed for USB2XB. */

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

	int  Gfx_Init(void);
	void Gfx_Shutdown(void);

	IDirect3DDevice8* Gfx_Device(void);
	int  Gfx_Width(void);
	int  Gfx_Height(void);
	int  Gfx_IsWidescreen(void);
	const char* Gfx_VideoModeStr(void);

	void Gfx_BeginFrame(DWORD clearColour);
	void Gfx_EndFrame(void);

#ifdef __cplusplus
}
#endif

#endif
