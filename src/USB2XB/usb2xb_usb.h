#ifndef USB2XB_USB_H
#define USB2XB_USB_H

#include <xtl.h>

typedef enum
{
    U2X_USB_OFFLINE = 0,
    U2X_USB_SCANNING,
    U2X_USB_ENUMERATING,
    U2X_USB_READY,
    U2X_USB_ERROR
} U2XUsbState;

int  USB2XB_USB_Init(void);
void USB2XB_USB_Shutdown(void);
void USB2XB_USB_Pump(void);
void USB2XB_USB_RequestScan(void);

U2XUsbState USB2XB_USB_State(void);
const char* USB2XB_USB_StatusText(void);
const char* USB2XB_USB_Product(void);

int USB2XB_USB_ReadSectors(DWORD lba, DWORD count, void* buffer);
int USB2XB_USB_WriteSectors(DWORD lba, DWORD count, const void* buffer);

DWORD USB2XB_USB_SectorSize(void);
DWORD USB2XB_USB_SectorCount(void);

int   USB2XB_USB_Fat32Detected(void);
DWORD USB2XB_USB_VolumeLba(void);

#endif
