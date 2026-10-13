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
#include "bspfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include "bridge_profile.inc"

static enginefuncs_t engine;
static globalvars_t *host_globals;
static uint32_t guest_globals, guest_engine, guest_api, guest_new_api;
static void bridge_missing(const char *direction,unsigned slot);
static void engine_dispatch(Ctx *c,unsigned slot);
static void pm_dispatch(Ctx *c,unsigned slot);
#include "engine_abi.h"
#define BHL_MODEL_NAMESPACE "server-model!"
#include "model_bridge.inc"
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
typedef struct EntitySnapshot {int valid;uint64_t access_epoch;entvars_t host;BhlEntvars guest;} EntitySnapshot;
static EntitySnapshot *entity_snapshots;
static unsigned server_call_depth,server_access_count;
static int server_demand_enabled,server_watch_running;
static uint64_t server_access_serial,server_access_epoch;
static unsigned server_accessed[8192];
static void server_lazy_begin(void);
static void server_access_verify(void);
static void server_lazy_materialize(void);
static void server_watch_suspend(void);
static void server_access_record(unsigned i)
{
    if(server_access_epoch&&entity_snapshots[i].access_epoch!=server_access_epoch){
        entity_snapshots[i].access_epoch=server_access_epoch;
        if(server_access_count>=8192)abort();server_accessed[server_access_count++]=i;
    }
}
static int packet_snapshot_valid,packet_readonly_call;
#include "v3_lifetime.inc"
#define STRING_CAPACITY (4*1024*1024u)
typedef struct StringEntry { uint32_t offset; struct StringEntry *next; } StringEntry;
static StringEntry *strings[4096];
typedef struct StringMap {uint32_t guest;int host;struct StringMap *next;} StringMap;
static StringMap *host_strings[4096],*guest_string_ids[4096];
static unsigned string_bucket(uint32_t id){return (id*2654435761u)>>20;}
static void ensure_entities(void)
{
    if(host_globals->maxEntities<1||host_globals->maxEntities>8192)abort();
    edict_capacity=(unsigned)host_globals->maxEntities;
    edict_t *base=engine.pfnPEntityOfEntIndexAllEntities(0);if(!base)abort();
    if(base!=edict_base&&entity_snapshots)memset(entity_snapshots,0,8192*sizeof *entity_snapshots);
    edict_base=base;
    if(guest_edicts)return;
    /* Native pools can move or grow on a new game. Guest entity addresses
       remain stable, while host pointers resolve against the current pool. */
    guest_edicts=bhl_heap_alloc(8192*sizeof(BhlEdict),1);
    rt_watch_register_range(guest_edicts,8192*sizeof(BhlEdict));
    private_data=calloc(8192,sizeof *private_data);
    entity_snapshots=calloc(8192,sizeof *entity_snapshots);
    guest_strings=bhl_heap_alloc(STRING_CAPACITY,1);
    if(!guest_edicts||!private_data||!guest_strings||!entity_snapshots)abort();
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
    unsigned slot=string_bucket(id);StringMap *m=host_strings[slot];while(m&&m->host!=id)m=m->next;
    if(m&&!strcmp((char*)G_MEM+guest_strings+m->guest,text))return m->guest;
    unsigned hash=2166136261u;for(const unsigned char *p=(const void*)text;*p;p++)hash=(hash^*p)*16777619u;hash&=4095;
    StringEntry *e=strings[hash];while(e&&strcmp((char*)G_MEM+guest_strings+e->offset,text))e=e->next;
    if(!e){size_t length=strlen(text)+1;if(length>STRING_CAPACITY-string_used)abort();
        e=calloc(1,sizeof *e);if(!e)abort();e->offset=string_used;e->next=strings[hash];strings[hash]=e;
        memcpy(G_MEM+guest_strings+string_used,text,length);string_used+=(uint32_t)length;}
    if(!m){m=calloc(1,sizeof *m);if(!m)abort();m->host=id;m->next=host_strings[slot];host_strings[slot]=m;}m->guest=e->offset;return e->offset;
}
static int host_string(uint32_t id)
{
    if(!id)return 0;const char *text=(char*)G_MEM+(uint32_t)(guest_strings+id);unsigned slot=string_bucket(id);
    StringMap *m=guest_string_ids[slot];while(m&&m->guest!=id)m=m->next;
    if(m){const char *existing=engine.pfnSzFromIndex(m->host);if(existing&&!strcmp(existing,text))return m->host;}
    else{m=calloc(1,sizeof *m);if(!m)abort();m->guest=id;m->next=guest_string_ids[slot];guest_string_ids[slot]=m;}
    m->host=engine.pfnAllocString(text);return m->host;
}
typedef struct StringValidation {int host;uint32_t guest;uint64_t batch;int equal;} StringValidation;
static StringValidation string_validations[4096];
static uint64_t string_validation_serial,string_validation_batch;
static int entity_string_equal(uint32_t guest,int host)
{
    if(!host)return 1;
    unsigned slot=((uint32_t)host*2654435761u^guest)&4095;
    StringValidation *p=string_validations+slot;
    if(string_validation_batch&&p->batch==string_validation_batch&&p->host==host&&p->guest==guest)return p->equal;
    const char *text=engine.pfnSzFromIndex(host);
    int equal=text&&!strcmp((char*)G_MEM+(uint32_t)(guest_strings+guest),text);
    *p=(StringValidation){host,guest,string_validation_batch,equal};return equal;
}
static int entity_strings_unchanged(const BhlEntvars *g,const entvars_t *h)
{
    /* String IDs can remain equal when the engine reuses its string pool.
       A byte-identical entvars record alone is not sufficient evidence. */
#define SAME_STRING(field) do{if(!entity_string_equal(g->field,h->field))return 0;}while(0)
    SAME_STRING(classname);SAME_STRING(globalname);SAME_STRING(model);
    SAME_STRING(viewmodel);SAME_STRING(weaponmodel);SAME_STRING(target);
    SAME_STRING(targetname);SAME_STRING(netname);SAME_STRING(message);
    SAME_STRING(noise);SAME_STRING(noise1);SAME_STRING(noise2);SAME_STRING(noise3);
#undef SAME_STRING
    return 1;
}
static void pull_entity(unsigned i)
{
    server_access_record(i);
    edict_t *h=edict_base+i;BhlEdict *g=(void*)(G_MEM+guest_edicts+i*sizeof *g);
    g->free=h->free;g->serialnumber=h->serialnumber;g->headnode=h->headnode;g->num_leafs=h->num_leafs;
    memcpy(g->leafnums,h->leafnums16,sizeof g->leafnums);g->freetime=h->freetime;
    g->pvPrivateData=h->pvPrivateData?private_data[i]:0;
    if(h->free&&!h->pvPrivateData)return;
    EntitySnapshot *snapshot=&entity_snapshots[i];
    if(snapshot->valid&&!memcmp(&snapshot->host,&h->v,sizeof h->v)&&entity_strings_unchanged(&snapshot->guest,&h->v))return;
    copy_entvars_to_guest(&g->v,&h->v);
    snapshot->host=h->v;snapshot->guest=g->v;snapshot->valid=1;
}
static void push_entity(unsigned i)
{
    if(server_access_epoch&&entity_snapshots[i].access_epoch!=server_access_epoch)pull_entity(i);
    BhlEdict *g=(void*)(G_MEM+guest_edicts+i*sizeof *g);edict_t *h=edict_base+i;
    if(h->free&&!h->pvPrivateData)return;
    EntitySnapshot *snapshot=&entity_snapshots[i];
    if(snapshot->valid&&!memcmp(&snapshot->guest,&g->v,sizeof g->v))return;
    copy_entvars_to_host(&h->v,&g->v);
    snapshot->host=h->v;snapshot->guest=g->v;snapshot->valid=1;
}
static void pull_entities(void)
{
    BHL_PROFILE("server-pull",0);ensure_entities();
    if(server_demand_enabled){server_lazy_begin();return;}
    server_lazy_materialize();
    /* String contents can change between boundaries, but not during this
       synchronous scan. Shared class/model IDs need one comparison per scan. */
    if(string_validation_batch||!++string_validation_serial)abort();string_validation_batch=string_validation_serial;
    for(unsigned i=0;i<edict_capacity;i++)pull_entity(i);
    string_validation_batch=0;
}
static void push_entities(void)
{
    BHL_PROFILE("server-push",0);server_access_verify();
    if(server_access_epoch){for(unsigned n=0;n<server_access_count;n++)push_entity(server_accessed[n]);}
    else if(guest_edicts)for(unsigned i=0;i<edict_capacity;i++)push_entity(i);
}
#include "server_entity_access.inc"

static uint32_t encoder_functions[3];
static struct delta_s *active_delta;
static uint32_t visibility_buffers[2];
static uint32_t model_copy(void *host)
{
    /* Share the epoch-aware studio cache; a new level can reuse an address
       for different model data. */
    return studio_data(host);
}
static void trace_to_guest(uint32_t address,const TraceResult *t)
{
    if(guest_edicts&&address-guest_edicts<8192*sizeof(BhlEdict))server_lazy_materialize();
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
static char *gs(uint32_t p)
{
    if(guest_edicts&&p-guest_edicts<8192*sizeof(BhlEdict))server_lazy_materialize();
    return p?(char *)G_MEM+p:NULL;
}
/* Only explicit, synchronous read-only arguments use this bounded borrow. */
static void *gs_read(uint32_t guest,size_t size)
{
    if(!guest)return NULL;
    if(server_access_epoch){
        uint64_t start=guest>guest_edicts?guest:guest_edicts,end=(uint64_t)guest+size;
        uint64_t limit=(uint64_t)guest_edicts+edict_capacity*sizeof(BhlEdict);
        if(end>limit)end=limit;
        if(start<end)for(unsigned i=(start-guest_edicts)/sizeof(BhlEdict);i<(end-guest_edicts+sizeof(BhlEdict)-1)/sizeof(BhlEdict);i++)server_touch(i);
    }
    return G_MEM+guest;
}
static uint32_t copy_string(const char *s)
{
    if(!s)return 0;size_t n=strlen(s)+1;uint32_t p=bhl_heap_alloc((uint32_t)n,0);
    if(!p)abort();memcpy(G_MEM+p,s,n);return p;
}
#include "borrowed_strings.inc"
#include "save_bridge.inc"
typedef struct InfoBuffer {char *host;uint32_t guest;unsigned capacity;struct InfoBuffer *next;} InfoBuffer;
static InfoBuffer *info_buffers;
static uint32_t info_copy(char *host,unsigned capacity)
{
    if(!host)return 0;InfoBuffer *p=info_buffers;while(p&&p->host!=host)p=p->next;
    if(!p){p=calloc(1,sizeof *p);p->host=host;p->capacity=capacity;p->guest=bhl_heap_alloc(capacity,1);p->next=info_buffers;info_buffers=p;}
    size_t n=strnlen(host,p->capacity);if(n==p->capacity)abort();memcpy(G_MEM+p->guest,host,n+1);return p->guest;
}
static char *info_host(uint32_t address)
{for(InfoBuffer *p=info_buffers;p;p=p->next)if(p->guest==address)return p->host;return gs(address);}
static void push_info(void)
{for(InfoBuffer *p=info_buffers;p;p=p->next){size_t n=strnlen(gs(p->guest),p->capacity);if(n==p->capacity)abort();memcpy(p->host,gs(p->guest),n+1);}}
static void pull_info(void)
{for(InfoBuffer *p=info_buffers;p;p=p->next)info_copy(p->host,p->capacity);}
#include "guest_format.inc"
static void sync_cvar(CvarBridge *v)
{
    uint32_t p=v->guest;const char *s=v->host->string?v->host->string:"";
    uint32_t old=rt_r32(G_MEM,p+4);
    if(!old||strcmp(gs(old),s)){bhl_heap_free(old);rt_w32(G_MEM,p+4,copy_string(s));}
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
    rt_w32(G_MEM,guest_globals+156,save_global_pull());
    rt_w32(G_MEM,guest_globals+144,host_globals->maxClients);
    rt_w32(G_MEM,guest_globals+148,host_globals->maxEntities);
    for(CvarBridge *v=cvars;v;v=v->next)sync_cvar(v);
}
static void push_globals(void)
{
    if(!guest_globals||!guest_edicts)return;
    memcpy(host_globals,G_MEM+guest_globals,116);
    host_globals->mapname=host_string(rt_r32(G_MEM,guest_globals+12));host_globals->startspot=host_string(rt_r32(G_MEM,guest_globals+16));
    host_globals->trace_ent=host_edict(rt_r32(G_MEM,guest_globals+116));
    memcpy(&host_globals->trace_inopen,G_MEM+guest_globals+120,32);
    memcpy(host_globals->vecLandmarkOffset,G_MEM+guest_globals+160,12);
    save_global_push();
}
static uint32_t server_call_full(unsigned slot,const uint32_t *args,unsigned n)
{
    packet_snapshot_valid=0;
    if(!server_call_depth++)server_demand_enabled=!bridge_eager_entities;
    sync_basic_globals();pull_info();
    uint32_t result=bhl_invoke(rt_r32(G_MEM,guest_api+4*slot),args,n,0);
    push_entities();push_globals();push_info();server_watch_suspend();
    if(!--server_call_depth)server_demand_enabled=0;return result;
}
static uint32_t server_call(unsigned slot,const uint32_t *args,unsigned n)
{
    BHL_PROFILE("server-export",slot);
    /* Original DispatchObjectCollisionBox RVA 0x43190 delegates through
       vtable +0x30. The base method (0x45bb0) and no-object fallback both
       reach 0x45990, which reads/writes only this entvars and constants.
       Unknown subclass methods keep complete synchronization. */
    if(slot==BHL_DLL_pfnSetAbsBox&&n==1&&args[0]&&rt_r32(G_MEM,guest_api+4*slot)==0x10043190u){
        edict_t *h=host_edict(args[0]);unsigned i=h-edict_base;
        uint32_t object=h->pvPrivateData?private_data[i]:0;
        if(!object||rt_r32(G_MEM,rt_r32(G_MEM,object)+0x30)==0x10045bb0u){
            packet_snapshot_valid=0;pull_entity(i);
            uint32_t r=bhl_invoke(0x10043190u,args,1,0);push_entity(i);return r;
        }
    }
    return server_call_full(slot,args,n);
}
#include "server_scalar_callbacks.h"
static uint32_t command_functions[32];
static unsigned command_count;
#define COMMAND(n) static void command_##n(void){sync_basic_globals();bhl_invoke(command_functions[n],NULL,0,0);push_entities();push_globals();push_info();}
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
#define RAW_RET(v) do{c->eax=(uint32_t)(v);c->esp+=4;return;}while(0)
static void engine_dispatch(Ctx *c,unsigned slot)
{
    BHL_PROFILE("server-engine",slot);
    if(bridge_trace)fprintf(stderr,"engine %s\n",engine_names[slot]);
    /* These APIs cannot observe edicts or globals. Avoid copying the entire
       world when original AI asks for a random value or reads a cvar. */
    switch(slot){
    /* Pinned sv_game.c MessageBegin only starts an engine-owned byte stream
       and stores the destination edict. MessageEnd reads edicts for routing
       (and legacy sound rewriting), but never writes game state or invokes
       game callbacks. Publish pending writes once, immediately before End. */
    case BHL_ENG_pfnMessageBegin:
        engine.pfnMessageBegin(A(0),A(1),(float*)gs_read(A(2),12),host_edict(A(3)));RAW_RET(0);
    case BHL_ENG_pfnMessageEnd:
        packet_snapshot_valid=0;push_entities();push_globals();
        engine.pfnMessageEnd();RAW_RET(0);
    /* MakeVectors only writes these three vectors in native globals. Other
       guest globals and pending entity writes remain owned by the caller. */
    case BHL_ENG_pfnMakeVectors:
        engine.pfnMakeVectors((float*)gs_read(A(0),12));
        memcpy(G_MEM+guest_globals+offsetof(globalvars_t,v_forward),host_globals->v_forward,3*sizeof(vec3_t));RAW_RET(0);
    case BHL_ENG_pfnSzFromIndex:RAW_RET(guest_strings+A(0));
    case BHL_ENG_pfnGetVarsOfEnt:RAW_RET(A(0)?A(0)+offsetof(BhlEdict,v):0);
    case BHL_ENG_pfnFindEntityByVars:RAW_RET(A(0)?A(0)-offsetof(BhlEdict,v):0);
    case BHL_ENG_pfnIndexOfEdict:RAW_RET(engine.pfnIndexOfEdict(host_edict(A(0))));
    case BHL_ENG_pfnPEntityOfEntIndex:RAW_RET(guest_edict(engine.pfnPEntityOfEntIndex(A(0))));
    case BHL_ENG_pfnPEntityOfEntOffset:RAW_RET(guest_edicts+A(0));
    case BHL_ENG_pfnEntOffsetOfPEntity:RAW_RET(A(0)-guest_edicts);
    case BHL_ENG_pfnAllocString:RAW_RET(guest_string(engine.pfnAllocString(gs(A(0)))));
    case BHL_ENG_pfnModelIndex:RAW_RET(engine.pfnModelIndex(gs(A(0))));
    case BHL_ENG_pfnModelFrames:RAW_RET(engine.pfnModelFrames(A(0)));
    case BHL_ENG_pfnGetModelPtr:{
        /* Native validity is engine-owned; the only game-owned input read
           by pinned pfnGetModelPtr is this entity's model index. */
        edict_t *h=host_edict(A(0));if(h){server_touch(h-edict_base);BhlEdict *g=(void*)(G_MEM+A(0));h->v.modelindex=g->v.modelindex;}
        RAW_RET(model_copy(engine.pfnGetModelPtr(h)));
    }
    case BHL_ENG_pfnVecToYaw:FPUSH(engine.pfnVecToYaw((float*)gs_read(A(0),12)));RAW_RET(0);
    case BHL_ENG_pfnVecToAngles:engine.pfnVecToAngles((float*)gs_read(A(0),12),(float*)gs(A(1)));RAW_RET(0);
    case BHL_ENG_pfnAngleVectors:engine.pfnAngleVectors((float*)gs_read(A(0),12),(float*)gs(A(1)),(float*)gs(A(2)),(float*)gs(A(3)));RAW_RET(0);
    case BHL_ENG_pfnTime:FPUSH(engine.pfnTime());RAW_RET(0);
    case BHL_ENG_pfnWriteByte:engine.pfnWriteByte(A(0));RAW_RET(0);
    case BHL_ENG_pfnWriteChar:engine.pfnWriteChar(A(0));RAW_RET(0);
    case BHL_ENG_pfnWriteShort:engine.pfnWriteShort(A(0));RAW_RET(0);
    case BHL_ENG_pfnWriteLong:engine.pfnWriteLong(A(0));RAW_RET(0);
    case BHL_ENG_pfnWriteAngle:engine.pfnWriteAngle(rt_rf32(G_MEM,c->esp+4));RAW_RET(0);
    case BHL_ENG_pfnWriteCoord:engine.pfnWriteCoord(rt_rf32(G_MEM,c->esp+4));RAW_RET(0);
    case BHL_ENG_pfnWriteString:engine.pfnWriteString(gs(A(0)));RAW_RET(0);
    case BHL_ENG_pfnWriteEntity:engine.pfnWriteEntity(A(0));RAW_RET(0);
    case BHL_ENG_pfnFindEntityInSphere:{
        /* Pinned sv_game.c only reads absmin/absmax from entvars here.
           Free/private/client validity stays engine-owned. Preserve native
           search semantics while publishing only the fields it consumes. */
        ensure_entities();
        for(unsigned i=0;i<edict_capacity;i++)if(!edict_base[i].free){
            server_touch(i);
            BhlEdict *g=(void*)(G_MEM+guest_edicts+i*sizeof *g);
            memcpy(edict_base[i].v.absmin,g->v.absmin,sizeof(vec3_t));
            memcpy(edict_base[i].v.absmax,g->v.absmax,sizeof(vec3_t));
        }
        RAW_RET(guest_edict(engine.pfnFindEntityInSphere(host_edict(A(0)),(float*)gs_read(A(1),12),rt_rf32(G_MEM,c->esp+12))));
    }
    case BHL_ENG_pfnRandomLong:RAW_RET(engine.pfnRandomLong((int32_t)A(0),(int32_t)A(1)));
    case BHL_ENG_pfnRandomFloat:FPUSH(engine.pfnRandomFloat(rt_rf32(G_MEM,c->esp+4),rt_rf32(G_MEM,c->esp+8)));RAW_RET(0);
    case BHL_ENG_pfnCVarGetFloat:FPUSH(engine.pfnCVarGetFloat(gs(A(0))));RAW_RET(0);
    case BHL_ENG_pfnCVarGetString:RAW_RET(borrowed_string(engine.pfnCVarGetString(gs(A(0)))));
    case BHL_ENG_pfnIsDedicatedServer:RAW_RET(engine.pfnIsDedicatedServer());
    case BHL_ENG_pfnCRC32_Init:engine.pfnCRC32_Init((CRC32_t*)gs(A(0)));RAW_RET(0);
    case BHL_ENG_pfnCRC32_ProcessBuffer:engine.pfnCRC32_ProcessBuffer((CRC32_t*)gs(A(0)),gs(A(1)),A(2));RAW_RET(0);
    case BHL_ENG_pfnCRC32_ProcessByte:engine.pfnCRC32_ProcessByte((CRC32_t*)gs(A(0)),A(1));RAW_RET(0);
    case BHL_ENG_pfnCRC32_Final:RAW_RET(engine.pfnCRC32_Final(A(0)));
    case BHL_ENG_pfnSetGroupMask:engine.pfnSetGroupMask(A(0),A(1));RAW_RET(0);
    case BHL_ENG_pfnCheckVisibility:
        if(packet_readonly_call)RAW_RET(engine.pfnCheckVisibility(host_edict(A(0)),(byte*)gs(A(1))));
        break;
    default:break;
    }
    packet_snapshot_valid=0;push_entities();push_globals();push_info();server_watch_suspend();
    switch(slot){
    case BHL_ENG_pfnCheckVisibility:RET(engine.pfnCheckVisibility(host_edict(A(0)),(byte*)gs(A(1))));
    case BHL_ENG_pfnFunctionFromName:RET(save_function_address(gs(A(0))));
    case BHL_ENG_pfnNameForFunction:RET(save_function_name(A(0)));
    case BHL_ENG_pfnCRC32_Init:engine.pfnCRC32_Init((CRC32_t*)gs(A(0)));RET(0);
    case BHL_ENG_pfnCRC32_ProcessBuffer:engine.pfnCRC32_ProcessBuffer((CRC32_t*)gs(A(0)),gs(A(1)),A(2));RET(0);
    case BHL_ENG_pfnCRC32_ProcessByte:engine.pfnCRC32_ProcessByte((CRC32_t*)gs(A(0)),A(1));RET(0);
    case BHL_ENG_pfnCRC32_Final:RET(engine.pfnCRC32_Final(A(0)));
    case BHL_ENG_pfnCheckParm:{char *next=NULL;int r=engine.pfnCheckParm(gs(A(0)),A(1)?&next:NULL);if(A(1))rt_w32(G_MEM,A(1),borrowed_string(next));RET(r);}
    case BHL_ENG_pfnCvar_DirectSet:{CvarBridge *v=cvars;while(v&&v->guest!=A(0))v=v->next;if(!v)bridge_missing("engine",slot);engine.pfnCvar_DirectSet(v->host,gs(A(1)));RET(0);}
    case BHL_ENG_pfnSetFatPVS:case BHL_ENG_pfnSetFatPAS:{
        unsigned i=slot==BHL_ENG_pfnSetFatPAS;byte *mask=i?engine.pfnSetFatPAS((float*)gs_read(A(0),12)):engine.pfnSetFatPVS((float*)gs_read(A(0),12));if(!mask)RET(0);
        const unsigned bytes=(MAX_MAP_LEAFS+7)/8; /* Pinned engine owns static arrays of exactly this size. */
        if(!visibility_buffers[i])visibility_buffers[i]=bhl_heap_alloc(bytes,1);memcpy(G_MEM+visibility_buffers[i],mask,bytes);RET(visibility_buffers[i]);
    }
    case BHL_ENG_pfnGetInfoKeyBuffer:{edict_t *e=host_edict(A(0));char *p=engine.pfnGetInfoKeyBuffer(e);unsigned cap=!e?32768:e==edict_base?512:256;if(e&&e!=edict_base&&e-edict_base>host_globals->maxClients)RET(borrowed_string(p));RET(info_copy(p,cap));}
    case BHL_ENG_pfnSetKeyValue:engine.pfnSetKeyValue(info_host(A(0)),gs(A(1)),gs(A(2)));pull_info();RET(0);
    case BHL_ENG_pfnSetClientKeyValue:engine.pfnSetClientKeyValue(A(0),info_host(A(1)),gs(A(2)),gs(A(3)));pull_info();RET(0);
    case BHL_ENG_pfnInfo_RemoveKey:engine.pfnInfo_RemoveKey(info_host(A(0)),gs(A(1)));pull_info();RET(0);
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
    case BHL_ENG_pfnTraceLine:{TraceResult t={0};engine.pfnTraceLine((float*)gs_read(A(0),12),(float*)gs_read(A(1),12),A(2),host_edict(A(3)),&t);trace_to_guest(A(4),&t);RET(0);}
    case BHL_ENG_pfnTraceToss:{TraceResult t={0};engine.pfnTraceToss(host_edict(A(0)),host_edict(A(1)),&t);trace_to_guest(A(2),&t);RET(0);}
    case BHL_ENG_pfnTraceHull:{TraceResult t={0};engine.pfnTraceHull((float*)gs_read(A(0),12),(float*)gs_read(A(1),12),A(2),A(3),host_edict(A(4)),&t);trace_to_guest(A(5),&t);RET(0);}
    case BHL_ENG_pfnTraceModel:{TraceResult t={0};engine.pfnTraceModel((float*)gs_read(A(0),12),(float*)gs_read(A(1),12),A(2),host_edict(A(3)),&t);trace_to_guest(A(4),&t);RET(0);}
    case BHL_ENG_pfnTraceMonsterHull:{TraceResult t={0};int r=engine.pfnTraceMonsterHull(host_edict(A(0)),(float*)gs_read(A(1),12),(float*)gs_read(A(2),12),A(3),host_edict(A(4)),&t);trace_to_guest(A(5),&t);RET(r);}
    case BHL_ENG_pfnTraceSphere:{TraceResult t={0};engine.pfnTraceSphere((float*)gs_read(A(0),12),(float*)gs_read(A(1),12),A(2),rt_rf32(G_MEM,c->esp+16),host_edict(A(4)),&t);trace_to_guest(A(5),&t);RET(0);}
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
    case BHL_ENG_pfnCVarGetString:RET(borrowed_string(engine.pfnCVarGetString(gs(A(0)))));
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
    case BHL_ENG_pfnCmd_Argv:RET(borrowed_string(engine.pfnCmd_Argv(A(0))));
    case BHL_ENG_pfnCmd_Args:RET(borrowed_string(engine.pfnCmd_Args()));
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
static void save_fields(SAVERESTOREDATA *context,const char *name,void *data,TYPEDESCRIPTION *fields,int count,int read)
{
    if(count<0||count>4096)save_fault("field count");SaveMirror *p=save_begin(context);
    unsigned bytes=0;for(int i=0;i<count;i++){
        if(fields[i].fieldSize<0||fields[i].fieldOffset<0)save_fault("field bounds");
        bytes=(bytes+3)&~3u;bytes+=save_field_size(fields[i].fieldType,0)*fields[i].fieldSize;
        if(bytes>16*1024*1024)save_fault("field payload size");
    }
    uint32_t description=bhl_heap_alloc(count*sizeof(BhlSaveField),1),payload=bhl_heap_alloc(bytes?bytes:4,1);unsigned offset=0;
    for(int i=0;i<count;i++){
        BhlSaveField *g=(void*)(G_MEM+description+i*sizeof *g);TYPEDESCRIPTION *h=fields+i;
        offset=(offset+3)&~3u;g->type=h->fieldType;g->name=save_text(h->fieldName);g->offset=offset;g->count=h->fieldSize;g->flags=h->flags;
        /* ReadFields receives output storage, including Xash's uninitialized
           temporary entvars during global-entity level transitions. Only a
           write may import native strings/pointers from that storage. The
           original reader fills the zeroed payload before we copy it back. */
        if(!read)save_field_copy((char*)data+h->fieldOffset,payload+offset,h->fieldType,h->fieldSize,1);
        offset+=save_field_size(h->fieldType,0)*h->fieldSize;
    }
    uint32_t args[]={p->guest,save_text(name),payload,description,count};server_call(read?BHL_DLL_pfnSaveReadFields:BHL_DLL_pfnSaveWriteFields,args,5);save_end(p);
    if(read)for(int i=0;i<count;i++){BhlSaveField *g=(void*)(G_MEM+description+i*sizeof *g);save_field_copy((char*)data+fields[i].fieldOffset,payload+g->offset,fields[i].fieldType,fields[i].fieldSize,0);}
    bhl_heap_free(payload);bhl_heap_free(description);
}
static void save_write_fields(SAVERESTOREDATA *s,const char *n,void *d,TYPEDESCRIPTION *f,int c){save_fields(s,n,d,f,c,0);}
static void save_read_fields(SAVERESTOREDATA *s,const char *n,void *d,TYPEDESCRIPTION *f,int c){save_fields(s,n,d,f,c,1);}
static void save_entity(edict_t *h,SAVERESTOREDATA *s)
{SaveMirror *p=save_begin(s);uint32_t args[]={guest_edict(h),p->guest};server_call(BHL_DLL_pfnSave,args,2);save_end(p);}
static int restore_entity(edict_t *h,SAVERESTOREDATA *s,int global)
{SaveMirror *p=save_begin(s);uint32_t args[]={guest_edict(h),p->guest,global};int r=server_call(BHL_DLL_pfnRestore,args,3);save_end(p);return r;}
static void save_global_state(SAVERESTOREDATA *s)
{SaveMirror *p=save_begin(s);server_call(BHL_DLL_pfnSaveGlobalState,&p->guest,1);save_end(p);}
static void restore_global_state(SAVERESTOREDATA *s)
{SaveMirror *p=save_begin(s);server_call(BHL_DLL_pfnRestoreGlobalState,&p->guest,1);save_end(p);}
static void register_encoders(void) { server_call(BHL_DLL_pfnRegisterEncoders,NULL,0);fprintf(stderr,"BHL v3 registered all three native delta encoders\n"); }
static int hull_bounds(int hull,float *mins,float *maxs)
{
    uint32_t p=bhl_heap_alloc(24,0);memcpy(G_MEM+p,mins,12);memcpy(G_MEM+p+12,maxs,12);
    uint32_t args[]={(uint32_t)hull,p,p+12};int result=server_call(BHL_DLL_pfnGetHullBounds,args,3);
    memcpy(mins,G_MEM+p,12);memcpy(maxs,G_MEM+p+12,12);bhl_heap_free(p);
    if(bridge_view_trace)fprintf(stderr,"BHL hullbounds server %d result=%d min=%.1f,%.1f,%.1f max=%.1f,%.1f,%.1f\n",hull,result,mins[0],mins[1],mins[2],maxs[0],maxs[1],maxs[2]);return result;
}
#include "movement_bridge.inc"
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

static void movement_run(playermove_t *move,qboolean server)
{movement_pull(move);uint32_t args[]={guest_pmove,server};server_call(BHL_DLL_pfnPM_Move,args,2);movement_push();}

static void link_entity(uint32_t function,entvars_t *pev)
{
    edict_t *h=(edict_t*)((char*)pev - offsetof(edict_t,v));
    sync_basic_globals();uint32_t arg=guest_edict(h)+offsetof(BhlEdict,v);
    bhl_invoke(function,&arg,1,0);push_entities();push_globals();
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
static void client_userinfo(edict_t *entity,char *buffer)
{uint32_t args[]={guest_edict(entity),info_copy(buffer,256)};server_call(BHL_DLL_pfnClientUserInfoChanged,args,2);}
static qboolean client_connect(edict_t *entity,const char *name,const char *address,char reason[128])
{
    uint32_t n=copy_string(name),a=copy_string(address),r=bhl_heap_alloc(128,1),args[]={guest_edict(entity),n,a,r};
    memcpy(G_MEM+r,reason,128);int result=server_call(BHL_DLL_pfnClientConnect,args,4);memcpy(reason,G_MEM+r,128);bhl_heap_free(n);bhl_heap_free(a);bhl_heap_free(r);return result;
}
static void update_client_data(const edict_t *entity,int weapons,clientdata_t *data)
{uint32_t p=bhl_heap_alloc(sizeof *data,0),args[]={guest_edict(entity),weapons,p};memcpy(G_MEM+p,data,sizeof *data);server_call(BHL_DLL_pfnUpdateClientData,args,3);memcpy(data,G_MEM+p,sizeof *data);bhl_heap_free(p);}
static int weapon_data(edict_t *entity,weapon_data_t *data)
{uint32_t bytes=64*sizeof *data,p=bhl_heap_alloc(bytes,0),args[]={guest_edict(entity),p};memcpy(G_MEM+p,data,bytes);int r=server_call(BHL_DLL_pfnGetWeaponData,args,2);memcpy(data,G_MEM+p,bytes);bhl_heap_free(p);return r;}
static void cmd_start(const edict_t *entity,const usercmd_t *cmd,unsigned seed)
{uint32_t p=bhl_heap_alloc(sizeof *cmd,0),args[]={guest_edict(entity),p,seed};memcpy(G_MEM+p,cmd,sizeof *cmd);server_call(BHL_DLL_pfnCmdStart,args,3);bhl_heap_free(p);}
static void setup_visibility(edict_t *view,edict_t *client,byte **pvs,byte **pas)
{
    uint32_t p=bhl_heap_alloc(8,1),args[]={guest_edict(view),guest_edict(client),p,p+4};server_call(BHL_DLL_pfnSetupVisibility,args,4);
    uint32_t v=rt_r32(G_MEM,p),a=rt_r32(G_MEM,p+4);*pvs=v?G_MEM+v:NULL;*pas=a?G_MEM+a:NULL;bhl_heap_free(p);
    server_lazy_materialize();packet_snapshot_valid=1;
}
static int add_full_pack(entity_state_t *state,int index,edict_t *entity,edict_t *host,int flags,int player,byte *set)
{
    uint32_t mask=0;
    if(set){uintptr_t address=(uintptr_t)set-(uintptr_t)G_MEM;if(address>UINT32_MAX){fprintf(stderr,"BHL native bridge: unrecognized visibility buffer\n");abort();}mask=address;}
    uint32_t p=bhl_heap_alloc(sizeof *state,0),args[]={p,index,guest_edict(entity),guest_edict(host),flags,player,mask};memcpy(G_MEM+p,state,sizeof *state);
    /* Pinned v3 AddToFullPack writes the output state and its own group-mask
       globals only. Xash calls SetupVisibility once before this read-only
       packet loop. Reuse that world snapshot instead of copying every entity
       again for every output entity (quadratic work). Other server callbacks
       and non-query engine calls invalidate the snapshot. */
    int result;
    if(packet_snapshot_valid){packet_readonly_call=1;result=bhl_invoke(rt_r32(G_MEM,guest_api+4*BHL_DLL_pfnAddToFullPack),args,7,0);packet_readonly_call=0;}
    else result=server_call(BHL_DLL_pfnAddToFullPack,args,7);
    memcpy(state,G_MEM+p,sizeof *state);bhl_heap_free(p);return result;
}
static void server_deactivate(void){server_call(BHL_DLL_pfnServerDeactivate,NULL,0);model_reset();}
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
    uint32_t result=bhl_invoke(fn,args,count,0);push_entities();push_globals();return result;
}
static void free_private(edict_t *entity)
{
    uint32_t arg=guest_edict(entity);unsigned i=(unsigned)(entity-edict_base);
    v3_retire_sprite(private_data[i]);
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
    out->pfnGetHullBounds=hull_bounds;out->pfnPM_Init=movement_init;out->pfnPM_Move=movement_run;
    out->pfnRegisterEncoders=register_encoders;out->pfnKeyValue=key_value;out->pfnSpawn=spawn_entity;out->pfnServerDeactivate=server_deactivate;
    out->pfnServerActivate=server_activate;out->pfnStartFrame=start_frame;
    out->pfnCreateBaseline=create_baseline;
    out->pfnClientUserInfoChanged=client_userinfo;out->pfnClientConnect=client_connect;
    out->pfnUpdateClientData=update_client_data;out->pfnGetWeaponData=weapon_data;out->pfnCmdStart=cmd_start;
    out->pfnSetupVisibility=setup_visibility;out->pfnAddToFullPack=add_full_pack;
    out->pfnSave=save_entity;out->pfnRestore=restore_entity;out->pfnSaveWriteFields=save_write_fields;out->pfnSaveReadFields=save_read_fields;
    out->pfnSaveGlobalState=save_global_state;out->pfnRestoreGlobalState=restore_global_state;
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
