/*---------------------------------------------------------------------------
    fileman.cpp -- USB2XB dual-pane browser.

    Keeps the useful DarkDash FileMan model:
      - independent source/destination pane state
      - directories first, selectable name/size/date sort
      - marking
      - pane switching
      - directory navigation
      - async copy/move with progress
      - operation menu
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "fileman.h"
#include "dd_copyjob.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "usb2xb_assets.h"
#include "usb2xb_audio.h"
#include "dd_iso.h"
#include "font.h"
#include "input.h"
#include "dd_osk.h"
#include "dd_textviewer.h"
#include "dd_hexviewer.h"
#include "dd_checksum.h"

#include "usb2xb_usb.h"
#include "usb2xb_storage.h"

/*
    DarkDash/XbDiag HDD partition map.
    DarkDash implements this in dd_mount.cpp; USB2XB keeps the test copy local
    so the existing project only needs this file replaced.
*/
typedef struct _U2X_XBOX_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    char* Buffer;
} U2X_XBOX_STRING;

extern "C"
{
    LONG WINAPI IoCreateSymbolicLink(
        U2X_XBOX_STRING* SymbolicLinkName,
        U2X_XBOX_STRING* DeviceName);

    LONG WINAPI IoDeleteSymbolicLink(
        U2X_XBOX_STRING* SymbolicLinkName);

    LONG WINAPI NtOpenSymbolicLinkObject(
        HANDLE* LinkHandle,
        void* ObjectAttributes);

    LONG WINAPI NtQuerySymbolicLinkObject(
        HANDLE LinkHandle,
        U2X_XBOX_STRING* LinkTarget,
        ULONG* ReturnedLength);

    LONG WINAPI NtClose(HANDLE Handle);
}

/* Xbox OBJECT_ATTRIBUTES layout used by the object-manager calls below. */
typedef struct _U2X_OBJECT_ATTRIBUTES
{
    HANDLE RootDirectory;
    U2X_XBOX_STRING* ObjectName;
    ULONG Attributes;
} U2X_OBJECT_ATTRIBUTES;

#define U2X_OBJ_CASE_INSENSITIVE 0x00000040UL

typedef struct
{
    char letter;
    const char* device;
} U2X_DRIVE_MAP;

static const U2X_DRIVE_MAP kU2xHddDrives[] =
{
    { 'C', "\\Device\\Harddisk0\\Partition2" },
    { 'E', "\\Device\\Harddisk0\\Partition1" },
    { 'F', "\\Device\\Harddisk0\\Partition6" },
    { 'G', "\\Device\\Harddisk0\\Partition7" },
    { 'X', "\\Device\\Harddisk0\\Partition3" },
    { 'Y', "\\Device\\Harddisk0\\Partition4" },
    { 'Z', "\\Device\\Harddisk0\\Partition5" }
};

#define U2X_HDD_DRIVE_COUNT \
    ((int)(sizeof(kU2xHddDrives)/sizeof(kU2xHddDrives[0])))

static void U2x_MountHddPartitions(void)
{
    char linkBuf[8];
    int i;

    for (i = 0; i < U2X_HDD_DRIVE_COUNT; ++i)
    {
        const char* dev = kU2xHddDrives[i].device;
        int devLen = 0;
        U2X_XBOX_STRING sLink;
        U2X_XBOX_STRING sDev;

        while (dev[devLen])++devLen;

        /* "\\??\\X:" -- six counted characters plus NUL. */
        linkBuf[0] = '\\';
        linkBuf[1] = '?';
        linkBuf[2] = '?';
        linkBuf[3] = '\\';
        linkBuf[4] = kU2xHddDrives[i].letter;
        linkBuf[5] = ':';
        linkBuf[6] = 0;

        sLink.Length = 6;
        sLink.MaximumLength = 7;
        sLink.Buffer = linkBuf;

        sDev.Length = (USHORT)devLen;
        sDev.MaximumLength = (USHORT)(devLen + 1);
        sDev.Buffer = (char*)dev;

        /*
            Same DarkDash behavior: remove stale loader mappings first, then
            bind the known Xbox partition mapping. Missing F/G partitions are
            harmless; later drive probing simply omits them.
        */
        IoDeleteSymbolicLink(&sLink);
        IoCreateSymbolicLink(&sLink, &sDev);
    }
}

#define FM_MAX_ENTRIES 512
#define FM_PATH_MAX    DD_PATH_MAX
#define FM_NAME_MAX    DD_NAME_MAX

#define FM_VISIBLE_ROWS       14

/*
    DarkDash-style hold acceleration.  A fresh press always moves immediately;
    repeated movement ramps from a deliberate 240 ms cadence to 45 ms.
*/
#define FM_NAV_STEP_SLOW_MS   240
#define FM_NAV_STEP_FAST_MS    45
#define FM_NAV_RAMP_MS       1400

/*
    Selected long-name marquee.  Character-step scrolling keeps this compatible
    with the existing atlas renderer without changing font clipping/rendering.
*/
#define FM_MARQUEE_STEP_MS    115
#define FM_MARQUEE_HOLD_STEPS   7
#define FM_MARQUEE_GAP           4
#define FM_FILTER_MAX            64

typedef struct
{
    char name[FM_NAME_MAX];
    DWORD sizeLo;
    ULONGLONG sortTime;
    int isDir;
    int isDrive;
    char devPath[8];
    int marked;
} FmEntry;

typedef struct
{
    DDStorage* fs;
    char path[FM_PATH_MAX];
    char root[FM_PATH_MAX];
    FmEntry ent[FM_MAX_ENTRIES];
    int count;
    int cursor;
    int scroll;

    ULONGLONG freeBytes;
    ULONGLONG totalBytes;
    int spaceValid;
    int freeKnown;

    /* Current-directory, case-insensitive filename filter. */
    char filter[FM_FILTER_MAX];

    int sortMode;

    int marqueeCursor;
    DWORD marqueeStart;
} Pane;

enum
{
    FM_SORT_NAME = 0,
    FM_SORT_SIZE,
    FM_SORT_DATE,
    FM_SORT_COUNT
};

static Pane s_pane[2];
static int s_active = 0;
static char s_msg[80];
static int s_usbReadyLast = 0;
static int s_usbRawReadyLast = 0;

/* shared scratch; rendered immediately, so one buffer is enough */
static char s_marqueeText[FM_PATH_MAX * 2 + FM_MARQUEE_GAP + 8];

enum
{
    FM_BROWSE = 0,
    FM_OPS,
    FM_DESTPICK,
    FM_OSK_MKDIR,
    FM_OSK_RENAME,
    FM_OSK_FILTER,
    FM_CONFIRM_DELETE,
    FM_CONFIRM_FORMAT,
    FM_CONFIRM_EXIT,
    FM_FORMATTING,
    FM_COPYING,
    FM_COPY_CONFLICT
};
static int s_mode = FM_BROWSE;
static int s_opCursor = 0;
static int s_helperOpen = 0;
static int s_usbHotplugBootstrapped = 0;

/*
    DarkDash-style staged copy/move ("clipboard") state.
    Choosing Copy/Move records the source and moves focus to the destination.
    The actual CopyJob does not begin until WHITE is pressed in FM_DESTPICK.
*/
static int s_copyMove = 0;
static int s_pendSrc = 0;
static int s_pendDest = 1;

static char s_renameOldPath[FM_PATH_MAX];
static int s_renamePane = 0;

/*
    XBE launch state.

    Internal FATX XBEs launch directly after remapping D: to their containing
    directory. USB XBEs are staged inside USB2XB's own install directory at
    D:\USB2XB_STAGE. Only that private folder is purged between launches.
    At handoff D: is rebound to the staging folder's native device path.
*/
static int  s_launchStaging = 0;
static char s_launchXbeName[FM_NAME_MAX];
static char s_launchStageDir[FM_PATH_MAX];

/*
    Lazy XBE metadata cache used by the START details panel.  XBE metadata is
    deliberately not read while simply browsing: on USB 1.1 even a tiny read
    is visible latency, so we only parse the highlighted XBE when details are
    actually open and then reuse the result until a pane reload/path change.
*/
typedef struct _U2X_XBE_INFO
{
    DDStorage* fs;
    char path[FM_PATH_MAX];
    int attempted;
    int valid;

    char title[81];
    DWORD titleId;
    DWORD region;
    DWORD initFlags;
    DWORD discNumber;
    DWORD version;
} U2X_XBE_INFO;

static U2X_XBE_INFO s_xbeInfo;

/* Copy preflight is evaluated once, after expand-once preparation completes. */
static int s_copyPreflightChecked = 0;
static int s_copyPreflightNoSpace = 0;

static const char* kOpsUsb[] = {
    "New Folder",
    "Rename",
    "Delete",
    "Hex Viewer",
    "Checksums",
    "Filter",
    "Rescan USB",
    "Unmount USB",
    "Format USB",
    "Sort"
};

static const char* kOpsXbox[] = {
    "New Folder",
    "Rename",
    "Delete",
    "Hex Viewer",
    "Checksums",
    "Filter",
    "Sort"
};

static const char* sort_mode_label(int mode)
{
    switch (mode)
    {
    case FM_SORT_SIZE:
        return "Sort: Size";

    case FM_SORT_DATE:
        return "Sort: Date";

    default:
        return "Sort: Name";
    }
}

static int ops_count(void)
{
    return s_active == 0 ? 10 : 7;
}

static const char* ops_label(int i)
{
    if (s_active == 0)
    {
        if (i == 7)
        {
            return USB2XB_StorageUsbUserUnmounted() ?
                "Mount USB" :
                "Unmount USB";
        }

        if (i == 9)
            return sort_mode_label(s_pane[0].sortMode);

        if (i == 5 && s_pane[0].filter[0])
            return "Filter: Active";

        return kOpsUsb[i];
    }

    if (i == 6)
        return sort_mode_label(s_pane[1].sortMode);

    if (i == 5 && s_pane[1].filter[0])
        return "Filter: Active";

    return kOpsXbox[i];
}


static int slen(const char* s) { int n = 0; while (s && s[n])++n; return n; }
static void scpy(char* d, int cap, const char* s)
{
    int i = 0; if (cap <= 0)return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; ++i; }d[i] = 0;
}
static void scat(char* d, int cap, const char* s)
{
    int n = slen(d), i = 0;
    while (s && s[i] && n < cap - 1)d[n++] = s[i++];
    d[n] = 0;
}
static void join(char* out, int cap, const char* a, const char* b)
{
    scpy(out, cap, a);
    if (slen(out) && out[slen(out) - 1] != '\\' && out[slen(out) - 1] != '/')
        scat(out, cap, "\\");
    scat(out, cap, b);
}
static int join_checked(char* out, int cap, const char* a, const char* b)
{
    int na = slen(a);
    int nb = slen(b);
    int sep = 0;
    int total;
    int pos = 0;

    if (!out || cap <= 0 || !b || !b[0])
        return 0;

    if (na > 0 &&
        a[na - 1] != '\\' &&
        a[na - 1] != '/')
    {
        sep = 1;
    }

    total = na + sep + nb;

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

    if (sep)
        out[pos++] = '\\';

    if (nb)
    {
        CopyMemory(out + pos, b, nb);
        pos += nb;
    }

    out[pos] = 0;
    return 1;
}
static int namecmp(const char* a, const char* b)
{
    int i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z')ca -= 32;
        if (cb >= 'a' && cb <= 'z')cb -= 32;
        if (ca != cb)return ca < cb ? -1 : 1;
        if (!ca)return 0;
        ++i;
    }
}
static int namecontains(const char* text, const char* filter)
{
    int i;
    int j;

    if (!filter || !filter[0])
        return 1;

    if (!text)
        return 0;

    for (i = 0; text[i]; ++i)
    {
        for (j = 0; filter[j]; ++j)
        {
            char a = text[i + j];
            char b = filter[j];

            if (!a)
                return 0;

            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;

            if (a != b)
                break;
        }

        if (!filter[j])
            return 1;
    }

    return 0;
}

static int isdot(const char* n)
{
    return n && n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}
static void setmsg(const char* m) { scpy(s_msg, sizeof(s_msg), m); }


static int is_xbe_name(const char* name)
{
    int n;
    char a, b, c;

    if (!name)
        return 0;

    n = slen(name);
    if (n < 4 || name[n - 4] != '.')
        return 0;

    a = name[n - 3];
    b = name[n - 2];
    c = name[n - 1];

    if (a >= 'A' && a <= 'Z')a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z')b = (char)(b + ('a' - 'A'));
    if (c >= 'A' && c <= 'Z')c = (char)(c + ('a' - 'A'));

    return a == 'x' && b == 'b' && c == 'e';
}



#define U2X_XBE_SIGNATURE              0x48454258UL


static int U2x_ValidateXbe(
    DDStorage* fs,
    const char* path)
{
    DDFileHandle h;
    DWORD sizeLo = 0;
    DWORD sig = 0;
    DWORD got = 0;
    int ok = 0;

    if (!fs ||
        !path ||
        !path[0] ||
        !fs->open_read ||
        !fs->read ||
        !fs->close)
    {
        return 0;
    }

    h = fs->open_read(fs, path, &sizeLo);
    if (!h)
        return 0;

    if (sizeLo >= sizeof(sig) &&
        fs->read(fs, h, &sig, sizeof(sig), &got) &&
        got == sizeof(sig) &&
        sig == U2X_XBE_SIGNATURE)
    {
        ok = 1;
    }

    fs->close(fs, h);
    return ok;
}



#define U2X_XBE_FILE_HEADER_BYTES       0x128UL
#define U2X_XBE_IMAGEBASE_OFFSET        0x104UL
#define U2X_XBE_CERTPTR_OFFSET          0x118UL
#define U2X_XBE_INITFLAGS_OFFSET        0x124UL

/* Certificate fields needed by the details panel. */
#define U2X_XBE_CERT_INFO_BYTES         176UL
#define U2X_XBE_CERT_TITLEID_OFFSET     8UL
#define U2X_XBE_CERT_TITLENAME_OFFSET   12UL
#define U2X_XBE_CERT_REGION_OFFSET      160UL
#define U2X_XBE_CERT_DISC_OFFSET        168UL
#define U2X_XBE_CERT_VERSION_OFFSET     172UL

static DWORD U2x_ReadLe32(const BYTE* p)
{
    return ((DWORD)p[0]) |
        ((DWORD)p[1] << 8) |
        ((DWORD)p[2] << 16) |
        ((DWORD)p[3] << 24);
}

static int U2x_FileReadExact(
    DDStorage* fs,
    DDFileHandle h,
    void* dst,
    DWORD bytes)
{
    BYTE* out = (BYTE*)dst;
    DWORD done = 0;

    while (done < bytes)
    {
        DWORD got = 0;
        DWORD want = bytes - done;

        if (!fs->read(fs, h, out + done, want, &got) || got == 0)
            return 0;

        done += got;
    }

    return 1;
}

static int U2x_FileSkip(
    DDStorage* fs,
    DDFileHandle h,
    DWORD bytes)
{
    BYTE scratch[256];

    while (bytes)
    {
        DWORD chunk = bytes > sizeof(scratch) ?
            (DWORD)sizeof(scratch) : bytes;

        if (!U2x_FileReadExact(fs, h, scratch, chunk))
            return 0;

        bytes -= chunk;
    }

    return 1;
}

static void U2x_XbeInfoInvalidate(void)
{
    ZeroMemory(&s_xbeInfo, sizeof(s_xbeInfo));
}

static void U2x_XbeTitleFromCertificate(
    const BYTE* cert,
    char* out,
    int cap)
{
    int i;
    int pos = 0;

    if (!out || cap <= 0)
        return;

    out[0] = 0;

    /* XBE certificate title is WCHAR[40], little-endian on Xbox. */
    for (i = 0; i < 40 && pos < cap - 1; ++i)
    {
        BYTE lo = cert[U2X_XBE_CERT_TITLENAME_OFFSET + i * 2];
        BYTE hi = cert[U2X_XBE_CERT_TITLENAME_OFFSET + i * 2 + 1];
        unsigned short wc = (unsigned short)(lo | ((unsigned short)hi << 8));
        char ch;

        if (wc == 0)
            break;

        if (wc >= 32 && wc <= 126)
            ch = (char)wc;
        else if (wc == '\t' || wc == '\r' || wc == '\n')
            ch = ' ';
        else
            ch = '?';

        out[pos++] = ch;
    }

    while (pos > 0 && out[pos - 1] == ' ')
        --pos;

    out[pos] = 0;
}

static int U2x_ParseXbeInfo(
    DDStorage* fs,
    const char* path,
    U2X_XBE_INFO* info)
{
    DDFileHandle h;
    DWORD sizeLo = 0;
    BYTE hdr[U2X_XBE_FILE_HEADER_BYTES];
    BYTE cert[U2X_XBE_CERT_INFO_BYTES];
    DWORD imageBase;
    DWORD certVa;
    DWORD certOff;
    int ok = 0;

    if (!fs || !path || !path[0] || !info ||
        !fs->open_read || !fs->read || !fs->close)
    {
        return 0;
    }

    h = fs->open_read(fs, path, &sizeLo);
    if (!h)
        return 0;

    if (sizeLo < U2X_XBE_FILE_HEADER_BYTES ||
        !U2x_FileReadExact(fs, h, hdr, U2X_XBE_FILE_HEADER_BYTES) ||
        U2x_ReadLe32(hdr) != U2X_XBE_SIGNATURE)
    {
        fs->close(fs, h);
        return 0;
    }

    imageBase = U2x_ReadLe32(hdr + U2X_XBE_IMAGEBASE_OFFSET);
    certVa = U2x_ReadLe32(hdr + U2X_XBE_CERTPTR_OFFSET);

    if (certVa < imageBase)
    {
        fs->close(fs, h);
        return 0;
    }

    certOff = certVa - imageBase;
    if (certOff > sizeLo ||
        sizeLo - certOff < U2X_XBE_CERT_INFO_BYTES)
    {
        fs->close(fs, h);
        return 0;
    }

    if (certOff < U2X_XBE_FILE_HEADER_BYTES)
    {
        /* Rare but legal layout: reopen and walk from byte zero. */
        fs->close(fs, h);
        h = fs->open_read(fs, path, &sizeLo);
        if (!h)
            return 0;

        if (!U2x_FileSkip(fs, h, certOff))
        {
            fs->close(fs, h);
            return 0;
        }
    }
    else if (!U2x_FileSkip(
        fs,
        h,
        certOff - U2X_XBE_FILE_HEADER_BYTES))
    {
        fs->close(fs, h);
        return 0;
    }

    if (U2x_FileReadExact(fs, h, cert, U2X_XBE_CERT_INFO_BYTES))
    {
        U2x_XbeTitleFromCertificate(cert, info->title, sizeof(info->title));
        info->titleId = U2x_ReadLe32(cert + U2X_XBE_CERT_TITLEID_OFFSET);
        info->region = U2x_ReadLe32(cert + U2X_XBE_CERT_REGION_OFFSET);
        info->discNumber = U2x_ReadLe32(cert + U2X_XBE_CERT_DISC_OFFSET);
        info->version = U2x_ReadLe32(cert + U2X_XBE_CERT_VERSION_OFFSET);
        info->initFlags = U2x_ReadLe32(hdr + U2X_XBE_INITFLAGS_OFFSET);
        ok = 1;
    }

    fs->close(fs, h);
    return ok;
}

static const U2X_XBE_INFO* U2x_XbeInfoFor(
    DDStorage* fs,
    const char* path)
{
    if (!fs || !path || !path[0])
        return 0;

    if (s_xbeInfo.attempted &&
        s_xbeInfo.fs == fs &&
        namecmp(s_xbeInfo.path, path) == 0)
    {
        return s_xbeInfo.valid ? &s_xbeInfo : 0;
    }

    U2x_XbeInfoInvalidate();
    s_xbeInfo.fs = fs;
    scpy(s_xbeInfo.path, sizeof(s_xbeInfo.path), path);
    s_xbeInfo.attempted = 1;
    s_xbeInfo.valid = U2x_ParseXbeInfo(fs, path, &s_xbeInfo);

    return s_xbeInfo.valid ? &s_xbeInfo : 0;
}

static const char* U2x_DeviceForDrive(char letter)
{
    int i;

    if (letter >= 'a' && letter <= 'z')
        letter = (char)(letter - ('a' - 'A'));

    for (i = 0; i < U2X_HDD_DRIVE_COUNT; ++i)
    {
        if (kU2xHddDrives[i].letter == letter)
            return kU2xHddDrives[i].device;
    }

    return 0;
}


static int U2x_BuildNativeDirectory(
    const char* fatxDir,
    char* out,
    int cap)
{
    const char* dev;
    const char* rest;

    if (!fatxDir ||
        !fatxDir[0] ||
        fatxDir[1] != ':' ||
        !out ||
        cap <= 0)
    {
        return 0;
    }

    dev = U2x_DeviceForDrive(fatxDir[0]);
    if (!dev)
        return 0;

    scpy(out, cap, dev);
    rest = fatxDir + 2;

    if (rest[0])
    {
        int n = slen(out);

        if (rest[0] != '\\' && rest[0] != '/')
        {
            if (n + 1 >= cap)
                return 0;

            out[n++] = '\\';
            out[n] = 0;
        }

        if (slen(out) + slen(rest) >= cap)
            return 0;

        scat(out, cap, rest);
    }

    return 1;
}


static int U2x_MapDToFatxDirectory(const char* fatxDir)
{
    char nativeDir[FM_PATH_MAX + 64];
    char dLinkBuf[8];
    U2X_XBOX_STRING sLink;
    U2X_XBOX_STRING sDev;
    int devLen;
    LONG st;

    if (!U2x_BuildNativeDirectory(
        fatxDir,
        nativeDir,
        sizeof(nativeDir)))
    {
        return 0;
    }

    dLinkBuf[0] = '\\';
    dLinkBuf[1] = '?';
    dLinkBuf[2] = '?';
    dLinkBuf[3] = '\\';
    dLinkBuf[4] = 'D';
    dLinkBuf[5] = ':';
    dLinkBuf[6] = 0;

    sLink.Length = 6;
    sLink.MaximumLength = 7;
    sLink.Buffer = dLinkBuf;

    devLen = slen(nativeDir);
    sDev.Length = (USHORT)devLen;
    sDev.MaximumLength = (USHORT)(devLen + 1);
    sDev.Buffer = nativeDir;

    IoDeleteSymbolicLink(&sLink);
    st = IoCreateSymbolicLink(&sLink, &sDev);

    return st >= 0 ? 1 : 0;
}


static int U2x_LaunchFatxXbe(
    const char* fatxDir,
    const char* xbeName)
{
    char launchPath[FM_NAME_MAX + 4];
    DWORD rc;

    if (!fatxDir ||
        !fatxDir[0] ||
        !xbeName ||
        !xbeName[0] ||
        !is_xbe_name(xbeName))
    {
        return 0;
    }

    if (!U2x_MapDToFatxDirectory(fatxDir))
        return 0;

    scpy(launchPath, sizeof(launchPath), "D:\\");
    scat(launchPath, sizeof(launchPath), xbeName);

    setmsg("Launching XBE...");
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);

    rc = XLaunchNewImage(
        launchPath,
        0);

    /*
        A successful launch quick-reboots and does not return. Any return means
        the launch request itself failed.
    */
    return rc == ERROR_SUCCESS ? 1 : 0;
}


static void addentry(
    Pane* p,
    const char* name,
    int isDir,
    int isDrive,
    const char* devPath,
    DWORD sizeLo)
{
    FmEntry* e;

    if (!p || p->count >= FM_MAX_ENTRIES)
        return;

    e = &p->ent[p->count++];

    scpy(e->name, sizeof(e->name), name);
    e->sizeLo = sizeLo;
    e->sortTime = 0;
    e->isDir = isDir;
    e->isDrive = isDrive;
    e->devPath[0] = 0;

    if (devPath)
        scpy(e->devPath, sizeof(e->devPath), devPath);

    e->marked = 0;
}


/*
    Exact DarkDash file-manager model:
      path == "" on the Xbox pane means virtual drive root.
    Only letters that resolve as directories are shown.
*/
static void load_xbox_drive_list(Pane* p)
{
    static const char kLetters[] = { 'C','E','F','G','X','Y','Z',0 };
    int i;

    if (!p)
        return;

    p->count = 0;
    p->cursor = 0;
    p->scroll = 0;
    p->marqueeCursor = -1;
    p->marqueeStart = GetTickCount();
    p->path[0] = 0;

    for (i = 0; kLetters[i]; ++i)
    {
        char root[4];
        char label[3];
        DWORD attr;

        root[0] = kLetters[i];
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;

        attr = GetFileAttributesA(root);

        if (attr != 0xFFFFFFFF &&
            (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            label[0] = kLetters[i];
            label[1] = ':';
            label[2] = 0;

            addentry(
                p,
                label,
                1,
                1,
                root,
                0);
        }
    }
}


static int sort_compare(
    const Pane* p,
    const FmEntry* a,
    const FmEntry* b)
{
    int mode;

    if (a->isDir != b->isDir)
        return a->isDir ? -1 : 1;

    mode = p ? p->sortMode : FM_SORT_NAME;

    /*
        Size is only meaningful for files. Directories remain alphabetical
        while still grouped ahead of files.
    */
    if (mode == FM_SORT_SIZE &&
        !a->isDir &&
        a->sizeLo != b->sizeLo)
    {
        return a->sizeLo > b->sizeLo ? -1 : 1;
    }

    /*
        Date uses each backend's native sortable last-write value. Newest first.
        A zero timestamp simply falls through to the stable name tie-breaker.
    */
    if (mode == FM_SORT_DATE &&
        a->sortTime != b->sortTime)
    {
        return a->sortTime > b->sortTime ? -1 : 1;
    }

    return namecmp(a->name, b->name);
}


static void sortpane(Pane* p)
{
    int i;
    int j;

    if (!p)
        return;

    for (i = 1; i < p->count; ++i)
    {
        FmEntry t = p->ent[i];

        j = i - 1;

        while (j >= 0 &&
            sort_compare(
                p,
                &p->ent[j],
                &t) > 0)
        {
            p->ent[j + 1] = p->ent[j];
            --j;
        }

        p->ent[j + 1] = t;
    }
}


static void change_sort_mode(Pane* p, int direction)
{
    char selectedName[FM_NAME_MAX];
    int selectedIsDir = 0;
    int selectedIsDrive = 0;
    int hadSelection = 0;
    int i;

    if (!p)
        return;

    selectedName[0] = 0;

    if (p->cursor >= 0 &&
        p->cursor < p->count)
    {
        scpy(
            selectedName,
            sizeof(selectedName),
            p->ent[p->cursor].name);

        selectedIsDir =
            p->ent[p->cursor].isDir;

        selectedIsDrive =
            p->ent[p->cursor].isDrive;

        hadSelection = 1;
    }

    if (direction < 0)
    {
        --p->sortMode;
        if (p->sortMode < 0)
            p->sortMode = FM_SORT_COUNT - 1;
    }
    else
    {
        ++p->sortMode;
        if (p->sortMode >= FM_SORT_COUNT)
            p->sortMode = 0;
    }

    sortpane(p);

    if (hadSelection)
    {
        for (i = 0; i < p->count; ++i)
        {
            if (p->ent[i].isDir == selectedIsDir &&
                p->ent[i].isDrive == selectedIsDrive &&
                namecmp(
                    p->ent[i].name,
                    selectedName) == 0)
            {
                p->cursor = i;
                break;
            }
        }
    }

    if (p->count <= 0)
    {
        p->cursor = 0;
        p->scroll = 0;
    }
    else
    {
        if (p->cursor < 0)
            p->cursor = 0;

        if (p->cursor >= p->count)
            p->cursor = p->count - 1;

        if (p->cursor < p->scroll)
            p->scroll = p->cursor;

        if (p->cursor >=
            p->scroll + FM_VISIBLE_ROWS)
        {
            p->scroll =
                p->cursor -
                FM_VISIBLE_ROWS + 1;
        }

        if (p->scroll < 0)
            p->scroll = 0;
    }

    p->marqueeCursor = -1;
    p->marqueeStart = GetTickCount();
}

static void cycle_sort_mode(Pane* p)
{
    change_sort_mode(p, 1);
}

static int pane_ready(Pane* p)
{
    return p && p->fs && (!p->fs->ready || p->fs->ready(p->fs));
}

static void pane_space_refresh(Pane* p)
{
    if (!p)
        return;

    p->freeBytes = 0;
    p->totalBytes = 0;
    p->spaceValid = 0;
    p->freeKnown = 0;

    if (!pane_ready(p))
        return;

    if (p == &s_pane[0])
    {
        if (USB2XB_StorageUsbSpace(
            &p->freeBytes,
            &p->totalBytes,
            &p->freeKnown))
        {
            p->spaceValid = 1;
        }
        return;
    }

    /* The Xbox virtual drive-list root has no single capacity. */
    if (p == &s_pane[1] &&
        p->path[0] &&
        p->path[1] == ':')
    {
        char root[4];
        ULARGE_INTEGER avail;
        ULARGE_INTEGER total;
        ULARGE_INTEGER totalFree;

        root[0] = p->path[0];
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;

        ZeroMemory(&avail, sizeof(avail));
        ZeroMemory(&total, sizeof(total));
        ZeroMemory(&totalFree, sizeof(totalFree));

        if (GetDiskFreeSpaceEx(
            root,
            &avail,
            &total,
            &totalFree))
        {
            p->freeBytes = avail.QuadPart;
            p->totalBytes = total.QuadPart;
            p->spaceValid = 1;
            p->freeKnown = 1;
        }
    }
}

static int copy_destination_space(ULONGLONG* freeBytes, int* freeKnown)
{
    if (freeBytes)
        *freeBytes = 0;
    if (freeKnown)
        *freeKnown = 0;

    /* USB-app staging always targets USB2XB's own D: volume. */
    if (s_launchStaging)
    {
        ULARGE_INTEGER avail;
        ULARGE_INTEGER total;
        ULARGE_INTEGER totalFree;

        ZeroMemory(&avail, sizeof(avail));
        ZeroMemory(&total, sizeof(total));
        ZeroMemory(&totalFree, sizeof(totalFree));

        if (!GetDiskFreeSpaceEx(
            "D:\\",
            &avail,
            &total,
            &totalFree))
        {
            return 0;
        }

        if (freeBytes)
            *freeBytes = avail.QuadPart;
        if (freeKnown)
            *freeKnown = 1;
        return 1;
    }

    if (s_pendDest >= 0 && s_pendDest < 2)
    {
        Pane* p = &s_pane[s_pendDest];

        pane_space_refresh(p);

        if (p->spaceValid)
        {
            if (freeBytes)
                *freeBytes = p->freeBytes;
            if (freeKnown)
                *freeKnown = p->freeKnown;
            return 1;
        }
    }

    return 0;
}

static void loadpane(Pane* p)
{
    DDDirHandle h;
    DDDirEntry de;
    int more;

    /* A reload may replace an XBE at the same path; do not retain stale metadata. */
    U2x_XbeInfoInvalidate();

    p->count = 0;
    p->cursor = 0;
    p->scroll = 0;
    p->marqueeCursor = -1;
    p->marqueeStart = GetTickCount();
    p->freeBytes = 0;
    p->totalBytes = 0;
    p->spaceValid = 0;
    p->freeKnown = 0;

    /*
        DarkDash virtual root applies to the Xbox pane only.
        The USB pane continues to use its FAT32 backend/root unchanged.
    */
    if (p == &s_pane[1] && p->path[0] == 0)
    {
        load_xbox_drive_list(p);
        sortpane(p);
        return;
    }

    if (!pane_ready(p)) return;

    pane_space_refresh(p);

    if (!p->fs->list_begin) return;

    ZeroMemory(&h, sizeof(h));
    more = p->fs->list_begin(p->fs, p->path, &h, &de);
    while (more && p->count < FM_MAX_ENTRIES) {
        if (!isdot(de.name) &&
            namecontains(de.name, p->filter)) {
            FmEntry* e = &p->ent[p->count++];
            scpy(e->name, sizeof(e->name), de.name);
            e->sizeLo = de.sizeLo;
            e->sortTime = de.sortTime;
            e->isDir = de.isDir;
            e->isDrive = 0;
            e->devPath[0] = 0;
            e->marked = 0;
        }
        more = p->fs->list_next(p->fs, &h, &de);
    }
    if (h.impl) p->fs->list_end(p->fs, &h);
    sortpane(p);
}

static void entrypath(Pane* p, int idx, char* out, int cap)
{
    if (!p || idx < 0 || idx >= p->count)
    {
        out[0] = 0;
        return;
    }

    if (p->ent[idx].isDrive)
    {
        scpy(out, cap, p->ent[idx].devPath);
        return;
    }

    join(out, cap, p->path, p->ent[idx].name);
}

static void move(Pane* p, int d)
{
    int vis = FM_VISIBLE_ROWS;
    int oldCursor;

    if (!p || p->count <= 0)return;

    oldCursor = p->cursor;
    p->cursor += d;

    if (p->cursor < 0)p->cursor = 0;
    if (p->cursor >= p->count)p->cursor = p->count - 1;
    if (p->cursor < p->scroll)p->scroll = p->cursor;
    if (p->cursor >= p->scroll + vis)p->scroll = p->cursor - vis + 1;

    if (p->cursor != oldCursor)
    {
        p->marqueeCursor = -1;
        p->marqueeStart = GetTickCount();
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }
}

static void enter(Pane* p)
{
    char np[FM_PATH_MAX];
    if (!p || p->cursor < 0 || p->cursor >= p->count)return;
    if (!p->ent[p->cursor].isDir)return;
    entrypath(p, p->cursor, np, sizeof(np));
    scpy(p->path, sizeof(p->path), np);
    p->filter[0] = 0;
    loadpane(p);
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
}

static void up(Pane* p)
{
    int n, rootLen, last = -1, i;
    char parent[FM_PATH_MAX];

    if (!p)
        return;

    /*
        Match DarkDash: the Xbox pane has a virtual root above each drive.
    */
    if (p == &s_pane[1])
    {
        n = slen(p->path);

        if (n == 0)
            return;

        /* "E:\\" -> virtual drive list. */
        if (n <= 3 &&
            p->path[1] == ':' &&
            (p->path[2] == '\\' || p->path[2] == '/'))
        {
            p->path[0] = 0;
            p->filter[0] = 0;
            loadpane(p);
            USB2XB_AudioPlay(U2X_SOUND_BACK);
            return;
        }

        scpy(parent, sizeof(parent), p->path);

        for (i = 0; i < n; ++i)
            if (parent[i] == '\\' || parent[i] == '/')
                last = i;

        if (last <= 2)
        {
            parent[0] = p->path[0];
            parent[1] = ':';
            parent[2] = '\\';
            parent[3] = 0;
        }
        else
        {
            parent[last] = 0;
        }

        scpy(p->path, sizeof(p->path), parent);
        p->filter[0] = 0;
        loadpane(p);
        USB2XB_AudioPlay(U2X_SOUND_BACK);
        return;
    }

    /*
        USB pane retains its existing backend-specific root boundary.
    */
    n = slen(p->path);
    rootLen = slen(p->root);

    if (n <= rootLen)
        return;

    scpy(parent, sizeof(parent), p->path);

    for (i = rootLen; i < n; ++i)
        if (parent[i] == '\\' || parent[i] == '/')
            last = i;

    if (last < rootLen)
        scpy(parent, sizeof(parent), p->root);
    else
        parent[last] = 0;

    scpy(p->path, sizeof(p->path), parent);
    p->filter[0] = 0;
    loadpane(p);
    USB2XB_AudioPlay(U2X_SOUND_BACK);
}

static int copy_marked_count(const Pane* p)
{
    int i, n = 0, count;

    if (!p)
        return 0;

    count = p->count;
    if (count < 0)count = 0;
    if (count > FM_MAX_ENTRIES)count = FM_MAX_ENTRIES;

    for (i = 0; i < count; ++i)
    {
        if (p->ent[i].marked && !p->ent[i].isDrive)
            ++n;
    }

    return n;
}


static int source_has_selection(Pane* p)
{
    int count;

    if (!p)
        return 0;

    if (p == &s_pane[1] && p->path[0] == 0)
        return 0;

    if (copy_marked_count(p) > 0)
        return 1;

    count = p->count;
    if (count < 0)count = 0;
    if (count > FM_MAX_ENTRIES)count = FM_MAX_ENTRIES;

    if (p->cursor < 0 || p->cursor >= count)
        return 0;

    return p->ent[p->cursor].isDrive ? 0 : 1;
}


/*
    Build the top-level CopyJob list.

    IMPORTANT XBOX STACK RULE:
    The caller supplies a STATIC buffer.  CopyJobItem is large (path + name +
    storage metadata), so CopyJobItem[512] must never live on the Xbox thread
    stack.  This mirrors DarkDash's static CJ_BUILD_MAX staging array.
*/
static int builditems(Pane* p, CopyJobItem* items, int cap)
{
    int i, n = 0, useMarks, count;
    char path[FM_PATH_MAX];

    if (!p || !items || cap <= 0)
        return 0;

    count = p->count;
    if (count < 0)count = 0;
    if (count > FM_MAX_ENTRIES)count = FM_MAX_ENTRIES;

    useMarks = (copy_marked_count(p) > 0);

    for (i = 0; i < count && n < cap; ++i)
    {
        if (p->ent[i].isDrive)
            continue;

        if (useMarks)
        {
            if (!p->ent[i].marked)
                continue;
        }
        else if (i != p->cursor)
        {
            continue;
        }

        if (!join_checked(
            path,
            sizeof(path),
            p->path,
            p->ent[i].name))
        {
            setmsg("Source path too long");
            USB2XB_AudioPlay(U2X_SOUND_ERROR);
            return -1;
        }

        items[n].fs = p->fs;
        scpy(items[n].src, sizeof(items[n].src), path);
        scpy(items[n].name, sizeof(items[n].name), p->ent[i].name);
        items[n].isDir = p->ent[i].isDir;
        items[n].sizeLo = p->ent[i].isDir ? 0 : p->ent[i].sizeLo;
        ++n;
    }

    return n;
}



/*
    USB staged launch handoff.

    The selected USB application's containing directory is copied into a private
    folder inside USB2XB's own title directory:

        D:\USB2XB_STAGE\

    D: is USB2XB's current title-directory mapping while this process is alive,
    so this path follows USB2XB regardless of whether it was installed on E:, F:,
    G:, etc.  At handoff we query D:'s current native symbolic-link target, append
    USB2XB_STAGE, rebind D: to that native folder, and launch D:\<selected>.xbe.

    Only USB2XB_STAGE is ever purged; the containing application directory is
    left untouched.
*/
static int U2x_ClearFatxDirectoryContents(
    DDStorage* fs,
    const char* dir)
{
    int guard = 0;

    if (!fs || !dir || !dir[0] ||
        !fs->list_begin || !fs->list_next || !fs->list_end)
    {
        return 0;
    }

    for (;;)
    {
        DDDirHandle h;
        DDDirEntry de;
        char child[FM_PATH_MAX];
        int more;
        int found = 0;
        int isDir = 0;

        ZeroMemory(&h, sizeof(h));
        more = fs->list_begin(fs, dir, &h, &de);

        while (more)
        {
            if (!isdot(de.name))
            {
                if (!join_checked(
                    child,
                    sizeof(child),
                    dir,
                    de.name))
                {
                    if (h.impl)
                        fs->list_end(fs, &h);
                    return 0;
                }

                found = 1;
                isDir = de.isDir;
                break;
            }

            more = fs->list_next(fs, &h, &de);
        }

        if (h.impl)
            fs->list_end(fs, &h);

        if (!found)
            return 1;

        if (!Fileops_DeletePath(fs, child, isDir))
            return 0;

        /* Corrupt/cyclic directory protection. */
        if (++guard > 8192)
            return 0;
    }
}


static int U2x_EnsureLaunchStageDirectory(DDStorage* fs)
{
    DWORD attr;

    if (!fs || !fs->mkdir || !fs->exists)
        return 0;

    scpy(
        s_launchStageDir,
        sizeof(s_launchStageDir),
        "D:\\USB2XB_STAGE");

    attr = GetFileAttributesA(s_launchStageDir);

    if (attr == 0xFFFFFFFF)
    {
        if (!fs->mkdir(fs, s_launchStageDir))
            return 0;

        attr = GetFileAttributesA(s_launchStageDir);
    }

    if (attr == 0xFFFFFFFF ||
        !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        return 0;
    }

    return 1;
}


static int U2x_QueryCurrentDTarget(
    char* out,
    int cap)
{
    char linkBuf[] = "\\??\\D:";
    U2X_XBOX_STRING linkName;
    U2X_XBOX_STRING target;
    U2X_OBJECT_ATTRIBUTES oa;
    HANDLE hLink = 0;
    LONG st;
    int n;

    if (!out || cap <= 1)
        return 0;

    out[0] = 0;

    linkName.Length = 6;
    linkName.MaximumLength = 7;
    linkName.Buffer = linkBuf;

    oa.RootDirectory = 0;
    oa.ObjectName = &linkName;
    oa.Attributes = U2X_OBJ_CASE_INSENSITIVE;

    st = NtOpenSymbolicLinkObject(
        &hLink,
        &oa);

    if (st < 0 || !hLink)
        return 0;

    target.Length = 0;
    target.MaximumLength = (USHORT)(cap - 1);
    target.Buffer = out;

    st = NtQuerySymbolicLinkObject(
        hLink,
        &target,
        0);

    NtClose(hLink);

    if (st < 0)
    {
        out[0] = 0;
        return 0;
    }

    n = (int)target.Length;
    if (n < 0 || n >= cap)
    {
        out[0] = 0;
        return 0;
    }

    out[n] = 0;
    return n > 0 ? 1 : 0;
}


static int U2x_BuildStageNativeDirectory(
    const char* currentDTarget,
    char* out,
    int cap)
{
    static const char stageLeaf[] = "\\USB2XB_STAGE";
    int n;

    if (!currentDTarget || !currentDTarget[0] || !out || cap <= 0)
        return 0;

    n = slen(currentDTarget);
    if (n + (int)sizeof(stageLeaf) > cap)
        return 0;

    scpy(out, cap, currentDTarget);
    scat(out, cap, stageLeaf);
    return 1;
}


static int U2x_MapDToNativeDirectory(
    const char* nativeDir)
{
    char linkBuf[] = "\\??\\D:";
    U2X_XBOX_STRING sLink;
    U2X_XBOX_STRING sDev;
    int devLen;

    if (!nativeDir || !nativeDir[0])
        return 0;

    sLink.Length = 6;
    sLink.MaximumLength = 7;
    sLink.Buffer = linkBuf;

    devLen = slen(nativeDir);
    sDev.Length = (USHORT)devLen;
    sDev.MaximumLength = (USHORT)(devLen + 1);
    sDev.Buffer = (char*)nativeDir;

    IoDeleteSymbolicLink(&sLink);
    return IoCreateSymbolicLink(&sLink, &sDev) == 0 ? 1 : 0;
}


static int U2x_LaunchSelfStage(
    const char* xbeName)
{
    char currentD[FM_PATH_MAX + 64];
    char nativeStage[FM_PATH_MAX + 64];
    char launchPath[FM_NAME_MAX + 4];
    DWORD rc;

    if (!xbeName || !xbeName[0] || !is_xbe_name(xbeName))
        return 0;

    /*
        USB2XB is already running with D: mapped to its application directory.
        Capture that exact native target, append the private staging folder,
        then use the same D:-remap + XLaunchNewImage path as the normal FATX
        launcher.  No image-path reconstruction or cache-drive special case.
    */
    if (!U2x_QueryCurrentDTarget(
        currentD,
        sizeof(currentD)) ||
        !U2x_BuildStageNativeDirectory(
            currentD,
            nativeStage,
            sizeof(nativeStage)))
    {
        return 0;
    }

    if (!U2x_MapDToNativeDirectory(nativeStage))
        return 0;

    scpy(launchPath, sizeof(launchPath), "D:\\");
    scat(launchPath, sizeof(launchPath), xbeName);

    setmsg("Launching XBE...");
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);

    rc = XLaunchNewImage(launchPath, NULL);

    /* Successful launch quick-reboots and never returns. Restore only on fail. */
    U2x_MapDToNativeDirectory(currentD);
    return rc == ERROR_SUCCESS ? 1 : 0;
}


static int start_usb_xbe_launch(
    Pane* p,
    const char* xbeName)
{
    static CopyJobItem launchItems[FM_MAX_ENTRIES];
    DDDirHandle h;
    DDDirEntry de;
    char xbePath[FM_PATH_MAX];
    int more;
    int n = 0;

    if (!p ||
        p != &s_pane[0] ||
        !xbeName ||
        !xbeName[0] ||
        !is_xbe_name(xbeName))
    {
        return 0;
    }

    if (!pane_ready(p) ||
        !p->path[0] ||
        !s_pane[1].fs)
    {
        setmsg("Storage not ready");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    /* Validate the selected source before clearing the launch cache. */
    if (!join_checked(
        xbePath,
        sizeof(xbePath),
        p->path,
        xbeName) ||
        !U2x_ValidateXbe(
            p->fs,
            xbePath))
    {
        setmsg("Invalid or unreadable XBE");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    /*
        Keep launch scratch data beside USB2XB itself, never on X/Y/Z. Create the
        private folder once, then purge only its contents on every launch.
    */
    if (!U2x_EnsureLaunchStageDirectory(s_pane[1].fs))
    {
        setmsg("Unable to create launch folder");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (!U2x_ClearFatxDirectoryContents(
        s_pane[1].fs,
        s_launchStageDir))
    {
        setmsg("Unable to clear launch folder");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    /*
        Stage the CONTENTS of the USB application's containing directory into
        D:\USB2XB_STAGE. Each top-level child becomes one CopyJob source item,
        so nested directories still use the normal proven recursive copy path.
    */
    ZeroMemory(launchItems, sizeof(launchItems));
    ZeroMemory(&h, sizeof(h));

    if (!p->fs->list_begin || !p->fs->list_next || !p->fs->list_end)
    {
        setmsg("USB directory unavailable");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    more = p->fs->list_begin(p->fs, p->path, &h, &de);
    while (more)
    {
        if (!isdot(de.name))
        {
            if (n >= FM_MAX_ENTRIES)
            {
                if (h.impl)
                    p->fs->list_end(p->fs, &h);
                setmsg("Too many launch files");
                USB2XB_AudioPlay(U2X_SOUND_ERROR);
                return 0;
            }

            launchItems[n].fs = p->fs;
            if (!join_checked(
                launchItems[n].src,
                sizeof(launchItems[n].src),
                p->path,
                de.name))
            {
                if (h.impl)
                    p->fs->list_end(p->fs, &h);
                setmsg("Launch source path too long");
                USB2XB_AudioPlay(U2X_SOUND_ERROR);
                return 0;
            }

            scpy(
                launchItems[n].name,
                sizeof(launchItems[n].name),
                de.name);
            launchItems[n].isDir = de.isDir;
            launchItems[n].sizeLo = de.isDir ? 0 : de.sizeLo;
            ++n;
        }

        more = p->fs->list_next(p->fs, &h, &de);
    }

    if (h.impl)
        p->fs->list_end(p->fs, &h);

    if (n <= 0)
    {
        setmsg("USB app folder is empty");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    scpy(
        s_launchXbeName,
        sizeof(s_launchXbeName),
        xbeName);

    if (!CopyJob_Begin(
        launchItems,
        n,
        s_pane[1].fs,
        s_launchStageDir,
        0))
    {
        const char* err = CopyJob_ErrorText();

        setmsg(
            (err && err[0]) ?
            err :
            "Unable to stage XBE");

        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    s_launchStaging = 1;
    s_copyPreflightChecked = 0;
    s_copyPreflightNoSpace = 0;
    s_copyMove = 0;
    s_pendSrc = 0;
    s_pendDest = 1;
    s_mode = FM_COPYING;

    setmsg("Preparing USB app...");
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
    return 1;
}


static int launch_selected_xbe(Pane* p)
{
    FmEntry* e;

    if (!p ||
        p->cursor < 0 ||
        p->cursor >= p->count)
    {
        return 0;
    }

    e = &p->ent[p->cursor];

    if (e->isDir ||
        e->isDrive ||
        !is_xbe_name(e->name))
    {
        return 0;
    }

    if (p == &s_pane[0])
    {
        start_usb_xbe_launch(p, e->name);
        return 1;
    }

    if (p == &s_pane[1])
    {
        if (!p->path[0])
        {
            setmsg("Open an Xbox drive first");
            USB2XB_AudioPlay(U2X_SOUND_ERROR);
            return 1;
        }

        if (!U2x_LaunchFatxXbe(
            p->path,
            e->name))
        {
            setmsg("Unable to launch XBE");
            USB2XB_AudioPlay(U2X_SOUND_ERROR);
        }

        return 1;
    }

    return 0;
}


/*
    Stage Copy/Move exactly like DarkDash:
      1. remember source pane + operation,
      2. move focus to the other pane,
      3. browse destination,
      4. WHITE pastes / starts the async CopyJob.
*/
static void stage_copy(int moveFlag)
{
    Pane* src = &s_pane[s_active];

    if (!pane_ready(src))
    {
        setmsg("Storage not ready");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    if (!source_has_selection(src))
    {
        setmsg("Nothing selected");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    s_copyMove = moveFlag ? 1 : 0;
    s_pendSrc = s_active;
    s_pendDest = s_active ^ 1;
    s_active = s_pendDest;
    s_mode = FM_DESTPICK;

    setmsg(
        s_copyMove ?
        "Move: choose destination, WHITE paste" :
        "Copy: choose destination, WHITE paste");

    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
}


/*
    Start the staged operation at the destination pane's CURRENT directory.

    Static on purpose: ~180 KB at 512 entries.  Putting this on the Xbox stack
    is enough to bugcheck/double-fault immediately.
*/
static int start_staged_copy(void)
{
    static CopyJobItem items[FM_MAX_ENTRIES];

    Pane* src = &s_pane[s_pendSrc];
    Pane* dst = &s_pane[s_active];
    int n;

    if (s_active == s_pendSrc)
    {
        setmsg("Choose other pane");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (!pane_ready(src) || !pane_ready(dst))
    {
        setmsg("Storage not ready");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (dst == &s_pane[1] && dst->path[0] == 0)
    {
        setmsg("Open an Xbox drive first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (!dst->path[0])
    {
        setmsg("Open destination first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (!dst->fs || !dst->fs->open_write || !dst->fs->write)
    {
        setmsg("Destination is read-only");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    n = builditems(src, items, FM_MAX_ENTRIES);

    if (n < 0)
        return 0;

    if (n == 0)
    {
        setmsg("Nothing selected");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    s_pendDest = s_active;

    if (CopyJob_Begin(
        items,
        n,
        dst->fs,
        dst->path,
        s_copyMove))
    {
        s_copyPreflightChecked = 0;
        s_copyPreflightNoSpace = 0;
        s_mode = FM_COPYING;
        setmsg(s_copyMove ? "Moving..." : "Copying...");
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
        return 1;
    }

    {
        const char* err = CopyJob_ErrorText();

        setmsg(
            (err && err[0]) ?
            err :
            "Unable to start job");
    }

    USB2XB_AudioPlay(U2X_SOUND_ERROR);
    return 0;
}

static int create_named_folder(
    Pane* p,
    const char* name)
{
    char path[FM_PATH_MAX];

    if (!p ||
        !name ||
        !name[0] ||
        !pane_ready(p) ||
        !p->fs ||
        !p->fs->mkdir)
    {
        setmsg("Folder create unavailable");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    /*
        Xbox virtual root is a drive list, not a writable directory.
        USB "/" remains a valid filesystem root.
    */
    if (p == &s_pane[1] &&
        p->path[0] == 0)
    {
        setmsg("Open an Xbox drive first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (!p->path[0])
    {
        setmsg("Open destination first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    join(
        path,
        sizeof(path),
        p->path,
        name);

    if (p->fs->exists &&
        p->fs->exists(
            p->fs,
            path))
    {
        setmsg("Name already exists");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (p->fs->mkdir(
        p->fs,
        path))
    {
        loadpane(p);
        setmsg("Folder created");
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
        return 1;
    }

    /*
        FAT32 creation is still intentionally 8.3-only at this stage.
        FATX accepts its normal native names.  The later LFN pass removes
        this USB-side limitation without changing the OSK/file-manager flow.
    */
    if (p == &s_pane[0])
        setmsg("Create failed (USB name?)");
    else
        setmsg("Folder create failed");

    USB2XB_AudioPlay(U2X_SOUND_ERROR);
    return 0;
}


static void begin_mkdir_osk(void)
{
    Pane* p = &s_pane[s_active];

    if (!p ||
        !pane_ready(p) ||
        !p->fs ||
        !p->fs->mkdir)
    {
        setmsg("Folder create unavailable");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    if (p == &s_pane[1] &&
        p->path[0] == 0)
    {
        setmsg("Open an Xbox drive first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    if (!p->path[0])
    {
        setmsg("Open destination first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    Osk_Open(
        OSK_TEXT,
        "",
        DD_FATX_NAME_MAX);

    s_mode = FM_OSK_MKDIR;
    setmsg("Enter folder name");
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
}

static void apply_filter(Pane* p, const char* text)
{
    char selectedName[FM_NAME_MAX];
    int selectedIsDir = 0;
    int selectedIsDrive = 0;
    int hadSelection = 0;
    int i;

    if (!p)
        return;

    selectedName[0] = 0;

    if (p->cursor >= 0 && p->cursor < p->count)
    {
        scpy(selectedName, sizeof(selectedName), p->ent[p->cursor].name);
        selectedIsDir = p->ent[p->cursor].isDir;
        selectedIsDrive = p->ent[p->cursor].isDrive;
        hadSelection = 1;
    }

    scpy(p->filter, sizeof(p->filter), text ? text : "");
    loadpane(p);

    if (hadSelection)
    {
        for (i = 0; i < p->count; ++i)
        {
            if (p->ent[i].isDir == selectedIsDir &&
                p->ent[i].isDrive == selectedIsDrive &&
                namecmp(p->ent[i].name, selectedName) == 0)
            {
                p->cursor = i;
                break;
            }
        }
    }

    if (p->cursor >= p->count && p->count > 0)
        p->cursor = p->count - 1;

    if (p->cursor < 0)
        p->cursor = 0;

    if (p->cursor < p->scroll)
        p->scroll = p->cursor;

    if (p->cursor >= p->scroll + FM_VISIBLE_ROWS)
        p->scroll = p->cursor - FM_VISIBLE_ROWS + 1;

    if (p->scroll < 0)
        p->scroll = 0;

    p->marqueeCursor = -1;
    p->marqueeStart = GetTickCount();
}

static void begin_filter_osk(void)
{
    Pane* p = &s_pane[s_active];

    if (!p || !pane_ready(p))
    {
        setmsg("Filter unavailable");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    if (p == &s_pane[1] && p->path[0] == 0)
    {
        setmsg("Open an Xbox drive first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    Osk_Open(
        OSK_TEXT,
        p->filter,
        FM_FILTER_MAX - 1);

    s_mode = FM_OSK_FILTER;
    setmsg("Filter text - blank clears");
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
}

static void begin_rename_osk(void)
{
    Pane* p = &s_pane[s_active];
    int count;
    char path[FM_PATH_MAX];

    if (!p ||
        !p->fs ||
        !p->fs->rename)
    {
        setmsg("Rename unavailable");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    count = p->count;

    if (count < 0)count = 0;
    if (count > FM_MAX_ENTRIES)count = FM_MAX_ENTRIES;

    if (p->cursor < 0 ||
        p->cursor >= count ||
        p->ent[p->cursor].isDrive)
    {
        setmsg("Nothing to rename");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    entrypath(
        p,
        p->cursor,
        path,
        sizeof(path));

    scpy(
        s_renameOldPath,
        sizeof(s_renameOldPath),
        path);

    s_renamePane = s_active;

    Osk_Open(
        OSK_TEXT,
        p->ent[p->cursor].name,
        DD_FATX_NAME_MAX);

    s_mode = FM_OSK_RENAME;
    setmsg("Rename item");
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
}


static int commit_rename(
    const char* newName)
{
    Pane* p = &s_pane[s_renamePane];
    char newPath[FM_PATH_MAX];

    if (!newName ||
        !newName[0] ||
        !p ||
        !p->fs ||
        !p->fs->rename)
    {
        setmsg("Rename failed");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    if (!p->path[0])
    {
        setmsg("Rename unavailable here");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    join(
        newPath,
        sizeof(newPath),
        p->path,
        newName);

    if (p->fs->exists &&
        p->fs->exists(
            p->fs,
            newPath))
    {
        /*
            Allow the backend to accept exact same-name/no-op cases, otherwise
            protect against replacing an existing item.
        */
        if (namecmp(
            s_renameOldPath,
            newPath) != 0)
        {
            setmsg("Name already exists");
            return 0;
        }
    }

    if (p->fs->rename(
        p->fs,
        s_renameOldPath,
        newPath))
    {
        loadpane(p);
        setmsg("Renamed");
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
        return 1;
    }

    if (s_renamePane == 0)
        setmsg("Rename failed");
    else
        setmsg("Rename failed");

    USB2XB_AudioPlay(U2X_SOUND_ERROR);
    return 0;
}


static void begin_format_confirm(void)
{
    if (s_active != 0)
    {
        setmsg("Format is USB only");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    if (!USB2XB_StorageUsbReady())
    {
        setmsg("USB FAT32 not ready");
        return;
    }

    s_mode = FM_CONFIRM_FORMAT;
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
    setmsg("Format USB? A=yes B=no");
}


static int delete_target_info(
    Pane* p,
    char* firstName,
    int firstNameCap)
{
    int i;
    int count;
    int marked = 0;
    int total = 0;

    if (firstName && firstNameCap > 0)
        firstName[0] = 0;

    if (!p || !p->fs)
        return 0;

    count = p->count;

    if (count < 0)count = 0;
    if (count > FM_MAX_ENTRIES)count = FM_MAX_ENTRIES;

    for (i = 0; i < count; ++i)
    {
        if (p->ent[i].marked &&
            !p->ent[i].isDrive)
        {
            ++marked;
        }
    }

    if (marked)
    {
        for (i = 0; i < count; ++i)
        {
            if (!p->ent[i].marked ||
                p->ent[i].isDrive)
            {
                continue;
            }

            if (total == 0 &&
                firstName &&
                firstNameCap > 0)
            {
                scpy(
                    firstName,
                    firstNameCap,
                    p->ent[i].name);
            }

            ++total;
        }

        return total;
    }

    if (p->cursor >= 0 &&
        p->cursor < count &&
        !p->ent[p->cursor].isDrive)
    {
        if (firstName && firstNameCap > 0)
        {
            scpy(
                firstName,
                firstNameCap,
                p->ent[p->cursor].name);
        }

        return 1;
    }

    return 0;
}


static void begin_delete_confirm(void)
{
    Pane* p = &s_pane[s_active];
    char firstName[FM_NAME_MAX];
    int count;

    count = delete_target_info(
        p,
        firstName,
        sizeof(firstName));

    if (count <= 0)
    {
        setmsg("Nothing to delete");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    s_mode = FM_CONFIRM_DELETE;
    setmsg("Confirm delete");
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
}


static void delete_selected(void)
{
    Pane* p = &s_pane[s_active];
    int i;
    int marked = 0;
    int done = 0;
    int failed = 0;
    int count;
    char path[FM_PATH_MAX];

    if (!p || !p->fs)
    {
        setmsg("Delete unavailable");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    count = p->count;
    if (count < 0)count = 0;
    if (count > FM_MAX_ENTRIES)count = FM_MAX_ENTRIES;

    for (i = 0; i < count; ++i)
    {
        if (p->ent[i].marked &&
            !p->ent[i].isDrive)
        {
            ++marked;
        }
    }

    /*
        Work backwards so list indices remain irrelevant while the backend
        mutates the directory. The pane is reloaded only after the operation.
    */
    for (i = count - 1; i >= 0; --i)
    {
        if (p->ent[i].isDrive)
            continue;

        if (marked)
        {
            if (!p->ent[i].marked)
                continue;
        }
        else if (i != p->cursor)
        {
            continue;
        }

        entrypath(
            p,
            i,
            path,
            sizeof(path));

        if (Fileops_DeletePath(
            p->fs,
            path,
            p->ent[i].isDir))
        {
            ++done;
        }
        else
        {
            ++failed;
        }
    }

    loadpane(p);

    if (done > 0 && !failed)
    {
        setmsg("Delete complete");
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
    }
    else if (done > 0)
    {
        setmsg("Delete partial");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
    }
    else
    {
        setmsg("Delete failed");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
    }
}

void FileMan_Init(DDStorage* left, const char* leftRoot,
    DDStorage* right, const char* rightRoot)
{
    ZeroMemory(s_pane, sizeof(s_pane));
    s_pane[0].fs = left;
    s_pane[1].fs = right;

    /*
        Same partition map used by DarkDash/XbDiag. Do this before the Xbox
        virtual-root probe so C/E/F/G/X/Y/Z resolve consistently.
    */
    U2x_MountHddPartitions();

    scpy(s_pane[0].root, sizeof(s_pane[0].root), leftRoot ? leftRoot : "/");
    scpy(s_pane[0].path, sizeof(s_pane[0].path), s_pane[0].root);

    /*
        DarkDash file manager: empty path == virtual drive list.
        root is also empty because B from any drive root returns here.
    */
    s_pane[1].root[0] = 0;
    s_pane[1].path[0] = 0;

    s_active = 0;
    s_mode = FM_BROWSE;
    s_copyMove = 0;
    s_pendSrc = 0;
    s_pendDest = 1;
    s_renameOldPath[0] = 0;
    s_renamePane = 0;
    s_launchStaging = 0;
    s_launchXbeName[0] = 0;
    s_launchStageDir[0] = 0;
    s_helperOpen = UI_IsWide() ? 1 : 0;
    s_usbHotplugBootstrapped = 0;
    Osk_Close();

    /*
        UI assets are ordinary PNG files stored with .dat names under D:\dat.
        Missing assets simply fall back to the procedural UI.
    */
    USB2XB_AssetsInit();

    /*
        UI sound is optional. Missing assets or audio init failure never block
        the file manager.
    */
    USB2XB_AudioInit();

    s_msg[0] = 0;

    s_usbRawReadyLast =
        (USB2XB_USB_State() == U2X_USB_READY &&
            USB2XB_USB_Fat32Detected()) ? 1 : 0;

    s_usbReadyLast =
        USB2XB_StorageUsbReady() ? 1 : 0;

    loadpane(&s_pane[0]);
    loadpane(&s_pane[1]);
}

void FileMan_Refresh(void)
{
    loadpane(&s_pane[0]);
    loadpane(&s_pane[1]);
}

/* ---- simplified controller mapping ------------------------------------- */

static WORD s_navHeldLast = 0;
static DWORD s_navRepeatAt = 0;
static DWORD s_navHoldStart = 0;

static int s_lsXLast = 0;
static int s_lsYLast = 0;
static DWORD s_lsRepeatAt = 0;
static DWORD s_lsHoldStart = 0;


static DWORD nav_repeat_interval(DWORD heldMs)
{
    DWORD span =
        FM_NAV_STEP_SLOW_MS -
        FM_NAV_STEP_FAST_MS;

    if (heldMs >= FM_NAV_RAMP_MS)
        return FM_NAV_STEP_FAST_MS;

    return
        FM_NAV_STEP_SLOW_MS -
        (heldMs * span) / FM_NAV_RAMP_MS;
}


static WORD vertical_nav_with_repeat(
    WORD pressed,
    WORD held)
{
    const WORD mask = (WORD)(BTN_DPAD_UP | BTN_DPAD_DOWN);
    WORD h = (WORD)(held & mask);
    WORD out = (WORD)(pressed & mask);
    DWORD now = GetTickCount();

    if (h != s_navHeldLast)
    {
        s_navHeldLast = h;
        s_navHoldStart = now;

        if (h)
            s_navRepeatAt = now + FM_NAV_STEP_SLOW_MS;
        else
            s_navRepeatAt = 0;
    }
    else if (h &&
        (LONG)(now - s_navRepeatAt) >= 0)
    {
        DWORD heldMs = now - s_navHoldStart;

        out |= h;
        s_navRepeatAt =
            now + nav_repeat_interval(heldMs);
    }

    return out;
}


static WORD left_stick_nav(
    int allowHorizontal,
    int* switchDir)
{
    int lx, ly, rx, ry;
    int y = 0;
    int x = 0;
    WORD out = 0;
    DWORD now = GetTickCount();
    const int threshold = 15000;

    if (switchDir)
        *switchDir = 0;

    GetSticks(lx, ly, rx, ry);

    if (ly > threshold)y = 1;
    else if (ly < -threshold)y = -1;

    if (lx > threshold)x = 1;
    else if (lx < -threshold)x = -1;

    if (y != s_lsYLast)
    {
        if (y > 0)out |= BTN_DPAD_UP;
        else if (y < 0)out |= BTN_DPAD_DOWN;

        s_lsYLast = y;
        s_lsHoldStart = now;

        if (y)
            s_lsRepeatAt = now + FM_NAV_STEP_SLOW_MS;
        else
            s_lsRepeatAt = 0;
    }
    else if (y &&
        (LONG)(now - s_lsRepeatAt) >= 0)
    {
        DWORD heldMs = now - s_lsHoldStart;

        out |= (y > 0) ? BTN_DPAD_UP : BTN_DPAD_DOWN;
        s_lsRepeatAt =
            now + nav_repeat_interval(heldMs);
    }

    if (allowHorizontal &&
        x != 0 &&
        x != s_lsXLast &&
        switchDir)
    {
        *switchDir = x;
    }

    s_lsXLast = x;
    return out;
}


static void page_move(Pane* p, int dir)
{
    int step = FM_VISIBLE_ROWS - 1;

    if (!p || p->count <= 0)
        return;

    move(p, dir > 0 ? step : -step);
}


/*
    Build a looping selected-name marquee.  Text that already fits is returned
    unchanged.  Long names pause at the beginning, then advance one character
    at a time and wrap through a short gap.
*/
static const char* marquee_name(
    Pane* p,
    int idx,
    const char* name,
    int fontSize,
    int maxW)
{
    DWORD now;
    DWORD elapsed;
    DWORD step;
    DWORD cycle;
    DWORD pos;
    int len;
    int i;
    int n = 0;

    if (!p || !name || !name[0] || maxW <= 0)
        return name ? name : "";

    if (Font_MeasureText(name, fontSize) <= maxW)
        return name;

    now = GetTickCount();

    if (p->marqueeCursor != idx)
    {
        p->marqueeCursor = idx;
        p->marqueeStart = now;
    }

    elapsed = now - p->marqueeStart;
    step = elapsed / FM_MARQUEE_STEP_MS;

    len = slen(name);

    if (len <= 0)
        return name;

    cycle =
        FM_MARQUEE_HOLD_STEPS +
        (DWORD)len +
        FM_MARQUEE_GAP;

    pos = step % cycle;

    if (pos < FM_MARQUEE_HOLD_STEPS)
        return name;

    pos -= FM_MARQUEE_HOLD_STEPS;

    s_marqueeText[0] = 0;

    if (pos < (DWORD)len)
    {
        const char* tail = name + (int)pos;

        scpy(
            s_marqueeText,
            sizeof(s_marqueeText),
            tail);

        for (i = 0;
            i < FM_MARQUEE_GAP &&
            n < (int)sizeof(s_marqueeText) - 1;
            ++i)
        {
            n = slen(s_marqueeText);

            if (n < (int)sizeof(s_marqueeText) - 1)
            {
                s_marqueeText[n] = ' ';
                s_marqueeText[n + 1] = 0;
            }
        }

        scat(
            s_marqueeText,
            sizeof(s_marqueeText),
            name);
    }
    else
    {
        int gapPos = (int)pos - len;
        int gapLeft = FM_MARQUEE_GAP - gapPos;

        if (gapLeft < 0)
            gapLeft = 0;

        for (i = 0;
            i < gapLeft &&
            n < (int)sizeof(s_marqueeText) - 1;
            ++i)
        {
            s_marqueeText[n++] = ' ';
        }

        s_marqueeText[n] = 0;

        scat(
            s_marqueeText,
            sizeof(s_marqueeText),
            name);
    }

    return s_marqueeText;
}


static void switch_pane(Pane** pp, int dir)
{
    int oldActive = s_active;

    if (dir < 0)
        s_active = 0;
    else if (dir > 0)
        s_active = 1;
    else
        s_active ^= 1;

    if (pp)
        *pp = &s_pane[s_active];

    if (s_active != oldActive)
        USB2XB_AudioPlay(U2X_SOUND_NAV);
}


static void toggle_mark(Pane* p)
{
    int markIndex;
    int safeCount;

    if (!p)
        return;

    markIndex = p->cursor;
    safeCount = p->count;

    if (safeCount < 0)safeCount = 0;
    if (safeCount > FM_MAX_ENTRIES)safeCount = FM_MAX_ENTRIES;

    if (markIndex < 0 || markIndex >= safeCount)
        return;

    if (p->ent[markIndex].isDrive)
    {
        setmsg("Open drive first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return;
    }

    p->ent[markIndex].marked =
        p->ent[markIndex].marked ? 0 : 1;

    setmsg(
        p->ent[markIndex].marked ?
        "Item selected" :
        "Item unselected");

    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
}


static int open_selected_hex(void)
{
    Pane* p = &s_pane[s_active];
    char path[FM_PATH_MAX];

    if (!p ||
        p->cursor < 0 ||
        p->cursor >= p->count ||
        p->ent[p->cursor].isDir ||
        p->ent[p->cursor].isDrive)
    {
        setmsg("Select a file first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    entrypath(p, p->cursor, path, sizeof(path));

    if (!HexViewer_Open(
        p->fs,
        path,
        p->ent[p->cursor].name))
    {
        setmsg("Unable to open hex viewer");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    s_mode = FM_BROWSE;
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
    return 1;
}


static int open_selected_checksum(void)
{
    Pane* p = &s_pane[s_active];
    char path[FM_PATH_MAX];

    if (!p ||
        p->cursor < 0 ||
        p->cursor >= p->count ||
        p->ent[p->cursor].isDir ||
        p->ent[p->cursor].isDrive)
    {
        setmsg("Select a file first");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    entrypath(p, p->cursor, path, sizeof(path));

    if (!ChecksumViewer_Open(
        p->fs,
        path,
        p->ent[p->cursor].name))
    {
        setmsg("Unable to calculate checksums");
        USB2XB_AudioPlay(U2X_SOUND_ERROR);
        return 0;
    }

    s_mode = FM_BROWSE;
    USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
    return 1;
}


static void run_context_action(int which)
{
    /*
        Xbox: New Folder / Rename / Delete / Hex Viewer / Checksums /
              Filter / Sort
        USB:  New Folder / Rename / Delete / Hex Viewer / Checksums / Filter /
              Rescan / Unmount|Mount / Format / Sort
    */
    if (which == 0)
    {
        begin_mkdir_osk();
        return;
    }

    if (which == 1)
    {
        begin_rename_osk();
        return;
    }

    if (which == 2)
    {
        begin_delete_confirm();
        return;
    }

    if (which == 3)
    {
        open_selected_hex();
        return;
    }

    if (which == 4)
    {
        open_selected_checksum();
        return;
    }

    if (which == 5)
    {
        begin_filter_osk();
        return;
    }

    if ((s_active == 0 && which == 9) ||
        (s_active == 1 && which == 6))
    {
        setmsg("Use LEFT / RIGHT to change sort");
        USB2XB_AudioPlay(U2X_SOUND_NAV);
        return;
    }

    if (s_active == 0 && which == 6)
    {
        s_usbHotplugBootstrapped = 1;
        USB2XB_USB_RequestScan();
        setmsg("USB scan started - hotplug enabled");
        s_mode = FM_BROWSE;
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
        return;
    }

    if (s_active == 0 && which == 7)
    {
        if (USB2XB_StorageUsbUserUnmounted())
        {
            USB2XB_StorageUsbRemount();

            scpy(
                s_pane[0].path,
                sizeof(s_pane[0].path),
                s_pane[0].root);
            s_pane[0].filter[0] = 0;

            setmsg("USB mount enabled");
        }
        else
        {
            USB2XB_StorageUsbUnmount();

            scpy(
                s_pane[0].path,
                sizeof(s_pane[0].path),
                s_pane[0].root);
            s_pane[0].filter[0] = 0;

            loadpane(&s_pane[0]);
            setmsg("USB unmounted");
        }

        s_mode = FM_BROWSE;
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
        return;
    }

    if (s_active == 0 && which == 8)
    {
        begin_format_confirm();
        return;
    }
}


int FileMan_Update(WORD pressed, WORD held)
{
    Pane* p = &s_pane[s_active];

    USB2XB_AudioPump();

    if (TextViewer_IsActive())
    {
        TextViewer_Update(pressed, held);
        return 0;
    }

    if (HexViewer_IsActive())
    {
        HexViewer_Update(pressed, held);
        return 0;
    }

    if (ChecksumViewer_IsActive())
    {
        ChecksumViewer_Update(pressed, held);
        return 0;
    }

    WORD nav = vertical_nav_with_repeat(pressed, held);
    int stickSwitch = 0;
    int usbRawReadyNow =
        (USB2XB_USB_State() == U2X_USB_READY &&
            USB2XB_USB_Fat32Detected()) ? 1 : 0;
    int usbReadyNow;

    /*
        An explicit Unmount remains sticky while the same device is present.
        A real unplug/replug is treated as a new mount event.
    */
    if (usbRawReadyNow &&
        !s_usbRawReadyLast &&
        USB2XB_StorageUsbUserUnmounted())
    {
        USB2XB_StorageUsbRemount();
    }

    s_usbRawReadyLast = usbRawReadyNow;
    usbReadyNow = USB2XB_StorageUsbReady() ? 1 : 0;

    if (usbReadyNow != s_usbReadyLast)
    {
        s_usbReadyLast = usbReadyNow;
        loadpane(&s_pane[0]);

        setmsg(
            usbReadyNow ?
            "USB FAT32 mounted" :
            "USB storage offline");
    }

    if (s_mode == FM_OSK_MKDIR ||
        s_mode == FM_OSK_RENAME ||
        s_mode == FM_OSK_FILTER)
    {
        int oskMode = s_mode;
        int r = Osk_Update(pressed);

        if (r == 1)
        {
            char name[FM_NAME_MAX];

            Osk_GetText(name, sizeof(name));
            Osk_Close();

            if (oskMode == FM_OSK_FILTER)
            {
                apply_filter(&s_pane[s_active], name);
                setmsg(name[0] ? "Filter applied" : "Filter cleared");
                USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
            }
            else if (name[0])
            {
                if (oskMode == FM_OSK_MKDIR)
                    create_named_folder(
                        &s_pane[s_active],
                        name);
                else
                    commit_rename(name);
            }
            else
            {
                setmsg(
                    oskMode == FM_OSK_MKDIR ?
                    "Folder name required" :
                    "Rename requires a name");
            }

            s_mode = FM_BROWSE;
        }
        else if (r == -1)
        {
            Osk_Close();
            s_mode = FM_BROWSE;
            setmsg(
                oskMode == FM_OSK_MKDIR ?
                "Folder create cancelled" :
                (oskMode == FM_OSK_RENAME ?
                    "Rename cancelled" :
                    "Filter cancelled"));
            USB2XB_AudioPlay(U2X_SOUND_BACK);
        }

        return 0;
    }

    if (s_mode == FM_CONFIRM_DELETE)
    {
        if (pressed & BTN_A)
        {
            delete_selected();
            s_mode = FM_BROWSE;
            return 0;
        }

        if ((pressed & BTN_B) ||
            (pressed & BTN_BACK))
        {
            s_mode = FM_BROWSE;
            setmsg("Delete cancelled");
            USB2XB_AudioPlay(U2X_SOUND_BACK);
            return 0;
        }

        return 0;
    }

    if (s_mode == FM_CONFIRM_FORMAT)
    {
        if (pressed & BTN_A)
        {
            if (USB2XB_StorageFormatUsbBegin())
            {
                s_mode = FM_FORMATTING;
                setmsg("Formatting USB...");
                USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
            }
            else
            {
                s_mode = FM_BROWSE;
                setmsg("Unable to start format");
                USB2XB_AudioPlay(U2X_SOUND_ERROR);
            }

            return 0;
        }

        if ((pressed & BTN_B) ||
            (pressed & BTN_BACK))
        {
            s_mode = FM_BROWSE;
            setmsg("Format cancelled");
            USB2XB_AudioPlay(U2X_SOUND_BACK);
        }

        return 0;
    }

    if (s_mode == FM_CONFIRM_EXIT)
    {
        if (pressed & BTN_A)
            return 1;

        if ((pressed & BTN_B) ||
            (pressed & BTN_BACK))
        {
            s_mode = FM_BROWSE;
            setmsg("Exit cancelled");
            USB2XB_AudioPlay(U2X_SOUND_BACK);
        }

        return 0;
    }

    if (s_mode == FM_FORMATTING)
    {
        int fr = USB2XB_StorageFormatUsbPump();

        if (fr == 1)
        {
            scpy(
                s_pane[0].path,
                sizeof(s_pane[0].path),
                s_pane[0].root);

            loadpane(&s_pane[0]);
            s_mode = FM_BROWSE;
            setmsg("USB format complete");
            USB2XB_AudioPlay(U2X_SOUND_COMPLETE);
        }
        else if (fr < 0)
        {
            s_mode = FM_BROWSE;
            setmsg("USB format failed");
            USB2XB_AudioPlay(U2X_SOUND_ERROR);
        }

        return 0;
    }

    if (s_mode == FM_COPYING)
    {
        int st;

        if ((pressed & BTN_B) ||
            (pressed & BTN_BACK))
        {
            CopyJob_Cancel();
        }

        st = CopyJob_Pump();

        /*
            CopyJob intentionally returns to the UI for one frame after the
            expand-once pass completes. At this point totalBytes is exact and
            no destination payload has been written yet, so this is the safe
            place to reject an oversized transfer.
        */
        if (st == CJ_RUNNING &&
            !CopyJob_Preparing() &&
            !s_copyPreflightChecked)
        {
            ULONGLONG required = CopyJob_TotalBytes();
            ULONGLONG freeBytes = 0;
            int freeKnown = 0;

            s_copyPreflightChecked = 1;

            if (copy_destination_space(&freeBytes, &freeKnown) &&
                freeKnown &&
                required > freeBytes)
            {
                s_copyPreflightNoSpace = 1;
                CopyJob_Cancel();
                return 0;
            }
        }

        if (st == CJ_CONFLICT)
        {
            /*
                The launch stage is cleared before copy begins, so a collision
                can only come from a race/stale cache entry. Overwrite it
                automatically rather than showing the normal copy conflict UI.
            */
            if (s_launchStaging)
            {
                if (CopyJob_ResolveConflict(
                    CJ_CONFLICT_OVERWRITE))
                {
                    return 0;
                }

                s_launchStaging = 0;
                s_mode = FM_BROWSE;
                setmsg("Launch staging failed");
                USB2XB_AudioPlay(U2X_SOUND_ERROR);
                return 0;
            }

            s_mode = FM_COPY_CONFLICT;
            return 0;
        }

        if (st != CJ_RUNNING)
        {
            s_mode = FM_BROWSE;

            if (s_launchStaging)
            {
                s_launchStaging = 0;

                if (st == CJ_DONE)
                {
                    /*
                        USB launch handoff:
                          1. app contents are staged in D:\\USB2XB_STAGE,
                          2. map D: to that folder's native device path,
                          3. launch D:\\<selected>.xbe.
                    */
                    if (!U2x_LaunchSelfStage(
                        s_launchXbeName))
                    {
                        setmsg("Unable to launch staged XBE");
                        USB2XB_AudioPlay(U2X_SOUND_ERROR);
                    }
                }
                else if (st == CJ_CANCELLED)
                {
                    if (s_copyPreflightNoSpace)
                    {
                        setmsg("Not enough free space");
                        USB2XB_AudioPlay(U2X_SOUND_ERROR);
                    }
                    else
                    {
                        setmsg("Launch cancelled");
                        USB2XB_AudioPlay(U2X_SOUND_BACK);
                    }
                    s_copyPreflightNoSpace = 0;
                }
                else
                {
                    const char* err = CopyJob_ErrorText();

                    setmsg(
                        (err && err[0]) ?
                        err :
                        "Launch staging failed");

                    USB2XB_AudioPlay(U2X_SOUND_ERROR);
                }

                loadpane(&s_pane[0]);
                loadpane(&s_pane[1]);

                return 0;
            }

            if (st == CJ_DONE)
            {
                if (CopyJob_SkippedCount() > 0)
                {
                    setmsg(
                        s_copyMove ?
                        "Move complete - items skipped" :
                        "Copy complete - items skipped");
                }
                else
                {
                    setmsg(
                        s_copyMove ?
                        "Move complete" :
                        "Copy complete");
                }

                USB2XB_AudioPlay(U2X_SOUND_COMPLETE);
            }
            else if (st == CJ_CANCELLED)
            {
                if (s_copyPreflightNoSpace)
                {
                    setmsg("Not enough free space");
                    USB2XB_AudioPlay(U2X_SOUND_ERROR);
                }
                else
                {
                    setmsg("Cancelled");
                    USB2XB_AudioPlay(U2X_SOUND_BACK);
                }
                s_copyPreflightNoSpace = 0;
            }
            else
            {
                const char* err = CopyJob_ErrorText();

                setmsg(
                    (err && err[0]) ?
                    err :
                    "Copy failed");

                USB2XB_AudioPlay(U2X_SOUND_ERROR);
            }

            loadpane(&s_pane[s_pendSrc]);

            if (s_pendDest != s_pendSrc)
                loadpane(&s_pane[s_pendDest]);
        }

        return 0;
    }


    if (s_mode == FM_COPY_CONFLICT)
    {
        if (pressed & BTN_A)
        {
            if (CopyJob_ResolveConflict(
                CJ_CONFLICT_OVERWRITE))
            {
                s_mode = FM_COPYING;
                setmsg("Overwrite selected");
                USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
            }

            return 0;
        }

        if (pressed & BTN_X)
        {
            if (CopyJob_ResolveConflict(
                CJ_CONFLICT_SKIP))
            {
                s_mode = FM_COPYING;
                setmsg("Skipped existing file");
                USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
            }

            return 0;
        }

        if ((pressed & BTN_B) ||
            (pressed & BTN_BACK))
        {
            CopyJob_ResolveConflict(
                CJ_CONFLICT_CANCEL);

            s_mode = FM_BROWSE;
            setmsg("Cancelled");
            USB2XB_AudioPlay(U2X_SOUND_BACK);

            loadpane(&s_pane[s_pendSrc]);

            if (s_pendDest != s_pendSrc)
                loadpane(&s_pane[s_pendDest]);

            return 0;
        }

        return 0;
    }

    if (s_mode == FM_OPS)
    {
        int count = ops_count();
        int sortRow = (s_active == 0) ? 9 : 6;
        int sortDir = 0;
        int stickSortDir = 0;

        nav |= left_stick_nav(1, &stickSortDir);

        if (nav & BTN_DPAD_UP)
        {
            --s_opCursor;

            if (s_opCursor < 0)
                s_opCursor = count - 1;

            USB2XB_AudioPlay(U2X_SOUND_NAV);
        }

        if (nav & BTN_DPAD_DOWN)
        {
            ++s_opCursor;

            if (s_opCursor >= count)
                s_opCursor = 0;

            USB2XB_AudioPlay(U2X_SOUND_NAV);
        }

        if ((pressed & BTN_B) ||
            (pressed & BTN_BACK) ||
            (pressed & BTN_X))
        {
            s_mode = FM_BROWSE;
            USB2XB_AudioPlay(U2X_SOUND_BACK);
            return 0;
        }

        if (s_opCursor == sortRow)
        {
            if (pressed & BTN_DPAD_LEFT)
                sortDir = -1;
            else if (pressed & BTN_DPAD_RIGHT)
                sortDir = 1;
            else if (stickSortDir < 0)
                sortDir = -1;
            else if (stickSortDir > 0)
                sortDir = 1;

            if (sortDir)
            {
                Pane* sortPane = &s_pane[s_active];

                change_sort_mode(sortPane, sortDir);
                setmsg(sort_mode_label(sortPane->sortMode));
                USB2XB_AudioPlay(U2X_SOUND_NAV);
                return 0;
            }
        }

        if (pressed & BTN_A)
            run_context_action(s_opCursor);

        return 0;
    }

    if (s_mode == FM_DESTPICK)
    {
        nav |= left_stick_nav(0, 0);

        if (nav & BTN_DPAD_UP)move(p, -1);
        if (nav & BTN_DPAD_DOWN)move(p, 1);

        if (pressed & BTN_LTRIG)page_move(p, -1);
        if (pressed & BTN_RTRIG)page_move(p, 1);

        if (pressed & BTN_A)
            enter(p);

        /*
            B remains the universal "go up" action even while choosing a
            destination. Back cancels the staged copy/move.
        */
        if (pressed & BTN_B)
            up(p);

        if (pressed & BTN_WHITE)
        {
            start_staged_copy();
            return 0;
        }

        if (pressed & BTN_BACK)
        {
            s_active = s_pendSrc;
            s_mode = FM_BROWSE;
            setmsg("Copy cancelled");
            USB2XB_AudioPlay(U2X_SOUND_BACK);
        }

        return 0;
    }

    nav |= left_stick_nav(1, &stickSwitch);

    if (nav & BTN_DPAD_UP)move(p, -1);
    if (nav & BTN_DPAD_DOWN)move(p, 1);

    /* D-pad left/right and fresh left-stick horizontal input select panes. */
    if (pressed & BTN_DPAD_LEFT)
        switch_pane(&p, -1);
    else if (pressed & BTN_DPAD_RIGHT)
        switch_pane(&p, 1);
    else if (stickSwitch < 0)
        switch_pane(&p, -1);
    else if (stickSwitch > 0)
        switch_pane(&p, 1);

    /* Triggers are now page navigation, not redundant pane switching. */
    if (pressed & BTN_LTRIG)page_move(p, -1);
    if (pressed & BTN_RTRIG)page_move(p, 1);

    if (pressed & BTN_A)
    {
        if (p->cursor >= 0 &&
            p->cursor < p->count &&
            !p->ent[p->cursor].isDir &&
            !p->ent[p->cursor].isDrive &&
            TextViewer_CanOpen(p->ent[p->cursor].name))
        {
            char viewPath[FM_PATH_MAX];

            entrypath(p, p->cursor, viewPath, sizeof(viewPath));

            if (TextViewer_Open(
                p->fs,
                viewPath,
                p->ent[p->cursor].name))
            {
                USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
            }
            else
            {
                setmsg("Unable to open text file");
                USB2XB_AudioPlay(U2X_SOUND_ERROR);
            }
            return 0;
        }

        if (launch_selected_xbe(p))
            return 0;

        enter(p);
    }

    if (pressed & BTN_B)
        up(p);

    if (pressed & BTN_Y)
        toggle_mark(p);

    if (pressed & BTN_WHITE)
    {
        stage_copy(0);
        return 0;
    }

    if (pressed & BTN_BLACK)
    {
        stage_copy(1);
        return 0;
    }

    if (pressed & BTN_X)
    {
        s_opCursor = 0;
        s_mode = FM_OPS;
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
        return 0;
    }

    if (pressed & BTN_START)
    {
        /*
            Hardware-proven bootstrap: do not touch the USB bus during app
            startup.  The first START press on the USB pane runs the proven
            manual scan.  Once that scan has run, the USB backend owns
            physical removal/reinsert hotplug for the rest of this session.
        */
        if (s_active == 0 && !s_usbHotplugBootstrapped)
        {
            s_usbHotplugBootstrapped = 1;
            USB2XB_USB_RequestScan();
            setmsg("USB scan started - hotplug enabled");
            USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
            return 0;
        }

        s_helperOpen = s_helperOpen ? 0 : 1;
        USB2XB_AudioPlay(U2X_SOUND_NAV);
        return 0;
    }

    if (pressed & BTN_BACK)
    {
        s_mode = FM_CONFIRM_EXIT;
        setmsg("Confirm exit");
        USB2XB_AudioPlay(U2X_SOUND_CONFIRM);
        return 0;
    }

    return 0;
}


static void u32toa(DWORD v, char* out, int cap)
{
    char tmp[16];
    int n = 0, i = 0;

    if (!out || cap <= 0)return;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }

    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (n > 0 && i < cap - 1)
        out[i++] = tmp[--n];
    out[i] = 0;
}

static void fmt_size(DWORD bytes, char* out, int cap)
{
    DWORD whole, frac;
    char a[16], b[8];

    if (bytes >= 1024u * 1024u * 1024u) {
        whole = bytes / (1024u * 1024u * 1024u);
        frac = (bytes % (1024u * 1024u * 1024u)) / (1024u * 1024u * 100u);
        u32toa(whole, a, sizeof(a));
        u32toa(frac, b, sizeof(b));
        scpy(out, cap, a); scat(out, cap, ".");
        if (frac < 10)scat(out, cap, "0");
        scat(out, cap, b); scat(out, cap, " GB");
    }
    else if (bytes >= 1024u * 1024u) {
        whole = bytes / (1024u * 1024u);
        frac = (bytes % (1024u * 1024u)) / (1024u * 100u);
        u32toa(whole, a, sizeof(a));
        u32toa(frac, b, sizeof(b));
        scpy(out, cap, a); scat(out, cap, ".");
        if (frac < 10)scat(out, cap, "0");
        scat(out, cap, b); scat(out, cap, " MB");
    }
    else if (bytes >= 1024u) {
        whole = bytes / 1024u;
        u32toa(whole, a, sizeof(a));
        scpy(out, cap, a); scat(out, cap, " KB");
    }
    else {
        u32toa(bytes, a, sizeof(a));
        scpy(out, cap, a); scat(out, cap, " B");
    }
}

static void u64toa(ULONGLONG v, char* out, int cap)
{
    char tmp[32];
    int n = 0, i = 0;

    if (!out || cap <= 0)
        return;

    if (v == 0)
    {
        out[0] = '0';
        out[1] = 0;
        return;
    }

    while (v && n < (int)sizeof(tmp))
    {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }

    while (n > 0 && i < cap - 1)
        out[i++] = tmp[--n];

    out[i] = 0;
}

static void fmt_space_value(ULONGLONG bytes, ULONGLONG unit,
    char* out, int cap)
{
    ULONGLONG whole;
    ULONGLONG frac;
    char a[32];
    char b[8];

    if (!out || cap <= 0 || unit == 0)
        return;

    whole = bytes / unit;
    frac = ((bytes % unit) * 10) / unit;

    u64toa(whole, a, sizeof(a));
    u64toa(frac, b, sizeof(b));
    scpy(out, cap, a);
    scat(out, cap, ".");
    scat(out, cap, b);
}

static void fmt_space_pair(const Pane* p, char* out, int cap)
{
    ULONGLONG unit;
    const char* suffix;
    char a[32];
    char b[32];

    if (!out || cap <= 0)
        return;

    out[0] = 0;

    if (!p || !p->spaceValid || p->totalBytes == 0)
        return;

    if (p->totalBytes >= (ULONGLONG)1024 * 1024 * 1024)
    {
        unit = (ULONGLONG)1024 * 1024 * 1024;
        suffix = " GB";
    }
    else if (p->totalBytes >= (ULONGLONG)1024 * 1024)
    {
        unit = (ULONGLONG)1024 * 1024;
        suffix = " MB";
    }
    else
    {
        unit = (ULONGLONG)1024;
        suffix = " KB";
    }

    if (p->freeKnown)
    {
        fmt_space_value(p->freeBytes, unit, a, sizeof(a));
        scpy(out, cap, a);
    }
    else
    {
        scpy(out, cap, "--");
    }

    fmt_space_value(p->totalBytes, unit, b, sizeof(b));
    scat(out, cap, "/");
    scat(out, cap, b);
    scat(out, cap, suffix);
}

static int marked_count(Pane* p)
{
    int i, n = 0, count;
    if (!p)return 0;

    count = p->count;
    if (count < 0)count = 0;
    if (count > FM_MAX_ENTRIES)count = FM_MAX_ENTRIES;

    for (i = 0; i < count; ++i)
        if (p->ent[i].marked)++n;
    return n;
}

static DWORD ui_panel(void)
{
    return UI_ARGB(226, 18, 14, 28);
}

static DWORD ui_panel_alt(void)
{
    return UI_ARGB(235, 25, 18, 38);
}

static DWORD ui_accent(void)
{
    return UI_ARGB(255, 194, 80, 255);
}

static DWORD ui_text_dim(void)
{
    return FONT_RGBA(156, 145, 174, 255);
}


static int logical_width(void)
{
    return UI_IsWide() ? 854 : 640;
}


static void draw_outline(
    float x, float y, float w, float h, DWORD c)
{
    UI_FillRect(x, y, w, 1, c);
    UI_FillRect(x, y + h - 1, w, 1, c);
    UI_FillRect(x, y, 1, h, c);
    UI_FillRect(x + w - 1, y, 1, h, c);
}


static void draw_text_shadow(
    IDirect3DDevice8* d,
    int x, int y,
    const char* text,
    int size,
    DWORD colour,
    int maxW)
{
    Font_DrawText(d, x + 1, y + 1, text, size, FONT_RGBA(0, 0, 0, 180), maxW);
    Font_DrawText(d, x, y, text, size, colour, maxW);
}


static void draw_text_right_shadow(
    IDirect3DDevice8* d,
    int x, int y,
    const char* text,
    int size,
    DWORD colour)
{
    Font_DrawTextRight(d, x + 1, y + 1, text, size, FONT_RGBA(0, 0, 0, 180));
    Font_DrawTextRight(d, x, y, text, size, colour);
}


/* forward declaration used by the hybrid ISO depth layer */
static void pane_layout(
    int idx,
    int* x,
    int* y,
    int* w,
    int* h);


/*
    Isometric presentation is reserved for transient overlays. Actions,
    confirmations and progress are rendered directly in ISO space, including
    their labels, selection rows and progress bars. The browser stays flat.
*/
static void iso_panel_begin(
    int x,
    int y,
    int w,
    int h,
    int strong)
{
    Iso_Begin();

    /*
        Keep enough angle to preserve USB2XB's isometric identity, but avoid
        the steeper perspective that made text and menu rows harder to read.
    */
    Iso_SetAngles(
        strong ?
        (UI_IsWide() ? 18.0f : 17.0f) :
        (UI_IsWide() ? 14.0f : 13.0f),
        strong ?
        (UI_IsWide() ? -10.0f : -9.0f) :
        (UI_IsWide() ? -7.0f : -6.0f));

    /*
        Two cheap projected shadow layers give the foreground window a much
        stronger sense of separation from the background without textures or
        blur passes.
    */
    Iso_FillRect(
        (float)(x + 15),
        (float)(y + 16),
        (float)w,
        (float)h,
        UI_ARGB(76, 0, 0, 0),
        0);

    Iso_FillRect(
        (float)(x + 8),
        (float)(y + 9),
        (float)w,
        (float)h,
        UI_ARGB(142, 0, 0, 0),
        0);

    /*
        Low-alpha perimeter feather.  The multisampled polygon edge does the
        real antialiasing; these two layers only soften the remaining harsh
        contrast at 480-line output.
    */
    Iso_FillRect(
        (float)(x - 2),
        (float)(y - 2),
        (float)(w + 4),
        (float)(h + 4),
        UI_ARGB(18, 118, 54, 154),
        1);

    Iso_FillRect(
        (float)(x - 1),
        (float)(y - 1),
        (float)(w + 2),
        (float)(h + 2),
        UI_ARGB(30, 164, 72, 211),
        1);

    /* main ISO surface */
    Iso_FillRect(
        (float)x,
        (float)y,
        (float)w,
        (float)h,
        UI_ARGB(250, 16, 10, 27),
        0);

    /* inner face */
    Iso_FillRect(
        (float)(x + 8),
        (float)(y + 9),
        (float)(w - 16),
        (float)(h - 18),
        UI_ARGB(216, 30, 19, 47),
        0);

    /* low-cost depth lip */
    Iso_FillRect(
        (float)(x + 10),
        (float)(y + h - 12),
        (float)(w - 20),
        5.0f,
        UI_ARGB(105, 4, 2, 9),
        0);

    /* accent rails */
    Iso_FillRect(
        (float)x,
        (float)y,
        (float)w,
        5.0f,
        UI_ARGB(242, 201, 85, 255),
        1);

    Iso_FillRect(
        (float)x,
        (float)y,
        4.0f,
        (float)h,
        UI_ARGB(142, 165, 64, 224),
        1);

    Iso_FillRect(
        (float)(x + w - 9),
        (float)(y + 14),
        5.0f,
        (float)(h - 28),
        UI_ARGB(72, 226, 135, 255),
        1);
}


static void iso_panel_end(void)
{
    Iso_End();
}


static void draw_backdrop(void)
{
    int vw = logical_width();

    /*
        Solid fallback first; asset draw is a no-op if sd.dat/hd.dat is absent.
    */
    UI_FillRect(0, 0, vw, 480, UI_ARGB(255, 7, 6, 12));
    USB2XB_AssetsDrawBackground();

    /*
        Keep the top/bottom utility rails dark enough for readable text over
        arbitrary background art.
    */
    UI_FillRect(0, 0, vw, 58, UI_ARGB(178, 9, 6, 16));
    UI_FillRect(0, 57, vw, 1, UI_ARGB(100, 194, 80, 255));
    UI_FillRect(0, 444, vw, 36, UI_ARGB(232, 10, 7, 17));
    UI_FillRect(0, 443, vw, 1, UI_ARGB(86, 194, 80, 255));

    /*
        Only a very light center wash now.  Individual panels provide their
        own contrast, so the background artwork remains part of the UI.
    */
    UI_FillRect(0, 58, vw, 386, UI_ARGB(24, 7, 6, 12));

    /*
        logo.dat replaces the old USB2XB title text.
        If it failed to load, retain a small text fallback.
    */
    USB2XB_AssetsDrawLogo();

    draw_text_right_shadow(
        Gfx_Device(),
        vw - 22, 22,
        Gfx_VideoModeStr(),
        FONT_SIZE_SMALL,
        ui_text_dim());
}


static void pane_layout(
    int idx,
    int* x, int* y, int* w, int* h)
{
    int vw = logical_width();
    int margin = UI_IsWide() ? 64 : 46;
    int gap = UI_IsWide() ? 14 : 10;
    int helperW = 0;
    int usable;
    int paneW;

    /*
        The background has strong side rails and a visible floor.  Keep the
        functional browser inside that central frame instead of covering the
        decorative edges from screen to screen.
    */
    if (s_helperOpen && UI_IsWide())
        helperW = 184;

    usable = vw - margin * 2;

    if (helperW > 0)
        usable -= helperW + gap;

    paneW = (usable - gap) / 2;

    *x = margin + (idx ? paneW + gap : 0);
    *y = 84;
    *w = paneW;
    *h = 344;
}


static void draw_mark_box(
    int x, int y, int marked, int selected)
{
    DWORD border = selected ? ui_accent() : UI_ARGB(255, 105, 88, 126);

    UI_FillRect(x, y, 11, 11, UI_ARGB(255, 12, 10, 18));
    draw_outline(x, y, 11, 11, border);

    if (marked)
        UI_FillRect(x + 3, y + 3, 5, 5, ui_accent());
}


static void draw_pane_flat(
    Pane* p,
    int idx,
    int active)
{
    IDirect3DDevice8* d = Gfx_Device();
    int x, y, w, h;
    int listY;
    int rowH = 18;
    int vis = FM_VISIBLE_ROWS;
    int row = 0;
    int i;
    int marks;
    char countText[48];
    char spaceText[48];
    char num[16];
    DWORD border = active ? ui_accent() : UI_ARGB(255, 65, 52, 82);
    const char* title = idx == 0 ? "USB" : "XBOX";

    pane_layout(idx, &x, &y, &w, &h);

    /* cheap layered panel shadow */
    UI_FillRect(x + 7, y + 8, w, h, UI_ARGB(72, 0, 0, 0));
    UI_FillRect(x + 4, y + 5, w, h, UI_ARGB(105, 0, 0, 0));

    /* translucent face so the background remains visible */
    UI_FillRect(
        x, y, w, h,
        active ?
        UI_ARGB(202, 24, 17, 36) :
        UI_ARGB(184, 16, 12, 25));

    draw_outline(x, y, w, h, border);
    draw_outline(
        x + 2, y + 2, w - 4, h - 4,
        active ?
        UI_ARGB(82, 225, 162, 255) :
        UI_ARGB(50, 99, 78, 117));

    UI_FillRect(x, y, w, 4, border);
    UI_FillRect(
        x + 1, y + 4, w - 2, 54,
        active ?
        UI_ARGB(62, 60, 31, 82) :
        UI_ARGB(48, 26, 19, 39));

    draw_text_shadow(
        d, x + 14, y + 12,
        title,
        FONT_SIZE_MEDIUM,
        active ? FONT_RGBA(244, 228, 255, 255) : FONT_RGBA(203, 190, 216, 255),
        70);

    if (s_mode == FM_DESTPICK)
    {
        const char* badge =
            idx == s_pendSrc ? "SOURCE" :
            idx == s_pendDest ? "DESTINATION" : "";

        if (badge[0])
        {
            draw_text_right_shadow(
                d,
                x + w - 14,
                y + 14,
                badge,
                FONT_SIZE_SMALL,
                idx == s_pendDest ? ui_accent() : ui_text_dim());
        }
    }
    else
    {
        fmt_space_pair(p, spaceText, sizeof(spaceText));

        if (spaceText[0])
        {
            draw_text_right_shadow(
                d,
                x + w - 14,
                y + 14,
                spaceText,
                FONT_SIZE_SMALL,
                active ?
                FONT_RGBA(210, 174, 239, 255) :
                ui_text_dim());
        }
        else if (active)
        {
            draw_text_right_shadow(
                d,
                x + w - 14,
                y + 14,
                "ACTIVE",
                FONT_SIZE_SMALL,
                FONT_RGBA(210, 174, 239, 255));
        }
    }

    Font_DrawTextEllipsis(
        d,
        x + 14,
        y + 36,
        p->path[0] ? p->path : (idx == 1 ? "DRIVES" : "/"),
        FONT_SIZE_SMALL,
        ui_text_dim(),
        w - 28);

    UI_FillRect(
        x + 12, y + 58, w - 24, 1,
        UI_ARGB(255, 52, 42, 67));

    if (!pane_ready(p))
    {
        Font_DrawText(
            d, x + 20, y + 92,
            idx == 0 ? "USB NOT MOUNTED" : "STORAGE OFFLINE",
            FONT_SIZE_MEDIUM,
            FONT_RED,
            w - 40);

        Font_DrawTextEllipsis(
            d, x + 20, y + 122,
            idx == 0 ?
            (!s_usbHotplugBootstrapped ?
                "Press START to scan USB" :
                (USB2XB_StorageUsbUserUnmounted() ?
                    "Unmounted - X Actions to mount" :
                    USB2XB_USB_StatusText())) :
            "Storage unavailable",
            FONT_SIZE_SMALL,
            ui_text_dim(),
            w - 40);
        return;
    }

    listY = y + 70;

    {
        int renderCount = p->count;

        if (renderCount < 0)renderCount = 0;
        if (renderCount > FM_MAX_ENTRIES)renderCount = FM_MAX_ENTRIES;

        for (i = p->scroll; i < renderCount && row < vis; ++i, ++row)
        {
            int ry = listY + row * rowH;
            int selected = (i == p->cursor);
            DWORD nameColour =
                p->ent[i].isDir ?
                FONT_RGBA(224, 205, 255, 255) :
                FONT_WHITE;
            char sizeText[32];
            int sizeW = 70;
            int nameW = w - 66 - sizeW;

            if (selected)
            {
                UI_FillRect(
                    x + 8, ry - 1, w - 16, rowH,
                    active ?
                    UI_ARGB(126, 121, 54, 174) :
                    UI_ARGB(72, 65, 46, 83));

                UI_FillRect(
                    x + 11, ry, w - 22, 1,
                    active ?
                    UI_ARGB(88, 235, 181, 255) :
                    UI_ARGB(42, 164, 138, 184));

                UI_FillRect(
                    x + 11, ry + rowH - 2, w - 22, 1,
                    UI_ARGB(95, 0, 0, 0));

                if (active)
                {
                    UI_FillRect(x + 8, ry - 1, 3, rowH, ui_accent());
                    UI_FillRect(
                        x + 11, ry - 1, 28, 2,
                        UI_ARGB(180, 228, 142, 255));
                }
            }

            draw_mark_box(
                x + 16,
                ry + 3,
                p->ent[i].marked,
                selected && active);

            if (p->ent[i].isDir)
            {
                UI_FillRect(
                    x + 33, ry + 5, 10, 7,
                    UI_ARGB(255, 171, 126, 225));
                UI_FillRect(
                    x + 35, ry + 3, 6, 3,
                    UI_ARGB(255, 171, 126, 225));
            }
            else
            {
                draw_outline(
                    x + 34, ry + 3, 9, 11,
                    UI_ARGB(255, 112, 94, 132));
            }

            {
                const char* drawName = p->ent[i].name;
                int useMarquee =
                    selected &&
                    active &&
                    Font_MeasureText(
                        p->ent[i].name,
                        FONT_SIZE_SMALL) > nameW;

                if (useMarquee)
                {
                    drawName = marquee_name(
                        p,
                        i,
                        p->ent[i].name,
                        FONT_SIZE_SMALL,
                        nameW);
                }

                if (selected)
                {
                    if (useMarquee)
                    {
                        Font_DrawText(
                            d,
                            x + 51,
                            ry + 2,
                            drawName,
                            FONT_SIZE_SMALL,
                            FONT_RGBA(0, 0, 0, 190),
                            nameW);
                    }
                    else
                    {
                        Font_DrawTextEllipsis(
                            d,
                            x + 51,
                            ry + 2,
                            drawName,
                            FONT_SIZE_SMALL,
                            FONT_RGBA(0, 0, 0, 190),
                            nameW);
                    }
                }

                if (useMarquee)
                {
                    Font_DrawText(
                        d,
                        x + 50,
                        ry + 1,
                        drawName,
                        FONT_SIZE_SMALL,
                        nameColour,
                        nameW);
                }
                else
                {
                    Font_DrawTextEllipsis(
                        d,
                        x + 50,
                        ry + 1,
                        drawName,
                        FONT_SIZE_SMALL,
                        nameColour,
                        nameW);
                }
            }

            if (!p->ent[i].isDir)
            {
                fmt_size(
                    p->ent[i].sizeLo,
                    sizeText,
                    sizeof(sizeText));

                Font_DrawTextRight(
                    d,
                    x + w - 14,
                    ry + 1,
                    sizeText,
                    FONT_SIZE_SMALL,
                    FONT_DARKGRAY);
            }
        }
    }

    marks = marked_count(p);

    scpy(countText, sizeof(countText), "ITEMS ");
    u32toa((DWORD)p->count, num, sizeof(num));
    scat(countText, sizeof(countText), num);

    if (marks)
    {
        scat(countText, sizeof(countText), "   SELECTED ");
        u32toa((DWORD)marks, num, sizeof(num));
        scat(countText, sizeof(countText), num);
    }

    Font_DrawText(
        d,
        x + 14,
        y + h - 24,
        countText,
        FONT_SIZE_SMALL,
        marks ? FONT_RGBA(177, 154, 197, 255) : FONT_DARKGRAY,
        w - 28);
}



static void fmt_hex32(DWORD v, char* out, int cap)
{
    static const char kHex[] = "0123456789ABCDEF";
    char tmp[11];
    int i;

    if (!out || cap <= 0)
        return;

    tmp[0] = '0';
    tmp[1] = 'x';
    for (i = 0; i < 8; ++i)
        tmp[2 + i] = kHex[(v >> ((7 - i) * 4)) & 0x0F];
    tmp[10] = 0;

    scpy(out, cap, tmp);
}

static void fmt_xbe_region(DWORD region, char* out, int cap)
{
    int any = 0;

    if (!out || cap <= 0)
        return;

    out[0] = 0;

    if (region & 0x00000001UL)
    {
        scat(out, cap, "NA");
        any = 1;
    }
    if (region & 0x00000002UL)
    {
        if (any) scat(out, cap, "/");
        scat(out, cap, "JPN");
        any = 1;
    }
    if (region & 0x00000004UL)
    {
        if (any) scat(out, cap, "/");
        scat(out, cap, "ROW");
        any = 1;
    }
    if (region & 0x80000000UL)
    {
        if (any) scat(out, cap, "/");
        scat(out, cap, "MFG");
        any = 1;
    }

    if (!any)
        fmt_hex32(region, out, cap);
}

static void fmt_disc_version(
    DWORD disc,
    DWORD version,
    char* out,
    int cap)
{
    char a[16];
    char b[16];

    if (!out || cap <= 0)
        return;

    u32toa(disc, a, sizeof(a));
    u32toa(version, b, sizeof(b));

    scpy(out, cap, a);
    scat(out, cap, " / ");
    scat(out, cap, b);
}

static void draw_helper_panel(void)
{
    IDirect3DDevice8* d = Gfx_Device();
    Pane* p = &s_pane[s_active];
    int vw = logical_width();
    int x;
    int y = 84;
    int w;
    int h = 344;
    int valid = 0;
    int isXbe = 0;
    FmEntry* e = 0;
    const U2X_XBE_INFO* xi = 0;
    char path[FM_PATH_MAX];
    char sizeText[32];
    char valueText[64];

    path[0] = 0;

    if (!s_helperOpen)
        return;

    if (UI_IsWide())
    {
        w = 184;
        x = vw - 64 - w;
    }
    else
    {
        w = 248;
        x = vw - w - 46;

        UI_FillRect(
            0, 58, vw, 386,
            UI_ARGB(126, 0, 0, 0));
    }

    if (p &&
        p->cursor >= 0 &&
        p->cursor < p->count)
    {
        e = &p->ent[p->cursor];
        valid = 1;

        if (!e->isDir && !e->isDrive && is_xbe_name(e->name))
        {
            entrypath(p, p->cursor, path, sizeof(path));
            xi = U2x_XbeInfoFor(p->fs, path);
            isXbe = xi != 0;
        }
    }

    UI_FillRect(x + 7, y + 8, w, h, UI_ARGB(78, 0, 0, 0));
    UI_FillRect(x + 4, y + 5, w, h, UI_ARGB(110, 0, 0, 0));
    UI_FillRect(x, y, w, h, UI_ARGB(222, 16, 11, 26));
    draw_outline(x, y, w, h, UI_ARGB(255, 96, 69, 126));
    draw_outline(x + 2, y + 2, w - 4, h - 4, UI_ARGB(65, 218, 147, 255));
    UI_FillRect(x, y, w, 4, ui_accent());

    draw_text_shadow(
        d, x + 14, y + 14,
        isXbe ? "XBE DETAILS" :
        (s_active == 0 ? "USB DETAILS" : "XBOX DETAILS"),
        FONT_SIZE_MEDIUM,
        FONT_WHITE,
        w - 28);

    UI_FillRect(
        x + 14, y + 40, w - 28, 1,
        UI_ARGB(80, 194, 80, 255));

    if (valid)
    {
        if (!path[0])
        {
            entrypath(
                p,
                p->cursor,
                path,
                sizeof(path));
        }

        Font_DrawText(
            d, x + 14, y + 52,
            "NAME",
            FONT_SIZE_SMALL,
            FONT_RGBA(174, 139, 207, 255),
            w - 28);

        {
            const char* detailName = e->name;
            int detailW = w - 28;
            int useMarquee =
                Font_MeasureText(
                    e->name,
                    FONT_SIZE_SMALL) > detailW;

            if (useMarquee)
            {
                detailName = marquee_name(
                    p,
                    p->cursor,
                    e->name,
                    FONT_SIZE_SMALL,
                    detailW);

                Font_DrawText(
                    d, x + 15, y + 73,
                    detailName,
                    FONT_SIZE_SMALL,
                    FONT_RGBA(0, 0, 0, 180),
                    detailW);

                Font_DrawText(
                    d, x + 14, y + 72,
                    detailName,
                    FONT_SIZE_SMALL,
                    FONT_WHITE,
                    detailW);
            }
            else
            {
                Font_DrawTextEllipsis(
                    d, x + 15, y + 73,
                    detailName,
                    FONT_SIZE_SMALL,
                    FONT_RGBA(0, 0, 0, 180),
                    detailW);

                Font_DrawTextEllipsis(
                    d, x + 14, y + 72,
                    detailName,
                    FONT_SIZE_SMALL,
                    FONT_WHITE,
                    detailW);
            }
        }

        if (isXbe)
        {
            /* XBE-specific view: compact rows leave the helper controls intact. */
            Font_DrawText(
                d, x + 14, y + 102,
                "TITLE",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                w - 28);

            Font_DrawTextEllipsis(
                d, x + 14, y + 120,
                xi->title[0] ? xi->title : "(NO TITLE)",
                FONT_SIZE_SMALL,
                FONT_WHITE,
                w - 28);

            Font_DrawText(
                d, x + 14, y + 148,
                "TITLE ID",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                70);
            fmt_hex32(xi->titleId, valueText, sizeof(valueText));
            Font_DrawTextRight(
                d, x + w - 14, y + 148,
                valueText,
                FONT_SIZE_SMALL,
                FONT_WHITE);

            Font_DrawText(
                d, x + 14, y + 168,
                "REGION",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                64);
            fmt_xbe_region(xi->region, valueText, sizeof(valueText));
            {
                int regionX = x + 76;
                int regionW = (x + w - 14) - regionX;
                const char* regionText = valueText;

                if (Font_MeasureText(
                    valueText,
                    FONT_SIZE_SMALL) > regionW)
                {
                    /*
                        Use the same selected-entry marquee clock as the name.
                        Keeping the same cursor key avoids two marquee states
                        fighting each other every frame.
                    */
                    regionText = marquee_name(
                        p,
                        p->cursor,
                        valueText,
                        FONT_SIZE_SMALL,
                        regionW);

                    Font_DrawText(
                        d, regionX + 1, y + 169,
                        regionText,
                        FONT_SIZE_SMALL,
                        FONT_RGBA(0, 0, 0, 180),
                        regionW);

                    Font_DrawText(
                        d, regionX, y + 168,
                        regionText,
                        FONT_SIZE_SMALL,
                        FONT_WHITE,
                        regionW);
                }
                else
                {
                    Font_DrawTextRight(
                        d, x + w - 14, y + 168,
                        regionText,
                        FONT_SIZE_SMALL,
                        FONT_WHITE);
                }
            }

            Font_DrawText(
                d, x + 14, y + 188,
                "INIT",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                64);
            fmt_hex32(xi->initFlags, valueText, sizeof(valueText));
            Font_DrawTextRight(
                d, x + w - 14, y + 188,
                valueText,
                FONT_SIZE_SMALL,
                FONT_WHITE);

            Font_DrawText(
                d, x + 14, y + 208,
                "DISC / VER",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                82);
            fmt_disc_version(
                xi->discNumber,
                xi->version,
                valueText,
                sizeof(valueText));
            {
                int versionX = x + 104;
                int versionW = (x + w - 14) - versionX;
                const char* versionText = valueText;

                if (Font_MeasureText(
                    valueText,
                    FONT_SIZE_SMALL) > versionW)
                {
                    versionText = marquee_name(
                        p,
                        p->cursor,
                        valueText,
                        FONT_SIZE_SMALL,
                        versionW);

                    Font_DrawText(
                        d, versionX + 1, y + 209,
                        versionText,
                        FONT_SIZE_SMALL,
                        FONT_RGBA(0, 0, 0, 180),
                        versionW);

                    Font_DrawText(
                        d, versionX, y + 208,
                        versionText,
                        FONT_SIZE_SMALL,
                        FONT_WHITE,
                        versionW);
                }
                else
                {
                    Font_DrawTextRight(
                        d, x + w - 14, y + 208,
                        versionText,
                        FONT_SIZE_SMALL,
                        FONT_WHITE);
                }
            }

            fmt_size(
                e->sizeLo,
                sizeText,
                sizeof(sizeText));

            Font_DrawText(
                d, x + 14, y + 228,
                "SIZE",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                64);
            Font_DrawTextRight(
                d, x + w - 14, y + 228,
                sizeText,
                FONT_SIZE_SMALL,
                FONT_WHITE);
        }
        else
        {
            Font_DrawText(
                d, x + 14, y + 106,
                "TYPE",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                w - 28);

            draw_text_shadow(
                d, x + 14, y + 126,
                e->isDrive ? "DRIVE" :
                e->isDir ? "FOLDER" : "FILE",
                FONT_SIZE_SMALL,
                FONT_WHITE,
                w - 28);

            if (!e->isDir && !e->isDrive)
            {
                fmt_size(
                    e->sizeLo,
                    sizeText,
                    sizeof(sizeText));

                Font_DrawText(
                    d, x + 14, y + 158,
                    "SIZE",
                    FONT_SIZE_SMALL,
                    FONT_RGBA(174, 139, 207, 255),
                    w - 28);

                draw_text_shadow(
                    d, x + 14, y + 178,
                    sizeText,
                    FONT_SIZE_SMALL,
                    FONT_WHITE,
                    w - 28);
            }

            Font_DrawText(
                d, x + 14, y + 212,
                "PATH",
                FONT_SIZE_SMALL,
                FONT_RGBA(174, 139, 207, 255),
                w - 28);

            {
                int pathW = w - 28;
                const char* pathText = path;

                if (Font_MeasureText(
                    path,
                    FONT_SIZE_SMALL) > pathW)
                {
                    pathText = marquee_name(
                        p,
                        p->cursor,
                        path,
                        FONT_SIZE_SMALL,
                        pathW);

                    Font_DrawText(
                        d, x + 15, y + 233,
                        pathText,
                        FONT_SIZE_SMALL,
                        FONT_RGBA(0, 0, 0, 180),
                        pathW);

                    Font_DrawText(
                        d, x + 14, y + 232,
                        pathText,
                        FONT_SIZE_SMALL,
                        ui_text_dim(),
                        pathW);
                }
                else
                {
                    Font_DrawTextEllipsis(
                        d, x + 14, y + 232,
                        pathText,
                        FONT_SIZE_SMALL,
                        ui_text_dim(),
                        pathW);
                }
            }
        }
    }

    Font_DrawText(
        d, x + 14, y + h - 89,
        "Y  SELECT / UNSELECT",
        FONT_SIZE_SMALL,
        ui_text_dim(),
        w - 28);

    Font_DrawText(
        d, x + 14, y + h - 71,
        "WHITE  COPY / PASTE",
        FONT_SIZE_SMALL,
        ui_text_dim(),
        w - 28);

    Font_DrawText(
        d, x + 14, y + h - 53,
        "BLACK  MOVE",
        FONT_SIZE_SMALL,
        ui_text_dim(),
        w - 28);

    Font_DrawText(
        d, x + 14, y + h - 35,
        "LT / RT  PAGE",
        FONT_SIZE_SMALL,
        ui_text_dim(),
        w - 28);

    Font_DrawText(
        d, x + 14, y + h - 17,
        (s_active == 0 && !s_usbHotplugBootstrapped) ?
        "START  SCAN USB" :
        "START  HIDE DETAILS",
        FONT_SIZE_SMALL,
        FONT_RGBA(127, 112, 146, 255),
        w - 28);
}


static void draw_modal_box_buttons(
    const char* title,
    const char* line1,
    const char* line2,
    const char* buttons)
{
    IDirect3DDevice8* d = Gfx_Device();
    int vw = logical_width();
    int w = UI_IsWide() ? 560 : 500;
    int h = 190;
    int x = (vw - w) / 2;
    int y = 134;

    UI_FillRect(
        0, 58, vw, 386,
        UI_ARGB(148, 0, 0, 0));

    iso_panel_begin(
        x, y, w, h, 1);

    Font_DrawTextIso(
        d,
        (float)(x + 32),
        (float)(y + 26),
        title,
        FONT_SIZE_MEDIUM,
        FONT_WHITE);

    Iso_FillRect(
        (float)(x + 30),
        (float)(y + 52),
        (float)(w - 70),
        1.0f,
        UI_ARGB(86, 212, 110, 255),
        1);

    Font_DrawTextIsoClip(
        d,
        (float)(x + 32),
        (float)(y + 69),
        line1,
        FONT_SIZE_SMALL,
        FONT_RGBA(232, 218, 242, 255),
        (float)(w - 82));

    if (line2 && line2[0])
    {
        Font_DrawTextIsoClip(
            d,
            (float)(x + 32),
            (float)(y + 99),
            line2,
            FONT_SIZE_SMALL,
            FONT_RGBA(170, 156, 188, 255),
            (float)(w - 82));
    }

    Iso_FillRect(
        (float)(x + 26),
        (float)(y + h - 52),
        (float)(w - 62),
        34.0f,
        UI_ARGB(72, 7, 4, 12),
        0);

    Iso_FillRect(
        (float)(x + 30),
        (float)(y + h - 45),
        (float)(w - 70),
        1.0f,
        UI_ARGB(135, 193, 87, 246),
        1);

    Font_DrawTextIso(
        d,
        (float)(x + 32),
        (float)(y + h - 31),
        (buttons && buttons[0]) ?
        buttons :
        "A  CONFIRM        B  CANCEL",
        FONT_SIZE_SMALL,
        FONT_RGBA(218, 195, 237, 255));

    iso_panel_end();
}


static void draw_ops_overlay(void)
{
    IDirect3DDevice8* d = Gfx_Device();
    int vw = logical_width();
    int w = UI_IsWide() ? 420 : 360;
    int count = ops_count();
    int sortRow = (s_active == 0) ? 9 : 6;
    int rowH = count > 9 ? 28 : (count > 8 ? 32 : 36);
    int h = 98 + count * rowH;
    int x = (vw - w) / 2;
    int y = count > 9 ? 66 : 84;
    int i;

    UI_FillRect(
        0, 58, vw, 386,
        UI_ARGB(136, 0, 0, 0));

    iso_panel_begin(
        x, y, w, h, 1);

    Font_DrawTextIso(
        d,
        (float)(x + 32),
        (float)(y + 26),
        "ACTIONS",
        FONT_SIZE_MEDIUM,
        FONT_WHITE);

    Font_DrawTextIso(
        d,
        (float)(x + 32),
        (float)(y + 54),
        s_active == 0 ? "USB" : "XBOX",
        FONT_SIZE_SMALL,
        FONT_RGBA(181, 158, 204, 255));

    Iso_FillRect(
        (float)(x + 28),
        (float)(y + 76),
        (float)(w - 64),
        1.0f,
        UI_ARGB(78, 194, 80, 255),
        1);

    for (i = 0; i < count; ++i)
    {
        int ry = y + 86 + i * rowH;

        if (i == s_opCursor)
        {
            Iso_FillRect(
                (float)(x + 22),
                (float)(ry - 6),
                (float)(w - 54),
                30.0f,
                UI_ARGB(162, 136, 51, 194),
                1);

            Iso_FillRect(
                (float)(x + 25),
                (float)(ry - 4),
                (float)(w - 60),
                1.0f,
                UI_ARGB(90, 244, 190, 255),
                1);

            Iso_FillRect(
                (float)(x + 25),
                (float)(ry + 22),
                (float)(w - 60),
                1.0f,
                UI_ARGB(80, 0, 0, 0),
                0);

            Iso_FillRect(
                (float)(x + 22),
                (float)(ry - 6),
                4.0f,
                30.0f,
                UI_ARGB(250, 226, 122, 255),
                1);
        }

        Font_DrawTextIso(
            d,
            (float)(x + 40),
            (float)ry,
            ops_label(i),
            FONT_SIZE_MEDIUM,
            i == s_opCursor ?
            FONT_WHITE :
            FONT_RGBA(208, 194, 222, 255));

        if (i == sortRow)
        {
            Font_DrawTextRight(
                d,
                (float)(x + w - 34),
                (float)(ry + 2),
                "<  >",
                FONT_SIZE_SMALL,
                i == s_opCursor ?
                FONT_RGBA(250, 226, 122, 255) :
                FONT_RGBA(150, 132, 166, 255));
        }
    }

    iso_panel_end();
}


static void draw_progress_box(
    const char* title,
    const char* name,
    DWORD done,
    DWORD total,
    int cancellable,
    int fileCurrent,
    int fileTotal)
{
    IDirect3DDevice8* d = Gfx_Device();
    int vw = logical_width();
    int w = UI_IsWide() ? 570 : 510;
    int h = 184;
    int x = (vw - w) / 2;
    int y = 140;
    float pct = 0.0f;
    DWORD wholePct = 0;
    char pctText[24];
    char num[16];
    char fileText[40];

    fileText[0] = 0;

    if (fileTotal < 0 &&
        fileCurrent>0)
    {
        char fileNum[16];

        scpy(
            fileText,
            sizeof(fileText),
            "FOUND ");

        u32toa(
            (DWORD)fileCurrent,
            fileNum,
            sizeof(fileNum));

        scat(fileText, sizeof(fileText), fileNum);
        scat(fileText, sizeof(fileText), " FILES");
    }
    else if (fileTotal > 1 &&
        fileCurrent > 0)
    {
        char fileNum[16];

        scpy(
            fileText,
            sizeof(fileText),
            "FILE ");

        u32toa(
            (DWORD)fileCurrent,
            fileNum,
            sizeof(fileNum));

        scat(fileText, sizeof(fileText), fileNum);
        scat(fileText, sizeof(fileText), " OF ");

        u32toa(
            (DWORD)fileTotal,
            fileNum,
            sizeof(fileNum));

        scat(fileText, sizeof(fileText), fileNum);
    }

    if (total)
    {
        pct = (float)done / (float)total;
        wholePct = (done >= total) ? 100u : (done * 100u) / total;
    }

    if (pct < 0.0f)pct = 0.0f;
    if (pct > 1.0f)pct = 1.0f;

    UI_FillRect(
        0, 58, vw, 386,
        UI_ARGB(144, 0, 0, 0));

    iso_panel_begin(
        x, y, w, h, 1);

    Font_DrawTextIso(
        d,
        (float)(x + 34),
        (float)(y + 27),
        title,
        FONT_SIZE_MEDIUM,
        FONT_WHITE);

    if (name && name[0])
    {
        Font_DrawTextIso(
            d,
            (float)(x + 34),
            (float)(y + 57),
            fileTotal < 0 ? "SOURCE SCAN" : "CURRENT FILE",
            FONT_SIZE_SMALL,
            FONT_RGBA(160, 132, 188, 255));

        if (fileText[0])
        {
            int fileW =
                Font_MeasureText(
                    fileText,
                    FONT_SIZE_SMALL);

            Font_DrawTextIso(
                d,
                (float)(x + w - 34 - fileW),
                (float)(y + 57),
                fileText,
                FONT_SIZE_SMALL,
                FONT_RGBA(205, 178, 226, 255));
        }

        Font_DrawTextIsoClip(
            d,
            (float)(x + 34),
            (float)(y + 76),
            name,
            FONT_SIZE_SMALL,
            FONT_RGBA(235, 224, 245, 255),
            (float)(w - 78));
    }

    Iso_FillRect(
        (float)(x + 32),
        (float)(y + 104),
        (float)(w - 78),
        17.0f,
        UI_ARGB(110, 0, 0, 0),
        0);

    Iso_FillRect(
        (float)(x + 34),
        (float)(y + 106),
        (float)(w - 82),
        13.0f,
        UI_ARGB(235, 43, 27, 58),
        0);

    Iso_FillRect(
        (float)(x + 34),
        (float)(y + 106),
        (float)(w - 82) * pct,
        13.0f,
        UI_ARGB(236, 190, 74, 252),
        1);

    Iso_FillRect(
        (float)(x + 34),
        (float)(y + 106),
        (float)(w - 82) * pct,
        3.0f,
        UI_ARGB(255, 236, 157, 255),
        1);

    u32toa(
        wholePct,
        num,
        sizeof(num));

    scpy(
        pctText,
        sizeof(pctText),
        num);

    scat(
        pctText,
        sizeof(pctText),
        "%");

    Font_DrawTextIso(
        d,
        (float)(x + 34),
        (float)(y + 140),
        cancellable ?
        "B / BACK  CANCEL" :
        "DO NOT REMOVE DRIVE",
        FONT_SIZE_SMALL,
        FONT_RGBA(168, 154, 187, 255));

    Font_DrawTextIso(
        d,
        (float)(x + w - 78),
        (float)(y + 140),
        pctText,
        FONT_SIZE_SMALL,
        FONT_WHITE);

    iso_panel_end();
}


static void draw_footer(void)
{
    IDirect3DDevice8* d = Gfx_Device();
    int vw = logical_width();
    int msgW = UI_IsWide() ? 420 : 275;
    const char* help;

    if (s_mode == FM_DESTPICK)
        help = "A OPEN    B UP    WHITE PASTE    BACK CANCEL";
    else if (s_mode == FM_CONFIRM_DELETE)
        help = "A DELETE    B CANCEL";
    else if (s_mode == FM_CONFIRM_FORMAT)
        help = "A FORMAT    B CANCEL";
    else if (s_mode == FM_CONFIRM_EXIT)
        help = "";
    else if (s_mode == FM_FORMATTING)
        help = "FORMATTING USB - DO NOT REMOVE DRIVE";
    else if (s_mode == FM_COPY_CONFLICT)
        help = "A OVERWRITE    X SKIP    B CANCEL";
    else
        help = "A OPEN / LAUNCH    B UP    X ACTIONS    BACK EXIT";

    if (s_msg[0])
    {
        Font_DrawTextEllipsis(
            d,
            21, 455,
            s_msg,
            FONT_SIZE_SMALL,
            FONT_RGBA(0, 0, 0, 190),
            msgW);

        Font_DrawTextEllipsis(
            d,
            20, 454,
            s_msg,
            FONT_SIZE_SMALL,
            FONT_WHITE,
            msgW);
    }

    Font_DrawTextRight(
        d,
        vw - 20, 454,
        help,
        FONT_SIZE_SMALL,
        ui_text_dim());
}


void FileMan_Render(void)
{
    IDirect3DDevice8* d = Gfx_Device();

    draw_backdrop();

    if (TextViewer_IsActive())
    {
        TextViewer_Render();
        return;
    }

    if (HexViewer_IsActive())
    {
        HexViewer_Render();
        return;
    }

    if (ChecksumViewer_IsActive())
    {
        ChecksumViewer_Render();
        return;
    }

    draw_pane_flat(
        &s_pane[0],
        0,
        s_active == 0);

    draw_pane_flat(
        &s_pane[1],
        1,
        s_active == 1);

    draw_helper_panel();

    if (s_mode == FM_OPS)
        draw_ops_overlay();

    if (s_mode == FM_CONFIRM_DELETE)
    {
        char firstName[FM_NAME_MAX];
        char line1[FM_NAME_MAX + 32];
        char num[16];
        int count;

        count = delete_target_info(
            &s_pane[s_active],
            firstName,
            sizeof(firstName));

        if (count > 1)
        {
            scpy(
                line1,
                sizeof(line1),
                "Delete ");

            u32toa(
                (DWORD)count,
                num,
                sizeof(num));

            scat(
                line1,
                sizeof(line1),
                num);

            scat(
                line1,
                sizeof(line1),
                " selected items?");
        }
        else
        {
            scpy(
                line1,
                sizeof(line1),
                firstName[0] ?
                firstName :
                "Selected item");
        }

        draw_modal_box_buttons(
            count > 1 ?
            "DELETE SELECTED ITEMS?" :
            "DELETE ITEM?",
            line1,
            "This cannot be undone.",
            "A DELETE        B CANCEL");
    }

    if (s_mode == FM_CONFIRM_FORMAT)
    {
        draw_modal_box_buttons(
            "FORMAT USB DRIVE?",
            "All data on the USB FAT32 partition will be erased.",
            "This cannot be undone.",
            "A FORMAT        B CANCEL");
    }

    if (s_mode == FM_CONFIRM_EXIT)
    {
        if (s_usbReadyLast)
        {
            draw_modal_box_buttons(
                "EXIT USB2XB?",
                "Unplug USB storage before exiting.",
                0,
                "A EXIT        B CANCEL");
        }
        else
        {
            draw_modal_box_buttons(
                "EXIT USB2XB?",
                "No USB storage is mounted.",
                0,
                "A EXIT        B CANCEL");
        }
    }

    if (s_mode == FM_COPY_CONFLICT)
    {
        DWORD done = 0;
        DWORD total = 0;
        int fd = 0;
        int fs = 0;
        char name[DD_NAME_MAX];

        CopyJob_Progress(
            &fd, &fs,
            name, sizeof(name),
            &done, &total);

        draw_modal_box_buttons(
            "FILE ALREADY EXISTS",
            name[0] ? name : "Destination file",
            "Choose how to handle this file.",
            "A OVERWRITE    X SKIP    B CANCEL");
    }

    if (s_mode == FM_FORMATTING)
    {
        DWORD done = 0;
        DWORD total = 0;

        USB2XB_StorageFormatUsbProgress(
            &done, &total);

        draw_progress_box(
            "FORMATTING USB",
            "Do not remove the USB drive.",
            done, total, 0,
            0, 0);
    }

    if (s_mode == FM_COPYING)
    {
        DWORD done = 0;
        DWORD total = 0;
        int fd = 0;
        int fs = 0;
        int fileCurrent = 0;
        int fileTotal = 0;
        char name[DD_NAME_MAX];

        if (CopyJob_Preparing())
        {
            CopyJob_Progress(
                &fd, &fs,
                name, sizeof(name),
                &done, &total);

            draw_progress_box(
                s_launchStaging ?
                "PREPARING USB APP" :
                (s_copyMove ? "PREPARING MOVE" : "PREPARING COPY"),
                s_launchStaging ?
                "Building launch file list..." :
                "Building file list...",
                0, 0, 1,
                fs, -1);
        }
        else
        {
            CopyJob_Progress(
                &fd, &fs,
                name, sizeof(name),
                &done, &total);

            CopyJob_FileCounter(
                &fileCurrent,
                &fileTotal);

            draw_progress_box(
                s_launchStaging ?
                "LOADING USB APP" :
                (s_copyMove ? "MOVING DATA" : "COPYING DATA"),
                name,
                done, total, 1,
                fileCurrent, fileTotal);
        }
    }

    if (s_mode == FM_OSK_MKDIR ||
        s_mode == FM_OSK_RENAME ||
        s_mode == FM_OSK_FILTER)
    {
        Osk_Draw(d);
    }

    draw_footer();
}
