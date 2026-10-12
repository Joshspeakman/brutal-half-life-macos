/* Native client boundary. Missing adapters stop with their name; no guest
 * object or function pointer is cast to a host pointer. */
#include "loader.h"
#include "xash3d_types.h"
#include "q_client.h"
#include "cvardef.h"
#include "const.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "ref_params.h"
#include "cdll_int.h"
#include "event_args.h"
#include "pmove.h"
struct hull_s;
#include "pm_defs.h"
#include "triangleapi.h"
#include "r_efx.h"
#include "event_api.h"
#include "demo_api.h"
#include "net_api.h"
#include "ivoicetweak.h"
#include <stdio.h>
#include <stdlib.h>
struct mstudioevent_s;
struct r_studio_interface_s;
struct engine_studio_api_s;
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define RET(v) do { c->eax=(uint32_t)(v);c->esp+=4;return; } while(0)
extern int bhl_heap_free(uint32_t);
static cl_enginefunc_t engine;
static uint32_t guest_engine;
static void client_missing(const char *name){fprintf(stderr,"BHL native client: unimplemented %s\n",name);abort();}
static char *gs(uint32_t p){return p?(char*)G_MEM+p:NULL;}
#include "guest_format.inc"
static uint32_t copy_bytes(const void *data,size_t size)
{
    if(!data)return 0;if(size>0x10000000)abort();uint32_t p=bhl_heap_alloc((uint32_t)size,0);if(!p)abort();memcpy(G_MEM+p,data,size);return p;
}
static uint32_t copy_string(const char *text){return text?copy_bytes(text,strlen(text)+1):0;}
static void sync_cvars(void);
static uint32_t invoke(const char *name,const uint32_t *args,unsigned count)
{
    sync_cvars();
    uint32_t fn=bhl_export("client.dll",name);if(!fn)client_missing(name);return bhl_invoke(fn,args,count,0);
}
static uint32_t command_guest[128],message_guest[128],event_guest[128];
static unsigned command_count,message_count,event_count;
static void run_command(unsigned slot){bhl_invoke(command_guest[slot],NULL,0,0);}
static int run_message(unsigned slot,const char *name,int size,void *data)
{
    if(size<0)abort();uint32_t n=copy_string(name),p=copy_bytes(data,size),args[]={n,(uint32_t)size,p};
    int result=bhl_invoke(message_guest[slot],args,3,0);bhl_heap_free(n);bhl_heap_free(p);return result;
}
static void run_event(unsigned slot,event_args_t *event)
{
    _Static_assert(sizeof(event_args_t)==72,"guest event layout");
    uint32_t p=copy_bytes(event,sizeof *event);bhl_invoke(event_guest[slot],&p,1,0);memcpy(event,G_MEM+p,sizeof *event);bhl_heap_free(p);
}
static uint32_t client_subtable(const char *,const void *);
static void client_dispatch(Ctx *,unsigned);
#include "client_engine_abi.h"
#include "client_subtables.h"
typedef struct CvarCopy { cvar_t *host;uint32_t guest,string;size_t capacity;struct CvarCopy *next; } CvarCopy;
static CvarCopy *cvars;
static uint32_t cvar_copy(cvar_t *h)
{
    if(!h)return 0;CvarCopy *p=cvars;while(p&&p->host!=h)p=p->next;
    if(!p){p=calloc(1,sizeof *p);if(!p)abort();p->host=h;p->guest=bhl_heap_alloc(20,1);rt_w32(G_MEM,p->guest,copy_string(h->name));p->next=cvars;cvars=p;}
    size_t size=strlen(h->string)+1;if(size>p->capacity){bhl_heap_free(p->string);p->capacity=size+32;p->string=bhl_heap_alloc(p->capacity,0);}
    memcpy(G_MEM+p->string,h->string,size);rt_w32(G_MEM,p->guest+4,p->string);rt_w32(G_MEM,p->guest+8,h->flags);rt_wf32(G_MEM,p->guest+12,h->value);
    return p->guest;
}
static void sync_cvars(void){for(CvarCopy *p=cvars;p;p=p->next)cvar_copy(p->host);}
static void client_dispatch(Ctx *c,unsigned slot)
{
    if(getenv("BHL_BRIDGE_TRACE"))fprintf(stderr,"client engine %s\n",client_engine_names[slot]);
    switch(slot){
    case BHL_CL_Con_Printf:case BHL_CL_Con_DPrintf:{char text[8192];format_guest(text,gs(A(0)),c->esp+8);if(slot==BHL_CL_Con_Printf)engine.Con_Printf("%s",text);else engine.Con_DPrintf("%s",text);RET(0);}
    case BHL_CL_Con_NPrintf:{char text[8192];format_guest(text,gs(A(1)),c->esp+12);engine.Con_NPrintf(A(0),"%s",text);RET(0);}
    case BHL_CL_pfnRegisterVariable:RET(cvar_copy(engine.pfnRegisterVariable(gs(A(0)),gs(A(1)),A(2))));
    case BHL_CL_pfnGetCvarPointer:RET(cvar_copy(engine.pfnGetCvarPointer(gs(A(0)))));
    case BHL_CL_pfnAddCommand:{unsigned i=command_count++;if(i>=128)abort();command_guest[i]=A(1);RET(engine.pfnAddCommand(gs(A(0)),commands[i]));}
    case BHL_CL_pfnHookUserMsg:{unsigned i=message_count++;if(i>=128)abort();message_guest[i]=A(1);RET(engine.pfnHookUserMsg(gs(A(0)),messages[i]));}
    case BHL_CL_pfnHookEvent:{unsigned i=event_count++;if(i>=128)abort();event_guest[i]=A(1);engine.pfnHookEvent(gs(A(0)),events[i]);RET(0);}
    case BHL_CL_COM_ExpandFilename:RET(engine.COM_ExpandFilename(gs(A(0)),gs(A(1)),A(2)));
    case BHL_CL_CheckParm:{char *next=NULL;int result=engine.CheckParm(gs(A(0)),A(1)?&next:NULL);if(A(1))rt_w32(G_MEM,A(1),copy_string(next));RET(result);}
    case BHL_CL_COM_LoadFile:{
        int size=0;byte *data=engine.COM_LoadFile(gs(A(0)),A(1),&size);if(A(2))rt_w32(G_MEM,A(2),size);if(!data)RET(0);
        if(size<0)abort();uint32_t p=bhl_heap_alloc((uint32_t)size+1,1);if(!p)abort();memcpy(G_MEM+p,data,size);engine.COM_FreeFile(data);RET(p);
    }
    case BHL_CL_COM_FreeFile:if(!bhl_heap_free(A(0)))abort();RET(0);
    case BHL_CL_COM_ParseFile:{char *original=gs(A(0));char *end=engine.COM_ParseFile(original,gs(A(1)));if(end&&(end<original||end>original+strlen(original)))abort();RET(end?A(0)+(uint32_t)(end-original):0);}
    case BHL_CL_pfnGetScreenInfo:{_Static_assert(sizeof(SCREENINFO)==532,"screen info ABI");RET(engine.pfnGetScreenInfo((void*)(G_MEM+A(0))));}
    case BHL_CL_pfnSPR_GetList:{int count=0;client_sprite_t *list=engine.pfnSPR_GetList(gs(A(0)),&count);if(count<0||count>65536)abort();if(A(1))rt_w32(G_MEM,A(1),count);RET(copy_bytes(list,count*sizeof *list));}
    case BHL_CL_VGui_GetPanel:{void *panel=engine.VGui_GetPanel();if((uintptr_t)panel>UINT32_MAX)client_missing("native VGUI object cannot be used by the guest client");RET((uintptr_t)panel);}
#include "client_scalar_callbacks.h"
    default:client_missing(client_engine_names[slot]);
    }
}
EXPORT int Initialize(cl_enginefunc_t *functions,int version)
{
    if(!functions||version!=7)return 0;engine=*functions;
    bhl_initialize();bhl_attach("vgui.dll");bhl_attach("client.dll");
    guest_engine=bhl_heap_alloc(135*4,1);fill_client_engine(guest_engine);
    uint32_t args[]={guest_engine,(uint32_t)version};int result=invoke("Initialize",args,2);
    fprintf(stderr,"BHL v3 client Initialize returned %d; registered %u events\n",result,event_count);return result;
}
EXPORT void HUD_Init(void){invoke("HUD_Init",NULL,0);fprintf(stderr,"BHL v3 HUD_Init completed\n");}
EXPORT int HUD_VidInit(void){return invoke("HUD_VidInit",NULL,0);}
EXPORT void HUD_Shutdown(void){invoke("HUD_Shutdown",NULL,0);}
EXPORT void HUD_Reset(void){invoke("HUD_Reset",NULL,0);}
EXPORT void IN_ActivateMouse(void){invoke("IN_ActivateMouse",NULL,0);}
EXPORT void IN_DeactivateMouse(void){invoke("IN_DeactivateMouse",NULL,0);}
EXPORT void IN_ClearStates(void){invoke("IN_ClearStates",NULL,0);}
EXPORT void IN_Accumulate(void){invoke("IN_Accumulate",NULL,0);}
EXPORT int HUD_Redraw(float time,int intermission){uint32_t args[2];memcpy(args,&time,4);args[1]=intermission;return invoke("HUD_Redraw",args,2);}
EXPORT void HUD_Frame(double time){uint32_t args[2];memcpy(args,&time,8);invoke("HUD_Frame",args,2);}
EXPORT void *KB_Find(const char *name)
{
    /* kbutton is three 32-bit integers and contains no pointers. The engine
       expects a live view so key-state changes must be shared, not copied. */
    uint32_t text=copy_string(name),p=invoke("KB_Find",&text,1);bhl_heap_free(text);
    return p?G_MEM+p:NULL;
}
EXPORT int HUD_GetHullBounds(int hull,float *mins,float *maxs)
{
    uint32_t p=bhl_heap_alloc(24,1),args[]={(uint32_t)hull,p,p+12};int result=invoke("HUD_GetHullBounds",args,3);
    memcpy(mins,G_MEM+p,12);memcpy(maxs,G_MEM+p+12,12);bhl_heap_free(p);return result;
}
static playermove_t *host_pmove;
static void pm_dispatch(Ctx *,unsigned);
#define BHL_PM_NAMESPACE "client-movement!"
#include "guest_movement.h"
static void pm_dispatch(Ctx *c,unsigned slot)
{
    switch(slot){
    case BHL_PM_COM_FileSize:RET(host_pmove->COM_FileSize(gs(A(0))));
    case BHL_PM_COM_LoadFile:{
        int size=0;byte *data=host_pmove->COM_LoadFile(gs(A(0)),A(1),&size);if(A(2))rt_w32(G_MEM,A(2),size);if(!data)RET(0);
        if(size<0)abort();uint32_t p=bhl_heap_alloc((uint32_t)size+1,1);if(!p)abort();memcpy(G_MEM+p,data,size);host_pmove->COM_FreeFile(data);RET(p);
    }
    case BHL_PM_COM_FreeFile:if(!bhl_heap_free(A(0)))abort();RET(0);
    case BHL_PM_memfgets:{
        uint32_t p=A(0),offset=A(2),buffer=A(3);int pos=(int)rt_r32(G_MEM,offset),size=(int)A(1),cap=(int)A(4);
        if(!p||!offset||!buffer||cap<=0||pos<0||pos>=size)RET(0);
        int n=0;while(pos<size&&n<cap-1){char ch=G_MEM[p+pos++];G_MEM[buffer+n++]=ch;if(ch=='\n')break;}
        G_MEM[buffer+n]=0;rt_w32(G_MEM,offset,pos);RET(buffer);
    }
    default:client_missing(pm_names[slot]);
    }
}
EXPORT void HUD_PlayerMoveInit(playermove_t *move)
{
    if(move->numphysent||move->nummoveent||move->numvisent)client_missing("populated PM_Init collision entities");
    host_pmove=move;uint32_t p=bhl_heap_alloc(sizeof(BhlPlayerMove),1);BhlPlayerMove *g=(void*)(G_MEM+p);
    pm_copy_values(g,move);pm_fill_callbacks(g);g->movevars=bhl_heap_alloc(sizeof(movevars_t),1);
    if(move->movevars)memcpy(G_MEM+g->movevars,move->movevars,sizeof(movevars_t));
    invoke("HUD_PlayerMoveInit",&p,1);fprintf(stderr,"BHL v3 client movement initialization completed\n");
}
#include "client_missing_exports.h"
