#include "../runtime/server_bridge.c"
#include <assert.h>
static edict_t fixture_pool[16],*fixture_edicts=fixture_pool;
static char fixture_strings[65536];static unsigned fixture_used=1,allocations;
static const char *string_value(int id){assert(id>=0&&(unsigned)id<fixture_used);return fixture_strings+id;}
static int string_alloc(const char *text)
{int p=fixture_used;size_t size=strlen(text)+1;assert(size+fixture_used<sizeof fixture_strings);memcpy(fixture_strings+p,text,size);fixture_used+=size;allocations++;return p;}
static edict_t *entity_at(int index){assert(index>=0&&index<16);return fixture_edicts+index;}
static edict_t *find_sphere(edict_t *start,const float *origin,float radius)
{assert(!start&&origin[0]==5&&radius==12);assert(fixture_edicts[1].v.absmin[0]==-9&&fixture_edicts[1].v.absmax[0]==19);assert(fixture_edicts[1].v.health==0);return fixture_edicts+1;}
static int fixture_visible=1,fixture_group;
static int packet_visibility(const edict_t *entity,byte *set){assert(entity==fixture_edicts+2);return fixture_visible;}
static void packet_group(int mask,int operation){assert(operation==0);fixture_group=mask;}
static int packet_index(const edict_t *entity){return entity-fixture_edicts;}
static int packet_model(const char *name){return *name?17:0;}
static void *fixture_model_data;
static void *get_model(edict_t *entity)
{assert(entity==fixture_edicts+1&&entity->v.modelindex==17&&entity->v.health==0);return fixture_model_data;}
static void collision_equivalence(void)
{
    edict_t *h=fixture_edicts+1;entvars_t initial=h->v;
    uint32_t arg=guest_edict(h),object=bhl_heap_alloc(16,1),table=bhl_heap_alloc(64,1);
    rt_w32(G_MEM,object,table);rt_w32(G_MEM,object+4,arg+offsetof(BhlEdict,v));rt_w32(G_MEM,table+0x30,0x10045bb0);
    for(unsigned scenario=0;scenario<4;scenario++){
        initial.solid=scenario&1?SOLID_BSP:SOLID_BBOX;initial.angles[1]=scenario&1?45:0;
        initial.origin[0]=20;initial.origin[2]=100;
        for(unsigned j=0;j<3;j++){initial.mins[j]=-8-(int)j;initial.maxs[j]=12+j;}
        h->pvPrivateData=scenario&2?(void*)1:NULL;private_data[1]=scenario&2?object:0;
        h->v=initial;server_call_full(BHL_DLL_pfnSetAbsBox,&arg,1);entvars_t expected=h->v;
        h->v=initial;server_call(BHL_DLL_pfnSetAbsBox,&arg,1);
        assert(!memcmp(&h->v,&expected,sizeof expected));assert(h->v.absmin[0]<h->v.origin[0]&&h->v.absmax[2]>h->v.origin[2]);
    }
    h->pvPrivateData=NULL;private_data[1]=0;bhl_heap_free(object);bhl_heap_free(table);
    puts("Original collision callback equivalence passed: base object, no object, rotated BSP, bounding box");
}
static void message_begin(int dest,int type,const float *origin,edict_t *recipient)
{assert(dest==MSG_ONE&&type==99&&!origin&&recipient==fixture_edicts+1);assert(recipient->v.health==10);}
static void message_end(void){assert(fixture_edicts[1].v.health==42&&host_globals->trace_flags==83);}
static void make_vectors(const float *angles)
{assert(angles[0]==17);for(unsigned i=0;i<3;i++){host_globals->v_forward[i]=i+1;host_globals->v_up[i]=i+7;host_globals->v_right[i]=i+4;}}
static void message_boundaries(void)
{
    engine.pfnMessageBegin=message_begin;engine.pfnMessageEnd=message_end;engine.pfnMakeVectors=make_vectors;
    fixture_edicts[1].v.health=10;sync_basic_globals();
    BhlEdict *g=(void*)(G_MEM+guest_edict(fixture_edicts+1));g->v.health=42;rt_w32(G_MEM,guest_globals+132,83);
    Ctx c={0};uint32_t sp=bhl_heap_alloc(32,1);c.esp=sp;
    rt_w32(G_MEM,sp+4,MSG_ONE);rt_w32(G_MEM,sp+8,99);rt_w32(G_MEM,sp+12,0);rt_w32(G_MEM,sp+16,guest_edict(fixture_edicts+1));
    engine_dispatch(&c,BHL_ENG_pfnMessageBegin);assert(g->v.health==42&&fixture_edicts[1].v.health==10);
    c.esp=sp;engine_dispatch(&c,BHL_ENG_pfnMessageEnd);assert(g->v.health==42&&fixture_edicts[1].v.health==42);
    g->v.health=53;uint32_t angles=bhl_heap_alloc(12,1);rt_wf32(G_MEM,angles,17);c.esp=sp;rt_w32(G_MEM,sp+4,angles);
    engine_dispatch(&c,BHL_ENG_pfnMakeVectors);
    assert(g->v.health==53&&fixture_edicts[1].v.health==42);
    assert(!memcmp(G_MEM+guest_globals+offsetof(globalvars_t,v_forward),host_globals->v_forward,3*sizeof(vec3_t)));
    bhl_heap_free(sp);bhl_heap_free(angles);push_entities();
    puts("Message and vector boundaries passed: pending writes survive and routing receives current state");
}
static void packet_snapshot_equivalence(void)
{
    engine.pfnCheckVisibility=packet_visibility;engine.pfnSetGroupMask=packet_group;
    engine.pfnIndexOfEdict=packet_index;engine.pfnModelIndex=packet_model;
    edict_t *entity=fixture_edicts+2,*host=fixture_edicts+1;
    entity->free=0;entity->v.pContainingEntity=entity;entity->v.model=string_alloc("models/fixture.mdl");entity->v.modelindex=7;
    entity->v.origin[1]=24;entity->v.angles[2]=17;entity->v.frame=3.25f;entity->v.owner=host;
    for(unsigned scenario=0;scenario<5;scenario++){
        fixture_visible=scenario!=1;entity->v.effects=scenario==2?EF_NODRAW:0;
        entity->v.groupinfo=host->v.groupinfo=scenario==3?4:0;
        entity_state_t full={0},packet={0};packet_snapshot_valid=0;
        int expected=add_full_pack(&full,2,entity,host,0,scenario==4,NULL);
        sync_basic_globals();packet_snapshot_valid=1;
        int actual=add_full_pack(&packet,2,entity,host,0,scenario==4,NULL);
        assert(actual==expected&&!memcmp(&full,&packet,sizeof full)&&fixture_group==0);
        assert(actual==(scenario!=1&&scenario!=2));
        if(actual)assert(packet.modelindex==7&&packet.origin[1]==24&&packet.frame==3.25f);
    }
    packet_snapshot_valid=0;entity->free=1;host->v.groupinfo=0;
    puts("Original packet callback equivalence passed: visible, culled, hidden, grouped and player states");
}
static void save_roundtrip(void)
{
    typedef struct {char title[13];int string_id;edict_t *target;float position[3],time;int counts[3];short number;unsigned untouched;} Record;
    TYPEDESCRIPTION fields[]={
        DEFINE_ARRAY(Record,title,FIELD_CHARACTER,13),DEFINE_GLOBAL_FIELD(Record,string_id,FIELD_STRING),
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
    /* Xash's global-entity transition probe passes an uninitialized stack
       destination. ReadFields must not dereference its old string IDs or
       pointers, and fields outside the supplied schema must stay untouched. */
    Record target;memset(&target,0xa5,sizeof target);s->pCurrentData=s->pBaseData;s->size=0;s->time=200;s->vecLandmarkOffset[0]=20;
    api.pfnSaveReadFields(s,"fixture-record",&target,fields,sizeof fields/sizeof *fields);
    assert(s->size==written&&!strcmp(source.title,target.title));assert(!strcmp(string_value(target.string_id),"retained-string"));
    assert(target.target==fixture_edicts+1&&target.position[0]==37&&target.position[1]==3&&target.position[2]==0&&target.time==225);
    assert(target.untouched==0xa5a5a5a5u);
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
static void tank_sprite_lifetime(void)
{
    uint32_t sprite=bhl_heap_alloc(0x260,1),other=bhl_heap_alloc(0x260,1);
    rt_w32(G_MEM,sprite,0x10200598);rt_w32(G_MEM,other,0x10200598);
    const uint32_t tables[]={0x102028ac,0x1020268c,0x1020279c,0x10202474,0x10200598};
    for(unsigned i=0;i<5;i++){
        private_data[i]=bhl_heap_alloc(0x2c4,1);fixture_edicts[i].pvPrivateData=(void*)1;fixture_edicts[i].free=0;
        rt_w32(G_MEM,private_data[i],tables[i]);rt_w32(G_MEM,private_data[i]+0x22c,sprite);
    }
    /* Live/unrelated sprites and unrelated classes retain their references. */
    v3_retire_sprite(other);for(unsigned i=0;i<5;i++)assert(rt_r32(G_MEM,private_data[i]+0x22c)==sprite);
    v3_retire_sprite(sprite);
    for(unsigned i=0;i<5;i++)assert(rt_r32(G_MEM,private_data[i]+0x22c)==(i==4?sprite:0));
    bhl_heap_free(sprite);uint32_t reused=bhl_heap_alloc(0x260,1);assert(reused==sprite);
    for(unsigned i=0;i<4;i++)assert(!rt_r32(G_MEM,private_data[i]+0x22c));
    for(unsigned i=0;i<5;i++){bhl_heap_free(private_data[i]);private_data[i]=0;fixture_edicts[i].pvPrivateData=NULL;}
    bhl_heap_free(reused);bhl_heap_free(other);
    puts("Mounted-weapon sprite lifetime passed: expired references clear before allocation reuse");
}
extern float watched_float(uint32_t);
extern void watched_write(uint32_t,float);
extern void watched_move(uint32_t,uint32_t,size_t);
static void server_demand_boundaries(void)
{
    uint32_t identity=guest_edict(fixture_edicts+1);
    uint32_t health=identity+offsetof(BhlEdict,v)+offsetof(BhlEntvars,health);
    fixture_edicts[1].v.health=25;server_demand_enabled=1;pull_entities();
    assert(*(float*)gs_read(health,4)==25&&server_access_epoch&&server_demand_enabled);
    assert(watched_float(health)==25);watched_write(health,35);
    push_entities();assert(fixture_edicts[1].v.health==35);server_watch_suspend();
    /* The engine updates host state between calls. The cached guest pointer
       observes the new value without querying entity identity again. */
    fixture_edicts[1].v.health=45;pull_entities();assert(watched_float(health)==45);
    watched_write(health,55);push_entities();server_watch_suspend();
    assert(fixture_edicts[1].v.health==55);
    fixture_edicts[1].v.health=65;pull_entities();watched_write(health,75);
    assert(gs(health)==(char*)G_MEM+health&&!server_access_epoch&&!server_demand_enabled);
    assert(rt_rf32(G_MEM,health)==75);push_entities();assert(fixture_edicts[1].v.health==75);
    /* Imported native services materialize all outstanding entities, retaining
       edits already made through translated memory writes. */
    server_demand_enabled=1;pull_entities();watched_write(health,85);
    rt_watch_materialize();assert(!server_access_epoch&&rt_rf32(G_MEM,health)==85);
    push_entities();assert(fixture_edicts[1].v.health==85);
    server_demand_enabled=0;pull_entities();
    puts("Demand server access passed: cached entvars, first writes, native handoff and pending writes");
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
    BhlEdict *unused=(void*)(G_MEM+guest_edict(fixture_edicts+7));
    fixture_edicts[7].serialnumber=41;fixture_edicts[7].leafnums16[3]=93;pull_entities();
    assert(unused->serialnumber==41&&unused->leafnums[3]==93);
    unused->serialnumber=7;unused->leafnums[3]=18;pull_entities();
    assert(unused->serialnumber==41&&unused->leafnums[3]==93);
    int original=fixture_edicts[1].v.classname;strcpy(fixture_strings+original,"crayer");
    sync_basic_globals();assert(!strcmp(gs(guest_strings+g->v.classname),"crayer"));
    server_demand_boundaries();
    save_roundtrip();
    collision_equivalence();
    message_boundaries();
    packet_snapshot_equivalence();
    edict_t replacement[16]={0};uint32_t identity=guest_edict(fixture_edicts+1);
    fixture_edicts=replacement;globals.maxEntities=16;fixture_edicts[1].v.origin[2]=321;
    sync_basic_globals();assert(guest_edict(fixture_edicts+1)==identity&&host_edict(identity)==fixture_edicts+1&&g->v.origin[2]==321);
    unsigned char model[256]={0};uint32_t magic=0x54534449,version=10,length=sizeof model;
    memcpy(model,&magic,4);memcpy(model+4,&version,4);memcpy(model+72,&length,4);model[244]=17;
    uint32_t mg=model_copy(model);assert(G_MEM[mg+244]==17);model[244]=29;model_invalidate();
    assert(model_copy(model)==mg&&G_MEM[mg+244]==29);
    engine.pfnFindEntityInSphere=find_sphere;g->v.absmin[0]=-9;g->v.absmax[0]=19;g->v.health=123;
    Ctx query={0};query.esp=bhl_heap_alloc(32,1);uint32_t sp=query.esp,origin=bhl_heap_alloc(12,1);rt_wf32(G_MEM,origin,5);
    rt_w32(G_MEM,sp+4,identity);engine_dispatch(&query,BHL_ENG_pfnEntOffsetOfPEntity);
    assert(query.eax==sizeof(BhlEdict)&&g->v.health==123&&fixture_edicts[1].v.health==0);
    query.esp=sp;rt_w32(G_MEM,sp+4,0);rt_w32(G_MEM,sp+8,origin);rt_wf32(G_MEM,sp+12,12);
    engine_dispatch(&query,BHL_ENG_pfnFindEntityInSphere);
    assert(query.eax==identity&&g->v.health==123&&fixture_edicts[1].v.health==0);
    fixture_model_data=model;engine.pfnGetModelPtr=get_model;g->v.modelindex=17;
    query.esp=sp;rt_w32(G_MEM,sp+4,identity);engine_dispatch(&query,BHL_ENG_pfnGetModelPtr);
    assert(query.eax==mg&&g->v.health==123&&fixture_edicts[1].v.health==0&&fixture_edicts[1].v.modelindex==17);
    tank_sprite_lifetime();
    puts("Server boundaries passed: string IDs, viewmodels, globals, allocation reuse");return 0;
}
