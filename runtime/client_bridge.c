/* Native client boundary. Missing adapters stop with their name; no guest
 * object or function pointer is cast to a host pointer. */
#include "loader.h"
#include "xash3d_types.h"
#include "q_client.h"
#include "cvardef.h"
#include "const.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "ref_params.h"
#include "cdll_int.h"
#include "event_args.h"
#include "pmove.h"
struct hull_s;
#include "pm_defs.h"
#include "triangleapi.h"
#include "r_efx.h"
#include "event_api.h"
#include "demo_api.h"
#include "net_api.h"
#include "ivoicetweak.h"
#include "screenfade.h"
#include "con_nprint.h"
#include <stdio.h>
#include <stdlib.h>
struct mstudioevent_s;
struct r_studio_interface_s;
struct engine_studio_api_s;
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define RET(v) do { c->eax=(uint32_t)(v);c->esp+=4;return; } while(0)
extern int bhl_heap_free(uint32_t);
static cl_enginefunc_t engine;
static uint32_t guest_engine;
static void client_missing(const char *name){fprintf(stderr,"BHL native client: unimplemented %s\n",name);abort();}
static char *gs(uint32_t p){return p?(char*)G_MEM+p:NULL;}
#include "guest_format.inc"
static uint32_t copy_bytes(const void *data,size_t size)
{
    if(!data)return 0;if(size>0x10000000)abort();uint32_t p=bhl_heap_alloc((uint32_t)size,0);if(!p)abort();memcpy(G_MEM+p,data,size);return p;
}
static uint32_t copy_string(const char *text){return text?copy_bytes(text,strlen(text)+1):0;}
static void sync_cvars(void);
static uint32_t invoke(const char *name,const uint32_t *args,unsigned count)
{
    sync_cvars();
    uint32_t fn=bhl_export("client.dll",name);if(!fn)client_missing(name);return bhl_invoke(fn,args,count,0);
}
static uint32_t command_guest[128],message_guest[128],event_guest[128];
static unsigned command_count,message_count,event_count;
static void run_command(unsigned slot){bhl_invoke(command_guest[slot],NULL,0,0);}
static int run_message(unsigned slot,const char *name,int size,void *data)
{
    if(size<0)abort();uint32_t n=copy_string(name),p=copy_bytes(data,size),args[]={n,(uint32_t)size,p};
    int result=bhl_invoke(message_guest[slot],args,3,0);bhl_heap_free(n);bhl_heap_free(p);return result;
}
static void run_event(unsigned slot,event_args_t *event)
{
    _Static_assert(sizeof(event_args_t)==72,"guest event layout");
    uint32_t p=copy_bytes(event,sizeof *event);bhl_invoke(event_guest[slot],&p,1,0);memcpy(event,G_MEM+p,sizeof *event);bhl_heap_free(p);
}
static uint32_t client_subtable(const char *,const void *);
static void client_dispatch(Ctx *,unsigned);
#include "client_engine_abi.h"
#include "client_subtables.h"
typedef struct CvarCopy { cvar_t *host;uint32_t guest,string;size_t capacity;struct CvarCopy *next; } CvarCopy;
static CvarCopy *cvars;
static uint32_t cvar_copy(cvar_t *h)
{
    if(!h)return 0;CvarCopy *p=cvars;while(p&&p->host!=h)p=p->next;
    if(!p){p=calloc(1,sizeof *p);if(!p)abort();p->host=h;p->guest=bhl_heap_alloc(20,1);rt_w32(G_MEM,p->guest,copy_string(h->name));p->next=cvars;cvars=p;}
    size_t size=strlen(h->string)+1;if(size>p->capacity){bhl_heap_free(p->string);p->capacity=size+32;p->string=bhl_heap_alloc(p->capacity,0);}
    memcpy(G_MEM+p->string,h->string,size);rt_w32(G_MEM,p->guest+4,p->string);rt_w32(G_MEM,p->guest+8,h->flags);rt_wf32(G_MEM,p->guest+12,h->value);
    return p->guest;
}
static void sync_cvars(void){for(CvarCopy *p=cvars;p;p=p->next)cvar_copy(p->host);}
/* The scalar entity prefix has the same layout; only its final pointers grow
   on ARM64. A stable guest allocation preserves entity pointer identity. */
typedef struct BhlClientEntity {
    int index,player;entity_state_t baseline,prevstate,curstate;
    int current_position;position_history_t ph[64];mouth_t mouth;latchedvars_t latched;
    float lastmove;vec3_t origin,angles,attachment[4];int trivial_accept;
    uint32_t model,efrag,topnode;float syncbase;int visframe;colorVec cvFloorColor;
} BhlClientEntity;
_Static_assert(sizeof(entity_state_t)==340,"entity state ABI");
_Static_assert(offsetof(BhlClientEntity,model)==2964,"guest client entity prefix");
_Static_assert(sizeof(BhlClientEntity)==3000,"guest client entity size");
typedef struct EntityCopy {cl_entity_t *host;uint32_t guest;struct EntityCopy *next;} EntityCopy;
static EntityCopy *entities;
typedef struct TextCopy {client_textmessage_t *host;uint32_t guest,name,message;struct TextCopy *next;} TextCopy;
static TextCopy *texts;
static uint32_t text_copy(client_textmessage_t *h)
{
    if(!h)return 0;TextCopy *p=texts;while(p&&p->host!=h)p=p->next;
    if(!p){p=calloc(1,sizeof *p);p->host=h;p->guest=bhl_heap_alloc(44,1);p->next=texts;texts=p;}
    memcpy(G_MEM+p->guest,h,36);
    if(!p->name||strcmp(gs(p->name),h->pName?h->pName:"")){bhl_heap_free(p->name);p->name=copy_string(h->pName?h->pName:"");}
    if(!p->message||strcmp(gs(p->message),h->pMessage?h->pMessage:"")){bhl_heap_free(p->message);p->message=copy_string(h->pMessage?h->pMessage:"");}
    rt_w32(G_MEM,p->guest+36,p->name);rt_w32(G_MEM,p->guest+40,p->message);return p->guest;
}
static uint32_t entity_copy(cl_entity_t *h)
{
    if(!h)return 0;EntityCopy *p=entities;while(p&&p->host!=h)p=p->next;
    if(!p){p=calloc(1,sizeof *p);p->host=h;p->guest=bhl_heap_alloc(sizeof(BhlClientEntity),1);p->next=entities;entities=p;}
    BhlClientEntity *g=(void*)(G_MEM+p->guest);memcpy(g,h,offsetof(BhlClientEntity,model));
    if(h->model||h->efrag||h->topnode)client_missing("client entity model/BSP pointers");
    g->model=g->efrag=g->topnode=0;g->syncbase=h->syncbase;g->visframe=h->visframe;g->cvFloorColor=h->cvFloorColor;return p->guest;
}
static void client_dispatch(Ctx *c,unsigned slot)
{
    if(getenv("BHL_BRIDGE_TRACE"))fprintf(stderr,"client engine %s\n",client_engine_names[slot]);
    switch(slot){
    case BHL_CL_Con_Printf:case BHL_CL_Con_DPrintf:{char text[8192];format_guest(text,gs(A(0)),c->esp+8);if(slot==BHL_CL_Con_Printf)engine.Con_Printf("%s",text);else engine.Con_DPrintf("%s",text);RET(0);}
    case BHL_CL_Con_NPrintf:{char text[8192];format_guest(text,gs(A(1)),c->esp+12);engine.Con_NPrintf(A(0),"%s",text);RET(0);}
    case BHL_CL_pfnRegisterVariable:RET(cvar_copy(engine.pfnRegisterVariable(gs(A(0)),gs(A(1)),A(2))));
    case BHL_CL_pfnGetCvarPointer:RET(cvar_copy(engine.pfnGetCvarPointer(gs(A(0)))));
    case BHL_CL_pfnAddCommand:{unsigned i=command_count++;if(i>=128)abort();command_guest[i]=A(1);RET(engine.pfnAddCommand(gs(A(0)),commands[i]));}
    case BHL_CL_pfnHookUserMsg:{unsigned i=message_count++;if(i>=128)abort();message_guest[i]=A(1);RET(engine.pfnHookUserMsg(gs(A(0)),messages[i]));}
    case BHL_CL_pfnHookEvent:{unsigned i=event_count++;if(i>=128)abort();event_guest[i]=A(1);engine.pfnHookEvent(gs(A(0)),events[i]);RET(0);}
    case BHL_CL_COM_ExpandFilename:RET(engine.COM_ExpandFilename(gs(A(0)),gs(A(1)),A(2)));
    case BHL_CL_CheckParm:{char *next=NULL;int result=engine.CheckParm(gs(A(0)),A(1)?&next:NULL);if(A(1))rt_w32(G_MEM,A(1),copy_string(next));RET(result);}
    case BHL_CL_COM_LoadFile:{
        int size=0;byte *data=engine.COM_LoadFile(gs(A(0)),A(1),&size);if(A(2))rt_w32(G_MEM,A(2),size);if(!data)RET(0);
        if(size<0)abort();uint32_t p=bhl_heap_alloc((uint32_t)size+1,1);if(!p)abort();memcpy(G_MEM+p,data,size);engine.COM_FreeFile(data);RET(p);
    }
    case BHL_CL_COM_FreeFile:if(!bhl_heap_free(A(0)))abort();RET(0);
    case BHL_CL_COM_ParseFile:{char *original=gs(A(0));char *end=engine.COM_ParseFile(original,gs(A(1)));if(end&&(end<original||end>original+strlen(original)))abort();RET(end?A(0)+(uint32_t)(end-original):0);}
    case BHL_CL_pfnGetScreenInfo:{_Static_assert(sizeof(SCREENINFO)==532,"screen info ABI");RET(engine.pfnGetScreenInfo((void*)(G_MEM+A(0))));}
    case BHL_CL_pfnSPR_GetList:{int count=0;client_sprite_t *list=engine.pfnSPR_GetList(gs(A(0)),&count);if(count<0||count>65536)abort();if(A(1))rt_w32(G_MEM,A(1),count);RET(copy_bytes(list,count*sizeof *list));}
    case BHL_CL_VGui_GetPanel:{void *panel=engine.VGui_GetPanel();if((uintptr_t)panel>UINT32_MAX)client_missing("native VGUI object cannot be used by the guest client");RET((uintptr_t)panel);}
    case BHL_CL_GetEntityByIndex:RET(entity_copy(engine.GetEntityByIndex(A(0))));
    case BHL_CL_GetLocalPlayer:RET(entity_copy(engine.GetLocalPlayer()));
    case BHL_CL_GetViewModel:RET(entity_copy(engine.GetViewModel()));
    case BHL_CL_pfnTextMessageGet:RET(text_copy(engine.pfnTextMessageGet(gs(A(0)))));
    case BHL_CL_pfnGetPlayerInfo:{
        hud_player_info_t h={0};engine.pfnGetPlayerInfo(A(0),&h);uint32_t p=A(1);memset(G_MEM+p,0,32);
        rt_w32(G_MEM,p,copy_string(h.name));rt_w16(G_MEM,p+4,h.ping);G_MEM[p+6]=h.thisplayer;G_MEM[p+7]=h.spectator;G_MEM[p+8]=h.packetloss;
        rt_w32(G_MEM,p+12,copy_string(h.model));rt_w16(G_MEM,p+16,h.topcolor);rt_w16(G_MEM,p+18,h.bottomcolor);rt_w64(G_MEM,p+24,h.m_nSteamID);RET(0);
    }
    case BHL_CL_pfnSetCrosshair:{wrect_t rect;memcpy(&rect,G_MEM+c->esp+8,sizeof rect);engine.pfnSetCrosshair(A(0),rect,A(5),A(6),A(7));RET(0);}
    case BHL_CL_Con_NXPrintf:{char text[8192];format_guest(text,gs(A(1)),c->esp+12);engine.Con_NXPrintf((con_nprint_t*)gs(A(0)),"%s",text);RET(0);}
    case BHL_CL_pfnGetScreenFade:{_Static_assert(sizeof(screenfade_t)==24,"fade ABI");engine.pfnGetScreenFade((screenfade_t*)gs(A(0)));RET(0);}
    case BHL_CL_pfnSetScreenFade:engine.pfnSetScreenFade((screenfade_t*)gs(A(0)));RET(0);
    case BHL_CL_pfnGetMousePos:engine.pfnGetMousePos((void*)gs(A(0)));RET(0);
#include "client_scalar_callbacks.h"
    default:client_missing(client_engine_names[slot]);
    }
}
EXPORT int Initialize(cl_enginefunc_t *functions,int version)
{
    if(!functions||version!=7)return 0;engine=*functions;
    bhl_initialize();bhl_attach("vgui.dll");bhl_attach("client.dll");
    guest_engine=bhl_heap_alloc(135*4,1);fill_client_engine(guest_engine);
    uint32_t args[]={guest_engine,(uint32_t)version};int result=invoke("Initialize",args,2);
    fprintf(stderr,"BHL v3 client Initialize returned %d; registered %u events\n",result,event_count);return result;
}
EXPORT void HUD_Init(void){invoke("HUD_Init",NULL,0);fprintf(stderr,"BHL v3 HUD_Init completed\n");}
EXPORT int HUD_VidInit(void){return invoke("HUD_VidInit",NULL,0);}
EXPORT void HUD_Shutdown(void){invoke("HUD_Shutdown",NULL,0);}
EXPORT void HUD_Reset(void){invoke("HUD_Reset",NULL,0);}
EXPORT void IN_ActivateMouse(void){invoke("IN_ActivateMouse",NULL,0);}
EXPORT void IN_DeactivateMouse(void){invoke("IN_DeactivateMouse",NULL,0);}
EXPORT void IN_ClearStates(void){invoke("IN_ClearStates",NULL,0);}
EXPORT void IN_Accumulate(void){invoke("IN_Accumulate",NULL,0);}
EXPORT void CAM_Think(void){invoke("CAM_Think",NULL,0);}
EXPORT int CL_IsThirdPerson(void){return invoke("CL_IsThirdPerson",NULL,0);}
EXPORT void IN_MouseEvent(int state){uint32_t a=state;invoke("IN_MouseEvent",&a,1);}
EXPORT void HUD_DrawNormalTriangles(void){invoke("HUD_DrawNormalTriangles",NULL,0);}
EXPORT void HUD_DrawTransparentTriangles(void){invoke("HUD_DrawTransparentTriangles",NULL,0);}
EXPORT void HUD_CreateEntities(void){invoke("HUD_CreateEntities",NULL,0);}
EXPORT void HUD_VoiceStatus(int index,qboolean talking){uint32_t a[]={index,talking};invoke("HUD_VoiceStatus",a,2);}
EXPORT int HUD_Key_Event(int event,int key,const char *binding)
{uint32_t p=copy_string(binding),a[]={event,key,p};int r=invoke("HUD_Key_Event",a,3);bhl_heap_free(p);return r;}
EXPORT char HUD_PlayerMoveTexture(char *name)
{uint32_t p=copy_string(name);char r=invoke("HUD_PlayerMoveTexture",&p,1);bhl_heap_free(p);return r;}
EXPORT void CL_CameraOffset(float *offset)
{uint32_t p=copy_bytes(offset,12);invoke("CL_CameraOffset",&p,1);memcpy(offset,G_MEM+p,12);bhl_heap_free(p);}
EXPORT void HUD_ChatInputPosition(int *x,int *y)
{uint32_t p=bhl_heap_alloc(8,1),a[]={p,p+4};rt_w32(G_MEM,p,*x);rt_w32(G_MEM,p+4,*y);invoke("HUD_ChatInputPosition",a,2);*x=rt_r32(G_MEM,p);*y=rt_r32(G_MEM,p+4);bhl_heap_free(p);}
EXPORT void HUD_DirectorMessage(int size,void *buffer)
{if(size<0)abort();uint32_t p=copy_bytes(buffer,size),a[]={size,p};invoke("HUD_DirectorMessage",a,2);bhl_heap_free(p);}
EXPORT void Demo_ReadBuffer(int size,byte *buffer)
{if(size<0)abort();uint32_t p=copy_bytes(buffer,size),a[]={size,p};invoke("Demo_ReadBuffer",a,2);bhl_heap_free(p);}
EXPORT int HUD_UpdateClientData(client_data_t *data,float time)
{_Static_assert(sizeof(client_data_t)==32,"client data ABI");uint32_t p=copy_bytes(data,sizeof *data),a[]={p,0};memcpy(a+1,&time,4);int r=invoke("HUD_UpdateClientData",a,2);memcpy(data,G_MEM+p,sizeof *data);bhl_heap_free(p);return r;}
EXPORT void CL_CreateMove(float time,usercmd_t *cmd,int active)
{_Static_assert(sizeof(usercmd_t)==52,"usercmd ABI");uint32_t p=copy_bytes(cmd,sizeof *cmd),a[]={0,p,active};memcpy(a,&time,4);invoke("CL_CreateMove",a,3);memcpy(cmd,G_MEM+p,sizeof *cmd);bhl_heap_free(p);}
EXPORT void HUD_TxferLocalOverrides(entity_state_t *state,const clientdata_t *data)
{uint32_t s=copy_bytes(state,sizeof *state),d=copy_bytes(data,sizeof *data),a[]={s,d};invoke("HUD_TxferLocalOverrides",a,2);memcpy(state,G_MEM+s,sizeof *state);bhl_heap_free(s);bhl_heap_free(d);}
EXPORT void HUD_ProcessPlayerState(entity_state_t *dst,const entity_state_t *src)
{uint32_t d=copy_bytes(dst,sizeof *dst),s=src==dst?d:copy_bytes(src,sizeof *src),a[]={d,s};invoke("HUD_ProcessPlayerState",a,2);memcpy(dst,G_MEM+d,sizeof *dst);bhl_heap_free(d);if(s!=d)bhl_heap_free(s);}
EXPORT void HUD_TxferPredictionData(entity_state_t *ps,const entity_state_t *pps,clientdata_t *pcd,const clientdata_t *ppcd,weapon_data_t *wd,const weapon_data_t *pwd)
{
    _Static_assert(sizeof(clientdata_t)==476,"prediction client data ABI");_Static_assert(sizeof(weapon_data_t)==88,"weapon data ABI");
    unsigned sizes[]={sizeof *ps,sizeof *pcd,64*sizeof *wd};void *dst[]={ps,pcd,wd};const void *src[]={pps,ppcd,pwd};uint32_t a[6];
    for(unsigned i=0;i<3;i++){a[2*i]=copy_bytes(dst[i],sizes[i]);a[2*i+1]=dst[i]==src[i]?a[2*i]:copy_bytes(src[i],sizes[i]);}
    invoke("HUD_TxferPredictionData",a,6);
    for(unsigned i=0;i<3;i++){memcpy(dst[i],G_MEM+a[2*i],sizes[i]);bhl_heap_free(a[2*i]);if(a[2*i+1]!=a[2*i])bhl_heap_free(a[2*i+1]);}
}
EXPORT void HUD_PostRunCmd(local_state_t *from,local_state_t *to,usercmd_t *cmd,int run,double time,unsigned seed)
{
    uint32_t f=copy_bytes(from,sizeof *from),t=from==to?f:copy_bytes(to,sizeof *to),p=copy_bytes(cmd,sizeof *cmd),a[]={f,t,p,run,0,0,seed};memcpy(a+4,&time,8);invoke("HUD_PostRunCmd",a,7);
    memcpy(from,G_MEM+f,sizeof *from);memcpy(to,G_MEM+t,sizeof *to);memcpy(cmd,G_MEM+p,sizeof *cmd);bhl_heap_free(f);if(f!=t)bhl_heap_free(t);bhl_heap_free(p);
}
EXPORT int HUD_Redraw(float time,int intermission){uint32_t args[2];memcpy(args,&time,4);args[1]=intermission;return invoke("HUD_Redraw",args,2);}
EXPORT void HUD_Frame(double time){uint32_t args[2];memcpy(args,&time,8);invoke("HUD_Frame",args,2);}
EXPORT void *KB_Find(const char *name)
{
    /* kbutton is three 32-bit integers and contains no pointers. The engine
       expects a live view so key-state changes must be shared, not copied. */
    uint32_t text=copy_string(name),p=invoke("KB_Find",&text,1);bhl_heap_free(text);
    return p?G_MEM+p:NULL;
}
EXPORT int HUD_GetHullBounds(int hull,float *mins,float *maxs)
{
    uint32_t p=bhl_heap_alloc(24,1),args[]={(uint32_t)hull,p,p+12};int result=invoke("HUD_GetHullBounds",args,3);
    memcpy(mins,G_MEM+p,12);memcpy(maxs,G_MEM+p+12,12);bhl_heap_free(p);return result;
}
static playermove_t *host_pmove;
static void pm_dispatch(Ctx *,unsigned);
#define BHL_PM_NAMESPACE "client-movement!"
#include "guest_movement.h"
static void pm_dispatch(Ctx *c,unsigned slot)
{
    switch(slot){
    case BHL_PM_COM_FileSize:RET(host_pmove->COM_FileSize(gs(A(0))));
    case BHL_PM_COM_LoadFile:{
        int size=0;byte *data=host_pmove->COM_LoadFile(gs(A(0)),A(1),&size);if(A(2))rt_w32(G_MEM,A(2),size);if(!data)RET(0);
        if(size<0)abort();uint32_t p=bhl_heap_alloc((uint32_t)size+1,1);if(!p)abort();memcpy(G_MEM+p,data,size);host_pmove->COM_FreeFile(data);RET(p);
    }
    case BHL_PM_COM_FreeFile:if(!bhl_heap_free(A(0)))abort();RET(0);
    case BHL_PM_memfgets:{
        uint32_t p=A(0),offset=A(2),buffer=A(3);int pos=(int)rt_r32(G_MEM,offset),size=(int)A(1),cap=(int)A(4);
        if(!p||!offset||!buffer||cap<=0||pos<0||pos>=size)RET(0);
        int n=0;while(pos<size&&n<cap-1){char ch=G_MEM[p+pos++];G_MEM[buffer+n++]=ch;if(ch=='\n')break;}
        G_MEM[buffer+n]=0;rt_w32(G_MEM,offset,pos);RET(buffer);
    }
    default:client_missing(pm_names[slot]);
    }
}
EXPORT void HUD_PlayerMoveInit(playermove_t *move)
{
    if(move->numphysent||move->nummoveent||move->numvisent)client_missing("populated PM_Init collision entities");
    host_pmove=move;uint32_t p=bhl_heap_alloc(sizeof(BhlPlayerMove),1);BhlPlayerMove *g=(void*)(G_MEM+p);
    pm_copy_values(g,move);pm_fill_callbacks(g);g->movevars=bhl_heap_alloc(sizeof(movevars_t),1);
    if(move->movevars)memcpy(G_MEM+g->movevars,move->movevars,sizeof(movevars_t));
    invoke("HUD_PlayerMoveInit",&p,1);fprintf(stderr,"BHL v3 client movement initialization completed\n");
}
#include "client_missing_exports.h"
