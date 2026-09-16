#pragma once
#include <xtl.h>

enum
{
    BTN_DPAD_UP    = XINPUT_GAMEPAD_DPAD_UP,
    BTN_DPAD_DOWN  = XINPUT_GAMEPAD_DPAD_DOWN,
    BTN_DPAD_LEFT  = XINPUT_GAMEPAD_DPAD_LEFT,
    BTN_DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT,

    BTN_START  = XINPUT_GAMEPAD_START,
    BTN_BACK   = XINPUT_GAMEPAD_BACK,
    BTN_LTHUMB = XINPUT_GAMEPAD_LEFT_THUMB,
    BTN_RTHUMB = XINPUT_GAMEPAD_RIGHT_THUMB,

    BTN_A      = 0x1000,
    BTN_B      = 0x2000,
    BTN_X      = 0x4000,
    BTN_Y      = 0x8000,
    BTN_BLACK  = 0x0100,
    BTN_WHITE  = 0x0200,
    BTN_LTRIG  = 0x0400,
    BTN_RTRIG  = 0x0800
};

enum ControllerType
{
    CT_UNKNOWN = 0,
    CT_DUKE,
    CT_TYPE_S
};

void InitInput(void);
void ShutdownInput(void);
void PumpInput(void);

WORD GetButtons(void);
WORD GetButtonsForPort(int port);

void GetSticks(int& lx, int& ly, int& rx, int& ry);
void GetSticksForPort(int port, int& lx, int& ly, int& rx, int& ry);
void GetRawSticks(int& lx, int& ly, int& rx, int& ry);

void GetTriggers(int& lt, int& rt, int& black, int& white,
    int& btnA, int& btnB, int& btnX, int& btnY);

void SetRumble(WORD left, WORD right);
bool IsPortConnected(int port);
bool IsMUPresent(int port, int slot);
ControllerType GetControllerType(int port);

int  GetActivePort(void);
void SetActivePort(int port);
void StepActivePort(int dir);
