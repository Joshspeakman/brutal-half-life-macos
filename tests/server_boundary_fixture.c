#include "../runtime/server_bridge.c"
#include <assert.h>
static edict_t fixture_edicts[8];
static char fixture_strings[65536];static unsigned fixture_used=1,allocations;
static const char *string_value(int id){assert(id>=0&&(unsigned)id<fixture_used);return fixture_strings+id;}
static int string_alloc(const char *text)
{int p=fixture_used;size_t size=strlen(text)+1;assert(size+fixture_used<sizeof fixture_strings);memcpy(fixture_strings+p,text,size);fixture_used+=size;allocations++;return p;}
static edict_t *entity_at(int index){assert(index>=0&&index<8);return fixture_edicts+index;}
static void save_roundtrip(void)
{
    typedef struct {char title[13];int string_id;edict_t *target;float position[3],time;int counts[3];short number;} Record;
    TYPEDESCRIPTION fields[]={
        DEFINE_ARRAY(Record,title,FIELD_CHARACTER,13),DEFINE_FIELD(Record,string_id,FIELD_STRING),
        DEFINE_FIELD(Record,target,FIELD_EDICT),DEFINE_FIELD(Record,position,FIELD_POSITION_VECTOR),
        DEFINE_FIELD(Record,time,FIELD_TIME),DEFINE_ARRAY(Record,counts,FIELD_INTEGER,3),DEFINE_FIELD(Record,number,FIELD_SHORT),
    };
    DLL_FUNCTIONS api;GiveFnptrsToDll(&engine,host_globals);assert(GetEntityAPI(&api,140));
    SAVERESTOREDATA *s=calloc(1,sizeof *s+4096);s->pBaseData=s->pCurrentData=(char*)(s+1);s->bufferSize=4096;
    s->tokenCount=127;s->pTokens=calloc(s->tokenCount,sizeof(char*));s->tableCount=2;s->pTable=calloc(2,sizeof(ENTITYTABLE));
    for(int i=0;i<2;i++){s->pTable[i].id=i;s->pTable[i].pent=fixture_edicts+i;s->pTable[i].classname=fixture_edicts[i].v.classname;}
    s->connectionCount=1;s->levelList[0].pentLandmark=fixture_edicts+1;strcpy(s->levelList[0].mapName,"save-fixture");
    s->fUseLandmark=1;s->vecLandmarkOffset[0]=10;s->time=100;host_globals->pSaveData=s;
    Record source={0};strcpy(source.title,"native-save");source.string_id=string_alloc("retained-string");source.target=fixture_edicts+1;
    source.position[0]=27;source.position[1]=3;source.time=125;source.counts[0]=1;source.counts[1]=-7;source.counts[2]=99;source.number=-55;
    api.pfnSaveWriteFields(s,"fixture-record",&source,fields,sizeof fields/sizeof *fields);
    assert(s->size>0&&s->size<4096&&s->pCurrentData==s->pBaseData+s->size);int written=s->size;
    /* Token strings must remain live after temporary field descriptions are
       gone; source/native pointer width and padding must not enter the file. */
    unsigned populated=0;for(int i=0;i<s->tokenCount;i++)if(s->pTokens[i]){assert(strlen(s->pTokens[i])>0);populated++;}assert(populated>=8);
    Record target={0};s->pCurrentData=s->pBaseData;s->size=0;s->time=200;s->vecLandmarkOffset[0]=20;
    api.pfnSaveReadFields(s,"fixture-record",&target,fields,sizeof fields/sizeof *fields);
    assert(s->size==written&&!strcmp(source.title,target.title));assert(!strcmp(string_value(target.string_id),"retained-string"));
    assert(target.target==fixture_edicts+1&&target.position[0]==37&&target.position[1]==3&&target.time==225);
    assert(!memcmp(target.counts,source.counts,sizeof source.counts)&&target.number==source.number);
    assert(s->levelList[0].pentLandmark==fixture_edicts+1&&!strcmp(s->levelList[0].mapName,"save-fixture"));
    uint32_t function=bhl_export("hl.dll","?AccelerateThink@CApacheHVR@@AAEXXZ");assert(function);
    assert(bhl_export("hl.dll",gs(save_function_name(function)))==function);
    const BhlModule *module=NULL;for(unsigned i=0;i<bhl_module_count;i++)if(!strcmp(bhl_modules[i].name,"hl.dll"))module=bhl_modules+i;
    uint32_t unnamed=module->base+0x11b0e0;const char *name=gs(save_function_name(unnamed));assert(save_function_address(name)==unnamed);
    char mismatch[128];strcpy(mismatch,name);mismatch[11]=mismatch[11]=='a'?'b':'a';assert(!save_function_address(mismatch));
    host_globals->pSaveData=NULL;free(s->pTokens);free(s->pTable);free(s);
    SAVERESTOREDATA transition={0};host_globals->pSaveData=&transition;sync_basic_globals();
    SaveMirror *mirror=save_find(&transition);BhlSave *guest=(void*)(G_MEM+mirror->guest);assert(!guest->base&&!guest->current&&!guest->capacity);
    guest->connection_count=1;guest->levels[0].entity=guest_edict(fixture_edicts+1);strcpy(guest->levels[0].map,"adjacent");push_globals();
    assert(transition.connectionCount==1&&transition.levelList[0].pentLandmark==fixture_edicts+1&&!strcmp(transition.levelList[0].mapName,"adjacent"));host_globals->pSaveData=NULL;
    sync_basic_globals();assert(!save_mirrors);
    puts("Original save serializer roundtrip passed: typed fields, time, landmarks, references, tokens, function names");
}
int main(void)
{
    bhl_initialize();globalvars_t globals={0};host_globals=&globals;globals.maxEntities=8;globals.maxClients=1;globals.pStringBase=fixture_strings;
    engine.pfnAllocString=string_alloc;engine.pfnSzFromIndex=string_value;engine.pfnPEntityOfEntIndexAllEntities=entity_at;
    guest_globals=bhl_heap_alloc(172,1);for(unsigned i=2;i<8;i++)fixture_edicts[i].free=1;
    globals.mapname=string_alloc("boundary-map");fixture_edicts[1].v.classname=string_alloc("player");
    fixture_edicts[1].v.viewmodel=string_alloc("models/v_original.mdl");fixture_edicts[1].v.weaponmodel=string_alloc("models/p_original.mdl");
    fixture_edicts[1].v.pContainingEntity=&fixture_edicts[1];sync_basic_globals();
    BhlEdict *g=(void*)(G_MEM+guest_edict(&fixture_edicts[1]));
    assert(!strcmp(gs(guest_strings+g->v.viewmodel),"models/v_original.mdl"));
    assert(!strcmp(gs(guest_strings+g->v.weaponmodel),"models/p_original.mdl"));
    uint32_t name=copy_string("models/v_changed.mdl");g->v.viewmodel=name-guest_strings;g->v.origin[2]=16;
    rt_w32(G_MEM,guest_globals+132,77);push_entities();push_globals();
    assert(!strcmp(string_value(fixture_edicts[1].v.viewmodel),"models/v_changed.mdl"));assert(fixture_edicts[1].v.origin[2]==16&&globals.trace_flags==77);
    sync_basic_globals();push_entities();push_globals();unsigned before=allocations;
    for(unsigned i=0;i<100;i++){sync_basic_globals();push_entities();push_globals();}
    assert(allocations==before);
    int original=fixture_edicts[1].v.classname;strcpy(fixture_strings+original,"crayer");
    sync_basic_globals();assert(!strcmp(gs(guest_strings+g->v.classname),"crayer"));
    save_roundtrip();
    puts("Server boundaries passed: string IDs, viewmodels, globals, allocation reuse");return 0;
}
