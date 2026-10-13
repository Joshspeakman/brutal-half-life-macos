#include "../runtime/client_bridge.c"
#include <assert.h>
#include "entity_types.h"
static cl_entity_t *fixture_entities;
static cl_entity_t view_entity;
static cl_entity_t *get_entity(int index){return index>=0&&index<3?&fixture_entities[index]:NULL;}
static cl_entity_t *get_view(void){return &view_entity;}
static float fixture_time(void){return 12.5f;}
static int fixture_contents(const float *point,int *actual){assert(point[0]==8);*actual=-3;return -3;}
static model_t *fixture_model(const char *name,int *index){assert(!strcmp(name,"fixture-model"));*index=11;return fixture_entities[1].model;}
static cl_entity_t *studio_current(void){return &fixture_entities[1];}
static int studio_bbox(void){assert(fixture_entities[1].origin[2]==44&&fixture_entities[2].origin[2]==0);fixture_entities[1].curstate.frame=77;return 1;}
static void native_particle_think(particle_t *p,float frame){p->vel[1]+=frame;}
static float visibility_random(float low,float high){assert(low<high);return 0.25f;}
static model_t cached_model,*cached_model_at;static unsigned model_load_calls;
static model_t *cache_load(const char *name,int *index)
{model_load_calls++;if(index)*index=cached_model_at?11:-1;return cached_model_at;}
static model_t *cache_index(int index){assert(index==11);return cached_model_at;}
static void model_lookup_lifetime(void)
{
    strcpy(cached_model.name,"models/fixture.mdl");cached_model_at=&cached_model;
    engine.CL_LoadModel=cache_load;engine.pfnGetModelByIndex=cache_index;
    int index;assert(client_load_model("models/fixture.mdl",&index)==&cached_model&&index==11&&model_load_calls==1);
    assert(client_load_model("MODELS\\FIXTURE.MDL",&index)==&cached_model&&index==11&&model_load_calls==1);
    cached_model_at=NULL;assert(!client_load_model("models/fixture.mdl",&index)&&index==-1&&model_load_calls==2);
    assert(!client_load_model("models/fixture.mdl",NULL)&&model_load_calls==3);
    cached_model_at=&cached_model;strcpy(cached_model.name,"models/reused.mdl");
    assert(client_load_model("models/fixture.mdl",NULL)==&cached_model&&model_load_calls==4);
    strcpy(cached_model.name,"models/fixture.mdl");model_reset();
    assert(client_load_model("models/fixture.mdl",NULL)==&cached_model&&model_load_calls==5);
    engine.CL_LoadModel=fixture_model;engine.pfnGetModelByIndex=NULL;
}
static void studio_event_scope(void)
{
    struct {studiohdr_t header;mstudioseqdesc_t sequence[2];mstudioevent_t event;} data={0};
    data.header.length=sizeof data;data.header.numseq=2;data.header.seqindex=offsetof(__typeof__(data),sequence);
    data.sequence[1].numevents=1;data.sequence[1].eventindex=offsetof(__typeof__(data),event);
    studio_header=&data.header;studio_header_epoch=model_epoch;studio.GetCurrentEntity=studio_current;
    BhlClientEntity *g=(void*)(G_MEM+entity_copy(fixture_entities+1));g->curstate.sequence=0;
    assert(!studio_events_reenter());g->curstate.sequence=1;data.event.event=4999;assert(!studio_events_reenter());
    data.event.event=5000;assert(studio_events_reenter());g->curstate.sequence=99;assert(studio_events_reenter());
    studio_header_epoch=model_epoch-1;assert(studio_events_reenter());studio_header=NULL;
}
static unsigned renderer_flag_calls;
static void renderer_flag(int value)
{assert(value==7);assert(fixture_entities[1].origin[2]==24);renderer_flag_calls++;}
static void renderer_state(void){renderer_flag(7);}
static void studio_flag_scope(void)
{
    fixture_entities[1].origin[2]=24;
    BhlClientEntity *g=(void*)(G_MEM+entity_copy(fixture_entities+1));g->origin[2]=48;
    studio.SetForceFaceFlags=renderer_flag;studio.StudioSetCullState=renderer_flag;
    studio.GL_SetRenderMode=renderer_flag;studio.SetChromeOrigin=renderer_state;studio.RestoreRenderer=renderer_state;
    const unsigned slots[]={BHL_STUDIO_SetForceFaceFlags,BHL_STUDIO_StudioSetCullState,
        BHL_STUDIO_GL_SetRenderMode,BHL_STUDIO_SetChromeOrigin,BHL_STUDIO_RestoreRenderer};
    uint32_t sp=bhl_heap_alloc(8,1);rt_w32(G_MEM,sp+4,7);
    for(unsigned i=0;i<sizeof slots/sizeof *slots;i++){
        Ctx c={0};c.esp=sp;studio_dispatch(&c,slots[i]);
        assert(c.esp==sp+4&&g->origin[2]==48&&fixture_entities[1].origin[2]==24);
    }
    assert(renderer_flag_calls==5);bhl_heap_free(sp);entities_push();
    puts("Renderer flags passed: pending entity writes survive state-only calls");
}
static particle_t lifetime_pool[3];static unsigned lifetime_activity[3];static int lifetime_pool_available=1;
static int lifetime_query(const particle_t **base,const unsigned **activity,unsigned *count)
{*base=lifetime_pool_available?lifetime_pool:NULL;*activity=lifetime_activity;*count=lifetime_pool_available?3:0;return 1;}
static void particle_pool_lifetime(void)
{
    particle_pool_query=lifetime_query;lifetime_activity[0]=1;
    uint32_t address=particle_guest(lifetime_pool);BhlParticle *g=(void*)(G_MEM+address);
    g->velocity[0]=41;particles_push();assert(lifetime_pool[0].vel[0]==41);
    /* Retired records can join a free list. Neither follow that list into
       unrelated slots nor overwrite the engine's free records. */
    lifetime_activity[0]=0;lifetime_pool[0].next=lifetime_pool+1;lifetime_pool[1].next=lifetime_pool+2;
    unsigned before=0;for(ParticleCopy *p=particles;p;p=p->next)before++;
    particles_pull();g->velocity[0]=77;particles_push();assert(lifetime_pool[0].vel[0]==41);
    unsigned after=0;for(ParticleCopy *p=particles;p;p=p->next)after++;assert(after==before);
    lifetime_activity[0]=2;lifetime_pool[0].next=NULL;lifetime_pool[0].vel[0]=3;
    particles_push();assert(lifetime_pool[0].vel[0]==3&&g->velocity[0]==3);
    g->velocity[0]=9;particles_push();assert(lifetime_pool[0].vel[0]==9);
    ParticleCopy *p=particle_find(lifetime_pool);p->think_fn=0x30001000;lifetime_pool[0].think=particle_think_native;
    lifetime_activity[0]=3;assert(particle_guest(lifetime_pool)==address&&g->think==0x30001000);
    p->think_fn=0x30002000;lifetime_activity[0]=4;assert(particle_guest(lifetime_pool)==address&&g->think==0x30002000);
    lifetime_pool_available=0;p->host=(void*)0xfafafafafafafafaull;
    particles_pull();particles_push(); /* shutdown must not read the freed pool */
    p->host=lifetime_pool;particle_pool_query=NULL;lifetime_pool[0].think=NULL;
    puts("Native particle lifetime passed: free lists, reuse, callbacks and pool destruction");
}
static int visible_temp(cl_entity_t *h)
{
    assert(h->origin[2]==123);assert(fixture_entities[2].origin[0]==0);
    h->curstate.frame=19;
    return HUD_AddEntity(ET_TEMPENTITY,h,"fixture-model");
}
static void visibility_equivalence(void)
{
    /* Run the actual translated export through both boundaries, including
       its custom sky-camera calculation and oscillating model origin. */
    guest_engine=bhl_heap_alloc(135*4,1);fill_client_engine(guest_engine);
    rt_w32(G_MEM,0x30138fc4,rt_r32(G_MEM,guest_engine+4*BHL_CL_pfnRandomFloat));
    engine.pfnRandomFloat=visibility_random;
    model_t model={0};model.type=mod_brush;model.mins[0]=-10;model.maxs[0]=30;
    cl_entity_t *h=fixture_entities+1;h->model=&model;
    const char *names[]={"ordinary","*3","models/ppdemodot.mdl","ordinary"};
    for(unsigned scenario=0;scenario<4;scenario++){
        cl_entity_t initial=*h;initial.origin[0]=12;initial.origin[1]=24;initial.origin[2]=48;
        uint32_t string=copy_string(names[scenario]);unsigned char expected[12],sky[12];int result=0;
        for(unsigned full=0;full<2;full++){
            *h=initial;rt_w32(G_MEM,0x3014caa4,scenario==3?4:0);rt_w32(G_MEM,0x3014caac,h->index);
            rt_w32(G_MEM,0x3015241c,3);rt_wf32(G_MEM,0x30155744,0);rt_wf32(G_MEM,0x3010abbc,1);rt_wf32(G_MEM,0x30155cb0,0.95f);
            memset(G_MEM+0x301521ac,0,12);
            int actual;
            if(full){uint32_t args[]={0,entity_copy(h),string};actual=invoke("HUD_AddEntity",args,3);}
            else actual=HUD_AddEntity(0,h,names[scenario]);
            if(!full){result=actual;memcpy(expected,h->origin,12);memcpy(sky,G_MEM+0x301521ac,12);}
            else assert(actual==result&&!memcmp(expected,h->origin,12)&&!memcmp(sky,G_MEM+0x301521ac,12));
        }
        assert(result==(scenario!=3));
        if(scenario==1)assert(rt_rf32(G_MEM,0x301521ac)==22);
        if(scenario==2)assert(h->origin[2]!=48);
        bhl_heap_free(string);
    }
    rt_w32(G_MEM,0x3014caa4,0);rt_w32(G_MEM,0x3015241c,0);h->model=NULL;entities_pull();
    puts("Original visibility callback equivalence passed: normal, sky camera, animated sky model, spectator");
}
int main(void)
{
    bhl_initialize();engine.GetEntityByIndex=get_entity;engine.GetViewModel=get_view;
    fixture_entities=calloc(3,sizeof *fixture_entities);fixture_entities[1].index=1;
    model_t model={0};strcpy(model.name,"fixture-model");model.type=mod_brush;
    fixture_entities[1].model=&model;fixture_entities[1].origin[0]=42;
    uint32_t p=entity_copy(&fixture_entities[1]);BhlClientEntity *g=(void*)(G_MEM+p);
    assert(g->origin[0]==42&&model_host(g->model)==&model);
    g->origin[1]=17;g->curstate.frame=8;entities_push();assert(fixture_entities[1].origin[1]==17&&fixture_entities[1].curstate.frame==8);
    /* Independent engine queries must retain guest writes without copying
       the whole entity list to the host or overwriting those writes. */
    engine.GetClientTime=fixture_time;g->curstate.frame=99;
    Ctx query={0};query.esp=bhl_heap_alloc(16,1);rt_w32(G_MEM,query.esp+4,1);
    uint32_t query_stack=query.esp;client_dispatch(&query,BHL_CL_GetEntityByIndex);
    assert(query.eax==p&&query.esp==query_stack+4&&g->curstate.frame==99&&fixture_entities[1].curstate.frame==8);
    query.esp=query_stack;client_dispatch(&query,BHL_CL_GetClientTime);
    assert(query.esp==query_stack+4&&g->curstate.frame==99&&fixture_entities[1].curstate.frame==8);
    engine.PM_PointContents=fixture_contents;engine.CL_LoadModel=fixture_model;
    uint32_t values=bhl_heap_alloc(32,1),name=copy_string("fixture-model");rt_wf32(G_MEM,values,8);
    query.esp=query_stack;rt_w32(G_MEM,query_stack+4,values);rt_w32(G_MEM,query_stack+8,values+12);client_dispatch(&query,BHL_CL_PM_PointContents);
    assert((int32_t)query.eax==-3&&(int32_t)rt_r32(G_MEM,values+12)==-3&&g->curstate.frame==99&&fixture_entities[1].curstate.frame==8);
    query.esp=query_stack;rt_w32(G_MEM,query_stack+4,name);rt_w32(G_MEM,query_stack+8,values+12);client_dispatch(&query,BHL_CL_CL_LoadModel);
    assert(model_host(query.eax)==&model&&rt_r32(G_MEM,values+12)==11&&g->curstate.frame==99&&fixture_entities[1].curstate.frame==8);
    bhl_heap_free(values);bhl_heap_free(name);
    entities_push();assert(fixture_entities[1].curstate.frame==99);bhl_heap_free(query_stack);
    /* Real engine replaces this allocation at signon. The cached entity ID
       must resolve anew without touching its now-freed model pointer. */
    cl_entity_t *old=fixture_entities;fixture_entities=calloc(3,sizeof *fixture_entities);fixture_entities[1].index=1;fixture_entities[1].origin[0]=99;
    old[1].model=(void*)0xfafafafafafafafaull;free(old);entities_pull();
    assert(g->origin[0]==99&&g->model==0&&entity_host(p)==&fixture_entities[1]);
    visibility_equivalence();
    model_lookup_lifetime();studio_event_scope();studio_flag_scope();
    TEMPENTITY temp[3]={0};temp[0].next=&temp[1];TEMPENTITY *free_head=&temp[0],*active_head=&temp[2];
    uint32_t f=temp_guest(free_head),a=temp_guest(active_head),heads=bhl_heap_alloc(8,1);
    temp_free_head=&free_head;temp_active_head=&active_head;temp_guest_heads=heads;
    rt_w32(G_MEM,heads,a);rt_w32(G_MEM,heads+4,0);
    ((BhlTempEntity*)(G_MEM+a))->entity.origin[2]=123;
    ((BhlTempEntity*)(G_MEM+a))->next=f;entities_push();
    assert(free_head==&temp[2]&&free_head->next==&temp[0]&&!active_head);
    assert(temp[2].entity.origin[2]==0); /* free bodies are not published */
    /* Scoped visibility still publishes/reads back the current entity and
       allows a nested original HUD callback, without flushing other edits. */
    uint32_t untouched=entity_copy(fixture_entities+2);
    ((BhlClientEntity*)(G_MEM+untouched))->origin[0]=456;
    rt_w32(G_MEM,heads,f);rt_w32(G_MEM,heads+4,a);((BhlTempEntity*)(G_MEM+a))->next=0;
    temp_add_visible=visible_temp;Ctx add={0};add.esp=bhl_heap_alloc(16,1);rt_w32(G_MEM,add.esp+4,a+68);
    client_call_depth=1;temp_add_guest(&add);client_call_depth=0;temp_add_visible=NULL;
    assert(add.eax==1&&((BhlTempEntity*)(G_MEM+a))->entity.curstate.frame==19);
    assert(((BhlClientEntity*)(G_MEM+untouched))->origin[0]==456&&fixture_entities[2].origin[0]==0);
    /* Restore the preceding free-list scenario for the allocation check. */
    rt_w32(G_MEM,heads,a);rt_w32(G_MEM,heads+4,0);((BhlTempEntity*)(G_MEM+a))->next=f;entities_push();
    /* An engine effect allocates while the original update is in progress. */
    TEMPENTITY *allocated=free_head;free_head=allocated->next;allocated->next=active_head;active_head=allocated;allocated->entity.curstate.frame=23;
    entities_pull();assert(rt_r32(G_MEM,heads)==f&&rt_r32(G_MEM,heads+4)==a);
    assert(((BhlTempEntity*)(G_MEM+a))->next==0&&((BhlTempEntity*)(G_MEM+a))->entity.curstate.frame==23);
    entities_push();assert(active_head->next==NULL&&free_head==&temp[0]);
    temp_guest_heads=0;temp_free_head=temp_active_head=NULL;
    float matrix[128][3][4]={0};uint32_t m=studio_buffer(matrix,sizeof matrix);
    rt_wf32(G_MEM,m+4,3.5f);studio_push();assert(matrix[0][0][1]==3.5f);
    matrix[127][2][3]=11;studio_pull();assert(rt_rf32(G_MEM,m+sizeof matrix-4)==11);
    assert(studio_buffer(matrix,sizeof matrix)==m);
    hull_t absent={0};uint32_t hg=hull_guest(&absent);BhlHull *gh=(void*)(G_MEM+hg);
    assert(!gh->planes&&!gh->clipnodes&&hull_host(hg)==&absent);
    mclipnode16_t node={0};mplane_t plane={0};hull_t moving={0};moving.clipnodes16=&node;moving.planes=&plane;
    plane.dist=8;hg=hull_guest(&moving);gh=(void*)(G_MEM+hg);assert(((mplane_t*)(G_MEM+gh->planes))->dist==8);
    plane.dist=19;assert(hull_guest(&moving)==hg&&((mplane_t*)(G_MEM+gh->planes))->dist==19);
    float mins[3]={-16,-16,-36},maxs[3]={16,16,36};
    assert(HUD_GetHullBounds(0,mins,maxs)==1&&mins[2]==-36&&maxs[2]==36);
    assert(HUD_GetHullBounds(3,mins,maxs)==0&&mins[0]==-16&&maxs[0]==16);
    char text[]="first";model.entities=text;uint32_t mg=model_guest(&model);BhlModel *gm=(void*)(G_MEM+mg);assert(!strcmp(gs(gm->entities),"first"));
    memcpy(text,"later",6);model_invalidate();assert(model_guest(&model)==mg&&!strcmp(gs(gm->entities),"later"));
    model.numframes=9;assert(model_guest(&model)==mg&&gm->numframes==9);
    uint32_t token=model_opaque(text,"fixture"),borrowed=borrowed_string("repeated-query");
    uint32_t before=bhl_heap_used();
    for(unsigned i=0;i<10000;i++)assert(borrowed_string("repeated-query")==borrowed);
    assert(bhl_heap_used()==before);
    /* Retiring a level frees large data copies, retains descriptor identity,
       and reuses opaque slots instead of exhausting the thunk table. */
    unsigned char *large=calloc(1,1024*1024);large[0]=123;
    for(unsigned i=0;i<160;i++){
        model_reset();uint32_t blob=model_bytes(large,1024*1024);
        assert(G_MEM[blob]==123&&model_guest(&model)==mg);
        assert(model_opaque(text,"fixture")==token);
    }
    assert(bhl_heap_used()-before<2*1024*1024);model_reset();free(large);
    dlight_t light={0};light.key=14;uint32_t dl=light_guest(&light);dlight_t *gl=(void*)(G_MEM+dl);gl->radius=100;gl->color.r=239;effects_push();assert(light.radius==100&&light.color.r==239);
    light.radius=75;effects_pull();assert(gl->radius==75);memset(&light,0,sizeof light);light.key=29;assert(light_guest(&light)==dl&&gl->key==29&&gl->radius==0);
    particle_t particle[2]={0};particle[0].next=particle+1;particle[0].think=native_particle_think;
    uint32_t pg=particle_guest(particle);BhlParticle *gp=(void*)(G_MEM+pg);assert(particle_host(gp->next)==particle+1&&gp->think==particle_think_thunk);
    gp->velocity[1]=17;gp->color=12;particles_push();assert(particle[0].vel[1]==17&&particle[0].color==12&&particle[0].think==native_particle_think);
    Ctx callback={0};callback.esp=bhl_heap_alloc(16,1);rt_w32(G_MEM,callback.esp+4,pg);rt_wf32(G_MEM,callback.esp+8,0.25f);particle_think_guest(&callback);
    assert(gp->velocity[1]==17.25f&&particle[0].vel[1]==17.25f);
    particle[0].next=NULL;particles_pull();assert(!gp->next);
    particle_pool_lifetime();
    BEAM beam[2]={0};beam[0].next=beam+1;beam[0].pFollowModel=&model;beam[0].particles=particle;
    uint32_t bg=beam_guest(beam);BhlBeam *gb=(void*)(G_MEM+bg);assert(beam_host(gb->next)==beam+1&&model_host(gb->model)==&model&&particle_host(gb->particles)==particle);
    gb->width=12;gb->brightness=0.8f;gb->next=0;beams_push();assert(beam[0].width==12&&beam[0].brightness==0.8f&&!beam[0].next);
    beam[0].die=9;beams_pull();assert(gb->die==9&&beam_guest(beam)==bg);
    netadr_t remote={0};remote.type=NA_LOOPBACK;char response[32];memset(response,0x5a,sizeof response);int response_size=sizeof response;
    assert(HUD_ConnectionlessPacket(&remote,"fixture",response,&response_size)==0&&response_size==0&&response[0]==0x5a);
    fixture_entities[2].index=2;uint32_t other=entity_copy(&fixture_entities[2]);
    ((BhlClientEntity*)(G_MEM+other))->origin[2]=123;g->origin[2]=44;
    studio.GetCurrentEntity=studio_current;studio.StudioCheckBBox=studio_bbox;
    Ctx draw={0};draw.esp=bhl_heap_alloc(16,1);studio_dispatch(&draw,BHL_STUDIO_StudioCheckBBox);
    assert(draw.eax==1&&g->curstate.frame==77&&((BhlClientEntity*)(G_MEM+other))->origin[2]==123&&fixture_entities[2].origin[2]==0);
    /* More identities than cache slots must not merge entities or discard
       pending guest writes. Re-query in reverse order after collisions. */
    cl_entity_t *many=calloc(4100,sizeof *many);uint32_t *ids=calloc(4100,sizeof *ids);
    for(unsigned i=0;i<4100;i++){many[i].index=-1;many[i].origin[0]=i;ids[i]=entity_copy(many+i);((BhlClientEntity*)(G_MEM+ids[i]))->origin[1]=i+1;}
    for(unsigned i=4100;i-->0;){assert(entity_reference(many+i)==ids[i]);assert(((BhlClientEntity*)(G_MEM+ids[i]))->origin[1]==i+1&&many[i].origin[1]==0);}
    entities_push();for(unsigned i=0;i<4100;i++)assert(many[i].origin[1]==i+1);
    entities_end();assert(!entity_find_active(many));assert(entity_copy(many)==ids[0]&&entity_find_active(many));
    model_t *many_models=calloc(4100,sizeof *many_models);uint32_t *model_ids=calloc(4100,sizeof *model_ids);
    for(unsigned i=0;i<4100;i++){many_models[i].type=mod_brush;many_models[i].numframes=i;model_ids[i]=model_guest(many_models+i);}
    model_reset();
    for(unsigned i=4100;i-->0;){many_models[i].numframes=i+1;assert(model_guest(many_models+i)==model_ids[i]);assert(((BhlModel*)(G_MEM+model_ids[i]))->numframes==i+1);}
    puts("Client boundaries passed: entity relocation, writes, temp lists, studio matrices");return 0;
}
