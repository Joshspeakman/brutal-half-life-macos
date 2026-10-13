/* Native test callbacks use guest ABI stacks; no Python frames are crossed by
 * cooperative thread termination. Linked only into the test fixture. */
#include "loader.h"
#include <stdlib.h>
static uint32_t api(Ctx *context,const char *name,const uint32_t *args,unsigned count)
{
    Ctx child=*context;child.esp-=4+4*count;rt_w32(G_MEM,child.esp,0x00100000);
    for(unsigned i=0;i<count;i++)rt_w32(G_MEM,child.esp+4+4*i,args[i]);
    rt_call(&child,bhl_thunk(name,NULL));if(child.esp!=context->esp)abort();return child.eax;
}
void test_tls_entry(Ctx *c)
{
    uint32_t p=rt_r32(G_MEM,c->esp+4),args[]={rt_r32(G_MEM,p),rt_r32(G_MEM,p+4)};
    uint32_t result=api(c,"KERNEL32.dll!TlsSetValue",args,2),delay=10;
    api(c,"KERNEL32.dll!Sleep",&delay,1);
    rt_w32(G_MEM,p+8,result?api(c,"KERNEL32.dll!TlsGetValue",args,1):0xffffffff);
    rt_w32(G_MEM,p+12,api(c,"KERNEL32.dll!GetCurrentThreadId",NULL,0));c->eax=args[1];c->esp+=8;
}
void test_wait_entry(Ctx *c)
{
    uint32_t p=rt_r32(G_MEM,c->esp+4),args[]={rt_r32(G_MEM,p),0xffffffff};rt_w32(G_MEM,p+4,1);
    c->eax=api(c,"KERNEL32.dll!WaitForSingleObject",args,2);c->esp+=8;
}
void test_cdecl_entry(Ctx *c){test_tls_entry(c);c->esp-=4;}
void test_critical_entry(Ctx *c)
{
    uint32_t p=rt_r32(G_MEM,c->esp+4),lock=rt_r32(G_MEM,p),delay=100;
    api(c,"KERNEL32.dll!EnterCriticalSection",&lock,1);rt_w32(G_MEM,p+4,1);
    api(c,"KERNEL32.dll!Sleep",&delay,1);rt_w32(G_MEM,p+8,1);
    api(c,"KERNEL32.dll!LeaveCriticalSection",&lock,1);delay=0xffffffff;
    api(c,"KERNEL32.dll!Sleep",&delay,1);c->eax=1;c->esp+=8;
}
