/*---------------------------------------------------------------------------
    usb2xb_audio.cpp -- small resident UI sound engine for USB2XB.

    Assets:
      D:\dat\nav.dat
      D:\dat\confirm.dat
      D:\dat\back.dat
      D:\dat\error.dat
      D:\dat\complete.dat

    Each .dat contains ordinary MP3 data.  Assets are decoded once at startup
    with minimp3, copied into resident DirectSound buffers, then played without
    any file I/O or MP3 decode in the navigation path.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <dsound.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#include "usb2xb_audio.h"

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif

#define U2X_AUDIO_MAX_FILE_BYTES   (512*1024)
#define U2X_AUDIO_MAX_PCM_SAMPLES  (44100*2*2) /* 2 sec stereo safety cap */

typedef struct
{
    LPDIRECTSOUNDBUFFER buffer;
} U2xSound;

static LPDIRECTSOUND s_ds = 0;
static U2xSound s_sound[U2X_SOUND_COUNT];
static int s_init = 0;

static const char* const s_paths[U2X_SOUND_COUNT] =
{
    "D:\\dat\\nav.dat",
    "D:\\dat\\confirm.dat",
    "D:\\dat\\back.dat",
    "D:\\dat\\error.dat",
    "D:\\dat\\complete.dat"
};

/*
    Millibels.  Keep navigation slightly quieter than explicit feedback.
    These are intentionally conservative and can be tuned later.
*/
static const LONG s_volume[U2X_SOUND_COUNT] =
{
    -900, /* nav */
    -650, /* confirm */
    -800, /* back */
    -450, /* error */
    -550  /* complete */
};


static int read_file(
    const char* path,
    unsigned char** outData,
    DWORD* outSize)
{
    HANDLE h;
    DWORD size;
    DWORD got = 0;
    unsigned char* data;

    if (outData)*outData = 0;
    if (outSize)*outSize = 0;

    if (!path || !outData || !outSize)
        return 0;

    h = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        0);

    if (h == INVALID_HANDLE_VALUE)
        return 0;

    size = GetFileSize(h, 0);

    if (size == 0 ||
        size == 0xFFFFFFFF ||
        size > U2X_AUDIO_MAX_FILE_BYTES)
    {
        CloseHandle(h);
        return 0;
    }

    data = (unsigned char*)malloc(size);

    if (!data)
    {
        CloseHandle(h);
        return 0;
    }

    if (!ReadFile(
        h,
        data,
        size,
        &got,
        0) ||
        got != size)
    {
        free(data);
        CloseHandle(h);
        return 0;
    }

    CloseHandle(h);

    *outData = data;
    *outSize = size;
    return 1;
}


static int decode_mp3(
    const unsigned char* data,
    DWORD bytes,
    short** outPcm,
    DWORD* outPcmBytes,
    int* outChannels,
    int* outHz)
{
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    short frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    short* pcm = 0;
    DWORD capSamples = 0;
    DWORD usedSamples = 0;
    DWORD pos = 0;
    int channels = 0;
    int hz = 0;

    if (outPcm)*outPcm = 0;
    if (outPcmBytes)*outPcmBytes = 0;
    if (outChannels)*outChannels = 0;
    if (outHz)*outHz = 0;

    if (!data ||
        !bytes ||
        !outPcm ||
        !outPcmBytes ||
        !outChannels ||
        !outHz)
    {
        return 0;
    }

    mp3dec_init(&dec);

    while (pos < bytes)
    {
        int samples;
        DWORD remain = bytes - pos;

        ZeroMemory(&info, sizeof(info));

        samples = mp3dec_decode_frame(
            &dec,
            data + pos,
            (int)remain,
            frame,
            &info);

        if (info.frame_bytes <= 0)
        {
            /*
                Skip junk/metadata one byte at a time.  UI assets are tiny,
                and this avoids an infinite loop on malformed input.
            */
            ++pos;
            continue;
        }

        pos += (DWORD)info.frame_bytes;

        if (samples <= 0)
            continue;

        if (info.channels <= 0 ||
            info.channels > 2 ||
            info.hz <= 0)
        {
            free(pcm);
            return 0;
        }

        if (channels == 0)
        {
            channels = info.channels;
            hz = info.hz;
        }
        else if (channels != info.channels ||
            hz != info.hz)
        {
            /* A UI effect should not change format mid-file. */
            free(pcm);
            return 0;
        }

        {
            DWORD add = (DWORD)samples * (DWORD)channels;
            DWORD need = usedSamples + add;

            if (need > U2X_AUDIO_MAX_PCM_SAMPLES)
            {
                free(pcm);
                return 0;
            }

            if (need > capSamples)
            {
                DWORD next = capSamples ? capSamples * 2 : 8192;
                short* grown;

                while (next < need)
                    next *= 2;

                if (next > U2X_AUDIO_MAX_PCM_SAMPLES)
                    next = U2X_AUDIO_MAX_PCM_SAMPLES;

                grown = (short*)realloc(
                    pcm,
                    (size_t)next * sizeof(short));

                if (!grown)
                {
                    free(pcm);
                    return 0;
                }

                pcm = grown;
                capSamples = next;
            }

            CopyMemory(
                pcm + usedSamples,
                frame,
                (size_t)add * sizeof(short));

            usedSamples = need;
        }
    }

    if (!pcm ||
        usedSamples == 0 ||
        channels == 0 ||
        hz == 0)
    {
        free(pcm);
        return 0;
    }

    *outPcm = pcm;
    *outPcmBytes = usedSamples * sizeof(short);
    *outChannels = channels;
    *outHz = hz;
    return 1;
}


static int create_buffer_from_pcm(
    const short* pcm,
    DWORD pcmBytes,
    int channels,
    int hz,
    LONG volume,
    LPDIRECTSOUNDBUFFER* outBuffer)
{
    WAVEFORMATEX wfx;
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buffer = 0;
    LPVOID p1 = 0;
    LPVOID p2 = 0;
    DWORD b1 = 0;
    DWORD b2 = 0;
    HRESULT hr;

    if (outBuffer)*outBuffer = 0;

    if (!s_ds ||
        !pcm ||
        !pcmBytes ||
        channels < 1 ||
        channels>2 ||
        hz <= 0 ||
        !outBuffer)
    {
        return 0;
    }

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)channels;
    wfx.nSamplesPerSec = (DWORD)hz;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(channels * 2);
    wfx.nAvgBytesPerSec =
        wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME;
    desc.dwBufferBytes = pcmBytes;
    desc.lpwfxFormat = &wfx;

    hr = IDirectSound_CreateSoundBuffer(
        s_ds,
        &desc,
        &buffer,
        0);

    if (FAILED(hr) || !buffer)
        return 0;

    hr = IDirectSoundBuffer_Lock(
        buffer,
        0,
        pcmBytes,
        &p1,
        &b1,
        &p2,
        &b2,
        0);

    if (FAILED(hr))
    {
        IDirectSoundBuffer_Release(buffer);
        return 0;
    }

    if (p1 && b1)
        CopyMemory(p1, pcm, b1);

    if (p2 && b2)
        CopyMemory(
            p2,
            ((const unsigned char*)pcm) + b1,
            b2);

    IDirectSoundBuffer_Unlock(
        buffer,
        p1, b1,
        p2, b2);

    IDirectSoundBuffer_SetVolume(
        buffer,
        volume);

    *outBuffer = buffer;
    return 1;
}


static int load_sound(
    int id,
    const char* path)
{
    unsigned char* file = 0;
    DWORD fileBytes = 0;
    short* pcm = 0;
    DWORD pcmBytes = 0;
    int channels = 0;
    int hz = 0;
    int ok = 0;

    if (id < 0 ||
        id >= U2X_SOUND_COUNT ||
        !path)
    {
        return 0;
    }

    if (!read_file(
        path,
        &file,
        &fileBytes))
    {
        return 0;
    }

    if (decode_mp3(
        file,
        fileBytes,
        &pcm,
        &pcmBytes,
        &channels,
        &hz))
    {
        ok = create_buffer_from_pcm(
            pcm,
            pcmBytes,
            channels,
            hz,
            s_volume[id],
            &s_sound[id].buffer);
    }

    free(pcm);
    free(file);
    return ok;
}


int USB2XB_AudioInit(void)
{
    int i;
    HRESULT hr;

    if (s_init)
        return s_ds ? 1 : 0;

    s_init = 1;
    ZeroMemory(s_sound, sizeof(s_sound));

    hr = DirectSoundCreate(
        0,
        &s_ds,
        0);

    if (FAILED(hr) || !s_ds)
    {
        s_ds = 0;
        return 0;
    }

    /*
        Individual missing/bad assets are intentionally non-fatal.  This also
        makes it easy to ship a partial sound pack while testing.
    */
    for (i = 0; i < U2X_SOUND_COUNT; ++i)
        load_sound(i, s_paths[i]);

    DirectSoundDoWork();
    return 1;
}


void USB2XB_AudioShutdown(void)
{
    int i;

    for (i = 0; i < U2X_SOUND_COUNT; ++i)
    {
        if (s_sound[i].buffer)
        {
            IDirectSoundBuffer_Release(
                s_sound[i].buffer);

            s_sound[i].buffer = 0;
        }
    }

    if (s_ds)
    {
        IDirectSound_Release(s_ds);
        s_ds = 0;
    }

    s_init = 0;
}


void USB2XB_AudioPump(void)
{
    if (s_ds)
        DirectSoundDoWork();
}


void USB2XB_AudioPlay(int soundId)
{
    LPDIRECTSOUNDBUFFER b;

    if (soundId < 0 ||
        soundId >= U2X_SOUND_COUNT)
    {
        return;
    }

    b = s_sound[soundId].buffer;

    if (!b)
        return;

    /*
        FROMSTART is important for cursor repeat: a new navigation event
        restarts the short tick immediately instead of waiting for the previous
        instance to reach its old play cursor.
    */
    IDirectSoundBuffer_Play(
        b,
        0,
        0,
        DSBPLAY_FROMSTART);
}
