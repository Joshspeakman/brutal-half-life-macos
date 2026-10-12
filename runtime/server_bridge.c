/* Native Xash3D -> 32-bit recompiled v3 server ABI. Incomplete entries fail by
 * name. Returning a callable table does not imply gameplay compatibility. */
#include "loader.h"
#include "xash3d_types.h"
#include "progdefs.h"
#include "edict.h"
#include "eiface.h"
#include "q_client.h"
#include "pmove.h"
struct hull_s;
#include "pm_defs.h"
#include "entity_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>

static enginefuncs_t engine;
static globalvars_t *host_globals;
static uint32_t guest_globals, guest_engine, guest_api, guest_new_api;
static void bridge_missing(const char *direction,unsigned slot);
static void engine_dispatch(Ctx *c,unsigned slot);
static void pm_dispatch(Ctx *c,unsigned slot);
#include "engine_abi.h"
#define BHL_PM_NAMESPACE "server-movement!"
#include "guest_movement.h"
extern int bhl_heap_free(uint32_t);
static playermove_t *host_pmove;
static uint32_t guest_pmove;
static uint32_t guest_edict(const edict_t *);
static edict_t *host_edict(uint32_t);
static uint32_t guest_string(int);
static int host_string(uint32_t);
#include "guest_entities.h"
static edict_t *edict_base;
static uint32_t guest_edicts, guest_strings, string_used=1;
static uint32_t *private_data;
static unsigned edict_capacity;
#define STRING_CAPACITY (4*1024*1024u)
typedef struct StringEntry { uint32_t offset; struct StringEntry *next; } StringEntry;
static StringEntry *strings;
static void ensure_entities(void)
{
    if(guest_edicts)return;
    if(host_globals->maxEntities<1||host_globals->maxEntities>8192)abort();
    edict_capacity=(unsigned)host_globals->maxEntities;
    edict_base=engine.pfnPEntityOfEntIndexAllEntities(0);if(!edict_base)abort();
    guest_edicts=bhl_heap_alloc(edict_capacity*sizeof(BhlEdict),1);
    private_data=calloc(edict_capacity,sizeof *private_data);
    guest_strings=bhl_heap_alloc(STRING_CAPACITY,1);
    if(!guest_edicts||!private_data||!guest_strings)abort();
}
static uint32_t guest_edict(const edict_t *host)
{
    if(!host)return 0;ensure_entities();
    ptrdiff_t i=host-edict_base;if(i<0||(unsigned)i>=edict_capacity)abort();
    return guest_edicts+(uint32_t)i*sizeof(BhlEdict);
}
static edict_t *host_edict(uint32_t p)
{
    if(!p)return NULL;ensure_entities();uint32_t delta=p-guest_edicts;
    if(delta%sizeof(BhlEdict)||delta/sizeof(BhlEdict)>=edict_capacity)abort();
    return edict_base+delta/sizeof(BhlEdict);
}
static uint32_t guest_string(int id)
{
    if(!id)return 0;ensure_entities();const char *text=engine.pfnSzFromIndex(id);
    if(!text||!*text)return 0;
    for(StringEntry *e=strings;e;e=e->next)if(!strcmp((char*)G_MEM+guest_strings+e->offset,text))return e->offset;
    size_t length=strlen(text)+1;if(length>STRING_CAPACITY-string_used)abort();
    StringEntry *e=calloc(1,sizeof *e);if(!e)abort();e->offset=string_used;e->next=strings;strings=e;
    memcpy(G_MEM+guest_strings+string_used,text,length);string_used+=(uint32_t)length;return e->offset;
}
static int host_string(uint32_t id)
{
    if(!id)return 0;return engine.pfnAllocString((char*)G_MEM+(uint32_t)(guest_strings+id));
}
static void pull_entities(void)
{
    ensure_entities();
    for(unsigned i=0;i<edict_capacity;i++){
        edict_t *h=edict_base+i;BhlEdict *g=(void*)(G_MEM+guest_edicts+i*sizeof *g);
        g->free=h->free;g->serialnumber=h->serialnumber;g->headnode=h->headnode;g->num_leafs=h->num_leafs;
        memcpy(g->leafnums,h->leafnums16,sizeof g->leafnums);g->freetime=h->freetime;
        g->pvPrivateData=h->pvPrivateData?private_data[i]:0;
        copy_entvars_to_guest(&g->v,&h->v);
    }
}
static void push_entities(void)
{
    if(!guest_edicts)return;
    for(unsigned i=0;i<edict_capacity;i++){
        BhlEdict *g=(void*)(G_MEM+guest_edicts+i*sizeof *g);edict_t *h=edict_base+i;
        copy_entvars_to_host(&h->v,&g->v);
    }
}

static uint32_t encoder_functions[3];
static struct delta_s *active_delta;
typedef struct ModelCopy { const void *host; uint32_t guest; struct ModelCopy *next; } ModelCopy;
static ModelCopy *model_copies;
static uint32_t model_copy(void *host)
{
    if(!host)return 0;
    for(ModelCopy *m=model_copies;m;m=m->next)if(m->host==host)return m->guest;
    uint32_t id,version,length;memcpy(&id,host,4);memcpy(&version,(char*)host+4,4);memcpy(&length,(char*)host+72,4);
    if(id!=0x54534449u||version!=10||length<244||length>32*1024*1024){fprintf(stderr,"BHL native bridge: unexpected studio model layout\n");abort();}
    ModelCopy *m=calloc(1,sizeof *m);if(!m)abort();m->host=host;m->guest=bhl_heap_alloc(length,0);if(!m->guest)abort();
    memcpy(G_MEM+m->guest,host,length);m->next=model_copies;model_copies=m;return m->guest;
}
static void trace_to_guest(uint32_t address,const TraceResult *t)
{
    memcpy(G_MEM+address,t,48);rt_w32(G_MEM,address+48,guest_edict(t->pHit));rt_w32(G_MEM,address+52,t->iHitgroup);
}
#define DELTA_HANDLE 0x7e000100u
static void encode(unsigned slot,struct delta_s *delta,const unsigned char *from,const unsigned char *to)
{
    struct delta_s *previous=active_delta;active_delta=delta;
    uint32_t p=bhl_heap_alloc(2*sizeof(entity_state_t),0);if(!p)abort();
    memcpy(G_MEM+p,from,sizeof(entity_state_t));memcpy(G_MEM+p+sizeof(entity_state_t),to,sizeof(entity_state_t));
    uint32_t args[]={DELTA_HANDLE,p,p+sizeof(entity_state_t)};
    bhl_invoke(encoder_functions[slot],args,3,0);bhl_heap_free(p);active_delta=previous;
}
static void encode_entity(struct delta_s *d,const unsigned char *f,const unsigned char *t){encode(0,d,f,t);}
static void encode_custom(struct delta_s *d,const unsigned char *f,const unsigned char *t){encode(1,d,f,t);}
static void encode_player(struct delta_s *d,const unsigned char *f,const unsigned char *t){encode(2,d,f,t);}
static void bridge_missing(const char *direction,unsigned slot)
{
    const char *name=direction[0]=='s'?(slot<50?server_names[slot]:"new interface"):(slot<159?engine_names[slot]:"unknown");
    fprintf(stderr,"BHL native bridge: unimplemented %s callback %u (%s)\n",direction,slot,name);abort();
}
typedef struct CvarBridge { uint32_t guest; cvar_t *host; struct CvarBridge *next; } CvarBridge;
static CvarBridge *cvars;
static char *gs(uint32_t p) { return p?(char *)G_MEM+p:NULL; }
static uint32_t copy_string(const char *s)
{
    if(!s)return 0;size_t n=strlen(s)+1;uint32_t p=bhl_heap_alloc((uint32_t)n,0);
    if(!p)abort();memcpy(G_MEM+p,s,n);return p;
}
#include "guest_format.inc"
static void sync_cvar(CvarBridge *v)
{
    uint32_t p=v->guest;const char *s=v->host->string?v->host->string:"";
    uint32_t old=rt_r32(G_MEM,p+4);
    if(!old||strcmp(gs(old),s))rt_w32(G_MEM,p+4,copy_string(s));
    rt_w32(G_MEM,p+8,v->host->flags);rt_wf32(G_MEM,p+12,v->host->value);
}
static uint32_t marshal_cvar(cvar_t *host,uint32_t preferred)
{
    if(!host)return 0;
    for(CvarBridge *v=cvars;v;v=v->next)if(v->host==host){sync_cvar(v);return v->guest;}
    CvarBridge *v=calloc(1,sizeof *v);if(!v)abort();v->host=host;
    v->guest=preferred?preferred:bhl_heap_alloc(20,1);v->next=cvars;cvars=v;
    rt_w32(G_MEM,v->guest,copy_string(host->name));sync_cvar(v);return v->guest;
}
static void sync_basic_globals(void)
{
    /* Through trace_plane_dist the two layouts are identical (29 words).
       Pointer-bearing fields are marshaled separately as they are supported. */
    pull_entities();
    memcpy(G_MEM+guest_globals,host_globals,116);
    rt_w32(G_MEM,guest_globals+12,guest_string(host_globals->mapname));
    rt_w32(G_MEM,guest_globals+16,guest_string(host_globals->startspot));
    rt_w32(G_MEM,guest_globals+116,guest_edict(host_globals->trace_ent));
    memcpy(G_MEM+guest_globals+120,&host_globals->trace_inopen,32);
    rt_w32(G_MEM,guest_globals+152,guest_strings);
    memcpy(G_MEM+guest_globals+160,host_globals->vecLandmarkOffset,12);
    if(host_globals->pSaveData){fprintf(stderr,"BHL native bridge: save-state structure conversion is not implemented\n");abort();}
    rt_w32(G_MEM,guest_globals+144,host_globals->maxClients);
    rt_w32(G_MEM,guest_globals+148,host_globals->maxEntities);
    for(CvarBridge *v=cvars;v;v=v->next)sync_cvar(v);
}
static uint32_t server_call(unsigned slot,const uint32_t *args,unsigned n)
{
    sync_basic_globals();
    uint32_t result=bhl_invoke(rt_r32(G_MEM,guest_api+4*slot),args,n,0);
    push_entities();return result;
}
#include "server_scalar_callbacks.h"
static uint32_t command_functions[32];
static unsigned command_count;
#define COMMAND(n) static void command_##n(void){sync_basic_globals();bhl_invoke(command_functions[n],NULL,0,0);}
COMMAND(0) COMMAND(1) COMMAND(2) COMMAND(3) COMMAND(4) COMMAND(5) COMMAND(6) COMMAND(7)
COMMAND(8) COMMAND(9) COMMAND(10) COMMAND(11) COMMAND(12) COMMAND(13) COMMAND(14) COMMAND(15)
COMMAND(16) COMMAND(17) COMMAND(18) COMMAND(19) COMMAND(20) COMMAND(21) COMMAND(22) COMMAND(23)
COMMAND(24) COMMAND(25) COMMAND(26) COMMAND(27) COMMAND(28) COMMAND(29) COMMAND(30) COMMAND(31)
static void (*commands[])(void)={command_0,command_1,command_2,command_3,command_4,command_5,command_6,command_7,
    command_8,command_9,command_10,command_11,command_12,command_13,command_14,command_15,
    command_16,command_17,command_18,command_19,command_20,command_21,command_22,command_23,
    command_24,command_25,command_26,command_27,command_28,command_29,command_30,command_31};
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define RET(v) do{uint32_t result_=(uint32_t)(v);sync_basic_globals();c->eax=result_;c->esp+=4;return;}while(0)
static void engine_dispatch(Ctx *c,unsigned slot)
{
    if(getenv("BHL_BRIDGE_TRACE"))fprintf(stderr,"engine %s\n",engine_names[slot]);
    push_entities();
    switch(slot){
    case BHL_ENG_pfnAlertMessage:{char text[8192];format_guest(text,gs(A(1)),c->esp+12);engine.pfnAlertMessage((ALERT_TYPE)A(0),"%s",text);RET(0);}
    case BHL_ENG_pfnClientCommand:{char text[8192];format_guest(text,gs(A(1)),c->esp+12);engine.pfnClientCommand(host_edict(A(0)),"%s",text);RET(0);}
    case BHL_ENG_pfnClientPrintf:engine.pfnClientPrintf(host_edict(A(0)),(PRINT_TYPE)A(1),gs(A(2)));RET(0);
    case BHL_ENG_pfnLoadFileForMe:{
        int size=0;byte *file=engine.pfnLoadFileForMe(gs(A(0)),&size);
        if(A(1))rt_w32(G_MEM,A(1),size);if(!file)RET(0);if(size<0)abort();
        uint32_t p=bhl_heap_alloc((uint32_t)size+1,1);if(!p)abort();memcpy(G_MEM+p,file,size);engine.pfnFreeFile(file);RET(p);
    }
    case BHL_ENG_pfnFreeFile:if(!bhl_heap_free(A(0)))abort();RET(0);
    case BHL_ENG_pfnGetModelPtr:RET(model_copy(engine.pfnGetModelPtr(host_edict(A(0)))));
    case BHL_ENG_pfnTraceLine:{TraceResult t={0};engine.pfnTraceLine((float*)gs(A(0)),(float*)gs(A(1)),A(2),host_edict(A(3)),&t);trace_to_guest(A(4),&t);RET(0);}
    case BHL_ENG_pfnTraceToss:{TraceResult t={0};engine.pfnTraceToss(host_edict(A(0)),host_edict(A(1)),&t);trace_to_guest(A(2),&t);RET(0);}
    case BHL_ENG_pfnTraceHull:{TraceResult t={0};engine.pfnTraceHull((float*)gs(A(0)),(float*)gs(A(1)),A(2),A(3),host_edict(A(4)),&t);trace_to_guest(A(5),&t);RET(0);}
    case BHL_ENG_pfnTraceModel:{TraceResult t={0};engine.pfnTraceModel((float*)gs(A(0)),(float*)gs(A(1)),A(2),host_edict(A(3)),&t);trace_to_guest(A(4),&t);RET(0);}
    case BHL_ENG_pfnTraceMonsterHull:{TraceResult t={0};int r=engine.pfnTraceMonsterHull(host_edict(A(0)),(float*)gs(A(1)),(float*)gs(A(2)),A(3),host_edict(A(4)),&t);trace_to_guest(A(5),&t);RET(r);}
    case BHL_ENG_pfnTraceSphere:{TraceResult t={0};engine.pfnTraceSphere((float*)gs(A(0)),(float*)gs(A(1)),A(2),rt_rf32(G_MEM,c->esp+16),host_edict(A(4)),&t);trace_to_guest(A(5),&t);RET(0);}
    case BHL_ENG_pfnPvAllocEntPrivateData:{
        edict_t *h=host_edict(A(0));if(!h)abort();unsigned i=(unsigned)(h-edict_base);uint32_t size=A(1);
        void *data=engine.pfnPvAllocEntPrivateData(h,(int32_t)size);
        if(private_data[i])bhl_heap_free(private_data[i]);private_data[i]=data?bhl_heap_alloc(size,1):0;
        RET(private_data[i]);
    }
    case BHL_ENG_pfnPvEntPrivateData:{edict_t *h=host_edict(A(0));RET(h?private_data[h-edict_base]:0);}
    case BHL_ENG_pfnFreeEntPrivateData:{edict_t *h=host_edict(A(0));if(!h)abort();unsigned i=(unsigned)(h-edict_base);
        engine.pfnFreeEntPrivateData(h);if(private_data[i])bhl_heap_free(private_data[i]);private_data[i]=0;RET(0);}
    case BHL_ENG_pfnAllocString:RET(guest_string(engine.pfnAllocString(gs(A(0)))));
    case BHL_ENG_pfnSzFromIndex:RET(guest_strings+A(0));
    case BHL_ENG_pfnGetVarsOfEnt:RET(A(0)?A(0)+offsetof(BhlEdict,v):0);
    case BHL_ENG_pfnFindEntityByVars:RET(A(0)?A(0)-offsetof(BhlEdict,v):0);
    case BHL_ENG_pfnIndexOfEdict:RET(engine.pfnIndexOfEdict(host_edict(A(0))));
    case BHL_ENG_pfnPEntityOfEntIndex:RET(guest_edict(engine.pfnPEntityOfEntIndex(A(0))));
    case BHL_ENG_pfnPEntityOfEntOffset:RET(guest_edicts+A(0));
    case BHL_ENG_pfnEntOffsetOfPEntity:RET(A(0)-guest_edicts);
    case BHL_ENG_pfnCreateEntity:RET(guest_edict(engine.pfnCreateEntity()));
    case BHL_ENG_pfnCreateNamedEntity:RET(guest_edict(engine.pfnCreateNamedEntity(host_string(A(0)))));
    case BHL_ENG_pfnRemoveEntity:engine.pfnRemoveEntity(host_edict(A(0)));RET(0);
    case BHL_ENG_pfnCVarRegister:case BHL_ENG_pfnCvar_RegisterVariable:{
        uint32_t p=A(0);cvar_t *v=calloc(1,sizeof *v);if(!v)abort();
        v->name=strdup(gs(rt_r32(G_MEM,p)));v->string=strdup(gs(rt_r32(G_MEM,p+4)));v->flags=rt_r32(G_MEM,p+8);v->value=rt_rf32(G_MEM,p+12);
        engine.pfnCVarRegister(v);cvar_t *registered=engine.pfnCVarGetPointer(v->name);
        if(!registered){fprintf(stderr,"cvar registration failed: %s\n",v->name);abort();}
        marshal_cvar(registered,p);RET(0);
    }
    case BHL_ENG_pfnCVarGetPointer:RET(marshal_cvar(engine.pfnCVarGetPointer(gs(A(0))),0));
    case BHL_ENG_pfnCVarGetFloat:FPUSH(engine.pfnCVarGetFloat(gs(A(0))));RET(0);
    case BHL_ENG_pfnCVarGetString:RET(copy_string(engine.pfnCVarGetString(gs(A(0)))));
    case BHL_ENG_pfnCVarSetFloat:engine.pfnCVarSetFloat(gs(A(0)),rt_rf32(G_MEM,c->esp+8));RET(0);
    case BHL_ENG_pfnCVarSetString:engine.pfnCVarSetString(gs(A(0)),gs(A(1)));RET(0);
    case BHL_ENG_pfnAddServerCommand:
        if(command_count>=32){fprintf(stderr,"too many guest server commands\n");abort();}
        command_functions[command_count]=A(1);engine.pfnAddServerCommand(gs(A(0)),commands[command_count]);command_count++;RET(0);
    case BHL_ENG_pfnServerCommand:engine.pfnServerCommand(gs(A(0)));RET(0);
    case BHL_ENG_pfnServerExecute:engine.pfnServerExecute();RET(0);
    case BHL_ENG_pfnServerPrint:engine.pfnServerPrint(gs(A(0)));RET(0);
    case BHL_ENG_pfnGetGameDir:engine.pfnGetGameDir(gs(A(0)));RET(0);
    case BHL_ENG_pfnCmd_Argc:RET(engine.pfnCmd_Argc());
    case BHL_ENG_pfnCmd_Argv:RET(copy_string(engine.pfnCmd_Argv(A(0))));
    case BHL_ENG_pfnCmd_Args:RET(copy_string(engine.pfnCmd_Args()));
    case BHL_ENG_pfnIsDedicatedServer:RET(engine.pfnIsDedicatedServer());
    case BHL_ENG_pfnRandomLong:RET(engine.pfnRandomLong((int32_t)A(0),(int32_t)A(1)));
    case BHL_ENG_pfnRandomFloat:FPUSH(engine.pfnRandomFloat(rt_rf32(G_MEM,c->esp+4),rt_rf32(G_MEM,c->esp+8)));RET(0);
    case BHL_ENG_pfnDeltaAddEncoder:{
        const char *name=gs(A(0));unsigned i;
        if(!strcmp(name,"Entity_Encode"))i=0;else if(!strcmp(name,"Custom_Encode"))i=1;else if(!strcmp(name,"Player_Encode"))i=2;else{bridge_missing("engine",slot);return;}
        encoder_functions[i]=A(1);engine.pfnDeltaAddEncoder((char*)name,i==0?encode_entity:i==1?encode_custom:encode_player);RET(0);
    }
    case BHL_ENG_pfnDeltaFindField:case BHL_ENG_pfnDeltaSetField:case BHL_ENG_pfnDeltaUnsetField:
    case BHL_ENG_pfnDeltaSetFieldByIndex:case BHL_ENG_pfnDeltaUnsetFieldByIndex:
        if(A(0)!=DELTA_HANDLE||!active_delta){fprintf(stderr,"invalid delta encoder handle\n");abort();}
        if(slot==BHL_ENG_pfnDeltaFindField)RET(engine.pfnDeltaFindField(active_delta,gs(A(1))));
        if(slot==BHL_ENG_pfnDeltaSetField)engine.pfnDeltaSetField(active_delta,gs(A(1)));
        if(slot==BHL_ENG_pfnDeltaUnsetField)engine.pfnDeltaUnsetField(active_delta,gs(A(1)));
        if(slot==BHL_ENG_pfnDeltaSetFieldByIndex)engine.pfnDeltaSetFieldByIndex(active_delta,A(1));
        if(slot==BHL_ENG_pfnDeltaUnsetFieldByIndex)engine.pfnDeltaUnsetFieldByIndex(active_delta,A(1));RET(0);
    case BHL_ENG_pfnGetCurrentPlayer:RET(engine.pfnGetCurrentPlayer());
#include "engine_scalar_callbacks.h"
    default:bridge_missing("engine",slot);
    }
}
static void game_init(void) { server_call(BHL_DLL_pfnGameInit,NULL,0);fprintf(stderr,"BHL v3 GameInit completed through native engine callbacks\n"); }
static const char *game_description(void) { return gs(server_call(BHL_DLL_pfnGetGameDescription,NULL,0)); }
static int lag_compensation(void) { return server_call(BHL_DLL_pfnAllowLagCompensation,NULL,0); }
static void reset_global_state(void) { server_call(BHL_DLL_pfnResetGlobalState,NULL,0); }
static void register_encoders(void) { server_call(BHL_DLL_pfnRegisterEncoders,NULL,0);fprintf(stderr,"BHL v3 registered all three native delta encoders\n"); }
static int hull_bounds(int hull,float *mins,float *maxs)
{
    uint32_t p=bhl_heap_alloc(24,0);memcpy(G_MEM+p,mins,12);memcpy(G_MEM+p+12,maxs,12);
    uint32_t args[]={(uint32_t)hull,p,p+12};int result=server_call(BHL_DLL_pfnGetHullBounds,args,3);
    memcpy(mins,G_MEM+p,12);memcpy(maxs,G_MEM+p+12,12);bhl_heap_free(p);return result;
}
static void pm_dispatch(Ctx *c,unsigned slot)
{
    if(getenv("BHL_BRIDGE_TRACE"))fprintf(stderr,"movement %s\n",pm_names[slot]);
    switch(slot){
    case BHL_PM_COM_FileSize:RET(host_pmove->COM_FileSize(gs(A(0))));
    case BHL_PM_COM_LoadFile:{
        int n=0;byte *file=host_pmove->COM_LoadFile(gs(A(0)),A(1),&n);
        if(A(2))rt_w32(G_MEM,A(2),n);if(!file)RET(0);
        if(n<0)abort();uint32_t p=bhl_heap_alloc((uint32_t)n+1,1);if(!p)abort();
        memcpy(G_MEM+p,file,n);host_pmove->COM_FreeFile(file);RET(p);
    }
    case BHL_PM_COM_FreeFile:if(!bhl_heap_free(A(0)))abort();RET(0);
    case BHL_PM_memfgets:{
        uint32_t p=A(0),offset=A(2),buffer=A(3);int pos=(int)rt_r32(G_MEM,offset),size=(int)A(1),cap=(int)A(4);
        if(!p||!offset||!buffer||cap<=0||pos<0||pos>=size)RET(0);
        int n=0;while(pos<size&&n<cap-1){char ch=G_MEM[p+pos++];G_MEM[buffer+n++]=ch;if(ch=='\n')break;}
        G_MEM[buffer+n]=0;rt_w32(G_MEM,offset,pos);RET(buffer);
    }
    default:fprintf(stderr,"BHL native bridge: unimplemented movement callback %s\n",pm_names[slot]);abort();
    }
}
static void movement_init(playermove_t *move)
{
    host_pmove=move;
    if(move->numphysent||move->nummoveent||move->numvisent){fprintf(stderr,"PM_Init received populated collision entities\n");abort();}
    guest_pmove=bhl_heap_alloc(sizeof(BhlPlayerMove),1);BhlPlayerMove *g=(void*)(G_MEM+guest_pmove);
    pm_copy_values(g,move);pm_fill_callbacks(g);
    g->movevars=bhl_heap_alloc(sizeof(movevars_t),1);if(move->movevars)memcpy(G_MEM+g->movevars,move->movevars,sizeof(movevars_t));
    server_call(BHL_DLL_pfnPM_Init,&guest_pmove,1);
    fprintf(stderr,"BHL v3 PM_Init completed through native movement callbacks\n");
}

static void link_entity(uint32_t function,entvars_t *pev)
{
    edict_t *h=(edict_t*)((char*)pev - offsetof(edict_t,v));
    sync_basic_globals();uint32_t arg=guest_edict(h)+offsetof(BhlEdict,v);
    bhl_invoke(function,&arg,1,0);push_entities();
}
#include "entity_exports.h"
static void key_value(edict_t *entity,KeyValueData *key)
{
    uint32_t data=bhl_heap_alloc(16,1),a=copy_string(key->szClassName),b=copy_string(key->szKeyName),d=copy_string(key->szValue);
    rt_w32(G_MEM,data,a);rt_w32(G_MEM,data+4,b);rt_w32(G_MEM,data+8,d);rt_w32(G_MEM,data+12,key->fHandled);
    uint32_t args[]={guest_edict(entity),data};server_call(BHL_DLL_pfnKeyValue,args,2);
    key->fHandled=rt_r32(G_MEM,data+12);bhl_heap_free(a);bhl_heap_free(b);bhl_heap_free(d);bhl_heap_free(data);
}
static int spawn_entity(edict_t *entity){uint32_t arg=guest_edict(entity);return server_call(BHL_DLL_pfnSpawn,&arg,1);}
static void server_deactivate(void){server_call(BHL_DLL_pfnServerDeactivate,NULL,0);}
static unsigned frame_count;
static void server_activate(edict_t *list,int count,int clients)
{
    uint32_t args[]={guest_edict(list),(uint32_t)count,(uint32_t)clients};server_call(BHL_DLL_pfnServerActivate,args,3);
    frame_count=0;fprintf(stderr,"BHL v3 ServerActivate completed\n");
}
static void start_frame(void)
{
    server_call(BHL_DLL_pfnStartFrame,NULL,0);
    if(++frame_count==10)fprintf(stderr,"BHL v3 completed 10 server frames\n");
}
static void create_baseline(int player,int index,entity_state_t *baseline,edict_t *entity,int model,vec3_t mins,vec3_t maxs)
{
    _Static_assert(sizeof(entity_state_t)==340,"GoldSrc entity_state ABI");
    uint32_t p=bhl_heap_alloc(sizeof(entity_state_t)+24,0);if(!p)abort();
    memcpy(G_MEM+p,baseline,sizeof *baseline);memcpy(G_MEM+p+sizeof *baseline,mins,12);memcpy(G_MEM+p+sizeof *baseline+12,maxs,12);
    uint32_t args[]={(uint32_t)player,(uint32_t)index,p,guest_edict(entity),(uint32_t)model,p+sizeof *baseline,p+sizeof *baseline+12};
    server_call(BHL_DLL_pfnCreateBaseline,args,7);memcpy(baseline,G_MEM+p,sizeof *baseline);bhl_heap_free(p);
}
static uint32_t new_call(unsigned slot,const uint32_t *args,unsigned count)
{
    sync_basic_globals();uint32_t fn=rt_r32(G_MEM,guest_new_api+4*slot);
    uint32_t result=bhl_invoke(fn,args,count,0);push_entities();return result;
}
static void free_private(edict_t *entity)
{
    uint32_t arg=guest_edict(entity);unsigned i=(unsigned)(entity-edict_base);
    new_call(0,&arg,1);if(private_data[i])bhl_heap_free(private_data[i]);private_data[i]=0;
}
static void game_shutdown(void){new_call(1,NULL,0);}
static int should_collide(edict_t *a,edict_t *b){uint32_t args[]={guest_edict(a),guest_edict(b)};return new_call(2,args,2);}
static void cvar_value(const edict_t *entity,const char *value)
{
    uint32_t text=copy_string(value),args[]={guest_edict(entity),text};new_call(3,args,2);bhl_heap_free(text);
}
static void cvar_value2(const edict_t *entity,int request,const char *name,const char *value)
{
    uint32_t n=copy_string(name),v=copy_string(value),args[]={guest_edict(entity),(uint32_t)request,n,v};
    new_call(4,args,4);bhl_heap_free(n);bhl_heap_free(v);
}

__attribute__((visibility("default"))) void GiveFnptrsToDll(enginefuncs_t *functions,globalvars_t *globals)
{
    engine=*functions;host_globals=globals;bhl_attach("hl.dll");
    guest_globals=bhl_heap_alloc(172,1);guest_engine=bhl_heap_alloc(159*4,1);
    guest_api=bhl_heap_alloc(50*4,1);guest_new_api=bhl_heap_alloc(5*4,1);
    for(unsigned i=0;i<159;i++){
        char name[128];snprintf(name,sizeof name,"engine!%s",engine_names[i]);
        rt_w32(G_MEM,guest_engine+4*i,bhl_thunk(name,engine_callbacks[i]));
    }
    uint32_t args[]={guest_engine,guest_globals};
    bhl_invoke(bhl_export("hl.dll","GiveFnptrsToDll"),args,2,1);
}
__attribute__((visibility("default"))) int GetEntityAPI(DLL_FUNCTIONS *out,int version)
{
    if(!out||version!=140)return 0;
    uint32_t args[]={guest_api,(uint32_t)version};
    if(!bhl_invoke(bhl_export("hl.dll","GetEntityAPI"),args,2,0))return 0;
    fill_missing(out);fill_scalar_server(out);out->pfnGameInit=game_init;out->pfnGetGameDescription=game_description;
    out->pfnAllowLagCompensation=lag_compensation;out->pfnResetGlobalState=reset_global_state;
    out->pfnGetHullBounds=hull_bounds;out->pfnPM_Init=movement_init;
    out->pfnRegisterEncoders=register_encoders;out->pfnKeyValue=key_value;out->pfnSpawn=spawn_entity;out->pfnServerDeactivate=server_deactivate;
    out->pfnServerActivate=server_activate;out->pfnStartFrame=start_frame;
    out->pfnCreateBaseline=create_baseline;
    return 1;
}
__attribute__((visibility("default"))) int GetEntityAPI2(DLL_FUNCTIONS *out,int *version)
{
    if(!version)return 0;if(*version!=140){*version=140;return 0;}return GetEntityAPI(out,*version);
}
__attribute__((visibility("default"))) int GetNewDLLFunctions(NEW_DLL_FUNCTIONS *out,int *version)
{
    if(!out||!version)return 0;if(*version!=1){*version=1;return 0;}
    uint32_t v=bhl_heap_alloc(4,1);rt_w32(G_MEM,v,1);uint32_t args[]={guest_new_api,v};
    int ok=bhl_invoke(bhl_export("hl.dll","GetNewDLLFunctions"),args,2,0);bhl_heap_free(v);if(!ok)return 0;
    memset(out,0,sizeof *out);
    if(rt_r32(G_MEM,guest_new_api))out->pfnOnFreeEntPrivateData=free_private;
    if(rt_r32(G_MEM,guest_new_api+4))out->pfnGameShutdown=game_shutdown;
    if(rt_r32(G_MEM,guest_new_api+8))out->pfnShouldCollide=should_collide;
    if(rt_r32(G_MEM,guest_new_api+12))out->pfnCvarValue=cvar_value;
    if(rt_r32(G_MEM,guest_new_api+16))out->pfnCvarValue2=cvar_value2;
    return 1;
}
