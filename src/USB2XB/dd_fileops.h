#ifndef USB2XB_FILEOPS_H
#define USB2XB_FILEOPS_H

#include <xtl.h>

#define DD_PATH_MAX 512
#define DD_NAME_MAX 256
#define DD_FATX_NAME_MAX 42

typedef struct DDStorage DDStorage;
typedef void* DDFileHandle;

typedef struct
{
    char  name[DD_NAME_MAX];
    DWORD sizeLo;
    int   isDir;

    /*
        Backend-native sortable last-write timestamp.

        FATX stores the raw FILETIME value. FAT32 stores the packed
        last-write date/time as (date << 16) | time. The values are only
        compared within one pane/backend, so both preserve chronological order.
    */
    ULONGLONG sortTime;
} DDDirEntry;

typedef struct
{
    void* impl;
} DDDirHandle;

typedef struct
{
    DDStorage* storage;
    char       path[DD_PATH_MAX];
    char       name[DD_NAME_MAX];
} DDPath;

struct DDStorage
{
    const char* label;
    void* ctx;

    int (*ready)(DDStorage* s);

    int (*list_begin)(DDStorage* s, const char* path,
        DDDirHandle* h, DDDirEntry* first);
    int (*list_next)(DDStorage* s, DDDirHandle* h, DDDirEntry* next);
    void (*list_end)(DDStorage* s, DDDirHandle* h);

    DDFileHandle(*open_read)(DDStorage* s, const char* path, DWORD* sizeLo);
    DDFileHandle(*open_write)(DDStorage* s, const char* path, int overwrite);
    int (*read)(DDStorage* s, DDFileHandle h, void* dst, DWORD bytes, DWORD* got);
    int (*write)(DDStorage* s, DDFileHandle h, const void* src, DWORD bytes, DWORD* put);
    void (*close)(DDStorage* s, DDFileHandle h);

    int (*mkdir)(DDStorage* s, const char* path);
    int (*rename)(DDStorage* s, const char* oldPath, const char* newPath);
    int (*remove_file)(DDStorage* s, const char* path);
    int (*remove_dir)(DDStorage* s, const char* path);
    int (*exists)(DDStorage* s, const char* path);
};


/*
    Native FATX component-name compatibility.
    FATX supports long names directly (not 8.3), with a 42-byte component
    ceiling and its own legal-character table.
*/
int DD_FatxNameValid(const char* name);

void DDStorage_InitFatx(DDStorage* s, const char* label);

int Fileops_CopyFileBegin(DDStorage* srcFs, const char* src,
    DDStorage* dstFs, const char* dst,
    int overwrite);
int Fileops_CopyFilePump(DWORD budgetBytes);
void Fileops_CopyFileCancel(void);
int Fileops_CopyFileActive(void);
void Fileops_CopyFileProgress(DWORD* done, DWORD* total);
int Fileops_CopyFileResult(void); /* 1 success, 0 active/idle, -1 fail, -2 cancel */

/*
    DarkDash-style recursive delete routed through DDStorage so the same tree
    operation works on native FATX and USB FAT32.
*/
int Fileops_DeletePath(DDStorage* fs, const char* path, int isDir);

#endif
