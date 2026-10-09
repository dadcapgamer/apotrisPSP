#ifdef PSP

#include "platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <sys/stat.h>

#include <SDL2/SDL.h>
#include <pspctrl.h>
#include <pspkernel.h>

#include "def.h"
#include "psp_music_table.h"

PSP_MODULE_INFO("Apotris PSP", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

namespace {
constexpr int kTargetFps = 60;
constexpr const char* kSaveDirectory = "ms0:/PSP/SAVEDATA/APOTRIS";
constexpr const char* kSavePath = "ms0:/PSP/SAVEDATA/APOTRIS/Apotris.sav";

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_Texture* texture = nullptr;
uint32_t currentKeys = 0;
uint32_t previousKeys = 0;
uint32_t frameStart = 0;
bool running = true;
bool renderEnabled = true;

uint32_t translateButtons(uint32_t buttons) {
    uint32_t result = 0;
    if (buttons & PSP_CTRL_CROSS) result |= KEY_A;
    if (buttons & PSP_CTRL_CIRCLE) result |= KEY_B;
    if (buttons & PSP_CTRL_LTRIGGER) result |= KEY_L;
    if (buttons & PSP_CTRL_RTRIGGER) result |= KEY_R;
    if (buttons & PSP_CTRL_UP) result |= KEY_UP;
    if (buttons & PSP_CTRL_DOWN) result |= KEY_DOWN;
    if (buttons & PSP_CTRL_LEFT) result |= KEY_LEFT;
    if (buttons & PSP_CTRL_RIGHT) result |= KEY_RIGHT;
    if (buttons & PSP_CTRL_SELECT) result |= KEY_SELECT;
    if (buttons & PSP_CTRL_START) result |= KEY_START;
    return result;
}

int exitCallback(int, int, void*) {
    running = false;
    return 0;
}

int callbackThread(SceSize, void*) {
    const int callback = sceKernelCreateCallback("Apotris exit", exitCallback, nullptr);
    sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

void installExitCallback() {
    const int thread = sceKernelCreateThread("Apotris callbacks", callbackThread,
                                             0x11, 0x1000, 0, nullptr);
    if (thread >= 0) sceKernelStartThread(thread, 0, nullptr);
}
} // namespace

Songs songs;
int rowStart = 0;
int rowEnd = 180;
float fps = 0.0f;

void windowInit() {
    installExitCallback();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        running = false;
        return;
    }

    window = SDL_CreateWindow("Apotris PSP", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 480, 272, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                SDL_TEXTUREACCESS_STREAMING, 240, 180);
    if (!window || !renderer || !texture) running = false;

    pspAudioInit();

    // Song ids index the linked-in module table; the split matches the
    // desktop backend's (menu files first sorted, then the rest).
    songs.menu = PSP_MUSIC_MENU_LIST;
    songs.game = PSP_MUSIC_GAME_LIST;
    frameStart = SDL_GetTicks();
}

void key_poll() {
    previousKeys = currentKeys;
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(&pad, 1);
    currentKeys = translateButtons(pad.Buttons);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) running = false;
    }
}

uint32_t key_is_down(uint32_t key) {
    return key == KEY_FULL ? currentKeys != 0 : (currentKeys & key) == key;
}

uint32_t key_hit(uint32_t key) {
    if (key == KEY_FULL) return previousKeys == 0 && currentKeys != 0;
    return (previousKeys & key) == 0 && (currentKeys & key) == key;
}

uint32_t key_released(uint32_t key) {
    if (key == KEY_FULL) return previousKeys != 0 && currentKeys == 0;
    return (previousKeys & key) == key && (currentKeys & key) == 0;
}

uint32_t key_first() {
    const uint32_t pressed = currentKeys & ~previousKeys;
    if (!pressed) return KEY_FULL - 1;
    for (uint32_t bit = 1; bit; bit <<= 1) {
        if (pressed & bit) return bit;
    }
    return KEY_FULL - 1;
}

uint32_t keys_raw() {
    return currentKeys ? (KEY_FULL ^ currentKeys) : KEY_FULL - 1;
}

void updateWindow(uint8_t* framebuffer) {
    if (!running || !renderEnabled || !texture || !renderer) return;

    SDL_UpdateTexture(texture, nullptr, framebuffer, 240 * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    // Apotris draws its GBA-sized 240x160 playfield in the center of a
    // 240x180 software surface. Crop the padding and preserve its 3:2 aspect.
    const SDL_Rect source{0, 10, 240, 160};
    const SDL_Rect destination{36, 0, 408, 272};
    SDL_RenderCopy(renderer, texture, &source, &destination);
    SDL_RenderPresent(renderer);
}

void refreshWindowSize() {}

bool closed() {
    const uint32_t now = SDL_GetTicks();
    const uint32_t elapsed = now - frameStart;
    const uint32_t frameMs = 1000 / kTargetFps;
    if (elapsed < frameMs) SDL_Delay(frameMs - elapsed);
    const uint32_t end = SDL_GetTicks();
    const uint32_t actual = end - frameStart;
    fps = actual ? 1000.0f / actual : 60.0f;
    frameStart = end;
    return running;
}

void toggleRendering(bool enabled) { renderEnabled = enabled; }
void initRumble() {}
void rumbleStart(int) {}
void rumbleStop() {}

void loadSavefile() {
    if (!savefile) savefile = new Save();
    std::ifstream input(kSavePath, std::ios::binary | std::ios::in);
    if (input) input.read(reinterpret_cast<char*>(savefile), sizeof(Save));
}

void saveSavefile() {
    mkdir(kSaveDirectory, 0777);
    std::ofstream output(kSavePath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output || !savefile) return;

    constexpr int saveSize = 1 << 15;
    char buffer[saveSize];
    std::memset(buffer, 0, sizeof(buffer));
    std::memcpy(buffer, savefile, std::min(sizeof(buffer), sizeof(Save)));
    output.write(buffer, sizeof(buffer));
}

void quit() {
    pspAudioShutdown();
    running = false;
}

std::map<int, std::string> keyToString = {
    {KEY_A, "Cross"}, {KEY_B, "Circle"}, {KEY_L, "L"},
    {KEY_R, "R"}, {KEY_SELECT, "Select"}, {KEY_START, "Start"},
    {KEY_LEFT, "Left"}, {KEY_UP, "Up"}, {KEY_RIGHT, "Right"},
    {KEY_DOWN, "Down"},
};

#endif
