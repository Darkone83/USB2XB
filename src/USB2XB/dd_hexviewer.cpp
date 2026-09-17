#include <xtl.h>
#include <stdlib.h>

#include "dd_hexviewer.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "font.h"
#include "input.h"
#include "usb2xb_audio.h"

#define HV_BYTES_PER_ROW   16
#define HV_VISIBLE_ROWS    16
#define HV_MAX_FILE_BYTES  (1024UL * 1024UL)
#define HV_READ_CHUNK      (32UL * 1024UL)
#define HV_REPEAT_DELAY    240UL
#define HV_REPEAT_STEP      55UL
#define HV_MARQUEE_STEP_MS 115UL
#define HV_MARQUEE_HOLD_STEPS 7UL
#define HV_MARQUEE_GAP 4

static int s_active = 0;
static unsigned char* s_buf = 0;
static DWORD s_loadedBytes = 0;
static DWORD s_fileBytes = 0;
static int s_truncated = 0;
static DWORD s_topRow = 0;
static char s_title[DD_NAME_MAX];
static char s_path[DD_PATH_MAX];
static char s_pathMarquee[DD_PATH_MAX * 2 + HV_MARQUEE_GAP + 8];
static DWORD s_pathMarqueeStart = 0;
static int s_repeatDir = 0;
static DWORD s_repeatAt = 0;

static int hv_slen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void hv_scpy(char* dst, int cap, const char* src)
{
    int i = 0;
    if (!dst || cap <= 0) return;
    while (src && src[i] && i < cap - 1)
    {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static const char* hv_marquee_path(int maxW)
{
    DWORD step;
    DWORD cycle;
    DWORD pos;
    int len;
    int i;
    int n = 0;

    if (!s_path[0] || maxW <= 0)
        return s_path;

    if (Font_MeasureText(s_path, FONT_SIZE_SMALL) <= maxW)
        return s_path;

    step = (GetTickCount() - s_pathMarqueeStart) / HV_MARQUEE_STEP_MS;
    len = hv_slen(s_path);
    if (len <= 0) return s_path;

    cycle = HV_MARQUEE_HOLD_STEPS + (DWORD)len + HV_MARQUEE_GAP;
    pos = step % cycle;
    if (pos < HV_MARQUEE_HOLD_STEPS) return s_path;
    pos -= HV_MARQUEE_HOLD_STEPS;
    s_pathMarquee[0] = 0;

    if (pos < (DWORD)len)
    {
        hv_scpy(s_pathMarquee, sizeof(s_pathMarquee), s_path + (int)pos);
        n = hv_slen(s_pathMarquee);
        for (i = 0; i < HV_MARQUEE_GAP && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = ' ';
        for (i = 0; s_path[i] && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = s_path[i];
        s_pathMarquee[n] = 0;
    }
    else
    {
        int gapLeft = HV_MARQUEE_GAP - ((int)pos - len);
        if (gapLeft < 0) gapLeft = 0;
        for (i = 0; i < gapLeft && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = ' ';
        for (i = 0; s_path[i] && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = s_path[i];
        s_pathMarquee[n] = 0;
    }

    return s_pathMarquee;
}

static char hv_hex(unsigned int v)
{
    v &= 0x0F;
    return (char)(v < 10 ? ('0' + v) : ('A' + (v - 10)));
}

static void hv_hex32(DWORD v, char* out)
{
    int i;
    for (i = 0; i < 8; ++i)
    {
        int shift = 28 - i * 4;
        out[i] = hv_hex((unsigned int)(v >> shift));
    }
    out[8] = 0;
}

static void hv_u32(DWORD v, char* out, int cap)
{
    char tmp[16];
    int n = 0;
    int i = 0;

    if (!out || cap <= 0) return;

    if (v == 0)
    {
        out[0] = '0';
        if (cap > 1) out[1] = 0;
        return;
    }

    while (v && n < (int)sizeof(tmp))
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (n > 0 && i < cap - 1)
        out[i++] = tmp[--n];
    out[i] = 0;
}

static void hv_free(void)
{
    if (s_buf)
    {
        free(s_buf);
        s_buf = 0;
    }

    s_loadedBytes = 0;
    s_fileBytes = 0;
    s_truncated = 0;
    s_topRow = 0;
    s_repeatDir = 0;
    s_repeatAt = 0;
}

void HexViewer_Close(void)
{
    hv_free();
    s_active = 0;
    s_title[0] = 0;
    s_path[0] = 0;
    s_pathMarqueeStart = 0;
}

int HexViewer_Open(DDStorage* fs, const char* path, const char* filename)
{
    DDFileHandle h;
    DWORD size = 0;
    DWORD target;
    DWORD done = 0;

    HexViewer_Close();

    if (!fs || !path || !path[0] || !fs->open_read || !fs->read || !fs->close)
        return 0;

    h = fs->open_read(fs, path, &size);
    if (!h)
        return 0;

    s_fileBytes = size;
    target = size;
    if (target > HV_MAX_FILE_BYTES)
    {
        target = HV_MAX_FILE_BYTES;
        s_truncated = 1;
    }

    s_buf = (unsigned char*)malloc(target ? target : 1);
    if (!s_buf)
    {
        fs->close(fs, h);
        hv_free();
        return 0;
    }

    while (done < target)
    {
        DWORD want = target - done;
        DWORD got = 0;

        if (want > HV_READ_CHUNK)
            want = HV_READ_CHUNK;

        if (!fs->read(fs, h, s_buf + done, want, &got))
        {
            fs->close(fs, h);
            HexViewer_Close();
            return 0;
        }

        if (got == 0)
            break;

        done += got;
    }

    fs->close(fs, h);

    if (done != target)
    {
        HexViewer_Close();
        return 0;
    }

    s_loadedBytes = done;
    s_topRow = 0;
    s_repeatDir = 0;
    s_repeatAt = 0;
    hv_scpy(s_title, sizeof(s_title), filename && filename[0] ? filename : "HEX VIEWER");
    hv_scpy(s_path, sizeof(s_path), path);
    s_pathMarqueeStart = GetTickCount();
    s_active = 1;
    return 1;
}

int HexViewer_IsActive(void)
{
    return s_active;
}

static DWORD hv_row_count(void)
{
    if (s_loadedBytes == 0)
        return 0;
    return (s_loadedBytes + HV_BYTES_PER_ROW - 1) / HV_BYTES_PER_ROW;
}

static void hv_clamp_top(void)
{
    DWORD rows = hv_row_count();
    DWORD maxTop = 0;

    if (rows > HV_VISIBLE_ROWS)
        maxTop = rows - HV_VISIBLE_ROWS;

    if (s_topRow > maxTop)
        s_topRow = maxTop;
}

static int hv_repeat_nav(WORD pressed, WORD held)
{
    int dir = 0;
    DWORD now = GetTickCount();

    if (pressed & BTN_DPAD_UP) dir = -1;
    else if (pressed & BTN_DPAD_DOWN) dir = 1;

    if (dir)
    {
        s_repeatDir = dir;
        s_repeatAt = now + HV_REPEAT_DELAY;
        return dir;
    }

    if (held & BTN_DPAD_UP) dir = -1;
    else if (held & BTN_DPAD_DOWN) dir = 1;

    if (!dir)
    {
        s_repeatDir = 0;
        s_repeatAt = 0;
        return 0;
    }

    if (dir != s_repeatDir)
    {
        s_repeatDir = dir;
        s_repeatAt = now + HV_REPEAT_DELAY;
        return 0;
    }

    if ((LONG)(now - s_repeatAt) >= 0)
    {
        s_repeatAt = now + HV_REPEAT_STEP;
        return dir;
    }

    return 0;
}

void HexViewer_Update(WORD pressed, WORD held)
{
    int dir;

    if (!s_active) return;

    if ((pressed & BTN_B) || (pressed & BTN_BACK))
    {
        HexViewer_Close();
        USB2XB_AudioPlay(U2X_SOUND_BACK);
        return;
    }

    dir = hv_repeat_nav(pressed, held);
    if (dir < 0)
    {
        if (s_topRow > 0)
            --s_topRow;
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }
    else if (dir > 0)
    {
        ++s_topRow;
        hv_clamp_top();
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }

    if (pressed & BTN_LTRIG)
    {
        if (s_topRow > HV_VISIBLE_ROWS)
            s_topRow -= HV_VISIBLE_ROWS;
        else
            s_topRow = 0;
        hv_clamp_top();
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }

    if (pressed & BTN_RTRIG)
    {
        s_topRow += HV_VISIBLE_ROWS;
        hv_clamp_top();
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }
}

static void hv_outline(float x, float y, float w, float h, DWORD c)
{
    UI_FillRect(x, y, w, 1, c);
    UI_FillRect(x, y + h - 1, w, 1, c);
    UI_FillRect(x, y, 1, h, c);
    UI_FillRect(x + w - 1, y, 1, h, c);
}

static void hv_make_hex_group(DWORD off, int first, int count, char* out, int cap)
{
    int n = 0;
    int i;

    if (!out || cap <= 0) return;

    for (i = 0; i < count && n < cap - 1; ++i)
    {
        DWORD pos = off + (DWORD)(first + i);
        if (i && n < cap - 1)
            out[n++] = ' ';

        if (pos < s_loadedBytes)
        {
            unsigned char b = s_buf[pos];
            if (n < cap - 1) out[n++] = hv_hex(b >> 4);
            if (n < cap - 1) out[n++] = hv_hex(b);
        }
        else
        {
            if (n < cap - 1) out[n++] = ' ';
            if (n < cap - 1) out[n++] = ' ';
        }
    }

    out[n] = 0;
}

static void hv_make_ascii(DWORD off, char* out, int cap)
{
    int i;
    int n = 0;

    if (!out || cap <= 0) return;

    for (i = 0; i < HV_BYTES_PER_ROW && n < cap - 1; ++i)
    {
        DWORD pos = off + (DWORD)i;
        unsigned char c;

        if (pos >= s_loadedBytes)
            break;

        c = s_buf[pos];
        if (c < 32 || c > 126)
            c = '.';
        out[n++] = (char)c;
    }

    out[n] = 0;
}

void HexViewer_Render(void)
{
    IDirect3DDevice8* d;
    int vw;
    int x;
    int y = 70;
    int w;
    int h = 356;
    int lineH;
    int i;
    char offText[9];
    char g0[32];
    char g1[32];
    char ascii[HV_BYTES_PER_ROW + 1];
    char status[80];
    char a[16];
    char b[16];

    if (!s_active) return;

    d = Gfx_Device();
    vw = (int)UI_Width();
    x = 32;
    w = vw - 64;

    UI_FillRect(0, 58, (float)vw, 386, UI_ARGB(126, 0, 0, 0));
    UI_FillRect((float)(x + 7), (float)(y + 8), (float)w, (float)h, UI_ARGB(78, 0, 0, 0));
    UI_FillRect((float)(x + 4), (float)(y + 5), (float)w, (float)h, UI_ARGB(110, 0, 0, 0));
    UI_FillRect((float)x, (float)y, (float)w, (float)h, UI_ARGB(238, 16, 11, 26));
    hv_outline((float)x, (float)y, (float)w, (float)h, UI_ARGB(255, 96, 69, 126));
    hv_outline((float)(x + 2), (float)(y + 2), (float)(w - 4), (float)(h - 4), UI_ARGB(65, 218, 147, 255));
    UI_FillRect((float)x, (float)y, (float)w, 4, UI_ARGB(255, 194, 80, 255));

    Font_DrawTextEllipsis(d, (float)(x + 14), (float)(y + 13),
        s_title[0] ? s_title : "HEX VIEWER",
        FONT_SIZE_MEDIUM, FONT_WHITE, w - 28);

    {
        const char* statusText = s_truncated ? "VIEW TRUNCATED" : "READ ONLY";
        DWORD statusColour = s_truncated ?
            FONT_RGBA(214, 181, 118, 255) :
            FONT_RGBA(127, 112, 146, 255);
        int statusW = Font_MeasureText(statusText, FONT_SIZE_SMALL);
        int pathW = w - 28 - statusW - 18;
        const char* pathText;

        if (pathW < 120) pathW = 120;
        pathText = hv_marquee_path(pathW);

        if (Font_MeasureText(s_path, FONT_SIZE_SMALL) > pathW)
            Font_DrawText(d, (float)(x + 14), (float)(y + 36),
                pathText, FONT_SIZE_SMALL,
                FONT_RGBA(154, 139, 173, 255), pathW);
        else
            Font_DrawTextEllipsis(d, (float)(x + 14), (float)(y + 36),
                pathText, FONT_SIZE_SMALL,
                FONT_RGBA(154, 139, 173, 255), pathW);

        Font_DrawTextRight(d, (float)(x + w - 14), (float)(y + 36),
            statusText, FONT_SIZE_SMALL, statusColour);
    }

    UI_FillRect((float)(x + 14), (float)(y + 54), (float)(w - 28), 1,
        UI_ARGB(80, 194, 80, 255));

    Font_DrawText(d, (float)(x + 18), (float)(y + 61),
        "OFFSET", FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 70);
    Font_DrawText(d, (float)(x + 92), (float)(y + 61),
        "HEX", FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 220);
    Font_DrawText(d, (float)(x + w - 132), (float)(y + 61),
        "ASCII", FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 120);

    /* Fixed design-space pitch keeps all 16 rows clear of the footer at 480p. */
    lineH = 15;

    if (s_loadedBytes == 0)
    {
        Font_DrawText(d, (float)(x + 18), (float)(y + 88),
            "(empty file)", FONT_SIZE_SMALL,
            FONT_RGBA(170, 170, 170, 255), w - 36);
    }
    else
    {
        for (i = 0; i < HV_VISIBLE_ROWS; ++i)
        {
            DWORD row = s_topRow + (DWORD)i;
            DWORD off = row * HV_BYTES_PER_ROW;
            float ry = (float)(y + 82 + i * lineH);

            if (off >= s_loadedBytes)
                break;

            hv_hex32(off, offText);
            hv_make_hex_group(off, 0, 8, g0, sizeof(g0));
            hv_make_hex_group(off, 8, 8, g1, sizeof(g1));
            hv_make_ascii(off, ascii, sizeof(ascii));

            Font_DrawText(d, (float)(x + 18), ry,
                offText, FONT_SIZE_SMALL,
                FONT_RGBA(188, 158, 214, 255), 68);
            Font_DrawText(d, (float)(x + 92), ry,
                g0, FONT_SIZE_SMALL, FONT_WHITE, 176);
            Font_DrawText(d, (float)(x + 270), ry,
                g1, FONT_SIZE_SMALL, FONT_WHITE, 176);
            Font_DrawText(d, (float)(x + w - 132), ry,
                ascii, FONT_SIZE_SMALL,
                FONT_RGBA(205, 195, 217, 255), 118);
        }
    }

    UI_FillRect((float)(x + 14), (float)(y + h - 32), (float)(w - 28), 1,
        UI_ARGB(255, 52, 42, 67));

    status[0] = 0;
    hv_scpy(status, sizeof(status), "OFFSET ");
    hv_hex32(s_topRow * HV_BYTES_PER_ROW, offText);
    {
        int n = hv_slen(status);
        int j = 0;
        while (offText[j] && n < (int)sizeof(status) - 1)
            status[n++] = offText[j++];
        status[n] = 0;
    }

    hv_u32(s_loadedBytes, a, sizeof(a));
    hv_u32(s_fileBytes, b, sizeof(b));

    Font_DrawText(d, (float)(x + 14), (float)(y + h - 23),
        status, FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 150);

    {
        char bytesText[48];
        int n;
        int j;
        hv_scpy(bytesText, sizeof(bytesText), a);
        n = hv_slen(bytesText);
        if (n < (int)sizeof(bytesText) - 1) bytesText[n++] = '/';
        j = 0;
        while (b[j] && n < (int)sizeof(bytesText) - 1)
            bytesText[n++] = b[j++];
        if (n < (int)sizeof(bytesText) - 1) bytesText[n++] = ' ';
        if (n < (int)sizeof(bytesText) - 1) bytesText[n++] = 'B';
        bytesText[n] = 0;

        Font_DrawTextRight(d, (float)(x + w - 14), (float)(y + h - 23),
            bytesText, FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255));
    }

    Font_DrawTextRight(d, (float)(vw - 20), 454,
        "DPAD SCROLL    LT / RT PAGE    B BACK",
        FONT_SIZE_SMALL, FONT_RGBA(168, 154, 187, 255));
}
