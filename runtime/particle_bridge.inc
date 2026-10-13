/* Particle records contain two callback pointers and a linked-list pointer.
 * Native renderer particles retain stable guest identities across reuse. */
typedef struct BhlParticle {
    vec3_t origin;short color,unused;uint32_t next;vec3_t velocity;
    float ramp,die;int type;uint32_t on_die,think;unsigned char userdata,padding[3];
} BhlParticle;
_Static_assert(sizeof(BhlParticle)==56,"particle ABI");
typedef struct ParticleCopy {
    particle_t *host;uint32_t guest,die_fn,think_fn;
    int valid;particle_t host_snapshot;BhlParticle guest_snapshot;
    int managed;unsigned generation;
    void (*native_die)(particle_t*);void (*native_think)(particle_t*,float);
    struct ParticleCopy *next,*host_next,*guest_next;
} ParticleCopy;
static ParticleCopy *particles,*particle_hosts[4096],*particle_guests[4096];
static uint32_t particle_die_thunk,particle_think_thunk;
#include <dlfcn.h>
static int (*particle_pool_query)(const particle_t **,const unsigned **,unsigned *);
static const particle_t *particle_pool_base;
static const unsigned *particle_pool_activity;
static unsigned particle_pool_count;
static void particle_pool_initialize(void)
{
    if(!engine.pEfxAPI||!engine.pEfxAPI->R_AllocParticle)return;
    Dl_info info={0};if(!dladdr((void*)engine.pEfxAPI->R_AllocParticle,&info)||!info.dli_fname)return;
    void *library=dlopen(info.dli_fname,RTLD_LAZY|RTLD_NOLOAD);if(!library)return;
    particle_pool_query=dlsym(library,"Xash_GetParticlePoolV1");dlclose(library);
    if(particle_pool_query)fprintf(stderr,"BHL native particle pool lifetime tracking enabled\n");
}
static void particle_pool_refresh(void)
{
    if(particle_pool_query&&particle_pool_query(&particle_pool_base,&particle_pool_activity,&particle_pool_count)!=1)
        client_missing("particle pool query version");
}
static int particle_pool_slot(const particle_t *h,unsigned *index)
{
    uintptr_t address=(uintptr_t)h,base=(uintptr_t)particle_pool_base;
    if(!particle_pool_base||address<base||address-base>=(uint64_t)particle_pool_count*sizeof *h||(address-base)%sizeof *h)return 0;
    *index=(address-base)/sizeof *h;return 1;
}
static int particle_lifetime(ParticleCopy *p)
{
    if(!particle_pool_query)return 1;unsigned index;
    if(!particle_pool_slot(p->host,&index))return !p->managed;
    p->managed=1;unsigned generation=particle_pool_activity[index];
    if(!generation){p->valid=0;return 0;}
    if(p->generation!=generation){p->generation=generation;p->valid=0;return 2;}
    return 1;
}
static ParticleCopy *particle_find(particle_t *h)
{
    unsigned bucket=temp_host_bucket(h);for(ParticleCopy *p=particle_hosts[bucket];p;p=p->host_next)if(p->host==h)return p;
    ParticleCopy *p=calloc(1,sizeof *p);if(!p)abort();p->host=h;p->guest=bhl_heap_alloc(sizeof(BhlParticle),1);p->next=particles;particles=p;
    p->host_next=particle_hosts[bucket];particle_hosts[bucket]=p;bucket=temp_guest_bucket(p->guest);p->guest_next=particle_guests[bucket];particle_guests[bucket]=p;return p;
}
static ParticleCopy *particle_from_guest(uint32_t g)
{for(ParticleCopy *p=particle_guests[temp_guest_bucket(g)];p;p=p->guest_next)if(p->guest==g)return p;client_missing("unknown particle");return NULL;}
static particle_t *particle_host(uint32_t g){return g?particle_from_guest(g)->host:NULL;}
static void particle_die_native(particle_t *h)
{ParticleCopy *p=particle_find(h);invoke_address(p->die_fn,&p->guest,1);}
static void particle_think_native(particle_t *h,float frame)
{ParticleCopy *p=particle_find(h);uint32_t a[]={p->guest,0};memcpy(a+1,&frame,4);invoke_address(p->think_fn,a,2);}
static void particle_die_guest(Ctx *c)
{entities_push();ParticleCopy *p=particle_from_guest(A(0));if(!p->native_die)client_missing("particle death callback");p->native_die(p->host);RET(0);}
static void particle_think_guest(Ctx *c)
{entities_push();ParticleCopy *p=particle_from_guest(A(0));if(!p->native_think)client_missing("particle think callback");p->native_think(p->host,rt_rf32(G_MEM,c->esp+8));RET(0);}
static void particle_pull(ParticleCopy *p)
{
    particle_t *h=p->host;BhlParticle *g=(void*)(G_MEM+p->guest);
    if(p->valid&&!memcmp(&p->host_snapshot,h,sizeof *h))return;
    memcpy(g,h,16);g->next=h->next?particle_find(h->next)->guest:0;memcpy(g->velocity,h->vel,24);
    if(h->on_die!=particle_die_native)p->native_die=h->on_die;if(h->think!=particle_think_native)p->native_think=h->think;
    g->on_die=h->on_die==particle_die_native?p->die_fn:h->on_die?particle_die_thunk:0;
    g->think=h->think==particle_think_native?p->think_fn:h->think?particle_think_thunk:0;g->userdata=h->userdata;
    p->host_snapshot=*h;p->guest_snapshot=*g;p->valid=1;
}
static void particles_pull(void)
{
    particle_pool_refresh();
    ParticleCopy *head;unsigned rounds=0;
    do{head=particles;for(ParticleCopy *p=head;p;p=p->next)if(particle_lifetime(p))particle_pull(p);if(++rounds>65536)client_missing("particle graph exceeds limit");}while(head!=particles);
}
static void particles_push(void)
{
    particle_pool_refresh();
    for(ParticleCopy *p=particles;p;p=p->next){particle_t *h=p->host;BhlParticle *g=(void*)(G_MEM+p->guest);
        int life=particle_lifetime(p);if(!life)continue;
        /* The engine may reuse a slot without returning it to the client.
           Never apply stale guest writes to the new native allocation. */
        if(life==2){particle_pull(p);continue;}
        if(p->valid&&!memcmp(&p->guest_snapshot,g,sizeof *g))continue;
        memcpy(h,g,16);h->next=particle_host(g->next);memcpy(h->vel,g->velocity,24);p->die_fn=g->on_die;p->think_fn=g->think;
        h->on_die=p->die_fn==particle_die_thunk?p->native_die:p->die_fn?particle_die_native:NULL;
        h->think=p->think_fn==particle_think_thunk?p->native_think:p->think_fn?particle_think_native:NULL;h->userdata=g->userdata;
        p->host_snapshot=*h;p->guest_snapshot=*g;p->valid=1;
    }
}
static uint32_t particle_guest(particle_t *h)
{
    if(!h)return 0;if(!particle_die_thunk){particle_die_thunk=bhl_thunk("client-particle!die",particle_die_guest);particle_think_thunk=bhl_thunk("client-particle!think",particle_think_guest);}
    particle_pool_refresh();
    unsigned count=0;for(particle_t *at=h;at;at=at->next){
        unsigned index;if(particle_pool_query&&particle_pool_slot(at,&index)&&!particle_pool_activity[index])break;
        if(++count>65536)client_missing("cyclic particle list");particle_find(at);
    }
    /* An allocator can recycle an identical native record with a new guest
       callback. The returned object must always be freshly marshaled. */
    ParticleCopy *first=particle_find(h);first->valid=0;particles_pull();return first->guest;
}
static uint32_t particle_alloc(uint32_t callback)
{
    particle_t *h=engine.pEfxAPI->R_AllocParticle(callback?particle_think_native:NULL);if(!h)return 0;
    /* Record the original callback before the new object is exposed. */
    ParticleCopy *p=particle_find(h);p->think_fn=callback;return particle_guest(h);
}

typedef struct BhlBeam {
    uint32_t next;int type,flags;vec3_t source,target,delta;
    float t,freq,die,width,amplitude,r,g,b,brightness,speed,frameRate,frame;
    int segments,startEntity,endEntity,modelIndex,frameCount;uint32_t model,particles;
} BhlBeam;
_Static_assert(sizeof(BhlBeam)==124,"beam ABI");
typedef struct BeamCopy {BEAM *host;uint32_t guest;struct BeamCopy *next,*host_next,*guest_next;} BeamCopy;
static BeamCopy *beams,*beam_hosts[4096],*beam_guests[4096];
static BeamCopy *beam_find(BEAM *h)
{
    unsigned bucket=temp_host_bucket(h);for(BeamCopy *p=beam_hosts[bucket];p;p=p->host_next)if(p->host==h)return p;
    BeamCopy *p=calloc(1,sizeof *p);if(!p)abort();p->host=h;p->guest=bhl_heap_alloc(sizeof(BhlBeam),1);p->next=beams;beams=p;
    p->host_next=beam_hosts[bucket];beam_hosts[bucket]=p;bucket=temp_guest_bucket(p->guest);p->guest_next=beam_guests[bucket];beam_guests[bucket]=p;return p;
}
static BEAM *beam_host(uint32_t g)
{if(!g)return NULL;for(BeamCopy *p=beam_guests[temp_guest_bucket(g)];p;p=p->guest_next)if(p->guest==g)return p->host;client_missing("unknown beam");return NULL;}
static void beams_pull(void)
{
    BeamCopy *head;unsigned rounds=0;
    do{head=beams;for(BeamCopy *p=head;p;p=p->next){BEAM *h=p->host;BhlBeam *g=(void*)(G_MEM+p->guest);
        g->next=h->next?beam_find(h->next)->guest:0;memcpy(&g->type,&h->type,112);g->model=model_guest(h->pFollowModel);g->particles=particle_guest(h->particles);}
        if(++rounds>65536)client_missing("beam graph exceeds limit");
    }while(head!=beams);
}
static void beams_push(void)
{
    for(BeamCopy *p=beams;p;p=p->next){BEAM *h=p->host;BhlBeam *g=(void*)(G_MEM+p->guest);
        h->next=beam_host(g->next);memcpy(&h->type,&g->type,112);h->pFollowModel=model_host(g->model);h->particles=particle_host(g->particles);}
}
static uint32_t beam_guest(BEAM *h)
{
    if(!h)return 0;unsigned count=0;for(BEAM *at=h;at;at=at->next){if(++count>65536)client_missing("cyclic beam list");beam_find(at);}
    beams_pull();return beam_find(h)->guest;
}
