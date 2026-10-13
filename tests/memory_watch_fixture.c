#define RT_MEMORY_WATCH
#include "rt.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
uint8_t *G_MEM;
static unsigned char pending[RT_WATCH_PAGES];
static unsigned calls,finished;
static void page(uint32_t index)
{calls++;memset(G_MEM+RT_WATCH_BASE+4096u*index,0x20+index,4096);}
static void finish(void){finished++;rt_watch_end();}
static void begin(void)
{memset(pending,0,sizeof pending);pending[0]=pending[1]=pending[2]=1;calls=0;rt_watch_begin(pending,page,finish);}
static void *worker(void *unused)
{assert(!rt_watch_pages);assert(rt_r8(G_MEM,RT_WATCH_BASE)==0);return NULL;}
/* Unaligned atomics are not used here, but rt.h references these fallbacks. */
void rt_bus_lock(void){} void rt_bus_unlock(void){}
int main(void)
{
    G_MEM=mmap(NULL,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(G_MEM!=MAP_FAILED);
    rt_watch_register_range(RT_WATCH_BASE,3*4096);
    begin();pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));pthread_join(thread,NULL);assert(!calls);
    assert(rt_r32(G_MEM,RT_WATCH_BASE)==0x20202020u&&calls==1);
    rt_w32(G_MEM,RT_WATCH_BASE,0x12345678);assert(rt_r32(G_MEM,RT_WATCH_BASE)==0x12345678&&calls==1);rt_watch_end();
    begin();rt_w64(G_MEM,RT_WATCH_BASE+4092,0x0102030405060708ull);assert(calls==2);
    assert(G_MEM[RT_WATCH_BASE+4091]==0x20&&G_MEM[RT_WATCH_BASE+4100]==0x21);rt_watch_end();
    begin();X128 value;rt_ldx(G_MEM,&value,RT_WATCH_BASE+4090,16);assert(calls==2&&value.b[5]==0x20&&value.b[6]==0x21);rt_watch_end();
    begin();rt_guest_move(G_MEM,RT_WATCH_BASE+8192,RT_WATCH_BASE-2,12);assert(calls==2);
    assert(G_MEM[RT_WATCH_BASE+8192]==0&&G_MEM[RT_WATCH_BASE+8194]==0x20);rt_watch_end();
    begin();rt_guest_set(G_MEM,RT_WATCH_BASE+4090,0x66,20);assert(calls==2&&G_MEM[RT_WATCH_BASE+4109]==0x66);rt_watch_end();
    begin();rt_guest_move(G_MEM,RT_WATCH_BASE,RT_WATCH_BASE,0);assert(!calls);
    assert(rt_lock_xchg(G_MEM,RT_WATCH_BASE,4,99)==0x20202020&&calls==1);
    rt_watch_materialize();assert(finished==1&&!rt_watch_pages);
    munmap(G_MEM,0x100000000ull);puts("Memory watch passed: thread isolation, read/write ordering, boundary spans, SSE, bulk copies, atomics and native fallback");
}
