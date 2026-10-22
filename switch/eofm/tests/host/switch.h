// Host stand-in for <switch.h>: only what eofm's main.c, gfx.c and rds.c use, for the screen
// preview (tests/preview.c). Nothing here talks to hardware.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef volatile uint64_t vu64;
typedef u32 Result;
typedef u32 Mutex;
typedef struct { int dummy; } Thread;
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc)    ((rc) != 0)
#define MAKERESULT(m, d) (((m) & 0x1FF) | ((d) & 0x1FFF) << 9)
#define Module_Libnx 345
#define LibnxError_NotInitialized 2

typedef enum { AudioTarget_Invalid, AudioTarget_Speaker, AudioTarget_Headphone, AudioTarget_Tv, AudioTarget_UsbOutputDevice, AudioTarget_Bluetooth } AudioTarget;
typedef struct { int dummy; } UsbHsClientIfSession;

typedef struct { u64 down, held; } PadState;
#define HidNpadStyleSet_NpadStandard 0
enum {
    HidNpadButton_A = 1 << 0, HidNpadButton_B = 1 << 1, HidNpadButton_X = 1 << 2, HidNpadButton_Y = 1 << 3,
    HidNpadButton_StickL = 1 << 4, HidNpadButton_StickR = 1 << 5, HidNpadButton_L = 1 << 6, HidNpadButton_R = 1 << 7,
    HidNpadButton_ZL = 1 << 8, HidNpadButton_ZR = 1 << 9, HidNpadButton_Plus = 1 << 10, HidNpadButton_Minus = 1 << 11,
    HidNpadButton_Left = 1 << 12, HidNpadButton_Up = 1 << 13, HidNpadButton_Right = 1 << 14, HidNpadButton_Down = 1 << 15,
    HidNpadButton_StickRLeft = 1 << 16, HidNpadButton_StickRRight = 1 << 17,
};
static inline void padConfigureInput(int n, int s) {}
static inline void padInitializeDefault(PadState *p) {}
static inline void padUpdate(PadState *p) {}
static inline u64 padGetButtonsDown(PadState *p) { return p->down; }
static inline u64 padGetButtons(PadState *p) { return p->held; }
typedef struct { u32 x, y; } HidTouchState;
typedef struct { s32 count; HidTouchState touches[16]; } HidTouchScreenState;
static inline void hidInitializeTouchScreen(void) {}
static inline size_t hidGetTouchScreenStates(HidTouchScreenState *s, size_t n) { return 0; }
static inline bool appletMainLoop(void) { return false; }
static inline void consoleInit(void *p) {}
static inline void consoleUpdate(void *p) {}
static inline void consoleExit(void *p) {}
static inline Result usbHsInitialize(void) { return 0; }
static inline void usbHsExit(void) {}
static inline void svcSleepThread(int64_t ns) {}
extern u64 g_fake_ms;
static inline u64 armGetSystemTick(void) { return g_fake_ms; }
static inline u64 armTicksToNs(u64 t) { return t * 1000000ULL; }
