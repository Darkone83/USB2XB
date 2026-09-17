#include <xtl.h>
#include <stdlib.h>
#include "usb2xb_storage.h"
#include "usb2xb_usb.h"

/*===========================================================================
    FAT32 backend for USB2XB

    Read path:
      - lazy mount after raw USB reports READY + FAT32 detected
      - FAT32 BPB parsing and FAT-chain traversal
      - root/subdirectory enumeration
      - 8.3 names + VFAT long file names
      - sequential file reads and exists()

    Write/mutation path:
      - create/truncate ordinary files
      - VFAT/LFN create, rename, lookup, delete and mkdir
      - collision-safe hidden short aliases for LFN entries
      - FAT32 cluster allocation / chain linking
      - update every FAT copy
      - publish directory entries on close
      - overwrite safely switches to the new chain before freeing the old one
      - USB-created names use native FATX component rules so files round-trip
        cleanly between USB FAT32 and the Xbox HDD

    Existing FAT32 LFNs longer than FATX permits remain readable/browsable.
    New names are intentionally limited to FATX-compatible components.
===========================================================================*/

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME     0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

#define FAT_EOC_MIN         0x0FFFFFF8u
#define FAT_BAD_CLUSTER     0x0FFFFFF7u

typedef struct
{
    int mounted;

    DWORD volumeLba;
    DWORD deviceSectorSize;
    DWORD totalSectors;

    DWORD sectorsPerCluster;
    DWORD reservedSectors;
    DWORD fatCount;
    DWORD fatSize;
    DWORD fatStartLba;
    DWORD dataStartLba;
    DWORD rootCluster;
    DWORD clusterCount;
    DWORD clusterBytes;

    DWORD nextFreeHint;

    /* FAT32 FSInfo bookkeeping. */
    DWORD fsInfoSector;
    DWORD backupBootSector;
    DWORD freeClusterCount;
    int freeCountKnown;
    int fsInfoDirty;
} FatVolume;

typedef struct
{
    char name[DD_NAME_MAX];
    DWORD size;
    DWORD firstCluster;
    UCHAR attr;
} FatEntry;

typedef struct
{
    DWORD startCluster;
    DWORD curCluster;
    DWORD sectorInCluster;
    DWORD entryInSector;
    DWORD clusterHops;

    unsigned char* sector;
    int sectorLoaded;
    int ended;

    char lfn[DD_NAME_MAX];
    int lfnActive;
} FatDir;

#define FAT_LFN_MAX_ENTRIES 20

typedef struct
{
    DWORD lba;
    DWORD off;
} FatSlot;

typedef struct
{
    int found;
    FatSlot shortSlot;
    FatSlot lfnSlots[FAT_LFN_MAX_ENTRIES];
    int lfnCount;
    unsigned char shortName[11];
    DWORD firstCluster;
    DWORD size;
    UCHAR attr;
} FatLocatedEntry;

#define FAT_FILE_READ   1
#define FAT_FILE_WRITE  2

typedef struct
{
    int mode;

    DWORD startCluster;
    DWORD size;
    DWORD pos;

    DWORD curCluster;
    DWORD curClusterIndex;

    unsigned char* sector;
    DWORD loadedLba;
    int sectorLoaded;

    /* write-side metadata */
    DWORD dirEntryLba;
    DWORD dirEntryOff;
    DWORD oldStartCluster;
    DWORD lastCluster;
    unsigned char shortName[11];

    FatSlot lfnSlots[FAT_LFN_MAX_ENTRIES];
    int lfnCount;
    int publishLfn;
    char longName[DD_NAME_MAX];

    int writeFailed;
} FatFile;

static FatVolume g_fat;

/*
    UI-level unmount.  This intentionally does not disturb the proven USB/BOT
    transport; it invalidates the FAT32 mount and blocks lazy remount until the
    user requests Mount/Remount or physically reconnects the device.
*/
static int g_usbUserUnmounted = 0;


/*===========================================================================
    Tiny helpers
===========================================================================*/
static WORD fat_le16(const unsigned char* p)
{
    return (WORD)((WORD)p[0] | ((WORD)p[1] << 8));
}

static DWORD fat_le32(const unsigned char* p)
{
    return (DWORD)p[0] |
        ((DWORD)p[1] << 8) |
        ((DWORD)p[2] << 16) |
        ((DWORD)p[3] << 24);
}


static void fat_put_le16(unsigned char* p, WORD v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}


static void fat_put_le32(unsigned char* p, DWORD v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}


static void fat_now_date_time(WORD* outDate, WORD* outTime)
{
    SYSTEMTIME st;
    WORD year;
    WORD month;
    WORD day;
    WORD hour;
    WORD minute;
    WORD second;

    GetLocalTime(&st);

    year = st.wYear;
    month = st.wMonth;
    day = st.wDay;
    hour = st.wHour;
    minute = st.wMinute;
    second = st.wSecond;

    /* FAT dates can represent 1980 through 2107. */
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;

    if (month < 1 || month > 12) month = 1;
    if (day < 1 || day > 31) day = 1;
    if (hour > 23) hour = 0;
    if (minute > 59) minute = 0;
    if (second > 59) second = 0;

    if (outDate)
    {
        *outDate = (WORD)(
            ((year - 1980) << 9) |
            (month << 5) |
            day);
    }

    if (outTime)
    {
        *outTime = (WORD)(
            (hour << 11) |
            (minute << 5) |
            (second >> 1));
    }
}

static int fat_power2(DWORD v)
{
    return v && ((v & (v - 1)) == 0);
}

static void fat_zero(void* p, int n)
{
    int i;
    unsigned char* b = (unsigned char*)p;

    if (!p || n <= 0)
        return;

    for (i = 0; i < n; ++i)
        b[i] = 0;
}

static void fat_copy_bytes(void* dst, const void* src, DWORD bytes)
{
    DWORD i;
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* p = (const unsigned char*)src;

    if (!dst || !src)
        return;

    for (i = 0; i < bytes; ++i)
        d[i] = p[i];
}


static int fat_slen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void fat_scpy(char* d, int cap, const char* s)
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

static int fat_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    return c;
}

static int fat_nameeq(const char* a, const char* b)
{
    int i = 0;

    if (!a || !b)
        return 0;

    for (;;)
    {
        int ca = fat_tolower((unsigned char)a[i]);
        int cb = fat_tolower((unsigned char)b[i]);

        if (ca != cb)
            return 0;

        if (!ca)
            return 1;

        ++i;
    }
}

static int fat_is_sep(char c)
{
    return c == '/' || c == '\\';
}


/*===========================================================================
    Raw sector + metadata cache + volume mount
===========================================================================*/
#define FAT_META_CACHE_ENTRIES 32
#define FAT_META_READAHEAD_SECTORS 4
#define FAT_ZERO_BATCH_SECTORS 32

typedef struct
{
    DWORD lba;
    DWORD age;
    int valid;
    unsigned char* data;
} FatMetaCacheEntry;

static FatMetaCacheEntry g_metaCache[FAT_META_CACHE_ENTRIES];
static unsigned char* g_metaCacheStorage = 0;
static unsigned char* g_metaReadAheadStorage = 0;
static DWORD g_metaCacheSectorSize = 0;
static DWORD g_metaCacheClock = 1;


static void fat_cache_discard(void)
{
    int i;

    if (g_metaCacheStorage)
    {
        free(g_metaCacheStorage);
        g_metaCacheStorage = 0;
    }

    if (g_metaReadAheadStorage)
    {
        free(g_metaReadAheadStorage);
        g_metaReadAheadStorage = 0;
    }

    g_metaCacheSectorSize = 0;
    g_metaCacheClock = 1;

    for (i = 0; i < FAT_META_CACHE_ENTRIES; ++i)
    {
        g_metaCache[i].lba = 0;
        g_metaCache[i].age = 0;
        g_metaCache[i].valid = 0;
        g_metaCache[i].data = 0;
    }
}


static int fat_cache_prepare(void)
{
    DWORD bytes;
    int i;

    if (!g_fat.deviceSectorSize)
        return 0;

    if (g_metaCacheStorage &&
        g_metaCacheSectorSize == g_fat.deviceSectorSize)
    {
        return 1;
    }

    fat_cache_discard();

    bytes = g_fat.deviceSectorSize * FAT_META_CACHE_ENTRIES;
    g_metaCacheStorage = (unsigned char*)malloc(bytes);

    if (!g_metaCacheStorage)
        return 0;

    /*
        Read-ahead is optional.  If this small helper allocation fails, keep
        the normal metadata cache active and fall back to one-sector fills.
    */
    g_metaReadAheadStorage = (unsigned char*)malloc(
        g_fat.deviceSectorSize * FAT_META_READAHEAD_SECTORS);

    g_metaCacheSectorSize = g_fat.deviceSectorSize;

    for (i = 0; i < FAT_META_CACHE_ENTRIES; ++i)
    {
        g_metaCache[i].data =
            g_metaCacheStorage +
            i * g_metaCacheSectorSize;
    }

    return 1;
}


static void fat_cache_touch(FatMetaCacheEntry* e)
{
    int i;

    if (!e)
        return;

    ++g_metaCacheClock;

    if (g_metaCacheClock == 0)
    {
        g_metaCacheClock = 1;

        for (i = 0; i < FAT_META_CACHE_ENTRIES; ++i)
            g_metaCache[i].age = g_metaCache[i].valid ? 1 : 0;
    }

    e->age = g_metaCacheClock;
}


static FatMetaCacheEntry* fat_cache_find(DWORD lba)
{
    int i;

    for (i = 0; i < FAT_META_CACHE_ENTRIES; ++i)
    {
        if (g_metaCache[i].valid &&
            g_metaCache[i].lba == lba)
        {
            return &g_metaCache[i];
        }
    }

    return 0;
}


static FatMetaCacheEntry* fat_cache_victim(void)
{
    FatMetaCacheEntry* oldest = 0;
    int i;

    for (i = 0; i < FAT_META_CACHE_ENTRIES; ++i)
    {
        if (!g_metaCache[i].valid)
            return &g_metaCache[i];

        if (!oldest ||
            g_metaCache[i].age < oldest->age)
        {
            oldest = &g_metaCache[i];
        }
    }

    return oldest;
}


static void fat_cache_invalidate_range(
    DWORD lba,
    DWORD count)
{
    int i;

    if (!count)
        return;

    for (i = 0; i < FAT_META_CACHE_ENTRIES; ++i)
    {
        if (g_metaCache[i].valid &&
            g_metaCache[i].lba >= lba &&
            g_metaCache[i].lba - lba < count)
        {
            g_metaCache[i].valid = 0;
            g_metaCache[i].age = 0;
        }
    }
}


static int fat_raw_read_sector(DWORD lba, void* dst)
{
    if (!dst || !g_fat.deviceSectorSize)
        return 0;

    return USB2XB_USB_ReadSectors(lba, 1, dst);
}


static int fat_raw_write_sector(DWORD lba, const void* src)
{
    int ok;

    if (!src || !g_fat.deviceSectorSize)
        return 0;

    ok = USB2XB_USB_WriteSectors(lba, 1, src);

    if (ok)
        fat_cache_invalidate_range(lba, 1);

    return ok;
}


/*
    Metadata cache: 32 sectors, LRU replacement, write-through, 4-sector read-ahead.

    Only FAT/directory/FSInfo traffic uses these wrappers. File payload data
    keeps using the direct/batched path so large copies cannot evict useful
    metadata and hotplug never has dirty metadata stranded in RAM.
*/
static int fat_read_sector(DWORD lba, void* dst)
{
    FatMetaCacheEntry* e;

    if (!dst || !g_fat.deviceSectorSize)
        return 0;

    if (!fat_cache_prepare())
        return fat_raw_read_sector(lba, dst);

    e = fat_cache_find(lba);

    if (e)
    {
        fat_cache_touch(e);
        fat_copy_bytes(dst, e->data, g_fat.deviceSectorSize);
        return 1;
    }

    /*
        Metadata miss read-ahead.  Directory walks and FAT scans are strongly
        sequential, so fetch a few adjacent sectors under one READ(10).  The
        USB backend still emits the same proven 512-byte physical data URBs.

        Keep the window deliberately small: it saves CBW/CSW round trips
        without turning a random metadata lookup into a large speculative read.
    */
    if (g_metaReadAheadStorage &&
        g_fat.mounted &&
        lba >= g_fat.volumeLba &&
        lba < g_fat.volumeLba + g_fat.totalSectors)
    {
        DWORD count = FAT_META_READAHEAD_SECTORS;
        DWORD remain =
            (g_fat.volumeLba + g_fat.totalSectors) - lba;
        DWORD i;

        if (count > remain)
            count = remain;

        if (count > 1 &&
            USB2XB_USB_ReadSectors(
                lba,
                count,
                g_metaReadAheadStorage))
        {
            for (i = 0; i < count; ++i)
            {
                FatMetaCacheEntry* fill =
                    fat_cache_find(lba + i);

                if (!fill)
                    fill = fat_cache_victim();

                if (!fill)
                    break;

                fat_copy_bytes(
                    fill->data,
                    g_metaReadAheadStorage +
                    i * g_fat.deviceSectorSize,
                    g_fat.deviceSectorSize);

                fill->lba = lba + i;
                fill->valid = 1;
                fat_cache_touch(fill);
            }

            e = fat_cache_find(lba);

            if (e)
            {
                fat_cache_touch(e);
                fat_copy_bytes(
                    dst,
                    e->data,
                    g_fat.deviceSectorSize);
                return 1;
            }
        }
    }

    e = fat_cache_victim();

    if (!e ||
        !fat_raw_read_sector(lba, e->data))
    {
        return 0;
    }

    e->lba = lba;
    e->valid = 1;
    fat_cache_touch(e);

    fat_copy_bytes(dst, e->data, g_fat.deviceSectorSize);
    return 1;
}

static int fat_write_sector(DWORD lba, const void* src)
{
    FatMetaCacheEntry* e;

    if (!src || !g_fat.deviceSectorSize)
        return 0;

    /* Write-through keeps existing FAT update ordering and hotplug safety. */
    if (!fat_raw_write_sector(lba, src))
        return 0;

    if (!fat_cache_prepare())
        return 1;

    e = fat_cache_find(lba);

    if (!e)
        e = fat_cache_victim();

    if (e)
    {
        fat_copy_bytes(e->data, src, g_fat.deviceSectorSize);
        e->lba = lba;
        e->valid = 1;
        fat_cache_touch(e);
    }

    return 1;
}


/*
    Contiguous data-path helpers. Metadata uses the cache-backed single-sector
    wrappers above, while file I/O can hand the USB layer an aligned run.
    The transport itself keeps each Xbox OHCI data URB at 512 bytes.
*/
static int fat_read_sectors(DWORD lba, DWORD count, void* dst)
{
    if (!dst || !count || !g_fat.deviceSectorSize)
        return 0;

    return USB2XB_USB_ReadSectors(lba, count, dst);
}


static int fat_write_sectors(DWORD lba, DWORD count, const void* src)
{
    int ok;

    if (!src || !count || !g_fat.deviceSectorSize)
        return 0;

    ok = USB2XB_USB_WriteSectors(lba, count, src);

    if (ok)
        fat_cache_invalidate_range(lba, count);

    return ok;
}


static int fat_fsinfo_valid(const unsigned char* sec)
{
    if (!sec)
        return 0;

    return
        fat_le32(sec + 0) == 0x41615252u &&
        fat_le32(sec + 484) == 0x61417272u &&
        fat_le32(sec + 508) == 0xAA550000u;
}


static void fat_load_fsinfo(void)
{
    unsigned char* sec;
    DWORD freeCount;
    DWORD nextFree;
    DWORD maxCluster;

    g_fat.freeCountKnown = 0;
    g_fat.freeClusterCount = 0;
    g_fat.fsInfoDirty = 0;

    if (!g_fat.mounted ||
        g_fat.fsInfoSector == 0xFFFFFFFFu)
    {
        return;
    }

    sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

    if (!sec)
        return;

    if (!fat_read_sector(
        g_fat.volumeLba + g_fat.fsInfoSector,
        sec) ||
        !fat_fsinfo_valid(sec))
    {
        free(sec);
        return;
    }

    freeCount = fat_le32(sec + 488);
    nextFree = fat_le32(sec + 492);
    maxCluster = g_fat.clusterCount + 1;

    if (freeCount <= g_fat.clusterCount)
    {
        g_fat.freeClusterCount = freeCount;
        g_fat.freeCountKnown = 1;
    }

    if (nextFree >= 2 && nextFree <= maxCluster)
        g_fat.nextFreeHint = nextFree;

    free(sec);
}


static int fat_flush_fsinfo(void)
{
    unsigned char* sec;
    DWORD primaryLba;
    DWORD nextFree;
    int primaryWritten = 0;

    if (!g_fat.mounted || !g_fat.fsInfoDirty)
        return 1;

    if (g_fat.fsInfoSector == 0xFFFFFFFFu)
    {
        g_fat.fsInfoDirty = 0;
        return 1;
    }

    /* Never turn a physical disconnect into recovery traffic. */
    if (USB2XB_USB_State() != U2X_USB_READY)
        return 0;

    sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    primaryLba = g_fat.volumeLba + g_fat.fsInfoSector;

    if (!fat_read_sector(primaryLba, sec) ||
        !fat_fsinfo_valid(sec))
    {
        free(sec);
        return 0;
    }

    fat_put_le32(
        sec + 488,
        g_fat.freeCountKnown ?
        g_fat.freeClusterCount :
        0xFFFFFFFFu);

    nextFree = g_fat.nextFreeHint;
    if (nextFree < 2 || nextFree > g_fat.clusterCount + 1)
        nextFree = 0xFFFFFFFFu;

    fat_put_le32(sec + 492, nextFree);

    if (!fat_write_sector(primaryLba, sec))
    {
        free(sec);
        return 0;
    }

    primaryWritten = 1;

    /*
        FAT32 normally mirrors FSInfo next to the backup boot sector.
        Update it when that copy exists and has valid FSInfo signatures.
        A bad/missing backup copy is non-fatal because FSInfo is advisory.
    */
    if (g_fat.backupBootSector != 0xFFFFFFFFu &&
        g_fat.backupBootSector + g_fat.fsInfoSector <
        g_fat.reservedSectors)
    {
        DWORD backupLba =
            g_fat.volumeLba +
            g_fat.backupBootSector +
            g_fat.fsInfoSector;

        if (fat_read_sector(backupLba, sec) &&
            fat_fsinfo_valid(sec))
        {
            fat_put_le32(
                sec + 488,
                g_fat.freeCountKnown ?
                g_fat.freeClusterCount :
                0xFFFFFFFFu);
            fat_put_le32(sec + 492, nextFree);
            fat_write_sector(backupLba, sec);
        }
    }

    free(sec);

    if (primaryWritten)
        g_fat.fsInfoDirty = 0;

    return primaryWritten;
}


static void fat_unmount(void)
{
    /* Cache is write-through, so unmount only has to discard stale reads. */
    fat_cache_discard();
    fat_zero(&g_fat, sizeof(g_fat));
}


static int fat_mount(void)
{
    unsigned char* boot;
    DWORD bytesPerSector;
    DWORD spc;
    DWORD reserved;
    DWORD fats;
    DWORD fatSize;
    DWORD rootCluster;
    DWORD total16;
    DWORD total32;
    DWORD total;
    DWORD nonData;
    DWORD dataSectors;
    DWORD fsInfoSector;
    DWORD backupBootSector;

    if (g_usbUserUnmounted)
    {
        fat_unmount();
        return 0;
    }

    if (USB2XB_USB_State() != U2X_USB_READY ||
        !USB2XB_USB_Fat32Detected())
    {
        fat_unmount();
        return 0;
    }

    if (g_fat.mounted &&
        g_fat.volumeLba == USB2XB_USB_VolumeLba() &&
        g_fat.deviceSectorSize == USB2XB_USB_SectorSize())
    {
        return 1;
    }

    fat_unmount();

    g_fat.volumeLba = USB2XB_USB_VolumeLba();
    g_fat.deviceSectorSize = USB2XB_USB_SectorSize();

    if (g_fat.deviceSectorSize < 512 ||
        g_fat.deviceSectorSize>65536 ||
        !fat_power2(g_fat.deviceSectorSize))
    {
        fat_unmount();
        return 0;
    }

    boot = (unsigned char*)malloc(g_fat.deviceSectorSize);

    if (!boot)
    {
        fat_unmount();
        return 0;
    }

    fat_zero(boot, (int)g_fat.deviceSectorSize);

    if (!USB2XB_USB_ReadSectors(g_fat.volumeLba, 1, boot))
    {
        free(boot);
        fat_unmount();
        return 0;
    }

    if (boot[510] != 0x55 || boot[511] != 0xAA)
    {
        free(boot);
        fat_unmount();
        return 0;
    }

    bytesPerSector = fat_le16(boot + 11);
    spc = boot[13];
    reserved = fat_le16(boot + 14);
    fats = boot[16];
    total16 = fat_le16(boot + 19);
    total32 = fat_le32(boot + 32);
    fatSize = fat_le32(boot + 36);
    rootCluster = fat_le32(boot + 44);
    fsInfoSector = fat_le16(boot + 48);
    backupBootSector = fat_le16(boot + 50);

    free(boot);

    total = total16 ? total16 : total32;

    if (bytesPerSector != g_fat.deviceSectorSize ||
        !fat_power2(spc) ||
        !reserved ||
        !fats ||
        !fatSize ||
        rootCluster < 2 ||
        !total)
    {
        fat_unmount();
        return 0;
    }

    nonData = reserved + fats * fatSize;

    if (total <= nonData)
    {
        fat_unmount();
        return 0;
    }

    dataSectors = total - nonData;

    g_fat.sectorsPerCluster = spc;
    g_fat.reservedSectors = reserved;
    g_fat.fatCount = fats;
    g_fat.fatSize = fatSize;
    g_fat.totalSectors = total;
    g_fat.rootCluster = rootCluster;

    g_fat.fatStartLba =
        g_fat.volumeLba + reserved;

    g_fat.dataStartLba =
        g_fat.volumeLba + nonData;

    g_fat.clusterCount =
        dataSectors / spc;

    g_fat.clusterBytes =
        g_fat.deviceSectorSize * spc;

    g_fat.nextFreeHint = 2;

    g_fat.fsInfoSector =
        (fsInfoSector > 0 && fsInfoSector < reserved) ?
        fsInfoSector : 0xFFFFFFFFu;

    g_fat.backupBootSector =
        (backupBootSector > 0 && backupBootSector < reserved) ?
        backupBootSector : 0xFFFFFFFFu;

    if (g_fat.clusterCount < 1 ||
        g_fat.clusterBytes < g_fat.deviceSectorSize)
    {
        fat_unmount();
        return 0;
    }

    g_fat.mounted = 1;
    fat_load_fsinfo();
    return 1;
}


static DWORD fat_cluster_lba(DWORD cluster)
{
    if (cluster < 2)
        return 0;

    return g_fat.dataStartLba +
        (cluster - 2) * g_fat.sectorsPerCluster;
}


static DWORD fat_next_cluster(DWORD cluster)
{
    unsigned char* sec;
    DWORD fatOffset;
    DWORD lba;
    DWORD off;
    DWORD value;

    if (!g_fat.mounted || cluster < 2)
        return 0;

    fatOffset = cluster * 4;
    lba = g_fat.fatStartLba +
        fatOffset / g_fat.deviceSectorSize;

    off = fatOffset % g_fat.deviceSectorSize;

    sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    if (!fat_read_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    if (off + 4 > g_fat.deviceSectorSize)
    {
        free(sec);
        return 0;
    }

    value = fat_le32(sec + off) & 0x0FFFFFFFu;
    free(sec);

    if (value >= FAT_EOC_MIN ||
        value == FAT_BAD_CLUSTER ||
        value < 2)
    {
        return 0;
    }

    return value;
}




static int fat_get_fat_value(
    DWORD cluster,
    DWORD* outValue)
{
    unsigned char* sec;
    DWORD fatOffset;
    DWORD lba;
    DWORD off;
    DWORD value;

    if (outValue)
        *outValue = 0;

    if (!g_fat.mounted ||
        !outValue ||
        cluster<2 ||
        cluster>g_fat.clusterCount + 1)
    {
        return 0;
    }

    fatOffset = cluster * 4;
    lba = g_fat.fatStartLba +
        fatOffset / g_fat.deviceSectorSize;
    off = fatOffset % g_fat.deviceSectorSize;

    if (off + 4 > g_fat.deviceSectorSize)
        return 0;

    sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    if (!fat_read_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    value = fat_le32(sec + off) & 0x0FFFFFFFu;
    free(sec);

    *outValue = value;
    return 1;
}


static int fat_set_fat_value(
    DWORD cluster,
    DWORD value)
{
    unsigned char* sec;
    DWORD fatOffset;
    DWORD sectorIndex;
    DWORD off;
    DWORD fat;
    DWORD oldRaw;
    DWORD newRaw;
    DWORD firstOldValue = 0;
    DWORD newValue;
    int haveFirstOldValue = 0;

    if (!g_fat.mounted ||
        cluster<2 ||
        cluster>g_fat.clusterCount + 1)
    {
        return 0;
    }

    fatOffset = cluster * 4;
    sectorIndex = fatOffset / g_fat.deviceSectorSize;
    off = fatOffset % g_fat.deviceSectorSize;

    if (off + 4 > g_fat.deviceSectorSize)
        return 0;

    sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    for (fat = 0; fat < g_fat.fatCount; ++fat)
    {
        DWORD lba =
            g_fat.fatStartLba +
            fat * g_fat.fatSize +
            sectorIndex;

        if (!fat_read_sector(lba, sec))
        {
            free(sec);
            return 0;
        }

        oldRaw = fat_le32(sec + off);

        if (!haveFirstOldValue)
        {
            firstOldValue = oldRaw & 0x0FFFFFFFu;
            haveFirstOldValue = 1;
        }

        newRaw = (oldRaw & 0xF0000000u) |
            (value & 0x0FFFFFFFu);

        fat_put_le32(sec + off, newRaw);

        if (!fat_write_sector(lba, sec))
        {
            free(sec);
            return 0;
        }
    }

    free(sec);

    newValue = value & 0x0FFFFFFFu;

    if (haveFirstOldValue &&
        ((firstOldValue == 0) != (newValue == 0)))
    {
        if (g_fat.freeCountKnown)
        {
            if (firstOldValue == 0 && newValue != 0)
            {
                if (g_fat.freeClusterCount > 0)
                    --g_fat.freeClusterCount;
                else
                    g_fat.freeCountKnown = 0;
            }
            else if (firstOldValue != 0 && newValue == 0)
            {
                if (g_fat.freeClusterCount < g_fat.clusterCount)
                    ++g_fat.freeClusterCount;
                else
                    g_fat.freeCountKnown = 0;
            }
        }

        g_fat.fsInfoDirty = 1;
    }

    return 1;
}


static int fat_zero_cluster(DWORD cluster)
{
    unsigned char* zero;
    DWORD lba;
    DWORD batchSectors;
    DWORD batchBytes;
    DWORD done;

    if (cluster < 2)
        return 0;

    lba = fat_cluster_lba(cluster);

    if (!lba)
        return 0;

    batchSectors = g_fat.sectorsPerCluster;

    if (batchSectors > FAT_ZERO_BATCH_SECTORS)
        batchSectors = FAT_ZERO_BATCH_SECTORS;

    if (!batchSectors)
        return 0;

    batchBytes = batchSectors * g_fat.deviceSectorSize;
    zero = (unsigned char*)malloc(batchBytes);

    if (!zero)
        return 0;

    fat_zero(zero, (int)batchBytes);

    done = 0;

    while (done < g_fat.sectorsPerCluster)
    {
        DWORD count = g_fat.sectorsPerCluster - done;

        if (count > batchSectors)
            count = batchSectors;

        if (!fat_write_sectors(
            lba + done,
            count,
            zero))
        {
            free(zero);
            return 0;
        }

        done += count;
    }

    free(zero);
    return 1;
}

static int fat_alloc_cluster(
    int zeroCluster,
    DWORD* outCluster)
{
    DWORD first;
    DWORD maxCluster;
    DWORD c;
    DWORD checked;

    if (outCluster)
        *outCluster = 0;

    if (!outCluster || !g_fat.mounted)
        return 0;

    maxCluster = g_fat.clusterCount + 1;

    first = g_fat.nextFreeHint;

    if (first<2 || first>maxCluster)
        first = 2;

    c = first;
    checked = 0;

    while (checked < g_fat.clusterCount)
    {
        DWORD v = 0;

        if (!fat_get_fat_value(c, &v))
            return 0;

        if (v == 0)
        {
            if (!fat_set_fat_value(c, 0x0FFFFFFFu))
                return 0;

            if (zeroCluster &&
                !fat_zero_cluster(c))
            {
                fat_set_fat_value(c, 0);
                return 0;
            }

            g_fat.nextFreeHint = c + 1;

            if (g_fat.nextFreeHint > maxCluster)
                g_fat.nextFreeHint = 2;

            *outCluster = c;
            return 1;
        }

        ++c;

        if (c > maxCluster)
            c = 2;

        ++checked;
    }

    return 0;
}


static void fat_free_chain(DWORD startCluster)
{
    DWORD c = startCluster;
    DWORD hops = 0;

    while (c >= 2 &&
        c <= g_fat.clusterCount + 1 &&
        hops <= g_fat.clusterCount)
    {
        DWORD next = 0;

        if (!fat_get_fat_value(c, &next))
            return;

        if (!fat_set_fat_value(c, 0))
            return;

        if (g_fat.nextFreeHint < 2 ||
            c < g_fat.nextFreeHint)
        {
            g_fat.nextFreeHint = c;
        }

        if (next >= FAT_EOC_MIN ||
            next == FAT_BAD_CLUSTER ||
            next < 2)
        {
            return;
        }

        c = next;
        ++hops;
    }
}


/*===========================================================================
    FAT directory parsing
===========================================================================*/
static void fat_lfn_reset(FatDir* d)
{
    if (!d)
        return;

    d->lfn[0] = 0;
    d->lfnActive = 0;
}


static void fat_lfn_put(FatDir* d, const unsigned char* e)
{
    static const unsigned char pos[13] =
    {
        1,3,5,7,9,
        14,16,18,20,22,24,
        28,30
    };

    int ord;
    int seq;
    int base;
    int i;

    if (!d || !e)
        return;

    ord = e[0];
    seq = ord & 0x1F;

    if (seq < 1 || seq>20)
    {
        fat_lfn_reset(d);
        return;
    }

    if (ord & 0x40)
    {
        fat_zero(d->lfn, sizeof(d->lfn));
        d->lfnActive = 1;
    }

    if (!d->lfnActive)
        return;

    base = (seq - 1) * 13;

    for (i = 0; i < 13; ++i)
    {
        WORD wc = fat_le16(e + pos[i]);
        int at = base + i;

        if (at >= DD_NAME_MAX - 1)
            continue;

        if (wc == 0x0000)
        {
            d->lfn[at] = 0;
            continue;
        }

        if (wc == 0xFFFF)
            continue;

        d->lfn[at] = (wc <= 255 && wc >= 32) ?
            (char)wc:'?';
    }

    d->lfn[DD_NAME_MAX - 1] = 0;
}


static void fat_short_name(const unsigned char* e, char* out, int cap)
{
    int n = 0;
    int i;
    int endBase = 8;
    int endExt = 3;
    unsigned char c;

    if (!e || !out || cap <= 0)
        return;

    while (endBase > 0 && e[endBase - 1] == ' ')
        --endBase;

    while (endExt > 0 && e[8 + endExt - 1] == ' ')
        --endExt;

    for (i = 0; i < endBase && n < cap - 1; ++i)
    {
        c = e[i];

        if (i == 0 && c == 0x05)
            c = 0xE5;

        out[n++] = (char)c;
    }

    if (endExt > 0 && n < cap - 1)
        out[n++] = '.';

    for (i = 0; i < endExt && n < cap - 1; ++i)
        out[n++] = (char)e[8 + i];

    out[n] = 0;
}


static int fat_dir_read_sector(FatDir* d)
{
    DWORD lba;

    if (!d || !d->sector || d->curCluster < 2)
        return 0;

    lba = fat_cluster_lba(d->curCluster);

    if (!lba)
        return 0;

    lba += d->sectorInCluster;

    if (!fat_read_sector(lba, d->sector))
        return 0;

    d->sectorLoaded = 1;
    return 1;
}


static int fat_dir_advance_cluster(FatDir* d)
{
    DWORD next;

    if (!d)
        return 0;

    next = fat_next_cluster(d->curCluster);

    if (!next)
    {
        d->ended = 1;
        return 0;
    }

    ++d->clusterHops;

    if (d->clusterHops > g_fat.clusterCount + 1)
    {
        d->ended = 1;
        return 0;
    }

    d->curCluster = next;
    d->sectorInCluster = 0;
    d->entryInSector = 0;
    d->sectorLoaded = 0;
    return 1;
}


static int fat_dir_next_raw(FatDir* d, FatEntry* out)
{
    DWORD entriesPerSector;

    if (!d || !out || d->ended)
        return 0;

    entriesPerSector =
        g_fat.deviceSectorSize / 32;

    if (!entriesPerSector)
        return 0;

    for (;;)
    {
        const unsigned char* e;
        UCHAR first;
        UCHAR attr;
        DWORD hi;
        DWORD lo;

        if (!d->sectorLoaded)
        {
            if (!fat_dir_read_sector(d))
            {
                d->ended = 1;
                return 0;
            }
        }

        if (d->entryInSector >= entriesPerSector)
        {
            d->entryInSector = 0;
            ++d->sectorInCluster;
            d->sectorLoaded = 0;

            if (d->sectorInCluster >= g_fat.sectorsPerCluster)
            {
                if (!fat_dir_advance_cluster(d))
                    return 0;
            }

            continue;
        }

        e = d->sector + d->entryInSector * 32;
        ++d->entryInSector;

        first = e[0];

        if (first == 0x00)
        {
            d->ended = 1;
            return 0;
        }

        if (first == 0xE5)
        {
            fat_lfn_reset(d);
            continue;
        }

        attr = e[11];

        if (attr == FAT_ATTR_LFN)
        {
            fat_lfn_put(d, e);
            continue;
        }

        if (attr & FAT_ATTR_VOLUME)
        {
            fat_lfn_reset(d);
            continue;
        }

        fat_zero(out, sizeof(*out));

        if (d->lfnActive && d->lfn[0])
            fat_scpy(out->name, sizeof(out->name), d->lfn);
        else
            fat_short_name(e, out->name, sizeof(out->name));

        fat_lfn_reset(d);

        if (!out->name[0])
            continue;

        hi = fat_le16(e + 20);
        lo = fat_le16(e + 26);

        out->firstCluster =
            (hi << 16) | lo;

        out->size = fat_le32(e + 28);
        out->attr = attr;

        return 1;
    }
}


static FatDir* fat_dir_open_cluster(DWORD cluster)
{
    FatDir* d;

    if (cluster < 2)
        return 0;

    d = (FatDir*)malloc(sizeof(FatDir));

    if (!d)
        return 0;

    fat_zero(d, sizeof(*d));

    d->sector = (unsigned char*)
        malloc(g_fat.deviceSectorSize);

    if (!d->sector)
    {
        free(d);
        return 0;
    }

    d->startCluster = cluster;
    d->curCluster = cluster;
    d->sectorInCluster = 0;
    d->entryInSector = 0;
    d->clusterHops = 0;
    d->sectorLoaded = 0;
    d->ended = 0;
    fat_lfn_reset(d);

    return d;
}


static void fat_dir_close(FatDir* d)
{
    if (!d)
        return;

    if (d->sector)
        free(d->sector);

    free(d);
}


/*===========================================================================
    Path resolution
===========================================================================*/
static int fat_next_component(
    const char** pp,
    char* out,
    int cap)
{
    const char* p;
    int n = 0;

    if (!pp || !out || cap <= 0)
        return 0;

    p = *pp;

    while (*p && fat_is_sep(*p))
        ++p;

    if (!*p)
    {
        *pp = p;
        out[0] = 0;
        return 0;
    }

    while (*p && !fat_is_sep(*p))
    {
        if (n < cap - 1)
            out[n++] = *p;
        ++p;
    }

    out[n] = 0;
    *pp = p;
    return n > 0;
}


static int fat_find_in_dir(
    DWORD dirCluster,
    const char* name,
    FatEntry* out)
{
    FatDir* d;
    FatEntry e;

    if (!name || !name[0])
        return 0;

    d = fat_dir_open_cluster(dirCluster);

    if (!d)
        return 0;

    while (fat_dir_next_raw(d, &e))
    {
        if (fat_nameeq(e.name, name))
        {
            if (out)
                *out = e;

            fat_dir_close(d);
            return 1;
        }
    }

    fat_dir_close(d);
    return 0;
}


static int fat_resolve_dir(
    const char* path,
    DWORD* outCluster)
{
    const char* p;
    char part[DD_NAME_MAX];
    DWORD cluster;

    if (!fat_mount() || !outCluster)
        return 0;

    cluster = g_fat.rootCluster;
    p = path ? path : "/";

    while (fat_next_component(&p, part, sizeof(part)))
    {
        FatEntry e;

        if (!fat_find_in_dir(cluster, part, &e))
            return 0;

        if (!(e.attr & FAT_ATTR_DIRECTORY) ||
            e.firstCluster < 2)
            return 0;

        cluster = e.firstCluster;
    }

    *outCluster = cluster;
    return 1;
}


static int fat_resolve_entry(
    const char* path,
    FatEntry* out)
{
    const char* p;
    const char* next;
    char part[DD_NAME_MAX];
    DWORD cluster;

    if (!fat_mount() || !path || !out)
        return 0;

    p = path;
    cluster = g_fat.rootCluster;

    while (fat_next_component(&p, part, sizeof(part)))
    {
        FatEntry e;

        next = p;

        while (*next && fat_is_sep(*next))
            ++next;

        if (!fat_find_in_dir(cluster, part, &e))
            return 0;

        if (!*next)
        {
            *out = e;
            return 1;
        }

        if (!(e.attr & FAT_ATTR_DIRECTORY) ||
            e.firstCluster < 2)
            return 0;

        cluster = e.firstCluster;
    }

    return 0;
}




/*===========================================================================
    FAT32 write helpers
===========================================================================*/
static int fat_short_char(int c)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;

    if ((c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'))
    {
        return c;
    }

    switch (c)
    {
    case '$':
    case '%':
    case '\'':
    case '-':
    case '_':
    case '@':
    case '~':
    case '`':
    case '!':
    case '(':
    case ')':
    case '{':
    case '}':
    case '^':
    case '#':
    case '&':
        return c;
    }

    return 0;
}


static int fat_make_short_name(
    const char* leaf,
    unsigned char out11[11])
{
    int len;
    int dot = -1;
    int i;
    int baseLen;
    int extLen;

    if (!leaf || !leaf[0] || !out11)
        return 0;

    len = fat_slen(leaf);

    if (len < 1)
        return 0;

    for (i = 0; i < 11; ++i)
        out11[i] = ' ';

    for (i = 0; i < len; ++i)
    {
        if (leaf[i] == '.')
            dot = i;
    }

    if (dot == 0)
        return 0;

    if (dot < 0)
    {
        baseLen = len;
        extLen = 0;
    }
    else
    {
        baseLen = dot;
        extLen = len - dot - 1;

        for (i = 0; i < dot; ++i)
            if (leaf[i] == '.')
                return 0;
    }

    if (baseLen < 1 || baseLen>8 ||
        extLen > 3)
    {
        return 0;
    }

    for (i = 0; i < baseLen; ++i)
    {
        int c = fat_short_char((unsigned char)leaf[i]);

        if (!c)
            return 0;

        out11[i] = (unsigned char)c;
    }

    for (i = 0; i < extLen; ++i)
    {
        int c = fat_short_char(
            (unsigned char)leaf[dot + 1 + i]);

        if (!c)
            return 0;

        out11[8 + i] = (unsigned char)c;
    }

    return 1;
}


static int fat_split_parent(
    const char* path,
    DWORD* outParentCluster,
    char* leaf,
    int leafCap)
{
    int len;
    int last = -1;
    int i;
    char parent[DD_PATH_MAX];

    if (!path ||
        !outParentCluster ||
        !leaf ||
        leafCap <= 0)
    {
        return 0;
    }

    len = fat_slen(path);

    while (len > 0 && fat_is_sep(path[len - 1]))
        --len;

    if (len <= 0)
        return 0;

    for (i = 0; i < len; ++i)
        if (fat_is_sep(path[i]))
            last = i;

    if (last < 0)
    {
        fat_scpy(leaf, leafCap, path);
        return fat_resolve_dir("/", outParentCluster);
    }

    {
        int n = 0;

        for (i = last + 1; i < len && n < leafCap - 1; ++i)
            leaf[n++] = path[i];

        leaf[n] = 0;
    }

    if (!leaf[0])
        return 0;

    if (last == 0)
    {
        parent[0] = '/';
        parent[1] = 0;
    }
    else
    {
        int n = 0;

        for (i = 0; i < last && n < DD_PATH_MAX - 1; ++i)
            parent[n++] = path[i];

        parent[n] = 0;
    }

    return fat_resolve_dir(
        parent,
        outParentCluster);
}


static int fat_short_equal(
    const unsigned char* entry,
    const unsigned char name11[11])
{
    int i;

    for (i = 0; i < 11; ++i)
        if (entry[i] != name11[i])
            return 0;

    return 1;
}


/*
    Locate an existing short entry or reserve a free 32-byte slot.
    If the directory chain is full, extend it by one zeroed cluster.
*/
static int fat_find_write_slot(
    DWORD dirCluster,
    const unsigned char name11[11],
    DWORD* outLba,
    DWORD* outOff,
    int* outExists,
    DWORD* outOldCluster,
    UCHAR* outOldAttr)
{
    DWORD cluster = dirCluster;
    DWORD hops = 0;
    DWORD freeLba = 0;
    DWORD freeOff = 0;
    int haveFree = 0;

    if (outLba)*outLba = 0;
    if (outOff)*outOff = 0;
    if (outExists)*outExists = 0;
    if (outOldCluster)*outOldCluster = 0;
    if (outOldAttr)*outOldAttr = 0;

    if (dirCluster < 2 ||
        !name11 ||
        !outLba ||
        !outOff ||
        !outExists)
    {
        return 0;
    }

    for (;;)
    {
        DWORD base = fat_cluster_lba(cluster);
        DWORD s;

        if (!base)
            return 0;

        for (s = 0; s < g_fat.sectorsPerCluster; ++s)
        {
            unsigned char* sec;
            DWORD ecount;
            DWORD ei;

            sec = (unsigned char*)malloc(
                g_fat.deviceSectorSize);

            if (!sec)
                return 0;

            if (!fat_read_sector(base + s, sec))
            {
                free(sec);
                return 0;
            }

            ecount = g_fat.deviceSectorSize / 32;

            for (ei = 0; ei < ecount; ++ei)
            {
                unsigned char* e = sec + ei * 32;
                UCHAR first = e[0];
                UCHAR attr = e[11];

                if (first == 0x00)
                {
                    if (!haveFree)
                    {
                        freeLba = base + s;
                        freeOff = ei * 32;
                        haveFree = 1;
                    }

                    free(sec);

                    *outLba = freeLba;
                    *outOff = freeOff;
                    *outExists = 0;
                    return 1;
                }

                if (first == 0xE5)
                {
                    if (!haveFree)
                    {
                        freeLba = base + s;
                        freeOff = ei * 32;
                        haveFree = 1;
                    }

                    continue;
                }

                if (attr == FAT_ATTR_LFN ||
                    (attr & FAT_ATTR_VOLUME))
                {
                    continue;
                }

                if (fat_short_equal(e, name11))
                {
                    DWORD hi = fat_le16(e + 20);
                    DWORD lo = fat_le16(e + 26);

                    if (outOldCluster)
                        *outOldCluster = (hi << 16) | lo;

                    if (outOldAttr)
                        *outOldAttr = attr;

                    *outLba = base + s;
                    *outOff = ei * 32;
                    *outExists = 1;

                    free(sec);
                    return 1;
                }
            }

            free(sec);
        }

        {
            DWORD next = fat_next_cluster(cluster);

            if (next)
            {
                cluster = next;
                ++hops;

                if (hops > g_fat.clusterCount + 1)
                    return 0;

                continue;
            }
        }

        if (haveFree)
        {
            *outLba = freeLba;
            *outOff = freeOff;
            *outExists = 0;
            return 1;
        }

        /*
            No free directory entries: extend the directory chain by one
            zeroed cluster and use its first slot.
        */
        {
            DWORD newCluster = 0;

            if (!fat_alloc_cluster(
                1,
                &newCluster))
            {
                return 0;
            }

            if (!fat_set_fat_value(
                cluster,
                newCluster))
            {
                fat_set_fat_value(newCluster, 0);
                return 0;
            }

            *outLba = fat_cluster_lba(newCluster);
            *outOff = 0;
            *outExists = 0;
            return *outLba ? 1 : 0;
        }
    }
}


static int fat_write_file_entry(
    DWORD lba,
    DWORD off,
    const unsigned char name11[11],
    DWORD firstCluster,
    DWORD size)
{
    unsigned char* sec;
    unsigned char* e;
    int i;
    WORD fatDate;
    WORD fatTime;

    if (!lba ||
        off + 32 > g_fat.deviceSectorSize ||
        !name11)
    {
        return 0;
    }

    sec = (unsigned char*)malloc(
        g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    if (!fat_read_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    e = sec + off;
    fat_zero(e, 32);

    for (i = 0; i < 11; ++i)
        e[i] = name11[i];

    e[11] = FAT_ATTR_ARCHIVE;

    fat_now_date_time(&fatDate, &fatTime);

    fat_put_le16(e + 14, fatTime); /* creation time */
    fat_put_le16(e + 16, fatDate); /* creation date */
    fat_put_le16(e + 18, fatDate); /* last access date */
    fat_put_le16(e + 22, fatTime); /* last write time */
    fat_put_le16(e + 24, fatDate); /* last write date */

    fat_put_le16(
        e + 20,
        (WORD)((firstCluster >> 16) & 0xFFFF));

    fat_put_le16(
        e + 26,
        (WORD)(firstCluster & 0xFFFF));

    fat_put_le32(e + 28, size);

    if (!fat_write_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    free(sec);
    return 1;
}



static int fat_write_directory_entry(
    DWORD lba,
    DWORD off,
    const unsigned char name11[11],
    DWORD firstCluster)
{
    unsigned char* sec;
    unsigned char* e;
    int i;
    WORD fatDate;
    WORD fatTime;

    if (!lba ||
        off + 32 > g_fat.deviceSectorSize ||
        !name11 ||
        firstCluster < 2)
    {
        return 0;
    }

    sec = (unsigned char*)malloc(
        g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    if (!fat_read_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    e = sec + off;
    fat_zero(e, 32);

    for (i = 0; i < 11; ++i)
        e[i] = name11[i];

    e[11] = FAT_ATTR_DIRECTORY;

    fat_now_date_time(&fatDate, &fatTime);

    fat_put_le16(e + 14, fatTime); /* creation time */
    fat_put_le16(e + 16, fatDate); /* creation date */
    fat_put_le16(e + 18, fatDate); /* last access date */
    fat_put_le16(e + 22, fatTime); /* last write time */
    fat_put_le16(e + 24, fatDate); /* last write date */

    fat_put_le16(
        e + 20,
        (WORD)((firstCluster >> 16) & 0xFFFF));

    fat_put_le16(
        e + 26,
        (WORD)(firstCluster & 0xFFFF));

    fat_put_le32(e + 28, 0);

    if (!fat_write_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    free(sec);
    return 1;
}


static void fat_make_dot_name(
    unsigned char out11[11],
    int parent)
{
    int i;

    for (i = 0; i < 11; ++i)
        out11[i] = ' ';

    out11[0] = '.';

    if (parent)
        out11[1] = '.';
}


static int fat_init_directory_cluster(
    DWORD cluster,
    DWORD parentCluster)
{
    unsigned char* sec;
    unsigned char dot[11];
    unsigned char dotdot[11];
    unsigned char* e;
    WORD fatDate;
    WORD fatTime;

    /*
        FAT32 root is represented as parent cluster 0 in a child directory's
        ".." entry. Non-root parents carry their real cluster number.
    */
    if (cluster < 2 ||
        (parentCluster != 0 && parentCluster < 2))
    {
        return 0;
    }

    sec = (unsigned char*)malloc(
        g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    fat_zero(sec, (int)g_fat.deviceSectorSize);

    fat_make_dot_name(dot, 0);
    fat_make_dot_name(dotdot, 1);
    fat_now_date_time(&fatDate, &fatTime);

    e = sec;

    {
        int i;
        for (i = 0; i < 11; ++i)
            e[i] = dot[i];
    }

    e[11] = FAT_ATTR_DIRECTORY;
    fat_put_le16(e + 14, fatTime);
    fat_put_le16(e + 16, fatDate);
    fat_put_le16(e + 18, fatDate);
    fat_put_le16(e + 22, fatTime);
    fat_put_le16(e + 24, fatDate);
    fat_put_le16(e + 20, (WORD)((cluster >> 16) & 0xFFFF));
    fat_put_le16(e + 26, (WORD)(cluster & 0xFFFF));

    e = sec + 32;

    {
        int i;
        for (i = 0; i < 11; ++i)
            e[i] = dotdot[i];
    }

    e[11] = FAT_ATTR_DIRECTORY;
    fat_put_le16(e + 14, fatTime);
    fat_put_le16(e + 16, fatDate);
    fat_put_le16(e + 18, fatDate);
    fat_put_le16(e + 22, fatTime);
    fat_put_le16(e + 24, fatDate);
    fat_put_le16(e + 20, (WORD)((parentCluster >> 16) & 0xFFFF));
    fat_put_le16(e + 26, (WORD)(parentCluster & 0xFFFF));

    /*
        fat_alloc_cluster(...,1) already zeroed the complete cluster, so only
        the first sector needs rewriting with "." and "..".
    */
    if (!fat_write_sector(
        fat_cluster_lba(cluster),
        sec))
    {
        free(sec);
        return 0;
    }

    free(sec);
    return 1;
}


static int fat_find_existing_short_entry(
    DWORD dirCluster,
    const unsigned char name11[11],
    DWORD* outLba,
    DWORD* outOff,
    DWORD* outFirstCluster,
    UCHAR* outAttr,
    int* outHasLfn)
{
    DWORD cluster = dirCluster;
    DWORD hops = 0;
    int lfnPending = 0;

    if (outLba)*outLba = 0;
    if (outOff)*outOff = 0;
    if (outFirstCluster)*outFirstCluster = 0;
    if (outAttr)*outAttr = 0;
    if (outHasLfn)*outHasLfn = 0;

    if (dirCluster < 2 || !name11)
        return 0;

    for (;;)
    {
        DWORD base = fat_cluster_lba(cluster);
        DWORD s;

        if (!base)
            return 0;

        for (s = 0; s < g_fat.sectorsPerCluster; ++s)
        {
            unsigned char* sec;
            DWORD entries;
            DWORD ei;

            sec = (unsigned char*)malloc(
                g_fat.deviceSectorSize);

            if (!sec)
                return 0;

            if (!fat_read_sector(base + s, sec))
            {
                free(sec);
                return 0;
            }

            entries = g_fat.deviceSectorSize / 32;

            for (ei = 0; ei < entries; ++ei)
            {
                unsigned char* e = sec + ei * 32;
                UCHAR first = e[0];
                UCHAR attr = e[11];

                if (first == 0x00)
                {
                    free(sec);
                    return 0;
                }

                if (first == 0xE5)
                {
                    lfnPending = 0;
                    continue;
                }

                if (attr == FAT_ATTR_LFN)
                {
                    lfnPending = 1;
                    continue;
                }

                if (attr & FAT_ATTR_VOLUME)
                {
                    lfnPending = 0;
                    continue;
                }

                if (fat_short_equal(e, name11))
                {
                    DWORD hi = fat_le16(e + 20);
                    DWORD lo = fat_le16(e + 26);

                    if (outLba)*outLba = base + s;
                    if (outOff)*outOff = ei * 32;
                    if (outFirstCluster)
                        *outFirstCluster = (hi << 16) | lo;
                    if (outAttr)*outAttr = attr;
                    if (outHasLfn)*outHasLfn = lfnPending;

                    free(sec);
                    return 1;
                }

                lfnPending = 0;
            }

            free(sec);
        }

        cluster = fat_next_cluster(cluster);

        if (!cluster)
            return 0;

        ++hops;

        if (hops > g_fat.clusterCount + 1)
            return 0;
    }
}



static UCHAR fat_lfn_checksum(const unsigned char name11[11])
{
    UCHAR sum = 0;
    int i;

    for (i = 0; i < 11; ++i)
        sum = (UCHAR)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);

    return sum;
}


static int fat_leaf_valid(const char* leaf)
{
    /*
        USB2XB intentionally creates FAT32 names that can round-trip through
        the Xbox HDD.  Existing FAT32 LFNs may be longer and remain readable,
        but mkdir/rename/create use the native FATX component rules.
    */
    return DD_FatxNameValid(leaf);
}


static int fat_short_name_exact_text(
    const char* leaf,
    const unsigned char name11[11])
{
    char rendered[16];
    int i;

    fat_short_name(name11, rendered, sizeof(rendered));

    for (i = 0;; ++i)
    {
        if (leaf[i] != rendered[i])
            return 0;

        if (!leaf[i])
            return 1;
    }
}


static int fat_short_exists(
    DWORD dirCluster,
    const unsigned char name11[11])
{
    return fat_find_existing_short_entry(
        dirCluster,
        name11,
        0, 0, 0, 0, 0);
}



#define FAT_ALIAS_FAST_PROBES 64

/*
    Build the canonical pieces used by the VFAT hidden "~n" alias.
    This mirrors fat_make_short_alias(), but lets the create planner prebuild
    a small alias window and test it while it is already walking the directory.
*/
static void fat_alias_parts(
    const char* leaf,
    char* base,
    int baseCap,
    int* outBaseN,
    char* ext,
    int extCap,
    int* outExtN)
{
    int baseN = 0;
    int extN = 0;
    int len;
    int dot = -1;
    int i;

    if (base && baseCap > 0)
        base[0] = 0;
    if (ext && extCap > 0)
        ext[0] = 0;
    if (outBaseN)
        *outBaseN = 0;
    if (outExtN)
        *outExtN = 0;

    if (!leaf || !base || baseCap < 2 || !ext || extCap < 1)
        return;

    len = fat_slen(leaf);

    for (i = len - 1; i >= 0; --i)
    {
        if (leaf[i] == '.')
        {
            dot = i;
            break;
        }
    }

    for (i = 0;
        i < (dot > 0 ? dot : len) && baseN < baseCap - 1;
        ++i)
    {
        int c = (unsigned char)leaf[i];
        int sc;

        if (c == ' ' || c == '.')
            continue;

        sc = fat_short_char(c);
        base[baseN++] = (char)(sc ? sc : '_');
    }

    if (baseN == 0)
        base[baseN++] = '_';

    base[baseN] = 0;

    if (dot > 0 && dot < len - 1)
    {
        for (i = dot + 1;
            i < len && extN < extCap - 1;
            ++i)
        {
            int c = (unsigned char)leaf[i];
            int sc;

            if (c == ' ' || c == '.')
                continue;

            sc = fat_short_char(c);
            ext[extN++] = (char)(sc ? sc : '_');
        }
    }

    ext[extN] = 0;

    if (outBaseN)
        *outBaseN = baseN;
    if (outExtN)
        *outExtN = extN;
}


static int fat_numbered_alias(
    const char* base,
    int baseN,
    const char* ext,
    int extN,
    DWORD n,
    unsigned char out11[11])
{
    char digits[8];
    int dn = 0;
    DWORD v = n;
    int suffixLen;
    int keep;
    int at;
    int i;

    if (!base || baseN < 1 || !ext || !out11 || n < 1)
        return 0;

    while (v && dn < (int)sizeof(digits))
    {
        digits[dn++] = (char)('0' + (v % 10));
        v /= 10;
    }

    suffixLen = 1 + dn; /* '~' + digits */
    keep = 8 - suffixLen;

    if (keep < 1)
        return 0;

    for (i = 0; i < 11; ++i)
        out11[i] = ' ';

    at = 0;

    for (i = 0; i < baseN && at < keep; ++i)
        out11[at++] = (unsigned char)base[i];

    out11[at++] = '~';

    while (dn > 0 && at < 8)
        out11[at++] = (unsigned char)digits[--dn];

    for (i = 0; i < extN && i < 3; ++i)
        out11[8 + i] = (unsigned char)ext[i];

    return 1;
}


static int fat_make_short_alias(
    DWORD dirCluster,
    const char* leaf,
    unsigned char out11[11],
    int* outNeedsLfn)
{
    unsigned char exact[11];
    char base[256];
    char ext[256];
    int baseN = 0;
    int extN = 0;
    int len;
    int dot = -1;
    int i;
    DWORD n;

    if (outNeedsLfn)
        *outNeedsLfn = 0;

    if (!leaf || !out11 || !fat_leaf_valid(leaf))
        return 0;

    /*
        If the requested spelling is already an unused canonical 8.3 name,
        publish it without an LFN chain.
    */
    if (fat_make_short_name(leaf, exact) &&
        !fat_short_exists(dirCluster, exact))
    {
        for (i = 0; i < 11; ++i)
            out11[i] = exact[i];

        if (outNeedsLfn)
            *outNeedsLfn =
            fat_short_name_exact_text(leaf, exact) ? 0 : 1;

        return 1;
    }

    len = fat_slen(leaf);

    for (i = len - 1; i >= 0; --i)
    {
        if (leaf[i] == '.')
        {
            dot = i;
            break;
        }
    }

    /*
        Build an ASCII-safe alias stem. Characters legal in VFAT but illegal
        in an SFN are represented by '_' in the hidden 8.3 alias.
    */
    for (i = 0; i < (dot > 0 ? dot : len) && baseN < (int)sizeof(base) - 1; ++i)
    {
        int c = (unsigned char)leaf[i];
        int sc;

        if (c == ' ' || c == '.')
            continue;

        sc = fat_short_char(c);

        if (sc)
            base[baseN++] = (char)sc;
        else
            base[baseN++] = '_';
    }

    if (baseN == 0)
        base[baseN++] = '_';

    base[baseN] = 0;

    if (dot > 0 && dot < len - 1)
    {
        for (i = dot + 1; i < len && extN < (int)sizeof(ext) - 1; ++i)
        {
            int c = (unsigned char)leaf[i];
            int sc;

            if (c == ' ' || c == '.')
                continue;

            sc = fat_short_char(c);

            if (sc)
                ext[extN++] = (char)sc;
            else
                ext[extN++] = '_';
        }
    }

    ext[extN] = 0;

    for (n = 1; n <= 999999u; ++n)
    {
        char digits[8];
        int dn = 0;
        DWORD v = n;
        int suffixLen;
        int keep;
        int at;

        while (v && dn < (int)sizeof(digits))
        {
            digits[dn++] = (char)('0' + (v % 10));
            v /= 10;
        }

        suffixLen = 1 + dn; /* '~' + digits */
        keep = 8 - suffixLen;

        if (keep < 1)
            continue;

        for (i = 0; i < 11; ++i)
            out11[i] = ' ';

        at = 0;

        for (i = 0; i < baseN && at < keep; ++i)
            out11[at++] = (unsigned char)base[i];

        out11[at++] = '~';

        while (dn > 0 && at < 8)
            out11[at++] = (unsigned char)digits[--dn];

        for (i = 0; i < extN && i < 3; ++i)
            out11[8 + i] = (unsigned char)ext[i];

        if (!fat_short_exists(dirCluster, out11))
        {
            if (outNeedsLfn)
                *outNeedsLfn = 1;

            return 1;
        }
    }

    return 0;
}


static void fat_lfn_build_entry(
    unsigned char out[32],
    const char* longName,
    int seq,
    int count,
    UCHAR checksum)
{
    static const unsigned char pos[13] =
    {
        1,3,5,7,9,
        14,16,18,20,22,24,
        28,30
    };

    int len = fat_slen(longName);
    int base = (seq - 1) * 13;
    int i;

    for (i = 0; i < 32; ++i)
        out[i] = 0xFF;

    out[0] = (unsigned char)seq;

    if (seq == count)
        out[0] |= 0x40;

    out[11] = FAT_ATTR_LFN;
    out[12] = 0;
    out[13] = checksum;
    out[26] = 0;
    out[27] = 0;

    for (i = 0; i < 13; ++i)
    {
        int at = base + i;
        WORD wc;

        if (at < len)
            wc = (WORD)(unsigned char)longName[at];
        else if (at == len)
            wc = 0x0000;
        else
            wc = 0xFFFF;

        fat_put_le16(out + pos[i], wc);
    }
}


static int fat_reserve_slots(
    DWORD dirCluster,
    int needed,
    FatSlot* outSlots)
{
    DWORD cluster = dirCluster;
    DWORD hops = 0;
    int run = 0;
    int endSeen = 0;

    if (dirCluster < 2 ||
        needed<1 ||
        needed>FAT_LFN_MAX_ENTRIES + 1 ||
        !outSlots)
    {
        return 0;
    }

    for (;;)
    {
        DWORD base = fat_cluster_lba(cluster);
        DWORD s;

        if (!base)
            return 0;

        for (s = 0; s < g_fat.sectorsPerCluster; ++s)
        {
            unsigned char* sec;
            DWORD entries;
            DWORD ei;

            sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

            if (!sec)
                return 0;

            if (!fat_read_sector(base + s, sec))
            {
                free(sec);
                return 0;
            }

            entries = g_fat.deviceSectorSize / 32;

            for (ei = 0; ei < entries; ++ei)
            {
                unsigned char first = sec[ei * 32];
                int freeSlot;

                if (first == 0x00)
                    endSeen = 1;

                freeSlot = endSeen || first == 0xE5;

                if (freeSlot)
                {
                    outSlots[run].lba = base + s;
                    outSlots[run].off = ei * 32;
                    ++run;

                    if (run >= needed)
                    {
                        free(sec);
                        return 1;
                    }
                }
                else
                {
                    run = 0;
                }
            }

            free(sec);
        }

        {
            DWORD next = fat_next_cluster(cluster);

            if (next)
            {
                cluster = next;
                ++hops;

                if (hops > g_fat.clusterCount + 1)
                    return 0;
            }
            else
            {
                DWORD newCluster = 0;

                if (!fat_alloc_cluster(1, &newCluster))
                    return 0;

                if (!fat_set_fat_value(cluster, newCluster))
                {
                    fat_set_fat_value(newCluster, 0);
                    return 0;
                }

                cluster = newCluster;
                endSeen = 1;
            }
        }
    }
}


static int fat_locate_name(
    DWORD dirCluster,
    const char* leaf,
    FatLocatedEntry* out)
{
    DWORD cluster = dirCluster;
    DWORD hops = 0;
    FatDir lfnState;
    FatSlot pending[FAT_LFN_MAX_ENTRIES];
    int pendingCount = 0;
    UCHAR pendingChecksum = 0;
    int checksumValid = 0;

    if (out)
        fat_zero(out, sizeof(*out));

    if (!out || dirCluster < 2 || !leaf || !leaf[0])
        return 0;

    fat_zero(&lfnState, sizeof(lfnState));
    fat_lfn_reset(&lfnState);

    for (;;)
    {
        DWORD base = fat_cluster_lba(cluster);
        DWORD s;

        if (!base)
            return 0;

        for (s = 0; s < g_fat.sectorsPerCluster; ++s)
        {
            unsigned char* sec;
            DWORD entries;
            DWORD ei;

            sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

            if (!sec)
                return 0;

            if (!fat_read_sector(base + s, sec))
            {
                free(sec);
                return 0;
            }

            entries = g_fat.deviceSectorSize / 32;

            for (ei = 0; ei < entries; ++ei)
            {
                unsigned char* e = sec + ei * 32;
                UCHAR first = e[0];
                UCHAR attr = e[11];

                if (first == 0x00)
                {
                    free(sec);
                    return 0;
                }

                if (first == 0xE5)
                {
                    fat_lfn_reset(&lfnState);
                    pendingCount = 0;
                    checksumValid = 0;
                    continue;
                }

                if (attr == FAT_ATTR_LFN)
                {
                    int ord = e[0];

                    if (ord & 0x40)
                    {
                        fat_lfn_reset(&lfnState);
                        pendingCount = 0;
                        pendingChecksum = e[13];
                        checksumValid = 1;
                    }
                    else if (!checksumValid ||
                        pendingChecksum != e[13])
                    {
                        fat_lfn_reset(&lfnState);
                        pendingCount = 0;
                        checksumValid = 0;
                    }

                    if (pendingCount < FAT_LFN_MAX_ENTRIES)
                    {
                        pending[pendingCount].lba = base + s;
                        pending[pendingCount].off = ei * 32;
                        ++pendingCount;
                    }

                    fat_lfn_put(&lfnState, e);
                    continue;
                }

                if (attr & FAT_ATTR_VOLUME)
                {
                    fat_lfn_reset(&lfnState);
                    pendingCount = 0;
                    checksumValid = 0;
                    continue;
                }

                {
                    char visible[DD_NAME_MAX];
                    int validLfn =
                        lfnState.lfnActive &&
                        lfnState.lfn[0] &&
                        checksumValid &&
                        pendingChecksum == fat_lfn_checksum(e);

                    if (validLfn)
                        fat_scpy(visible, sizeof(visible), lfnState.lfn);
                    else
                        fat_short_name(e, visible, sizeof(visible));

                    if (fat_nameeq(visible, leaf))
                    {
                        DWORD hi = fat_le16(e + 20);
                        DWORD lo = fat_le16(e + 26);
                        int i;

                        out->found = 1;
                        out->shortSlot.lba = base + s;
                        out->shortSlot.off = ei * 32;
                        out->lfnCount = validLfn ? pendingCount : 0;

                        for (i = 0; i < out->lfnCount; ++i)
                            out->lfnSlots[i] = pending[i];

                        for (i = 0; i < 11; ++i)
                            out->shortName[i] = e[i];

                        out->firstCluster = (hi << 16) | lo;
                        out->size = fat_le32(e + 28);
                        out->attr = attr;

                        free(sec);
                        return 1;
                    }
                }

                fat_lfn_reset(&lfnState);
                pendingCount = 0;
                checksumValid = 0;
            }

            free(sec);
        }

        cluster = fat_next_cluster(cluster);

        if (!cluster)
            return 0;

        ++hops;

        if (hops > g_fat.clusterCount + 1)
            return 0;
    }
}



/*
    Plan creation of a new directory entry in one directory walk.

    The older create path independently walked the same directory to:
      1) find a visible-name collision,
      2) find an unused hidden 8.3 alias, and
      3) find a contiguous run of free LFN/SFN slots.

    Large directories made that repeated work dominate tiny-file copies.
    This planner gathers all three facts together.  The 64 common "~n"
    aliases are checked during the same pass; only unusually collision-heavy
    directories fall back to the older exhaustive alias search.
*/
static int fat_plan_create_entry(
    DWORD dirCluster,
    const char* leaf,
    FatLocatedEntry* outExisting,
    unsigned char out11[11],
    int* outNeedsLfn,
    int* outLfnCount,
    FatSlot* outSlots)
{
    unsigned char exact[11];
    int exactPossible;
    int exactTaken = 0;
    char aliasBase[256];
    char aliasExt[256];
    int aliasBaseN = 0;
    int aliasExtN = 0;
    unsigned char aliases[FAT_ALIAS_FAST_PROBES][11];
    unsigned char aliasUsed[FAT_ALIAS_FAST_PROBES];
    int aliasCount = 0;
    int maxLfnCount;
    int maxNeeded;
    int chosenNeedsLfn = 0;
    int chosenLfnCount = 0;
    int i;

    DWORD cluster = dirCluster;
    DWORD hops = 0;
    int endSeen = 0;
    int scanDone = 0;

    FatSlot currentRun[FAT_LFN_MAX_ENTRIES + 1];
    FatSlot bestRun[FAT_LFN_MAX_ENTRIES + 1];
    int currentRunCount = 0;
    int bestRunCount = 0;

    FatDir lfnState;
    FatSlot pending[FAT_LFN_MAX_ENTRIES];
    int pendingCount = 0;
    UCHAR pendingChecksum = 0;
    int checksumValid = 0;

    if (outExisting)
        fat_zero(outExisting, sizeof(*outExisting));
    if (outNeedsLfn)
        *outNeedsLfn = 0;
    if (outLfnCount)
        *outLfnCount = 0;
    if (outSlots)
        fat_zero(outSlots,
            sizeof(FatSlot) * (FAT_LFN_MAX_ENTRIES + 1));

    if (dirCluster < 2 ||
        !leaf || !leaf[0] ||
        !outExisting || !out11 ||
        !outNeedsLfn || !outLfnCount || !outSlots ||
        !fat_leaf_valid(leaf))
    {
        return 0;
    }

    maxLfnCount = (fat_slen(leaf) + 12) / 13;
    if (maxLfnCount < 1 || maxLfnCount > FAT_LFN_MAX_ENTRIES)
        return 0;
    maxNeeded = maxLfnCount + 1;

    exactPossible = fat_make_short_name(leaf, exact);

    fat_alias_parts(
        leaf,
        aliasBase,
        sizeof(aliasBase),
        &aliasBaseN,
        aliasExt,
        sizeof(aliasExt),
        &aliasExtN);

    fat_zero(aliasUsed, sizeof(aliasUsed));

    for (i = 0; i < FAT_ALIAS_FAST_PROBES; ++i)
    {
        if (!fat_numbered_alias(
            aliasBase,
            aliasBaseN,
            aliasExt,
            aliasExtN,
            (DWORD)(i + 1),
            aliases[i]))
        {
            break;
        }
        ++aliasCount;
    }

    fat_zero(&lfnState, sizeof(lfnState));
    fat_lfn_reset(&lfnState);
    fat_zero(currentRun, sizeof(currentRun));
    fat_zero(bestRun, sizeof(bestRun));

    while (!scanDone)
    {
        DWORD base = fat_cluster_lba(cluster);
        DWORD s;

        if (!base)
            return 0;

        for (s = 0; s < g_fat.sectorsPerCluster && !scanDone; ++s)
        {
            unsigned char* sec;
            DWORD entries;
            DWORD ei;

            sec = (unsigned char*)malloc(g_fat.deviceSectorSize);
            if (!sec)
                return 0;

            if (!fat_read_sector(base + s, sec))
            {
                free(sec);
                return 0;
            }

            entries = g_fat.deviceSectorSize / 32;

            for (ei = 0; ei < entries; ++ei)
            {
                unsigned char* e = sec + ei * 32;
                UCHAR first = e[0];
                UCHAR attr = e[11];
                int freeSlot = 0;

                if (first == 0x00)
                    endSeen = 1;

                if (endSeen || first == 0xE5)
                    freeSlot = 1;

                if (freeSlot)
                {
                    if (currentRunCount < maxNeeded)
                    {
                        currentRun[currentRunCount].lba = base + s;
                        currentRun[currentRunCount].off = ei * 32;
                        ++currentRunCount;
                    }

                    if (currentRunCount > bestRunCount)
                    {
                        bestRunCount = currentRunCount;
                        for (i = 0; i < bestRunCount; ++i)
                            bestRun[i] = currentRun[i];
                    }

                    fat_lfn_reset(&lfnState);
                    pendingCount = 0;
                    checksumValid = 0;

                    /* After 0x00 there can be no later live directory entry. */
                    if (endSeen && bestRunCount >= maxNeeded)
                    {
                        scanDone = 1;
                        break;
                    }

                    continue;
                }

                currentRunCount = 0;

                if (attr == FAT_ATTR_LFN)
                {
                    int ord = e[0];

                    if (ord & 0x40)
                    {
                        fat_lfn_reset(&lfnState);
                        pendingCount = 0;
                        pendingChecksum = e[13];
                        checksumValid = 1;
                    }
                    else if (!checksumValid || pendingChecksum != e[13])
                    {
                        fat_lfn_reset(&lfnState);
                        pendingCount = 0;
                        checksumValid = 0;
                    }

                    if (pendingCount < FAT_LFN_MAX_ENTRIES)
                    {
                        pending[pendingCount].lba = base + s;
                        pending[pendingCount].off = ei * 32;
                        ++pendingCount;
                    }

                    fat_lfn_put(&lfnState, e);
                    continue;
                }

                if (attr & FAT_ATTR_VOLUME)
                {
                    fat_lfn_reset(&lfnState);
                    pendingCount = 0;
                    checksumValid = 0;
                    continue;
                }

                if (exactPossible && fat_short_equal(e, exact))
                    exactTaken = 1;

                for (i = 0; i < aliasCount; ++i)
                {
                    if (!aliasUsed[i] && fat_short_equal(e, aliases[i]))
                    {
                        aliasUsed[i] = 1;
                        break;
                    }
                }

                {
                    char visible[DD_NAME_MAX];
                    int validLfn =
                        lfnState.lfnActive &&
                        lfnState.lfn[0] &&
                        checksumValid &&
                        pendingChecksum == fat_lfn_checksum(e);

                    if (validLfn)
                        fat_scpy(visible, sizeof(visible), lfnState.lfn);
                    else
                        fat_short_name(e, visible, sizeof(visible));

                    if (fat_nameeq(visible, leaf))
                    {
                        DWORD hi = fat_le16(e + 20);
                        DWORD lo = fat_le16(e + 26);

                        outExisting->found = 1;
                        outExisting->shortSlot.lba = base + s;
                        outExisting->shortSlot.off = ei * 32;
                        outExisting->lfnCount = validLfn ? pendingCount : 0;

                        for (i = 0; i < outExisting->lfnCount; ++i)
                            outExisting->lfnSlots[i] = pending[i];

                        for (i = 0; i < 11; ++i)
                            outExisting->shortName[i] = e[i];

                        outExisting->firstCluster = (hi << 16) | lo;
                        outExisting->size = fat_le32(e + 28);
                        outExisting->attr = attr;

                        free(sec);
                        return 1;
                    }
                }

                fat_lfn_reset(&lfnState);
                pendingCount = 0;
                checksumValid = 0;
            }

            free(sec);
        }

        if (scanDone)
            break;

        {
            DWORD next = fat_next_cluster(cluster);

            if (!next)
                break;

            cluster = next;
            ++hops;

            if (hops > g_fat.clusterCount + 1)
                return 0;
        }
    }

    if (exactPossible && !exactTaken)
    {
        for (i = 0; i < 11; ++i)
            out11[i] = exact[i];

        chosenNeedsLfn =
            fat_short_name_exact_text(leaf, exact) ? 0 : 1;
    }
    else
    {
        int chosen = -1;

        for (i = 0; i < aliasCount; ++i)
        {
            if (!aliasUsed[i])
            {
                chosen = i;
                break;
            }
        }

        if (chosen >= 0)
        {
            for (i = 0; i < 11; ++i)
                out11[i] = aliases[chosen][i];
            chosenNeedsLfn = 1;
        }
        else
        {
            /* Very collision-heavy directory: preserve the exhaustive path. */
            if (!fat_make_short_alias(
                dirCluster,
                leaf,
                out11,
                &chosenNeedsLfn))
            {
                return 0;
            }
        }
    }

    chosenLfnCount = chosenNeedsLfn ? maxLfnCount : 0;

    {
        int needed = chosenLfnCount + 1;

        if (bestRunCount >= needed)
        {
            for (i = 0; i < needed; ++i)
                outSlots[i] = bestRun[i];
        }
        else
        {
            /* Rare full/fragmented directory: let the proven extender handle it. */
            if (!fat_reserve_slots(dirCluster, needed, outSlots))
                return 0;
        }
    }

    *outNeedsLfn = chosenNeedsLfn;
    *outLfnCount = chosenLfnCount;
    return 1;
}


static int fat_publish_named_entry(
    const FatSlot* lfnSlots,
    int lfnCount,
    const FatSlot* shortSlot,
    const unsigned char name11[11],
    const char* longName,
    UCHAR attr,
    DWORD firstCluster,
    DWORD size)
{
    int i;

    if (!shortSlot || !name11)
        return 0;

    if (lfnCount > 0)
    {
        UCHAR checksum = fat_lfn_checksum(name11);

        if (!lfnSlots || !longName || !longName[0])
            return 0;

        i = 0;

        while (i < lfnCount)
        {
            unsigned char* sec;
            DWORD lba = lfnSlots[i].lba;
            int j = i;

            sec = (unsigned char*)malloc(g_fat.deviceSectorSize);

            if (!sec)
                return 0;

            if (!fat_read_sector(lba, sec))
            {
                free(sec);
                return 0;
            }

            /*
                Consecutive LFN slots are normally in the same directory
                sector. Patch every slot in that sector and issue one metadata
                write instead of one WRITE(10) per 32-byte entry.
            */
            while (j < lfnCount && lfnSlots[j].lba == lba)
            {
                unsigned char e[32];
                int seq = lfnCount - j;
                int k;

                fat_lfn_build_entry(
                    e,
                    longName,
                    seq,
                    lfnCount,
                    checksum);

                for (k = 0; k < 32; ++k)
                    sec[lfnSlots[j].off + k] = e[k];

                ++j;
            }

            if (!fat_write_sector(lba, sec))
            {
                free(sec);
                return 0;
            }

            free(sec);
            i = j;
        }
    }

    if (attr & FAT_ATTR_DIRECTORY)
    {
        return fat_write_directory_entry(
            shortSlot->lba,
            shortSlot->off,
            name11,
            firstCluster);
    }

    return fat_write_file_entry(
        shortSlot->lba,
        shortSlot->off,
        name11,
        firstCluster,
        size);
}


static int fat_mark_entry_deleted(
    DWORD lba,
    DWORD off)
{
    unsigned char* sec;

    if (!lba ||
        off + 32 > g_fat.deviceSectorSize)
    {
        return 0;
    }

    sec = (unsigned char*)malloc(
        g_fat.deviceSectorSize);

    if (!sec)
        return 0;

    if (!fat_read_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    sec[off] = 0xE5;

    if (!fat_write_sector(lba, sec))
    {
        free(sec);
        return 0;
    }

    free(sec);
    return 1;
}


static int fat_delete_located(
    const FatLocatedEntry* loc)
{
    int i;

    if (!loc || !loc->found)
        return 0;

    /*
        Hide the object first by deleting the SFN entry. Orphaned LFN entries
        after a power failure are ignored by normal FAT readers.
    */
    if (!fat_mark_entry_deleted(
        loc->shortSlot.lba,
        loc->shortSlot.off))
    {
        return 0;
    }

    for (i = 0; i < loc->lfnCount; ++i)
    {
        if (!fat_mark_entry_deleted(
            loc->lfnSlots[i].lba,
            loc->lfnSlots[i].off))
        {
            return 0;
        }
    }

    return 1;
}




static int fat_directory_empty(DWORD cluster)
{
    FatDir* d;
    FatEntry e;
    int empty = 1;

    if (cluster < 2)
        return 0;

    d = fat_dir_open_cluster(cluster);

    if (!d)
        return 0;

    while (fat_dir_next_raw(d, &e))
    {
        if (fat_nameeq(e.name, ".") ||
            fat_nameeq(e.name, ".."))
        {
            continue;
        }

        empty = 0;
        break;
    }

    fat_dir_close(d);
    return empty;
}


static int usb_mkdir(
    DDStorage* s,
    const char* path)
{
    DWORD parentCluster;
    char leaf[DD_NAME_MAX];
    FatLocatedEntry existing;
    unsigned char name11[11];
    int needsLfn = 0;
    int lfnCount = 0;
    FatSlot slots[FAT_LFN_MAX_ENTRIES + 1];
    DWORD newCluster = 0;

    (void)s;

    parentCluster = 0;
    fat_zero(&existing, sizeof(existing));
    fat_zero(slots, sizeof(slots));

    if (!fat_mount() ||
        !path ||
        !path[0])
    {
        return 0;
    }

    if (!fat_split_parent(
        path,
        &parentCluster,
        leaf,
        sizeof(leaf)))
    {
        return 0;
    }

    if (!fat_plan_create_entry(
        parentCluster,
        leaf,
        &existing,
        name11,
        &needsLfn,
        &lfnCount,
        slots))
    {
        return 0;
    }

    if (existing.found)
        return (existing.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;

    if (!fat_alloc_cluster(
        1,
        &newCluster))
    {
        fat_flush_fsinfo();
        return 0;
    }

    if (!fat_init_directory_cluster(
        newCluster,
        parentCluster == g_fat.rootCluster ?
        0 : parentCluster))
    {
        fat_free_chain(newCluster);
        fat_flush_fsinfo();
        return 0;
    }

    if (!fat_publish_named_entry(
        slots,
        lfnCount,
        &slots[lfnCount],
        name11,
        needsLfn ? leaf : 0,
        FAT_ATTR_DIRECTORY,
        newCluster,
        0))
    {
        fat_free_chain(newCluster);
        fat_flush_fsinfo();
        return 0;
    }

    fat_flush_fsinfo();
    return 1;
}


static int usb_rename(
    DDStorage* s,
    const char* oldPath,
    const char* newPath)
{
    DWORD oldParent;
    DWORD newParent;
    char oldLeaf[DD_NAME_MAX];
    char newLeaf[DD_NAME_MAX];
    FatLocatedEntry oldEntry;
    FatLocatedEntry collision;
    unsigned char new11[11];
    int needsLfn = 0;
    int lfnCount = 0;
    FatSlot slots[FAT_LFN_MAX_ENTRIES + 1];

    (void)s;

    oldParent = 0;
    newParent = 0;
    fat_zero(&oldEntry, sizeof(oldEntry));
    fat_zero(&collision, sizeof(collision));
    fat_zero(slots, sizeof(slots));

    if (!fat_mount() ||
        !oldPath || !oldPath[0] ||
        !newPath || !newPath[0])
    {
        return 0;
    }

    if (!fat_split_parent(
        oldPath,
        &oldParent,
        oldLeaf,
        sizeof(oldLeaf)) ||
        !fat_split_parent(
            newPath,
            &newParent,
            newLeaf,
            sizeof(newLeaf)))
    {
        return 0;
    }

    if (oldParent != newParent)
        return 0;

    if (fat_nameeq(oldLeaf, newLeaf))
        return 1;

    if (!fat_locate_name(
        oldParent,
        oldLeaf,
        &oldEntry))
    {
        return 0;
    }

    if (fat_locate_name(
        newParent,
        newLeaf,
        &collision))
    {
        return 0;
    }

    if (!fat_make_short_alias(
        newParent,
        newLeaf,
        new11,
        &needsLfn))
    {
        return 0;
    }

    if (needsLfn)
    {
        lfnCount = (fat_slen(newLeaf) + 12) / 13;

        if (lfnCount<1 ||
            lfnCount>FAT_LFN_MAX_ENTRIES)
        {
            return 0;
        }
    }

    if (!fat_reserve_slots(
        newParent,
        lfnCount + 1,
        slots))
    {
        return 0;
    }

    /*
        Publish the new name first, then remove the old directory-entry chain.
        Both names may briefly reference the same cluster chain, but the data
        itself is never moved or rewritten.
    */
    if (!fat_publish_named_entry(
        slots,
        lfnCount,
        &slots[lfnCount],
        new11,
        needsLfn ? newLeaf : 0,
        oldEntry.attr,
        oldEntry.firstCluster,
        oldEntry.size))
    {
        fat_flush_fsinfo();
        return 0;
    }

    if (!fat_delete_located(&oldEntry))
    {
        FatLocatedEntry rollback;

        fat_zero(&rollback, sizeof(rollback));

        if (fat_locate_name(
            newParent,
            newLeaf,
            &rollback))
        {
            fat_delete_located(&rollback);
        }

        fat_flush_fsinfo();
        return 0;
    }

    fat_flush_fsinfo();
    return 1;
}


static int usb_remove_file(
    DDStorage* s,
    const char* path)
{
    DWORD parentCluster;
    char leaf[DD_NAME_MAX];
    FatLocatedEntry loc;

    (void)s;

    parentCluster = 0;
    fat_zero(&loc, sizeof(loc));

    if (!fat_mount() ||
        !fat_split_parent(
            path,
            &parentCluster,
            leaf,
            sizeof(leaf)))
    {
        return 0;
    }

    if (!fat_locate_name(
        parentCluster,
        leaf,
        &loc))
    {
        return 0;
    }

    if (loc.attr & FAT_ATTR_DIRECTORY)
        return 0;

    if (!fat_delete_located(&loc))
        return 0;

    if (loc.firstCluster >= 2)
        fat_free_chain(loc.firstCluster);

    fat_flush_fsinfo();
    return 1;
}


static int usb_remove_dir(
    DDStorage* s,
    const char* path)
{
    DWORD parentCluster;
    char leaf[DD_NAME_MAX];
    FatLocatedEntry loc;

    (void)s;

    parentCluster = 0;
    fat_zero(&loc, sizeof(loc));

    if (!fat_mount() ||
        !fat_split_parent(
            path,
            &parentCluster,
            leaf,
            sizeof(leaf)))
    {
        return 0;
    }

    if (!fat_locate_name(
        parentCluster,
        leaf,
        &loc))
    {
        return 0;
    }

    if (!(loc.attr & FAT_ATTR_DIRECTORY) ||
        loc.firstCluster < 2)
    {
        return 0;
    }

    if (!fat_directory_empty(loc.firstCluster))
        return 0;

    if (!fat_delete_located(&loc))
        return 0;

    fat_free_chain(loc.firstCluster);
    fat_flush_fsinfo();
    return 1;
}


/*===========================================================================
    DDStorage directory API
===========================================================================*/
static int usb_ready(DDStorage* s)
{
    (void)s;
    return fat_mount();
}


static void fat_fill_dd(
    const FatEntry* in,
    DDDirEntry* out)
{
    if (!in || !out)
        return;

    fat_zero(out, sizeof(*out));
    fat_scpy(out->name, sizeof(out->name), in->name);
    out->sizeLo = in->size;
    out->isDir = (in->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
}


static int usb_list_begin(
    DDStorage* s,
    const char* path,
    DDDirHandle* h,
    DDDirEntry* first)
{
    DWORD cluster;
    FatDir* d;
    FatEntry e;

    (void)s;

    if (!h || !first)
        return 0;

    h->impl = 0;

    if (!fat_resolve_dir(path, &cluster))
        return 0;

    d = fat_dir_open_cluster(cluster);

    if (!d)
        return 0;

    h->impl = d;

    if (!fat_dir_next_raw(d, &e))
    {
        fat_dir_close(d);
        h->impl = 0;
        return 0;
    }

    fat_fill_dd(&e, first);
    return 1;
}


static int usb_list_next(
    DDStorage* s,
    DDDirHandle* h,
    DDDirEntry* next)
{
    FatDir* d;
    FatEntry e;

    (void)s;

    if (!h || !h->impl || !next)
        return 0;

    d = (FatDir*)h->impl;

    if (!fat_dir_next_raw(d, &e))
        return 0;

    fat_fill_dd(&e, next);
    return 1;
}


static void usb_list_end(
    DDStorage* s,
    DDDirHandle* h)
{
    (void)s;

    if (!h || !h->impl)
        return;

    fat_dir_close((FatDir*)h->impl);
    h->impl = 0;
}


/*===========================================================================
    DDStorage read-only file API
===========================================================================*/
static DDFileHandle usb_open_read(
    DDStorage* s,
    const char* path,
    DWORD* sizeLo)
{
    FatEntry e;
    FatFile* f;

    (void)s;

    if (!fat_resolve_entry(path, &e))
        return 0;

    if (e.attr & FAT_ATTR_DIRECTORY)
        return 0;

    f = (FatFile*)malloc(sizeof(FatFile));

    if (!f)
        return 0;

    fat_zero(f, sizeof(*f));
    f->mode = FAT_FILE_READ;

    f->sector = (unsigned char*)
        malloc(g_fat.deviceSectorSize);

    if (!f->sector)
    {
        free(f);
        return 0;
    }

    f->startCluster = e.firstCluster;
    f->size = e.size;
    f->pos = 0;
    f->curCluster = e.firstCluster;
    f->curClusterIndex = 0;
    f->sectorLoaded = 0;
    f->loadedLba = 0xFFFFFFFFu;

    if (sizeLo)
        *sizeLo = e.size;

    return (DDFileHandle)f;
}


static int fat_file_seek_cluster(
    FatFile* f,
    DWORD wantedIndex)
{
    DWORD c;
    DWORD idx;

    if (!f)
        return 0;

    if (f->size == 0)
        return 1;

    if (f->startCluster < 2)
        return 0;

    if (wantedIndex < f->curClusterIndex)
    {
        f->curCluster = f->startCluster;
        f->curClusterIndex = 0;
    }

    c = f->curCluster;
    idx = f->curClusterIndex;

    while (idx < wantedIndex)
    {
        c = fat_next_cluster(c);

        if (!c)
            return 0;

        ++idx;

        if (idx > g_fat.clusterCount + 1)
            return 0;
    }

    f->curCluster = c;
    f->curClusterIndex = idx;
    return 1;
}


static int usb_read(
    DDStorage* s,
    DDFileHandle h,
    void* dst,
    DWORD bytes,
    DWORD* got)
{
    FatFile* f = (FatFile*)h;
    unsigned char* out = (unsigned char*)dst;
    DWORD total = 0;

    (void)s;

    if (got)
        *got = 0;

    if (!f || !dst || f->mode != FAT_FILE_READ)
        return 0;

    if (f->pos >= f->size || bytes == 0)
        return 1;

    if (bytes > f->size - f->pos)
        bytes = f->size - f->pos;

    while (total < bytes)
    {
        DWORD clusterIndex;
        DWORD inCluster;
        DWORD sectorInCluster;
        DWORD offInSector;
        DWORD lba;
        DWORD chunk;

        clusterIndex =
            f->pos / g_fat.clusterBytes;

        if (!fat_file_seek_cluster(
            f, clusterIndex))
        {
            return 0;
        }

        inCluster =
            f->pos % g_fat.clusterBytes;

        sectorInCluster =
            inCluster / g_fat.deviceSectorSize;

        offInSector =
            inCluster % g_fat.deviceSectorSize;

        lba = fat_cluster_lba(
            f->curCluster);

        if (!lba)
            return 0;

        lba += sectorInCluster;

        /*
            Fast path: hand every complete, aligned sector remaining in this
            cluster to the USB backend at once.  The USB layer converts the run
            into one multi-block READ(10) while retaining 512-byte OHCI URBs.
        */
        if (offInSector == 0 &&
            bytes - total >= g_fat.deviceSectorSize)
        {
            DWORD fullSectors =
                (bytes - total) / g_fat.deviceSectorSize;
            DWORD clusterSectors =
                g_fat.sectorsPerCluster - sectorInCluster;
            DWORD runBytes;

            if (fullSectors > clusterSectors)
                fullSectors = clusterSectors;

            if (fullSectors)
            {
                if (!fat_read_sectors(
                    lba,
                    fullSectors,
                    out + total))
                {
                    return 0;
                }

                runBytes =
                    fullSectors * g_fat.deviceSectorSize;

                total += runBytes;
                f->pos += runBytes;

                /* Any old one-sector cache is no longer authoritative. */
                f->sectorLoaded = 0;
                f->loadedLba = 0xFFFFFFFFu;
                continue;
            }
        }

        if (!f->sectorLoaded ||
            f->loadedLba != lba)
        {
            if (!fat_raw_read_sector(
                lba, f->sector))
            {
                return 0;
            }

            f->loadedLba = lba;
            f->sectorLoaded = 1;
        }

        chunk =
            g_fat.deviceSectorSize - offInSector;

        if (chunk > bytes - total)
            chunk = bytes - total;

        {
            DWORD i;
            for (i = 0; i < chunk; ++i)
                out[total + i] = f->sector[offInSector + i];
        }

        total += chunk;
        f->pos += chunk;
    }

    if (got)
        *got = total;

    return 1;
}


static DDFileHandle usb_open_write(
    DDStorage* s,
    const char* path,
    int overwrite)
{
    DWORD parentCluster;
    char leaf[DD_NAME_MAX];
    FatLocatedEntry existing;
    unsigned char name11[11];
    int needsLfn = 0;
    int lfnCount = 0;
    FatSlot slots[FAT_LFN_MAX_ENTRIES + 1];
    FatFile* f;
    int i;

    (void)s;

    parentCluster = 0;
    fat_zero(&existing, sizeof(existing));
    fat_zero(slots, sizeof(slots));

    if (!fat_mount() ||
        !path ||
        !path[0])
    {
        return 0;
    }

    if (!fat_split_parent(
        path,
        &parentCluster,
        leaf,
        sizeof(leaf)))
    {
        return 0;
    }

    if (!fat_plan_create_entry(
        parentCluster,
        leaf,
        &existing,
        name11,
        &needsLfn,
        &lfnCount,
        slots))
    {
        return 0;
    }

    if (existing.found)
    {
        if (existing.attr & FAT_ATTR_DIRECTORY)
            return 0;

        if (!overwrite)
            return 0;

        for (i = 0; i < 11; ++i)
            name11[i] = existing.shortName[i];
    }

    f = (FatFile*)malloc(sizeof(FatFile));

    if (!f)
        return 0;

    fat_zero(f, sizeof(*f));

    f->mode = FAT_FILE_WRITE;

    f->sector = (unsigned char*)
        malloc(g_fat.deviceSectorSize);

    if (!f->sector)
    {
        free(f);
        return 0;
    }

    if (existing.found)
    {
        f->dirEntryLba = existing.shortSlot.lba;
        f->dirEntryOff = existing.shortSlot.off;
        f->oldStartCluster = existing.firstCluster;
        f->publishLfn = 0;
        f->lfnCount = 0;
    }
    else
    {
        f->dirEntryLba = slots[lfnCount].lba;
        f->dirEntryOff = slots[lfnCount].off;
        f->oldStartCluster = 0;
        f->publishLfn = needsLfn;
        f->lfnCount = lfnCount;

        for (i = 0; i < lfnCount; ++i)
            f->lfnSlots[i] = slots[i];

        if (needsLfn)
            fat_scpy(
                f->longName,
                sizeof(f->longName),
                leaf);
    }

    for (i = 0; i < 11; ++i)
        f->shortName[i] = name11[i];

    f->startCluster = 0;
    f->lastCluster = 0;
    f->curCluster = 0;
    f->size = 0;
    f->pos = 0;
    f->writeFailed = 0;

    return (DDFileHandle)f;
}


static int usb_write(
    DDStorage* s,
    DDFileHandle h,
    const void* src,
    DWORD bytes,
    DWORD* put)
{
    FatFile* f = (FatFile*)h;
    const unsigned char* in =
        (const unsigned char*)src;
    DWORD total = 0;

    (void)s;

    if (put)
        *put = 0;

    if (!f ||
        f->mode != FAT_FILE_WRITE ||
        !src)
    {
        return 0;
    }

    while (total < bytes)
    {
        DWORD inCluster;
        DWORD sectorInCluster;
        DWORD offInSector;
        DWORD lba;
        DWORD chunk;

        inCluster =
            f->pos % g_fat.clusterBytes;

        if (f->curCluster < 2 ||
            (f->pos > 0 && inCluster == 0))
        {
            DWORD newCluster = 0;

            if (!fat_alloc_cluster(
                0,
                &newCluster))
            {
                f->writeFailed = 1;
                return 0;
            }

            if (f->lastCluster >= 2)
            {
                if (!fat_set_fat_value(
                    f->lastCluster,
                    newCluster))
                {
                    fat_set_fat_value(
                        newCluster,
                        0);

                    f->writeFailed = 1;
                    return 0;
                }
            }
            else
            {
                f->startCluster =
                    newCluster;
            }

            f->lastCluster = newCluster;
            f->curCluster = newCluster;
        }

        sectorInCluster =
            inCluster / g_fat.deviceSectorSize;

        offInSector =
            inCluster % g_fat.deviceSectorSize;

        lba = fat_cluster_lba(
            f->curCluster);

        if (!lba)
        {
            f->writeFailed = 1;
            return 0;
        }

        lba += sectorInCluster;

        /*
            Fast path: write every complete sector remaining in this cluster as
            one logical run.  The USB transport still emits 512-byte data URBs,
            so MaxBulkTDperTransfer remains the validated value of 8.
        */
        if (offInSector == 0 &&
            bytes - total >= g_fat.deviceSectorSize)
        {
            DWORD fullSectors =
                (bytes - total) / g_fat.deviceSectorSize;
            DWORD clusterSectors =
                g_fat.sectorsPerCluster - sectorInCluster;
            DWORD runBytes;

            if (fullSectors > clusterSectors)
                fullSectors = clusterSectors;

            if (fullSectors)
            {
                if (!fat_write_sectors(
                    lba,
                    fullSectors,
                    in + total))
                {
                    f->writeFailed = 1;
                    return 0;
                }

                runBytes =
                    fullSectors * g_fat.deviceSectorSize;

                total += runBytes;
                f->pos += runBytes;

                if (f->pos > f->size)
                    f->size = f->pos;

                continue;
            }
        }

        chunk =
            g_fat.deviceSectorSize -
            offInSector;

        if (chunk > bytes - total)
            chunk = bytes - total;

        if (offInSector == 0 &&
            chunk == g_fat.deviceSectorSize)
        {
            if (!fat_raw_write_sector(
                lba,
                in + total))
            {
                f->writeFailed = 1;
                return 0;
            }
        }
        else
        {
            DWORD i;

            /*
                Sequential file writes may revisit a partially filled sector
                on the next Fileops pump. Read/modify/write preserves bytes
                already committed in that sector.
            */
            if (!fat_raw_read_sector(
                lba,
                f->sector))
            {
                f->writeFailed = 1;
                return 0;
            }

            for (i = 0; i < chunk; ++i)
                f->sector[offInSector + i] =
                in[total + i];

            if (!fat_raw_write_sector(
                lba,
                f->sector))
            {
                f->writeFailed = 1;
                return 0;
            }
        }

        total += chunk;
        f->pos += chunk;

        if (f->pos > f->size)
            f->size = f->pos;
    }

    if (put)
        *put = total;

    return 1;
}


static void usb_close(
    DDStorage* s,
    DDFileHandle h)
{
    FatFile* f = (FatFile*)h;

    (void)s;

    if (!f)
        return;

    if (f->mode == FAT_FILE_WRITE)
    {
        int committed = 0;

        if (!f->writeFailed)
        {
            FatSlot shortSlot;

            shortSlot.lba = f->dirEntryLba;
            shortSlot.off = f->dirEntryOff;

            committed = fat_publish_named_entry(
                f->lfnSlots,
                f->publishLfn ? f->lfnCount : 0,
                &shortSlot,
                f->shortName,
                f->publishLfn ? f->longName : 0,
                FAT_ATTR_ARCHIVE,
                f->startCluster,
                f->size);
        }

        if (committed)
        {
            /*
                Directory entry now points at the new chain.  Only now is it
                safe to free an overwritten file's old chain.
            */
            if (f->oldStartCluster >= 2 &&
                f->oldStartCluster != f->startCluster)
            {
                fat_free_chain(
                    f->oldStartCluster);
            }
        }
        else
        {
            /*
                The new file was never published.  Reclaim its orphan chain
                where possible; an existing file entry remains untouched.
            */
            if (f->startCluster >= 2)
                fat_free_chain(
                    f->startCluster);
        }

        /*
            Keep FAT32 FSInfo current once the complete file mutation has
            committed (or its orphan chain has been reclaimed).
        */
        fat_flush_fsinfo();
    }

    if (f->sector)
        free(f->sector);

    free(f);
}


static int usb_exists(
    DDStorage* s,
    const char* path)
{
    FatEntry e;
    DWORD cluster;

    (void)s;

    if (!path)
        return 0;

    if (!fat_slen(path) ||
        (fat_is_sep(path[0]) && !path[1]))
    {
        return fat_resolve_dir(path, &cluster);
    }

    return fat_resolve_entry(path, &e);
}


/*===========================================================================
    Incremental USB FAT32 quick format

    This reformats the EXISTING mounted partition. It does not alter the MBR
    partition table. Geometry is retained from the mounted FAT32 volume.
===========================================================================*/
typedef struct
{
    int active;
    int failed;

    FatVolume v;

    unsigned char* sector;

    DWORD phase;
    DWORD fatIndex;
    DWORD fatSector;
    DWORD rootSector;

    DWORD done;
    DWORD total;
} UsbFormatState;

static UsbFormatState g_fmt;


static void fmt_reset(void)
{
    if (g_fmt.sector)
        free(g_fmt.sector);

    fat_zero(&g_fmt, sizeof(g_fmt));
}


static void fmt_build_boot(unsigned char* b)
{
    DWORD serial;

    fat_zero(b, (int)g_fmt.v.deviceSectorSize);

    /* JMP + OEM */
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;

    b[3] = 'U'; b[4] = 'S'; b[5] = 'B'; b[6] = '2';
    b[7] = 'X'; b[8] = 'B'; b[9] = ' '; b[10] = ' ';

    fat_put_le16(b + 11, (WORD)g_fmt.v.deviceSectorSize);
    b[13] = (unsigned char)g_fmt.v.sectorsPerCluster;
    fat_put_le16(b + 14, (WORD)g_fmt.v.reservedSectors);
    b[16] = (unsigned char)g_fmt.v.fatCount;

    fat_put_le16(b + 17, 0); /* FAT32 root entries */
    fat_put_le16(b + 19, 0); /* total16 */
    b[21] = 0xF8;
    fat_put_le16(b + 22, 0); /* FAT16 size */

    /* conventional geometry metadata; not used for LBA addressing */
    fat_put_le16(b + 24, 63);
    fat_put_le16(b + 26, 255);

    fat_put_le32(b + 28, g_fmt.v.volumeLba);
    fat_put_le32(b + 32, g_fmt.v.totalSectors);
    fat_put_le32(b + 36, g_fmt.v.fatSize);

    fat_put_le16(b + 40, 0); /* ExtFlags */
    fat_put_le16(b + 42, 0); /* FS version */

    fat_put_le32(b + 44, g_fmt.v.rootCluster);

    fat_put_le16(
        b + 48,
        g_fmt.v.reservedSectors > 1 ? 1 : 0xFFFF);

    fat_put_le16(
        b + 50,
        g_fmt.v.reservedSectors > 6 ? 6 : 0);

    b[64] = 0x80;
    b[66] = 0x29;

    serial = GetTickCount();
    fat_put_le32(b + 67, serial);

    {
        static const char label[11] = {
            'U','S','B','2','X','B',' ',' ',' ',' ',' '
        };
        static const char type[8] = {
            'F','A','T','3','2',' ',' ',' '
        };
        int i;

        for (i = 0; i < 11; ++i)
            b[71 + i] = (unsigned char)label[i];

        for (i = 0; i < 8; ++i)
            b[82 + i] = (unsigned char)type[i];
    }

    b[510] = 0x55;
    b[511] = 0xAA;
}


static void fmt_build_fsinfo(unsigned char* b)
{
    DWORD freeCount;

    fat_zero(b, (int)g_fmt.v.deviceSectorSize);

    fat_put_le32(b + 0, 0x41615252u);
    fat_put_le32(b + 484, 0x61417272u);

    freeCount =
        g_fmt.v.clusterCount > 0 ?
        g_fmt.v.clusterCount - 1 : 0;

    fat_put_le32(b + 488, freeCount);
    fat_put_le32(b + 492, 3);

    fat_put_le32(b + 508, 0xAA550000u);
}


static void fmt_build_fat_sector(
    unsigned char* b,
    DWORD sectorIndex)
{
    fat_zero(b, (int)g_fmt.v.deviceSectorSize);

    if (sectorIndex == 0)
    {
        fat_put_le32(b + 0, 0x0FFFFFF8u);
        fat_put_le32(b + 4, 0xFFFFFFFFu);

        /*
            Standard FAT32 root cluster is normally 2.  Retained geometry may
            use another legal root cluster, so place its EOC marker wherever
            its FAT entry falls.
        */
        if ((g_fmt.v.rootCluster * 4) <
            g_fmt.v.deviceSectorSize)
        {
            fat_put_le32(
                b + g_fmt.v.rootCluster * 4,
                0x0FFFFFFFu);
        }
    }
    else
    {
        DWORD fatByte =
            g_fmt.v.rootCluster * 4;

        DWORD rootSector =
            fatByte / g_fmt.v.deviceSectorSize;

        DWORD rootOff =
            fatByte % g_fmt.v.deviceSectorSize;

        if (rootSector == sectorIndex &&
            rootOff + 4 <= g_fmt.v.deviceSectorSize)
        {
            fat_put_le32(
                b + rootOff,
                0x0FFFFFFFu);
        }
    }
}


static int fmt_write(
    DWORD lba,
    const unsigned char* b)
{
    if (!USB2XB_USB_WriteSectors(lba, 1, b))
    {
        g_fmt.failed = 1;
        return 0;
    }

    ++g_fmt.done;
    return 1;
}


int USB2XB_StorageFormatUsbBegin(void)
{
    DWORD metadataWrites;

    if (g_fmt.active)
        return 0;

    if (!fat_mount())
        return 0;

    fmt_reset();

    g_fmt.v = g_fat;

    /* Formatting rewrites metadata/data wholesale; no cached read may survive. */
    fat_cache_discard();

    if (g_fmt.v.deviceSectorSize != 512 ||
        g_fmt.v.fatCount < 1 ||
        g_fmt.v.fatSize < 1 ||
        g_fmt.v.rootCluster < 2)
    {
        fmt_reset();
        return 0;
    }

    g_fmt.sector = (unsigned char*)
        malloc(g_fmt.v.deviceSectorSize);

    if (!g_fmt.sector)
    {
        fmt_reset();
        return 0;
    }

    /*
        Primary boot + primary FSInfo when available + backup boot/FSInfo when
        the standard reserved-sector layout supports them.
    */
    metadataWrites = 1;

    if (g_fmt.v.reservedSectors > 1)
        ++metadataWrites;

    if (g_fmt.v.reservedSectors > 6)
        ++metadataWrites;

    if (g_fmt.v.reservedSectors > 7)
        ++metadataWrites;

    g_fmt.total =
        metadataWrites +
        g_fmt.v.fatCount * g_fmt.v.fatSize +
        g_fmt.v.sectorsPerCluster;

    g_fmt.phase = 0;
    g_fmt.active = 1;
    g_fmt.failed = 0;
    g_fmt.done = 0;
    g_fmt.fatIndex = 0;
    g_fmt.fatSector = 0;
    g_fmt.rootSector = 0;

    return 1;
}


int USB2XB_StorageFormatUsbActive(void)
{
    return g_fmt.active;
}


void USB2XB_StorageFormatUsbProgress(
    DWORD* done,
    DWORD* total)
{
    if (done)*done = g_fmt.done;
    if (total)*total = g_fmt.total;
}


int USB2XB_StorageFormatUsbPump(void)
{
    int budget = 8;

    if (!g_fmt.active)
        return g_fmt.failed ? -1 : 1;

    while (budget-- > 0 && g_fmt.active)
    {
        switch (g_fmt.phase)
        {
        case 0: /* primary boot */
            fmt_build_boot(g_fmt.sector);

            if (!fmt_write(
                g_fmt.v.volumeLba,
                g_fmt.sector))
            {
                goto fail;
            }

            g_fmt.phase = 1;
            break;

        case 1: /* primary FSInfo */
            if (g_fmt.v.reservedSectors > 1)
            {
                fmt_build_fsinfo(g_fmt.sector);

                if (!fmt_write(
                    g_fmt.v.volumeLba + 1,
                    g_fmt.sector))
                {
                    goto fail;
                }
            }

            g_fmt.phase = 2;
            break;

        case 2: /* backup boot */
            if (g_fmt.v.reservedSectors > 6)
            {
                fmt_build_boot(g_fmt.sector);

                if (!fmt_write(
                    g_fmt.v.volumeLba + 6,
                    g_fmt.sector))
                {
                    goto fail;
                }
            }

            g_fmt.phase = 3;
            break;

        case 3: /* backup FSInfo */
            if (g_fmt.v.reservedSectors > 7)
            {
                fmt_build_fsinfo(g_fmt.sector);

                if (!fmt_write(
                    g_fmt.v.volumeLba + 7,
                    g_fmt.sector))
                {
                    goto fail;
                }
            }

            g_fmt.phase = 4;
            break;

        case 4: /* clear + initialize FAT copies */
            if (g_fmt.fatIndex >= g_fmt.v.fatCount)
            {
                g_fmt.phase = 5;
                break;
            }

            fmt_build_fat_sector(
                g_fmt.sector,
                g_fmt.fatSector);

            if (!fmt_write(
                g_fmt.v.fatStartLba +
                g_fmt.fatIndex * g_fmt.v.fatSize +
                g_fmt.fatSector,
                g_fmt.sector))
            {
                goto fail;
            }

            ++g_fmt.fatSector;

            if (g_fmt.fatSector >= g_fmt.v.fatSize)
            {
                g_fmt.fatSector = 0;
                ++g_fmt.fatIndex;
            }
            break;

        case 5: /* zero the root directory cluster */
            if (g_fmt.rootSector >=
                g_fmt.v.sectorsPerCluster)
            {
                g_fmt.phase = 6;
                break;
            }

            fat_zero(
                g_fmt.sector,
                (int)g_fmt.v.deviceSectorSize);

            if (!fmt_write(
                g_fmt.v.dataStartLba +
                (g_fmt.v.rootCluster - 2) *
                g_fmt.v.sectorsPerCluster +
                g_fmt.rootSector,
                g_fmt.sector))
            {
                goto fail;
            }

            ++g_fmt.rootSector;
            break;

        case 6:
            g_fmt.active = 0;

            /*
                Force the FAT32 backend to reread the freshly formatted volume.
            */
            fat_unmount();

            if (!fat_mount())
            {
                g_fmt.failed = 1;
                return -1;
            }

            if (g_fmt.sector)
            {
                free(g_fmt.sector);
                g_fmt.sector = 0;
            }

            return 1;

        default:
            goto fail;
        }
    }

    return 0;

fail:
    g_fmt.active = 0;
    g_fmt.failed = 1;

    if (g_fmt.sector)
    {
        free(g_fmt.sector);
        g_fmt.sector = 0;
    }

    fat_unmount();
    return -1;
}


/*===========================================================================
    Public setup
===========================================================================*/
int USB2XB_StorageUsbSpace(ULONGLONG* freeBytes,
    ULONGLONG* totalBytes,
    int* freeKnown)
{
    ULONGLONG clusterBytes;

    if (freeBytes)
        *freeBytes = 0;
    if (totalBytes)
        *totalBytes = 0;
    if (freeKnown)
        *freeKnown = 0;

    if (!fat_mount() || !g_fat.mounted)
        return 0;

    clusterBytes = (ULONGLONG)g_fat.clusterBytes;

    if (totalBytes)
        *totalBytes = (ULONGLONG)g_fat.clusterCount * clusterBytes;

    /*
        FAT32 FSInfo is advisory. USB2XB keeps a valid free count current after
        its own mutations, but externally-created media may mark it unknown.
        Do not turn a pane refresh into a multi-megabyte FAT scan in that case.
    */
    if (g_fat.freeCountKnown)
    {
        if (freeBytes)
            *freeBytes = (ULONGLONG)g_fat.freeClusterCount * clusterBytes;
        if (freeKnown)
            *freeKnown = 1;
    }

    return 1;
}

void USB2XB_StorageInitUsb(DDStorage* s)
{
    if (!s)
        return;

    fat_zero(s, sizeof(*s));
    g_usbUserUnmounted = 0;
    fat_unmount();

    s->label = "USB FAT32";
    s->ready = usb_ready;

    s->list_begin = usb_list_begin;
    s->list_next = usb_list_next;
    s->list_end = usb_list_end;

    s->open_read = usb_open_read;
    s->open_write = usb_open_write;
    s->read = usb_read;
    s->write = usb_write;
    s->close = usb_close;

    /*
        Folder-op validation: expose the same backend primitives DDStorage
        already provides on FATX. Recursive tree deletion remains centralized
        in dd_fileops so both filesystems behave the same way.
    */
    s->mkdir = usb_mkdir;
    s->rename = usb_rename;
    s->remove_file = usb_remove_file;
    s->remove_dir = usb_remove_dir;

    s->exists = usb_exists;
}


void USB2XB_StorageUsbUnmount(void)
{
    /*
        FileMan only exposes this while idle, so there should be no live FAT
        handles here.  Invalidate all cached BPB/FAT geometry immediately.
    */
    fat_flush_fsinfo();
    g_usbUserUnmounted = 1;
    fat_unmount();
}


void USB2XB_StorageUsbRemount(void)
{
    /*
        Re-enable lazy mounting and force a fresh BPB read.  FileMan may also
        request a low-level rescan, but the storage layer itself never tears
        down or rewrites the validated USB transport state.
    */
    fat_flush_fsinfo();
    g_usbUserUnmounted = 0;
    fat_unmount();
}


int USB2XB_StorageUsbUserUnmounted(void)
{
    return g_usbUserUnmounted ? 1 : 0;
}


int USB2XB_StorageUsbReady(void)
{
    return fat_mount();
}
