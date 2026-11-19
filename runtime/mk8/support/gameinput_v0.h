// Windows SDK GameInput v0 COM ABI (x64). Only the methods we call are typed.
// GameInputCreate from System32 returns this interface even on newer Windows.
#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct GI GI;
typedef struct GIDevice GIDevice;
typedef struct GIReading GIReading;
typedef struct { uint32_t buttons; float lt, rt, lx, ly, rx, ry; } GIState;
typedef struct { float low, high, lt, rt; } GIRumble;
typedef struct {
    uint32_t size;
    uint16_t vendor, product;
    uint8_t device_identity_and_capabilities[100];
    uint32_t rumble_motors;
} GIInfoPrefix;

typedef void (CALLBACK *GIChanged)(uint64_t, void *, GIDevice *, uint64_t, unsigned, unsigned);
typedef struct {
    FARPROC unused0[2];
    ULONG (WINAPI *release)(GI *);
    FARPROC timestamp;
    HRESULT (WINAPI *read)(GI *, unsigned, GIDevice *, GIReading **);
    FARPROC unused5[4];
    HRESULT (WINAPI *subscribe)(GI *, GIDevice *, unsigned, unsigned, unsigned, void *, GIChanged, uint64_t *);
    FARPROC unused10[3];
    bool (WINAPI *unsubscribe)(GI *, uint64_t, uint64_t);
} GIMethods;
struct GI { const GIMethods *v; };
typedef struct {
    FARPROC query;
    ULONG (WINAPI *retain)(GIDevice *);
    ULONG (WINAPI *release)(GIDevice *);
    const GIInfoPrefix *(WINAPI *info)(GIDevice *);
    unsigned (WINAPI *status)(GIDevice *);
    FARPROC unused5[5];
    void (WINAPI *rumble)(GIDevice *, const GIRumble *);
} GIDeviceMethods;
struct GIDevice { const GIDeviceMethods *v; };
typedef struct {
    FARPROC unused0[2];
    ULONG (WINAPI *release)(GIReading *);
    FARPROC unused3[3];
    void (WINAPI *device)(GIReading *, GIDevice **);
    FARPROC unused7[15];
    bool (WINAPI *gamepad)(GIReading *, GIState *);
} GIReadingMethods;
struct GIReading { const GIReadingMethods *v; };
