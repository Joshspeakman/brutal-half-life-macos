#ifndef BHL_LOADER_H
#define BHL_LOADER_H
#include "win32.h"
typedef struct BhlExport { const char *name; uint32_t address; } BhlExport;
typedef struct BhlModule {
    const char *name, *sha256;
    uint32_t base, size, entry;
    const BhlExport *exports;
    unsigned export_count;
} BhlModule;
extern const BhlModule bhl_modules[];
extern const unsigned bhl_module_count;
void bhl_initialize(void);
uint32_t bhl_export(const char *module, const char *name);
uint32_t bhl_invoke(uint32_t function, const uint32_t *args, unsigned count, int stdcall);
uint32_t bhl_thiscall(uint32_t function, uint32_t object, const uint32_t *args, unsigned count);
void bhl_attach(const char *module);
void bhl_enter_guest_thread(Ctx *context);
void bhl_leave_guest_thread(void);
#endif
