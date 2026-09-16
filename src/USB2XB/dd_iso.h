#ifndef USB2XB_ISO_H
#define USB2XB_ISO_H

/* DarkDash orthographic isometric camera, retained for USB2XB. */

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

void Iso_SetAngles(float pitchDeg, float yawDeg);
void Iso_SetBreathe(float dPitch, float dYaw);
void Iso_GetAngles(float* pitchDeg, float* yawDeg);
void Iso_NudgeAngles(float dPitch, float dYaw);

void Iso_Begin(void);
void Iso_End(void);

void Iso_FillRect(float vx, float vy, float vw, float vh,
                  DWORD colour, int additive);

typedef struct {
    float vx;
    float vy;
    DWORD colour;
} IsoStripPt;

void Iso_DrawStrip(const IsoStripPt* pts, int n, int additive);
void Iso_Project(float vx, float vy, float* outVx, float* outVy);

#ifdef __cplusplus
}
#endif

#endif
