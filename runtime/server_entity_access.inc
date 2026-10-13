/* The original server reaches entvars through cached pointers in its private
 * objects. The translated memory barrier observes these accesses directly. */
static unsigned char server_pending[RT_WATCH_PAGES];
static void server_watch_suspend(void)
{if(server_watch_running){rt_watch_end();server_watch_running=0;}}
static void server_touch(unsigned i)
{if(server_access_epoch&&entity_snapshots[i].access_epoch!=server_access_epoch)pull_entity(i);}
static void server_lazy_page(uint32_t page)
{
    uint64_t start=RT_WATCH_BASE+4096ull*page,end=start+4096;
    if(start<guest_edicts)start=guest_edicts;
    uint64_t limit=(uint64_t)guest_edicts+edict_capacity*sizeof(BhlEdict);
    if(end>limit)end=limit;
    if(start>=end)return;
    /* A page can contain parts of multiple edicts. Materialize them all;
       later touches of a second page cannot overwrite a pending guest write. */
    unsigned first=(start-guest_edicts)/sizeof(BhlEdict);
    unsigned last=(end-guest_edicts+sizeof(BhlEdict)-1)/sizeof(BhlEdict);
    if(string_validation_batch||!++string_validation_serial)abort();
    string_validation_batch=string_validation_serial;
    for(unsigned i=first;i<last;i++)server_touch(i);
    string_validation_batch=0;
}
static void server_lazy_materialize(void)
{
    if(server_demand_enabled&&server_call_depth)server_demand_rearm=1;
    server_demand_enabled=0;
    if(!server_access_epoch)return;
    BHL_PROFILE("server-lazy-fallback",0);bridge_fallback("server");
    uint64_t epoch=server_access_epoch;server_access_epoch=0;server_watch_suspend();
    if(string_validation_batch||!++string_validation_serial)abort();
    string_validation_batch=string_validation_serial;
    for(unsigned i=0;i<edict_capacity;i++)if(entity_snapshots[i].access_epoch!=epoch)pull_entity(i);
    string_validation_batch=0;server_access_count=0;
}
static void server_lazy_begin(void)
{
    server_watch_suspend();
    /* A native service may have written into a borrowed guest output. Keep
       the complete path for that boundary rather than dropping its write. */
    for(unsigned n=0;n<server_access_count;n++){
        unsigned i=server_accessed[n];BhlEdict *g=(void*)(G_MEM+guest_edicts+i*sizeof *g);
        if(i<edict_capacity&&!edict_base[i].free&&memcmp(&entity_snapshots[i].guest,&g->v,sizeof g->v)){
            bridge_boundary="lazy-begin-native-write";server_lazy_materialize();pull_entities();return;
        }
    }
    if(!++server_access_serial)abort();server_access_epoch=server_access_serial;server_access_count=0;
    memset(server_pending,0,sizeof server_pending);
    unsigned first=(guest_edicts-RT_WATCH_BASE)>>12;
    unsigned end=(guest_edicts-RT_WATCH_BASE+edict_capacity*sizeof(BhlEdict)+4095)>>12;
    memset(server_pending+first,1,end-first);
    rt_watch_begin(server_pending,server_lazy_page,server_lazy_materialize);server_watch_running=1;
}

static void server_access_verify(void)
{
    if(!server_access_epoch||!bridge_verify_entities)return;
    for(unsigned i=0;i<edict_capacity;i++)if(!edict_base[i].free&&entity_snapshots[i].valid&&entity_snapshots[i].access_epoch!=server_access_epoch){
        BhlEdict *g=(void*)(G_MEM+guest_edicts+i*sizeof *g);
        if(memcmp(&entity_snapshots[i].guest,&g->v,sizeof g->v)){
            fprintf(stderr,"BHL untracked server entity write: %u\n",i);abort();
        }
    }
}
