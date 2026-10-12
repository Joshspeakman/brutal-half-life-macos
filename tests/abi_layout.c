#include "rt.h"
#include <stddef.h>
size_t bhl_ctx_size(void) { return sizeof(Ctx); }
size_t bhl_ctx_xmm_offset(void) { return offsetof(Ctx, xmm); }
