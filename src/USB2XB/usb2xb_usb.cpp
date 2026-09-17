/*
    USB2XB USB MASS STORAGE BACKEND
    -------------------------------
    Original Xbox USB Mass Storage transport used by USB2XB.

    Hotplug experiment:
      - keep the proven Camera/X-View manual enumeration path
      - do NOT register a USB mass-storage class driver
      - do NOT splice the manually-owned node into the Xbox hub tree
      - support both Original Xbox USB topologies:
          * v1.0 TI downstream hub
          * later MCPX OHCI root-hub ports
      - poll the active physical port while mounted
      - on physical removal, close the owned endpoints at passive level
      - return the USB address with USBD_FreeUsbAddress(), then recycle the node
      - poll the available port providers while offline and enumerate the next insertion

    Important:
      - cleanup ordering follows the Xbox USBD source: endpoints first,
        address second, device-tree node last
      - if endpoint cleanup fails, the node is deliberately left allocated and
        USB2XB asks for an app restart rather than risking device-tree corruption

    Hardware-validated transport requirements retained:
      - full-speed bulk MPS 64
      - MaxBulkTDperTransfer = 8
      - one 512-byte READ(10)/WRITE(10) data phase at a time

    xbox_usb.h is consumed as-is.
*/

#include <xtl.h>
#include "xbox_usb.h"
#include "usb2xb_usb.h"


/*===========================================================================
    Xbox USB resource provider / manual hotplug ownership

    IMPORTANT:
    The original Xbox USBD class lookup matches only bClass plus interface-vs-
    device level.  It does NOT distinguish subclass/protocol.  Registering a
    second class-08 driver therefore collides with the native Memory Unit
    driver and can lock enumeration on insertion.

    Keep the hardware-proven FF/00/00 X-View-shaped registration only for USB
    resource reservation.  USB2XB continues to enumerate MSC manually.

    The FF/00/00 declaration remains only to reserve the resources required by
    the proven manual path.  The manually-owned MSC node is intentionally kept
    private; physical removal is detected by polling the active physical port.
===========================================================================*/

DECLARE_XPP_TYPE(XViewType)
USB_DEVICE_TYPE_TABLE_BEGIN(XView)
USB_DEVICE_TYPE_TABLE_ENTRY(&XViewType_TABLE)
USB_DEVICE_TYPE_TABLE_END()

USB_CLASS_DRIVER_DECLARATION(XView, 0xFF, 0x00, 0x00)

#pragma data_seg(".XPP$ClassXView")
USB_CLASS_DECLARATION_POINTER(XView)
#pragma data_seg(".XPP$Data")
#pragma comment(linker, "/include:_XViewDescriptionPointer")

extern "C" VOID XViewInit(IUsbInit* UsbInit)
{
    USB_RESOURCE_REQUIREMENTS rr;

    if (UsbInit == 0)
        return;

    rr.ConnectorType = USB_CONNECTOR_TYPE_HIGH_POWER;
    rr.MaxDevices = 1;
    rr.MaxCompositeInterfaces = 1;
    rr.MaxControlEndpoints = 1;
    rr.MaxBulkEndpoints = 2;
    rr.MaxInterruptEndpoints = 0;
    rr.MaxControlTDperTransfer = 0;
    rr.MaxBulkTDperTransfer = 8;
    rr.MaxIsochEndpoints = 0;
    rr.MaxIsochMaxBuffers = 0;

    UsbInit->RegisterResources(&rr);
}

extern "C" VOID XViewAddDevice(IUsbDevice* Device)
{
    if (Device)
        Device->AddComplete(USBD_STATUS_SUCCESS);
}

extern "C" VOID XViewRemoveDevice(IUsbDevice* Device)
{
    if (Device)
        Device->RemoveComplete();
}


#define U2X_NODE_SIZE          0x20
#define U2X_MAX_NODES          32
#define U2X_TREE_BASE_OFF      0xE0
#define U2X_IDX_NONE           0x80
#define U2X_USBD_PENDING       0x40000000u

#define U2X_TI_HUB_VID         0x0451
#define U2X_TI_HUB_PID         0x2046

#define U2X_PORT_RESET         4
#define U2X_PORT_POWER         8
#define U2X_C_PORT_CONNECTION  16
#define U2X_C_PORT_RESET       20
#define U2X_PORT_STAT_POWER    0x0100

/* Xbox USBD / OHCI topology facts used only by this translation unit. */
#define U2X_UDN_ROOT_HUB       0x00
#define U2X_UDN_HUB            0x01
#define U2X_MAX_ROOT_HUBS      4
#define U2X_USBD_HCD_OFF       0x18
#define U2X_OHCI_RHDA_OFF      0x48
#define U2X_OHCI_RHPS_OFF      0x54
#define U2X_OHCI_RH_CCS        0x00000001u
#define U2X_OHCI_RH_PES        0x00000002u
#define U2X_OHCI_RH_PRS        0x00000010u
#define U2X_OHCI_RH_PRSC       0x00100000u

/* SubmitRequest routes 0x82 to OpenDefaultEndpoint on the owned node. */
#define U2X_URB_OPEN_DEFAULT_EP  0x82
#define U2X_URB_CLOSE_DEFAULT_EP 0xC3

#define U2X_CBW_SIGNATURE 0x43425355u
#define U2X_CSW_SIGNATURE 0x53425355u
#define U2X_SCSI_TEST_UNIT_READY 0x00
#define U2X_SCSI_INQUIRY 0x12
#define U2X_SCSI_READ_CAPACITY10 0x25
#define U2X_SCSI_READ10 0x28
#define U2X_SCSI_WRITE10 0x2A
#define U2X_MSC_BULK_ONLY_RESET 0xFF
#define U2X_CMD_DMA_BYTES 64
#define U2X_SECTOR_DMA_BYTES 512
#define U2X_BOT_MAX_BLOCKS 32
#define U2X_ENDPOINT_STATE_DATA_TOGGLE_RESET 0x04

#pragma pack(push,1)
typedef struct
{
    DWORD dCBWSignature;
    DWORD dCBWTag;
    DWORD dCBWDataTransferLength;
    UCHAR bmCBWFlags;
    UCHAR bCBWLUN;
    UCHAR bCBWCBLength;
    UCHAR CBWCB[16];
} U2X_CBW;

typedef struct
{
    DWORD dCSWSignature;
    DWORD dCSWTag;
    DWORD dCSWDataResidue;
    UCHAR bCSWStatus;
} U2X_CSW;
#pragma pack(pop)

class CDeviceTree
{
public:
    char _opaque[0x200];
    IUsbDevice* AllocDevice();
    void FreeDevice(IUsbDevice* Device);
};

extern CDeviceTree g_DeviceTree;
extern "C" BOOLEAN __stdcall MmIsAddressValid(PVOID VirtualAddress);
extern "C" PVOID __stdcall MmAllocateContiguousMemory(ULONG NumberOfBytes);
extern "C" VOID __stdcall MmFreeContiguousMemory(PVOID BaseAddress);

/*
    Minimal view of the Xbox USBD host-controller header.  The HCD-private
    extension begins immediately after AddressList (offset 0x18 on Xbox).
*/
struct _USBD_HOST_CONTROLLER
{
    ULONG ControllerNumber;
    IUsbDevice* RootHub;
    ULONG AddressList[4];
};

/* Proven Camera/X-View manual-address allocator. */
unsigned char __fastcall USBD_AllocateUsbAddress(
    struct _USBD_HOST_CONTROLLER* hc);
void __fastcall USBD_FreeUsbAddress(
    struct _USBD_HOST_CONTROLLER* hc,
    unsigned char address);

static U2XUsbState s_state = U2X_USB_OFFLINE;
static char s_status[96] = "USB IDLE";
static char s_product[2] = "";

static IUsbDevice* s_hubDev = 0;
static IUsbDevice* s_ownedDev = 0;
static int s_hubPort = -1;
static int s_hubNports = 0;
static int s_hubIsRoot = 0;
static IUsbDevice* s_rootHubs[U2X_MAX_ROOT_HUBS];
static int s_rootHubCount = 0;
static unsigned char s_portEnabled[9];
static unsigned char s_portCandidate[9];

static int s_scanRequested = 0;
static int s_manualAttemptUsed = 0;
static int s_transportUsbdOwned = 0;
static DWORD s_notBeforeTick = 0;

/* Manual-node hotplug lifecycle. */
static LONG s_linkLost = 0;
static DWORD s_nextHotplugPoll = 0;
static int s_hotplugPolling = 0;

static UCHAR s_cfgValue = 0;
static UCHAR s_mscIfNum = 0;
static UCHAR s_mscAlt = 0;
static UCHAR s_epOutAddr = 0;
static UCHAR s_epInAddr = 0;
static USHORT s_epOutMps = 0;
static USHORT s_epInMps = 0;

static void* s_epOutHandle = 0;
static void* s_epInHandle = 0;
static ULONG s_toggleOut = 0;
static ULONG s_toggleIn = 0;

static unsigned char* s_cmdDma = 0;
static unsigned char* s_sectorDma = 0;
static DWORD s_cbwTag = 0x55425831u;
static ULONG s_lastSubmitRaw = 0;
static ULONG s_lastHdrStatus = 0;

/*
    Persistent media/volume facts exposed through usb2xb_usb.h.
    The transport currently supports 512-byte logical sectors, matching the
    hardware-validated stick and the 8-TD bulk quota.
*/
static DWORD s_sectorSize = 0;
static DWORD s_sectorCount = 0;
static DWORD s_volumeLba = 0;
static int s_fat32Detected = 0;

static UCHAR s_sectorsPerCluster = 0;
static USHORT s_reservedSectors = 0;
static UCHAR s_fatCount = 0;
static DWORD s_sectorsPerFat = 0;
static DWORD s_rootCluster = 0;


/* ------------------------------------------------------------------------- */

static void U2x_Zero(void* p, int n)
{
    int i;
    unsigned char* b;

    b = (unsigned char*)p;

    for (i = 0; i < n; ++i)
        b[i] = 0;
}


static void U2x_CopyText(char* dst, int cap, const char* src)
{
    int i;

    if (!dst || cap < 1)
        return;

    i = 0;

    if (src)
    {
        while (src[i] && i < cap - 1)
        {
            dst[i] = src[i];
            ++i;
        }
    }

    dst[i] = 0;
}


static void U2x_SetStatus(const char* text)
{
    U2x_CopyText(s_status, sizeof(s_status), text);
}


static void U2x_SetCandidateStatus(int port)
{
    char msg[32];
    const char* base = "USB: CANDIDATE P";
    int i;

    i = 0;

    while (base[i] && i < (int)sizeof(msg) - 2)
    {
        msg[i] = base[i];
        ++i;
    }

    if (port >= 0 && port <= 9 && i < (int)sizeof(msg) - 1)
        msg[i++] = (char)('0' + port);

    msg[i] = 0;
    U2x_SetStatus(msg);
}


static void U2x_SetPortReadFailStatus(int port)
{
    char msg[32];
    const char* base = "USB: PORT READ FAIL P";
    int i;

    i = 0;

    while (base[i] && i < (int)sizeof(msg) - 2)
    {
        msg[i] = base[i];
        ++i;
    }

    if (port >= 0 && port <= 9 && i < (int)sizeof(msg) - 1)
        msg[i++] = (char)('0' + port);

    msg[i] = 0;
    U2x_SetStatus(msg);
}


/* ------------------------------------------------------------------------- */

static int U2x_Readable(const void* p, int len)
{
    if (!p || len < 1)
        return 0;

    if (!MmIsAddressValid((PVOID)p))
        return 0;

    if (!MmIsAddressValid(
        (PVOID)((const unsigned char*)p + len - 1)))
        return 0;

    return 1;
}


static ULONG U2x_SubmitPoll(IUsbDevice* dev, PURB urb)
{
    LONG st;
    int spins;

    if (!dev || !urb)
        return 0x7FFFFFFFu;

    s_lastSubmitRaw = 0;
    s_lastHdrStatus = U2X_USBD_PENDING;

    urb->Header.Status = (USBD_STATUS)U2X_USBD_PENDING;
    urb->Header.CompleteProc = 0;
    urb->Header.CompleteContext = 0;

    st = dev->SubmitRequest(urb);

    s_lastSubmitRaw = (ULONG)st;
    s_lastHdrStatus = (ULONG)(*(volatile ULONG*)&urb->Header.Status);

    if (s_lastHdrStatus != U2X_USBD_PENDING)
        return s_lastHdrStatus;

    if ((ULONG)st != U2X_USBD_PENDING)
        return (ULONG)st;

    for (spins = 0; spins < 2000; ++spins)
    {
        s_lastHdrStatus = (ULONG)(*(volatile ULONG*)&urb->Header.Status);

        if (s_lastHdrStatus != U2X_USBD_PENDING)
            break;

        Sleep(1);
    }

    s_lastHdrStatus = (ULONG)(*(volatile ULONG*)&urb->Header.Status);

    if (s_lastHdrStatus == U2X_USBD_PENDING)
        return 0x7FFFFFFFu;

    return s_lastHdrStatus;
}

static ULONG U2x_Control(
    IUsbDevice* dev,
    UCHAR bmReqType,
    UCHAR bReq,
    USHORT wValue,
    USHORT wIndex,
    void* buf,
    USHORT len,
    UCHAR dir)
{
    URB_CONTROL_TRANSFER urb;

    if (!dev)
        return (ULONG)USBD_STATUS_NO_DEVICE;

    U2x_Zero(&urb, sizeof(urb));

    urb.Hdr.Length = (UCHAR)sizeof(URB_CONTROL_TRANSFER);
    urb.Hdr.Function = URB_FUNCTION_CONTROL_TRANSFER;
    urb.EndpointHandle = 0;
    urb.TransferBufferLength = len;
    urb.TransferBuffer = buf;
    urb.TransferDirection = dir;
    urb.ShortTransferOK = 1;
    urb.InterruptDelay = USBD_DELAY_INTERRUPT_0_MS;

    urb.SetupPacket.bmRequestType = bmReqType;
    urb.SetupPacket.bRequest = bReq;
    urb.SetupPacket.wValue = wValue;
    urb.SetupPacket.wIndex = wIndex;
    urb.SetupPacket.wLength = len;

    return U2x_SubmitPoll(dev, (PURB)&urb);
}


static int U2x_GetVidPid(
    IUsbDevice* dev,
    int* vid,
    int* pid)
{
    unsigned char buf[18];
    ULONG st;
    int i;

    if (!dev || !vid || !pid)
        return 0;

    for (i = 0; i < 18; ++i)
        buf[i] = 0;

    st = U2x_Control(
        dev,
        0x80,
        USB_REQUEST_GET_DESCRIPTOR,
        0x0100,
        0,
        buf,
        18,
        USB_TRANSFER_DIRECTION_IN);

    if (st != 0)
        return 0;

    *vid = (int)buf[8] | ((int)buf[9] << 8);
    *pid = (int)buf[10] | ((int)buf[11] << 8);

    return 1;
}


/* ------------------------------------------------------------------------- */

static char* U2x_GetNodeBase(void)
{
    char* tree;

    tree = (char*)(void*)&g_DeviceTree;

    if (!U2x_Readable(tree + U2X_TREE_BASE_OFF, 4))
        return 0;

    return *(char**)(tree + U2X_TREE_BASE_OFF);
}


static IUsbDevice* U2x_NodeAt(char* base, int idx)
{
    char* node;

    if (!base || idx < 0 || idx >= U2X_MAX_NODES)
        return 0;

    node = base + idx * U2X_NODE_SIZE;

    if (!U2x_Readable(node, U2X_NODE_SIZE))
        return 0;

    return (IUsbDevice*)(void*)node;
}


static int U2x_NodeIndex(char* base, IUsbDevice* dev)
{
    char* p;
    int off;

    if (!base || !dev)
        return -1;

    p = (char*)(void*)dev;

    if (p < base)
        return -1;

    off = (int)(p - base);

    if (off % U2X_NODE_SIZE)
        return -1;

    off /= U2X_NODE_SIZE;

    if (off < 0 || off >= U2X_MAX_NODES)
        return -1;

    return off;
}


static void U2x_InspectNode(IUsbDevice* dev)
{
    unsigned char* b;
    unsigned char parent;
    int vid;
    int pid;
    int i;

    if (!dev)
        return;

    b = (unsigned char*)(void*)dev;
    parent = b[1];

    if (b[0] == U2X_UDN_ROOT_HUB && parent == U2X_IDX_NONE)
    {
        for (i = 0; i < s_rootHubCount; ++i)
        {
            if (s_rootHubs[i] == dev)
                return;
        }

        if (s_rootHubCount < U2X_MAX_ROOT_HUBS)
            s_rootHubs[s_rootHubCount++] = dev;

        return;
    }

    if (s_hubDev)
        return;

    if (parent == U2X_IDX_NONE || b[0] != U2X_UDN_HUB)
        return;

    vid = -1;
    pid = -1;

    if (!U2x_GetVidPid(dev, &vid, &pid))
        return;

    if (vid == U2X_TI_HUB_VID && pid == U2X_TI_HUB_PID)
    {
        s_hubDev = dev;
        s_hubIsRoot = 0;
    }
}


static void U2x_WalkTree(void)
{
    char* base;
    IUsbDevice* stack[U2X_MAX_NODES];
    unsigned char visited[U2X_MAX_NODES];
    int sp;
    int count;
    int i;

    base = U2x_GetNodeBase();

    if (!base)
        return;

    for (i = 0; i < U2X_MAX_NODES; ++i)
        visited[i] = 0;

    sp = 0;

    for (i = 0; i < U2X_MAX_NODES; ++i)
    {
        IUsbDevice* node;
        unsigned char* b;

        node = U2x_NodeAt(base, i);

        if (!node)
            continue;

        b = (unsigned char*)(void*)node;

        if (b[0] == U2X_UDN_ROOT_HUB && b[1] == U2X_IDX_NONE)
        {
            if (sp < U2X_MAX_NODES)
                stack[sp++] = node;
        }
    }

    {
        IUsbDevice* node0;

        node0 = U2x_NodeAt(base, 0);

        if (node0 && sp < U2X_MAX_NODES)
            stack[sp++] = node0;
    }

    count = 0;

    while (sp > 0 && count < U2X_MAX_NODES)
    {
        IUsbDevice* dev;
        unsigned char* b;
        int idx;
        int childIdx;
        int guard;

        dev = stack[--sp];
        idx = U2x_NodeIndex(base, dev);

        if (idx < 0 || visited[idx])
            continue;

        visited[idx] = 1;
        ++count;

        U2x_InspectNode(dev);

        if (s_hubDev)
            return;

        b = (unsigned char*)(void*)dev;
        childIdx = (int)b[2];
        guard = 0;

        while (childIdx != U2X_IDX_NONE &&
            guard < U2X_MAX_NODES)
        {
            IUsbDevice* child;
            unsigned char* cb;
            int cidx;

            child = U2x_NodeAt(base, childIdx);

            if (!child)
                break;

            cb = (unsigned char*)(void*)child;
            cidx = U2x_NodeIndex(base, child);

            if (cidx >= 0 &&
                !visited[cidx] &&
                sp < U2X_MAX_NODES)
            {
                stack[sp++] = child;
            }

            childIdx = (int)cb[3];
            ++guard;
        }
    }
}


/* ------------------------------------------------------------------------- */

static struct _USBD_HOST_CONTROLLER* U2x_GetHostController(IUsbDevice* dev)
{
    unsigned char* b;

    if (!dev || !U2x_Readable(dev, U2X_NODE_SIZE))
        return 0;

    b = (unsigned char*)(void*)dev;
    return *(struct _USBD_HOST_CONTROLLER**)(b + 0x0C);
}

static void* U2x_GetHcdExtension(IUsbDevice* dev)
{
    struct _USBD_HOST_CONTROLLER* hc;

    hc = U2x_GetHostController(dev);
    if (!hc || !U2x_Readable(hc, U2X_USBD_HCD_OFF))
        return 0;

    return (void*)((unsigned char*)(void*)hc + U2X_USBD_HCD_OFF);
}

static volatile unsigned char* U2x_GetRootOperationalRegisters(IUsbDevice* root)
{
    void* hcd;

    hcd = U2x_GetHcdExtension(root);
    if (!hcd || !U2x_Readable(hcd, sizeof(void*)))
        return 0;

    return *(volatile unsigned char**)(void*)hcd;
}

static int U2x_ReadRootPortStatus(IUsbDevice* root, int port, ULONG* status)
{
    volatile unsigned char* regs;
    ULONG nports;

    if (!root || !status || port < 1)
        return 0;

    regs = U2x_GetRootOperationalRegisters(root);
    if (!regs)
        return 0;

    nports = (*(volatile ULONG*)(regs + U2X_OHCI_RHDA_OFF)) & 0xFFu;
    if (nports < 1 || nports > 8 || (ULONG)port > nports)
        return 0;

    *status = *(volatile ULONG*)(regs + U2X_OHCI_RHPS_OFF + ((port - 1) * 4));
    return 1;
}

static int U2x_ResetRootPort(IUsbDevice* root, int port)
{
    volatile unsigned char* regs;
    volatile ULONG* portReg;
    ULONG rhda;
    ULONG status;
    ULONG nports;
    int tries;

    if (!root || port < 1)
        return 0;

    regs = U2x_GetRootOperationalRegisters(root);
    if (!regs)
        return 0;

    rhda = *(volatile ULONG*)(regs + U2X_OHCI_RHDA_OFF);
    nports = rhda & 0xFFu;
    if (nports < 1 || nports > 8 || (ULONG)port > nports)
        return 0;

    portReg = (volatile ULONG*)(
        regs + U2X_OHCI_RHPS_OFF + ((port - 1) * 4));

    status = *portReg;
    if (!(status & U2X_OHCI_RH_CCS))
        return 0;

    /*
        The Xbox HCD root hub is not a transfer-capable USB hub.  Do the
        same OHCI root-port operation locally rather than depending on the
        non-exported HCD_ResetRootHubPort() helper from RXDK/libxapi.

        OHCI root-hub port-status registers use write-one commands:
          bit 4  -> SetPortReset
          bit 20 -> clear PortResetStatusChange

        Poll live CCS/PRS/PES state rather than the HCD callback.  This keeps
        USB2XB independent of the private HCD reset-completion bookkeeping.
    */
    *portReg = U2X_OHCI_RH_PRS;

    for (tries = 0; tries < 40; ++tries)
    {
        Sleep(5);
        status = *portReg;

        if (!(status & U2X_OHCI_RH_CCS))
            return 0;

        if (!(status & U2X_OHCI_RH_PRS) &&
            (status & U2X_OHCI_RH_PES))
        {
            /* Acknowledge the reset-complete change latch if still set. */
            if (status & U2X_OHCI_RH_PRSC)
                *portReg = U2X_OHCI_RH_PRSC;

            /* USB reset recovery interval before address-zero traffic. */
            Sleep(10);

            status = *portReg;
            return ((status & U2X_OHCI_RH_CCS) &&
                (status & U2X_OHCI_RH_PES) &&
                !(status & U2X_OHCI_RH_PRS)) ? 1 : 0;
        }
    }

    /* Best-effort latch cleanup on timeout; the live state remains decisive. */
    status = *portReg;
    if (status & U2X_OHCI_RH_PRSC)
        *portReg = U2X_OHCI_RH_PRSC;

    return 0;
}

static int U2x_ReadHubDescriptor(IUsbDevice* hub)
{
    unsigned char hd[16];
    ULONG st;
    int i;
    int nports;

    if (!hub)
        return 0;

    if (s_hubIsRoot)
    {
        volatile unsigned char* regs;
        ULONG rhda;

        regs = U2x_GetRootOperationalRegisters(hub);
        if (!regs)
            return 0;

        rhda = *(volatile ULONG*)(regs + U2X_OHCI_RHDA_OFF);
        nports = (int)(rhda & 0xFFu);
        if (nports < 1 || nports > 8)
            return 0;

        s_hubNports = nports;
        s_hubPort = -1;
        for (i = 0; i < 9; ++i)
        {
            s_portEnabled[i] = 0;
            s_portCandidate[i] = 0;
        }
        return 1;
    }

    for (i = 0; i < 16; ++i)
        hd[i] = 0;

    st = U2x_Control(
        hub,
        0xA0,
        USB_REQUEST_GET_DESCRIPTOR,
        0x2900,
        0,
        hd,
        9,
        USB_TRANSFER_DIRECTION_IN);

    if (st != 0)
        return 0;

    nports = (int)hd[2];

    if (nports < 1 || nports>8)
        return 0;

    s_hubNports = nports;
    s_hubPort = -1;

    for (i = 0; i < 9; ++i)
    {
        s_portEnabled[i] = 0;
        s_portCandidate[i] = 0;
    }

    return 1;
}


static int U2x_ReadHubPort(IUsbDevice* hub, int port)
{
    unsigned char ps[4];
    ULONG st;
    int i;
    int status;
    int change;

    if (!hub || port<1 || port>s_hubNports)
        return 0;

    if (s_hubIsRoot)
    {
        ULONG rootStatus;

        if (!U2x_ReadRootPortStatus(hub, port, &rootStatus))
            return 0;

        if (!(rootStatus & U2X_OHCI_RH_CCS) || (rootStatus & U2X_OHCI_RH_PRS))
            return 1;

        if (rootStatus & U2X_OHCI_RH_PES)
        {
            s_portEnabled[port] = 1;
            return 1;
        }

        /* Avoid racing the native Xbox settle/reset/enumeration window. */
        Sleep(150);
        if (!U2x_ReadRootPortStatus(hub, port, &rootStatus))
            return 0;

        if ((rootStatus & U2X_OHCI_RH_CCS) &&
            !(rootStatus & U2X_OHCI_RH_PES) &&
            !(rootStatus & U2X_OHCI_RH_PRS))
        {
            s_portCandidate[port] = 1;
            if (s_hubPort < 1)
                s_hubPort = port;
        }
        return 1;
    }

    for (i = 0; i < 4; ++i)
        ps[i] = 0;

    st = U2x_Control(
        hub,
        0xA3,
        USB_REQUEST_GET_STATUS,
        0,
        (USHORT)port,
        ps,
        4,
        USB_TRANSFER_DIRECTION_IN);

    if (st != 0)
        return 0;

    status = (int)ps[0] | ((int)ps[1] << 8);
    change = (int)ps[2] | ((int)ps[3] << 8);

    /*
        Camera/X-View hotplug rule:
        acknowledge C_PORT_CONNECTION, settle, then read the live status again.
    */
    if (change & 0x0001)
    {
        st = U2x_Control(
            hub,
            0x23,
            USB_REQUEST_CLEAR_FEATURE,
            U2X_C_PORT_CONNECTION,
            (USHORT)port,
            0,
            0,
            USB_TRANSFER_DIRECTION_OUT);

        if (st != 0)
            return 0;

        Sleep(20);

        for (i = 0; i < 4; ++i)
            ps[i] = 0;

        st = U2x_Control(
            hub,
            0xA3,
            USB_REQUEST_GET_STATUS,
            0,
            (USHORT)port,
            ps,
            4,
            USB_TRANSFER_DIRECTION_IN);

        if (st != 0)
            return 0;

        status = (int)ps[0] | ((int)ps[1] << 8);
    }

    /*
        Camera/X-View port-power rule.
    */
    if (!(status & U2X_PORT_STAT_POWER))
    {
        st = U2x_Control(
            hub,
            0x23,
            USB_REQUEST_SET_FEATURE,
            U2X_PORT_POWER,
            (USHORT)port,
            0,
            0,
            USB_TRANSFER_DIRECTION_OUT);

        if (st != 0)
            return 0;

        Sleep(40);

        for (i = 0; i < 4; ++i)
            ps[i] = 0;

        st = U2x_Control(
            hub,
            0xA3,
            USB_REQUEST_GET_STATUS,
            0,
            (USHORT)port,
            ps,
            4,
            USB_TRANSFER_DIRECTION_IN);

        if (st != 0)
            return 0;

        status = (int)ps[0] | ((int)ps[1] << 8);
    }

    /*
        bit0 = connected
        bit1 = enabled

        Step 1 target is connected + NOT enabled.
    */
    if (status & 0x0001)
    {
        if (status & 0x0002)
        {
            s_portEnabled[port] = 1;
        }
        else
        {
            s_portCandidate[port] = 1;

            if (s_hubPort < 1)
                s_hubPort = port;
        }
    }

    return 1;
}



/* -------------------------------------------------------------------------
    STEP 2 primitives -- copied from the camera/X-View manual path.
------------------------------------------------------------------------- */

static int U2x_ResetPort(IUsbDevice* hub, int port)
{
    unsigned char ps[4];
    ULONG st;
    int tries;
    int status;

    if (!hub || port < 1)
        return 0;

    if (s_hubIsRoot)
        return U2x_ResetRootPort(hub, port);

    st = U2x_Control(
        hub,
        0x23,
        USB_REQUEST_SET_FEATURE,
        U2X_PORT_RESET,
        (USHORT)port,
        0,
        0,
        USB_TRANSFER_DIRECTION_OUT);

    if (st != 0)
        return 0;

    status = 0;

    for (tries = 0; tries < 20; ++tries)
    {
        Sleep(15);
        U2x_Zero(ps, sizeof(ps));

        st = U2x_Control(
            hub,
            0xA3,
            USB_REQUEST_GET_STATUS,
            0,
            (USHORT)port,
            ps,
            4,
            USB_TRANSFER_DIRECTION_IN);

        if (st != 0)
            continue;

        status = (int)ps[0] | ((int)ps[1] << 8);

        if (status & 0x0002)
            break;
    }

    /*
        X-View clears the reset-change latch after the enable poll.
        Failure to clear the latch is not used as the enable result; the live
        enabled bit above is the authoritative result.
    */
    U2x_Control(
        hub,
        0x23,
        USB_REQUEST_CLEAR_FEATURE,
        U2X_C_PORT_RESET,
        (USHORT)port,
        0,
        0,
        USB_TRANSFER_DIRECTION_OUT);

    return (status & 0x0002) ? 1 : 0;
}


static IUsbDevice* U2x_BuildOwnedNode(int port)
{
    IUsbDevice* node;
    unsigned char* nb;

    if (port < 1)
        return 0;

    node = g_DeviceTree.AllocDevice();

    if (!node)
        return 0;

    if (!U2x_Readable(node, U2X_NODE_SIZE))
        return 0;

    nb = (unsigned char*)(void*)node;

    /*
        Camera/X-View manual node seed:
          [0] = 0xFE  manual/owned node marker
          [4] = downstream hub port
          [5] = USB address 0
          [6] = EP0 max packet 8 until the first device descriptor read
    */
    nb[0] = 0xFE;
    nb[4] = (unsigned char)port;
    nb[5] = 0;
    nb[6] = 8;
    nb[7] = 0xFF;

    /*
        AllocDevice() deliberately does not initialize m_HostController.
        The native DeviceConnected path copies it from the parent hub before
        enumeration.  Do the same for our manual node instead of relying on
        stale storage in a recycled node slot.
    */
    if (s_hubDev && U2x_Readable(s_hubDev, U2X_NODE_SIZE))
    {
        unsigned char* hb = (unsigned char*)(void*)s_hubDev;
        *(void**)(nb + 0x0C) = *(void**)(hb + 0x0C);
    }

    return node;
}


static ULONG U2x_OpenDefaultEP(IUsbDevice* node)
{
    URB u;

    if (!node)
        return (ULONG)USBD_STATUS_NO_DEVICE;

    U2x_Zero(&u, sizeof(u));

    u.Header.Length = (UCHAR)sizeof(URB);
    u.Header.Function = U2X_URB_OPEN_DEFAULT_EP;

    return U2x_SubmitPoll(node, (PURB)&u);
}


static ULONG U2x_CloseDefaultEP(IUsbDevice* node)
{
    URB u;

    if (!node)
        return (ULONG)USBD_STATUS_NO_DEVICE;

    U2x_Zero(&u, sizeof(u));

    u.Header.Length = (UCHAR)sizeof(URB);
    u.Header.Function = U2X_URB_CLOSE_DEFAULT_EP;

    return U2x_SubmitPoll(node, (PURB)&u);
}



/* -------------------------------------------------------------------------
    STEP 3: first DEVICE descriptor read only.

    This is NOT the configuration descriptor and therefore does not interact
    with the Xbox 80-byte configuration-descriptor ceiling/padding behavior.
------------------------------------------------------------------------- */

static char U2x_HexNibble(UCHAR v)
{
    v &= 0x0F;
    return (v < 10) ? (char)('0' + v) : (char)('A' + (v - 10));
}


static void U2x_SetDesc8Status(UCHAR mps0)
{
    char msg[32];
    const char* base = "USB: DESC8 OK MPS ";
    int i;

    i = 0;

    while (base[i] && i < (int)sizeof(msg) - 3)
    {
        msg[i] = base[i];
        ++i;
    }

    msg[i++] = U2x_HexNibble((UCHAR)(mps0 >> 4));
    msg[i++] = U2x_HexNibble(mps0);
    msg[i] = 0;

    U2x_SetStatus(msg);
}



static void U2x_SetAddressStatus(UCHAR addr)
{
    char msg[32];
    const char* base = "USB: ADDR OK ";
    int i;

    i = 0;

    while (base[i] && i < (int)sizeof(msg) - 3)
    {
        msg[i] = base[i];
        ++i;
    }

    msg[i++] = U2x_HexNibble((UCHAR)(addr >> 4));
    msg[i++] = U2x_HexNibble(addr);
    msg[i] = 0;

    U2x_SetStatus(msg);
}



static void U2x_SetCfg9Status(USHORT total, UCHAR cfgValue)
{
    char msg[40];
    const char* base = "USB: CFG9 OK T";
    int i;

    i = 0;

    while (base[i] && i < (int)sizeof(msg) - 9)
    {
        msg[i] = base[i];
        ++i;
    }

    msg[i++] = U2x_HexNibble((UCHAR)(total >> 12));
    msg[i++] = U2x_HexNibble((UCHAR)(total >> 8));
    msg[i++] = U2x_HexNibble((UCHAR)(total >> 4));
    msg[i++] = U2x_HexNibble((UCHAR)total);

    msg[i++] = ' ';
    msg[i++] = 'C';

    msg[i++] = U2x_HexNibble((UCHAR)(cfgValue >> 4));
    msg[i++] = U2x_HexNibble(cfgValue);

    msg[i] = 0;

    U2x_SetStatus(msg);
}


static int U2x_ReadConfigHeader9(
    IUsbDevice* node,
    USHORT* outTotal,
    UCHAR* outConfigValue)
{
    unsigned char d[9];
    ULONG st;
    int i;
    USHORT total;

    if (!node || !outTotal || !outConfigValue)
        return 0;

    for (i = 0; i < 9; ++i)
        d[i] = 0;

    /*
        Padding-safe probe:
          GET_DESCRIPTOR(CONFIGURATION,0), length = 9 only.

        We intentionally do NOT use wTotalLength as the request length here.
        That is the exact boundary we are validating before Step 6.
    */
    st = U2x_Control(
        node,
        0x80,
        USB_REQUEST_GET_DESCRIPTOR,
        0x0200,
        0,
        d,
        9,
        USB_TRANSFER_DIRECTION_IN);

    if (st != 0)
        return 0;

    if (d[0] != 9 ||
        d[1] != USB_CONFIGURATION_DESCRIPTOR_TYPE)
        return 0;

    total = (USHORT)((USHORT)d[2] | ((USHORT)d[3] << 8));

    if (total < 9)
        return 0;

    if (d[5] == 0)
        return 0;

    *outTotal = total;
    *outConfigValue = d[5];

    return 1;
}



static void U2x_SetCfg32Status(
    UCHAR outEp,
    UCHAR inEp,
    USHORT outMps,
    USHORT inMps)
{
    char msg[32];
    int i;

    /*
        Keep this intentionally compact because the UI status field truncates
        longer diagnostics.

        Example:
          MSC O01 I81 40/40
    */
    i = 0;

    msg[i++] = 'M';
    msg[i++] = 'S';
    msg[i++] = 'C';
    msg[i++] = ' ';

    msg[i++] = 'O';
    msg[i++] = U2x_HexNibble((UCHAR)(outEp >> 4));
    msg[i++] = U2x_HexNibble(outEp);

    msg[i++] = ' ';
    msg[i++] = 'I';
    msg[i++] = U2x_HexNibble((UCHAR)(inEp >> 4));
    msg[i++] = U2x_HexNibble(inEp);

    msg[i++] = ' ';

    /*
        Full-speed bulk MPS is <=64 here, so two hex digits are sufficient
        and keep the complete result visible.
    */
    msg[i++] = U2x_HexNibble((UCHAR)(outMps >> 4));
    msg[i++] = U2x_HexNibble((UCHAR)outMps);

    msg[i++] = '/';

    msg[i++] = U2x_HexNibble((UCHAR)(inMps >> 4));
    msg[i++] = U2x_HexNibble((UCHAR)inMps);

    msg[i] = 0;

    U2x_SetStatus(msg);
}

static int U2x_ReadAndParseConfig32(IUsbDevice* node)
{
    unsigned char d[32];
    ULONG st;
    int i;
    int off;
    int curMsc;
    int foundIn;
    int foundOut;

    if (!node)
        return 0;

    for (i = 0; i < 32; ++i)
        d[i] = 0;

    /*
        Step 5 proved wTotalLength == 0x0020 on this hardware.
        Request exactly those 32 bytes; never exceed the reported total.
    */
    st = U2x_Control(
        node,
        0x80,
        USB_REQUEST_GET_DESCRIPTOR,
        0x0200,
        0,
        d,
        32,
        USB_TRANSFER_DIRECTION_IN);

    if (st != 0)
        return 0;

    if (d[0] != 9 ||
        d[1] != USB_CONFIGURATION_DESCRIPTOR_TYPE)
        return 0;

    if (((USHORT)d[2] | ((USHORT)d[3] << 8)) != 0x0020)
        return 0;

    s_cfgValue = d[5];
    s_mscIfNum = 0;
    s_mscAlt = 0;
    s_epOutAddr = 0;
    s_epInAddr = 0;
    s_epOutMps = 0;
    s_epInMps = 0;

    curMsc = 0;
    foundIn = 0;
    foundOut = 0;
    off = 0;

    while (off + 2 <= 32)
    {
        int len;
        int type;

        len = (int)d[off];
        type = (int)d[off + 1];

        if (len < 2 || off + len>32)
            return 0;

        if (type == USB_INTERFACE_DESCRIPTOR_TYPE)
        {
            if (len < 9)
                return 0;

            curMsc =
                d[off + 5] == 0x08 &&
                d[off + 6] == 0x06 &&
                d[off + 7] == 0x50;

            if (curMsc)
            {
                s_mscIfNum = d[off + 2];
                s_mscAlt = d[off + 3];
                foundIn = 0;
                foundOut = 0;
            }
        }
        else if (type == USB_ENDPOINT_DESCRIPTOR_TYPE && curMsc)
        {
            UCHAR addr;
            UCHAR attr;
            USHORT mps;

            if (len < 7)
                return 0;

            addr = d[off + 2];
            attr = d[off + 3];
            mps = (USHORT)(
                (USHORT)d[off + 4] |
                ((USHORT)d[off + 5] << 8));

            if ((attr & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_BULK &&
                mps > 0 &&
                mps <= 64)
            {
                if (addr & USB_ENDPOINT_DIRECTION_MASK)
                {
                    if (!foundIn)
                    {
                        s_epInAddr = addr;
                        s_epInMps = mps;
                        foundIn = 1;
                    }
                }
                else
                {
                    if (!foundOut)
                    {
                        s_epOutAddr = addr;
                        s_epOutMps = mps;
                        foundOut = 1;
                    }
                }
            }
        }

        off += len;
    }

    return curMsc && foundIn && foundOut;
}



static void U2x_SetConfiguredStatus(UCHAR cfg)
{
    char msg[24];
    const char* base = "CFG SET OK ";
    int i;

    i = 0;

    while (base[i] && i < (int)sizeof(msg) - 3)
    {
        msg[i] = base[i];
        ++i;
    }

    msg[i++] = U2x_HexNibble((UCHAR)(cfg >> 4));
    msg[i++] = U2x_HexNibble(cfg);
    msg[i] = 0;

    U2x_SetStatus(msg);
}



static ULONG U2x_OpenBulk(
    IUsbDevice* dev,
    UCHAR epAddr,
    USHORT maxPacket,
    void** outHandle,
    PULONG toggleStore)
{
    URB u;
    ULONG st;

    if (!dev || !outHandle || !toggleStore || !maxPacket)
        return (ULONG)USBD_STATUS_INVALID_PARAMETER;

    U2x_Zero(&u, sizeof(u));

    /*
        Match the hardware-proven X-View endpoint-open URB exactly.
        Use the full URB union as backing storage while Hdr.Length identifies
        the active URB_OPEN_ENDPOINT arm.
    */
    u.OpenEndpoint.Hdr.Length = (UCHAR)sizeof(URB_OPEN_ENDPOINT);
    u.OpenEndpoint.Hdr.Function = URB_FUNCTION_OPEN_ENDPOINT;
    u.OpenEndpoint.FunctionAddress = 0;
    u.OpenEndpoint.EndpointAddress = epAddr;
    u.OpenEndpoint.EndpointType = USB_ENDPOINT_TYPE_BULK;
    u.OpenEndpoint.Interval = 0;
    u.OpenEndpoint.DataToggleBits = toggleStore;
    u.OpenEndpoint.MaxPacketSize = maxPacket;
    u.OpenEndpoint.LowSpeed = 0;

    st = U2x_SubmitPoll(dev, (PURB)&u);

    if (st == 0)
        *outHandle = u.OpenEndpoint.EndpointHandle;

    return st;
}


static void U2x_SetBulkOpenStatus(void)
{
    U2x_SetStatus("BULK OPEN OK");
}



static void U2x_SetCbwDiagStatus(ULONG raw, ULONG hdr)
{
    /*
        Keep raw SubmitRequest/Header values in the existing diagnostic globals,
        but present a useful error to the user instead of Rxxxxxxxx Hxxxxxxxx.
    */
    (void)raw;
    (void)hdr;
    U2x_SetStatus("USB transfer failed");
}


static ULONG U2x_BulkOut(
    IUsbDevice* dev,
    void* handle,
    void* buffer,
    ULONG len)
{
    URB u;

    if (!dev || !handle || !buffer || !len)
        return (ULONG)USBD_STATUS_INVALID_PARAMETER;

    if (s_linkLost)
        return (ULONG)USBD_STATUS_NO_DEVICE;

    U2x_Zero(&u, sizeof(u));

    u.BulkOrInterruptTransfer.Hdr.Length =
        (UCHAR)sizeof(URB_BULK_OR_INTERRUPT_TRANSFER);

    u.BulkOrInterruptTransfer.Hdr.Function =
        URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;

    u.BulkOrInterruptTransfer.EndpointHandle = handle;
    u.BulkOrInterruptTransfer.TransferBufferLength = len;
    u.BulkOrInterruptTransfer.TransferBuffer = buffer;
    u.BulkOrInterruptTransfer.TransferDirection =
        USB_TRANSFER_DIRECTION_OUT;
    u.BulkOrInterruptTransfer.ShortTransferOK = 1;
    u.BulkOrInterruptTransfer.InterruptDelay = 0;

    return U2x_SubmitPoll(dev, (PURB)&u);
}



static ULONG U2x_BulkIn(
    IUsbDevice* dev,
    void* handle,
    void* buffer,
    ULONG len)
{
    URB u;

    if (!dev || !handle || !buffer || !len)
        return (ULONG)USBD_STATUS_INVALID_PARAMETER;

    if (s_linkLost)
        return (ULONG)USBD_STATUS_NO_DEVICE;

    U2x_Zero(&u, sizeof(u));

    u.BulkOrInterruptTransfer.Hdr.Length =
        (UCHAR)sizeof(URB_BULK_OR_INTERRUPT_TRANSFER);

    u.BulkOrInterruptTransfer.Hdr.Function =
        URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;

    u.BulkOrInterruptTransfer.EndpointHandle = handle;
    u.BulkOrInterruptTransfer.TransferBufferLength = len;
    u.BulkOrInterruptTransfer.TransferBuffer = buffer;
    u.BulkOrInterruptTransfer.TransferDirection =
        USB_TRANSFER_DIRECTION_IN;
    u.BulkOrInterruptTransfer.ShortTransferOK = 1;
    u.BulkOrInterruptTransfer.InterruptDelay = 0;

    return U2x_SubmitPoll(dev, (PURB)&u);
}


static int U2x_ReadTurCsw(
    ULONG expectedTag,
    UCHAR* outScsiStatus,
    ULONG* outUsbStatus)
{
    U2X_CSW* csw;
    ULONG st;

    if (outScsiStatus)
        *outScsiStatus = 0xFF;

    if (outUsbStatus)
        *outUsbStatus = 0;

    if (!s_ownedDev || !s_epInHandle || !s_cmdDma)
        return 0;

    if (sizeof(U2X_CSW) != 13)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    st = U2x_BulkIn(
        s_ownedDev,
        s_epInHandle,
        s_cmdDma,
        sizeof(U2X_CSW));

    if (outUsbStatus)
        *outUsbStatus = st;

    if (st != 0)
        return 0;

    csw = (U2X_CSW*)s_cmdDma;

    if (csw->dCSWSignature != U2X_CSW_SIGNATURE)
    {
        U2x_SetStatus("Storage response invalid");
        return 0;
    }

    if (csw->dCSWTag != expectedTag)
    {
        U2x_SetStatus("Storage response invalid");
        return 0;
    }

    /*
        TEST UNIT READY has no data phase, so residue should be zero.
        A nonzero residue here indicates an invalid BOT completion.
    */
    if (csw->dCSWDataResidue != 0)
    {
        U2x_SetStatus("Storage response invalid");
        return 0;
    }

    if (csw->bCSWStatus > 2)
    {
        U2x_SetStatus("Storage response invalid");
        return 0;
    }

    if (outScsiStatus)
        *outScsiStatus = csw->bCSWStatus;

    return 1;
}


static void U2x_SetCswStatus(UCHAR status)
{
    (void)status;
    U2x_SetStatus("Storage not ready");
}



static int U2x_SendInquiryCbw(
    ULONG* outStatus,
    ULONG* outTag)
{
    U2X_CBW* cbw;
    ULONG st;

    if (outStatus)
        *outStatus = 0;

    if (outTag)
        *outTag = 0;

    if (!s_ownedDev || !s_epOutHandle)
        return 0;

    if (sizeof(U2X_CBW) != 31)
        return 0;

    if (!s_cmdDma)
        s_cmdDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_CMD_DMA_BYTES);

    if (!s_cmdDma)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    cbw = (U2X_CBW*)s_cmdDma;

    cbw->dCBWSignature = U2X_CBW_SIGNATURE;
    cbw->dCBWTag = ++s_cbwTag;

    if (!cbw->dCBWTag)
        cbw->dCBWTag = ++s_cbwTag;

    cbw->dCBWDataTransferLength = 36;
    cbw->bmCBWFlags = 0x80; /* device-to-host */
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = 6;

    cbw->CBWCB[0] = U2X_SCSI_INQUIRY;
    cbw->CBWCB[1] = 0x00;   /* EVPD = 0 */
    cbw->CBWCB[2] = 0x00;
    cbw->CBWCB[3] = 0x00;
    cbw->CBWCB[4] = 36;     /* allocation length */
    cbw->CBWCB[5] = 0x00;

    st = U2x_BulkOut(
        s_ownedDev,
        s_epOutHandle,
        s_cmdDma,
        sizeof(U2X_CBW));

    if (outStatus)
        *outStatus = st;

    if (outTag)
        *outTag = cbw->dCBWTag;

    return st == 0;
}


static int U2x_ReadInquiryData(
    unsigned char* out36,
    ULONG* outUsbStatus)
{
    ULONG st;

    if (outUsbStatus)
        *outUsbStatus = 0;

    if (!out36 || !s_ownedDev || !s_epInHandle || !s_cmdDma)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    st = U2x_BulkIn(
        s_ownedDev,
        s_epInHandle,
        s_cmdDma,
        36);

    if (outUsbStatus)
        *outUsbStatus = st;

    if (st != 0)
        return 0;

    {
        int i;
        for (i = 0; i < 36; ++i)
            out36[i] = s_cmdDma[i];
    }

    return 1;
}


static int U2x_ReadCswForTag(
    ULONG expectedTag,
    UCHAR* outScsiStatus,
    ULONG* outUsbStatus)
{
    U2X_CSW* csw;
    ULONG st;

    if (outScsiStatus)
        *outScsiStatus = 0xFF;

    if (outUsbStatus)
        *outUsbStatus = 0;

    if (!s_ownedDev || !s_epInHandle || !s_cmdDma)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    st = U2x_BulkIn(
        s_ownedDev,
        s_epInHandle,
        s_cmdDma,
        sizeof(U2X_CSW));

    if (outUsbStatus)
        *outUsbStatus = st;

    if (st != 0)
        return 0;

    csw = (U2X_CSW*)s_cmdDma;

    if (csw->dCSWSignature != U2X_CSW_SIGNATURE)
    {
        U2x_SetStatus("Storage response invalid");
        return 0;
    }

    if (csw->dCSWTag != expectedTag)
    {
        U2x_SetStatus("Storage response invalid");
        return 0;
    }

    if (csw->bCSWStatus > 2)
    {
        U2x_SetStatus("Storage response invalid");
        return 0;
    }

    if (outScsiStatus)
        *outScsiStatus = csw->bCSWStatus;

    return 1;
}


static void U2x_SetInquiryOkStatus(
    const unsigned char* data,
    UCHAR cswStatus)
{
    (void)data;
    (void)cswStatus;
    U2x_SetStatus("Device inquiry failed");
}



static DWORD U2x_ReadBe32(const unsigned char* p)
{
    return ((DWORD)p[0] << 24) |
        ((DWORD)p[1] << 16) |
        ((DWORD)p[2] << 8) |
        ((DWORD)p[3]);
}


static int U2x_SendReadCapacity10Cbw(
    ULONG* outStatus,
    ULONG* outTag)
{
    U2X_CBW* cbw;
    ULONG st;

    if (outStatus)
        *outStatus = 0;

    if (outTag)
        *outTag = 0;

    if (!s_ownedDev || !s_epOutHandle)
        return 0;

    if (sizeof(U2X_CBW) != 31)
        return 0;

    if (!s_cmdDma)
        s_cmdDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_CMD_DMA_BYTES);

    if (!s_cmdDma)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    cbw = (U2X_CBW*)s_cmdDma;

    cbw->dCBWSignature = U2X_CBW_SIGNATURE;
    cbw->dCBWTag = ++s_cbwTag;

    if (!cbw->dCBWTag)
        cbw->dCBWTag = ++s_cbwTag;

    cbw->dCBWDataTransferLength = 8;
    cbw->bmCBWFlags = 0x80; /* device-to-host */
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = 10;

    cbw->CBWCB[0] = U2X_SCSI_READ_CAPACITY10;
    /* Remaining READ CAPACITY(10) CDB bytes are zero. */

    st = U2x_BulkOut(
        s_ownedDev,
        s_epOutHandle,
        s_cmdDma,
        sizeof(U2X_CBW));

    if (outStatus)
        *outStatus = st;

    if (outTag)
        *outTag = cbw->dCBWTag;

    return st == 0;
}


static int U2x_ReadCapacity10Data(
    DWORD* outLastLba,
    DWORD* outBlockSize,
    ULONG* outUsbStatus)
{
    unsigned char cap[8];
    ULONG st;
    int i;

    if (outLastLba)
        *outLastLba = 0;

    if (outBlockSize)
        *outBlockSize = 0;

    if (outUsbStatus)
        *outUsbStatus = 0;

    if (!s_ownedDev || !s_epInHandle || !s_cmdDma)
        return 0;

    for (i = 0; i < 8; ++i)
        cap[i] = 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    st = U2x_BulkIn(
        s_ownedDev,
        s_epInHandle,
        s_cmdDma,
        8);

    if (outUsbStatus)
        *outUsbStatus = st;

    if (st != 0)
        return 0;

    for (i = 0; i < 8; ++i)
        cap[i] = s_cmdDma[i];

    if (outLastLba)
        *outLastLba = U2x_ReadBe32(&cap[0]);

    if (outBlockSize)
        *outBlockSize = U2x_ReadBe32(&cap[4]);

    return 1;
}


static void U2x_SetCapacityStatus(
    DWORD lastLba,
    DWORD blockSize,
    UCHAR cswStatus)
{
    (void)lastLba;
    (void)blockSize;
    (void)cswStatus;
    U2x_SetStatus("Capacity read failed");
}



static int U2x_SendRead10OneSectorCbw(
    DWORD lba,
    ULONG* outStatus,
    ULONG* outTag)
{
    U2X_CBW* cbw;
    ULONG st;

    if (outStatus)
        *outStatus = 0;

    if (outTag)
        *outTag = 0;

    if (!s_ownedDev || !s_epOutHandle)
        return 0;

    if (sizeof(U2X_CBW) != 31)
        return 0;

    if (!s_cmdDma)
        s_cmdDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_CMD_DMA_BYTES);

    if (!s_cmdDma)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    cbw = (U2X_CBW*)s_cmdDma;

    cbw->dCBWSignature = U2X_CBW_SIGNATURE;
    cbw->dCBWTag = ++s_cbwTag;

    if (!cbw->dCBWTag)
        cbw->dCBWTag = ++s_cbwTag;

    cbw->dCBWDataTransferLength = 512;
    cbw->bmCBWFlags = 0x80; /* device-to-host */
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = 10;

    /*
        READ(10), caller-supplied LBA, transfer length 1 block.
        SCSI READ(10) encodes the LBA big-endian in CDB bytes 2..5.
    */
    cbw->CBWCB[0] = U2X_SCSI_READ10;
    cbw->CBWCB[2] = (UCHAR)((lba >> 24) & 0xFF);
    cbw->CBWCB[3] = (UCHAR)((lba >> 16) & 0xFF);
    cbw->CBWCB[4] = (UCHAR)((lba >> 8) & 0xFF);
    cbw->CBWCB[5] = (UCHAR)(lba & 0xFF);
    cbw->CBWCB[7] = 0x00;
    cbw->CBWCB[8] = 0x01;

    st = U2x_BulkOut(
        s_ownedDev,
        s_epOutHandle,
        s_cmdDma,
        sizeof(U2X_CBW));

    if (outStatus)
        *outStatus = st;

    if (outTag)
        *outTag = cbw->dCBWTag;

    return st == 0;
}


static int U2x_ReadLba0Data(
    unsigned char* first16,
    UCHAR* sig510,
    UCHAR* sig511,
    ULONG* outUsbStatus)
{
    ULONG st;
    int i;

    if (outUsbStatus)
        *outUsbStatus = 0;

    if (!first16 || !sig510 || !sig511)
        return 0;

    if (!s_ownedDev || !s_epInHandle)
        return 0;

    if (!s_sectorDma)
        s_sectorDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_SECTOR_DMA_BYTES);

    if (!s_sectorDma)
        return 0;

    U2x_Zero(s_sectorDma, U2X_SECTOR_DMA_BYTES);

    st = U2x_BulkIn(
        s_ownedDev,
        s_epInHandle,
        s_sectorDma,
        U2X_SECTOR_DMA_BYTES);

    if (outUsbStatus)
        *outUsbStatus = st;

    if (st != 0)
        return 0;

    for (i = 0; i < 16; ++i)
        first16[i] = s_sectorDma[i];

    *sig510 = s_sectorDma[510];
    *sig511 = s_sectorDma[511];

    return 1;
}





static void U2x_SetReadFailStatus(UCHAR cswStatus)
{
    (void)cswStatus;
    U2x_SetStatus("USB read failed");
}


/*
    Reusable, hardware-proven one-sector BOT READ(10):
      CBW -> 512-byte DATA IN -> CSW

    Keeping the data phase at one logical sector means the already-validated
    MaxBulkTDperTransfer=8 is sufficient on the Xbox OHCI host:
      512 / 64-byte MPS = 8 TDs.
*/
static int U2x_BotReadOneSector(
    DWORD lba,
    unsigned char* out512)
{
    ULONG cbwSt;
    ULONG dataSt;
    ULONG cswSt;
    ULONG tag;
    UCHAR scsiStatus;
    int i;

    if (!out512 ||
        !s_ownedDev ||
        !s_epOutHandle ||
        !s_epInHandle)
        return 0;

    cbwSt = 0;
    dataSt = 0;
    cswSt = 0;
    tag = 0;
    scsiStatus = 0xFF;

    if (!U2x_SendRead10OneSectorCbw(
        lba,
        &cbwSt,
        &tag))
    {
        if (cbwSt)
            U2x_SetCbwDiagStatus(
                s_lastSubmitRaw,
                s_lastHdrStatus);
        else
            U2x_SetStatus("USB read command failed");
        return 0;
    }

    if (!s_sectorDma)
        s_sectorDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_SECTOR_DMA_BYTES);

    if (!s_sectorDma)
    {
        U2x_SetStatus("USB read buffer failed");
        return 0;
    }

    U2x_Zero(s_sectorDma, U2X_SECTOR_DMA_BYTES);

    dataSt = U2x_BulkIn(
        s_ownedDev,
        s_epInHandle,
        s_sectorDma,
        U2X_SECTOR_DMA_BYTES);

    if (dataSt != 0)
    {
        U2x_SetCbwDiagStatus(
            s_lastSubmitRaw,
            s_lastHdrStatus);
        return 0;
    }

    if (!U2x_ReadCswForTag(
        tag,
        &scsiStatus,
        &cswSt))
    {
        if (cswSt)
            U2x_SetCbwDiagStatus(
                s_lastSubmitRaw,
                s_lastHdrStatus);
        return 0;
    }

    if (scsiStatus != 0)
    {
        U2x_SetReadFailStatus(scsiStatus);
        return 0;
    }

    for (i = 0; i < U2X_SECTOR_DMA_BYTES; ++i)
        out512[i] = s_sectorDma[i];

    return 1;
}



static int U2x_SendWrite10OneSectorCbw(
    DWORD lba,
    ULONG* outStatus,
    ULONG* outTag)
{
    U2X_CBW* cbw;
    ULONG st;

    if (outStatus)
        *outStatus = 0;

    if (outTag)
        *outTag = 0;

    if (!s_ownedDev || !s_epOutHandle)
        return 0;

    if (sizeof(U2X_CBW) != 31)
        return 0;

    if (!s_cmdDma)
        s_cmdDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_CMD_DMA_BYTES);

    if (!s_cmdDma)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    cbw = (U2X_CBW*)s_cmdDma;

    cbw->dCBWSignature = U2X_CBW_SIGNATURE;
    cbw->dCBWTag = ++s_cbwTag;

    if (!cbw->dCBWTag)
        cbw->dCBWTag = ++s_cbwTag;

    cbw->dCBWDataTransferLength = 512;
    cbw->bmCBWFlags = 0x00; /* host-to-device */
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = 10;

    /*
        WRITE(10), caller-supplied LBA, transfer length 1 block.
        LBA is big-endian in CDB bytes 2..5.
    */
    cbw->CBWCB[0] = U2X_SCSI_WRITE10;
    cbw->CBWCB[2] = (UCHAR)((lba >> 24) & 0xFF);
    cbw->CBWCB[3] = (UCHAR)((lba >> 16) & 0xFF);
    cbw->CBWCB[4] = (UCHAR)((lba >> 8) & 0xFF);
    cbw->CBWCB[5] = (UCHAR)(lba & 0xFF);
    cbw->CBWCB[7] = 0x00;
    cbw->CBWCB[8] = 0x01;

    st = U2x_BulkOut(
        s_ownedDev,
        s_epOutHandle,
        s_cmdDma,
        sizeof(U2X_CBW));

    if (outStatus)
        *outStatus = st;

    if (outTag)
        *outTag = cbw->dCBWTag;

    return st == 0;
}


static int U2x_BotWriteOneSector(
    DWORD lba,
    const unsigned char* in512)
{
    ULONG cbwSt;
    ULONG dataSt;
    ULONG cswSt;
    ULONG tag;
    UCHAR scsiStatus;
    int i;

    if (!in512 ||
        !s_ownedDev ||
        !s_epOutHandle ||
        !s_epInHandle)
    {
        return 0;
    }

    cbwSt = 0;
    dataSt = 0;
    cswSt = 0;
    tag = 0;
    scsiStatus = 0xFF;

    if (!U2x_SendWrite10OneSectorCbw(
        lba,
        &cbwSt,
        &tag))
    {
        if (cbwSt)
            U2x_SetCbwDiagStatus(
                s_lastSubmitRaw,
                s_lastHdrStatus);
        else
            U2x_SetStatus("USB write command failed");

        return 0;
    }

    if (!s_sectorDma)
        s_sectorDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_SECTOR_DMA_BYTES);

    if (!s_sectorDma)
    {
        U2x_SetStatus("USB write buffer failed");
        return 0;
    }

    /*
        Xbox OHCI bulk DMA uses the same proven contiguous 512-byte bounce
        buffer as READ(10).  At MPS 64 this remains exactly 8 TDs, so the
        validated MaxBulkTDperTransfer=8 quota is sufficient.
    */
    for (i = 0; i < U2X_SECTOR_DMA_BYTES; ++i)
        s_sectorDma[i] = in512[i];

    dataSt = U2x_BulkOut(
        s_ownedDev,
        s_epOutHandle,
        s_sectorDma,
        U2X_SECTOR_DMA_BYTES);

    if (dataSt != 0)
    {
        U2x_SetCbwDiagStatus(
            s_lastSubmitRaw,
            s_lastHdrStatus);
        return 0;
    }

    if (!U2x_ReadCswForTag(
        tag,
        &scsiStatus,
        &cswSt))
    {
        if (cswSt)
            U2x_SetCbwDiagStatus(
                s_lastSubmitRaw,
                s_lastHdrStatus);

        return 0;
    }

    if (scsiStatus != 0)
    {
        U2x_SetStatus("USB write failed");
        return 0;
    }

    return 1;
}


static USHORT U2x_ReadLe16(const unsigned char* p)
{
    return (USHORT)(
        ((USHORT)p[0]) |
        ((USHORT)p[1] << 8));
}


static DWORD U2x_ReadLe32(const unsigned char* p)
{
    return ((DWORD)p[0]) |
        ((DWORD)p[1] << 8) |
        ((DWORD)p[2] << 16) |
        ((DWORD)p[3] << 24);
}


static int U2x_FindFirstMbrPartition(
    const unsigned char* sector,
    UCHAR* outType,
    DWORD* outStartLba,
    DWORD* outSectorCount)
{
    int i;

    if (outType)
        *outType = 0;

    if (outStartLba)
        *outStartLba = 0;

    if (outSectorCount)
        *outSectorCount = 0;

    if (!sector)
        return 0;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return 0;

    for (i = 0; i < 4; ++i)
    {
        const unsigned char* e = sector + 446 + (i * 16);
        UCHAR type = e[4];
        DWORD start = U2x_ReadLe32(e + 8);
        DWORD count = U2x_ReadLe32(e + 12);

        if (type != 0 && count != 0)
        {
            if (outType)
                *outType = type;

            if (outStartLba)
                *outStartLba = start;

            if (outSectorCount)
                *outSectorCount = count;

            return 1;
        }
    }

    return 0;
}




static int U2x_ParseFat32Bpb(
    const unsigned char* sector,
    USHORT* outReserved,
    UCHAR* outFatCount,
    DWORD* outSectorsPerFat,
    DWORD* outTotalSectors,
    DWORD* outRootCluster)
{
    USHORT bytesPerSector;
    UCHAR sectorsPerCluster;
    USHORT reserved;
    UCHAR fatCount;
    USHORT total16;
    DWORD total32;
    USHORT spf16;
    DWORD spf32;
    DWORD rootCluster;

    if (!sector)
        return 0;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return 0;

    bytesPerSector = U2x_ReadLe16(sector + 11);
    sectorsPerCluster = sector[13];
    reserved = U2x_ReadLe16(sector + 14);
    fatCount = sector[16];
    total16 = U2x_ReadLe16(sector + 19);
    spf16 = U2x_ReadLe16(sector + 22);
    total32 = U2x_ReadLe32(sector + 32);
    spf32 = U2x_ReadLe32(sector + 36);
    rootCluster = U2x_ReadLe32(sector + 44);

    /*
        FAT32-specific structural checks only.
        Do not calculate or follow FAT/data LBAs yet.
    */
    if (bytesPerSector != 512)
        return 0;

    if (sectorsPerCluster == 0)
        return 0;

    if (reserved == 0 || fatCount == 0)
        return 0;

    /*
        FAT32 uses the 32-bit sectors/FAT field and normally leaves
        the FAT12/16 16-bit sectors/FAT field at zero.
    */
    if (spf16 != 0 || spf32 == 0)
        return 0;

    if ((total16 == 0 && total32 == 0) ||
        (total16 != 0 && total32 != 0))
        return 0;

    if (rootCluster < 2)
        return 0;

    if (outReserved)
        *outReserved = reserved;

    if (outFatCount)
        *outFatCount = fatCount;

    if (outSectorsPerFat)
        *outSectorsPerFat = spf32;

    if (outTotalSectors)
        *outTotalSectors = (total16 != 0) ? (DWORD)total16 : total32;

    if (outRootCluster)
        *outRootCluster = rootCluster;

    return 1;
}


static void U2x_SetFat32BpbStatus(
    USHORT reserved,
    UCHAR fatCount,
    DWORD sectorsPerFat,
    DWORD rootCluster)
{
    char msg[40];
    int n;
    int i;

    /*
        Compact raw BPB summary:
          R0020 F02 S00003A7C C00000002

        R = reserved sectors
        F = FAT count
        S = sectors per FAT
        C = root cluster
    */
    n = 0;

    msg[n++] = 'R';
    for (i = 3; i >= 0; --i)
        msg[n++] = U2x_HexNibble(
            (UCHAR)((reserved >> (i * 4)) & 0x0F));

    msg[n++] = ' ';
    msg[n++] = 'F';
    msg[n++] = U2x_HexNibble((UCHAR)(fatCount >> 4));
    msg[n++] = U2x_HexNibble(fatCount);

    msg[n++] = ' ';
    msg[n++] = 'S';
    for (i = 7; i >= 0; --i)
        msg[n++] = U2x_HexNibble(
            (UCHAR)((sectorsPerFat >> (i * 4)) & 0x0F));

    msg[n++] = ' ';
    msg[n++] = 'C';
    for (i = 7; i >= 0; --i)
        msg[n++] = U2x_HexNibble(
            (UCHAR)((rootCluster >> (i * 4)) & 0x0F));

    msg[n] = 0;
    U2x_SetStatus(msg);
}


static void U2x_SetBootSectorStatus(
    USHORT bytesPerSector,
    UCHAR sectorsPerCluster,
    UCHAR sig510,
    UCHAR sig511,
    UCHAR cswStatus)
{
    (void)bytesPerSector;
    (void)sectorsPerCluster;
    (void)sig510;
    (void)sig511;
    (void)cswStatus;
    U2x_SetStatus("Boot sector read failed");
}


static void U2x_SetMbrStatus(
    UCHAR type,
    DWORD startLba)
{
    char msg[24];
    int n;
    int i;

    /*
        Compact status:
          P0C@00000800

        This does not yet interpret the partition type as FAT32.
        It only exposes the first populated MBR entry.
    */
    n = 0;
    msg[n++] = 'P';
    msg[n++] = U2x_HexNibble((UCHAR)(type >> 4));
    msg[n++] = U2x_HexNibble(type);
    msg[n++] = '@';

    for (i = 7; i >= 0; --i)
        msg[n++] = U2x_HexNibble(
            (UCHAR)((startLba >> (i * 4)) & 0x0F));

    msg[n] = 0;
    U2x_SetStatus(msg);
}


static void U2x_SetRead0Status(
    UCHAR sig510,
    UCHAR sig511,
    UCHAR cswStatus)
{
    (void)sig510;
    (void)sig511;
    (void)cswStatus;
    U2x_SetStatus("MBR read failed");
}


static int U2x_SendSingleTurCbw(ULONG* outStatus)
{
    U2X_CBW* cbw;
    ULONG st;

    if (outStatus)
        *outStatus = 0;

    if (!s_ownedDev || !s_epOutHandle)
        return 0;

    if (sizeof(U2X_CBW) != 31)
        return 0;

    if (!s_cmdDma)
        s_cmdDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_CMD_DMA_BYTES);

    if (!s_cmdDma)
        return 0;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    cbw = (U2X_CBW*)s_cmdDma;

    cbw->dCBWSignature = U2X_CBW_SIGNATURE;
    cbw->dCBWTag = ++s_cbwTag;

    if (!cbw->dCBWTag)
        cbw->dCBWTag = ++s_cbwTag;

    cbw->dCBWDataTransferLength = 0;
    cbw->bmCBWFlags = 0x00;
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = 6;
    cbw->CBWCB[0] = U2X_SCSI_TEST_UNIT_READY;

    st = U2x_BulkOut(
        s_ownedDev,
        s_epOutHandle,
        s_cmdDma,
        sizeof(U2X_CBW));

    if (outStatus)
        *outStatus = st;

    return st == 0;
}


static int U2x_ReadDeviceDescriptor8(
    IUsbDevice* node,
    UCHAR* outMaxPacket0)
{
    unsigned char d[8];
    ULONG st;
    UCHAR mps0;
    int i;

    if (!node || !outMaxPacket0)
        return 0;

    for (i = 0; i < 8; ++i)
        d[i] = 0;

    /*
        Proven camera/X-View enumeration order:
        first request at USB address 0 is exactly 8 bytes of DEVICE descriptor.
        Byte 7 is bMaxPacketSize0.
    */
    st = U2x_Control(
        node,
        0x80,
        USB_REQUEST_GET_DESCRIPTOR,
        0x0100,
        0,
        d,
        8,
        USB_TRANSFER_DIRECTION_IN);

    if (st != 0)
        return 0;

    if (d[0] < 8 ||
        d[1] != USB_DEVICE_DESCRIPTOR_TYPE)
        return 0;

    mps0 = d[7];

    if (mps0 != 8 &&
        mps0 != 16 &&
        mps0 != 32 &&
        mps0 != 64)
        return 0;

    *outMaxPacket0 = mps0;
    return 1;
}




static void U2x_RunMountedTransport(void)
{
    ULONG outSt;
    ULONG inSt;

    s_epOutHandle = 0;
    s_epInHandle = 0;
    s_toggleOut = 0;
    s_toggleIn = 0;

    U2x_SetStatus("USB: OPEN BULK OUT");

    outSt = U2x_OpenBulk(
        s_ownedDev,
        s_epOutAddr,
        s_epOutMps,
        &s_epOutHandle,
        &s_toggleOut);

    if (outSt != 0 || !s_epOutHandle)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("Bulk OUT open failed");
        return;
    }

    U2x_SetStatus("USB: OPEN BULK IN");

    inSt = U2x_OpenBulk(
        s_ownedDev,
        s_epInAddr,
        s_epInMps,
        &s_epInHandle,
        &s_toggleIn);

    if (inSt != 0 || !s_epInHandle)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("Bulk IN open failed");
        return;
    }

    {
        ULONG cbwSt;

        cbwSt = 0;
        U2x_SetStatus("USB: SEND CBW31");

        if (!U2x_SendSingleTurCbw(&cbwSt))
        {
            s_state = U2X_USB_ERROR;

            if (cbwSt)
                U2x_SetCbwDiagStatus(
                    s_lastSubmitRaw,
                    s_lastHdrStatus);
            else
                U2x_SetStatus("Storage command failed");

            return;
        }

        {
            ULONG cswUsbSt;
            UCHAR cswStatus;
            ULONG expectedTag;

            /*
                The CBW helper increments s_cbwTag immediately
                before submission. Preserve that tag for CSW
                validation.
            */
            expectedTag = s_cbwTag;
            cswUsbSt = 0;
            cswStatus = 0xFF;

            U2x_SetStatus("USB: READ CSW");

            if (!U2x_ReadTurCsw(
                expectedTag,
                &cswStatus,
                &cswUsbSt))
            {
                s_state = U2X_USB_ERROR;

                /*
                    If the helper already emitted a semantic
                    CSW validation error, keep it. Otherwise
                    expose the USB transport status compactly.
                */
                if (cswUsbSt != 0)
                    U2x_SetCbwDiagStatus(
                        s_lastSubmitRaw,
                        s_lastHdrStatus);

                return;
            }

            if (cswStatus != 0)
            {
                /*
                    TEST UNIT READY did not pass. Preserve the
                    hardware result and stop; do not continue
                    into INQUIRY on a failed command.
                */
                s_state = U2X_USB_OFFLINE;
                U2x_SetCswStatus(cswStatus);
                return;
            }

            {
                unsigned char inquiry[36];
                ULONG inqCbwSt;
                ULONG inqDataSt;
                ULONG inqCswSt;
                ULONG inqTag;
                UCHAR inqStatus;
                int i;

                for (i = 0; i < 36; ++i)
                    inquiry[i] = 0;

                inqCbwSt = 0;
                inqDataSt = 0;
                inqCswSt = 0;
                inqTag = 0;
                inqStatus = 0xFF;

                U2x_SetStatus("USB: INQ CBW");

                if (!U2x_SendInquiryCbw(
                    &inqCbwSt,
                    &inqTag))
                {
                    s_state = U2X_USB_ERROR;
                    U2x_SetCbwDiagStatus(
                        s_lastSubmitRaw,
                        s_lastHdrStatus);
                    return;
                }

                U2x_SetStatus("USB: INQ DATA");

                if (!U2x_ReadInquiryData(
                    inquiry,
                    &inqDataSt))
                {
                    s_state = U2X_USB_ERROR;
                    U2x_SetCbwDiagStatus(
                        s_lastSubmitRaw,
                        s_lastHdrStatus);
                    return;
                }

                U2x_SetStatus("USB: INQ CSW");

                if (!U2x_ReadCswForTag(
                    inqTag,
                    &inqStatus,
                    &inqCswSt))
                {
                    s_state = U2X_USB_ERROR;
                    U2x_SetCbwDiagStatus(
                        s_lastSubmitRaw,
                        s_lastHdrStatus);
                    return;
                }

                if (inqStatus != 0)
                {
                    /*
                        INQUIRY transport completed, but the
                        command itself failed. Preserve that
                        result and stop before READ CAPACITY.
                    */
                    s_state = U2X_USB_OFFLINE;
                    U2x_SetInquiryOkStatus(
                        inquiry,
                        inqStatus);
                    return;
                }

                {
                    ULONG capCbwSt;
                    ULONG capDataSt;
                    ULONG capCswSt;
                    ULONG capTag;
                    UCHAR capStatus;
                    DWORD lastLba;
                    DWORD blockSize;

                    capCbwSt = 0;
                    capDataSt = 0;
                    capCswSt = 0;
                    capTag = 0;
                    capStatus = 0xFF;
                    lastLba = 0;
                    blockSize = 0;

                    U2x_SetStatus("USB: CAP CBW");

                    if (!U2x_SendReadCapacity10Cbw(
                        &capCbwSt,
                        &capTag))
                    {
                        s_state = U2X_USB_ERROR;
                        U2x_SetCbwDiagStatus(
                            s_lastSubmitRaw,
                            s_lastHdrStatus);
                        return;
                    }

                    U2x_SetStatus("USB: CAP DATA");

                    if (!U2x_ReadCapacity10Data(
                        &lastLba,
                        &blockSize,
                        &capDataSt))
                    {
                        s_state = U2X_USB_ERROR;
                        U2x_SetCbwDiagStatus(
                            s_lastSubmitRaw,
                            s_lastHdrStatus);
                        return;
                    }

                    U2x_SetStatus("USB: CAP CSW");

                    if (!U2x_ReadCswForTag(
                        capTag,
                        &capStatus,
                        &capCswSt))
                    {
                        s_state = U2X_USB_ERROR;
                        U2x_SetCbwDiagStatus(
                            s_lastSubmitRaw,
                            s_lastHdrStatus);
                        return;
                    }

                    if (capStatus != 0)
                    {
                        s_state = U2X_USB_OFFLINE;
                        U2x_SetCapacityStatus(
                            lastLba,
                            blockSize,
                            capStatus);
                        return;
                    }

                    /*
                        READ(10) is only valid for this isolated
                        test when READ CAPACITY reported 512-byte
                        logical blocks.
                    */
                    if (blockSize != 512)
                    {
                        s_state = U2X_USB_ERROR;
                        U2x_SetStatus("Unsupported sector size");
                        return;
                    }

                    {
                        ULONG readCbwSt;
                        ULONG readDataSt;
                        ULONG readCswSt;
                        ULONG readTag;
                        UCHAR readStatus;
                        UCHAR sig510;
                        UCHAR sig511;
                        unsigned char first16[16];
                        int i;

                        readCbwSt = 0;
                        readDataSt = 0;
                        readCswSt = 0;
                        readTag = 0;
                        readStatus = 0xFF;
                        sig510 = 0;
                        sig511 = 0;

                        for (i = 0; i < 16; ++i)
                            first16[i] = 0;

                        U2x_SetStatus("USB: R0 CBW");

                        if (!U2x_SendRead10OneSectorCbw(
                            0,
                            &readCbwSt,
                            &readTag))
                        {
                            s_state = U2X_USB_ERROR;
                            U2x_SetCbwDiagStatus(
                                s_lastSubmitRaw,
                                s_lastHdrStatus);
                            return;
                        }

                        U2x_SetStatus("USB: R0 DATA");

                        if (!U2x_ReadLba0Data(
                            first16,
                            &sig510,
                            &sig511,
                            &readDataSt))
                        {
                            s_state = U2X_USB_ERROR;
                            U2x_SetCbwDiagStatus(
                                s_lastSubmitRaw,
                                s_lastHdrStatus);
                            return;
                        }

                        U2x_SetStatus("USB: R0 CSW");

                        if (!U2x_ReadCswForTag(
                            readTag,
                            &readStatus,
                            &readCswSt))
                        {
                            s_state = U2X_USB_ERROR;
                            U2x_SetCbwDiagStatus(
                                s_lastSubmitRaw,
                                s_lastHdrStatus);
                            return;
                        }

                        if (readStatus != 0)
                        {
                            s_state = U2X_USB_OFFLINE;
                            U2x_SetRead0Status(
                                sig510,
                                sig511,
                                readStatus);
                            return;
                        }

                        {
                            UCHAR partType;
                            DWORD partStart;
                            DWORD partCount;

                            partType = 0;
                            partStart = 0;
                            partCount = 0;

                            /*
                                STEP 15A:
                                Inspect only the already-read
                                LBA 0 partition table. Do not
                                issue another USB transfer.
                            */
                            if (U2x_FindFirstMbrPartition(
                                s_sectorDma,
                                &partType,
                                &partStart,
                                &partCount))
                            {
                                ULONG bootCbwSt;
                                ULONG bootDataSt;
                                ULONG bootCswSt;
                                ULONG bootTag;
                                UCHAR bootStatus;
                                UCHAR bootSig510;
                                UCHAR bootSig511;
                                unsigned char bootFirst16[16];
                                USHORT bytesPerSector;
                                UCHAR sectorsPerCluster;
                                int j;

                                bootCbwSt = 0;
                                bootDataSt = 0;
                                bootCswSt = 0;
                                bootTag = 0;
                                bootStatus = 0xFF;
                                bootSig510 = 0;
                                bootSig511 = 0;
                                bytesPerSector = 0;
                                sectorsPerCluster = 0;

                                for (j = 0; j < 16; ++j)
                                    bootFirst16[j] = 0;

                                U2x_SetStatus("USB: BOOT CBW");

                                if (!U2x_SendRead10OneSectorCbw(
                                    partStart,
                                    &bootCbwSt,
                                    &bootTag))
                                {
                                    s_state = U2X_USB_ERROR;
                                    U2x_SetCbwDiagStatus(
                                        s_lastSubmitRaw,
                                        s_lastHdrStatus);
                                    return;
                                }

                                U2x_SetStatus("USB: BOOT DATA");

                                if (!U2x_ReadLba0Data(
                                    bootFirst16,
                                    &bootSig510,
                                    &bootSig511,
                                    &bootDataSt))
                                {
                                    s_state = U2X_USB_ERROR;
                                    U2x_SetCbwDiagStatus(
                                        s_lastSubmitRaw,
                                        s_lastHdrStatus);
                                    return;
                                }

                                /*
                                    Raw BPB fields only:
                                      offset 11: bytes/sector LE16
                                      offset 13: sectors/cluster
                                */
                                bytesPerSector =
                                    (USHORT)s_sectorDma[11] |
                                    ((USHORT)s_sectorDma[12] << 8);

                                sectorsPerCluster =
                                    s_sectorDma[13];

                                U2x_SetStatus("USB: BOOT CSW");

                                if (!U2x_ReadCswForTag(
                                    bootTag,
                                    &bootStatus,
                                    &bootCswSt))
                                {
                                    s_state = U2X_USB_ERROR;
                                    U2x_SetCbwDiagStatus(
                                        s_lastSubmitRaw,
                                        s_lastHdrStatus);
                                    return;
                                }

                                if (bootStatus != 0)
                                {
                                    s_state = U2X_USB_OFFLINE;
                                    U2x_SetBootSectorStatus(
                                        bytesPerSector,
                                        sectorsPerCluster,
                                        bootSig510,
                                        bootSig511,
                                        bootStatus);
                                    return;
                                }

                                {
                                    USHORT reserved;
                                    UCHAR fatCount;
                                    DWORD sectorsPerFat;
                                    DWORD totalSectors;
                                    DWORD rootCluster;

                                    reserved = 0;
                                    fatCount = 0;
                                    sectorsPerFat = 0;
                                    totalSectors = 0;
                                    rootCluster = 0;

                                    /*
                                        STEP 16A:
                                        Parse FAT32 BPB fields
                                        from the boot sector
                                        already in s_sectorDma.
                                        No new USB transaction.
                                    */
                                    if (!U2x_ParseFat32Bpb(
                                        s_sectorDma,
                                        &reserved,
                                        &fatCount,
                                        &sectorsPerFat,
                                        &totalSectors,
                                        &rootCluster))
                                    {
                                        s_state = U2X_USB_ERROR;
                                        U2x_SetStatus("FAT32 metadata invalid");
                                        return;
                                    }

                                    /*
                                        The transport and FAT32
                                        volume are now usable by
                                        the rest of USB2XB.
                                    */
                                    s_sectorSize = blockSize;
                                    s_sectorCount = lastLba + 1;
                                    s_volumeLba = partStart;
                                    s_fat32Detected = 1;

                                    s_sectorsPerCluster =
                                        sectorsPerCluster;
                                    s_reservedSectors = reserved;
                                    s_fatCount = fatCount;
                                    s_sectorsPerFat =
                                        sectorsPerFat;
                                    s_rootCluster = rootCluster;

                                    /*
                                        Clamp the BPB-reported
                                        partition span against
                                        the physical device.
                                        Detection already passed;
                                        this guards only metadata.
                                    */
                                    if (s_volumeLba >= s_sectorCount ||
                                        totalSectors >
                                        s_sectorCount - s_volumeLba)
                                    {
                                        s_state = U2X_USB_ERROR;
                                        U2x_SetStatus(
                                            "FAT32 range invalid");
                                        return;
                                    }

                                    s_state = U2X_USB_READY;
                                    s_hotplugPolling = 0;
                                    s_nextHotplugPoll = GetTickCount() + 250;
                                    U2x_SetStatus("USB FAT32 READY");
                                }
                            }
                            else
                            {
                                s_state = U2X_USB_OFFLINE;
                                U2x_SetStatus("Partition not found");
                            }
                        }
                    }
                }
            }
        }
    }

}


static void U2x_RunStep8(void)
{
    ULONG st;

    /*
        Manual node/address pools are finite and there is no free operation at
        this layer. This validation build therefore permits one manual attempt
        per app launch.
    */
    if (s_manualAttemptUsed)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("Restart app to rescan");
        return;
    }

    s_manualAttemptUsed = 1;
    s_transportUsbdOwned = 0;
    InterlockedExchange(&s_linkLost, 0);

    s_state = U2X_USB_ENUMERATING;
    U2x_SetStatus("USB: RESET CANDIDATE");

    if (!U2x_ResetPort(s_hubDev, s_hubPort))
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("USB reset failed");
        return;
    }

    U2x_SetStatus("USB: ALLOC OWNED NODE");

    s_ownedDev = U2x_BuildOwnedNode(s_hubPort);

    if (!s_ownedDev)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("USB setup failed");
        return;
    }

    U2x_SetStatus("USB: OPEN EP0 ADDR0");

    st = U2x_OpenDefaultEP(s_ownedDev);

    if (st != 0)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("USB control open failed");
        return;
    }

    {
        UCHAR maxPacket0;

        maxPacket0 = 0;

        U2x_SetStatus("USB: READ DEV DESC8");

        if (!U2x_ReadDeviceDescriptor8(
            s_ownedDev,
            &maxPacket0))
        {
            s_state = U2X_USB_ERROR;
            U2x_SetStatus("Device descriptor failed");
            return;
        }

        ((unsigned char*)(void*)s_ownedDev)[6] = maxPacket0;

        {
            unsigned char* nb;
            void* hc;
            UCHAR addr;
            ULONG closeSt;
            ULONG openSt;

            nb = (unsigned char*)(void*)s_ownedDev;
            hc = *(void**)(nb + 0x0C);

            if (!hc)
            {
                s_state = U2X_USB_ERROR;
                U2x_SetStatus("USB host unavailable");
                return;
            }

            U2x_SetStatus("USB: ALLOC ADDRESS");

            addr = USBD_AllocateUsbAddress(
                (struct _USBD_HOST_CONTROLLER*)hc);

            if (addr == 0)
            {
                s_state = U2X_USB_ERROR;
                U2x_SetStatus("USB address failed");
                return;
            }

            U2x_SetStatus("USB: SET ADDRESS");

            st = U2x_Control(
                s_ownedDev,
                0x00,
                USB_REQUEST_SET_ADDRESS,
                (USHORT)addr,
                0,
                0,
                0,
                USB_TRANSFER_DIRECTION_OUT);

            if (st != 0)
            {
                s_state = U2X_USB_ERROR;
                U2x_SetStatus("USB address setup failed");
                return;
            }

            /*
                Camera/X-View address-recovery delay.
                Do not retarget the node until the device has accepted the
                address on the wire.
            */
            Sleep(5);

            nb[5] = addr;

            U2x_SetStatus("USB: REOPEN EP0");

            closeSt = U2x_CloseDefaultEP(s_ownedDev);

            if (closeSt != 0)
            {
                s_state = U2X_USB_ERROR;
                U2x_SetStatus("USB control close failed");
                return;
            }

            openSt = U2x_OpenDefaultEP(s_ownedDev);

            if ((LONG)openSt < 0)
            {
                s_state = U2X_USB_ERROR;
                U2x_SetStatus("USB control reopen failed");
                return;
            }

            {
                USHORT configTotal;
                UCHAR configValue;

                configTotal = 0;
                configValue = 0;

                U2x_SetStatus("USB: READ CFG9");

                if (!U2x_ReadConfigHeader9(
                    s_ownedDev,
                    &configTotal,
                    &configValue))
                {
                    s_state = U2X_USB_ERROR;
                    U2x_SetStatus("Config descriptor failed");
                    return;
                }

                /*
                    Step 5 hardware result was T0020. Keep the parser bound to
                    that validated size for Step 6.
                */
                if (configTotal != 0x0020)
                {
                    s_state = U2X_USB_ERROR;
                    U2x_SetStatus("Unsupported USB config");
                    return;
                }

                U2x_SetStatus("USB: READ CFG32");

                if (!U2x_ReadAndParseConfig32(s_ownedDev))
                {
                    s_state = U2X_USB_ERROR;
                    U2x_SetStatus("Storage interface missing");
                    return;
                }

                /*
                    Step 6 is hardware-validated. Now perform only the standard
                    SET_CONFIGURATION transition using the descriptor's actual
                    bConfigurationValue. Do not open bulk pipes yet.
                */
                U2x_SetStatus("USB: SET CONFIG");

                st = U2x_Control(
                    s_ownedDev,
                    0x00,
                    USB_REQUEST_SET_CONFIGURATION,
                    (USHORT)s_cfgValue,
                    0,
                    0,
                    0,
                    USB_TRANSFER_DIRECTION_OUT);

                if (st != 0)
                {
                    s_state = U2X_USB_ERROR;
                    U2x_SetStatus("USB configuration failed");
                    return;
                }

                U2x_RunMountedTransport();
            }
        }
    }
}



static void U2x_ClearMediaState(void)
{
    s_product[0] = 0;

    s_sectorSize = 0;
    s_sectorCount = 0;
    s_volumeLba = 0;
    s_fat32Detected = 0;
    s_sectorsPerCluster = 0;
    s_reservedSectors = 0;
    s_fatCount = 0;
    s_sectorsPerFat = 0;
    s_rootCluster = 0;
}


static int U2x_GetHubPortConnected(IUsbDevice* hub, int port, int* connected)
{
    unsigned char ps[4];
    ULONG st;
    int i;
    int status;

    if (!hub || !connected || port < 1 || port > s_hubNports)
        return 0;

    if (s_hubIsRoot)
    {
        ULONG rootStatus;
        if (!U2x_ReadRootPortStatus(hub, port, &rootStatus))
            return 0;
        *connected = (rootStatus & U2X_OHCI_RH_CCS) ? 1 : 0;
        return 1;
    }

    for (i = 0; i < 4; ++i)
        ps[i] = 0;

    /*
        Read live port state only.  Do not clear change bits here: the Xbox hub
        driver is still free to service its own interrupt/change bookkeeping.
    */
    st = U2x_Control(
        hub,
        0xA3,
        USB_REQUEST_GET_STATUS,
        0,
        (USHORT)port,
        ps,
        4,
        USB_TRANSFER_DIRECTION_IN);

    if (st != 0)
        return 0;

    status = (int)ps[0] | ((int)ps[1] << 8);
    *connected = (status & 0x0001) ? 1 : 0;
    return 1;
}


static ULONG U2x_CloseBulkEndpoint(IUsbDevice* dev, void* handle)
{
    URB u;

    if (!dev || !handle)
        return 0;

    U2x_Zero(&u, sizeof(u));
    u.CloseEndpoint.Hdr.Length = (UCHAR)sizeof(URB_CLOSE_ENDPOINT);
    u.CloseEndpoint.Hdr.Function = URB_FUNCTION_CLOSE_ENDPOINT;
    u.CloseEndpoint.EndpointHandle = handle;
    u.CloseEndpoint.HcdNextClose = 0;
    u.CloseEndpoint.DataToggleBits = 0;

    /*
        U2x_SubmitPoll is called from the normal title thread here.  With no
        completion callback, IUsbDevice::SubmitRequest performs the Xbox
        driver's synchronous wait for asynchronous close URBs.
    */
    return U2x_SubmitPoll(dev, (PURB)&u);
}


static int U2x_ReclaimManualDeviceAfterDisconnect(void)
{
    IUsbDevice* dev;
    unsigned char* nb;
    struct _USBD_HOST_CONTROLLER* hc;
    UCHAR addr;
    ULONG st;

    dev = s_ownedDev;

    if (!dev || !U2x_Readable(dev, U2X_NODE_SIZE))
        return 0;

    /* Block all new BOT I/O before touching endpoint lifetime. */
    InterlockedExchange(&s_linkLost, 1);

    nb = (unsigned char*)(void*)dev;

    /* Close EP0 if the manual enumeration session still owns it. */
    if (*(void**)(nb + 0x08) != 0)
    {
        st = U2x_CloseDefaultEP(dev);
        if (st != 0)
        {
            s_state = U2X_USB_ERROR;
            U2x_SetStatus("USB cleanup failed - restart");
            return 0;
        }
    }

    if (s_epOutHandle)
    {
        st = U2x_CloseBulkEndpoint(dev, s_epOutHandle);
        if (st != 0)
        {
            s_state = U2X_USB_ERROR;
            U2x_SetStatus("USB cleanup failed - restart");
            return 0;
        }
        s_epOutHandle = 0;
    }

    if (s_epInHandle)
    {
        st = U2x_CloseBulkEndpoint(dev, s_epInHandle);
        if (st != 0)
        {
            s_state = U2X_USB_ERROR;
            U2x_SetStatus("USB cleanup failed - restart");
            return 0;
        }
        s_epInHandle = 0;
    }

    hc = *(struct _USBD_HOST_CONTROLLER**)(nb + 0x0C);
    addr = nb[5];

    if (!hc)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("USB cleanup failed - restart");
        return 0;
    }

    /*
        Match the native USBD reclaim ordering: close endpoints, return the
        address to the host-controller bitmap, then recycle the device node.
        This node was never inserted into a hub child list, so RemoveChild is
        intentionally not part of this private-node cleanup path.
    */
    if (addr)
    {
        USBD_FreeUsbAddress(hc, addr);
        nb[5] = 0;
    }

    g_DeviceTree.FreeDevice(dev);

    s_ownedDev = 0;
    s_toggleOut = 0;
    s_toggleIn = 0;
    s_transportUsbdOwned = 0;
    s_manualAttemptUsed = 0;

    U2x_ClearMediaState();

    s_hubPort = -1;
    s_state = U2X_USB_OFFLINE;
    U2x_SetStatus("USB removed");

    s_hotplugPolling = 1;
    s_nextHotplugPoll = GetTickCount() + 500;
    return 1;
}


static int U2x_FindRootHubCandidate(void)
{
    int rh;
    int port;
    int readableRoot = 0;

    for (rh = 0; rh < s_rootHubCount; ++rh)
    {
        IUsbDevice* root = s_rootHubs[rh];
        if (!root)
            continue;

        s_hubDev = root;
        s_hubIsRoot = 1;

        if (!U2x_ReadHubDescriptor(root))
            continue;

        readableRoot = 1;
        for (port = 1; port <= s_hubNports; ++port)
        {
            if (!U2x_ReadHubPort(root, port))
                break;
        }

        if (port <= s_hubNports)
            continue;

        if (s_hubPort > 0)
            return 1;
    }

    return readableRoot ? 0 : -1;
}

static void U2x_PollHotplugInsert(void)
{
    int port;

    if (s_hubIsRoot)
    {
        int found = U2x_FindRootHubCandidate();

        if (found < 0)
        {
            s_hotplugPolling = 0;
            s_hubDev = 0;
            s_scanRequested = 1;
            s_notBeforeTick = GetTickCount() + 250;
            return;
        }

        if (!found)
        {
            s_state = U2X_USB_OFFLINE;
            U2x_SetStatus("No USB device");
            s_nextHotplugPoll = GetTickCount() + 500;
            return;
        }

        s_hotplugPolling = 0;
        U2x_SetStatus("USB detected");
        U2x_RunStep8();
        return;
    }

    if (!s_hubDev)
    {
        s_hotplugPolling = 0;
        s_scanRequested = 1;
        s_notBeforeTick = GetTickCount() + 250;
        return;
    }

    if (!U2x_ReadHubDescriptor(s_hubDev))
    {
        s_hotplugPolling = 0;
        s_hubDev = 0;
        s_scanRequested = 1;
        s_notBeforeTick = GetTickCount() + 250;
        return;
    }

    for (port = 1; port <= s_hubNports; ++port)
    {
        if (!U2x_ReadHubPort(s_hubDev, port))
        {
            s_state = U2X_USB_OFFLINE;
            U2x_SetStatus("USB hub read failed");
            s_nextHotplugPoll = GetTickCount() + 500;
            return;
        }
    }

    if (s_hubPort < 1)
    {
        s_state = U2X_USB_OFFLINE;
        U2x_SetStatus("No USB device");
        s_nextHotplugPoll = GetTickCount() + 500;
        return;
    }

    s_hotplugPolling = 0;
    U2x_SetStatus("USB detected");
    U2x_RunStep8();
}


static void U2x_RunDiscovery(void)
{
    int port;
    int rh;

    s_transportUsbdOwned = 0;
    s_hubDev = 0;
    s_ownedDev = 0;
    s_hubPort = -1;
    s_hubNports = 0;
    s_hubIsRoot = 0;
    s_rootHubCount = 0;

    for (rh = 0; rh < U2X_MAX_ROOT_HUBS; ++rh)
        s_rootHubs[rh] = 0;

    s_sectorSize = 0;
    s_sectorCount = 0;
    s_volumeLba = 0;
    s_fat32Detected = 0;
    s_sectorsPerCluster = 0;
    s_reservedSectors = 0;
    s_fatCount = 0;
    s_sectorsPerFat = 0;
    s_rootCluster = 0;

    for (port = 0; port < 9; ++port)
    {
        s_portEnabled[port] = 0;
        s_portCandidate[port] = 0;
    }

    s_state = U2X_USB_SCANNING;
    U2x_SetStatus("Searching USB ports");
    U2x_WalkTree();

    if (s_hubDev)
    {
        s_hubIsRoot = 0;

        if (!U2x_ReadHubDescriptor(s_hubDev))
        {
            s_state = U2X_USB_ERROR;
            U2x_SetStatus("USB hub read failed");
            return;
        }

        for (port = 1; port <= s_hubNports; ++port)
        {
            if (!U2x_ReadHubPort(s_hubDev, port))
            {
                s_state = U2X_USB_ERROR;
                U2x_SetPortReadFailStatus(port);
                return;
            }
        }
    }
    else
    {
        int found;

        if (s_rootHubCount < 1)
        {
            s_state = U2X_USB_OFFLINE;
            U2x_SetStatus("USB controller not found");
            return;
        }

        s_hubIsRoot = 1;
        found = U2x_FindRootHubCandidate();

        if (found < 0)
        {
            s_state = U2X_USB_ERROR;
            U2x_SetStatus("USB root hub read failed");
            return;
        }
    }

    if (s_hubPort < 1)
    {
        s_state = U2X_USB_OFFLINE;
        U2x_SetStatus("No USB device");
        s_hotplugPolling = 1;
        s_nextHotplugPoll = GetTickCount() + 500;
        return;
    }

    s_hotplugPolling = 0;
    U2x_SetCandidateStatus(s_hubPort);
    U2x_RunStep8();
}



/* ------------------------------------------------------------------------- */

int USB2XB_USB_Init(void)
{
    int i;

    s_state = U2X_USB_OFFLINE;
    U2x_SetStatus("USB IDLE");
    s_product[0] = 0;

    s_hubDev = 0;
    s_hubPort = -1;
    s_hubNports = 0;
    s_hubIsRoot = 0;
    s_rootHubCount = 0;
    for (i = 0; i < U2X_MAX_ROOT_HUBS; ++i)
        s_rootHubs[i] = 0;
    s_epOutHandle = 0;
    s_epInHandle = 0;
    s_toggleOut = 0;
    s_toggleIn = 0;
    s_cmdDma = 0;
    s_sectorDma = 0;

    s_sectorSize = 0;
    s_sectorCount = 0;
    s_volumeLba = 0;
    s_fat32Detected = 0;
    s_sectorsPerCluster = 0;
    s_reservedSectors = 0;
    s_fatCount = 0;
    s_sectorsPerFat = 0;
    s_rootCluster = 0;

    for (i = 0; i < 9; ++i)
    {
        s_portEnabled[i] = 0;
        s_portCandidate[i] = 0;
    }

    s_scanRequested = 0;
    s_manualAttemptUsed = 0;
    s_transportUsbdOwned = 0;
    s_hotplugPolling = 0;
    s_nextHotplugPoll = 0;
    InterlockedExchange(&s_linkLost, 0);

    /*
        X-View rule: never walk g_DeviceTree while normal XInitDevices
        enumeration is still settling.
    */
    s_notBeforeTick = GetTickCount() + 2000;

    return 1;
}


void USB2XB_USB_Shutdown(void)
{
    int i;

    s_state = U2X_USB_OFFLINE;
    U2x_SetStatus("USB OFFLINE");

    s_hubDev = 0;
    s_ownedDev = 0;
    s_hubPort = -1;
    s_hubNports = 0;
    s_hubIsRoot = 0;
    s_rootHubCount = 0;
    for (i = 0; i < U2X_MAX_ROOT_HUBS; ++i)
        s_rootHubs[i] = 0;
    s_epOutHandle = 0;
    s_epInHandle = 0;
    s_toggleOut = 0;
    s_toggleIn = 0;
    if (s_cmdDma)
    {
        MmFreeContiguousMemory(s_cmdDma);
        s_cmdDma = 0;
    }
    if (s_sectorDma)
    {
        MmFreeContiguousMemory(s_sectorDma);
        s_sectorDma = 0;
    }
    s_scanRequested = 0;
    s_transportUsbdOwned = 0;
    s_hotplugPolling = 0;
    InterlockedExchange(&s_linkLost, 1);

    s_sectorSize = 0;
    s_sectorCount = 0;
    s_volumeLba = 0;
    s_fat32Detected = 0;
    s_sectorsPerCluster = 0;
    s_reservedSectors = 0;
    s_fatCount = 0;
    s_sectorsPerFat = 0;
    s_rootCluster = 0;

    for (i = 0; i < 9; ++i)
    {
        s_portEnabled[i] = 0;
        s_portCandidate[i] = 0;
    }
}


void USB2XB_USB_RequestScan(void)
{
    if (s_state == U2X_USB_READY && s_ownedDev)
    {
        U2x_SetStatus("USB already connected");
        return;
    }

    /*
        A manual attempt is reusable only after disconnect cleanup has returned
        its USB address and node. Failed bring-up still requires restart.
    */
    if (s_manualAttemptUsed)
    {
        U2x_SetStatus("Restart app to rescan");
        return;
    }

    s_hotplugPolling = 0;
    s_scanRequested = 1;
    s_state = U2X_USB_SCANNING;
    U2x_SetStatus("Searching for USB");
}


void USB2XB_USB_Pump(void)
{
    DWORD now;

    now = GetTickCount();

    if (s_scanRequested)
    {
        if ((LONG)(now - s_notBeforeTick) < 0)
        {
            U2x_SetStatus("Waiting for USB bus");
            return;
        }

        s_scanRequested = 0;
        U2x_RunDiscovery();
        return;
    }

    /*
        Mounted-device removal detection stays outside the Xbox device tree.
        A failed hub-status read is ignored for this pass so a transient hub
        control error can never destroy a working transport.
    */
    if (s_state == U2X_USB_READY &&
        s_ownedDev &&
        s_hubDev &&
        s_hubPort > 0 &&
        (LONG)(now - s_nextHotplugPoll) >= 0)
    {
        int connected = 1;

        s_nextHotplugPoll = now + 250;

        if (U2x_GetHubPortConnected(s_hubDev, s_hubPort, &connected) &&
            !connected)
        {
            U2x_ReclaimManualDeviceAfterDisconnect();
            return;
        }
    }

    /*
        Once the TI hub is known, physical insertion needs no menu action.
        Poll only while offline and only when there is no unreclaimed manual
        attempt. Mounted removal is handled by the live-port poll above.
    */
    if (s_hotplugPolling &&
        !s_manualAttemptUsed &&
        s_state == U2X_USB_OFFLINE &&
        (LONG)(now - s_nextHotplugPoll) >= 0)
    {
        U2x_PollHotplugInsert();
    }
}


/*===========================================================================
    Runtime BOT transfer/recovery path

    Keep the hardware-validated Xbox OHCI quota at 8 TDs by never submitting
    more than one 512-byte sector in an individual bulk data URB.  A logical
    READ(10)/WRITE(10), however, may cover several sectors; those sectors are
    streamed as successive 512-byte URBs before a single CSW is read.
===========================================================================*/
#define U2X_BOT_RESULT_OK        1
#define U2X_BOT_RESULT_FAILED    0
#define U2X_BOT_RESULT_RECOVER  -1


static ULONG U2x_SetEndpointToggleData0(void* handle)
{
    URB u;

    if (!s_ownedDev || !handle)
        return (ULONG)USBD_STATUS_INVALID_PARAMETER;

    U2x_Zero(&u, sizeof(u));
    u.GetSetEndpointState.Hdr.Length =
        (UCHAR)sizeof(URB_GET_SET_ENDPOINT_STATE);
    u.GetSetEndpointState.Hdr.Function =
        URB_FUNCTION_SET_ENDPOINT_STATE;
    u.GetSetEndpointState.EndpointHandle = handle;
    u.GetSetEndpointState.EndpointState =
        U2X_ENDPOINT_STATE_DATA_TOGGLE_RESET;

    return U2x_SubmitPoll(s_ownedDev, (PURB)&u);
}


static int U2x_BotResetRecovery(void)
{
    ULONG st;
    int connected = 1;

    if (!s_ownedDev || !s_epOutHandle || !s_epInHandle || s_linkLost)
        return 0;

    /*
        Before issuing recovery traffic, distinguish a real unplug from a BOT
        protocol fault.  The proven hotplug cleanup owns physical removal.
    */
    if (s_hubDev && s_hubPort > 0 &&
        U2x_GetHubPortConnected(s_hubDev, s_hubPort, &connected) &&
        !connected)
    {
        U2x_ReclaimManualDeviceAfterDisconnect();
        return 0;
    }

    U2x_SetStatus("Recovering USB storage");

    /* Bulk-Only Mass Storage Reset: class/interface request 0xFF. */
    st = U2x_Control(
        s_ownedDev,
        0x21,
        U2X_MSC_BULK_ONLY_RESET,
        0,
        (USHORT)s_mscIfNum,
        0,
        0,
        USB_TRANSFER_DIRECTION_OUT);

    if (st != 0)
        goto fail;

    /*
        BOT reset leaves halt/toggle state undefined.  Clear the device-side
        halt on both pipes, then explicitly return the Xbox host-side endpoint
        state to DATA0 as well.
    */
    st = U2x_Control(
        s_ownedDev,
        0x02,
        USB_REQUEST_CLEAR_FEATURE,
        0,
        (USHORT)s_epInAddr,
        0,
        0,
        USB_TRANSFER_DIRECTION_OUT);

    if (st != 0)
        goto fail;

    s_toggleIn = 0;
    st = U2x_SetEndpointToggleData0(s_epInHandle);

    if (st != 0)
        goto fail;

    st = U2x_Control(
        s_ownedDev,
        0x02,
        USB_REQUEST_CLEAR_FEATURE,
        0,
        (USHORT)s_epOutAddr,
        0,
        0,
        USB_TRANSFER_DIRECTION_OUT);

    if (st != 0)
        goto fail;

    s_toggleOut = 0;
    st = U2x_SetEndpointToggleData0(s_epOutHandle);

    if (st != 0)
        goto fail;

    U2x_SetStatus("USB transport recovered");
    return 1;

fail:
    /* A disappearing device should flow back through the proven hotplug path. */
    connected = 1;

    if (s_hubDev && s_hubPort > 0 &&
        U2x_GetHubPortConnected(s_hubDev, s_hubPort, &connected) &&
        !connected)
    {
        U2x_ReclaimManualDeviceAfterDisconnect();
        return 0;
    }

    s_state = U2X_USB_ERROR;
    U2x_SetStatus("USB recovery failed");
    return 0;
}


static int U2x_ReadRuntimeCsw(
    ULONG expectedTag,
    ULONG expectedBytes,
    UCHAR* outStatus)
{
    U2X_CSW* csw;
    ULONG st;

    if (outStatus)
        *outStatus = 0xFF;

    if (!s_ownedDev || !s_epInHandle || !s_cmdDma)
        return U2X_BOT_RESULT_RECOVER;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);

    st = U2x_BulkIn(
        s_ownedDev,
        s_epInHandle,
        s_cmdDma,
        sizeof(U2X_CSW));

    if (st != 0)
        return U2X_BOT_RESULT_RECOVER;

    csw = (U2X_CSW*)s_cmdDma;

    if (csw->dCSWSignature != U2X_CSW_SIGNATURE ||
        csw->dCSWTag != expectedTag ||
        csw->bCSWStatus > 2 ||
        csw->dCSWDataResidue > expectedBytes)
    {
        U2x_SetStatus("Storage response invalid");
        return U2X_BOT_RESULT_RECOVER;
    }

    if (outStatus)
        *outStatus = csw->bCSWStatus;

    if (csw->bCSWStatus == 2)
    {
        U2x_SetStatus("USB storage phase error");
        return U2X_BOT_RESULT_RECOVER;
    }

    if (csw->bCSWStatus == 1)
    {
        U2x_SetStatus("USB storage command failed");
        return U2X_BOT_RESULT_FAILED;
    }

    if (csw->dCSWDataResidue != 0)
    {
        U2x_SetStatus("USB transfer incomplete");
        return U2X_BOT_RESULT_RECOVER;
    }

    return U2X_BOT_RESULT_OK;
}


static int U2x_SendRuntimeRwCbw(
    DWORD lba,
    DWORD count,
    int write,
    ULONG* outTag)
{
    U2X_CBW* cbw;
    ULONG st;
    ULONG bytes;

    if (outTag)
        *outTag = 0;

    if (!s_cmdDma || !count || count > U2X_BOT_MAX_BLOCKS)
        return U2X_BOT_RESULT_FAILED;

    bytes = count * U2X_SECTOR_DMA_BYTES;

    U2x_Zero(s_cmdDma, U2X_CMD_DMA_BYTES);
    cbw = (U2X_CBW*)s_cmdDma;

    cbw->dCBWSignature = U2X_CBW_SIGNATURE;
    cbw->dCBWTag = ++s_cbwTag;

    if (!cbw->dCBWTag)
        cbw->dCBWTag = ++s_cbwTag;

    cbw->dCBWDataTransferLength = bytes;
    cbw->bmCBWFlags = write ? 0x00 : 0x80;
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = 10;
    cbw->CBWCB[0] = write ? U2X_SCSI_WRITE10 : U2X_SCSI_READ10;
    cbw->CBWCB[2] = (UCHAR)((lba >> 24) & 0xFF);
    cbw->CBWCB[3] = (UCHAR)((lba >> 16) & 0xFF);
    cbw->CBWCB[4] = (UCHAR)((lba >> 8) & 0xFF);
    cbw->CBWCB[5] = (UCHAR)(lba & 0xFF);
    cbw->CBWCB[7] = (UCHAR)((count >> 8) & 0xFF);
    cbw->CBWCB[8] = (UCHAR)(count & 0xFF);

    st = U2x_BulkOut(
        s_ownedDev,
        s_epOutHandle,
        s_cmdDma,
        sizeof(U2X_CBW));

    if (st != 0)
        return U2X_BOT_RESULT_RECOVER;

    if (outTag)
        *outTag = cbw->dCBWTag;

    return U2X_BOT_RESULT_OK;
}


static int U2x_BotReadBlocksOnce(
    DWORD lba,
    DWORD count,
    unsigned char* out)
{
    ULONG tag = 0;
    DWORD i;
    int r;
    UCHAR cswStatus = 0xFF;

    if (!out || !count || count > U2X_BOT_MAX_BLOCKS)
        return U2X_BOT_RESULT_FAILED;

    if (!s_sectorDma)
        s_sectorDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_SECTOR_DMA_BYTES);

    if (!s_sectorDma)
    {
        U2x_SetStatus("USB read buffer failed");
        return U2X_BOT_RESULT_FAILED;
    }

    r = U2x_SendRuntimeRwCbw(lba, count, 0, &tag);

    if (r != U2X_BOT_RESULT_OK)
        return r;

    for (i = 0; i < count; ++i)
    {
        ULONG st;

        U2x_Zero(s_sectorDma, U2X_SECTOR_DMA_BYTES);

        st = U2x_BulkIn(
            s_ownedDev,
            s_epInHandle,
            s_sectorDma,
            U2X_SECTOR_DMA_BYTES);

        if (st != 0)
            return U2X_BOT_RESULT_RECOVER;

        CopyMemory(
            out + i * U2X_SECTOR_DMA_BYTES,
            s_sectorDma,
            U2X_SECTOR_DMA_BYTES);
    }

    return U2x_ReadRuntimeCsw(
        tag,
        count * U2X_SECTOR_DMA_BYTES,
        &cswStatus);
}


static int U2x_BotWriteBlocksOnce(
    DWORD lba,
    DWORD count,
    const unsigned char* in)
{
    ULONG tag = 0;
    DWORD i;
    int r;
    UCHAR cswStatus = 0xFF;

    if (!in || !count || count > U2X_BOT_MAX_BLOCKS)
        return U2X_BOT_RESULT_FAILED;

    if (!s_sectorDma)
        s_sectorDma = (unsigned char*)
        MmAllocateContiguousMemory(U2X_SECTOR_DMA_BYTES);

    if (!s_sectorDma)
    {
        U2x_SetStatus("USB write buffer failed");
        return U2X_BOT_RESULT_FAILED;
    }

    r = U2x_SendRuntimeRwCbw(lba, count, 1, &tag);

    if (r != U2X_BOT_RESULT_OK)
        return r;

    for (i = 0; i < count; ++i)
    {
        ULONG st;

        CopyMemory(
            s_sectorDma,
            in + i * U2X_SECTOR_DMA_BYTES,
            U2X_SECTOR_DMA_BYTES);

        st = U2x_BulkOut(
            s_ownedDev,
            s_epOutHandle,
            s_sectorDma,
            U2X_SECTOR_DMA_BYTES);

        if (st != 0)
            return U2X_BOT_RESULT_RECOVER;
    }

    return U2x_ReadRuntimeCsw(
        tag,
        count * U2X_SECTOR_DMA_BYTES,
        &cswStatus);
}


static int U2x_BotReadBlocks(
    DWORD lba,
    DWORD count,
    unsigned char* out)
{
    int r = U2x_BotReadBlocksOnce(lba, count, out);

    if (r == U2X_BOT_RESULT_OK)
        return 1;

    if (r != U2X_BOT_RESULT_RECOVER)
        return 0;

    if (!U2x_BotResetRecovery())
        return 0;

    r = U2x_BotReadBlocksOnce(lba, count, out);

    if (r == U2X_BOT_RESULT_RECOVER)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("USB transport failed");
    }

    return r == U2X_BOT_RESULT_OK;
}


static int U2x_BotWriteBlocks(
    DWORD lba,
    DWORD count,
    const unsigned char* in)
{
    int r = U2x_BotWriteBlocksOnce(lba, count, in);

    if (r == U2X_BOT_RESULT_OK)
        return 1;

    if (r != U2X_BOT_RESULT_RECOVER)
        return 0;

    if (!U2x_BotResetRecovery())
        return 0;

    /* Rewriting the same logical blocks with the same bytes is idempotent. */
    r = U2x_BotWriteBlocksOnce(lba, count, in);

    if (r == U2X_BOT_RESULT_RECOVER)
    {
        s_state = U2X_USB_ERROR;
        U2x_SetStatus("USB transport failed");
    }

    return r == U2X_BOT_RESULT_OK;
}


U2XUsbState USB2XB_USB_State(void)
{
    return s_state;
}


const char* USB2XB_USB_StatusText(void)
{
    return s_status;
}


const char* USB2XB_USB_Product(void)
{
    return s_product;
}


/*
    Runtime sector backend.  Logical requests are batched into multi-block
    READ(10)/WRITE(10) commands while each physical bulk data URB remains one
    512-byte sector, preserving MaxBulkTDperTransfer=8.
*/
int USB2XB_USB_ReadSectors(
    DWORD lba,
    DWORD count,
    void* buffer)
{
    unsigned char* dst;
    DWORD done;

    if (!buffer || count == 0)
        return 0;

    if (s_linkLost ||
        s_state != U2X_USB_READY ||
        !s_ownedDev ||
        !s_epOutHandle ||
        !s_epInHandle ||
        s_sectorSize != 512)
        return 0;

    if (lba >= s_sectorCount ||
        count > (s_sectorCount - lba))
        return 0;

    dst = (unsigned char*)buffer;
    done = 0;

    while (done < count)
    {
        DWORD run = count - done;

        if (run > U2X_BOT_MAX_BLOCKS)
            run = U2X_BOT_MAX_BLOCKS;

        if (!U2x_BotReadBlocks(
            lba + done,
            run,
            dst + done * U2X_SECTOR_DMA_BYTES))
        {
            return 0;
        }

        done += run;
    }

    U2x_SetStatus("USB read OK");
    return 1;
}


int USB2XB_USB_WriteSectors(
    DWORD lba,
    DWORD count,
    const void* buffer)
{
    const unsigned char* src;
    DWORD done;

    if (!buffer || count == 0)
        return 0;

    if (s_linkLost ||
        s_state != U2X_USB_READY ||
        !s_ownedDev ||
        !s_epOutHandle ||
        !s_epInHandle ||
        s_sectorSize != 512)
    {
        return 0;
    }

    if (lba >= s_sectorCount ||
        count > (s_sectorCount - lba))
    {
        return 0;
    }

    src = (const unsigned char*)buffer;
    done = 0;

    while (done < count)
    {
        DWORD run = count - done;

        if (run > U2X_BOT_MAX_BLOCKS)
            run = U2X_BOT_MAX_BLOCKS;

        if (!U2x_BotWriteBlocks(
            lba + done,
            run,
            src + done * U2X_SECTOR_DMA_BYTES))
        {
            return 0;
        }

        done += run;
    }

    U2x_SetStatus("USB write OK");
    return 1;
}


DWORD USB2XB_USB_SectorSize(void)
{
    return s_sectorSize;
}


DWORD USB2XB_USB_SectorCount(void)
{
    return s_sectorCount;
}


int USB2XB_USB_Fat32Detected(void)
{
    return s_fat32Detected;
}


DWORD USB2XB_USB_VolumeLba(void)
{
    return s_volumeLba;
}
