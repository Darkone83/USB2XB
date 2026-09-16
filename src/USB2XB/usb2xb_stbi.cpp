/*---------------------------------------------------------------------------
    usb2xb_stbi.c -- PNG decode for USB2XB via stb_image.

    Assets keep a .dat filename on disk, but they contain ordinary PNG bytes.
    stb_image detects the image from the file magic, not the extension.

    This mirrors DarkDash's RXDK-safe stb configuration, trimmed to PNG only.
---------------------------------------------------------------------------*/
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x) ((void)0)

#include "stb_image.h"
#include "usb2xb_stbi.h"

unsigned char* USB2XB_StbLoadImageMem(
    const unsigned char* data,
    int len,
    int* w,
    int* h)
{
    int comp = 0;

    if (!data || len <= 0 || !w || !h)
        return 0;

    return stbi_load_from_memory(
        data,
        len,
        w,
        h,
        &comp,
        4);
}

void USB2XB_StbFree(void* p)
{
    if (p)
        stbi_image_free(p);
}
