/* CL_LoadModel scans the entire precache table. Original v3 repeatedly asks
 * for the same effect models. Cache successful names, but validate the live
 * index, pointer, name and map/video epoch before using a hit. Missing models
 * are never cached, since later precaching can make them available. */
typedef struct ClientModelLookup {char name[64];model_t *host;int index;uint32_t epoch;} ClientModelLookup;
static ClientModelLookup client_model_lookups[256];
static int client_model_key(const char *name,char key[64],unsigned *bucket)
{
    if(!name)return 0;unsigned hash=2166136261u;
    for(unsigned i=0;i<64;i++){
        unsigned char c=name[i];if(c>=128)return 0;
        if(!c){key[i]=0;*bucket=hash&255;return i!=0;}
        if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';key[i]=c;hash=(hash^c)*16777619u;
    }
    return 0;
}
static model_t *client_load_model(const char *name,int *index)
{
    char key[64],current[64];unsigned bucket,unused;
    if(!engine.pfnGetModelByIndex||!client_model_key(name,key,&bucket))return engine.CL_LoadModel(name,index);
    ClientModelLookup *p=client_model_lookups+bucket;
    if(p->epoch==model_epoch&&!strcmp(p->name,key)&&p->host==engine.pfnGetModelByIndex(p->index)&&
       client_model_key(p->host->name,current,&unused)&&!strcmp(current,key)){
        if(index)*index=p->index;return p->host;
    }
    int number=-1;model_t *host=engine.CL_LoadModel(name,&number);if(index)*index=number;
    if(host&&number>0){strcpy(p->name,key);p->host=host;p->index=number;p->epoch=model_epoch;}
    return host;
}
