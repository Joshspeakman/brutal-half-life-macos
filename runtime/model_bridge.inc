/* Shared model identity and the pointer-free model data used at the ABI.
 * Engine-owned BSP graphs are opaque until a dedicated graph adapter exists.
 * Their tokens live in the unreadable thunk area: direct guest access faults
 * instead of silently reading an empty or truncated native structure. */
#include "com_model.h"
extern int bhl_heap_free(uint32_t);
typedef struct BhlHull {uint32_t clipnodes,planes;int firstclipnode,lastclipnode;vec3_t clip_mins,clip_maxs;} BhlHull;
typedef struct BhlModel {
    char name[64];int needload,type,numframes;uint32_t mempool;int flags;vec3_t mins,maxs;float radius;
    int firstmodelsurface,nummodelsurfaces,numsubmodels;uint32_t submodels;
    int numplanes;uint32_t planes;int numleafs;uint32_t leafs;int numvertexes;uint32_t vertexes;
    int numedges;uint32_t edges;int numnodes;uint32_t nodes;int numtexinfo;uint32_t texinfo;
    int numsurfaces;uint32_t surfaces;int numsurfedges;uint32_t surfedges;int numclipnodes;uint32_t clipnodes;
    int nummarksurfaces;uint32_t marksurfaces;BhlHull hulls[4];int numtextures;uint32_t textures;
    uint32_t visdata,lightdata,entities,cache;
} BhlModel;
_Static_assert(sizeof(BhlModel)==392,"guest model ABI");
_Static_assert(sizeof(BhlHull)==40,"guest hull ABI");
typedef struct ModelData {const void *host;uint32_t guest,size,epoch;struct ModelData *next;} ModelData;
static ModelData *model_data;
typedef struct ModelMirror {const model_t *host;uint32_t guest,epoch;model_t snapshot;struct ModelMirror *next;} ModelMirror;
static ModelMirror *model_mirrors;
static uint32_t model_epoch=1;
static void model_invalidate(void){if(!++model_epoch)abort();}
/* Video/map initialization invalidates borrowed model storage. Descriptors
 * keep their identity, but previous levels must not retain whole BSP/studio
 * copies. Opaque token slots are reusable after this boundary. */
static void model_reset(void)
{
    model_invalidate();ModelData **at=&model_data;
    while(*at){ModelData *d=*at;if(d->size){*at=d->next;bhl_heap_free(d->guest);free(d);}else{d->host=NULL;at=&d->next;}}
}
static unsigned opaque_serial;
static void model_fault(const char *what){fprintf(stderr,"BHL native model: unimplemented or invalid %s\n",what);abort();}
static uint32_t model_opaque(const void *host,const char *type)
{
    if(!host)return 0;for(ModelData *d=model_data;d;d=d->next)if(d->host==host&&!d->size)return d->guest;
    for(ModelData *d=model_data;d;d=d->next)if(!d->host&&!d->size){d->host=host;return d->guest;}
    char name[128];snprintf(name,sizeof name,BHL_MODEL_NAMESPACE "opaque-%s-%u",type,opaque_serial++);
    ModelData *d=calloc(1,sizeof *d);if(!d)abort();d->host=host;d->guest=bhl_thunk(name,NULL);d->next=model_data;model_data=d;return d->guest;
}
static uint32_t model_bytes(const void *host,uint32_t bytes)
{
    if(!host)return 0;if(bytes>32*1024*1024)model_fault("data size");if(!bytes)return model_opaque(host,"zero-length");
    for(ModelData *d=model_data;d;d=d->next)if(d->size&&d->host==host&&d->size==bytes){if(d->epoch!=model_epoch){memcpy(G_MEM+d->guest,host,bytes);d->epoch=model_epoch;}return d->guest;}
    ModelData *d=calloc(1,sizeof *d);if(!d)abort();d->host=host;d->size=bytes;d->epoch=model_epoch;d->guest=bhl_heap_alloc(bytes,0);if(!d->guest)abort();memcpy(G_MEM+d->guest,host,bytes);d->next=model_data;model_data=d;return d->guest;
}
static const void *model_host_data(uint32_t p)
{
    if(!p)return NULL;for(ModelData *d=model_data;d;d=d->next)if(p==d->guest||(d->size&&p>d->guest&&p-d->guest<d->size))return (const char*)d->host+(p-d->guest);
    model_fault("unrecognized model data pointer");return NULL;
}
static uint32_t studio_data(const void *host)
{
    if(!host)return 0;uint32_t magic,version,size;memcpy(&magic,host,4);memcpy(&version,(char*)host+4,4);memcpy(&size,(char*)host+72,4);
    if(magic!=0x54534449||version!=10||size<244||size>32*1024*1024)model_fault("studio header");return model_bytes(host,size);
}
static uint32_t model_guest(const model_t *h)
{
    if(!h)return 0;ModelMirror *m=model_mirrors;while(m&&m->host!=h)m=m->next;
    if(!m){m=calloc(1,sizeof *m);if(!m)abort();m->host=h;m->guest=bhl_heap_alloc(sizeof(BhlModel),1);m->next=model_mirrors;model_mirrors=m;}
    /* Model descriptors are engine-owned and unchanged through most studio
       callbacks. Avoid re-walking every model array and entity text lump. */
    if(m->epoch==model_epoch&&!memcmp(&m->snapshot,h,sizeof *h))return m->guest;
    if(m->epoch==model_epoch&&strcmp(m->snapshot.name,h->name))model_invalidate();
    int replaced=m->snapshot.mempool!=h->mempool||strcmp(m->snapshot.name,h->name);
    memcpy(&m->snapshot,h,sizeof *h);m->epoch=model_epoch;
    BhlModel *g=(void*)(G_MEM+m->guest);memcpy(g,h,124);
    /* The original client uses studio submodels as its own 16-entry cache
       array. Preserve that guest allocation separately from Xash's cache. */
    if(h->type!=mod_studio)g->submodels=model_opaque(h->submodels,"submodels");
    else if(replaced)g->submodels=0;
    g->numplanes=h->numplanes;g->planes=model_bytes(h->planes,h->numplanes*sizeof *h->planes);
    g->numleafs=h->numleafs;g->leafs=model_opaque(h->leafs,"leafs");
    g->numvertexes=h->numvertexes;g->vertexes=model_bytes(h->vertexes,h->numvertexes*sizeof *h->vertexes);
    g->numedges=h->numedges;g->edges=model_bytes(h->edges16,h->numedges*sizeof *h->edges16);
    g->numnodes=h->numnodes;g->nodes=model_opaque(h->nodes,"nodes");
    g->numtexinfo=h->numtexinfo;g->texinfo=model_opaque(h->texinfo,"texinfo");
    g->numsurfaces=h->numsurfaces;g->surfaces=model_opaque(h->surfaces,"surfaces");
    g->numsurfedges=h->numsurfedges;g->surfedges=model_bytes(h->surfedges,h->numsurfedges*sizeof *h->surfedges);
    g->numclipnodes=h->numclipnodes;
    if(h->flags&MODEL_QBSP2)model_fault("extended BSP collision format");
    g->clipnodes=model_bytes(h->clipnodes16,h->numclipnodes*sizeof *h->clipnodes16);
    g->nummarksurfaces=h->nummarksurfaces;g->marksurfaces=model_opaque(h->marksurfaces,"marksurfaces");
    for(unsigned i=0;i<4;i++){const hull_t *a=&h->hulls[i];BhlHull *b=&g->hulls[i];b->firstclipnode=a->firstclipnode;b->lastclipnode=a->lastclipnode;
        b->clipnodes=model_bytes(a->clipnodes16,(a->lastclipnode+1)*sizeof *a->clipnodes16);b->planes=model_bytes(a->planes,h->numplanes*sizeof *a->planes);memcpy(b->clip_mins,a->clip_mins,24);}
    g->numtextures=h->numtextures;g->textures=model_opaque(h->textures,"textures");g->visdata=model_opaque(h->visdata,"visdata");g->lightdata=model_opaque(h->lightdata,"lightdata");
    g->entities=h->entities?model_bytes(h->entities,strlen(h->entities)+1):0;
    g->cache=h->type==mod_studio?studio_data(h->cache.data):model_opaque(h->cache.data,"cache");return m->guest;
}
static model_t *model_host(uint32_t p)
{if(!p)return NULL;for(ModelMirror *m=model_mirrors;m;m=m->next)if(m->guest==p)return (model_t*)m->host;model_fault("model handle");return NULL;}
