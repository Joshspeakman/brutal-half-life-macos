/* The original client computes animation and bone transforms. Engine drawing
 * consumes explicitly synchronized matrices and converted model identities. */
static engine_studio_api_t studio;
static uint32_t guest_studio,guest_studio_interface;
typedef struct StudioBuffer {void *host;uint32_t guest,size;struct StudioBuffer *next;} StudioBuffer;
static StudioBuffer *studio_buffers;
typedef struct StudioPlayer {player_info_t *host;uint32_t guest;struct StudioPlayer *next;} StudioPlayer;
static StudioPlayer *studio_players;
typedef struct StudioCache {uint32_t guest;cache_user_t host;struct StudioCache *next;} StudioCache;
static StudioCache *studio_caches;
static void studio_dispatch(Ctx *,unsigned);
#include "studio_abi.h"
static uint32_t studio_buffer(void *host,uint32_t size)
{
    if(!host)return 0;StudioBuffer *p=studio_buffers;while(p&&(p->host!=host||p->size!=size))p=p->next;
    if(!p){p=calloc(1,sizeof *p);if(!p)abort();p->host=host;p->size=size;p->guest=bhl_heap_alloc(size,1);p->next=studio_buffers;studio_buffers=p;}
    memcpy(G_MEM+p->guest,host,size);return p->guest;
}
static uint32_t studio_model_pointer(const void *host)
{
    if(!host)return 0;
    for(ModelData *p=model_data;p;p=p->next)if(p->size&&(uintptr_t)host>=(uintptr_t)p->host&&(uintptr_t)host-(uintptr_t)p->host<p->size)
        return p->guest+((uintptr_t)host-(uintptr_t)p->host);
    client_missing("studio pointer outside a mirrored model");return 0;
}
static void studio_player_pull(StudioPlayer *p)
{
    player_info_t *h=p->host;uint32_t g=p->guest;memcpy(G_MEM+g,h,404);
    customization_t *c=&h->customdata;uint32_t custom=g+404;
    rt_w32(G_MEM,custom,c->bInUse);memcpy(G_MEM+custom+4,&c->resource,128);
    rt_w32(G_MEM,custom+132,model_opaque(c->resource.pNext,"resource-next"));rt_w32(G_MEM,custom+136,model_opaque(c->resource.pPrev,"resource-prev"));
    rt_w32(G_MEM,custom+140,c->bTranslated);rt_w32(G_MEM,custom+144,c->nUserData1);rt_w32(G_MEM,custom+148,c->nUserData2);
    rt_w32(G_MEM,custom+152,model_opaque(c->pInfo,"custom-info"));rt_w32(G_MEM,custom+156,model_opaque(c->pBuffer,"custom-buffer"));rt_w32(G_MEM,custom+160,model_opaque(c->pNext,"custom-next"));
    memcpy(G_MEM+g+568,h->hashedcdkey,16);
}
static uint32_t studio_player(player_info_t *host)
{
    if(!host)return 0;StudioPlayer *p=studio_players;while(p&&p->host!=host)p=p->next;
    if(!p){p=calloc(1,sizeof *p);if(!p)abort();p->host=host;p->guest=bhl_heap_alloc(584,1);p->next=studio_players;studio_players=p;}
    studio_player_pull(p);return p->guest;
}
static void studio_pull(void)
{
    for(StudioBuffer *p=studio_buffers;p;p=p->next)memcpy(G_MEM+p->guest,p->host,p->size);
    for(StudioPlayer *p=studio_players;p;p=p->next)studio_player_pull(p);
}
static void studio_push(void)
{
    for(StudioBuffer *p=studio_buffers;p;p=p->next)memcpy(p->host,G_MEM+p->guest,p->size);
    for(StudioPlayer *p=studio_players;p;p=p->next)memcpy(p->host,G_MEM+p->guest,404);
}
static StudioCache *studio_cache(uint32_t guest)
{
    StudioCache *p=studio_caches;while(p&&p->guest!=guest)p=p->next;
    if(!p){p=calloc(1,sizeof *p);if(!p)abort();p->guest=guest;p->next=studio_caches;studio_caches=p;}
    return p;
}
static void studio_dispatch(Ctx *c,unsigned slot)
{
    entities_push();if(getenv("BHL_BRIDGE_TRACE"))fprintf(stderr,"studio %s\n",studio_names[slot]);
    switch(slot){
    case BHL_STUDIO_Mem_Calloc:{uint64_t size=(uint64_t)A(0)*A(1);if(size>UINT32_MAX)RET(0);RET(bhl_heap_alloc(size,1));}
    case BHL_STUDIO_Cache_Check:{StudioCache *p=studio_cache(A(0));void *h=studio.Cache_Check(&p->host);if(!h){rt_w32(G_MEM,A(0),0);RET(0);}RET(rt_r32(G_MEM,A(0)));}
    case BHL_STUDIO_LoadCacheFile:{
        StudioCache *p=studio_cache(A(1));studio.LoadCacheFile(gs(A(0)),&p->host);void *h=p->host.data;
        if(!h){rt_w32(G_MEM,A(1),0);RET(0);}uint32_t magic,size;memcpy(&magic,h,4);memcpy(&size,(char*)h+72,4);
        if((magic!=0x51534449&&magic!=0x54534449)||size<76||size>32*1024*1024)client_missing("studio cache file format");
        rt_w32(G_MEM,A(1),model_bytes(h,size));RET(0);
    }
    case BHL_STUDIO_Mod_ForName:RET(model_guest(studio.Mod_ForName(gs(A(0)),A(1))));
    case BHL_STUDIO_Mod_Extradata:RET(studio_data(studio.Mod_Extradata(model_host(A(0)))));
    case BHL_STUDIO_GetModelByIndex:RET(model_guest(studio.GetModelByIndex(A(0))));
    case BHL_STUDIO_GetCurrentEntity:RET(entity_copy(studio.GetCurrentEntity()));
    case BHL_STUDIO_PlayerInfo:RET(studio_player(studio.PlayerInfo(A(0))));
    case BHL_STUDIO_GetPlayerState:{
        entity_state_t *h=studio.GetPlayerState(A(0));
        for(EntityCopy *p=entities;p;p=p->next)if(h==&p->host->curstate)RET(p->guest+offsetof(BhlClientEntity,curstate));
        RET(studio_buffer(h,sizeof *h));
    }
    case BHL_STUDIO_GetViewEntity:RET(entity_copy(studio.GetViewEntity()));
    case BHL_STUDIO_GetTimes:studio.GetTimes((int*)gs(A(0)),(double*)gs(A(1)),(double*)gs(A(2)));RET(0);
    case BHL_STUDIO_GetCvar:RET(cvar_copy(studio.GetCvar(gs(A(0)))));
    case BHL_STUDIO_GetViewInfo:studio.GetViewInfo((float*)gs(A(0)),(float*)gs(A(1)),(float*)gs(A(2)),(float*)gs(A(3)));RET(0);
    case BHL_STUDIO_GetChromeSprite:RET(model_guest(studio.GetChromeSprite()));
    case BHL_STUDIO_GetModelCounters:{int *s=NULL,*a=NULL;studio.GetModelCounters(&s,&a);if(A(0))rt_w32(G_MEM,A(0),studio_buffer(s,4));if(A(1))rt_w32(G_MEM,A(1),studio_buffer(a,4));RET(0);}
    case BHL_STUDIO_GetAliasScale:studio.GetAliasScale((float*)gs(A(0)),(float*)gs(A(1)));RET(0);
    /* Despite the historical pointer-star spelling these APIs return flat
       float[128][3][4] and float[3][4] arrays, not pointer graphs. */
    case BHL_STUDIO_StudioGetBoneTransform:RET(studio_buffer(studio.StudioGetBoneTransform(),128*3*4*4));
    case BHL_STUDIO_StudioGetLightTransform:RET(studio_buffer(studio.StudioGetLightTransform(),128*3*4*4));
    case BHL_STUDIO_StudioGetAliasTransform:RET(studio_buffer(studio.StudioGetAliasTransform(),3*4*4));
    case BHL_STUDIO_StudioGetRotationMatrix:RET(studio_buffer(studio.StudioGetRotationMatrix(),3*4*4));
    case BHL_STUDIO_StudioSetupModel:{void *body=NULL,*sub=NULL;studio.StudioSetupModel(A(0),&body,&sub);if(A(1))rt_w32(G_MEM,A(1),studio_model_pointer(body));if(A(2))rt_w32(G_MEM,A(2),studio_model_pointer(sub));RET(0);}
    case BHL_STUDIO_StudioCheckBBox:RET(studio.StudioCheckBBox());
    case BHL_STUDIO_StudioDynamicLight:case BHL_STUDIO_StudioEntityLight:case BHL_STUDIO_StudioSetupLighting:{
        uint32_t p=slot==BHL_STUDIO_StudioDynamicLight?A(1):A(0);alight_t light={0};memcpy(&light,G_MEM+p,20);light.plightvec=(float*)gs(rt_r32(G_MEM,p+20));
        if(slot==BHL_STUDIO_StudioDynamicLight)studio.StudioDynamicLight(entity_host(A(0)),&light);
        else if(slot==BHL_STUDIO_StudioEntityLight)studio.StudioEntityLight(&light);else studio.StudioSetupLighting(&light);
        memcpy(G_MEM+p,&light,20);
        if(light.plightvec!=((float*)gs(rt_r32(G_MEM,p+20))))rt_w32(G_MEM,p+20,studio_buffer(light.plightvec,12));RET(0);
    }
    case BHL_STUDIO_StudioDrawPoints:studio.StudioDrawPoints();RET(0);
    case BHL_STUDIO_StudioDrawHulls:studio.StudioDrawHulls();RET(0);
    case BHL_STUDIO_StudioDrawAbsBBox:studio.StudioDrawAbsBBox();RET(0);
    case BHL_STUDIO_StudioDrawBones:studio.StudioDrawBones();RET(0);
    case BHL_STUDIO_StudioSetupSkin:studio.StudioSetupSkin((void*)model_host_data(A(0)),A(1));RET(0);
    case BHL_STUDIO_StudioSetRemapColors:studio.StudioSetRemapColors(A(0),A(1));RET(0);
    case BHL_STUDIO_SetupPlayerModel:RET(model_guest(studio.SetupPlayerModel(A(0))));
    case BHL_STUDIO_StudioClientEvents:studio.StudioClientEvents();RET(0);
    case BHL_STUDIO_GetForceFaceFlags:RET(studio.GetForceFaceFlags());
    case BHL_STUDIO_SetForceFaceFlags:studio.SetForceFaceFlags(A(0));RET(0);
    case BHL_STUDIO_StudioSetHeader:studio.StudioSetHeader((void*)model_host_data(A(0)));RET(0);
    case BHL_STUDIO_SetRenderModel:studio.SetRenderModel(model_host(A(0)));RET(0);
    case BHL_STUDIO_SetupRenderer:studio.SetupRenderer(A(0));RET(0);
    case BHL_STUDIO_RestoreRenderer:studio.RestoreRenderer();RET(0);
    case BHL_STUDIO_SetChromeOrigin:studio.SetChromeOrigin();RET(0);
    case BHL_STUDIO_IsHardware:RET(studio.IsHardware());
    case BHL_STUDIO_GL_StudioDrawShadow:studio.GL_StudioDrawShadow();RET(0);
    case BHL_STUDIO_GL_SetRenderMode:studio.GL_SetRenderMode(A(0));RET(0);
    case BHL_STUDIO_StudioSetRenderamt:studio.StudioSetRenderamt(A(0));RET(0);
    case BHL_STUDIO_StudioSetCullState:studio.StudioSetCullState(A(0));RET(0);
    case BHL_STUDIO_StudioRenderShadow:studio.StudioRenderShadow(A(0),(float*)gs(A(1)),(float*)gs(A(2)),(float*)gs(A(3)),(float*)gs(A(4)));RET(0);
    default:client_missing(studio_names[slot]);
    }
}
static int studio_draw_model(int flags)
{uint32_t a=flags;return invoke_address(rt_r32(G_MEM,guest_studio_interface+4),&a,1);}
static int studio_draw_player(int flags,entity_state_t *state)
{
    uint32_t p=copy_bytes(state,sizeof *state),a[]={flags,p};int r=invoke_address(rt_r32(G_MEM,guest_studio_interface+8),a,2);
    memcpy(state,G_MEM+p,sizeof *state);bhl_heap_free(p);return r;
}
static int studio_initialize(int version,r_studio_interface_t **out,engine_studio_api_t *api)
{
    if(version!=1||!out||!api)return 0;studio=*api;
    if(!guest_studio)guest_studio=bhl_heap_alloc(46*4,1);studio_fill(guest_studio);
    uint32_t p=bhl_heap_alloc(4,1),a[]={version,p,guest_studio};int result=invoke("HUD_GetStudioModelInterface",a,3);
    if(result){guest_studio_interface=rt_r32(G_MEM,p);if(!guest_studio_interface||rt_r32(G_MEM,guest_studio_interface)!=1)client_missing("studio interface version");
        static r_studio_interface_t native={1,studio_draw_model,studio_draw_player};*out=&native;
        fprintf(stderr,"BHL v3 original studio renderer initialized\n");}
    bhl_heap_free(p);return result;
}
