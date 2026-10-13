/* Successful names retain live index/pointer/name validation. Absent models
 * are cached only with the engine's explicit table/lifetime revision. Older
 * engines without that contract retain the complete lookup for every miss. */
#include <dlfcn.h>
typedef struct ClientModelLookup {char name[64];model_t *host;int index;uint32_t epoch;uint64_t revision;} ClientModelLookup;
static ClientModelLookup client_model_lookups[4096];
static uint64_t (*client_model_revision)(void);
static void client_model_lookup_initialize(void)
{
    Dl_info info={0};if(!engine.CL_LoadModel||!dladdr((void*)engine.CL_LoadModel,&info)||!info.dli_fname)return;
    void *library=dlopen(info.dli_fname,RTLD_LAZY|RTLD_NOLOAD|RTLD_FIRST);if(!library)return;
    client_model_revision=dlsym(library,"Xash_GetModelLookupRevisionV1");dlclose(library);
    if(client_model_revision)fprintf(stderr,"BHL native model lookup revision tracking enabled\n");
}
static int client_model_key(const char *name,char key[64],unsigned *bucket)
{
    if(!name)return 0;unsigned hash=2166136261u;
    for(unsigned i=0;i<64;i++){
        unsigned char c=name[i];if(c>=128)return 0;
        if(!c){key[i]=0;*bucket=hash&4095;return i!=0;}
        if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';key[i]=c;hash=(hash^c)*16777619u;
    }
    return 0;
}
static model_t *client_load_model(const char *name,int *index)
{
    char key[64],current[64];unsigned bucket,unused;
    if(!engine.pfnGetModelByIndex||!client_model_key(name,key,&bucket))return engine.CL_LoadModel(name,index);
    ClientModelLookup *p=client_model_lookups+bucket;
    for(unsigned n=0;n<4096;n++){
        p=client_model_lookups+((bucket+n)&4095);
        if(p->epoch!=model_epoch||!strcmp(p->name,key))break;
    }
    uint64_t revision=client_model_revision?client_model_revision():0;
    if(p->epoch==model_epoch&&!strcmp(p->name,key)){
        if(!p->host&&revision&&p->revision==revision){if(index)*index=-1;return NULL;}
        if(p->host&&(!client_model_revision||p->revision==revision)&&p->host==engine.pfnGetModelByIndex(p->index)&&
           client_model_key(p->host->name,current,&unused)&&!strcmp(current,key)){
            if(index)*index=p->index;return p->host;
        }
    }
    int number=-1;model_t *host=engine.CL_LoadModel(name,&number);if(index)*index=number;
    if((host&&number>0)||(!host&&revision)){
        strcpy(p->name,key);p->host=host;p->index=number;p->epoch=model_epoch;
        /* Native loading can invalidate the revision while answering. */
        p->revision=client_model_revision?client_model_revision():0;
    }
    return host;
}
