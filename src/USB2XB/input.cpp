#include "input.h"
#include <string.h>
#include <stdlib.h>

/*
    USB2XB input layer

    IMPORTANT:
    USB2XB intentionally does NOT register or query XDEVICE_TYPE_MEMORY_UNIT.

    A conventional FAT32 USB flash drive connected through an Xbox controller
    port must be left alone by the stock XAPI Memory Unit path. USB2XB will own
    mass-storage access through its dedicated USB MSC driver instead.

    Registering XDEVICE_TYPE_MEMORY_UNIT here causes the stock XDK MU stack to
    probe USB storage devices. That is appropriate for genuine FATX Xbox memory
    units, but it conflicts with our FAT32/raw-MSC use case and can lock the
    console when a conventional FAT32 flash drive is present.
*/

#define MAX_PORTS 4
#define ANALOG_THRESHOLD 30
#define STICK_DEADZONE 8000

static HANDLE       g_padHandles[MAX_PORTS];
static XINPUT_STATE g_padStates[MAX_PORTS];
static WORD         g_padButtons[MAX_PORTS];
static ControllerType g_ctrlType[MAX_PORTS];
static int          g_activePort = -1;

static int nav_port(void)
{
    int i;

    if (g_padHandles[0])
        return 0;

    for (i = 1; i < MAX_PORTS; ++i) {
        if (g_padHandles[i])
            return i;
    }

    return -1;
}

static void open_pad(int i)
{
    XINPUT_CAPABILITIES caps;

    if (i < 0 || i >= MAX_PORTS || g_padHandles[i])
        return;

    g_padHandles[i] =
        XInputOpen(XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);

    g_ctrlType[i] = CT_UNKNOWN;

    if (!g_padHandles[i])
        return;

    ZeroMemory(&caps, sizeof(caps));

    if (XInputGetCapabilities(g_padHandles[i], &caps) == ERROR_SUCCESS) {
        if (caps.SubType == XINPUT_DEVSUBTYPE_GC_GAMEPAD_ALT)
            g_ctrlType[i] = CT_TYPE_S;
        else if (caps.SubType == XINPUT_DEVSUBTYPE_GC_GAMEPAD)
            g_ctrlType[i] = CT_DUKE;
    }

    if (g_activePort < 0)
        g_activePort = i;
}

void InitInput(void)
{
    XDEVICE_PREALLOC_TYPE types[1];
    DWORD connected;
    int i;

    ZeroMemory(types, sizeof(types));

    /*
        Gamepads only.

        Do not add XDEVICE_TYPE_MEMORY_UNIT here. FAT32 USB storage belongs to
        usb2xb_usb.cpp, not XAPI's FATX MU driver.
    */
    types[0].DeviceType = XDEVICE_TYPE_GAMEPAD;
    types[0].dwPreallocCount = 4;

    XInitDevices(1, types);

    ZeroMemory(g_padHandles, sizeof(g_padHandles));
    ZeroMemory(g_padStates, sizeof(g_padStates));
    ZeroMemory(g_padButtons, sizeof(g_padButtons));
    ZeroMemory(g_ctrlType, sizeof(g_ctrlType));

    g_activePort = -1;

    connected = XGetDevices(XDEVICE_TYPE_GAMEPAD);

    for (i = 0; i < MAX_PORTS; ++i) {
        if (connected & (1u << i))
            open_pad(i);
    }
}

void ShutdownInput(void)
{
    int i;

    for (i = 0; i < MAX_PORTS; ++i) {
        if (g_padHandles[i]) {
            XInputClose(g_padHandles[i]);
            g_padHandles[i] = NULL;
        }

        ZeroMemory(&g_padStates[i], sizeof(g_padStates[i]));
        g_padButtons[i] = 0;
        g_ctrlType[i] = CT_UNKNOWN;
    }

    g_activePort = -1;
}

void PumpInput(void)
{
    DWORD ins = 0;
    DWORD rem = 0;
    int i;

    /*
        Gamepad hotplug only.

        Deliberately no XGetDeviceChanges(XDEVICE_TYPE_MEMORY_UNIT, ...).
    */
    if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem)) {
        for (i = 0; i < MAX_PORTS; ++i) {
            DWORD bit = 1u << i;

            if (rem & bit) {
                if (g_padHandles[i]) {
                    XInputClose(g_padHandles[i]);
                    g_padHandles[i] = NULL;
                }

                ZeroMemory(&g_padStates[i], sizeof(g_padStates[i]));
                g_padButtons[i] = 0;
                g_ctrlType[i] = CT_UNKNOWN;

                if (g_activePort == i)
                    g_activePort = -1;
            }

            if (ins & bit)
                open_pad(i);
        }
    }

    for (i = 0; i < MAX_PORTS; ++i) {
        XINPUT_STATE st;
        WORD raw;
        WORD mask;
        const BYTE* a;

        if (!g_padHandles[i])
            continue;

        ZeroMemory(&st, sizeof(st));

        if (XInputGetState(g_padHandles[i], &st) != ERROR_SUCCESS) {
            ZeroMemory(&g_padStates[i], sizeof(g_padStates[i]));
            g_padButtons[i] = 0;
            continue;
        }

        g_padStates[i] = st;

        raw = st.Gamepad.wButtons;

        /*
            Genuine OG Xbox controllers only use the low digital bits here.
            Some third-party pads mirror analog buttons into high wButtons bits,
            so keep the DarkDash compatibility behavior.
        */
        mask = raw & 0x00FF;
        a = st.Gamepad.bAnalogButtons;

        if (a[XINPUT_GAMEPAD_A] > ANALOG_THRESHOLD ||
            (raw & 0x1000))
            mask |= BTN_A;

        if (a[XINPUT_GAMEPAD_B] > ANALOG_THRESHOLD ||
            (raw & 0x2000))
            mask |= BTN_B;

        if (a[XINPUT_GAMEPAD_X] > ANALOG_THRESHOLD ||
            (raw & 0x4000))
            mask |= BTN_X;

        if (a[XINPUT_GAMEPAD_Y] > ANALOG_THRESHOLD ||
            (raw & 0x8000))
            mask |= BTN_Y;

        if (a[XINPUT_GAMEPAD_BLACK] > ANALOG_THRESHOLD ||
            (raw & 0x0100))
            mask |= BTN_BLACK;

        if (a[XINPUT_GAMEPAD_WHITE] > ANALOG_THRESHOLD ||
            (raw & 0x0200))
            mask |= BTN_WHITE;

        if (a[XINPUT_GAMEPAD_LEFT_TRIGGER] > ANALOG_THRESHOLD ||
            (raw & 0x0400))
            mask |= BTN_LTRIG;

        if (a[XINPUT_GAMEPAD_RIGHT_TRIGGER] > ANALOG_THRESHOLD ||
            (raw & 0x0800))
            mask |= BTN_RTRIG;

        g_padButtons[i] = mask;
    }

    if (g_activePort < 0)
        g_activePort = nav_port();
}

WORD GetButtons(void)
{
    int p = nav_port();

    if (p < 0)
        return 0;

    return g_padButtons[p];
}

WORD GetButtonsForPort(int port)
{
    if (port < 0 ||
        port >= MAX_PORTS ||
        !g_padHandles[port])
        return 0;

    return g_padButtons[port];
}

static void sticks_for(
    int p,
    int& lx,
    int& ly,
    int& rx,
    int& ry,
    int deadzone)
{
    const XINPUT_GAMEPAD* gp;

    lx = 0;
    ly = 0;
    rx = 0;
    ry = 0;

    if (p < 0 ||
        p >= MAX_PORTS ||
        !g_padHandles[p])
        return;

    gp = &g_padStates[p].Gamepad;

    lx = gp->sThumbLX;
    ly = gp->sThumbLY;
    rx = gp->sThumbRX;
    ry = gp->sThumbRY;

    if (deadzone) {
        if (abs(lx) < STICK_DEADZONE)
            lx = 0;
        if (abs(ly) < STICK_DEADZONE)
            ly = 0;
        if (abs(rx) < STICK_DEADZONE)
            rx = 0;
        if (abs(ry) < STICK_DEADZONE)
            ry = 0;
    }
}

void GetSticks(int& lx, int& ly, int& rx, int& ry)
{
    sticks_for(nav_port(), lx, ly, rx, ry, 1);
}

void GetSticksForPort(
    int port,
    int& lx,
    int& ly,
    int& rx,
    int& ry)
{
    sticks_for(port, lx, ly, rx, ry, 1);
}

void GetRawSticks(
    int& lx,
    int& ly,
    int& rx,
    int& ry)
{
    sticks_for(nav_port(), lx, ly, rx, ry, 0);
}

void GetTriggers(
    int& lt,
    int& rt,
    int& black,
    int& white,
    int& btnA,
    int& btnB,
    int& btnX,
    int& btnY)
{
    int p = nav_port();
    const BYTE* a;

    lt = 0;
    rt = 0;
    black = 0;
    white = 0;
    btnA = 0;
    btnB = 0;
    btnX = 0;
    btnY = 0;

    if (p < 0 || !g_padHandles[p])
        return;

    a = g_padStates[p].Gamepad.bAnalogButtons;

    lt = a[XINPUT_GAMEPAD_LEFT_TRIGGER];
    rt = a[XINPUT_GAMEPAD_RIGHT_TRIGGER];
    black = a[XINPUT_GAMEPAD_BLACK];
    white = a[XINPUT_GAMEPAD_WHITE];
    btnA = a[XINPUT_GAMEPAD_A];
    btnB = a[XINPUT_GAMEPAD_B];
    btnX = a[XINPUT_GAMEPAD_X];
    btnY = a[XINPUT_GAMEPAD_Y];
}

void SetRumble(WORD left, WORD right)
{
    int p = nav_port();
    XINPUT_FEEDBACK fb;

    if (p < 0 || !g_padHandles[p])
        return;

    ZeroMemory(&fb, sizeof(fb));

    fb.Rumble.wLeftMotorSpeed = left;
    fb.Rumble.wRightMotorSpeed = right;

    XInputSetState(g_padHandles[p], &fb);
}

bool IsPortConnected(int port)
{
    if (port < 0 || port >= MAX_PORTS)
        return false;

    return g_padHandles[port] != NULL;
}

/*
    Retained for source compatibility with callers that include input.h.

    USB2XB must not ask XAPI about Memory Units, so this always returns false.
    USB storage presence will come from the dedicated MSC layer instead.
*/
bool IsMUPresent(int port, int slot)
{
    (void)port;
    (void)slot;
    return false;
}

ControllerType GetControllerType(int port)
{
    if (port < 0 || port >= MAX_PORTS)
        return CT_UNKNOWN;

    return g_ctrlType[port];
}

int GetActivePort(void)
{
    return g_activePort;
}

void SetActivePort(int port)
{
    if (port >= 0 &&
        port < MAX_PORTS &&
        g_padHandles[port])
        g_activePort = port;
}

void StepActivePort(int dir)
{
    int n;
    int p;

    if (dir == 0)
        return;

    p = g_activePort;

    if (p < 0)
        p = 0;

    for (n = 0; n < MAX_PORTS; ++n) {
        if (dir > 0)
            p = (p + 1) % MAX_PORTS;
        else
            p = (p + MAX_PORTS - 1) % MAX_PORTS;

        if (g_padHandles[p]) {
            g_activePort = p;
            return;
        }
    }
}
