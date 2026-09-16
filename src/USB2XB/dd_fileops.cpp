#include <xtl.h>
#include <stdlib.h>
#include "dd_fileops.h"


int DD_FatxNameValid(const char* name)
{
    /*
        Mirrors the original FATX driver's FatxIsValidFatFileName rules for
        one-byte component names:
          - 1..42 bytes
          - "." and ".." are reserved
          - control characters are illegal
          - these printable characters are illegal:
              " * + , / : ; < = > ? \ |
        Bytes >= 0x80 are not rejected by the FATX kernel table.
    */
    int len = 0;
    int i;

    if (!name || !name[0])
        return 0;

    while (name[len])
        ++len;

    if (len<1 || len>DD_FATX_NAME_MAX)
        return 0;

    if ((len == 1 && name[0] == '.') ||
        (len == 2 && name[0] == '.' && name[1] == '.'))
    {
        return 0;
    }

    for (i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)name[i];

        if (c < 32 ||
            c == '"' ||
            c == '*' ||
            c == '+' ||
            c == ',' ||
            c == '/' ||
            c == ':' ||
            c == ';' ||
            c == '<' ||
            c == '=' ||
            c == '>' ||
            c == '?' ||
            c == '\\' ||
            c == '|')
        {
            return 0;
        }
    }

    return 1;
}


typedef struct
{
    HANDLE h;
    WIN32_FIND_DATA fd;
    int firstConsumed;
} FatxFind;

static int fatx_ready(DDStorage* s) { (void)s; return 1; }

static void join_pattern(char* out, int cap, const char* path)
{
    int i = 0, n = 0;
    while (path && path[i] && n < cap - 1) out[n++] = path[i++];
    if (n && out[n - 1] != '\\' && n < cap - 1) out[n++] = '\\';
    if (n < cap - 1) out[n++] = '*';
    out[n] = 0;
}

static void fill_entry(DDDirEntry* e, const WIN32_FIND_DATA* fd)
{
    int i = 0;
    while (fd->cFileName[i] && i < DD_NAME_MAX - 1) {
        e->name[i] = fd->cFileName[i];
        ++i;
    }
    e->name[i] = 0;
    e->sizeLo = fd->nFileSizeLow;
    e->isDir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

static int fatx_list_begin(DDStorage* s, const char* path,
    DDDirHandle* dh, DDDirEntry* first)
{
    FatxFind* f;
    char pat[DD_PATH_MAX + 4];
    (void)s;

    if (!dh || !first) return 0;
    f = (FatxFind*)malloc(sizeof(FatxFind));
    if (!f) return 0;
    ZeroMemory(f, sizeof(*f));
    join_pattern(pat, sizeof(pat), path);
    f->h = FindFirstFileA(pat, &f->fd);
    if (f->h == INVALID_HANDLE_VALUE) {
        free(f);
        dh->impl = NULL;
        return 0;
    }
    dh->impl = f;
    fill_entry(first, &f->fd);
    return 1;
}

static int fatx_list_next(DDStorage* s, DDDirHandle* dh, DDDirEntry* next)
{
    FatxFind* f = (FatxFind*)dh->impl;
    (void)s;
    if (!f || !next) return 0;
    if (!FindNextFileA(f->h, &f->fd)) return 0;
    fill_entry(next, &f->fd);
    return 1;
}

static void fatx_list_end(DDStorage* s, DDDirHandle* dh)
{
    FatxFind* f;
    (void)s;
    if (!dh || !dh->impl) return;
    f = (FatxFind*)dh->impl;
    if (f->h != INVALID_HANDLE_VALUE) FindClose(f->h);
    free(f);
    dh->impl = NULL;
}

static DDFileHandle fatx_open_read(DDStorage* s, const char* path, DWORD* sizeLo)
{
    HANDLE h;
    (void)s;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (sizeLo) *sizeLo = GetFileSize(h, NULL);
    return (DDFileHandle)h;
}

static DDFileHandle fatx_open_write(DDStorage* s, const char* path, int overwrite)
{
    HANDLE h;
    (void)s;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
        overwrite ? CREATE_ALWAYS : CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return (DDFileHandle)h;
}

static int fatx_read(DDStorage* s, DDFileHandle h, void* dst, DWORD bytes, DWORD* got)
{
    (void)s;
    return ReadFile((HANDLE)h, dst, bytes, got, NULL) ? 1 : 0;
}

static int fatx_write(DDStorage* s, DDFileHandle h, const void* src, DWORD bytes, DWORD* put)
{
    (void)s;
    return WriteFile((HANDLE)h, src, bytes, put, NULL) ? 1 : 0;
}

static void fatx_close(DDStorage* s, DDFileHandle h)
{
    (void)s;
    if (h) CloseHandle((HANDLE)h);
}

static int fatx_mkdir(DDStorage* s, const char* path)
{
    DWORD a;
    (void)s;
    a = GetFileAttributesA(path);
    if (a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    return CreateDirectoryA(path, NULL) ? 1 : 0;
}

static int fatx_rename(
    DDStorage* s,
    const char* oldPath,
    const char* newPath)
{
    (void)s;

    if (!oldPath || !oldPath[0] ||
        !newPath || !newPath[0])
    {
        return 0;
    }

    return MoveFileA(oldPath, newPath) ? 1 : 0;
}


static int fatx_remove_file(DDStorage* s, const char* path)
{
    (void)s;
    return DeleteFileA(path) ? 1 : 0;
}

static int fatx_remove_dir(DDStorage* s, const char* path)
{
    (void)s;
    return RemoveDirectoryA(path) ? 1 : 0;
}

static int fatx_exists(DDStorage* s, const char* path)
{
    (void)s;
    return GetFileAttributesA(path) != 0xFFFFFFFF;
}

void DDStorage_InitFatx(DDStorage* s, const char* label)
{
    ZeroMemory(s, sizeof(*s));
    s->label = label ? label : "XBOX";
    s->ready = fatx_ready;
    s->list_begin = fatx_list_begin;
    s->list_next = fatx_list_next;
    s->list_end = fatx_list_end;
    s->open_read = fatx_open_read;
    s->open_write = fatx_open_write;
    s->read = fatx_read;
    s->write = fatx_write;
    s->close = fatx_close;
    s->mkdir = fatx_mkdir;
    s->rename = fatx_rename;
    s->remove_file = fatx_remove_file;
    s->remove_dir = fatx_remove_dir;
    s->exists = fatx_exists;
}

/* one-file incremental copy core; CopyJob layers tree walking on top */

/*
    Copy into a sibling temporary file first, then commit it into place only
    after the entire source has copied successfully.

    This matters for both backends:
      - FATX CREATE_ALWAYS would otherwise destroy an existing destination
        before we know the transfer can complete.
      - USB FAT32 publishes a writer on close; a cancelled/read-failed copy
        must not accidentally publish a partial replacement.

    The temporary and backup names are fixed 8.3-safe leaves in the same
    directory, so FAT32 rename remains same-parent as required.
*/
static struct
{
    DDStorage* srcFs;
    DDStorage* dstFs;
    DDFileHandle src;
    DDFileHandle dst;
    DWORD total;
    DWORD done;
    int active;
    int cancel;
    int result;

    char finalDst[DD_PATH_MAX];
    char tempDst[DD_PATH_MAX];

    unsigned char buf[64 * 1024];
} g_copy;


static int copy_slen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}


static int copy_is_sep(char c)
{
    return c == '\\' || c == '/';
}


static char copy_fold(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - ('a' - 'A'));

    if (c == '/')
        c = '\\';

    return c;
}


static int copy_trim_len(const char* p)
{
    int n = copy_slen(p);

    while (n > 1 &&
        copy_is_sep(p[n - 1]) &&
        !(n == 3 && p[1] == ':'))
    {
        --n;
    }

    return n;
}


static int copy_path_equal(const char* a, const char* b)
{
    int na;
    int nb;
    int i;

    if (!a || !b)
        return 0;

    na = copy_trim_len(a);
    nb = copy_trim_len(b);

    if (na != nb)
        return 0;

    for (i = 0; i < na; ++i)
    {
        if (copy_fold(a[i]) != copy_fold(b[i]))
            return 0;
    }

    return 1;
}


static int copy_make_sidecar(
    DDStorage* fs,
    const char* finalPath,
    char kind,
    const char* ext,
    char* out,
    int cap)
{
    int len;
    int slash = -1;
    int parentLen;
    int attempt;

    if (!fs ||
        !fs->exists ||
        !finalPath ||
        !finalPath[0] ||
        !ext ||
        !out ||
        cap <= 0)
    {
        return 0;
    }

    len = copy_slen(finalPath);

    if (len >= DD_PATH_MAX)
        return 0;

    {
        int i;
        for (i = 0; i < len; ++i)
        {
            if (copy_is_sep(finalPath[i]))
                slash = i;
        }
    }

    parentLen = slash + 1;

    /*
        "U2XT0000.TMP" / "U2XB0000.BAK"
        12 characters plus NUL.
    */
    if (parentLen + 12 >= cap)
        return 0;

    for (attempt = 0; attempt < 10000; ++attempt)
    {
        char leaf[13];
        int v = attempt;
        int i;

        leaf[0] = 'U';
        leaf[1] = '2';
        leaf[2] = 'X';
        leaf[3] = kind;

        leaf[7] = (char)('0' + (v % 10)); v /= 10;
        leaf[6] = (char)('0' + (v % 10)); v /= 10;
        leaf[5] = (char)('0' + (v % 10)); v /= 10;
        leaf[4] = (char)('0' + (v % 10));

        leaf[8] = '.';
        leaf[9] = ext[0];
        leaf[10] = ext[1];
        leaf[11] = ext[2];
        leaf[12] = 0;

        if (parentLen)
            CopyMemory(out, finalPath, parentLen);

        for (i = 0; i < 12; ++i)
            out[parentLen + i] = leaf[i];

        out[parentLen + 12] = 0;

        if (!fs->exists(fs, out))
            return 1;
    }

    out[0] = 0;
    return 0;
}


static void copy_close_handles(void)
{
    if (g_copy.src &&
        g_copy.srcFs &&
        g_copy.srcFs->close)
    {
        g_copy.srcFs->close(
            g_copy.srcFs,
            g_copy.src);
    }

    if (g_copy.dst &&
        g_copy.dstFs &&
        g_copy.dstFs->close)
    {
        g_copy.dstFs->close(
            g_copy.dstFs,
            g_copy.dst);
    }

    g_copy.src = 0;
    g_copy.dst = 0;
}


static void copy_remove_temp(void)
{
    if (g_copy.tempDst[0] &&
        g_copy.dstFs &&
        g_copy.dstFs->remove_file)
    {
        g_copy.dstFs->remove_file(
            g_copy.dstFs,
            g_copy.tempDst);
    }
}


static int copy_commit_temp(void)
{
    int finalExists;
    char backup[DD_PATH_MAX];

    if (!g_copy.dstFs ||
        !g_copy.dstFs->exists ||
        !g_copy.dstFs->rename ||
        !g_copy.dstFs->remove_file)
    {
        copy_remove_temp();
        return 0;
    }

    finalExists =
        g_copy.dstFs->exists(
            g_copy.dstFs,
            g_copy.finalDst);

    backup[0] = 0;

    if (finalExists)
    {
        /*
            Destination must be a file.  If open_read cannot open it, decline
            replacement rather than risking renaming a directory out of place.
        */
        DDFileHandle probe;
        DWORD ignored = 0;

        probe = g_copy.dstFs->open_read(
            g_copy.dstFs,
            g_copy.finalDst,
            &ignored);

        if (!probe)
        {
            copy_remove_temp();
            return 0;
        }

        g_copy.dstFs->close(
            g_copy.dstFs,
            probe);

        if (!copy_make_sidecar(
            g_copy.dstFs,
            g_copy.finalDst,
            'B',
            "BAK",
            backup,
            sizeof(backup)))
        {
            copy_remove_temp();
            return 0;
        }

        if (!g_copy.dstFs->rename(
            g_copy.dstFs,
            g_copy.finalDst,
            backup))
        {
            copy_remove_temp();
            return 0;
        }
    }

    if (!g_copy.dstFs->rename(
        g_copy.dstFs,
        g_copy.tempDst,
        g_copy.finalDst))
    {
        if (backup[0])
        {
            g_copy.dstFs->rename(
                g_copy.dstFs,
                backup,
                g_copy.finalDst);
        }

        copy_remove_temp();
        return 0;
    }

    g_copy.tempDst[0] = 0;

    if (backup[0])
    {
        if (!g_copy.dstFs->remove_file(
            g_copy.dstFs,
            backup))
        {
            /*
                The new destination is already valid.  Return failure so a move
                will NOT delete its source when cleanup was incomplete.
            */
            return 0;
        }
    }

    return 1;
}


static void copy_finish(int result)
{
    copy_close_handles();

    if (result == 1)
    {
        if (!copy_commit_temp())
            result = -1;
    }
    else
    {
        copy_remove_temp();
    }

    g_copy.result = result;
    g_copy.active = 0;
}


int Fileops_CopyFileBegin(
    DDStorage* srcFs,
    const char* src,
    DDStorage* dstFs,
    const char* dst,
    int overwrite)
{
    DWORD size = 0;

    ZeroMemory(
        &g_copy,
        sizeof(g_copy));

    if (!srcFs ||
        !dstFs ||
        !src ||
        !src[0] ||
        !dst ||
        !dst[0] ||
        !srcFs->open_read ||
        !srcFs->read ||
        !srcFs->close ||
        !dstFs->open_read ||
        !dstFs->open_write ||
        !dstFs->write ||
        !dstFs->close ||
        !dstFs->exists ||
        !dstFs->rename ||
        !dstFs->remove_file)
    {
        return 0;
    }

    if (copy_slen(src) >= DD_PATH_MAX ||
        copy_slen(dst) >= DD_PATH_MAX)
    {
        return 0;
    }

    if (srcFs == dstFs &&
        copy_path_equal(src, dst))
    {
        return 0;
    }

    if (!overwrite &&
        dstFs->exists(
            dstFs,
            dst))
    {
        return 0;
    }

    if (!copy_make_sidecar(
        dstFs,
        dst,
        'T',
        "TMP",
        g_copy.tempDst,
        sizeof(g_copy.tempDst)))
    {
        return 0;
    }

    CopyMemory(
        g_copy.finalDst,
        dst,
        copy_slen(dst) + 1);

    g_copy.srcFs = srcFs;
    g_copy.dstFs = dstFs;

    g_copy.src = srcFs->open_read(
        srcFs,
        src,
        &size);

    if (!g_copy.src)
    {
        ZeroMemory(
            &g_copy,
            sizeof(g_copy));
        return 0;
    }

    g_copy.dst = dstFs->open_write(
        dstFs,
        g_copy.tempDst,
        0);

    if (!g_copy.dst)
    {
        srcFs->close(
            srcFs,
            g_copy.src);

        ZeroMemory(
            &g_copy,
            sizeof(g_copy));
        return 0;
    }

    g_copy.total = size;
    g_copy.active = 1;
    return 1;
}


int Fileops_CopyFilePump(DWORD budgetBytes)
{
    DWORD budget =
        budgetBytes ?
        budgetBytes :
        sizeof(g_copy.buf);

    if (!g_copy.active)
        return g_copy.result;

    while (budget &&
        g_copy.active)
    {
        DWORD want =
            (budget < sizeof(g_copy.buf)) ?
            budget :
            sizeof(g_copy.buf);

        DWORD got = 0;
        DWORD put = 0;

        if (g_copy.cancel)
        {
            copy_finish(-2);
            break;
        }

        if (!g_copy.srcFs->read(
            g_copy.srcFs,
            g_copy.src,
            g_copy.buf,
            want,
            &got))
        {
            copy_finish(-1);
            break;
        }

        if (!got)
        {
            copy_finish(1);
            break;
        }

        if (!g_copy.dstFs->write(
            g_copy.dstFs,
            g_copy.dst,
            g_copy.buf,
            got,
            &put) ||
            put != got)
        {
            copy_finish(-1);
            break;
        }

        g_copy.done += got;
        budget -= got;
    }

    return g_copy.result;
}


void Fileops_CopyFileCancel(void)
{
    if (g_copy.active)
        g_copy.cancel = 1;
}


int Fileops_CopyFileActive(void)
{
    return g_copy.active;
}


void Fileops_CopyFileProgress(
    DWORD* done,
    DWORD* total)
{
    if (done)
        *done = g_copy.done;

    if (total)
        *total = g_copy.total;
}


int Fileops_CopyFileResult(void)
{
    return g_copy.result;
}

/* -------------------------------------------------------------------------
   Recursive delete, following DarkDash's Fileops_Delete ownership model but
   expressed through DDStorage so FATX and USB FAT32 share one operation path.
   ------------------------------------------------------------------------- */
#define FILEOPS_DELETE_MAX_DEPTH 40

static int fileops_is_dot(const char* n)
{
    return n &&
        n[0] == '.' &&
        (n[1] == 0 ||
            (n[1] == '.' && n[2] == 0));
}


static void fileops_join(
    char* out,
    int cap,
    const char* base,
    const char* leaf)
{
    int n = 0;
    int i = 0;

    if (!out || cap <= 0)
        return;

    while (base && base[n] && n < cap - 1)
    {
        out[n] = base[n];
        ++n;
    }

    if (n > 0 &&
        out[n - 1] != '\\' &&
        out[n - 1] != '/' &&
        n < cap - 1)
    {
        out[n++] = '\\';
    }

    while (leaf && leaf[i] && n < cap - 1)
        out[n++] = leaf[i++];

    out[n] = 0;
}


static int fileops_delete_rec(
    DDStorage* fs,
    const char* path,
    int isDir,
    int depth)
{
    int ok = 1;

    if (!fs || !path || !path[0])
        return 0;

    if (!isDir)
    {
        if (!fs->remove_file)
            return 0;

        return fs->remove_file(
            fs,
            path);
    }

    if (depth >= FILEOPS_DELETE_MAX_DEPTH ||
        !fs->remove_dir)
    {
        return 0;
    }

    if (fs->list_begin &&
        fs->list_next &&
        fs->list_end)
    {
        DDDirHandle dh;
        DDDirEntry e;
        int have;

        ZeroMemory(&dh, sizeof(dh));
        ZeroMemory(&e, sizeof(e));

        have = fs->list_begin(
            fs,
            path,
            &dh,
            &e);

        while (have)
        {
            if (!fileops_is_dot(e.name))
            {
                char child[DD_PATH_MAX];

                fileops_join(
                    child,
                    sizeof(child),
                    path,
                    e.name);

                if (!fileops_delete_rec(
                    fs,
                    child,
                    e.isDir,
                    depth + 1))
                {
                    ok = 0;
                }
            }

            have = fs->list_next(
                fs,
                &dh,
                &e);
        }

        if (dh.impl)
            fs->list_end(fs, &dh);
    }

    if (!fs->remove_dir(
        fs,
        path))
    {
        ok = 0;
    }

    return ok;
}


int Fileops_DeletePath(
    DDStorage* fs,
    const char* path,
    int isDir)
{
    return fileops_delete_rec(
        fs,
        path,
        isDir ? 1 : 0,
        0);
}

