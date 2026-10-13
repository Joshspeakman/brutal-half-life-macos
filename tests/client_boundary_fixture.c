#include "../runtime/client_bridge.c"
#include <assert.h>
static cl_entity_t *fixture_entities;
static cl_entity_t view_entity;
static cl_entity_t *get_entity(int index){return index>=0&&index<3?&fixture_entities[index]:NULL;}
static cl_entity_t *get_view(void){return &view_entity;}
static void native_particle_think(particle_t *p,float frame){p->vel[1]+=frame;}
int main(void)
{
    bhl_initialize();engine.GetEntityByIndex=get_entity;engine.GetViewModel=get_view;
    fixture_entities=calloc(3,sizeof *fixture_entities);fixture_entities[1].index=1;
    model_t model={0};strcpy(model.name,"fixture-model");model.type=mod_brush;
    fixture_entities[1].model=&model;fixture_entities[1].origin[0]=42;
    uint32_t p=entity_copy(&fixture_entities[1]);BhlClientEntity *g=(void*)(G_MEM+p);
    assert(g->origin[0]==42&&model_host(g->model)==&model);
    g->origin[1]=17;g->curstate.frame=8;entities_push();assert(fixture_entities[1].origin[1]==17&&fixture_entities[1].curstate.frame==8);
    /* Real engine replaces this allocation at signon. The cached entity ID
       must resolve anew without touching its now-freed model pointer. */
    cl_entity_t *old=fixture_entities;fixture_entities=calloc(3,sizeof *fixture_entities);fixture_entities[1].index=1;fixture_entities[1].origin[0]=99;
    old[1].model=(void*)0xfafafafafafafafaull;free(old);entities_pull();
    assert(g->origin[0]==99&&g->model==0&&entity_host(p)==&fixture_entities[1]);
    TEMPENTITY temp[3]={0};temp[0].next=&temp[1];TEMPENTITY *free_head=&temp[0],*active_head=&temp[2];
    uint32_t f=temp_guest(free_head),a=temp_guest(active_head),heads=bhl_heap_alloc(8,1);
    temp_free_head=&free_head;temp_active_head=&active_head;temp_guest_heads=heads;
    rt_w32(G_MEM,heads,a);rt_w32(G_MEM,heads+4,0);
    ((BhlTempEntity*)(G_MEM+a))->next=f;entities_push();
    assert(free_head==&temp[2]&&free_head->next==&temp[0]&&!active_head);
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
    dlight_t light={0};light.key=14;uint32_t dl=light_guest(&light);dlight_t *gl=(void*)(G_MEM+dl);gl->radius=100;gl->color.r=239;effects_push();assert(light.radius==100&&light.color.r==239);
    light.radius=75;effects_pull();assert(gl->radius==75);memset(&light,0,sizeof light);light.key=29;assert(light_guest(&light)==dl&&gl->key==29&&gl->radius==0);
    particle_t particle[2]={0};particle[0].next=particle+1;particle[0].think=native_particle_think;
    uint32_t pg=particle_guest(particle);BhlParticle *gp=(void*)(G_MEM+pg);assert(particle_host(gp->next)==particle+1&&gp->think==particle_think_thunk);
    gp->velocity[1]=17;gp->color=12;particles_push();assert(particle[0].vel[1]==17&&particle[0].color==12&&particle[0].think==native_particle_think);
    Ctx callback={0};callback.esp=bhl_heap_alloc(16,1);rt_w32(G_MEM,callback.esp+4,pg);rt_wf32(G_MEM,callback.esp+8,0.25f);particle_think_guest(&callback);
    assert(gp->velocity[1]==17.25f&&particle[0].vel[1]==17.25f);
    particle[0].next=NULL;particles_pull();assert(!gp->next);
    BEAM beam[2]={0};beam[0].next=beam+1;beam[0].pFollowModel=&model;beam[0].particles=particle;
    uint32_t bg=beam_guest(beam);BhlBeam *gb=(void*)(G_MEM+bg);assert(beam_host(gb->next)==beam+1&&model_host(gb->model)==&model&&particle_host(gb->particles)==particle);
    gb->width=12;gb->brightness=0.8f;gb->next=0;beams_push();assert(beam[0].width==12&&beam[0].brightness==0.8f&&!beam[0].next);
    beam[0].die=9;beams_pull();assert(gb->die==9&&beam_guest(beam)==bg);
    puts("Client boundaries passed: entity relocation, writes, temp lists, studio matrices");return 0;
}
