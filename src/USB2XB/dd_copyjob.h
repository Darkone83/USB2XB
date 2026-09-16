/*---------------------------------------------------------------------------
    dd_copyjob.h -- asynchronous, cancellable copy/move of files & trees.

    USB2XB expands each staged source tree once into a bounded flat work list,
    then copies directly from that list. This mirrors XbDiag's proven
    expand-then-transfer model and provides a true final file total without
    enumerating the source tree a second time.
---------------------------------------------------------------------------*/
#ifndef DD_COPYJOB_H
#define DD_COPYJOB_H

#include "dd_fileops.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct
    {
        DDStorage* fs;
        char src[DD_PATH_MAX];
        char name[DD_NAME_MAX];
        int isDir;
    } CopyJobItem;

    enum
    {
        CJ_RUNNING = 0,
        CJ_DONE,
        CJ_FAILED,
        CJ_CANCELLED,
        CJ_IDLE,
        CJ_CONFLICT
    };

    enum
    {
        CJ_CONFLICT_OVERWRITE = 1,
        CJ_CONFLICT_SKIP,
        CJ_CONFLICT_CANCEL
    };

    int CopyJob_Begin(const CopyJobItem* items, int nItems,
        DDStorage* destFs, const char* destDir, int isMove);
    int CopyJob_Pump(void);
    void CopyJob_Cancel(void);
    int CopyJob_Active(void);

    /*
        Resolve a paused destination-file collision.  OVERWRITE resumes the file,
        SKIP leaves the existing destination untouched, CANCEL stops the job.
    */
    int CopyJob_ResolveConflict(int action);

    int CopyJob_SkippedCount(void);

    /* True while the source tree is being expanded into the work list. */
    int CopyJob_Preparing(void);

    /*
        True nested-file progress. Once preparation finishes, total is final and
        current is the 1-based file ordinal being transferred.
    */
    void CopyJob_FileCounter(int* current, int* total);

    void CopyJob_Progress(int* filesDone, int* filesSeen,
        char* curName, int cap,
        DWORD* curDone, DWORD* curTotal);

    /* Empty string when no copy-engine error is pending. */
    const char* CopyJob_ErrorText(void);

#ifdef __cplusplus
}
#endif

#endif
