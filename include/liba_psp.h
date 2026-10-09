#pragma once

#include <cinttypes>
#include <map>
#include <string>

#include "audio_files.h"
#include "liba_pc.h"

// Keep these as individual bits. Apotris stores combinations of controls in
// its save data, so the native PSP_CTRL_* values are translated at the edge.
#define KEY_A (1u << 0)
#define KEY_B (1u << 1)
#define KEY_L (1u << 2)
#define KEY_R (1u << 3)
#define KEY_UP (1u << 4)
#define KEY_DOWN (1u << 5)
#define KEY_LEFT (1u << 6)
#define KEY_RIGHT (1u << 7)
#define KEY_SELECT (1u << 8)
#define KEY_START (1u << 9)
#define KEY_FULL 0xffffffffu

extern void updateWindow(uint8_t* framebuffer);
extern void refreshWindowSize();
extern bool closed();

extern void windowInit();
extern void pspAudioInit();
extern void pspAudioShutdown();
extern void key_poll();
extern uint32_t key_is_down(uint32_t key);
extern uint32_t key_hit(uint32_t key);
extern uint32_t key_released(uint32_t key);
extern uint32_t key_first();
extern uint32_t keys_raw();

extern int rowStart;
extern int rowEnd;
extern std::map<int, std::string> keyToString;
