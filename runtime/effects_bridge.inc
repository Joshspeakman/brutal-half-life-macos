/* Temporary entities retain the original linked lists, embedded entities and
 * guest callbacks. Native engine callback addresses never enter guest memory. */
typedef struct BhlTempEntity {
    int flags;float die,frameMax,x,y,z,fadeSpeed,bounceFactor;int hitSound;
    uint32_t hitcallback,callback,next;int priority;short clientIndex,padding;
    vec3_t tentOffset;BhlClientEntity entity;
} BhlTempEntity;
_Static_assert(offsetof(BhlTempEntity,entity)==68,"tempent prefix ABI");
_Static_assert(sizeof(BhlTempEntity)==3068,"tempent ABI");
typedef struct TempCopy {
    TEMPENTITY *host;uint32_t guest,hit,callback;
    int valid;
    uint64_t access_epoch;
    unsigned char host_snapshot[offsetof(TEMPENTITY,entity)],guest_snapshot[offsetof(BhlTempEntity,entity)];
    EntityCopy *entity;
    void (*native_hit)(TEMPENTITY *,pmtrace_t *);
    void (*native_callback)(TEMPENTITY *,float,float);
    struct TempCopy *next,*host_next,*guest_next,*access_next;
} TempCopy;
static TempCopy *temps;
static TempCopy *temp_accessed;
static TempCopy *temp_hosts[4096],*temp_guests[4096];
static unsigned temp_host_bucket(const void *p){return ((uintptr_t)p/8*2654435761u)&4095;}
static unsigned temp_guest_bucket(uint32_t p){return (p/4*2654435761u)&4095;}
static uint32_t temp_hit_thunk,temp_callback_thunk;
static int (*temp_add_visible)(cl_entity_t *);
static void (*temp_play_sound)(TEMPENTITY *,float);
static TEMPENTITY **temp_free_head,**temp_active_head;
static uint32_t temp_guest_heads;
typedef struct LightCopy {dlight_t *host;uint32_t guest;struct LightCopy *next;} LightCopy;
static LightCopy *lights;
static uint32_t light_guest(dlight_t *h)
{
    _Static_assert(sizeof(dlight_t)==40,"dynamic light ABI");if(!h)return 0;
    LightCopy *p=lights;while(p&&p->host!=h)p=p->next;
    if(!p){p=calloc(1,sizeof *p);if(!p)abort();p->host=h;p->guest=bhl_heap_alloc(sizeof *h,1);p->next=lights;lights=p;}
    memcpy(G_MEM+p->guest,h,sizeof *h);return p->guest;
}
#include "particle_bridge.inc"
static TempCopy *temp_find(TEMPENTITY *h)
{
    unsigned bucket=temp_host_bucket(h);for(TempCopy *p=temp_hosts[bucket];p;p=p->host_next)if(p->host==h)return p;
    TempCopy *p=calloc(1,sizeof *p);if(!p)abort();p->host=h;p->guest=entity_copy(&h->entity)-68;
    p->entity=entity_find_guest(p->guest+68);if(!p->entity)abort();entity_promote(p->entity,4);p->entity->active=1;p->entity->temp=p;
    p->next=temps;temps=p;p->host_next=temp_hosts[bucket];temp_hosts[bucket]=p;
    bucket=temp_guest_bucket(p->guest);p->guest_next=temp_guests[bucket];temp_guests[bucket]=p;return p;
}
static TEMPENTITY *temp_host(uint32_t p)
{if(!p)return NULL;for(TempCopy *t=temp_guests[temp_guest_bucket(p)];t;t=t->guest_next)if(t->guest==p)return t->host;client_missing("unknown tempent");return NULL;}
static void temp_entities_prepare(int guest)
{
    if(!temp_guest_heads)return;
    /* The free list's next pointers must stay mirrored, but its unused 3 KiB
       entity bodies are not renderer inputs. Walk the actual active list on
       each side so allocations made during an update become visible before
       synchronization; never infer activity from expiry time or flags. */
    for(TempCopy *p=temps;p;p=p->next)p->entity->active=0;
    TEMPENTITY *at=guest?temp_host(rt_r32(G_MEM,temp_guest_heads+4)):*temp_active_head;
    unsigned count=0;
    while(at){
        if(++count>65536)client_missing("cyclic active tempent list");
        TempCopy *p=temp_find(at);p->entity->active=1;
        if(guest&&entity_access_epoch)temp_lazy_pull(p);
        at=guest?temp_host(rt_r32(G_MEM,p->guest+offsetof(BhlTempEntity,next))):at->next;
    }
}
static void temp_hit_native(TEMPENTITY *h,pmtrace_t *trace)
{
    TempCopy *p=temp_find(h);uint32_t t=copy_bytes(trace,sizeof *trace),a[]={p->guest,t};
    invoke_address(p->hit,a,2);memcpy(trace,G_MEM+t,sizeof *trace);bhl_heap_free(t);
}
static void temp_callback_native(TEMPENTITY *h,float frame,float time)
{TempCopy *p=temp_find(h);uint32_t a[]={p->guest,0,0};memcpy(a+1,&frame,4);memcpy(a+2,&time,4);invoke_address(p->callback,a,3);}
static void temp_hit_guest(Ctx *c)
{
    entities_push();TempCopy *p=temp_find(temp_host(A(0)));if(!p->native_hit)client_missing("tempent hit callback");
    p->native_hit(p->host,(void*)gs(A(1)));RET(0);
}
static void temp_callback_guest(Ctx *c)
{
    entities_push();TempCopy *p=temp_find(temp_host(A(0)));if(!p->native_callback)client_missing("tempent callback");
    p->native_callback(p->host,rt_rf32(G_MEM,c->esp+8),rt_rf32(G_MEM,c->esp+12));RET(0);
}
static void temp_pull(TempCopy *p)
{
    if(entity_access_epoch&&p->access_epoch!=entity_access_epoch){p->access_epoch=entity_access_epoch;p->access_next=temp_accessed;temp_accessed=p;}
    TEMPENTITY *h=p->host;BhlTempEntity *g=(void*)(G_MEM+p->guest);
    if(p->valid&&!memcmp(p->host_snapshot,h,sizeof p->host_snapshot))return;
    memcpy(g,h,36);
    if(h->hitcallback!=temp_hit_native)p->native_hit=h->hitcallback;
    if(h->callback!=temp_callback_native)p->native_callback=h->callback;
    g->hitcallback=h->hitcallback==temp_hit_native?p->hit:h->hitcallback?temp_hit_thunk:0;
    g->callback=h->callback==temp_callback_native?p->callback:h->callback?temp_callback_thunk:0;
    g->next=h->next?temp_find(h->next)->guest:0;g->priority=h->priority;g->clientIndex=h->clientIndex;memcpy(g->tentOffset,h->tentOffset,12);
    memcpy(p->host_snapshot,h,sizeof p->host_snapshot);memcpy(p->guest_snapshot,g,sizeof p->guest_snapshot);p->valid=1;
}
static void temp_lazy_pull(TempCopy *p)
{if(p->access_epoch!=entity_access_epoch)temp_pull(p);}
static void temp_lazy_begin(void){temp_accessed=NULL;}
static int temp_lazy_pending(void)
{for(TempCopy *p=temp_accessed;p;p=p->access_next)if(p->valid&&memcmp(p->guest_snapshot,G_MEM+p->guest,sizeof p->guest_snapshot))return 1;return 0;}
static void temp_lazy_materialize(uint64_t epoch)
{for(TempCopy *p=temps;p;p=p->next)if(p->access_epoch!=epoch)temp_pull(p);temp_accessed=NULL;}
static void temp_access_verify(void)
{
    for(TempCopy *p=temps;p;p=p->next)if(p->valid&&p->access_epoch!=entity_access_epoch&&memcmp(p->guest_snapshot,G_MEM+p->guest,sizeof p->guest_snapshot)){
        fprintf(stderr,"BHL untracked tempent header write: %08x\n",p->guest);abort();
    }
}
static uint32_t temp_guest(TEMPENTITY *h)
{
    if(!h)return 0;
    if(!temp_hit_thunk){temp_hit_thunk=bhl_thunk("client-temp!hit",temp_hit_guest);temp_callback_thunk=bhl_thunk("client-temp!callback",temp_callback_guest);}
    /* Pre-register every successor before exposing the list to original code. */
    unsigned count=0;for(TEMPENTITY *at=h;at;at=at->next){if(++count>65536)client_missing("cyclic tempent list");temp_find(at);}
    temp_find(h)->valid=0;
    for(TempCopy *p=temps;p;p=p->next)temp_pull(p);
    TempCopy *first=temp_find(h);first->entity->active=1;entity_pull(first->entity);return first->guest;
}
static uint32_t temp_custom_alloc(const float *origin,model_t *model,int high,uint32_t callback)
{
    TEMPENTITY *h=engine.pEfxAPI->CL_TentEntAllocCustom(origin,model,high,callback?temp_callback_native:NULL);if(!h)return 0;
    TempCopy *p=temp_find(h);p->callback=callback;return temp_guest(h);
}
static uint32_t projectile_guest_functions[128],tracer_guest_functions[128];
static unsigned projectile_count,tracer_count;
static unsigned effect_callback_slot(uint32_t *functions,unsigned *count,uint32_t fn)
{for(unsigned i=0;i<*count;i++)if(functions[i]==fn)return i;if(*count>=128)client_missing("effect callback capacity");unsigned i=(*count)++;functions[i]=fn;return i;}
static void (*projectile_callback(uint32_t fn))(TEMPENTITY *,pmtrace_t *)
{return fn?projectile_callbacks[effect_callback_slot(projectile_guest_functions,&projectile_count,fn)]:NULL;}
static void (*tracer_callback(uint32_t fn))(particle_t *)
{return fn?tracer_callbacks[effect_callback_slot(tracer_guest_functions,&tracer_count,fn)]:NULL;}
static void projectile_hit(unsigned slot,TEMPENTITY *h,pmtrace_t *trace)
{
    TempCopy *p=temp_find(h);p->hit=projectile_guest_functions[slot];h->hitcallback=temp_hit_native;temp_guest(h);
    uint32_t t=copy_bytes(trace,sizeof *trace),args[]={p->guest,t};invoke_address(p->hit,args,2);memcpy(trace,G_MEM+t,sizeof *trace);bhl_heap_free(t);
}
static void tracer_death(unsigned slot,particle_t *h)
{
    ParticleCopy *p=particle_find(h);p->die_fn=tracer_guest_functions[slot];h->on_die=particle_die_native;
    uint32_t g=particle_guest(h);invoke_address(p->die_fn,&g,1);
}
static void effects_pull(void)
{
    temp_entities_prepare(0);
    particles_pull();beams_pull();
    for(LightCopy *p=lights;p;p=p->next)memcpy(G_MEM+p->guest,p->host,sizeof *p->host);
    if(!entity_demand_enabled)for(TempCopy *p=temps;p;p=p->next)temp_pull(p);
    if(temp_guest_heads){rt_w32(G_MEM,temp_guest_heads,*temp_free_head?temp_find(*temp_free_head)->guest:0);rt_w32(G_MEM,temp_guest_heads+4,*temp_active_head?temp_find(*temp_active_head)->guest:0);}
}
static void effects_push(void)
{
    particles_push();beams_push();
    for(LightCopy *p=lights;p;p=p->next)memcpy(p->host,G_MEM+p->guest,sizeof *p->host);
    for(TempCopy *p=entity_access_epoch?temp_accessed:temps;p;p=entity_access_epoch?p->access_next:p->next){
        TEMPENTITY *h=p->host;BhlTempEntity *g=(void*)(G_MEM+p->guest);
        if(p->valid&&!memcmp(p->guest_snapshot,g,sizeof p->guest_snapshot))continue;
        memcpy(h,g,36);
        p->hit=g->hitcallback;p->callback=g->callback;
        h->hitcallback=p->hit==temp_hit_thunk?p->native_hit:p->hit?temp_hit_native:NULL;
        h->callback=p->callback==temp_callback_thunk?p->native_callback:p->callback?temp_callback_native:NULL;
        h->next=temp_host(g->next);h->priority=g->priority;h->clientIndex=g->clientIndex;memcpy(h->tentOffset,g->tentOffset,12);
        memcpy(p->host_snapshot,h,sizeof p->host_snapshot);memcpy(p->guest_snapshot,g,sizeof p->guest_snapshot);p->valid=1;
    }
    if(temp_guest_heads){*temp_free_head=temp_host(rt_r32(G_MEM,temp_guest_heads));*temp_active_head=temp_host(rt_r32(G_MEM,temp_guest_heads+4));}
}
static void temp_add_guest(Ctx *c)
{
    if(!temp_add_visible)client_missing("tempent visible callback lifetime");
    /* Pinned CL_TempEntAddEntity/CL_AddVisibleEntity/R_AddEntity only consume
       this entity and renderer state for non-player ET_TEMPENTITY objects.
       They call the original HUD_AddEntity filter, whose nonspectator path
       is also scoped. Keep free/active-list edits and unrelated effects in
       guest ownership until a callback that actually consumes them. */
    /* The original update explicitly submits this entity for drawing. That
       establishes this callback's lifetime without walking both global lists
       again for every submitted tempent. Complete boundaries still traverse
       the actual lists and restore their ownership. */
    EntityCopy *submitted=entity_find_guest(A(0));if(submitted&&submitted->source==4)submitted->active=1;
    rt_watch_access(A(0),sizeof(BhlClientEntity));
    if(!rt_r32(G_MEM,0x3014caa4u)&&!rt_r32(G_MEM,A(0)+offsetof(BhlClientEntity,player))){
        cl_entity_t *h=entity_host(A(0));
        uint32_t result=temp_add_visible(h);EntityCopy *p=entity_find_active(h);if(p)entity_pull(p);
        RAW_RET(result);
    }
    entities_push();RET(temp_add_visible(entity_host(A(0))));
}
static void temp_sound_guest(Ctx *c)
{entities_push();if(!temp_play_sound)client_missing("tempent sound callback lifetime");temp_play_sound(temp_host(A(0)),rt_rf32(G_MEM,c->esp+8));RET(0);}
static void temp_update(double frame,double time,double gravity,TEMPENTITY **free_list,TEMPENTITY **active_list,int (*add)(cl_entity_t *),void (*sound)(TEMPENTITY *,float))
{
    if(temp_add_visible||temp_play_sound)client_missing("nested tempent update");temp_add_visible=add;temp_play_sound=sound;
    uint32_t p=bhl_heap_alloc(8,1),a[10]={0};rt_w32(G_MEM,p,temp_guest(*free_list));rt_w32(G_MEM,p+4,temp_guest(*active_list));
    temp_free_head=free_list;temp_active_head=active_list;temp_guest_heads=p;
    memcpy(a,&frame,8);memcpy(a+2,&time,8);memcpy(a+4,&gravity,8);a[6]=p;a[7]=p+4;
    a[8]=bhl_thunk("client-temp!add-visible",temp_add_guest);a[9]=bhl_thunk("client-temp!play-sound",temp_sound_guest);
    invoke("HUD_TempEntUpdate",a,10);*free_list=temp_host(rt_r32(G_MEM,p));*active_list=temp_host(rt_r32(G_MEM,p+4));
    temp_guest_heads=0;temp_free_head=temp_active_head=NULL;bhl_heap_free(p);temp_add_visible=NULL;temp_play_sound=NULL;
}
