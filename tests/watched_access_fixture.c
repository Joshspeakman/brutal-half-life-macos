/* These operations use exactly the helpers compiled into the original DLL's
 * translated instructions. The bridge fixture itself remains uninstrumented. */
#define RT_MEMORY_WATCH
#include "rt.h"
float watched_float(uint32_t address){return rt_rf32(G_MEM,address);}
uint32_t watched_u32(uint32_t address){return rt_r32(G_MEM,address);}
void watched_write_u32(uint32_t address,uint32_t value){rt_w32(G_MEM,address,value);}
void watched_write(uint32_t address,float value){rt_wf32(G_MEM,address,value);}
void watched_move(uint32_t to,uint32_t from,size_t size){rt_guest_move(G_MEM,to,from,size);}
void watched_vector(uint32_t to,uint32_t from){X128 value;rt_ldx(G_MEM,&value,from,16);rt_stx(G_MEM,to,&value,16);}
