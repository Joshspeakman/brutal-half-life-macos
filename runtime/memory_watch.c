#include "rt.h"
#include <stdlib.h>
__thread unsigned char *rt_watch_pages;
unsigned char rt_watch_known[RT_WATCH_PAGES];
static __thread void (*watch_page)(uint32_t);
static __thread void (*watch_materialize)(void);
void rt_watch_register_range(uint32_t address,size_t size)
{
    uint64_t end=(uint64_t)address+size;
    if(address<RT_WATCH_BASE||end>RT_WATCH_BASE+RT_WATCH_SIZE||!size)abort();
    for(unsigned page=(address-RT_WATCH_BASE)>>12;page<((end-RT_WATCH_BASE+4095)>>12);page++)
        __atomic_store_n(rt_watch_known+page,1,__ATOMIC_RELAXED);
}
void rt_watch_begin(unsigned char *pages,void (*page)(uint32_t),void (*materialize)(void))
{if(rt_watch_pages||!pages||!page||!materialize)abort();watch_page=page;watch_materialize=materialize;rt_watch_pages=pages;}
void rt_watch_end(void)
{rt_watch_pages=NULL;watch_page=NULL;watch_materialize=NULL;}
void rt_watch_materialize(void)
{void (*callback)(void)=watch_materialize;if(callback)callback();}
void rt_watch_access(uint32_t address,size_t size)
{
    if(!rt_watch_pages||!size)return;
    uint64_t end=(uint64_t)address+size;
    if(end>0x100000000ull)abort();
    uint64_t first=address>RT_WATCH_BASE?address:RT_WATCH_BASE;
    if(end>RT_WATCH_BASE+RT_WATCH_SIZE)end=RT_WATCH_BASE+RT_WATCH_SIZE;
    for(uint64_t p=first&~4095ull;p<end&&rt_watch_pages;p+=4096){
        unsigned index=(p-RT_WATCH_BASE)>>12;
        if(rt_watch_pages[index]){rt_watch_pages[index]=0;watch_page(index);}
    }
}
