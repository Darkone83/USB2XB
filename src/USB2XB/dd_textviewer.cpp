#include <xtl.h>
#include <stdlib.h>

#include "dd_textviewer.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "font.h"
#include "input.h"
#include "usb2xb_audio.h"

#define TV_MAX_FILE_BYTES (512UL * 1024UL)
#define TV_MAX_LINES      8192
#define TV_WRAP_COLS      80
#define TV_READ_CHUNK     (32UL * 1024UL)
#define TV_REPEAT_DELAY   240UL
#define TV_REPEAT_STEP     55UL
#define TV_MARQUEE_STEP_MS 115UL
#define TV_MARQUEE_HOLD_STEPS 7UL
#define TV_MARQUEE_GAP 4

typedef struct _TV_LINE
{
    DWORD offset;
    WORD length;
} TV_LINE;

static int s_active = 0;
static unsigned char* s_buf = 0;
static DWORD s_loadedBytes = 0;
static DWORD s_fileBytes = 0;
static int s_truncated = 0;
static TV_LINE s_lines[TV_MAX_LINES];
static int s_lineCount = 0;
static int s_scroll = 0;
static char s_title[DD_NAME_MAX];
static char s_path[DD_PATH_MAX];
static char s_pathMarquee[DD_PATH_MAX * 2 + TV_MARQUEE_GAP + 8];
static DWORD s_pathMarqueeStart = 0;
static int s_repeatDir = 0;
static DWORD s_repeatAt = 0;

static int tv_slen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void tv_scpy(char* dst, int cap, const char* src)
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

static const char* tv_marquee_path(int maxW)
{
    DWORD now;
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

    now = GetTickCount();
    step = (now - s_pathMarqueeStart) / TV_MARQUEE_STEP_MS;
    len = tv_slen(s_path);

    if (len <= 0)
        return s_path;

    cycle = TV_MARQUEE_HOLD_STEPS + (DWORD)len + TV_MARQUEE_GAP;
    pos = step % cycle;

    if (pos < TV_MARQUEE_HOLD_STEPS)
        return s_path;

    pos -= TV_MARQUEE_HOLD_STEPS;
    s_pathMarquee[0] = 0;

    if (pos < (DWORD)len)
    {
        const char* tail = s_path + (int)pos;
        tv_scpy(s_pathMarquee, sizeof(s_pathMarquee), tail);
        n = tv_slen(s_pathMarquee);

        for (i = 0; i < TV_MARQUEE_GAP && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = ' ';

        s_pathMarquee[n] = 0;

        for (i = 0; s_path[i] && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = s_path[i];

        s_pathMarquee[n] = 0;
    }
    else
    {
        int gapPos = (int)pos - len;
        int gapLeft = TV_MARQUEE_GAP - gapPos;

        if (gapLeft < 0) gapLeft = 0;

        for (i = 0; i < gapLeft && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = ' ';

        for (i = 0; s_path[i] && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = s_path[i];

        s_pathMarquee[n] = 0;
    }

    return s_pathMarquee;
}

static char tv_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int tv_ext_match(const char* name, const char* ext)
{
    int nl;
    int el;
    int i;

    if (!name || !ext) return 0;
    nl = tv_slen(name);
    el = tv_slen(ext);
    if (nl < el) return 0;

    for (i = 0; i < el; ++i)
    {
        if (tv_lower(name[nl - el + i]) != tv_lower(ext[i]))
            return 0;
    }
    return 1;
}

int TextViewer_CanOpen(const char* filename)
{
    if (!filename || !filename[0]) return 0;

    return tv_ext_match(filename, ".txt") ||
        tv_ext_match(filename, ".csv") ||
        tv_ext_match(filename, ".cfg") ||
        tv_ext_match(filename, ".ini") ||
        tv_ext_match(filename, ".log") ||
        tv_ext_match(filename, ".ver");
}

static void tv_free(void)
{
    if (s_buf)
    {
        free(s_buf);
        s_buf = 0;
    }

    s_loadedBytes = 0;
    s_fileBytes = 0;
    s_truncated = 0;
    s_lineCount = 0;
    s_scroll = 0;
    s_repeatDir = 0;
    s_repeatAt = 0;
}

void TextViewer_Close(void)
{
    tv_free();
    s_active = 0;
    s_title[0] = 0;
    s_path[0] = 0;
    s_pathMarqueeStart = 0;
}

static void tv_add_line(DWORD off, DWORD len)
{
    while (len > TV_WRAP_COLS && s_lineCount < TV_MAX_LINES)
    {
        s_lines[s_lineCount].offset = off;
        s_lines[s_lineCount].length = TV_WRAP_COLS;
        ++s_lineCount;
        off += TV_WRAP_COLS;
        len -= TV_WRAP_COLS;
    }

    if (s_lineCount < TV_MAX_LINES)
    {
        s_lines[s_lineCount].offset = off;
        s_lines[s_lineCount].length = (WORD)len;
        ++s_lineCount;
    }
}

static void tv_build_lines(void)
{
    DWORD p = 0;

    s_lineCount = 0;

    if (!s_buf || s_loadedBytes == 0)
        return;

    while (p < s_loadedBytes && s_lineCount < TV_MAX_LINES)
    {
        DWORD start = p;
        DWORD len;

        while (p < s_loadedBytes && s_buf[p] != '\r' && s_buf[p] != '\n')
            ++p;

        len = p - start;
        tv_add_line(start, len);

        if (p < s_loadedBytes && s_buf[p] == '\r')
            ++p;
        if (p < s_loadedBytes && s_buf[p] == '\n')
            ++p;
    }

    if (p < s_loadedBytes)
        s_truncated = 1;
}

int TextViewer_Open(DDStorage* fs, const char* path, const char* filename)
{
    DDFileHandle h;
    DWORD size = 0;
    DWORD target;
    DWORD done = 0;

    TextViewer_Close();

    if (!fs || !path || !path[0] || !fs->open_read || !fs->read || !fs->close)
        return 0;

    h = fs->open_read(fs, path, &size);
    if (!h)
        return 0;

    s_fileBytes = size;
    target = size;
    if (target > TV_MAX_FILE_BYTES)
    {
        target = TV_MAX_FILE_BYTES;
        s_truncated = 1;
    }

    s_buf = (unsigned char*)malloc(target ? target : 1);
    if (!s_buf)
    {
        fs->close(fs, h);
        tv_free();
        return 0;
    }

    while (done < target)
    {
        DWORD want = target - done;
        DWORD got = 0;

        if (want > TV_READ_CHUNK)
            want = TV_READ_CHUNK;

        if (!fs->read(fs, h, s_buf + done, want, &got))
        {
            fs->close(fs, h);
            TextViewer_Close();
            return 0;
        }

        if (got == 0)
            break;

        done += got;
    }

    fs->close(fs, h);

    if (done != target)
    {
        TextViewer_Close();
        return 0;
    }

    s_loadedBytes = done;
    tv_build_lines();
    tv_scpy(s_title, sizeof(s_title), filename && filename[0] ? filename : "TEXT VIEWER");
    tv_scpy(s_path, sizeof(s_path), path);
    s_pathMarqueeStart = GetTickCount();
    s_scroll = 0;
    s_repeatDir = 0;
    s_repeatAt = 0;
    s_active = 1;
    return 1;
}

int TextViewer_IsActive(void)
{
    return s_active;
}

static int tv_visible_rows(void)
{
    int lineH = Font_LineHeight(FONT_SIZE_SMALL) + 2;
    int rows;

    if (lineH < 1) lineH = 14;
    rows = 274 / lineH;
    if (rows < 8) rows = 8;
    if (rows > 22) rows = 22;
    return rows;
}

static void tv_clamp_scroll(void)
{
    int rows = tv_visible_rows();
    int maxScroll = s_lineCount - rows;

    if (maxScroll < 0) maxScroll = 0;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > maxScroll) s_scroll = maxScroll;
}

static int tv_repeat_nav(WORD pressed, WORD held)
{
    int dir = 0;
    DWORD now = GetTickCount();

    if (pressed & BTN_DPAD_UP) dir = -1;
    else if (pressed & BTN_DPAD_DOWN) dir = 1;

    if (dir)
    {
        s_repeatDir = dir;
        s_repeatAt = now + TV_REPEAT_DELAY;
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
        s_repeatAt = now + TV_REPEAT_DELAY;
        return 0;
    }

    if ((LONG)(now - s_repeatAt) >= 0)
    {
        s_repeatAt = now + TV_REPEAT_STEP;
        return dir;
    }

    return 0;
}

void TextViewer_Update(WORD pressed, WORD held)
{
    int dir;
    int rows;

    if (!s_active) return;

    if ((pressed & BTN_B) || (pressed & BTN_BACK))
    {
        TextViewer_Close();
        USB2XB_AudioPlay(U2X_SOUND_BACK);
        return;
    }

    dir = tv_repeat_nav(pressed, held);
    if (dir)
    {
        s_scroll += dir;
        tv_clamp_scroll();
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }

    rows = tv_visible_rows();

    if (pressed & BTN_LTRIG)
    {
        s_scroll -= rows - 1;
        tv_clamp_scroll();
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }

    if (pressed & BTN_RTRIG)
    {
        s_scroll += rows - 1;
        tv_clamp_scroll();
        USB2XB_AudioPlay(U2X_SOUND_NAV);
    }
}

static void tv_outline(float x, float y, float w, float h, DWORD c)
{
    UI_FillRect(x, y, w, 1, c);
    UI_FillRect(x, y + h - 1, w, 1, c);
    UI_FillRect(x, y, 1, h, c);
    UI_FillRect(x + w - 1, y, 1, h, c);
}

static void tv_u32(DWORD v, char* out, int cap)
{
    char tmp[16];
    int n = 0;
    int i;

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

    i = 0;
    while (n > 0 && i < cap - 1)
        out[i++] = tmp[--n];
    out[i] = 0;
}

static void tv_draw_line(IDirect3DDevice8* d, int idx, float x, float y, int maxW)
{
    char line[TV_WRAP_COLS + 1];
    int i;
    int n;
    DWORD off;

    if (idx < 0 || idx >= s_lineCount || !s_buf) return;

    off = s_lines[idx].offset;
    n = (int)s_lines[idx].length;
    if (n > TV_WRAP_COLS) n = TV_WRAP_COLS;

    for (i = 0; i < n; ++i)
    {
        unsigned char c = s_buf[off + i];
        if (c == '\t') c = ' ';
        else if (c < 32) c = ' ';
        line[i] = (char)c;
    }
    line[n] = 0;

    Font_DrawText(d, x + 1, y + 1, line, FONT_SIZE_SMALL,
        FONT_RGBA(0, 0, 0, 170), maxW);
    Font_DrawText(d, x, y, line, FONT_SIZE_SMALL, FONT_WHITE, maxW);
}

void TextViewer_Render(void)
{
    IDirect3DDevice8* d;
    int vw;
    int x;
    int y = 70;
    int w;
    int h = 356;
    int lineH;
    int rows;
    int i;
    char pos[64];
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
    tv_outline((float)x, (float)y, (float)w, (float)h, UI_ARGB(255, 96, 69, 126));
    tv_outline((float)(x + 2), (float)(y + 2), (float)(w - 4), (float)(h - 4), UI_ARGB(65, 218, 147, 255));
    UI_FillRect((float)x, (float)y, (float)w, 4, UI_ARGB(255, 194, 80, 255));

    Font_DrawTextEllipsis(d, (float)(x + 14), (float)(y + 13),
        s_title[0] ? s_title : "TEXT VIEWER",
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
        pathText = tv_marquee_path(pathW);

        if (Font_MeasureText(s_path, FONT_SIZE_SMALL) > pathW)
        {
            Font_DrawText(d, (float)(x + 14), (float)(y + 36),
                pathText, FONT_SIZE_SMALL,
                FONT_RGBA(154, 139, 173, 255), pathW);
        }
        else
        {
            Font_DrawTextEllipsis(d, (float)(x + 14), (float)(y + 36),
                pathText, FONT_SIZE_SMALL,
                FONT_RGBA(154, 139, 173, 255), pathW);
        }

        Font_DrawTextRight(d, (float)(x + w - 14), (float)(y + 36),
            statusText, FONT_SIZE_SMALL, statusColour);
    }

    UI_FillRect((float)(x + 14), (float)(y + 54), (float)(w - 28), 1,
        UI_ARGB(80, 194, 80, 255));

    lineH = Font_LineHeight(FONT_SIZE_SMALL) + 2;
    if (lineH < 1) lineH = 14;
    rows = tv_visible_rows();

    if (s_lineCount == 0)
    {
        Font_DrawText(d, (float)(x + 18), (float)(y + 74),
            "(empty file)", FONT_SIZE_SMALL,
            FONT_RGBA(170, 170, 170, 255), w - 36);
    }
    else
    {
        for (i = 0; i < rows; ++i)
        {
            int idx = s_scroll + i;
            if (idx >= s_lineCount) break;
            tv_draw_line(d, idx,
                (float)(x + 18),
                (float)(y + 68 + i * lineH),
                w - 36);
        }
    }

    UI_FillRect((float)(x + 14), (float)(y + h - 32), (float)(w - 28), 1,
        UI_ARGB(255, 52, 42, 67));

    pos[0] = 0;
    tv_u32((DWORD)(s_lineCount ? s_scroll + 1 : 0), a, sizeof(a));
    tv_u32((DWORD)s_lineCount, b, sizeof(b));
    tv_scpy(pos, sizeof(pos), "LINE ");
    {
        int n = tv_slen(pos);
        int j = 0;
        while (a[j] && n < (int)sizeof(pos) - 1) pos[n++] = a[j++];
        if (n < (int)sizeof(pos) - 1) pos[n++] = ' ';
        if (n < (int)sizeof(pos) - 1) pos[n++] = '/';
        if (n < (int)sizeof(pos) - 1) pos[n++] = ' ';
        j = 0;
        while (b[j] && n < (int)sizeof(pos) - 1) pos[n++] = b[j++];
        pos[n] = 0;
    }

    Font_DrawText(d, (float)(x + 14), (float)(y + h - 23),
        pos, FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 180);

    Font_DrawTextRight(d, (float)(vw - 20), 454,
        "DPAD SCROLL    LT / RT PAGE    B BACK",
        FONT_SIZE_SMALL, FONT_RGBA(168, 154, 187, 255));
}
