/* Save files are still encoded by the original game. Only the in-memory
 * engine structures and TYPEDESCRIPTION offsets cross this boundary. */
typedef struct BhlSaveLevel {char map[32],landmark[32];uint32_t entity;vec3_t origin;} BhlSaveLevel;
typedef struct BhlSaveEntity {int id;uint32_t entity;int location,size,flags;uint32_t classname;} BhlSaveEntity;
typedef struct BhlSave {
    uint32_t base,current;int size,capacity,token_size,token_count;uint32_t tokens;
    int index,table_count,connection_count;uint32_t table;BhlSaveLevel levels[16];
    int use_landmark;char landmark[20];vec3_t offset;float time;char map[32];
} BhlSave;
typedef struct BhlSaveField {int type;uint32_t name;int offset;short count,flags;} BhlSaveField;
_Static_assert(sizeof(BhlSaveLevel)==80,"save level ABI");
_Static_assert(sizeof(BhlSaveEntity)==24,"save entity ABI");
_Static_assert(sizeof(BhlSave)==1396,"save context ABI");
_Static_assert(sizeof(BhlSaveField)==16,"save field ABI");
typedef struct SaveText {uint32_t guest;struct SaveText *next;} SaveText;
static SaveText *save_texts[4096];
extern GuestFn rt_lookup(uint32_t);
static void save_fault(const char *name){fprintf(stderr,"BHL native save: unimplemented or invalid %s\n",name);abort();}
static uint32_t save_text(const char *s)
{
    if(!s)return 0;unsigned hash=2166136261u;for(const unsigned char *p=(void*)s;*p;p++)hash=(hash^*p)*16777619u;hash&=4095;
    for(SaveText *t=save_texts[hash];t;t=t->next)if(!strcmp(gs(t->guest),s))return t->guest;
    SaveText *t=calloc(1,sizeof *t);if(!t)abort();t->guest=copy_string(s);t->next=save_texts[hash];save_texts[hash]=t;return t->guest;
}
static uint32_t save_function_name(uint32_t address)
{
    if(!address)return 0;
    for(unsigned i=0;i<bhl_module_count;i++)if(!strcmp(bhl_modules[i].name,"hl.dll")){
        const BhlModule *m=bhl_modules+i;
        for(unsigned j=0;j<m->export_count;j++)if(m->exports[j].address==address)return save_text(m->exports[j].name);
        /* v3 installs some unexported think/use callbacks. Preserve these
           instead of dropping them; the binary digest prevents resolving a
           local save against a different release at the same relative RVA. */
        if(address>=m->base&&address-m->base<m->size&&rt_lookup(address)){
            char name[128];snprintf(name,sizeof name,"bhl-native:%s:%08x",m->sha256,address-m->base);return save_text(name);
        }
    }
    save_fault("function is not a translated server entry");return 0;
}
static uint32_t save_function_address(const char *name)
{
    if(!name)return 0;uint32_t address=bhl_export("hl.dll",name);if(address)return address;
    for(unsigned i=0;i<bhl_module_count;i++)if(!strcmp(bhl_modules[i].name,"hl.dll")){
        const BhlModule *m=bhl_modules+i;char prefix[112];snprintf(prefix,sizeof prefix,"bhl-native:%s:",m->sha256);size_t n=strlen(prefix);
        if(strncmp(name,prefix,n)||strlen(name+n)!=8)return 0;
        for(unsigned j=0;j<8;j++)if(!isxdigit((unsigned char)name[n+j]))return 0;
        unsigned long rva=strtoul(name+n,NULL,16);if(rva>=m->size)return 0;address=m->base+rva;return rt_lookup(address)?address:0;
    }
    return 0;
}
typedef struct SaveMirror {
    SAVERESTOREDATA *host;uint32_t guest,buffer,tokens,table;
    unsigned capacity,token_capacity,table_capacity,active;struct SaveMirror *next;
} SaveMirror;
static SaveMirror *save_mirrors;
static void save_collect(void)
{
    SaveMirror **at=&save_mirrors;
    while(*at){SaveMirror *p=*at;if(p->active||p->host==host_globals->pSaveData){at=&p->next;continue;}
        *at=p->next;bhl_heap_free(p->guest);bhl_heap_free(p->buffer);bhl_heap_free(p->tokens);bhl_heap_free(p->table);free(p);}
}
static void save_reserve(uint32_t *p,unsigned *capacity,unsigned size)
{if(size>*capacity){bhl_heap_free(*p);*p=bhl_heap_alloc(size,1);if(!*p)abort();*capacity=size;}}
static SaveMirror *save_find(SAVERESTOREDATA *h)
{
    if(!h)return NULL;for(SaveMirror *p=save_mirrors;p;p=p->next)if(p->host==h)return p;
    SaveMirror *p=calloc(1,sizeof *p);if(!p)abort();p->host=h;p->guest=bhl_heap_alloc(sizeof(BhlSave),1);p->next=save_mirrors;save_mirrors=p;return p;
}
static void save_pull(SaveMirror *p,int buffer)
{
    SAVERESTOREDATA *h=p->host;BhlSave *g=(void*)(G_MEM+p->guest);
    if(h->bufferSize<0||h->bufferSize>64*1024*1024||h->tokenCount<0||h->tokenCount>65536||h->tableCount<0||h->tableCount>8192||h->connectionCount<0||h->connectionCount>16)save_fault("context counts");
    /* Pinned SaveInit owns one allocation: descriptor followed by buffer.
       Restore can advance pBaseData while preserving the total capacity. */
    char *origin=(char*)(h+1);intptr_t base=(uintptr_t)h->pBaseData-(uintptr_t)origin,current=(uintptr_t)h->pCurrentData-(uintptr_t)origin;
    /* Adjacent-level discovery uses a zeroed stack descriptor with just its
       landmark list; no save stream exists during ParmsChangeLevel. */
    if(!h->bufferSize&&!h->pBaseData&&!h->pCurrentData)base=current=0;
    if(base<0||current<0||base>h->bufferSize||current>h->bufferSize){fprintf(stderr,"BHL save cursor base=%td current=%td capacity=%d size=%d table=%d\n",base,current,h->bufferSize,h->size,h->tableCount);save_fault("buffer cursor");}
    save_reserve(&p->buffer,&p->capacity,h->bufferSize);
    save_reserve(&p->tokens,&p->token_capacity,h->tokenCount*4);
    save_reserve(&p->table,&p->table_capacity,h->tableCount*sizeof(BhlSaveEntity));
    if(buffer&&h->bufferSize)memcpy(G_MEM+p->buffer,origin,h->bufferSize);
    g->base=h->pBaseData?p->buffer+base:0;g->current=h->pCurrentData?p->buffer+current:0;g->size=h->size;g->capacity=h->bufferSize;
    g->token_size=h->tokenSize;g->token_count=h->tokenCount;g->tokens=h->pTokens?p->tokens:0;
    if(h->tokenCount&&!h->pTokens)save_fault("missing tokens");
    for(int i=0;i<h->tokenCount;i++)rt_w32(G_MEM,p->tokens+4*i,save_text(h->pTokens[i]));
    g->index=h->currentIndex;g->table_count=h->tableCount;g->connection_count=h->connectionCount;g->table=h->pTable?p->table:0;
    if(h->tableCount&&!h->pTable)save_fault("missing entity table");
    for(int i=0;i<h->tableCount;i++){ENTITYTABLE *a=h->pTable+i;BhlSaveEntity *b=(void*)(G_MEM+p->table+i*sizeof *b);
        b->id=a->id;b->entity=guest_edict(a->pent);b->location=a->location;b->size=a->size;b->flags=a->flags;b->classname=guest_string(a->classname);}
    for(int i=0;i<16;i++){LEVELLIST *a=h->levelList+i;BhlSaveLevel *b=g->levels+i;memcpy(b,a,64);b->entity=guest_edict(a->pentLandmark);memcpy(b->origin,a->vecLandmarkOrigin,12);}
    memcpy(&g->use_landmark,&h->fUseLandmark,72);
}
static void save_push(SaveMirror *p,int buffer)
{
    SAVERESTOREDATA *h=p->host;BhlSave *g=(void*)(G_MEM+p->guest);
    if(g->capacity!=h->bufferSize||g->token_count!=h->tokenCount||g->table_count!=h->tableCount||g->connection_count<0||g->connection_count>16||g->size<0||g->size>h->bufferSize)save_fault("changed save allocation");
    if(h->bufferSize?(g->base-p->buffer>(unsigned)h->bufferSize||g->current-p->buffer>(unsigned)h->bufferSize):(g->base||g->current))save_fault("guest buffer cursor");
    if(g->tokens!=(h->pTokens?p->tokens:0)||g->table!=(h->pTable?p->table:0))save_fault("changed table pointers");
    char *origin=(char*)(h+1);if(buffer&&h->bufferSize)memcpy(origin,G_MEM+p->buffer,h->bufferSize);
    h->pBaseData=g->base?origin+(g->base-p->buffer):NULL;h->pCurrentData=g->current?origin+(g->current-p->buffer):NULL;h->size=g->size;h->tokenSize=g->token_size;
    for(int i=0;i<h->tokenCount;i++)h->pTokens[i]=gs(rt_r32(G_MEM,p->tokens+4*i));
    h->currentIndex=g->index;h->connectionCount=g->connection_count;
    for(int i=0;i<h->tableCount;i++){ENTITYTABLE *a=h->pTable+i;BhlSaveEntity *b=(void*)(G_MEM+p->table+i*sizeof *b);
        a->id=b->id;a->pent=host_edict(b->entity);a->location=b->location;a->size=b->size;a->flags=b->flags;a->classname=host_string(b->classname);}
    for(int i=0;i<16;i++){LEVELLIST *a=h->levelList+i;BhlSaveLevel *b=g->levels+i;memcpy(a,b,64);a->pentLandmark=host_edict(b->entity);memcpy(a->vecLandmarkOrigin,b->origin,12);}
    memcpy(&h->fUseLandmark,&g->use_landmark,72);
}
static uint32_t save_global_pull(void)
{save_collect();SaveMirror *p=save_find(host_globals->pSaveData);if(!p)return 0;save_pull(p,!p->active);return p->guest;}
static void save_global_push(void)
{
    SaveMirror *p=save_find(host_globals->pSaveData);uint32_t g=rt_r32(G_MEM,guest_globals+156);
    if(g!=(p?p->guest:0))save_fault("global save context reassignment");if(p)save_push(p,!p->active);
}
static SaveMirror *save_begin(SAVERESTOREDATA *h)
{SaveMirror *p=save_find(h);if(!p)save_fault("null context");save_pull(p,!p->active);p->active++;return p;}
static void save_end(SaveMirror *p)
{if(!p->active)abort();p->active--;save_push(p,!p->active);}

static unsigned save_field_size(FIELDTYPE type,int native)
{
    switch(type){
    case FIELD_VECTOR:case FIELD_POSITION_VECTOR:return 12;
    case FIELD_SHORT:return 2;case FIELD_CHARACTER:return 1;
    case FIELD_EDICT:case FIELD_EVARS:case FIELD_CLASSPTR:case FIELD_POINTER:case FIELD_FUNCTION:return native?sizeof(void*):4;
    case FIELD_EHANDLE:return native?16:8;
    case FIELD_FLOAT:case FIELD_STRING:case FIELD_ENTITY:case FIELD_INTEGER:case FIELD_BOOLEAN:case FIELD_TIME:case FIELD_MODELNAME:case FIELD_SOUNDNAME:return 4;
    default:save_fault("field type");return 0;
    }
}
static void save_field_copy(void *native,uint32_t guest,FIELDTYPE type,int count,int to_guest)
{
    unsigned hs=save_field_size(type,1),gsz=save_field_size(type,0);
    for(int i=0;i<count;i++){
        char *h=(char*)native+i*hs;uint32_t g=guest+i*gsz,v=0;void *pointer=NULL;
        switch(type){
        case FIELD_STRING:case FIELD_MODELNAME:case FIELD_SOUNDNAME:
            if(to_guest){memcpy(&v,h,4);rt_w32(G_MEM,g,guest_string(v));}else{v=host_string(rt_r32(G_MEM,g));memcpy(h,&v,4);}break;
        case FIELD_EDICT:case FIELD_EVARS:
            if(to_guest){memcpy(&pointer,h,sizeof pointer);v=guest_edict(type==FIELD_EVARS&&pointer?(edict_t*)((char*)pointer - offsetof(edict_t,v)):pointer);if(v&&type==FIELD_EVARS)v+=offsetof(BhlEdict,v);rt_w32(G_MEM,g,v);}
            else{v=rt_r32(G_MEM,g);edict_t *e=host_edict(v&&type==FIELD_EVARS?v - offsetof(BhlEdict,v):v);pointer=e&&type==FIELD_EVARS?(void*)&e->v:e;memcpy(h,&pointer,sizeof pointer);}break;
        case FIELD_ENTITY:
            if(to_guest){memcpy(&v,h,4);if(v%sizeof(edict_t))save_fault("entity offset");rt_w32(G_MEM,g,v/sizeof(edict_t)*sizeof(BhlEdict));}
            else{v=rt_r32(G_MEM,g);if(v%sizeof(BhlEdict))save_fault("guest entity offset");v=v/sizeof(BhlEdict)*sizeof(edict_t);memcpy(h,&v,4);}break;
        case FIELD_CLASSPTR:case FIELD_EHANDLE:case FIELD_POINTER:case FIELD_FUNCTION:
            /* Engine-owned field schemas contain none of these. Original
               private class fields never leave their 32-bit address space. */
            save_fault("native pointer field schema");break;
        default:if(to_guest)memcpy(G_MEM+g,h,gsz);else memcpy(h,G_MEM+g,gsz);break;
        }
    }
}
