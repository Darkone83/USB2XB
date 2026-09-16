#ifndef USB2XB_AUDIO_H
#define USB2XB_AUDIO_H

enum
{
    U2X_SOUND_NAV = 0,
    U2X_SOUND_CONFIRM,
    U2X_SOUND_BACK,
    U2X_SOUND_ERROR,
    U2X_SOUND_COMPLETE,
    U2X_SOUND_COUNT
};

/*
    UI audio is optional.  Init failure or a missing individual asset simply
    leaves that sound silent; it must never prevent USB2XB from running.
*/
int  USB2XB_AudioInit(void);
void USB2XB_AudioShutdown(void);
void USB2XB_AudioPump(void);
void USB2XB_AudioPlay(int soundId);

#endif
#pragma once
