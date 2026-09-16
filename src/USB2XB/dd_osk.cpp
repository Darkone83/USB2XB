/*---------------------------------------------------------------------------
    dd_osk.cpp -- controller on-screen keyboard.

    Ported from DarkDash's dd_osk.cpp.  Input behavior and geometry are kept
    intentionally aligned with DarkDash.  USB2XB does not carry DarkDash's
    theme manager, so the three Theme_Color lookups are mapped to USB2XB's
    existing purple UI palette instead.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>

#include "dd_osk.h"
#include "dd_ui.h"
#include "font.h"
#include "input.h"

typedef struct
{
    char sets[3];
    int wide;
} Key;

#define MAX_KEYS_PER_ROW 13

#define SK_SPACE  '\x01'
#define SK_BKSP   '\x02'
#define SK_DONE   '\x03'
#define SK_CANCEL '\x04'

static const Key k_t0[MAX_KEYS_PER_ROW] = {
    {{'1','1','!'},1},{{'2','2','@'},1},{{'3','3','#'},1},{{'4','4','$'},1},
    {{'5','5','%'},1},{{'6','6','^'},1},{{'7','7','&'},1},{{'8','8','*'},1},
    {{'9','9','('},1},{{'0','0',')'},1},{{'-','-','_'},1},{{'=','=','+'},1},
    {{0,0,0},0}
};

static const Key k_t1[MAX_KEYS_PER_ROW] = {
    {{'q','Q','q'},1},{{'w','W','w'},1},{{'e','E','e'},1},{{'r','R','r'},1},
    {{'t','T','t'},1},{{'y','Y','y'},1},{{'u','U','u'},1},{{'i','I','i'},1},
    {{'o','O','o'},1},{{'p','P','p'},1},{{'[','[','{'},1},{{']',']','}'},1},
    {{0,0,0},0}
};

static const Key k_t2[MAX_KEYS_PER_ROW] = {
    {{'a','A','a'},1},{{'s','S','s'},1},{{'d','D','d'},1},{{'f','F','f'},1},
    {{'g','G','g'},1},{{'h','H','h'},1},{{'j','J','j'},1},{{'k','K','k'},1},
    {{'l','L','l'},1},{{';',';',':'},1},{{'\'','\'','"'},1},
    {{0,0,0},0}
};

static const Key k_t3[MAX_KEYS_PER_ROW] = {
    {{'z','Z','z'},1},{{'x','X','x'},1},{{'c','C','c'},1},{{'v','V','v'},1},
    {{'b','B','b'},1},{{'n','N','n'},1},{{'m','M','m'},1},{{',',',','<'},1},
    {{'.','.','>'},1},{{'/','/','?'},1},{{0,0,0},0}
};

static const Key k_t4[MAX_KEYS_PER_ROW] = {
    {{SK_SPACE,SK_SPACE,SK_SPACE},4},
    {{SK_BKSP,SK_BKSP,SK_BKSP},2},
    {{SK_DONE,SK_DONE,SK_DONE},2},
    {{SK_CANCEL,SK_CANCEL,SK_CANCEL},2},
    {{0,0,0},0}
};

static const Key* const k_text[5] = {
    k_t0,k_t1,k_t2,k_t3,k_t4
};

static const int k_textCount[5] = {
    12,12,11,10,4
};

static const Key k_n0[MAX_KEYS_PER_ROW] = {
    {{'1','1','1'},1},{{'2','2','2'},1},{{'3','3','3'},1},{{0,0,0},0}
};
static const Key k_n1[MAX_KEYS_PER_ROW] = {
    {{'4','4','4'},1},{{'5','5','5'},1},{{'6','6','6'},1},{{0,0,0},0}
};
static const Key k_n2[MAX_KEYS_PER_ROW] = {
    {{'7','7','7'},1},{{'8','8','8'},1},{{'9','9','9'},1},{{0,0,0},0}
};
static const Key k_n3[MAX_KEYS_PER_ROW] = {
    {{'.','.','.'},1},{{'0','0','0'},1},{{SK_BKSP,SK_BKSP,SK_BKSP},1},
    {{0,0,0},0}
};
static const Key k_n4[MAX_KEYS_PER_ROW] = {
    {{SK_DONE,SK_DONE,SK_DONE},2},
    {{SK_CANCEL,SK_CANCEL,SK_CANCEL},1},
    {{0,0,0},0}
};

static const Key* const k_num[5] = {
    k_n0,k_n1,k_n2,k_n3,k_n4
};

static const int k_numCount[5] = {
    3,3,3,3,2
};

#define ROWS 5

#define KB_TEXT_X   40.0f
#define KB_TEXT_Y   150.0f
#define KB_TEXT_H   40.0f
#define KB_KEY_H    34.0f
#define KB_KEY_GAP  4.0f
#define KB_ROW_GAP  4.0f

static float s_panelX;
static float s_panelY;
static float s_panelW;

static int s_open = 0;
static int s_mode = OSK_TEXT;
static int s_keyset = 0;
static int s_row = 0;
static int s_col = 0;
static int s_maxLen = OSK_MAX_LEN;
static char s_text[OSK_MAX_LEN + 1];
static int s_len = 0;
static int s_blink = 0;
static WORD s_lastBtn = 0;
static int s_repeat = 0;

#define REPEAT_INITIAL 18
#define REPEAT_RATE     5

static const Key* const* Rows(void)
{
    return s_mode == OSK_NUMERIC ? k_num : k_text;
}

static const int* Counts(void)
{
    return s_mode == OSK_NUMERIC ? k_numCount : k_textCount;
}

static int RowCount(int r)
{
    return Counts()[r];
}

static void ClampCol(void)
{
    int n = RowCount(s_row);

    if (s_col >= n)
        s_col = n - 1;

    if (s_col < 0)
        s_col = 0;
}

static void LayoutForMode(void)
{
    if (s_mode == OSK_NUMERIC)
    {
        s_panelW = 240.0f;
        s_panelX = (UI_VIRT_W - s_panelW) * 0.5f;
        s_panelY = 210.0f;
    }
    else
    {
        s_panelX = KB_TEXT_X;
        s_panelW = UI_VIRT_W - KB_TEXT_X * 2.0f;
        s_panelY = 210.0f;
    }
}

void Osk_Open(int mode, const char* initial, int maxLen)
{
    int len;

    s_open = 1;
    s_mode = mode;
    s_keyset = 0;
    s_row = 0;
    s_col = 0;

    if (maxLen < 1)
        maxLen = 1;

    s_maxLen = maxLen > OSK_MAX_LEN ? OSK_MAX_LEN : maxLen;

    s_lastBtn = 0;
    s_repeat = 0;
    s_blink = 0;

    LayoutForMode();

    if (initial)
    {
        len = (int)lstrlenA(initial);

        if (len > s_maxLen)
            len = s_maxLen;

        memcpy(s_text, initial, len);
        s_text[len] = 0;
        s_len = len;
    }
    else
    {
        s_text[0] = 0;
        s_len = 0;
    }
}

void Osk_Close(void)
{
    s_open = 0;
}

int Osk_IsOpen(void)
{
    return s_open;
}

void Osk_GetText(char* buf, int buflen)
{
    int n;

    if (!buf || buflen <= 0)
        return;

    n = s_len < buflen - 1 ? s_len : buflen - 1;

    memcpy(buf, s_text, n);
    buf[n] = 0;
}

static void TypeKey(int row, int col)
{
    char ch = Rows()[row][col].sets[s_keyset];

    switch (ch)
    {
    case SK_SPACE:
        if (s_len < s_maxLen)
        {
            s_text[s_len++] = ' ';
            s_text[s_len] = 0;
        }
        break;

    case SK_BKSP:
        if (s_len > 0)
        {
            --s_len;
            s_text[s_len] = 0;
        }
        break;

    case SK_DONE:
    case SK_CANCEL:
        break;

    default:
        if (ch >= 0x20 && s_len < s_maxLen)
        {
            s_text[s_len++] = ch;
            s_text[s_len] = 0;

            if (s_keyset == 1)
                s_keyset = 0;
        }
        break;
    }
}

int Osk_Update(WORD pressed)
{
    WORD held;

    if (!s_open)
        return 0;

    ++s_blink;

    held = GetButtons();

    if (held == s_lastBtn && held != 0)
    {
        ++s_repeat;

        if (s_repeat < REPEAT_INITIAL)
            pressed = 0;
        else if ((s_repeat - REPEAT_INITIAL) % REPEAT_RATE != 0)
            pressed = 0;
    }
    else
    {
        s_repeat = 0;
        s_lastBtn = held;
    }

    if (pressed & BTN_DPAD_UP)
    {
        s_row = (s_row + ROWS - 1) % ROWS;
        ClampCol();
    }

    if (pressed & BTN_DPAD_DOWN)
    {
        s_row = (s_row + 1) % ROWS;
        ClampCol();
    }

    if (pressed & BTN_DPAD_LEFT)
        s_col = (s_col + RowCount(s_row) - 1) % RowCount(s_row);

    if (pressed & BTN_DPAD_RIGHT)
        s_col = (s_col + 1) % RowCount(s_row);

    if (pressed & (BTN_A | BTN_LTRIG))
    {
        char action =
            Rows()[s_row][s_col].sets[s_keyset];

        if (action == SK_DONE)
        {
            s_open = 0;
            return 1;
        }

        if (action == SK_CANCEL)
        {
            s_open = 0;
            return -1;
        }

        TypeKey(s_row, s_col);
    }

    if (pressed & BTN_B)
    {
        if (s_len > 0)
        {
            --s_len;
            s_text[s_len] = 0;
        }
    }

    if ((pressed & BTN_X) && s_mode == OSK_TEXT)
        s_keyset = (s_keyset + 1) % 3;

    if ((pressed & BTN_LTHUMB) && s_mode == OSK_TEXT)
        s_keyset = (s_keyset == 1) ? 0 : 1;

    if ((pressed & BTN_Y) && s_mode == OSK_TEXT)
    {
        if (s_len < s_maxLen)
        {
            s_text[s_len++] = ' ';
            s_text[s_len] = 0;
        }
    }

    if (pressed & BTN_START)
    {
        s_open = 0;
        return 1;
    }

    if (pressed & BTN_BACK)
    {
        s_open = 0;
        return -1;
    }

    return 0;
}

static void Outline(
    float x,
    float y,
    float w,
    float h,
    DWORD c)
{
    UI_FillRect(x, y, w, 1.0f, c);
    UI_FillRect(x, y + h - 1.0f, w, 1.0f, c);
    UI_FillRect(x, y, 1.0f, h, c);
    UI_FillRect(x + w - 1.0f, y, 1.0f, h, c);
}

static void KeyRect(
    int row,
    int col,
    float* ox,
    float* ow)
{
    const Key* keys = Rows()[row];
    int n = RowCount(row);
    int i;
    float units = 0.0f;
    float unit;
    float cx = s_panelX;

    for (i = 0; i < n; ++i)
        units += (float)keys[i].wide;

    unit =
        (s_panelW - KB_KEY_GAP * (float)(n - 1)) /
        units;

    for (i = 0; i < col; ++i)
        cx += (float)keys[i].wide * unit + KB_KEY_GAP;

    *ox = cx;
    *ow = (float)keys[col].wide * unit;
}

void Osk_Draw(IDirect3DDevice8* d)
{
    /*
        DarkDash used Theme_Color here.  Keep the exact OSK behavior/layout,
        but use USB2XB's existing purple UI palette.
    */
    DWORD accent = UI_ARGB(255, 194, 80, 255);
    DWORD text = UI_ARGB(255, 224, 205, 255);
    DWORD dim = UI_ARGB(255, 126, 102, 150);

    int row;
    int col;
    int n;
    int gh;
    float kx;
    float ky;
    float kw;
    float tx;
    float tw;
    int textFieldWidth;
    char label[4];

    if (!s_open || !d)
        return;

    UI_FillRect(
        0.0f,
        0.0f,
        UI_VIRT_W,
        UI_VIRT_H,
        UI_ARGB(204, 0, 0, 0));

    tx = s_mode == OSK_NUMERIC ?
        s_panelX : KB_TEXT_X;

    tw = s_mode == OSK_NUMERIC ?
        s_panelW :
        (UI_VIRT_W - KB_TEXT_X * 2.0f);

    /*
        Avoid compiler-generated __ftol2_sse on RXDK.
        These are the exact pixel widths of the text field minus 20px padding:
          numeric: 240 - 20 = 220
          text:    560 - 20 = 540
        Do not cast the float 'tw' to int here.
    */
    textFieldWidth =
        s_mode == OSK_NUMERIC ? 220 : 540;

    UI_FillRect(
        tx,
        KB_TEXT_Y,
        tw,
        KB_TEXT_H,
        UI_ARGB(255, 16, 12, 24));

    Outline(
        tx,
        KB_TEXT_Y,
        tw,
        KB_TEXT_H,
        dim);

    {
        char disp[OSK_MAX_LEN + 2];

        gh = Font_GlyphHeight(
            FONT_SIZE_MEDIUM);

        lstrcpyA(disp, s_text);

        if ((s_blink / 20) % 2 == 0)
            lstrcatA(disp, "_");

        Font_DrawText(
            d,
            tx + 10.0f,
            KB_TEXT_Y +
            (KB_TEXT_H - (float)gh) * 0.5f,
            disp,
            FONT_SIZE_MEDIUM,
            text,
            textFieldWidth);
    }

    gh = Font_GlyphHeight(
        FONT_SIZE_MEDIUM);

    for (row = 0; row < ROWS; ++row)
    {
        n = RowCount(row);
        ky = s_panelY +
            (float)row *
            (KB_KEY_H + KB_ROW_GAP);

        for (col = 0; col < n; ++col)
        {
            const Key* k =
                &Rows()[row][col];

            char ch =
                k->sets[s_keyset];

            int sel =
                row == s_row &&
                col == s_col;

            DWORD bg;
            DWORD fg;
            DWORD bd;

            KeyRect(
                row,
                col,
                &kx,
                &kw);

            if (sel)
            {
                bg = accent;
                fg = UI_ARGB(255, 16, 12, 24);
                bd = accent;
            }
            else
            {
                bd = dim;

                if (ch == SK_DONE)
                {
                    bg = UI_ARGB(255, 30, 25, 48);
                    fg = accent;
                }
                else if (ch == SK_CANCEL)
                {
                    bg = UI_ARGB(255, 54, 20, 28);
                    fg = UI_ARGB(255, 232, 80, 96);
                }
                else
                {
                    bg = UI_ARGB(255, 24, 18, 34);
                    fg = text;
                }
            }

            UI_FillRect(
                kx,
                ky,
                kw,
                KB_KEY_H,
                bg);

            Outline(
                kx,
                ky,
                kw,
                KB_KEY_H,
                bd);

            switch (ch)
            {
            case SK_SPACE:
                lstrcpyA(label, "SP");
                break;

            case SK_BKSP:
                lstrcpyA(label, "<X");
                break;

            case SK_DONE:
                lstrcpyA(label, "OK");
                break;

            case SK_CANCEL:
                lstrcpyA(label, "X");
                break;

            default:
                label[0] = ch;
                label[1] = 0;
                break;
            }

            Font_DrawTextCentered(
                d,
                kx,
                ky +
                (KB_KEY_H - (float)gh) * 0.5f,
                kw,
                label,
                FONT_SIZE_MEDIUM,
                fg);
        }
    }

    Font_DrawTextCentered(
        d,
        0.0f,
        s_panelY +
        ROWS * (KB_KEY_H + KB_ROW_GAP) + 8.0f,
        UI_VIRT_W,
        s_mode == OSK_NUMERIC ?
        "A Type   B Del   Start OK   Back Cancel" :
        "A Type  B Del  X Case  L3 Caps  Y Space  Start OK  Back Cancel",
        FONT_SIZE_SMALL,
        dim);
}
