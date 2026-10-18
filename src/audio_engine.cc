#include "audio_engine.h"

#include <stdlib.h>
#include <string.h>

#include <mutex>

#include <SDL3/SDL.h>

namespace fallout {

#define AUDIO_ENGINE_SOUND_BUFFERS 8

struct AudioEngineSoundBuffer {
    bool active;
    unsigned int size;
    int bitsPerSample;
    int channels;
    int rate;
    void* data;
    int volume;
    bool playing;
    bool looping;
    unsigned int pos;
    SDL_AudioStream* stream;
    std::recursive_mutex mutex;
};

extern bool GNW95_isActive;

// SDL3 dropped SDL_MIX_MAXVOLUME; the game still expresses sound buffer volume
// on the 0..128 scale it used, so keep the constant locally and scale to the
// [0.0..1.0] SDL_MixAudio now expects.
#define AUDIO_ENGINE_MAX_VOLUME 128

// The audio callback runs on an SDL worker thread. The nxdk backend gives those
// threads a single page of stack, so both mixing buffers live on the heap
// instead of on that thread's stack.
#define AUDIO_ENGINE_MIX_BUFFER_SIZE 8192
#define AUDIO_ENGINE_CONVERT_BUFFER_SIZE 1024

// How much source data is handed to a sound's converter at a time. sound.cc
// reads soundBuffer->pos back to decide when to refill streaming sections and
// when a track has finished, so the cursor must not run far ahead of what is
// audible - but one put still has to cover many output bytes to stay cheap.
#define AUDIO_ENGINE_FEED_CHUNK_SIZE 512

static bool soundBufferIsValid(int soundBufferIndex);
static void SDLCALL audioEngineMixin(void* userData, SDL_AudioStream* stream, int additionalAmount, int totalAmount);

static SDL_AudioSpec gAudioEngineSpec;
static SDL_AudioStream* gAudioEngineStream = NULL;
static int gAudioEngineSilence = 0;
static Uint8* gAudioEngineMixBuffer = NULL;
static Uint8* gAudioEngineConvertBuffer = NULL;
static AudioEngineSoundBuffer gAudioEngineSoundBuffers[AUDIO_ENGINE_SOUND_BUFFERS];

static bool audioEngineIsInitialized()
{
    return gAudioEngineStream != NULL;
}

static bool soundBufferIsValid(int soundBufferIndex)
{
    return soundBufferIndex >= 0 && soundBufferIndex < AUDIO_ENGINE_SOUND_BUFFERS;
}

// Mixes all active sound buffers into `output`, which is `length` bytes of the
// device format. Mirrors the SDL2 audio callback this used to be.
static void audioEngineMixinChunk(Uint8* output, int length)
{
    memset(output, gAudioEngineSilence, length);

    if (!GNW95_isActive) {
        return;
    }

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[index]);
        std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

        if (soundBuffer->active && soundBuffer->playing) {
            int srcFrameSize = soundBuffer->bitsPerSample / 8 * soundBuffer->channels;

            unsigned char* buffer = gAudioEngineConvertBuffer;
            int pos = 0;

            // Set once the source data has run out, either because a one-shot
            // sound reached its end or defensively on a degenerate frame size.
            // The converter is still drained afterwards so the tail is heard.
            bool exhausted = false;

            while (pos < length) {
                int wanted = length - pos;
                if (wanted > AUDIO_ENGINE_CONVERT_BUFFER_SIZE) {
                    wanted = AUDIO_ENGINE_CONVERT_BUFFER_SIZE;
                }

                // Top the converter up in contiguous runs. Feeding it a single
                // frame per iteration - as this did originally - costs a locked
                // put/get pair for every few output bytes, which is far too
                // expensive to sustain once SDL is also resampling.
                while (!exhausted && SDL_GetAudioStreamAvailable(soundBuffer->stream) < wanted) {
                    unsigned int run = soundBuffer->size - soundBuffer->pos;
                    if (run > AUDIO_ENGINE_FEED_CHUNK_SIZE) {
                        run = AUDIO_ENGINE_FEED_CHUNK_SIZE;
                    }
                    run -= run % srcFrameSize;

                    if (run == 0) {
                        exhausted = true;
                        break;
                    }

                    SDL_PutAudioStreamData(soundBuffer->stream,
                        (unsigned char*)soundBuffer->data + soundBuffer->pos, run);
                    soundBuffer->pos += run;

                    if (soundBuffer->pos >= soundBuffer->size) {
                        if (soundBuffer->looping) {
                            soundBuffer->pos %= soundBuffer->size;
                        } else {
                            soundBuffer->playing = false;
                            exhausted = true;
                        }
                    }
                }

                // Availability was just ensured above, so nothing left to read
                // here really does mean this sound is finished for now.
                int bytesRead = SDL_GetAudioStreamData(soundBuffer->stream, buffer, wanted);
                if (bytesRead <= 0) {
                    break;
                }

                SDL_MixAudio(output + pos, buffer, gAudioEngineSpec.format, bytesRead,
                    (float)soundBuffer->volume / AUDIO_ENGINE_MAX_VOLUME);

                pos += bytesRead;
            }
        }
    }
}

static void SDLCALL audioEngineMixin(void* userData, SDL_AudioStream* stream, int additionalAmount, int totalAmount)
{
    (void)userData;
    (void)totalAmount;

    while (additionalAmount > 0) {
        int length = additionalAmount;
        if (length > AUDIO_ENGINE_MIX_BUFFER_SIZE) {
            length = AUDIO_ENGINE_MIX_BUFFER_SIZE;
        }

        audioEngineMixinChunk(gAudioEngineMixBuffer, length);
        SDL_PutAudioStreamData(stream, gAudioEngineMixBuffer, length);

        additionalAmount -= length;
    }
}

bool audioEngineInit()
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return false;
    }

    // SDL2's nxdk driver hardcoded a 1024 frame device buffer and ignored what
    // the app asked for, so this 4096 only started taking effect under SDL3.
    // Matching SDL2 exactly by asking for 1024 was tried and is audibly worse -
    // the music starves - so the extra headroom is doing real work here even
    // though it costs latency. Leave it at 4096 unless there is evidence to
    // move it.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "4096");

    gAudioEngineSpec.freq = 22050;
    gAudioEngineSpec.format = SDL_AUDIO_S16;
    gAudioEngineSpec.channels = 2;

    gAudioEngineMixBuffer = (Uint8*)malloc(AUDIO_ENGINE_MIX_BUFFER_SIZE);
    gAudioEngineConvertBuffer = (Uint8*)malloc(AUDIO_ENGINE_CONVERT_BUFFER_SIZE);
    if (gAudioEngineMixBuffer == NULL || gAudioEngineConvertBuffer == NULL) {
        free(gAudioEngineMixBuffer);
        gAudioEngineMixBuffer = NULL;
        free(gAudioEngineConvertBuffer);
        gAudioEngineConvertBuffer = NULL;
        return false;
    }

    gAudioEngineStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &gAudioEngineSpec, audioEngineMixin, NULL);
    if (gAudioEngineStream == NULL) {
        free(gAudioEngineMixBuffer);
        gAudioEngineMixBuffer = NULL;
        free(gAudioEngineConvertBuffer);
        gAudioEngineConvertBuffer = NULL;
        return false;
    }

    // SDL2 opened the device with SDL_AUDIO_ALLOW_ANY_CHANGE, so the mixer ran
    // in the hardware's own format and nothing had to be resampled on the audio
    // thread. SDL3 instead honours the spec we asked for and quietly converts,
    // which on Xbox means resampling every callback from 22050Hz to the AC97's
    // 48000Hz. Adopt the device format instead so that conversion disappears -
    // each sound's own stream then converts straight to the hardware rate.
    SDL_AudioSpec deviceSpec;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(gAudioEngineStream), &deviceSpec, NULL)
        && SDL_SetAudioStreamFormat(gAudioEngineStream, &deviceSpec, NULL)) {
        gAudioEngineSpec = deviceSpec;
    }

    gAudioEngineSilence = SDL_GetSilenceValueForFormat(gAudioEngineSpec.format);

    SDL_ResumeAudioStreamDevice(gAudioEngineStream);

    return true;
}

void audioEngineExit()
{
    if (audioEngineIsInitialized()) {
        // Also closes the device that was opened alongside the stream.
        SDL_DestroyAudioStream(gAudioEngineStream);
        gAudioEngineStream = NULL;

        free(gAudioEngineMixBuffer);
        gAudioEngineMixBuffer = NULL;
        free(gAudioEngineConvertBuffer);
        gAudioEngineConvertBuffer = NULL;
    }

    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void audioEnginePause()
{
    if (audioEngineIsInitialized()) {
        SDL_PauseAudioStreamDevice(gAudioEngineStream);
    }
}

void audioEngineResume()
{
    if (audioEngineIsInitialized()) {
        SDL_ResumeAudioStreamDevice(gAudioEngineStream);
    }
}

int audioEngineCreateSoundBuffer(unsigned int size, int bitsPerSample, int channels, int rate)
{
    if (!audioEngineIsInitialized()) {
        return -1;
    }

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[index]);
        std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

        if (!soundBuffer->active) {
            soundBuffer->active = true;
            soundBuffer->size = size;
            soundBuffer->bitsPerSample = bitsPerSample;
            soundBuffer->channels = channels;
            soundBuffer->rate = rate;
            soundBuffer->volume = AUDIO_ENGINE_MAX_VOLUME;
            soundBuffer->playing = false;
            soundBuffer->looping = false;
            soundBuffer->pos = 0;
            soundBuffer->data = malloc(size);
            SDL_AudioSpec srcSpec;
            srcSpec.format = bitsPerSample == 16 ? SDL_AUDIO_S16 : SDL_AUDIO_S8;
            srcSpec.channels = channels;
            srcSpec.freq = rate;
            soundBuffer->stream = SDL_CreateAudioStream(&srcSpec, &gAudioEngineSpec);
            return index;
        }
    }

    return -1;
}

bool audioEngineSoundBufferRelease(int soundBufferIndex)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->active = false;

    free(soundBuffer->data);
    soundBuffer->data = NULL;

    SDL_DestroyAudioStream(soundBuffer->stream);
    soundBuffer->stream = NULL;

    return true;
}

bool audioEngineSoundBufferSetVolume(int soundBufferIndex, int volume)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->volume = volume;

    return true;
}

bool audioEngineSoundBufferGetVolume(int soundBufferIndex, int* volumePtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    *volumePtr = soundBuffer->volume;

    return true;
}

bool audioEngineSoundBufferSetPan(int soundBufferIndex, int pan)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    // NOTE: Audio engine does not support sound panning. I'm not sure it's
    // even needed. For now this value is silently ignored.

    return true;
}

bool audioEngineSoundBufferPlay(int soundBufferIndex, unsigned int flags)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->playing = true;

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_PLAY_LOOPING) != 0) {
        soundBuffer->looping = true;
    }

    return true;
}

bool audioEngineSoundBufferStop(int soundBufferIndex)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->playing = false;

    // Don't let buffered audio from before the stop leak into a later replay.
    SDL_ClearAudioStream(soundBuffer->stream);

    return true;
}

bool audioEngineSoundBufferGetCurrentPosition(int soundBufferIndex, unsigned int* readPosPtr, unsigned int* writePosPtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (readPosPtr != NULL) {
        *readPosPtr = soundBuffer->pos;
    }

    if (writePosPtr != NULL) {
        *writePosPtr = soundBuffer->pos;

        if (soundBuffer->playing) {
            // 15 ms lead
            // See: https://docs.microsoft.com/en-us/previous-versions/windows/desktop/mt708925(v=vs.85)#remarks
            *writePosPtr += soundBuffer->rate / 150;
            *writePosPtr %= soundBuffer->size;
        }
    }

    return true;
}

bool audioEngineSoundBufferSetCurrentPosition(int soundBufferIndex, unsigned int pos)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->pos = pos % soundBuffer->size;

    // Whatever the converter still holds was produced from the old position -
    // playing it out after a seek is heard as audio arriving out of order.
    SDL_ClearAudioStream(soundBuffer->stream);

    return true;
}

bool audioEngineSoundBufferLock(int soundBufferIndex, unsigned int writePos, unsigned int writeBytes, void** audioPtr1, unsigned int* audioBytes1, void** audioPtr2, unsigned int* audioBytes2, unsigned int flags)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (audioBytes1 == NULL) {
        return false;
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_FROM_WRITE_POS) != 0) {
        if (!audioEngineSoundBufferGetCurrentPosition(soundBufferIndex, NULL, &writePos)) {
            return false;
        }
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_ENTIRE_BUFFER) != 0) {
        writeBytes = soundBuffer->size;
    }

    if (writePos + writeBytes <= soundBuffer->size) {
        *(unsigned char**)audioPtr1 = (unsigned char*)soundBuffer->data + writePos;
        *audioBytes1 = writeBytes;

        if (audioPtr2 != NULL) {
            *audioPtr2 = NULL;
        }

        if (audioBytes2 != NULL) {
            *audioBytes2 = 0;
        }
    } else {
        unsigned int remainder = writePos + writeBytes - soundBuffer->size;
        *(unsigned char**)audioPtr1 = (unsigned char*)soundBuffer->data + writePos;
        *audioBytes1 = soundBuffer->size - writePos;

        if (audioPtr2 != NULL) {
            *(unsigned char**)audioPtr2 = (unsigned char*)soundBuffer->data;
        }

        if (audioBytes2 != NULL) {
            *audioBytes2 = writeBytes - (soundBuffer->size - writePos);
        }
    }

    // TODO: Mark range as locked.

    return true;
}

bool audioEngineSoundBufferUnlock(int soundBufferIndex, void* audioPtr1, unsigned int audioBytes1, void* audioPtr2, unsigned int audioBytes2)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    // TODO: Mark range as unlocked.

    return true;
}

bool audioEngineSoundBufferGetStatus(int soundBufferIndex, unsigned int* statusPtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (statusPtr == NULL) {
        return false;
    }

    *statusPtr = 0;

    if (soundBuffer->playing) {
        *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_PLAYING;

        if (soundBuffer->looping) {
            *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_LOOPING;
        }
    }

    return true;
}

} // namespace fallout
