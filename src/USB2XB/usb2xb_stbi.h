#ifndef USB2XB_STBI_H
#define USB2XB_STBI_H

#ifdef __cplusplus
extern "C" {
#endif

    unsigned char* USB2XB_StbLoadImageMem(
        const unsigned char* data,
        int len,
        int* w,
        int* h);

    void USB2XB_StbFree(void* p);

#ifdef __cplusplus
}
#endif

#endif
