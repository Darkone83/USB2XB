#ifndef USB2XB_STORAGE_H
#define USB2XB_STORAGE_H
#include "dd_fileops.h"

void USB2XB_StorageInitUsb(DDStorage* s);
int  USB2XB_StorageUsbReady(void);

/*
    Logical mount control used by FileMan.  Unmount invalidates only the FAT32
    layer; it deliberately leaves the hardware-validated USB/BOT path alone.
*/
void USB2XB_StorageUsbUnmount(void);
void USB2XB_StorageUsbRemount(void);
int  USB2XB_StorageUsbUserUnmounted(void);

/*
    Quick-format the currently mounted USB partition as FAT32.
    The MBR/partition boundary is preserved. Formatting is incremental so the
    UI remains responsive while large FAT tables are cleared.
*/
int  USB2XB_StorageFormatUsbBegin(void);
int  USB2XB_StorageFormatUsbPump(void); /* 0 running, 1 done, -1 failed */
int  USB2XB_StorageFormatUsbActive(void);
void USB2XB_StorageFormatUsbProgress(DWORD* done, DWORD* total);

#endif
