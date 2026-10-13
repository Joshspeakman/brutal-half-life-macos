#ifndef BHL_MEMORY_WATCH_H
#define BHL_MEMORY_WATCH_H
/* Synchronous borrowed-memory scopes. A page is materialized before its first
 * translated access, including writes. Native pointer escapes end the scope.
 * State is per thread: audio workers never enter a renderer's scope. */
#define RT_WATCH_BASE 0x60000000u
#define RT_WATCH_SIZE 0x08000000u
#define RT_WATCH_PAGES (RT_WATCH_SIZE / 4096u)
extern __thread unsigned char *rt_watch_pages;
extern unsigned char rt_watch_known[RT_WATCH_PAGES];
void rt_watch_register_range(uint32_t,size_t);
void rt_watch_begin(unsigned char *, void (*)(uint32_t), void (*)(void));
void rt_watch_end(void);
void rt_watch_materialize(void);
void rt_watch_access(uint32_t, size_t);
static inline void rt_watch_memory(uint32_t address,size_t size)
{
#ifdef RT_MEMORY_WATCH
    if (address-RT_WATCH_BASE < RT_WATCH_SIZE) {
        unsigned page=(address-RT_WATCH_BASE)>>12;
        if (size > 4096u-(address&4095u))
            rt_watch_access(address,size);
        else if (__atomic_load_n(rt_watch_known+page,__ATOMIC_RELAXED) &&
                 rt_watch_pages && rt_watch_pages[page]) rt_watch_access(address,size);
    } else if (address<RT_WATCH_BASE && size>RT_WATCH_BASE-address)
        rt_watch_access(address,size);
#endif
}
static inline void rt_guest_move(uint8_t *memory,uint32_t to,uint32_t from,size_t size)
{rt_watch_memory(from,size);rt_watch_memory(to,size);memmove(memory+to,memory+from,size);}
static inline void rt_guest_set(uint8_t *memory,uint32_t to,int value,size_t size)
{rt_watch_memory(to,size);memset(memory+to,value,size);}
#endif
