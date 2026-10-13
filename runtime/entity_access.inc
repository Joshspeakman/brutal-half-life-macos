/* Render scopes keep original entity pointer identities. Translated memory
 * accesses materialize every mirror on a page before the load/store executes.
 * This includes pointers cached by the original DLL across callbacks. */
typedef struct EntityPage {EntityCopy *entity;struct EntityPage *next;} EntityPage;
static EntityCopy *entity_unwatched;
static EntityPage *entity_pages[RT_WATCH_PAGES];
static unsigned char entity_registered[RT_WATCH_PAGES],entity_pending[RT_WATCH_PAGES];
static void entity_lazy_register(EntityCopy *p)
{
    uint32_t start=p->guest-(p->source==3?0:68);
    uint64_t end=(uint64_t)p->guest+sizeof(BhlClientEntity);
    if(start<RT_WATCH_BASE||end>RT_WATCH_BASE+RT_WATCH_SIZE)
        {p->unwatched_next=entity_unwatched;entity_unwatched=p;return;}
    rt_watch_register_range(start,end-start);
    for(unsigned page=(start-RT_WATCH_BASE)>>12;page<((end-RT_WATCH_BASE+4095)>>12);page++){
        EntityPage *node=malloc(sizeof *node);if(!node)abort();
        node->entity=p;node->next=entity_pages[page];entity_pages[page]=node;
        entity_registered[page]=1;
        if(entity_access_epoch)entity_pending[page]=1;
    }
}
static void entity_lazy_page(uint32_t page)
{
    for(EntityPage *node=entity_pages[page];node;node=node->next){
        EntityCopy *p=node->entity;
        if(p->temp)temp_lazy_pull(p->temp);
        if(p->access_epoch!=entity_access_epoch)entity_pull(p);
    }
}
static void entity_watch_suspend(void)
{if(entity_watch_running){rt_watch_end();entity_watch_running=0;}}
static void entity_lazy_begin(void)
{
    entity_watch_suspend();
    for(EntityCopy *p=entity_accessed;p;p=p->access_next){
        if(p->active&&p->valid&&memcmp(&p->guest_snapshot,G_MEM+p->guest,sizeof p->guest_snapshot)){
            entity_lazy_materialize();entities_pull();return;
        }
    }
    if(temp_lazy_pending()){entity_lazy_materialize();entities_pull();return;}
    if(!++entity_access_serial)abort();
    entity_access_epoch=entity_access_serial;entity_accessed=NULL;temp_lazy_begin();
    memcpy(entity_pending,entity_registered,sizeof entity_pending);
    rt_watch_begin(entity_pending,entity_lazy_page,entity_lazy_materialize);entity_watch_running=1;
    /* Guest-owned static/stack entities lie outside the watched heap. */
    for(EntityCopy *p=entity_unwatched;p;p=p->unwatched_next)entity_pull(p);
}
static void entity_lazy_materialize(void)
{
    entity_demand_enabled=0;
    if(!entity_access_epoch)return;
    BHL_PROFILE("client-lazy-fallback",0);
    uint64_t epoch=entity_access_epoch;entity_access_epoch=0;entity_watch_suspend();
    temp_lazy_materialize(epoch);
    /* Accessed mirrors may contain pending original writes. Never pull them
       again here: this is also used before nested/reentrant native services. */
    model_validation_begin();
    for(EntityCopy *p=entities;p;p=p->next)if(p->access_epoch!=epoch)entity_pull(p);
    model_validation_end();entity_accessed=NULL;
}
static void entity_access_verify(void)
{
    if(!entity_access_epoch||!bridge_verify_entities)return;
    temp_access_verify();
    for(EntityCopy *p=entities;p;p=p->next)if(p->active&&p->valid&&p->access_epoch!=entity_access_epoch&&memcmp(&p->guest_snapshot,G_MEM+p->guest,sizeof p->guest_snapshot)){
        fprintf(stderr,"BHL untracked client entity write: %08x source=%d index=%d epoch=%llu accessed=%llu\n",p->guest,p->source,p->index,(unsigned long long)entity_access_epoch,(unsigned long long)p->access_epoch);
        const unsigned char *before=(const void*)&p->guest_snapshot,*after=G_MEM+p->guest;
        unsigned shown=0;for(unsigned i=0;i<sizeof p->guest_snapshot&&shown<16;i++)if(before[i]!=after[i]){fprintf(stderr,"  byte %u: %02x -> %02x\n",i,before[i],after[i]);shown++;}
        abort();
    }
}
static void entity_lazy_finish(void)
{
    entity_access_verify();
    BHL_PROFILE("client-lazy-complete",0);
    entity_access_epoch=0;entity_watch_suspend();
    for(EntityCopy *p=entity_accessed;p;p=p->access_next)entity_push(p);
    entity_accessed=NULL;
}
static void entity_pointer_escape(uint32_t guest)
{
    if(!entity_access_epoch||guest-RT_WATCH_BASE>=RT_WATCH_SIZE)return;
    for(EntityPage *node=entity_pages[(guest-RT_WATCH_BASE)>>12];node;node=node->next){
        EntityCopy *p=node->entity;unsigned prefix=p->source==3?0:68;
        if(guest-(p->guest-prefix)<sizeof(BhlClientEntity)+prefix){
            /* gs() does not carry a length or ownership contract. A native
               borrower therefore gets the complete, coherent memory view. */
            entity_lazy_materialize();return;
        }
    }
}

/* Bounded read-only inputs are consumed synchronously by the pinned engine.
   Outputs or unbounded pointer escapes retain the full fallback above. */
static void *gs_read(uint32_t guest,size_t size)
{
    if(!guest)return NULL;
    if(entity_access_epoch){
        uint64_t end=(uint64_t)guest+size;
        uint64_t start=guest>RT_WATCH_BASE?guest:RT_WATCH_BASE;
        if(end>RT_WATCH_BASE+RT_WATCH_SIZE)end=RT_WATCH_BASE+RT_WATCH_SIZE;
        for(uint64_t address=start&~4095ull;address<end;address+=4096){
            for(EntityPage *node=entity_pages[(address-RT_WATCH_BASE)>>12];node;node=node->next){
                EntityCopy *p=node->entity;
                if(p->temp&&(uint64_t)p->guest-68<end&&(uint64_t)p->guest>start)temp_lazy_pull(p->temp);
                if((uint64_t)p->guest<end&&(uint64_t)p->guest+sizeof(BhlClientEntity)>start&&p->access_epoch!=entity_access_epoch)entity_pull(p);
            }
        }
    }
    return G_MEM+guest;
}
