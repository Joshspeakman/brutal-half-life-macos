/* Shared client/server movement marshalling. Collision remains in the native
 * engine; movement decisions execute the original translated game routines. */
#define PM_RET(v) do { c->eax=(uint32_t)(v);c->esp+=4;return; } while(0)
static void movement_error(const char *name){fprintf(stderr,"BHL native movement: unimplemented or invalid %s\n",name);abort();}
static void movement_counts(const playermove_t *h)
{if(h->numphysent<0||h->numphysent>MAX_PHYSENTS||h->numvisent<0||h->numvisent>MAX_PHYSENTS||h->nummoveent<0||h->nummoveent>MAX_MOVEENTS||h->numtouch<0||h->numtouch>MAX_PHYSENTS)movement_error("entity/touch count");}
static void movement_pull(playermove_t *h)
{
    if(!guest_pmove)movement_error("move before initialization");host_pmove=h;movement_counts(h);BhlPlayerMove *g=(void*)(G_MEM+guest_pmove);pm_copy_values(g,h);
    for(int i=0;i<h->numphysent;i++)phys_to_guest(&g->physents[i],&h->physents[i]);
    for(int i=0;i<h->nummoveent;i++)phys_to_guest(&g->moveents[i],&h->moveents[i]);
    for(int i=0;i<h->numvisent;i++)phys_to_guest(&g->visents[i],&h->visents[i]);
    if(h->movevars)memcpy(G_MEM+g->movevars,h->movevars,sizeof *h->movevars);
}
static void movement_push(void)
{
    BhlPlayerMove *g=(void*)(G_MEM+guest_pmove);playermove_t *h=host_pmove;pm_write_values(h,g);movement_counts(h);
    for(int i=0;i<h->numphysent;i++)phys_to_host(&h->physents[i],&g->physents[i]);
    for(int i=0;i<h->nummoveent;i++)phys_to_host(&h->moveents[i],&g->moveents[i]);
    for(int i=0;i<h->numvisent;i++)phys_to_host(&h->visents[i],&g->visents[i]);
    if(h->movevars)memcpy(h->movevars,G_MEM+g->movevars,sizeof *h->movevars);
}
static physent_t *movement_physent(uint32_t p,physent_t *temporary)
{
    if(!p)return NULL;
    uint32_t offsets[]={offsetof(BhlPlayerMove,physents),offsetof(BhlPlayerMove,moveents),offsetof(BhlPlayerMove,visents)};
    int counts[]={host_pmove->numphysent,host_pmove->nummoveent,host_pmove->numvisent};physent_t *arrays[]={host_pmove->physents,host_pmove->moveents,host_pmove->visents};
    for(unsigned i=0;i<3;i++){uint32_t delta=p-guest_pmove-offsets[i];if(delta%sizeof(BhlPhysent)==0&&delta/sizeof(BhlPhysent)<(unsigned)counts[i])return arrays[i]+delta/sizeof(BhlPhysent);}
    phys_to_host(temporary,(void*)(G_MEM+p));return temporary;
}
typedef struct HullCopy {hull_t *host;uint32_t guest;struct HullCopy *next;} HullCopy;
static HullCopy *hull_copies;
static uint32_t hull_guest(hull_t *h)
{
    if(!h)return 0;HullCopy *p=hull_copies;while(p&&p->host!=h)p=p->next;
    if(!p){p=calloc(1,sizeof *p);p->host=h;p->guest=bhl_heap_alloc(sizeof(BhlHull),1);p->next=hull_copies;hull_copies=p;}
    BhlHull *g=(void*)(G_MEM+p->guest);g->firstclipnode=h->firstclipnode;g->lastclipnode=h->lastclipnode;memcpy(g->clip_mins,h->clip_mins,24);
    /* Xash deliberately leaves absent BSP hulls with no planes. Its point
       contents callback returns CONTENTS_NONE without consulting the nodes. */
    if(!h->planes||(h->firstclipnode<0&&!h->clipnodes16)){g->clipnodes=0;g->planes=0;return p->guest;}
    if(!h->clipnodes16){fprintf(stderr,"BHL hull has no nodes: first=%d last=%d\n",h->firstclipnode,h->lastclipnode);movement_error("nonterminal hull without clipnodes");}
    if(h->lastclipnode<0||h->lastclipnode>1000000)movement_error("hull node count");
    unsigned planes=0;for(int i=0;i<=h->lastclipnode;i++){int plane=h->clipnodes16[i].planenum;if(plane<0||plane>1000000)movement_error("hull plane index");if((unsigned)plane>=planes)planes=plane+1;}
    g->clipnodes=model_bytes(h->clipnodes16,(h->lastclipnode+1)*sizeof *h->clipnodes16);g->planes=model_bytes(h->planes,planes*sizeof *h->planes);
    /* Engine box hulls reuse the same allocation with new plane distances. */
    memcpy(G_MEM+g->planes,h->planes,planes*sizeof *h->planes);return p->guest;
}
static hull_t *hull_host(uint32_t p)
{
    if(!p)return NULL;for(HullCopy *h=hull_copies;h;h=h->next)if(h->guest==p)return h->host;
    for(ModelMirror *m=model_mirrors;m;m=m->next){uint32_t d=p-m->guest-offsetof(BhlModel,hulls);if(d%sizeof(BhlHull)==0&&d/sizeof(BhlHull)<4)return (hull_t*)&m->host->hulls[d/sizeof(BhlHull)];}
    movement_error("hull pointer");return NULL;
}
static uint32_t ignore_function;
static int movement_ignore(physent_t *h)
{uint32_t p=bhl_heap_alloc(sizeof(BhlPhysent),1);phys_to_guest((void*)(G_MEM+p),h);int r=bhl_invoke(ignore_function,&p,1,0);bhl_heap_free(p);return r;}
static void pm_dispatch(Ctx *c,unsigned slot)
{
    if(getenv("BHL_BRIDGE_TRACE"))fprintf(stderr,"movement %s\n",pm_names[slot]);
    if(guest_pmove)movement_push();
    switch(slot){
    case BHL_PM_PM_Info_ValueForKey:PM_RET(copy_string(host_pmove->PM_Info_ValueForKey(gs(A(0)),gs(A(1)))));
    case BHL_PM_Con_Printf:case BHL_PM_Con_DPrintf:{char text[8192];format_guest(text,gs(A(0)),c->esp+8);if(slot==BHL_PM_Con_Printf)host_pmove->Con_Printf("%s",text);else host_pmove->Con_DPrintf("%s",text);PM_RET(0);}
    case BHL_PM_Con_NPrintf:{char text[8192];format_guest(text,gs(A(1)),c->esp+12);host_pmove->Con_NPrintf(A(0),"%s",text);PM_RET(0);}
    case BHL_PM_Sys_FloatTime:FPUSH(host_pmove->Sys_FloatTime());PM_RET(0);
    case BHL_PM_RandomLong:PM_RET(host_pmove->RandomLong(A(0),A(1)));
    case BHL_PM_RandomFloat:FPUSH(host_pmove->RandomFloat(rt_rf32(G_MEM,c->esp+4),rt_rf32(G_MEM,c->esp+8)));PM_RET(0);
    case BHL_PM_PM_Particle:host_pmove->PM_Particle((float*)gs(A(0)),A(1),rt_rf32(G_MEM,c->esp+12),A(3),A(4));PM_RET(0);
    case BHL_PM_PM_TestPlayerPosition:PM_RET(host_pmove->PM_TestPlayerPosition((float*)gs(A(0)),(pmtrace_t*)gs(A(1))));
    case BHL_PM_PM_StuckTouch:{host_pmove->PM_StuckTouch(A(0),(pmtrace_t*)gs(A(1)));BhlPlayerMove *g=(void*)(G_MEM+guest_pmove);g->numtouch=host_pmove->numtouch;memcpy(g->touchindex,host_pmove->touchindex,sizeof g->touchindex);PM_RET(0);}
    case BHL_PM_PM_PointContents:PM_RET(host_pmove->PM_PointContents((float*)gs(A(0)),(int*)gs(A(1))));
    case BHL_PM_PM_TruePointContents:PM_RET(host_pmove->PM_TruePointContents((float*)gs(A(0))));
    case BHL_PM_PM_HullPointContents:PM_RET(host_pmove->PM_HullPointContents(hull_host(A(0)),A(1),(float*)gs(A(2))));
    case BHL_PM_PM_GetModelType:PM_RET(host_pmove->PM_GetModelType(model_host(A(0))));
    case BHL_PM_PM_GetModelBounds:host_pmove->PM_GetModelBounds(model_host(A(0)),(float*)gs(A(1)),(float*)gs(A(2)));PM_RET(0);
    case BHL_PM_PM_TraceModel:{
        physent_t temp;trace_t trace={0};float fraction=host_pmove->PM_TraceModel(movement_physent(A(0),&temp),(float*)gs(A(1)),(float*)gs(A(2)),&trace);
        /* Pinned PM_TraceModel always clears ent; trace_t's trailing pointer
           otherwise makes this distinct from the pointer-free pmtrace_t. */
        if(trace.ent)movement_error("TraceModel unexpectedly returned an edict");
        memcpy(G_MEM+A(3),&trace,48);rt_w32(G_MEM,A(3)+48,0);rt_w32(G_MEM,A(3)+52,trace.hitgroup);FPUSH(fraction);PM_RET(0);
    }
    case BHL_PM_PM_HullForBsp:{physent_t temp;PM_RET(hull_guest(host_pmove->PM_HullForBsp(movement_physent(A(0),&temp),(float*)gs(A(1)))));}
    case BHL_PM_PM_PlayerTrace:{/* MSVC cdecl struct return: hidden destination is stack argument zero. */
        pmtrace_t result=host_pmove->PM_PlayerTrace((float*)gs(A(1)),(float*)gs(A(2)),A(3),A(4));memcpy(G_MEM+A(0),&result,sizeof result);PM_RET(A(0));
    }
    case BHL_PM_PM_PlayerTraceEx:{uint32_t previous=ignore_function;ignore_function=A(4);pmtrace_t result=host_pmove->PM_PlayerTraceEx((float*)gs(A(1)),(float*)gs(A(2)),A(3),A(4)?movement_ignore:NULL);ignore_function=previous;memcpy(G_MEM+A(0),&result,sizeof result);PM_RET(A(0));}
    case BHL_PM_PM_TestPlayerPositionEx:{uint32_t previous=ignore_function;ignore_function=A(2);int r=host_pmove->PM_TestPlayerPositionEx((float*)gs(A(0)),(pmtrace_t*)gs(A(1)),A(2)?movement_ignore:NULL);ignore_function=previous;PM_RET(r);}
    case BHL_PM_PM_TraceLine:case BHL_PM_PM_TraceLineEx:{
        uint32_t previous=ignore_function;ignore_function=A(4);pmtrace_t *result=slot==BHL_PM_PM_TraceLine?host_pmove->PM_TraceLine((float*)gs(A(0)),(float*)gs(A(1)),A(2),A(3),A(4)):host_pmove->PM_TraceLineEx((float*)gs(A(0)),(float*)gs(A(1)),A(2),A(3),A(4)?movement_ignore:NULL);ignore_function=previous;
        static uint32_t trace;if(!result)PM_RET(0);if(!trace)trace=bhl_heap_alloc(sizeof *result,1);memcpy(G_MEM+trace,result,sizeof *result);PM_RET(trace);
    }
    case BHL_PM_PM_PlaySound:host_pmove->PM_PlaySound(A(0),gs(A(1)),rt_rf32(G_MEM,c->esp+12),rt_rf32(G_MEM,c->esp+16),A(4),A(5));PM_RET(0);
    case BHL_PM_PM_TraceTexture:PM_RET(copy_string(host_pmove->PM_TraceTexture(A(0),(float*)gs(A(1)),(float*)gs(A(2)))));
    case BHL_PM_PM_PlaybackEventFull:host_pmove->PM_PlaybackEventFull(A(0),A(1),A(2),rt_rf32(G_MEM,c->esp+16),(float*)gs(A(4)),(float*)gs(A(5)),rt_rf32(G_MEM,c->esp+28),rt_rf32(G_MEM,c->esp+32),A(8),A(9),A(10),A(11));PM_RET(0);
    case BHL_PM_COM_FileSize:PM_RET(host_pmove->COM_FileSize(gs(A(0))));
    case BHL_PM_COM_LoadFile:{int size=0;byte *file=host_pmove->COM_LoadFile(gs(A(0)),A(1),&size);if(A(2))rt_w32(G_MEM,A(2),size);if(!file)PM_RET(0);if(size<0)abort();uint32_t p=bhl_heap_alloc(size+1,1);memcpy(G_MEM+p,file,size);host_pmove->COM_FreeFile(file);PM_RET(p);}
    case BHL_PM_COM_FreeFile:if(!bhl_heap_free(A(0)))abort();PM_RET(0);
    case BHL_PM_memfgets:{
        uint32_t p=A(0),offset=A(2),buffer=A(3);int pos=(int)rt_r32(G_MEM,offset),size=(int)A(1),cap=(int)A(4);
        if(!p||!offset||!buffer||cap<=0||pos<0||pos>=size)PM_RET(0);
        int n=0;while(pos<size&&n<cap-1){char ch=G_MEM[p+pos++];G_MEM[buffer+n++]=ch;if(ch=='\n')break;}
        G_MEM[buffer+n]=0;rt_w32(G_MEM,offset,pos);PM_RET(buffer);
    }
    default:movement_error(pm_names[slot]);
    }
}
#undef PM_RET
