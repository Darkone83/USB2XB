#include <xtl.h>

#include "dd_checksum.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "font.h"
#include "input.h"
#include "usb2xb_audio.h"

#define CK_READ_CHUNK      (32UL * 1024UL)
#define CK_FRAME_MAX       (512UL * 1024UL)
#define CK_FRAME_MS        10UL
#define CK_MARQUEE_STEP_MS 115UL
#define CK_MARQUEE_HOLD_STEPS 7UL
#define CK_MARQUEE_GAP 4

/* ========================================================================== */
/* Small streaming CRC32 + MD5 implementation                                 */
/* ========================================================================== */

static DWORD s_crcTable[256];
static int   s_crcTableReady = 0;

static void crc32_init_table(void)
{
    DWORD i;

    if (s_crcTableReady)
        return;

    for (i = 0; i < 256; ++i)
    {
        DWORD c = i;
        int bit;

        for (bit = 0; bit < 8; ++bit)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);

        s_crcTable[i] = c;
    }

    s_crcTableReady = 1;
}

static DWORD crc32_update(DWORD crc, const unsigned char* data, DWORD bytes)
{
    DWORD i;

    for (i = 0; i < bytes; ++i)
        crc = s_crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);

    return crc;
}

typedef struct _CK_MD5
{
    DWORD state[4];
    DWORD count[2];
    unsigned char buffer[64];
} CK_MD5;

#define CK_MD5_F(x,y,z) (((x) & (y)) | ((~(x)) & (z)))
#define CK_MD5_G(x,y,z) (((x) & (z)) | ((y) & (~(z))))
#define CK_MD5_H(x,y,z) ((x) ^ (y) ^ (z))
#define CK_MD5_I(x,y,z) ((y) ^ ((x) | (~(z))))
#define CK_MD5_ROT(x,n) (((x) << (n)) | ((x) >> (32 - (n))))

#define CK_MD5_FF(a,b,c,d,x,s,ac) \
    { (a) += CK_MD5_F((b),(c),(d)) + (x) + (DWORD)(ac); \
      (a) = CK_MD5_ROT((a),(s)); (a) += (b); }
#define CK_MD5_GG(a,b,c,d,x,s,ac) \
    { (a) += CK_MD5_G((b),(c),(d)) + (x) + (DWORD)(ac); \
      (a) = CK_MD5_ROT((a),(s)); (a) += (b); }
#define CK_MD5_HH(a,b,c,d,x,s,ac) \
    { (a) += CK_MD5_H((b),(c),(d)) + (x) + (DWORD)(ac); \
      (a) = CK_MD5_ROT((a),(s)); (a) += (b); }
#define CK_MD5_II(a,b,c,d,x,s,ac) \
    { (a) += CK_MD5_I((b),(c),(d)) + (x) + (DWORD)(ac); \
      (a) = CK_MD5_ROT((a),(s)); (a) += (b); }

static void md5_encode(unsigned char* out, const DWORD* in, int words)
{
    int i;
    int j = 0;

    for (i = 0; i < words; ++i)
    {
        DWORD v = in[i];
        out[j++] = (unsigned char)(v & 0xFF);
        out[j++] = (unsigned char)((v >> 8) & 0xFF);
        out[j++] = (unsigned char)((v >> 16) & 0xFF);
        out[j++] = (unsigned char)((v >> 24) & 0xFF);
    }
}

static void md5_decode(DWORD* out, const unsigned char* in, int bytes)
{
    int i;
    int j = 0;

    for (i = 0; j < bytes; ++i, j += 4)
    {
        out[i] =
            ((DWORD)in[j]) |
            ((DWORD)in[j + 1] << 8) |
            ((DWORD)in[j + 2] << 16) |
            ((DWORD)in[j + 3] << 24);
    }
}

static void md5_transform(DWORD state[4], const unsigned char block[64])
{
    DWORD a = state[0];
    DWORD b = state[1];
    DWORD c = state[2];
    DWORD d = state[3];
    DWORD x[16];

    md5_decode(x, block, 64);

    CK_MD5_FF(a, b, c, d, x[0], 7, 0xd76aa478UL);
    CK_MD5_FF(d, a, b, c, x[1], 12, 0xe8c7b756UL);
    CK_MD5_FF(c, d, a, b, x[2], 17, 0x242070dbUL);
    CK_MD5_FF(b, c, d, a, x[3], 22, 0xc1bdceeeUL);
    CK_MD5_FF(a, b, c, d, x[4], 7, 0xf57c0fafUL);
    CK_MD5_FF(d, a, b, c, x[5], 12, 0x4787c62aUL);
    CK_MD5_FF(c, d, a, b, x[6], 17, 0xa8304613UL);
    CK_MD5_FF(b, c, d, a, x[7], 22, 0xfd469501UL);
    CK_MD5_FF(a, b, c, d, x[8], 7, 0x698098d8UL);
    CK_MD5_FF(d, a, b, c, x[9], 12, 0x8b44f7afUL);
    CK_MD5_FF(c, d, a, b, x[10], 17, 0xffff5bb1UL);
    CK_MD5_FF(b, c, d, a, x[11], 22, 0x895cd7beUL);
    CK_MD5_FF(a, b, c, d, x[12], 7, 0x6b901122UL);
    CK_MD5_FF(d, a, b, c, x[13], 12, 0xfd987193UL);
    CK_MD5_FF(c, d, a, b, x[14], 17, 0xa679438eUL);
    CK_MD5_FF(b, c, d, a, x[15], 22, 0x49b40821UL);

    CK_MD5_GG(a, b, c, d, x[1], 5, 0xf61e2562UL);
    CK_MD5_GG(d, a, b, c, x[6], 9, 0xc040b340UL);
    CK_MD5_GG(c, d, a, b, x[11], 14, 0x265e5a51UL);
    CK_MD5_GG(b, c, d, a, x[0], 20, 0xe9b6c7aaUL);
    CK_MD5_GG(a, b, c, d, x[5], 5, 0xd62f105dUL);
    CK_MD5_GG(d, a, b, c, x[10], 9, 0x02441453UL);
    CK_MD5_GG(c, d, a, b, x[15], 14, 0xd8a1e681UL);
    CK_MD5_GG(b, c, d, a, x[4], 20, 0xe7d3fbc8UL);
    CK_MD5_GG(a, b, c, d, x[9], 5, 0x21e1cde6UL);
    CK_MD5_GG(d, a, b, c, x[14], 9, 0xc33707d6UL);
    CK_MD5_GG(c, d, a, b, x[3], 14, 0xf4d50d87UL);
    CK_MD5_GG(b, c, d, a, x[8], 20, 0x455a14edUL);
    CK_MD5_GG(a, b, c, d, x[13], 5, 0xa9e3e905UL);
    CK_MD5_GG(d, a, b, c, x[2], 9, 0xfcefa3f8UL);
    CK_MD5_GG(c, d, a, b, x[7], 14, 0x676f02d9UL);
    CK_MD5_GG(b, c, d, a, x[12], 20, 0x8d2a4c8aUL);

    CK_MD5_HH(a, b, c, d, x[5], 4, 0xfffa3942UL);
    CK_MD5_HH(d, a, b, c, x[8], 11, 0x8771f681UL);
    CK_MD5_HH(c, d, a, b, x[11], 16, 0x6d9d6122UL);
    CK_MD5_HH(b, c, d, a, x[14], 23, 0xfde5380cUL);
    CK_MD5_HH(a, b, c, d, x[1], 4, 0xa4beea44UL);
    CK_MD5_HH(d, a, b, c, x[4], 11, 0x4bdecfa9UL);
    CK_MD5_HH(c, d, a, b, x[7], 16, 0xf6bb4b60UL);
    CK_MD5_HH(b, c, d, a, x[10], 23, 0xbebfbc70UL);
    CK_MD5_HH(a, b, c, d, x[13], 4, 0x289b7ec6UL);
    CK_MD5_HH(d, a, b, c, x[0], 11, 0xeaa127faUL);
    CK_MD5_HH(c, d, a, b, x[3], 16, 0xd4ef3085UL);
    CK_MD5_HH(b, c, d, a, x[6], 23, 0x04881d05UL);
    CK_MD5_HH(a, b, c, d, x[9], 4, 0xd9d4d039UL);
    CK_MD5_HH(d, a, b, c, x[12], 11, 0xe6db99e5UL);
    CK_MD5_HH(c, d, a, b, x[15], 16, 0x1fa27cf8UL);
    CK_MD5_HH(b, c, d, a, x[2], 23, 0xc4ac5665UL);

    CK_MD5_II(a, b, c, d, x[0], 6, 0xf4292244UL);
    CK_MD5_II(d, a, b, c, x[7], 10, 0x432aff97UL);
    CK_MD5_II(c, d, a, b, x[14], 15, 0xab9423a7UL);
    CK_MD5_II(b, c, d, a, x[5], 21, 0xfc93a039UL);
    CK_MD5_II(a, b, c, d, x[12], 6, 0x655b59c3UL);
    CK_MD5_II(d, a, b, c, x[3], 10, 0x8f0ccc92UL);
    CK_MD5_II(c, d, a, b, x[10], 15, 0xffeff47dUL);
    CK_MD5_II(b, c, d, a, x[1], 21, 0x85845dd1UL);
    CK_MD5_II(a, b, c, d, x[8], 6, 0x6fa87e4fUL);
    CK_MD5_II(d, a, b, c, x[15], 10, 0xfe2ce6e0UL);
    CK_MD5_II(c, d, a, b, x[6], 15, 0xa3014314UL);
    CK_MD5_II(b, c, d, a, x[13], 21, 0x4e0811a1UL);
    CK_MD5_II(a, b, c, d, x[4], 6, 0xf7537e82UL);
    CK_MD5_II(d, a, b, c, x[11], 10, 0xbd3af235UL);
    CK_MD5_II(c, d, a, b, x[2], 15, 0x2ad7d2bbUL);
    CK_MD5_II(b, c, d, a, x[9], 21, 0xeb86d391UL);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(CK_MD5* ctx)
{
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    ctx->state[0] = 0x67452301UL;
    ctx->state[1] = 0xefcdab89UL;
    ctx->state[2] = 0x98badcfeUL;
    ctx->state[3] = 0x10325476UL;
}

static void md5_update(CK_MD5* ctx, const unsigned char* input, DWORD inputLen)
{
    DWORD i = 0;
    DWORD index = (ctx->count[0] >> 3) & 0x3F;
    DWORD bits = inputLen << 3;
    DWORD partLen;

    ctx->count[0] += bits;
    if (ctx->count[0] < bits)
        ++ctx->count[1];
    ctx->count[1] += (inputLen >> 29);

    partLen = 64 - index;

    if (inputLen >= partLen)
    {
        CopyMemory(ctx->buffer + index, input, partLen);
        md5_transform(ctx->state, ctx->buffer);

        for (i = partLen; i + 63 < inputLen; i += 64)
            md5_transform(ctx->state, input + i);

        index = 0;
    }

    if (i < inputLen)
        CopyMemory(ctx->buffer + index, input + i, inputLen - i);
}

static void md5_final(unsigned char digest[16], CK_MD5* ctx)
{
    static const unsigned char padding[64] = { 0x80 };
    unsigned char bits[8];
    DWORD index;
    DWORD padLen;

    md5_encode(bits, ctx->count, 2);

    index = (ctx->count[0] >> 3) & 0x3F;
    padLen = (index < 56) ? (56 - index) : (120 - index);

    md5_update(ctx, padding, padLen);
    md5_update(ctx, bits, 8);
    md5_encode(digest, ctx->state, 4);
}

/* ========================================================================== */
/* Viewer state                                                               */
/* ========================================================================== */

static int s_active = 0;
static int s_finished = 0;
static int s_error = 0;
static DDStorage* s_fs = 0;
static DDFileHandle s_file = 0;
static DWORD s_total = 0;
static DWORD s_done = 0;
static DWORD s_crc = 0xFFFFFFFFUL;
static DWORD s_crcFinal = 0;
static CK_MD5 s_md5;
static unsigned char s_md5Digest[16];
static unsigned char s_readBuf[CK_READ_CHUNK];
static char s_title[DD_NAME_MAX];
static char s_path[DD_PATH_MAX];
static char s_pathMarquee[DD_PATH_MAX * 2 + CK_MARQUEE_GAP + 8];
static DWORD s_pathMarqueeStart = 0;
static char s_crcText[9];
static char s_md5Text[33];

static void ck_scpy(char* dst, int cap, const char* src)
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

static int ck_slen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

static const char* ck_marquee_path(int maxW)
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

    step = (GetTickCount() - s_pathMarqueeStart) / CK_MARQUEE_STEP_MS;
    len = ck_slen(s_path);
    if (len <= 0) return s_path;

    cycle = CK_MARQUEE_HOLD_STEPS + (DWORD)len + CK_MARQUEE_GAP;
    pos = step % cycle;
    if (pos < CK_MARQUEE_HOLD_STEPS) return s_path;
    pos -= CK_MARQUEE_HOLD_STEPS;
    s_pathMarquee[0] = 0;

    if (pos < (DWORD)len)
    {
        ck_scpy(s_pathMarquee, sizeof(s_pathMarquee), s_path + (int)pos);
        n = ck_slen(s_pathMarquee);
        for (i = 0; i < CK_MARQUEE_GAP && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = ' ';
        for (i = 0; s_path[i] && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = s_path[i];
        s_pathMarquee[n] = 0;
    }
    else
    {
        int gapLeft = CK_MARQUEE_GAP - ((int)pos - len);
        if (gapLeft < 0) gapLeft = 0;
        for (i = 0; i < gapLeft && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = ' ';
        for (i = 0; s_path[i] && n < (int)sizeof(s_pathMarquee) - 1; ++i)
            s_pathMarquee[n++] = s_path[i];
        s_pathMarquee[n] = 0;
    }

    return s_pathMarquee;
}

static char ck_hex(unsigned int v)
{
    v &= 0x0F;
    return (char)(v < 10 ? ('0' + v) : ('A' + (v - 10)));
}

static void ck_hex32(DWORD v, char out[9])
{
    int i;
    for (i = 0; i < 8; ++i)
        out[i] = ck_hex((unsigned int)(v >> (28 - i * 4)));
    out[8] = 0;
}

static void ck_md5_text(const unsigned char digest[16], char out[33])
{
    int i;
    int n = 0;
    for (i = 0; i < 16; ++i)
    {
        out[n++] = ck_hex(digest[i] >> 4);
        out[n++] = ck_hex(digest[i]);
    }
    out[n] = 0;
}

static void ck_u32(DWORD v, char* out, int cap)
{
    char tmp[16];
    int n = 0;
    int i = 0;

    if (!out || cap <= 0) return;

    if (!v)
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

static void ck_close_handle(void)
{
    if (s_file && s_fs && s_fs->close)
        s_fs->close(s_fs, s_file);
    s_file = 0;
}

static void ck_finish(void)
{
    if (s_finished || s_error)
        return;

    s_crcFinal = s_crc ^ 0xFFFFFFFFUL;
    md5_final(s_md5Digest, &s_md5);
    ck_hex32(s_crcFinal, s_crcText);
    ck_md5_text(s_md5Digest, s_md5Text);
    ck_close_handle();
    s_finished = 1;
}

static void ck_fail(void)
{
    ck_close_handle();
    s_error = 1;
}

void ChecksumViewer_Close(void)
{
    ck_close_handle();
    s_active = 0;
    s_finished = 0;
    s_error = 0;
    s_fs = 0;
    s_total = 0;
    s_done = 0;
    s_crc = 0xFFFFFFFFUL;
    s_crcFinal = 0;
    s_title[0] = 0;
    s_path[0] = 0;
    s_pathMarqueeStart = 0;
    s_crcText[0] = 0;
    s_md5Text[0] = 0;
}

int ChecksumViewer_Open(DDStorage* fs, const char* path, const char* filename)
{
    DWORD size = 0;

    ChecksumViewer_Close();

    if (!fs || !path || !path[0] || !fs->open_read || !fs->read || !fs->close)
        return 0;

    s_file = fs->open_read(fs, path, &size);
    if (!s_file)
        return 0;

    crc32_init_table();
    md5_init(&s_md5);

    s_fs = fs;
    s_total = size;
    s_done = 0;
    s_crc = 0xFFFFFFFFUL;
    ck_scpy(s_title, sizeof(s_title), filename && filename[0] ? filename : "CHECKSUMS");
    ck_scpy(s_path, sizeof(s_path), path);
    s_pathMarqueeStart = GetTickCount();
    s_active = 1;

    if (s_total == 0)
        ck_finish();

    return 1;
}

int ChecksumViewer_IsActive(void)
{
    return s_active;
}

void ChecksumViewer_Update(WORD pressed, WORD held)
{
    DWORD frameStart;
    DWORD frameBytes = 0;
    (void)held;

    if (!s_active)
        return;

    if ((pressed & BTN_B) || (pressed & BTN_BACK))
    {
        ChecksumViewer_Close();
        USB2XB_AudioPlay(U2X_SOUND_BACK);
        return;
    }

    if (s_finished || s_error)
        return;

    frameStart = GetTickCount();

    while (!s_finished && !s_error && frameBytes < CK_FRAME_MAX)
    {
        DWORD remain = s_total - s_done;
        DWORD want;
        DWORD got = 0;

        if (remain == 0)
        {
            ck_finish();
            USB2XB_AudioPlay(U2X_SOUND_COMPLETE);
            break;
        }

        if (frameBytes && (DWORD)(GetTickCount() - frameStart) >= CK_FRAME_MS)
            break;

        want = remain;
        if (want > CK_READ_CHUNK)
            want = CK_READ_CHUNK;

        if (!s_fs->read(s_fs, s_file, s_readBuf, want, &got))
        {
            ck_fail();
            USB2XB_AudioPlay(U2X_SOUND_ERROR);
            break;
        }

        if (got == 0)
        {
            ck_fail();
            USB2XB_AudioPlay(U2X_SOUND_ERROR);
            break;
        }

        s_crc = crc32_update(s_crc, s_readBuf, got);
        md5_update(&s_md5, s_readBuf, got);
        s_done += got;
        frameBytes += got;

        if (s_done >= s_total)
        {
            ck_finish();
            USB2XB_AudioPlay(U2X_SOUND_COMPLETE);
            break;
        }
    }
}

static void ck_outline(float x, float y, float w, float h, DWORD c)
{
    UI_FillRect(x, y, w, 1, c);
    UI_FillRect(x, y + h - 1, w, 1, c);
    UI_FillRect(x, y, 1, h, c);
    UI_FillRect(x + w - 1, y, 1, h, c);
}

void ChecksumViewer_Render(void)
{
    IDirect3DDevice8* d;
    int vw;
    int x;
    int y = 104;
    int w;
    int h = 276;
    float ratio;
    float barW;
    char sizeText[48];
    char doneText[16];
    char totalText[16];
    char pctText[16];
    int pct;

    if (!s_active)
        return;

    d = Gfx_Device();
    vw = (int)UI_Width();
    x = UI_IsWide() ? 74 : 46;
    w = vw - x * 2;

    UI_FillRect(0, 58, (float)vw, 386, UI_ARGB(126, 0, 0, 0));
    UI_FillRect((float)(x + 7), (float)(y + 8), (float)w, (float)h, UI_ARGB(78, 0, 0, 0));
    UI_FillRect((float)(x + 4), (float)(y + 5), (float)w, (float)h, UI_ARGB(110, 0, 0, 0));
    UI_FillRect((float)x, (float)y, (float)w, (float)h, UI_ARGB(238, 16, 11, 26));
    ck_outline((float)x, (float)y, (float)w, (float)h, UI_ARGB(255, 96, 69, 126));
    ck_outline((float)(x + 2), (float)(y + 2), (float)(w - 4), (float)(h - 4), UI_ARGB(65, 218, 147, 255));
    UI_FillRect((float)x, (float)y, (float)w, 4, UI_ARGB(255, 194, 80, 255));

    Font_DrawText(d, (float)(x + 18), (float)(y + 16),
        "CHECKSUMS", FONT_SIZE_MEDIUM, FONT_WHITE, 200);

    Font_DrawTextRight(d, (float)(x + w - 18), (float)(y + 18),
        s_error ? "READ ERROR" : (s_finished ? "COMPLETE" : "CALCULATING"),
        FONT_SIZE_SMALL,
        s_error ? FONT_RGBA(235, 117, 117, 255) :
        (s_finished ? FONT_RGBA(151, 218, 151, 255) : FONT_RGBA(181, 158, 204, 255)));

    Font_DrawTextEllipsis(d, (float)(x + 18), (float)(y + 45),
        s_title[0] ? s_title : "FILE",
        FONT_SIZE_SMALL, FONT_RGBA(218, 195, 237, 255), w - 36);

    {
        int pathW = w - 36;
        const char* pathText = ck_marquee_path(pathW);

        if (Font_MeasureText(s_path, FONT_SIZE_SMALL) > pathW)
            Font_DrawText(d, (float)(x + 18), (float)(y + 63),
                pathText, FONT_SIZE_SMALL,
                FONT_RGBA(145, 130, 163, 255), pathW);
        else
            Font_DrawTextEllipsis(d, (float)(x + 18), (float)(y + 63),
                pathText, FONT_SIZE_SMALL,
                FONT_RGBA(145, 130, 163, 255), pathW);
    }

    UI_FillRect((float)(x + 18), (float)(y + 80), (float)(w - 36), 1,
        UI_ARGB(80, 194, 80, 255));

    ratio = s_total ? ((float)s_done / (float)s_total) : 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    barW = (float)(w - 36);

    UI_FillRect((float)(x + 18), (float)(y + 91), barW, 12,
        UI_ARGB(255, 35, 27, 45));
    UI_FillRect((float)(x + 18), (float)(y + 91), barW * ratio, 12,
        UI_ARGB(220, 144, 73, 190));
    ck_outline((float)(x + 18), (float)(y + 91), barW, 12,
        UI_ARGB(175, 194, 80, 255));

    pct = s_total ? (int)(ratio * 100.0f + 0.5f) : 100;
    ck_u32((DWORD)pct, pctText, sizeof(pctText));
    {
        int n = 0;
        while (pctText[n]) ++n;
        if (n < (int)sizeof(pctText) - 1)
        {
            pctText[n++] = '%';
            pctText[n] = 0;
        }
    }

    ck_u32(s_done, doneText, sizeof(doneText));
    ck_u32(s_total, totalText, sizeof(totalText));
    ck_scpy(sizeText, sizeof(sizeText), doneText);
    {
        int n = 0;
        int j = 0;
        while (sizeText[n]) ++n;
        if (n < (int)sizeof(sizeText) - 1) sizeText[n++] = '/';
        while (totalText[j] && n < (int)sizeof(sizeText) - 1) sizeText[n++] = totalText[j++];
        if (n < (int)sizeof(sizeText) - 1) sizeText[n++] = ' ';
        if (n < (int)sizeof(sizeText) - 1) sizeText[n++] = 'B';
        sizeText[n] = 0;
    }

    Font_DrawText(d, (float)(x + 18), (float)(y + 112),
        sizeText, FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 220);
    Font_DrawTextRight(d, (float)(x + w - 18), (float)(y + 112),
        pctText, FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255));

    Font_DrawText(d, (float)(x + 18), (float)(y + 151),
        "CRC32", FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 80);
    Font_DrawText(d, (float)(x + 108), (float)(y + 151),
        s_finished ? s_crcText : "--------",
        FONT_SIZE_SMALL, FONT_WHITE, w - 126);

    Font_DrawText(d, (float)(x + 18), (float)(y + 187),
        "MD5", FONT_SIZE_SMALL, FONT_RGBA(174, 139, 207, 255), 80);
    Font_DrawText(d, (float)(x + 108), (float)(y + 187),
        s_finished ? s_md5Text : "--------------------------------",
        FONT_SIZE_SMALL, FONT_WHITE, w - 126);

    if (s_error)
    {
        Font_DrawText(d, (float)(x + 18), (float)(y + 222),
            "Unable to read the complete file.",
            FONT_SIZE_SMALL, FONT_RGBA(235, 117, 117, 255), w - 36);
    }
    else if (!s_finished)
    {
        Font_DrawText(d, (float)(x + 18), (float)(y + 222),
            "CRC32 and MD5 are calculated in one streaming pass.",
            FONT_SIZE_SMALL, FONT_RGBA(168, 154, 187, 255), w - 36);
    }
    else
    {
        Font_DrawText(d, (float)(x + 18), (float)(y + 222),
            "Checksum calculation complete.",
            FONT_SIZE_SMALL, FONT_RGBA(168, 154, 187, 255), w - 36);
    }

    Font_DrawTextRight(d, (float)(vw - 20), 454,
        s_finished || s_error ? "B BACK" : "B CANCEL",
        FONT_SIZE_SMALL, FONT_RGBA(168, 154, 187, 255));
}
