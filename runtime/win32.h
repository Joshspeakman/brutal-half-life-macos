#ifndef BHL_WIN32_H
#define BHL_WIN32_H
#include "rt.h"
#define BHL_THUNK_BASE 0xfff00000u
#define BHL_HEAP_BASE 0x60000000u
#define BHL_HEAP_END 0x68000000u
#define BHL_TEB 0x7ffde000u
#define BHL_STACK_BASE 0x70000000u
#define BHL_STACK_SIZE 0x00400000u
void bhl_process_init(Ctx *c);
uint32_t bhl_heap_alloc(uint32_t bytes, int zero);
uint32_t bhl_heap_used(void);
const char *bhl_import_name(uint32_t index);
uint32_t bhl_thunk(const char *name, void (*callback)(Ctx *));
void bhl_set_deterministic(int enabled);
void bhl_set_trace(int enabled);
#endif
