/* Load data sections only. PE instructions are never executed by this loader. */
#include "loader.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>

extern uint8_t *rt_test_mem(void);
extern int rt_test_run(Ctx *,uint32_t,uint32_t *,const char **);
static Ctx root_context;
static __thread Ctx *active_context;
static int initialized;
static uint8_t attached[16];
static void fail(const char *what) { fprintf(stderr,"BHL native loader: %s\n",what);abort(); }
static uint16_t u16(const uint8_t *b,size_t p) { uint16_t v;memcpy(&v,b+p,2);return v; }
static uint32_t u32(const uint8_t *b,size_t p) { uint32_t v;memcpy(&v,b+p,4);return v; }

void bhl_initialize(void)
{
    if(initialized)return;
    const char *directory=getenv("BHL_IMAGE_DIR");
    if(!directory)fail("BHL_IMAGE_DIR must point to the verified prepared images");
    if(!rt_test_mem())fail("cannot reserve guest address space");
    for(unsigned k=0;k<bhl_module_count;k++){
        const BhlModule *m=&bhl_modules[k];char path[4096];
        if(snprintf(path,sizeof path,"%s/%s",directory,m->name)>=(int)sizeof path)fail("image path too long");
        FILE *f=fopen(path,"rb");if(!f)fail(path);
        if(fseek(f,0,SEEK_END))fail("image seek failed");long length=ftell(f);rewind(f);
        if(length<256||length>32*1024*1024)fail("invalid PE file size");
        uint8_t *bytes=malloc((size_t)length);if(!bytes||fread(bytes,1,(size_t)length,f)!=(size_t)length)fail("image read failed");fclose(f);
        unsigned char hash[32];char hex[65];CC_SHA256(bytes,(CC_LONG)length,hash);
        for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",hash[i]);
        if(strcmp(hex,m->sha256))fail("prepared image hash differs from compiled translation");
        uint32_t pe=u32(bytes,0x3c),optional=pe+24;
        if(pe>(uint32_t)length-256||u32(bytes,pe)!=0x4550||u16(bytes,pe+4)!=0x14c||u16(bytes,optional)!=0x10b)fail("invalid PE32 header");
        if(u32(bytes,optional+28)!=m->base||u32(bytes,optional+56)!=m->size)fail("PE layout differs from build metadata");
        uint32_t headers=u32(bytes,optional+60);if(headers>(uint32_t)length||headers>m->size)fail("invalid header size");
        memset(G_MEM+m->base,0,m->size);memcpy(G_MEM+m->base,bytes,headers);
        unsigned n=u16(bytes,pe+6);uint32_t sections=optional+u16(bytes,pe+20);
        if((uint64_t)sections+40*n>(uint64_t)length)fail("invalid section table");
        for(unsigned i=0;i<n;i++){
            uint32_t p=sections+40*i,va=u32(bytes,p+12),size=u32(bytes,p+16),raw=u32(bytes,p+20);
            if((uint64_t)va+size>m->size||(uint64_t)raw+size>(uint64_t)length)fail("invalid PE section bounds");
            memcpy(G_MEM+m->base+va,bytes+raw,size);
        }
        free(bytes);
    }
    bhl_set_trace(getenv("BHL_TRACE")!=NULL);
    bhl_process_init(&root_context);initialized=1;
}

uint32_t bhl_export(const char *module,const char *name)
{
    for(unsigned k=0;k<bhl_module_count;k++)if(!strcmp(module,bhl_modules[k].name))
        for(unsigned i=0;i<bhl_modules[k].export_count;i++)if(!strcmp(name,bhl_modules[k].exports[i].name))return bhl_modules[k].exports[i].address;
    return 0;
}
static uint32_t invoke_context(uint32_t function,uint32_t object,const uint32_t *args,unsigned count,int stdcall,int thiscall)
{
    if(!function||count>32)fail("invalid guest call");
    Ctx *parent=active_context,*source=parent?parent:&root_context,child=*source;
    uint32_t sp=source->esp-16-4*(count+1);
    if(sp<rt_r32(G_MEM,source->fs_base+8)+0x1000)fail("guest stack exhausted");
    child.esp=sp;if(thiscall)child.ecx=object;rt_w32(G_MEM,sp,0x00100000);
    for(unsigned i=0;i<count;i++)rt_w32(G_MEM,sp+4+4*i,args[i]);
    active_context=&child;uint32_t pc=0;const char *why=NULL;
    int result=rt_test_run(&child,function,&pc,&why);active_context=parent;
    if(result){fprintf(stderr,"BHL guest call %08x failed at %08x: %s\n",function,pc,why);abort();}
    /* Windows discards the entire stack when a thread entry returns. Legacy
       FMOD passes cdecl entries to CreateThread; both return conventions are
       therefore accepted only for that terminal call (mode 2). */
    int stack_ok=stdcall==2?(child.esp==sp+4||child.esp==sp+4+4*count):child.esp==sp+4+(stdcall?4*count:0);
    if(!stack_ok){fprintf(stderr,"BHL guest call %08x returned stack %08x from %08x\n",function,child.esp,sp);fail("guest calling convention/stack mismatch");}
    return child.eax;
}
uint32_t bhl_invoke(uint32_t function,const uint32_t *args,unsigned count,int stdcall)
{ return invoke_context(function,0,args,count,stdcall,0); }
uint32_t bhl_thiscall(uint32_t function,uint32_t object,const uint32_t *args,unsigned count)
{ if(!object)fail("null thiscall object");return invoke_context(function,object,args,count,1,1); }
void bhl_attach(const char *module)
{
    bhl_initialize();
    for(unsigned i=0;i<bhl_module_count;i++)if(!strcmp(module,bhl_modules[i].name)){
        if(attached[i])return;
        uint32_t args[]={bhl_modules[i].base,1,0};
        if(bhl_invoke(bhl_modules[i].entry,args,3,1)!=1)fail("DLL_PROCESS_ATTACH rejected");
        attached[i]=1;return;
    }
    fail("module missing from native build");
}
static void thread_notifications(unsigned reason)
{
    extern int bhl_thread_notifications_enabled(uint32_t);
    for(unsigned i=0;i<bhl_module_count;i++)if(attached[i]&&bhl_thread_notifications_enabled(bhl_modules[i].base)){
        uint32_t args[]={bhl_modules[i].base,reason,0};bhl_invoke(bhl_modules[i].entry,args,3,1);
    }
}
void bhl_enter_guest_thread(Ctx *context){active_context=context;thread_notifications(2);}
void bhl_leave_guest_thread(void){extern void bhl_fls_thread_exit(void);thread_notifications(3);bhl_fls_thread_exit();active_context=NULL;}
