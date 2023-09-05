#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

#if RETRO_REV0U
#include "Legacy/AudioLegacy.cpp"
#endif

#ifdef RSDKv5_USE_LIBVORBIS

#include <vorbis/vorbisfile.h>

static struct VorbisMetadata
{
    OggVorbis_File file;
    const unsigned char *buffer;
    size_t size;
    size_t position;
} vorbisMetadata;

#else

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis/stb_vorbis.c"

stb_vorbis *vorbisInfo = NULL;
stb_vorbis_alloc vorbisAlloc;

#endif

SFXInfo RSDK::sfxList[SFX_COUNT];
ChannelInfo RSDK::channels[CHANNEL_COUNT];

char streamFilePath[0x40];
uint8 *streamBuffer    = NULL;
int32 streamBufferSize = 0;
uint32 streamStartPos  = 0;
int32 streamLoopPoint  = 0;

#ifdef RSDKv5_USE_LIBVORBIS
static size_t fread_wrapper(void *output, size_t size, size_t count, void *file)
{
    VorbisMetadata *vorbisMetadata = (VorbisMetadata*)file;

    count = MIN(count, (vorbisMetadata->size - vorbisMetadata->position) / size);

    memcpy(output, &vorbisMetadata->buffer[vorbisMetadata->position], count * size);

    vorbisMetadata->position += count * size;

    return count;
}

static int fseek_wrapper(void *file, ogg_int64_t offset, int origin)
{
    VorbisMetadata *vorbisMetadata = (VorbisMetadata*)file;

    switch (origin)
    {
        case SEEK_SET:
            vorbisMetadata->position = offset;
            break;

        case SEEK_CUR:
            vorbisMetadata->position += offset;
            break;

        case SEEK_END:
            vorbisMetadata->position = vorbisMetadata->size + offset;
            break;

        default:
            return -1;
    }

    return 0;
}

static long ftell_wrapper(void *file)
{
    VorbisMetadata *vorbisMetadata = (VorbisMetadata*)file;

    return vorbisMetadata->position;
}

static const ov_callbacks vorbisCallbacks = {
    fread_wrapper,
    fseek_wrapper,
    NULL,
    ftell_wrapper
};
#endif

#if RETRO_AUDIODEVICE_XAUDIO
#include "XAudio/XAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_SDL2
#include "SDL2/SDL2AudioDevice.cpp"
#elif RETRO_AUDIODEVICE_PORT
#include "PortAudio/PortAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_MINI
#include "MiniAudio/MiniAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_OBOE
#include "Oboe/OboeAudioDevice.cpp"
#endif

uint8 AudioDeviceBase::initializedAudioChannels = false;
uint8 AudioDeviceBase::audioState               = 0;
uint8 AudioDeviceBase::audioFocus               = 0;

int32 AudioDeviceBase::clampBuffer[MIX_BUFFER_SIZE];
void AudioDeviceBase::Release()
{
#ifdef RSDKv5_USE_LIBVORBIS
    ov_clear(&vorbisMetadata.file);
#else
    // This is missing, meaning that the garbage collector will never reclaim stb_vorbis's buffer.
#if !RETRO_USE_ORIGINAL_CODE
    stb_vorbis_close(vorbisInfo);
    vorbisInfo = NULL;
#endif
#endif
}

void AudioDeviceBase::ProcessAudioMixing(void *stream, int32 length)
{
    int16 *outputPointer = (int16 *)stream;

    for (int32 samplesRemaining = length, samplesToDo; samplesRemaining != 0; samplesRemaining -= samplesToDo)
    {
        samplesToDo = MIN(MIX_BUFFER_SIZE, samplesRemaining);

        int32 *streamF    = clampBuffer;
        int32 *streamEndF = clampBuffer + samplesToDo;

        memset(clampBuffer, 0, samplesToDo * sizeof(int32));

        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            ChannelInfo *channel = &channels[c];

            switch (channel->state) {
                default:
                case CHANNEL_IDLE: break;

                case CHANNEL_SFX: {
                    int16 *sfxBuffer = &channel->samplePtr[channel->bufferPos];

                    float volL = channel->volume, volR = channel->volume;
                    if (channel->pan < 0.0f)
                        volR = (1.0f + channel->pan) * channel->volume;
                    else
                        volL = (1.0f - channel->pan) * channel->volume;

                    int32 panL = (int32)(volL * engine.soundFXVolume * TO_FIXED(1));
                    int32 panR = (int32)(volR * engine.soundFXVolume * TO_FIXED(1));

                    uint32 speedPercent       = 0;
                    int32 *curStreamF = streamF;
                    while (curStreamF < streamEndF && streamF < streamEndF) {
                        // Perform linear interpolation.
                        int16 sample;
#if !RETRO_USE_ORIGINAL_CODE
                        if (!sfxBuffer) // PROTECTION FOR v5U (and other mysterious crashes 👻)
                            sample = 0;
                        else
#endif
                            sample = FROM_FIXED((sfxBuffer[1] - sfxBuffer[0]) * speedPercent) + sfxBuffer[0];

                        speedPercent += channel->speed;
                        sfxBuffer += FROM_FIXED(speedPercent);
                        channel->bufferPos += FROM_FIXED(speedPercent);
                        speedPercent %= TO_FIXED(1);

                        curStreamF[0] += FROM_FIXED(sample * panL);
                        curStreamF[1] += FROM_FIXED(sample * panR);
                        curStreamF += 2;

                        if (channel->bufferPos >= channel->sampleLength) {
                            if (channel->loop == (uint32)-1) {
                                channel->state   = CHANNEL_IDLE;
                                channel->soundID = -1;
                                break;
                            }
                            else {
                                channel->bufferPos -= (uint32)channel->sampleLength;
                                channel->bufferPos += channel->loop;

                                sfxBuffer = &channel->samplePtr[channel->bufferPos];
                            }
                        }
                    }

                    break;
                }

                case CHANNEL_STREAM: {
                    int16 *streamBuffer = &channel->samplePtr[channel->bufferPos];

                    float volL = channel->volume, volR = channel->volume;
                    if (channel->pan < 0.0f)
                        volR = (1.0f + channel->pan) * channel->volume;
                    else
                        volL = (1.0f - channel->pan) * channel->volume;

                    int32 panL = (int32)(volL * engine.streamVolume * TO_FIXED(1));
                    int32 panR = (int32)(volR * engine.streamVolume * TO_FIXED(1));

                    uint32 speedPercent       = 0;
                    int32 *curStreamF = streamF;
                    while (curStreamF < streamEndF && streamF < streamEndF) {
                        speedPercent += channel->speed;
                        int32 next = FROM_FIXED(speedPercent);
                        speedPercent %= TO_FIXED(1);

                        curStreamF[0] += FROM_FIXED(streamBuffer[0] * panL);
                        curStreamF[1] += FROM_FIXED(streamBuffer[1] * panR);
                        curStreamF += 2;

                        streamBuffer += next * 2;
                        channel->bufferPos += next * 2;

                        if (channel->bufferPos >= channel->sampleLength) {
                            channel->bufferPos -= (uint32)channel->sampleLength;

                            streamBuffer = &channel->samplePtr[channel->bufferPos];

                            UpdateStreamBuffer(channel);
                        }
                    }
                    break;
                }

                case CHANNEL_LOADING_STREAM: break;
            }
        }

        for (int32 i = 0; i < samplesToDo; ++i)
            *outputPointer++ = (int16)CLAMP(clampBuffer[i], -0x7FFF, 0x7FFF);
    }
}

void AudioDeviceBase::InitAudioChannels()
{
    for (int32 i = 0; i < CHANNEL_COUNT; ++i) {
        channels[i].soundID = -1;
        channels[i].state   = CHANNEL_IDLE;
    }

    GEN_HASH_MD5("Stream Channel 0", sfxList[SFX_COUNT - 1].hash);
    sfxList[SFX_COUNT - 1].scope              = SCOPE_GLOBAL;
    sfxList[SFX_COUNT - 1].maxConcurrentPlays = 1;
    sfxList[SFX_COUNT - 1].length             = MIX_BUFFER_SIZE;
    AllocateStorage((void **)&sfxList[SFX_COUNT - 1].buffer, MIX_BUFFER_SIZE * sizeof(int16), DATASET_MUS, false);

    initializedAudioChannels = true;
}

#ifdef RSDKv5_USE_LIBVORBIS
static inline bool32 IsBigEndian(void)
{
    union
    {
        uint16 integer;
        uint8 bytes[2];
    } test;

    test.integer = 1;

    return test.bytes[1] == 1;
}
#endif

void RSDK::UpdateStreamBuffer(ChannelInfo *channel)
{
    int32 bufferRemaining = MIX_BUFFER_SIZE;
    int16 *buffer         = channel->samplePtr;

    for (int32 s = 0; s < MIX_BUFFER_SIZE;) {
#ifdef RSDKv5_USE_LIBVORBIS
        int32 samples = ov_read(&vorbisMetadata.file, (char*)buffer, bufferRemaining * sizeof(int16), IsBigEndian(), 2, 1, NULL) / sizeof(int16);
#else
        int32 samples = stb_vorbis_get_samples_short_interleaved(vorbisInfo, 2, buffer, bufferRemaining) * 2;
#endif
        if (!samples) {
            if (channel->loop == 1 &&
#ifdef RSDKv5_USE_LIBVORBIS
                ov_pcm_seek(&vorbisMetadata.file, streamLoopPoint) == 0
#else
                stb_vorbis_seek_frame(vorbisInfo, streamLoopPoint)
#endif
                ) {
                // we're looping & the seek was successful, get more samples
            }
            else {
                channel->state   = CHANNEL_IDLE;
                channel->soundID = -1;
                memset(buffer, 0, sizeof(int16) * bufferRemaining);

                break;
            }
        }

        s += samples;
        buffer += samples;
        bufferRemaining = MIX_BUFFER_SIZE - s;
    }

    for (int32 i = 0; i < MIX_BUFFER_SIZE; ++i) channel->samplePtr[i] /= 2;
}

void RSDK::LoadStream(ChannelInfo *channel)
{
    if (channel->state != CHANNEL_LOADING_STREAM)
        return;

#ifdef RSDKv5_USE_LIBVORBIS
    ov_clear(&vorbisMetadata.file);
#else
    stb_vorbis_close(vorbisInfo);
#endif

    FileInfo info;
    InitFileInfo(&info);

    if (LoadFile(&info, streamFilePath, FMODE_RB)) {
        streamBufferSize = info.fileSize;
        streamBuffer     = NULL;
        AllocateStorage((void **)&streamBuffer, info.fileSize, DATASET_MUS, false);
        ReadBytes(&info, streamBuffer, streamBufferSize);
        CloseFile(&info);

        if (streamBufferSize > 0) {
#ifdef RSDKv5_USE_LIBVORBIS
            vorbisMetadata.buffer = streamBuffer;
            vorbisMetadata.size = streamBufferSize;
            vorbisMetadata.position = 0;

            if (ov_open_callbacks(&vorbisMetadata, &vorbisMetadata.file, NULL, 0, vorbisCallbacks) == 0) {
#else
            vorbisAlloc.alloc_buffer_length_in_bytes = 512 * 1024; // 512KiB
            AllocateStorage((void **)&vorbisAlloc.alloc_buffer, 512 * 1024, DATASET_MUS, false);

            vorbisInfo = stb_vorbis_open_memory(streamBuffer, streamBufferSize, NULL, &vorbisAlloc);
            if (vorbisInfo) {
#endif
                if (streamStartPos)
#ifdef RSDKv5_USE_LIBVORBIS
                    ov_pcm_seek(&vorbisMetadata.file, streamStartPos);
#else
                    stb_vorbis_seek(vorbisInfo, streamStartPos);
#endif
                UpdateStreamBuffer(channel);

                channel->state = CHANNEL_STREAM;
            }
        }
    }

    if (channel->state == CHANNEL_LOADING_STREAM)
        channel->state = CHANNEL_IDLE;
}

int32 RSDK::PlayStream(const char *filename, uint32 slot, uint32 startPos, uint32 loopPoint, bool32 loadASync)
{
    if (!engine.streamsEnabled)
        return -1;

    if (slot >= CHANNEL_COUNT) {
        for (int32 c = 0; c < CHANNEL_COUNT && slot >= CHANNEL_COUNT; ++c) {
            if (channels[c].soundID == -1 && channels[c].state != CHANNEL_LOADING_STREAM) {
                slot = c;
            }
        }

        // as a last resort, run through all channels
        // pick the channel closest to being finished
        if (slot >= CHANNEL_COUNT) {
            uint32 len = 0xFFFFFFFF;
            for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
                if (channels[c].sampleLength < len && channels[c].state != CHANNEL_LOADING_STREAM) {
                    slot = c;
                    len  = (uint32)channels[c].sampleLength;
                }
            }
        }
    }

    if (slot >= CHANNEL_COUNT)
        return -1;

    ChannelInfo *channel = &channels[slot];

    LockAudioDevice();

    channel->soundID      = 0xFF;
    channel->loop         = loopPoint != 0;
    channel->priority     = 0xFF;
    channel->state        = CHANNEL_LOADING_STREAM;
    channel->pan          = 0.0f;
    channel->volume       = 1.0f;
    channel->sampleLength = sfxList[SFX_COUNT - 1].length;
    channel->samplePtr    = sfxList[SFX_COUNT - 1].buffer;
    channel->bufferPos    = 0;
    channel->speed        = TO_FIXED(1);

    sprintf_s(streamFilePath, sizeof(streamFilePath), "Data/Music/%s", filename);
    streamStartPos  = startPos;
    streamLoopPoint = loopPoint;

    AudioDevice::HandleStreamLoad(channel, loadASync);

    UnlockAudioDevice();

    return slot;
}

#define WAV_SIG_HEADER (0x46464952) // RIFF
#define WAV_SIG_DATA   (0x61746164) // data

void RSDK::LoadSfxToSlot(char *filename, uint8 slot, uint8 plays, uint8 scope)
{
    FileInfo info;
    InitFileInfo(&info);

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/SoundFX/%s", filename);

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(filename, hash);

    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
        HASH_COPY_MD5(sfxList[slot].hash, hash);
        sfxList[slot].scope              = scope;
        sfxList[slot].maxConcurrentPlays = plays;

        uint8 type = fullFilePath[strlen(fullFilePath) - 1];
        if (type == 'v' || type == 'V') { // A very loose way of checking that we're trying to load a '.wav' file.
            uint32 signature = ReadInt32(&info, false);

            if (signature == WAV_SIG_HEADER) {
                ReadInt32(&info, false); // chunk size
                ReadInt32(&info, false); // WAVE
                ReadInt32(&info, false); // FMT
#if !RETRO_USE_ORIGINAL_CODE
                int32 chunkSize = ReadInt32(&info, false); // chunk size
#else
                ReadInt32(&info, false); // chunk size
#endif
                ReadInt16(&info);        // audio format
                ReadInt16(&info);        // channels
                ReadInt32(&info, false); // sample rate
                ReadInt32(&info, false); // bytes per sec
                ReadInt16(&info);        // block align
                ReadInt16(&info);        // format

                Seek_Set(&info, 34);
                uint16 sampleBits = ReadInt16(&info);

#if !RETRO_USE_ORIGINAL_CODE
                // Original code added to help fix some issues
                Seek_Set(&info, 20 + chunkSize);
#endif

                // Find the data header
                int32 loop = 0;
                while (true) {
                    signature = ReadInt32(&info, false);
                    if (signature == WAV_SIG_DATA)
                        break;

                    loop += 4;
                    if (loop >= 0x40) {
                        if (loop != 0x100) {
                            CloseFile(&info);
                            // There's a bug here: `sfxList[id].scope` is not reset to `SCOPE_NONE`,
                            // meaning that the game will consider the SFX valid and allow it to be played.
                            // This can cause a crash because the SFX is incomplete.
#if !RETRO_USE_ORIGINAL_CODE
                            PrintLog(PRINT_ERROR, "Unable to read sfx: %s", filename);
#endif
                            return;
                        }
                        else {
                            break;
                        }
                    }
                }

                uint32 length = ReadInt32(&info, false);
                if (sampleBits == 16)
                    length /= 2;

                AllocateStorage((void **)&sfxList[slot].buffer, sizeof(int16) * length, DATASET_SFX, false);
                sfxList[slot].length = length;

                // Convert the sample data to S16 format
                int16 *buffer = (int16 *)sfxList[slot].buffer;
                if (sampleBits == 8) {
                    // 8-bit sample. Convert from U8 to S8, and then from S8 to S16.
                    for (int32 s = 0; s < length; ++s)
                        *buffer++ = ((int32)ReadInt8(&info) - 0x80) * (1 << 8);
                }
                else {
                    // 16-bit sample.
                    for (int32 s = 0; s < length; ++s) {
                        // For some reason, the game performs sign-extension manually here.
                        // Note that this is different from the 8-bit format's unsigned-to-signed conversion.
                        int32 sample = (uint16)ReadInt16(&info);

                        if (sample > 0x7FFF)
                            sample = (sample & 0x7FFF) - 0x8000;

                        *buffer++ = (sample * 3) / 4;
                    }
                }
            }
#if !RETRO_USE_ORIGINAL_CODE
            else {
                PrintLog(PRINT_ERROR, "Invalid header in sfx: %s", filename);
            }
#endif
        }
#if !RETRO_USE_ORIGINAL_CODE
        else {
            // what the
            PrintLog(PRINT_ERROR, "Could not find header in sfx: %s", filename);
        }
#endif
    }
#if !RETRO_USE_ORIGINAL_CODE
    else {
        PrintLog(PRINT_ERROR, "Unable to open sfx: %s", filename);
    }
#endif

    CloseFile(&info);
}

void RSDK::LoadSfx(char *filename, uint8 plays, uint8 scope)
{
    // Find an empty sound slot.
    uint16 id = -1;
    for (uint32 i = 0; i < SFX_COUNT; ++i) {
        if (sfxList[i].scope == SCOPE_NONE) {
            id = i;
            break;
        }
    }

    if (id != (uint16)-1)
        LoadSfxToSlot(filename, id, plays, scope);
}

int32 RSDK::PlaySfx(uint16 sfx, uint32 loopPoint, uint32 priority)
{
    if (sfx >= SFX_COUNT || !sfxList[sfx].scope)
        return -1;

    uint8 count = 0;
    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].soundID == sfx)
            ++count;
    }

    int8 slot = -1;
    // if we've hit the max, replace the oldest one
    if (count >= sfxList[sfx].maxConcurrentPlays) {
        int32 highestStackID = 0;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            int32 stackID = sfxList[sfx].playCount - channels[c].playIndex;
            if (stackID > highestStackID && channels[c].soundID == sfx) {
                slot           = c;
                highestStackID = stackID;
            }
        }
    }

    // if we don't have a slot yet, try to pick any channel that's not currently playing
    for (int32 c = 0; c < CHANNEL_COUNT && slot < 0; ++c) {
        if (channels[c].soundID == -1 && channels[c].state != CHANNEL_LOADING_STREAM) {
            slot = c;
        }
    }

    // as a last resort, run through all channels
    // pick the channel closest to being finished AND with lower priority
    if (slot < 0) {
        uint32 len = 0xFFFFFFFF;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            if (channels[c].sampleLength < len && priority > channels[c].priority && channels[c].state != CHANNEL_LOADING_STREAM) {
                slot = c;
                len  = (uint32)channels[c].sampleLength;
            }
        }
    }

    if (slot == -1)
        return -1;

    LockAudioDevice();

    channels[slot].state        = CHANNEL_SFX;
    channels[slot].bufferPos    = 0;
    channels[slot].samplePtr    = sfxList[sfx].buffer;
    channels[slot].sampleLength = sfxList[sfx].length;
    channels[slot].volume       = 1.0f;
    channels[slot].pan          = 0.0f;
    channels[slot].speed        = TO_FIXED(1);
    channels[slot].soundID      = sfx;
    if (loopPoint >= 2)
        channels[slot].loop = loopPoint;
    else
        channels[slot].loop = loopPoint - 1;
    channels[slot].priority  = priority;
    channels[slot].playIndex = sfxList[sfx].playCount++;

    UnlockAudioDevice();

    return slot;
}

void RSDK::SetChannelAttributes(uint8 channel, float volume, float panning, float speed)
{
    if (channel < CHANNEL_COUNT) {
        volume                   = fminf(4.0f, volume);
        volume                   = fmaxf(0.0f, volume);
        channels[channel].volume = volume;

        panning               = fminf(1.0f, panning);
        panning               = fmaxf(-1.0f, panning);
        channels[channel].pan = panning;

        if (speed > 0.0f)
            channels[channel].speed = (int32)(speed * TO_FIXED(1));
        else if (speed == 1.0f)
            channels[channel].speed = TO_FIXED(1);
    }
}

uint32 RSDK::GetChannelPos(uint32 channel)
{
    if (channel >= CHANNEL_COUNT)
        return 0;

    if (channels[channel].state == CHANNEL_SFX)
        return channels[channel].bufferPos;

    if (channels[channel].state == CHANNEL_STREAM) {
#ifdef RSDKv5_USE_LIBVORBIS
        return ov_pcm_tell(&vorbisMetadata.file);
#else
        if (!vorbisInfo->current_loc_valid || vorbisInfo->current_loc < 0)
            return 0;

        return vorbisInfo->current_loc;
#endif
    }

    return 0;
}

double RSDK::GetVideoStreamPos()
{
    if (channels[0].state == CHANNEL_STREAM && AudioDevice::audioState && AudioDevice::initializedAudioChannels)
#ifdef RSDKv5_USE_LIBVORBIS
        return ov_pcm_tell(&vorbisMetadata.file) / (double)AUDIO_FREQUENCY;
#else
        if (vorbisInfo->current_loc_valid)
            return vorbisInfo->current_loc / (double)AUDIO_FREQUENCY;
#endif

    return -1.0;
}

void RSDK::ClearStageSfx()
{
    LockAudioDevice();

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].state == CHANNEL_SFX || channels[c].state == (CHANNEL_SFX | CHANNEL_PAUSED)) {
            channels[c].soundID = -1;
            channels[c].state   = CHANNEL_IDLE;
        }
    }

    // Unload stage SFX
    for (int32 s = 0; s < SFX_COUNT; ++s) {
        if (sfxList[s].scope >= SCOPE_STAGE) {
            MEM_ZERO(sfxList[s]);
            sfxList[s].scope = SCOPE_NONE;
        }
    }

    UnlockAudioDevice();
}

#if RETRO_USE_MOD_LOADER
void RSDK::ClearGlobalSfx()
{
    LockAudioDevice();

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].state == CHANNEL_SFX || channels[c].state == (CHANNEL_SFX | CHANNEL_PAUSED)) {
            channels[c].soundID = -1;
            channels[c].state   = CHANNEL_IDLE;
        }
    }

    // Unload global SFX
    for (int32 s = 0; s < SFX_COUNT; ++s) {
        // clear global sfx (do NOT clear the stream channel 0 slot)
        if (sfxList[s].scope == SCOPE_GLOBAL && s != SFX_COUNT - 1) {
            MEM_ZERO(sfxList[s]);
            sfxList[s].scope = SCOPE_NONE;
        }
    }

    UnlockAudioDevice();
}
#endif
