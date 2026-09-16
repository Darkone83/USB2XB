#include <xtl.h>
#include <stdlib.h>
#include "dd_copyjob.h"

#define COPYJOB_MAX_ITEMS 512
#define COPYJOB_MAX_WORK 4096
#define STACK_MAX 40
#define COPYJOB_CHUNKS_PER_PUMP 2
#define COPYJOB_EXPAND_STEPS_PER_PUMP 4

enum
{
    JOB_PHASE_IDLE = 0,
    JOB_PHASE_EXPAND,
    JOB_PHASE_COPY
};

enum
{
    WORK_FILE = 1,
    WORK_DIR_BEGIN,
    WORK_DIR_END
};

typedef struct
{
    DDStorage* fs;
    char src[DD_PATH_MAX];
    char dst[DD_PATH_MAX];

    int type;

    /*
        For files / DIR_BEGIN, parentDir is the work-list index of the parent
        DIR_BEGIN, or -1 for a top-level item.  DIR_END uses matchDir to point
        back to its DIR_BEGIN.
    */
    int parentDir;
    int matchDir;

    int conflictChecked;
    int overwriteApproved;

    /*
        MOVE only: set on a directory and its ancestors when one of its files
        is skipped, so the source directory is preserved.
    */
    int keepSource;
} FlatWork;

typedef struct
{
    DDStorage* fs;
    char src[DD_PATH_MAX];
    char dst[DD_PATH_MAX];

    int beginIndex;
    int phase;          /* 0=open enumeration, 1=enumerate */

    DDDirHandle dh;
    DDDirEntry current;
    int haveCurrent;
} ExpandFrame;


/*
    XbDiag-style expand-once work list.

    Source directories are enumerated exactly once during JOB_PHASE_EXPAND.
    The completed flat list then drives the copy phase directly, so a true
    FILE n OF total counter is available without a second source-tree walk.
*/
static FlatWork g_work[COPYJOB_MAX_WORK];
static ExpandFrame g_expand[STACK_MAX];

static struct
{
    int active, cancel, state, move;
    int phase;

    DDStorage* dstFs;
    char destDir[DD_PATH_MAX];

    CopyJobItem items[COPYJOB_MAX_ITEMS];
    int nItems;
    int expandItemIndex;

    int expandSp;

    int workCount;
    int workIndex;

    int fileTotal;
    int filesProcessed;
    int filesDone;
    int filesSeen;
    int filesSkipped;

    char curName[DD_NAME_MAX];

    int conflict;

    /*
        Human-readable copy-engine failure. Keep this short enough for the
        file-manager status line.
    */
    char lastError[96];
} g_job;


static int slen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}


static int scpy_checked(char* d, int cap, const char* s)
{
    int n;

    if (!d || cap <= 0)
        return 0;

    n = slen(s);

    if (n >= cap)
    {
        d[0] = 0;
        return 0;
    }

    if (n)
        CopyMemory(d, s, n);

    d[n] = 0;
    return 1;
}


static void scpy(char* d, int cap, const char* s)
{
    int i = 0;

    if (!d || cap <= 0)
        return;

    while (s && s[i] && i < cap - 1)
    {
        d[i] = s[i];
        ++i;
    }

    d[i] = 0;
}


static void set_error(const char* text)
{
    scpy(
        g_job.lastError,
        sizeof(g_job.lastError),
        text ? text : "Copy failed");
}


static int is_sep(char c)
{
    return c == '\\' || c == '/';
}


static char fold_char(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - ('a' - 'A'));

    if (c == '/')
        c = '\\';

    return c;
}


static int path_trim_len(const char* p)
{
    int n = slen(p);

    /*
        Preserve "\\" and "X:\\" roots, but ignore cosmetic trailing separators
        elsewhere when comparing paths.
    */
    while (n > 1 &&
        is_sep(p[n - 1]) &&
        !(n == 3 && p[1] == ':'))
    {
        --n;
    }

    return n;
}


static int path_prefix_equal(const char* a, const char* b, int count)
{
    int i;

    for (i = 0; i < count; ++i)
    {
        if (fold_char(a[i]) != fold_char(b[i]))
            return 0;
    }

    return 1;
}


static int path_equal(const char* a, const char* b)
{
    int na;
    int nb;

    if (!a || !b)
        return 0;

    na = path_trim_len(a);
    nb = path_trim_len(b);

    if (na != nb)
        return 0;

    return path_prefix_equal(a, b, na);
}


/* True only when child is strictly below parent. */
static int path_is_descendant(const char* child, const char* parent)
{
    int nc;
    int np;

    if (!child || !parent)
        return 0;

    nc = path_trim_len(child);
    np = path_trim_len(parent);

    if (nc <= np || !path_prefix_equal(child, parent, np))
        return 0;

    /*
        A root already ends in a separator; otherwise require a component
        boundary so "E:\\Games2" is not considered inside "E:\\Games".
    */
    if (np > 0 && is_sep(parent[np - 1]))
        return 1;

    return is_sep(child[np]) ? 1 : 0;
}


static int join_checked(
    char* out,
    int cap,
    const char* a,
    const char* b)
{
    int na = slen(a);
    int nb = slen(b);
    int needSep = 0;
    int total;
    int pos = 0;

    if (!out || cap <= 0 || !b || !b[0])
        return 0;

    if (na > 0 &&
        !is_sep(a[na - 1]))
    {
        needSep = 1;
    }

    total = na + needSep + nb;

    if (total >= cap)
    {
        out[0] = 0;
        return 0;
    }

    if (na)
    {
        CopyMemory(out, a, na);
        pos = na;
    }

    if (needSep)
        out[pos++] = '\\';

    if (nb)
    {
        CopyMemory(out + pos, b, nb);
        pos += nb;
    }

    out[pos] = 0;
    return 1;
}


static int isdot(const char* n)
{
    return n &&
        n[0] == '.' &&
        (n[1] == 0 ||
            (n[1] == '.' && n[2] == 0));
}


static const char* basename_ptr(const char* path)
{
    const char* base = path;
    int i = 0;

    if (!path)
        return "";

    while (path[i])
    {
        if (is_sep(path[i]))
            base = path + i + 1;

        ++i;
    }

    return base;
}


static int append_work(
    int type,
    DDStorage* fs,
    const char* src,
    const char* dst,
    int parentDir,
    int matchDir)
{
    FlatWork* w;
    int idx;

    if (g_job.workCount >= COPYJOB_MAX_WORK)
    {
        set_error("Too many files or folders");
        return -1;
    }

    idx = g_job.workCount;
    w = &g_work[idx];
    ZeroMemory(w, sizeof(*w));

    w->type = type;
    w->fs = fs;
    w->parentDir = parentDir;
    w->matchDir = matchDir;

    if (type != WORK_DIR_END)
    {
        if (!fs ||
            !src || !src[0] ||
            !dst || !dst[0])
        {
            set_error("Invalid copy path");
            return -1;
        }

        if (!scpy_checked(
            w->src,
            sizeof(w->src),
            src) ||
            !scpy_checked(
                w->dst,
                sizeof(w->dst),
                dst))
        {
            set_error("Path too long");
            return -1;
        }
    }

    ++g_job.workCount;
    return idx;
}


static int append_file(
    DDStorage* fs,
    const char* src,
    const char* dst,
    int parentDir)
{
    int idx = append_work(
        WORK_FILE,
        fs,
        src,
        dst,
        parentDir,
        -1);

    if (idx >= 0)
    {
        ++g_job.fileTotal;
        ++g_job.filesSeen;
    }

    return idx;
}


static int push_expand_dir(
    DDStorage* fs,
    const char* src,
    const char* dst,
    int parentDir)
{
    ExpandFrame* f;
    int beginIndex;

    if (g_job.expandSp >= STACK_MAX)
    {
        set_error("Folder nesting too deep");
        return 0;
    }

    beginIndex = append_work(
        WORK_DIR_BEGIN,
        fs,
        src,
        dst,
        parentDir,
        -1);

    if (beginIndex < 0)
        return 0;

    f = &g_expand[g_job.expandSp];
    ZeroMemory(f, sizeof(*f));

    if (!scpy_checked(
        f->src,
        sizeof(f->src),
        src) ||
        !scpy_checked(
            f->dst,
            sizeof(f->dst),
            dst))
    {
        set_error("Path too long");
        return 0;
    }

    f->fs = fs;
    f->beginIndex = beginIndex;

    ++g_job.expandSp;
    return 1;
}


static int append_dir_end(int beginIndex)
{
    return append_work(
        WORK_DIR_END,
        0,
        0,
        0,
        -1,
        beginIndex) >= 0;
}


static void close_expand_stack(void)
{
    while (g_job.expandSp > 0)
    {
        ExpandFrame* f =
            &g_expand[g_job.expandSp - 1];

        if (f->dh.impl &&
            f->fs &&
            f->fs->list_end)
        {
            f->fs->list_end(
                f->fs,
                &f->dh);
        }

        f->dh.impl = 0;
        --g_job.expandSp;
    }
}


static void finish(int state)
{
    close_expand_stack();

    if (Fileops_CopyFileActive())
    {
        /*
            Do not merely set the inner cancel flag and abandon it. Pump once
            so Fileops closes both handles and removes its temporary file.
            The cancel branch runs before any additional read/write.
        */
        Fileops_CopyFileCancel();
        Fileops_CopyFilePump(1);
    }

    g_job.active = 0;
    g_job.state = state;
    g_job.phase = JOB_PHASE_IDLE;
}


/*
    One expand-once source-tree step.
      -1 = failure
       0 = more expansion remains
       1 = expansion finished; copy phase is ready
*/
static int expand_step(void)
{
    ExpandFrame* f;

    /*
        With no open directory frame, seed the next staged top-level item.
    */
    if (g_job.expandSp <= 0)
    {
        CopyJobItem* it;
        char dst[DD_PATH_MAX];

        if (g_job.expandItemIndex >= g_job.nItems)
        {
            g_job.phase = JOB_PHASE_COPY;
            g_job.workIndex = 0;
            g_job.filesProcessed = 0;
            g_job.curName[0] = 0;
            return 1;
        }

        it = &g_job.items[g_job.expandItemIndex++];

        if (!join_checked(
            dst,
            sizeof(dst),
            g_job.destDir,
            it->name))
        {
            set_error("Destination path too long");
            return -1;
        }

        if (it->isDir)
        {
            if (!push_expand_dir(
                it->fs,
                it->src,
                dst,
                -1))
            {
                return -1;
            }
        }
        else
        {
            if (append_file(
                it->fs,
                it->src,
                dst,
                -1) < 0)
            {
                return -1;
            }
        }

        return 0;
    }

    f = &g_expand[g_job.expandSp - 1];

    if (f->phase == 0)
    {
        if (!f->fs ||
            !f->fs->list_begin ||
            !f->fs->list_next ||
            !f->fs->list_end)
        {
            set_error("Folder enumeration unavailable");
            return -1;
        }

        f->phase = 1;

        if (f->fs->list_begin(
            f->fs,
            f->src,
            &f->dh,
            &f->current))
        {
            f->haveCurrent = 1;
        }
        else
        {
            /*
                Preserve the existing backend convention: false can mean an
                empty directory as well as enumeration failure.
            */
            f->haveCurrent = 0;
        }

        return 0;
    }

    if (f->phase == 1)
    {
        DDDirEntry e;
        char csrc[DD_PATH_MAX];
        char cdst[DD_PATH_MAX];

        if (f->haveCurrent)
        {
            e = f->current;
            f->haveCurrent = 0;
        }
        else if (f->dh.impl &&
            f->fs->list_next(
                f->fs,
                &f->dh,
                &e))
        {
            /* got next child */
        }
        else
        {
            int beginIndex = f->beginIndex;

            if (f->dh.impl)
            {
                f->fs->list_end(
                    f->fs,
                    &f->dh);
            }

            f->dh.impl = 0;
            --g_job.expandSp;

            if (!append_dir_end(beginIndex))
                return -1;

            return 0;
        }

        if (isdot(e.name))
            return 0;

        if (!DD_FatxNameValid(e.name))
        {
            set_error("Name is not FATX compatible");
            return -1;
        }

        if (!join_checked(
            csrc,
            sizeof(csrc),
            f->src,
            e.name) ||
            !join_checked(
                cdst,
                sizeof(cdst),
                f->dst,
                e.name))
        {
            set_error("Nested path too long");
            return -1;
        }

        if (e.isDir)
        {
            if (!push_expand_dir(
                f->fs,
                csrc,
                cdst,
                f->beginIndex))
            {
                return -1;
            }
        }
        else
        {
            if (append_file(
                f->fs,
                csrc,
                cdst,
                f->beginIndex) < 0)
            {
                return -1;
            }
        }

        return 0;
    }

    set_error("Expand state error");
    return -1;
}


static void mark_parent_dirs_keep_source(int parentDir)
{
    int idx = parentDir;

    while (idx >= 0 &&
        idx < g_job.workCount)
    {
        FlatWork* d = &g_work[idx];

        if (d->type != WORK_DIR_BEGIN)
            break;

        d->keepSource = 1;
        idx = d->parentDir;
    }
}


int CopyJob_Begin(
    const CopyJobItem* items,
    int nItems,
    DDStorage* destFs,
    const char* destDir,
    int isMove)
{
    int i;

    if (g_job.active)
    {
        set_error("Copy already active");
        return 0;
    }

    /*
        Do not clear the multi-megabyte work array here. Entries are zeroed as
        they are appended. Only reset live job state and the small top-level
        staging set.
    */
    ZeroMemory(&g_job, sizeof(g_job));
    g_job.state = CJ_IDLE;
    g_job.phase = JOB_PHASE_IDLE;

    if (!items ||
        nItems <= 0 ||
        !destFs ||
        !destDir ||
        !destDir[0])
    {
        set_error("Invalid copy request");
        return 0;
    }

    if (nItems > COPYJOB_MAX_ITEMS)
    {
        set_error("Too many selected items");
        return 0;
    }

    if (!scpy_checked(
        g_job.destDir,
        sizeof(g_job.destDir),
        destDir))
    {
        set_error("Destination path too long");
        return 0;
    }

    /*
        Validate every top-level destination before the source tree is
        expanded or any destination write is attempted.
    */
    for (i = 0; i < nItems; ++i)
    {
        char dst[DD_PATH_MAX];

        if (!items[i].fs ||
            !items[i].src[0] ||
            !items[i].name[0])
        {
            set_error("Invalid selected item");
            return 0;
        }

        if (slen(items[i].src) >= DD_PATH_MAX ||
            slen(items[i].name) >= DD_NAME_MAX)
        {
            set_error("Source path too long");
            return 0;
        }

        if (!DD_FatxNameValid(items[i].name))
        {
            set_error("Name is not FATX compatible");
            return 0;
        }

        if (!join_checked(
            dst,
            sizeof(dst),
            g_job.destDir,
            items[i].name))
        {
            set_error("Destination path too long");
            return 0;
        }

        if (items[i].fs == destFs)
        {
            if (path_equal(items[i].src, dst))
            {
                set_error("Source and destination are the same");
                return 0;
            }

            if (items[i].isDir &&
                path_is_descendant(dst, items[i].src))
            {
                set_error("Cannot copy folder into itself");
                return 0;
            }
        }
    }

    g_job.dstFs = destFs;
    g_job.move = isMove ? 1 : 0;

    for (i = 0; i < nItems; ++i)
        g_job.items[i] = items[i];

    g_job.nItems = nItems;
    g_job.expandItemIndex = 0;
    g_job.expandSp = 0;
    g_job.workCount = 0;
    g_job.workIndex = 0;
    g_job.fileTotal = 0;
    g_job.filesProcessed = 0;
    g_job.filesDone = 0;
    g_job.filesSeen = 0;
    g_job.filesSkipped = 0;
    g_job.conflict = 0;
    g_job.cancel = 0;
    g_job.curName[0] = 0;
    g_job.lastError[0] = 0;

    g_job.phase = JOB_PHASE_EXPAND;
    g_job.state = CJ_RUNNING;
    g_job.active = 1;

    return 1;
}


int CopyJob_Pump(void)
{
    int iter;

    if (!g_job.active)
        return g_job.state ? g_job.state : CJ_IDLE;

    if (g_job.conflict)
        return CJ_CONFLICT;

    if (g_job.cancel)
    {
        set_error("Cancelled");
        finish(CJ_CANCELLED);
        return CJ_CANCELLED;
    }

    /*
        Phase 1: expand the source tree once into the flat work list. This is
        deliberately incremental so slow USB enumeration cannot monopolize a
        render frame.
    */
    if (g_job.phase == JOB_PHASE_EXPAND)
    {
        for (iter = 0;
            iter < COPYJOB_EXPAND_STEPS_PER_PUMP &&
            g_job.phase == JOB_PHASE_EXPAND;
            ++iter)
        {
            int er = expand_step();

            if (er < 0)
            {
                finish(CJ_FAILED);
                return CJ_FAILED;
            }

            if (er > 0)
                break;
        }

        /*
            Keep expansion and transfer on separate frames. Once expansion is
            complete, fileTotal is final and FILE n OF total is truthful.
        */
        return CJ_RUNNING;
    }

    if (g_job.phase != JOB_PHASE_COPY)
    {
        set_error("Copy state error");
        finish(CJ_FAILED);
        return CJ_FAILED;
    }

    /*
        Phase 2: consume the already-expanded work list. No source directory
        enumeration happens here.
    */
    for (iter = 0; iter < COPYJOB_CHUNKS_PER_PUMP; ++iter)
    {
        FlatWork* w;

        if (Fileops_CopyFileActive())
        {
            int r = Fileops_CopyFilePump(64 * 1024);

            if (r < 0)
            {
                if (r == -2)
                {
                    set_error("Cancelled");
                    finish(CJ_CANCELLED);
                }
                else
                {
                    set_error("File transfer failed");
                    finish(CJ_FAILED);
                }

                return g_job.state;
            }

            if (r == 1)
            {
                if (g_job.workIndex < 0 ||
                    g_job.workIndex >= g_job.workCount)
                {
                    set_error("Copy state error");
                    finish(CJ_FAILED);
                    return CJ_FAILED;
                }

                w = &g_work[g_job.workIndex];

                if (w->type != WORK_FILE)
                {
                    set_error("Copy state error");
                    finish(CJ_FAILED);
                    return CJ_FAILED;
                }

                ++g_job.filesDone;
                ++g_job.filesProcessed;

                if (g_job.move)
                {
                    if (!w->fs ||
                        !w->fs->remove_file ||
                        !w->fs->remove_file(
                            w->fs,
                            w->src))
                    {
                        set_error("Copied, source delete failed");
                        finish(CJ_FAILED);
                        return CJ_FAILED;
                    }
                }

                ++g_job.workIndex;
            }

            continue;
        }

        if (g_job.workIndex >= g_job.workCount)
        {
            finish(CJ_DONE);
            return CJ_DONE;
        }

        w = &g_work[g_job.workIndex];

        if (w->type == WORK_DIR_BEGIN)
        {
            g_job.curName[0] = 0;

            if (g_job.dstFs->exists &&
                g_job.dstFs->exists(
                    g_job.dstFs,
                    w->dst))
            {
                DDFileHandle probe = 0;
                DWORD probeSize = 0;

                if (g_job.dstFs->open_read)
                {
                    probe = g_job.dstFs->open_read(
                        g_job.dstFs,
                        w->dst,
                        &probeSize);
                }

                if (probe)
                {
                    g_job.dstFs->close(
                        g_job.dstFs,
                        probe);

                    set_error("Destination name is a file");
                    finish(CJ_FAILED);
                    return CJ_FAILED;
                }

                /* Existing destination directory: merge naturally. */
            }
            else if (!g_job.dstFs->mkdir ||
                !g_job.dstFs->mkdir(
                    g_job.dstFs,
                    w->dst))
            {
                set_error("Unable to create destination folder");
                finish(CJ_FAILED);
                return CJ_FAILED;
            }

            ++g_job.workIndex;
            continue;
        }

        if (w->type == WORK_DIR_END)
        {
            FlatWork* begin;

            g_job.curName[0] = 0;

            if (w->matchDir < 0 ||
                w->matchDir >= g_job.workCount)
            {
                set_error("Copy state error");
                finish(CJ_FAILED);
                return CJ_FAILED;
            }

            begin = &g_work[w->matchDir];

            if (begin->type != WORK_DIR_BEGIN)
            {
                set_error("Copy state error");
                finish(CJ_FAILED);
                return CJ_FAILED;
            }

            if (g_job.move &&
                !begin->keepSource)
            {
                if (!begin->fs ||
                    !begin->fs->remove_dir ||
                    !begin->fs->remove_dir(
                        begin->fs,
                        begin->src))
                {
                    set_error("Copied, source folder delete failed");
                    finish(CJ_FAILED);
                    return CJ_FAILED;
                }
            }

            ++g_job.workIndex;
            continue;
        }

        if (w->type != WORK_FILE)
        {
            set_error("Copy state error");
            finish(CJ_FAILED);
            return CJ_FAILED;
        }

        scpy(
            g_job.curName,
            sizeof(g_job.curName),
            basename_ptr(w->src));

        if (!w->conflictChecked)
        {
            if (g_job.dstFs->exists &&
                g_job.dstFs->exists(
                    g_job.dstFs,
                    w->dst))
            {
                DDFileHandle probe = 0;
                DWORD probeSize = 0;

                if (g_job.dstFs->open_read)
                {
                    probe = g_job.dstFs->open_read(
                        g_job.dstFs,
                        w->dst,
                        &probeSize);
                }

                if (!probe)
                {
                    set_error("Destination name is a folder");
                    finish(CJ_FAILED);
                    return CJ_FAILED;
                }

                g_job.dstFs->close(
                    g_job.dstFs,
                    probe);

                g_job.conflict = 1;
                g_job.state = CJ_CONFLICT;
                return CJ_CONFLICT;
            }

            w->conflictChecked = 1;
        }

        if (!Fileops_CopyFileBegin(
            w->fs,
            w->src,
            g_job.dstFs,
            w->dst,
            w->overwriteApproved ? 1 : 0))
        {
            set_error("Unable to open copy file");
            finish(CJ_FAILED);
            return CJ_FAILED;
        }
    }

    return CJ_RUNNING;
}


int CopyJob_ResolveConflict(int action)
{
    FlatWork* w;

    if (!g_job.active ||
        !g_job.conflict ||
        g_job.phase != JOB_PHASE_COPY ||
        g_job.workIndex < 0 ||
        g_job.workIndex >= g_job.workCount)
    {
        return 0;
    }

    w = &g_work[g_job.workIndex];

    if (w->type != WORK_FILE)
        return 0;

    if (action == CJ_CONFLICT_CANCEL)
    {
        g_job.conflict = 0;
        set_error("Cancelled");
        finish(CJ_CANCELLED);
        return 1;
    }

    if (action == CJ_CONFLICT_SKIP)
    {
        ++g_job.filesSkipped;
        ++g_job.filesProcessed;

        /*
            Skip never changes the destination and never deletes the source.
            Preserve the containing source directory and all of its ancestors
            for MOVE operations.
        */
        if (g_job.move)
            mark_parent_dirs_keep_source(w->parentDir);

        ++g_job.workIndex;
        g_job.conflict = 0;
        g_job.state = CJ_RUNNING;
        g_job.curName[0] = 0;
        return 1;
    }

    if (action == CJ_CONFLICT_OVERWRITE)
    {
        w->conflictChecked = 1;
        w->overwriteApproved = 1;
        g_job.conflict = 0;
        g_job.state = CJ_RUNNING;
        return 1;
    }

    return 0;
}


int CopyJob_SkippedCount(void)
{
    return g_job.filesSkipped;
}


int CopyJob_Preparing(void)
{
    return (g_job.active &&
        g_job.phase == JOB_PHASE_EXPAND) ? 1 : 0;
}


void CopyJob_FileCounter(
    int* current,
    int* total)
{
    int cur = 0;
    int count = 0;

    if (g_job.phase == JOB_PHASE_COPY ||
        (!g_job.active &&
            g_job.state == CJ_DONE))
    {
        count = g_job.fileTotal;

        if (count > 0)
        {
            cur = g_job.filesProcessed + 1;

            if (cur > count)
                cur = count;
        }
    }

    if (current)
        *current = cur;

    if (total)
        *total = count;
}


void CopyJob_Cancel(void)
{
    if (g_job.conflict)
    {
        g_job.conflict = 0;
        set_error("Cancelled");
        finish(CJ_CANCELLED);
        return;
    }

    g_job.cancel = 1;

    if (Fileops_CopyFileActive())
        Fileops_CopyFileCancel();
}


int CopyJob_Active(void)
{
    return g_job.active;
}


void CopyJob_Progress(
    int* filesDone,
    int* filesSeen,
    char* curName,
    int cap,
    DWORD* curDone,
    DWORD* curTotal)
{
    int i = 0;

    if (filesDone)
        *filesDone = g_job.filesDone;

    if (filesSeen)
        *filesSeen = g_job.filesSeen;

    if (curDone || curTotal)
    {
        if (Fileops_CopyFileActive())
        {
            Fileops_CopyFileProgress(
                curDone,
                curTotal);
        }
        else
        {
            if (curDone)
                *curDone = 0;

            if (curTotal)
                *curTotal = 0;
        }
    }

    if (curName && cap > 0)
    {
        while (g_job.curName[i] &&
            i < cap - 1)
        {
            curName[i] = g_job.curName[i];
            ++i;
        }

        curName[i] = 0;
    }
}


const char* CopyJob_ErrorText(void)
{
    return g_job.lastError;
}
