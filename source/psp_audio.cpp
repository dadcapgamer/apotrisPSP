#ifdef PSP

#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <SDL2/SDL.h>
#include <libxmp-lite/xmp.h>

#include "def.h"
#include "psp_music_table.h"
#include "psp_sfx_table.h"
#include "save.h"

// One software mixer feeds the single 44100 Hz stereo output that sceAudio
// provides. Sound effects are stored mono at PSP_SFX_SAMPLE_RATE (half the
// output rate) and interpolated up, which halves their resident size and costs
// nothing extra -- sfxRate() needs arbitrary playback rates regardless. Music
// is decoded from the linked-in modules by libxmp on a producer thread and
// handed to the callback through a ring buffer, so a long decode can never
// stall the audio device.

static_assert(PSP_SFX_TABLE_COUNT == SFX_COUNT,
              "generated sfx table is out of step with SoundEffects");

namespace {

constexpr int kOutputRate = 44100;
constexpr int kOutputChannels = 2;
// sceAudio wants sample counts that are a multiple of 64.
constexpr int kBufferFrames = 1024;
constexpr int kVoiceCount = 8;

// About 0.37 s of slack between the decoder and the audio device.
constexpr int kMusicRingFrames = 16384;
constexpr int kMusicChunkFrames = 1024;

// Position and step are 16.16 fixed point sample indices into the effect.
struct Voice {
    const int16_t* samples;
    uint32_t length;
    uint32_t position;
    uint32_t step;
    int32_t volume; // 0..256
    uint32_t order; // for least-recently-started stealing
    bool active;
};

SDL_AudioDeviceID device = 0;
Voice voices[kVoiceCount];
uint32_t voiceOrder = 0;
int32_t accumulator[kBufferFrames * kOutputChannels];

xmp_context musicContext = nullptr;
SDL_mutex* musicMutex = nullptr;
SDL_Thread* musicThread = nullptr;
int16_t musicRing[kMusicRingFrames * kOutputChannels];
int16_t musicStaging[kMusicChunkFrames * kOutputChannels];

std::atomic<uint32_t> musicWrite{0};
std::atomic<uint32_t> musicRead{0};
std::atomic<int32_t> musicVolumeScale{0}; // 0..256
std::atomic<bool> musicPlaying{false};
std::atomic<bool> musicPaused{false};
std::atomic<bool> musicEnded{false};
std::atomic<bool> musicThreadRun{false};

bool musicModuleLoaded = false; // guarded by musicMutex
int musicLoopCount = 0;         // guarded by musicMutex
int currentSong = -1;           // guarded by musicMutex

int32_t sfxVolumeScale() {
    if (!savefile) return 0;
    const int volume = savefile->settings.sfxVolume;
    return std::clamp(volume, 0, 10) * 256 / 10;
}

void mixVoice(Voice& voice, int frames) {
    uint32_t position = voice.position;
    const uint32_t step = voice.step;
    const uint32_t last = voice.length - 1;

    for (int frame = 0; frame < frames; ++frame) {
        const uint32_t index = position >> 16;
        if (index >= last) {
            voice.active = false;
            return;
        }

        // An 8-bit interpolation fraction keeps the whole term inside int32:
        // the sample delta needs 18 bits and the fraction adds 8.
        const int32_t a = voice.samples[index];
        const int32_t b = voice.samples[index + 1];
        const int32_t fraction = static_cast<int32_t>((position >> 8) & 0xff);
        const int32_t sample = a + (((b - a) * fraction) >> 8);

        // The source is mono; it feeds both sides equally.
        const int32_t scaled = (sample * voice.volume) >> 8;
        accumulator[frame * 2] += scaled;
        accumulator[frame * 2 + 1] += scaled;
        position += step;
    }

    voice.position = position;
}

void mixMusic(int frames) {
    const int32_t volume = musicVolumeScale.load(std::memory_order_relaxed);
    if (volume == 0) return;

    const uint32_t read = musicRead.load(std::memory_order_relaxed);
    const uint32_t available =
        musicWrite.load(std::memory_order_acquire) - read;
    const int count = std::min<int>(frames, available);

    for (int frame = 0; frame < count; ++frame) {
        const uint32_t slot = (read + frame) % kMusicRingFrames;
        accumulator[frame * 2] +=
            (musicRing[slot * 2] * volume) >> 8;
        accumulator[frame * 2 + 1] +=
            (musicRing[slot * 2 + 1] * volume) >> 8;
    }

    musicRead.store(read + count, std::memory_order_release);
}

void audioCallback(void*, Uint8* stream, int length) {
    const int frames = std::min<int>(length / (kOutputChannels * sizeof(int16_t)),
                                     kBufferFrames);
    std::memset(accumulator, 0, frames * kOutputChannels * sizeof(accumulator[0]));

    for (Voice& voice : voices) {
        if (voice.active) mixVoice(voice, frames);
    }

    if (musicPlaying.load(std::memory_order_relaxed) &&
        !musicPaused.load(std::memory_order_relaxed)) {
        mixMusic(frames);
    }

    int16_t* output = reinterpret_cast<int16_t*>(stream);
    for (int i = 0; i < frames * kOutputChannels; ++i) {
        *output++ = static_cast<int16_t>(
            std::clamp(accumulator[i], INT32_C(-32768), INT32_C(32767)));
    }
}

// Decodes ahead of the audio device. Never touched by the callback, so a slow
// module load or a pattern change cannot cause an underrun on its own.
int musicThreadMain(void*) {
    while (musicThreadRun.load(std::memory_order_relaxed)) {
        bool decoded = false;

        SDL_LockMutex(musicMutex);
        if (musicModuleLoaded && !musicPaused.load(std::memory_order_relaxed)) {
            const uint32_t write = musicWrite.load(std::memory_order_relaxed);
            const uint32_t used = write - musicRead.load(std::memory_order_acquire);
            if (kMusicRingFrames - used >= kMusicChunkFrames) {
                const int result = xmp_play_buffer(
                    musicContext, musicStaging,
                    kMusicChunkFrames * kOutputChannels * sizeof(int16_t),
                    musicLoopCount);

                if (result < 0) {
                    // The module reached its end and was not set to repeat.
                    musicModuleLoaded = false;
                    musicEnded.store(true, std::memory_order_relaxed);
                } else {
                    for (int frame = 0; frame < kMusicChunkFrames; ++frame) {
                        const uint32_t slot = (write + frame) % kMusicRingFrames;
                        musicRing[slot * 2] = musicStaging[frame * 2];
                        musicRing[slot * 2 + 1] = musicStaging[frame * 2 + 1];
                    }
                    musicWrite.store(write + kMusicChunkFrames,
                                     std::memory_order_release);
                    decoded = true;
                }
            }
        }
        SDL_UnlockMutex(musicMutex);

        if (!decoded) SDL_Delay(4);
    }
    return 0;
}

// Caller must hold musicMutex.
void releaseModuleLocked() {
    if (!musicModuleLoaded) return;
    xmp_end_player(musicContext);
    xmp_release_module(musicContext);
    musicModuleLoaded = false;
}

void resetRing() {
    // The callback must not be reading while the indices move.
    SDL_LockAudioDevice(device);
    musicRead.store(0, std::memory_order_relaxed);
    musicWrite.store(0, std::memory_order_relaxed);
    SDL_UnlockAudioDevice(device);
}

} // namespace

void pspAudioInit() {
    SDL_AudioSpec want{};
    want.freq = kOutputRate;
    want.format = AUDIO_S16SYS;
    want.channels = kOutputChannels;
    want.samples = kBufferFrames;
    want.callback = audioCallback;

    SDL_AudioSpec have{};
    device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!device) return;

    musicContext = xmp_create_context();
    musicMutex = SDL_CreateMutex();
    if (musicContext && musicMutex) {
        musicThreadRun.store(true, std::memory_order_relaxed);
        musicThread = SDL_CreateThread(musicThreadMain, "apotris-music", nullptr);
    }

    SDL_PauseAudioDevice(device, 0);
}

void pspAudioShutdown() {
    if (!device) return;

    if (musicThread) {
        musicThreadRun.store(false, std::memory_order_relaxed);
        SDL_WaitThread(musicThread, nullptr);
        musicThread = nullptr;
    }

    SDL_CloseAudioDevice(device);
    device = 0;

    if (musicContext) {
        SDL_LockMutex(musicMutex);
        releaseModuleLocked();
        SDL_UnlockMutex(musicMutex);
        xmp_free_context(musicContext);
        musicContext = nullptr;
    }

    if (musicMutex) {
        SDL_DestroyMutex(musicMutex);
        musicMutex = nullptr;
    }
}

void sfxRate(int n, float playbackRate) {
    if (!device || n < 0 || n >= PSP_SFX_TABLE_COUNT) return;

    const int32_t volume = sfxVolumeScale();
    if (volume == 0) return;

    const PspSfxEntry& entry = pspSfxTable[n];
    if (entry.length < 2) return;

    if (playbackRate < 0.01f) playbackRate = 0.01f;
    const uint32_t step = static_cast<uint32_t>(
        playbackRate * PSP_SFX_SAMPLE_RATE * 65536.0f / kOutputRate);
    if (step == 0) return;

    SDL_LockAudioDevice(device);

    // Prefer a free voice; otherwise steal whichever started longest ago.
    Voice* chosen = nullptr;
    for (Voice& voice : voices) {
        if (!voice.active) {
            chosen = &voice;
            break;
        }
        if (!chosen || voice.order < chosen->order) chosen = &voice;
    }

    chosen->samples = pspSfxSamples + entry.offset;
    chosen->length = entry.length;
    chosen->position = 0;
    chosen->step = step;
    chosen->volume = volume;
    chosen->order = ++voiceOrder;
    chosen->active = true;

    SDL_UnlockAudioDevice(device);
}

void sfx(int n) { sfxRate(n, 1.0f); }

void startSong(int song, bool loop) {
    if (!device || !musicContext || song < 0 || song >= PSP_MUSIC_COUNT) return;

    SDL_LockMutex(musicMutex);
    if (song == currentSong && musicModuleLoaded &&
        !musicPaused.load(std::memory_order_relaxed)) {
        SDL_UnlockMutex(musicMutex);
        return;
    }

    musicPlaying.store(false, std::memory_order_relaxed);
    releaseModuleLocked();

    const PspMusicEntry& entry = pspMusicTable[song];
    if (xmp_load_module_from_memory(musicContext, pspMusicData + entry.offset,
                                    static_cast<long>(entry.length)) != 0) {
        currentSong = -1;
        SDL_UnlockMutex(musicMutex);
        return;
    }

    if (xmp_start_player(musicContext, kOutputRate, 0) != 0) {
        xmp_release_module(musicContext);
        currentSong = -1;
        SDL_UnlockMutex(musicMutex);
        return;
    }

    // loop == 0 repeats forever; 1 stops after one pass and reports XMP_END,
    // which songEndHandler() turns into the next track.
    musicLoopCount = loop ? 0 : 1;
    musicModuleLoaded = true;
    currentSong = song;
    musicEnded.store(false, std::memory_order_relaxed);
    musicPaused.store(false, std::memory_order_relaxed);
    SDL_UnlockMutex(musicMutex);

    resetRing();
    musicPlaying.store(true, std::memory_order_relaxed);
}

void stopSong() {
    if (!musicContext) return;
    musicPlaying.store(false, std::memory_order_relaxed);
    SDL_LockMutex(musicMutex);
    releaseModuleLocked();
    currentSong = -1;
    SDL_UnlockMutex(musicMutex);
    resetRing();
}

void pauseSong() { musicPaused.store(true, std::memory_order_relaxed); }

void resumeSong() { musicPaused.store(false, std::memory_order_relaxed); }

void setMusicVolume(int volume) {
    musicVolumeScale.store(std::clamp(volume, 0, 512) * 256 / 512,
                           std::memory_order_relaxed);
}

void setMusicTempo(int tempo) {
    if (!musicContext) return;
    if (tempo < 1) tempo = 1;

    // 1024 is normal speed, matching the GBA build's mmSetModuleTempo(). The
    // factor is a time stretch, so it is the reciprocal: 512 plays at half
    // speed. This changes musical tempo only -- it does not shift pitch.
    SDL_LockMutex(musicMutex);
    if (musicModuleLoaded) xmp_set_tempo_factor(musicContext, 1024.0 / tempo);
    SDL_UnlockMutex(musicMutex);
}

void songEndHandler() {
    if (!musicEnded.exchange(false, std::memory_order_relaxed)) return;
    if (musicVolumeScale.load(std::memory_order_relaxed) == 0) return;

    if (savefile->settings.cycleSongs == 1) { // CYCLE
        playNextSong();
    } else if (savefile->settings.cycleSongs == 2) { // SHUFFLE
        playSongRandom(currentMenu);
    }
}

#endif
