#include "loader.h"
#include "vgui_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
extern void InitVGUISupportAPI(vguiapi_t *);
static int quads,textures,ink,starts,stops,screen_ready,offsets,last_offset[2];
static void noop(void){}
static void start(void){starts++;} static void stop(void){stops++;}
static void color(int *c){}static void one(int x){}
static void offset(int x,int y){assert(screen_ready);offsets++;last_offset[0]=x;last_offset[1]=y;}
static int ingame(void){return 1;}static int generate(void){return ++textures;}
static void cursor(VGUI_DefaultCursor c){}static void getpos(int *x,int *y){*x=30;*y=40;}
static void quad(const vpoint_t *a,const vpoint_t *b){assert(a->point[0]>=0&&a->point[1]>=0&&b->point[0]<=1024&&b->point[1]<=768);quads++;}
static void upload(int id,const char *pixels,int w,int h){for(int i=0;i<w*h;i++)if((unsigned char)pixels[i*4+3]>0)ink++;}
static uint32_t call(uint32_t o,unsigned slot,uint32_t *a,unsigned n){return bhl_thiscall(rt_r32(G_MEM,rt_r32(G_MEM,o)+slot*4),o,a,n);}
int main(void){
 vguiapi_t api={0};api.DrawInit=start;api.DrawShutdown=stop;api.SetupDrawingText=color;api.SetupDrawingRect=color;api.SetupDrawingImage=color;api.BindTexture=one;api.EnableTexture=one;api.UploadTexture=upload;api.DrawQuad=quad;api.GenerateTexture=generate;api.CursorSelect=cursor;api.IsInGame=ingame;api.GetCursorPos=getpos;api.SetPaintOffset=offset;
 InitVGUISupportAPI(&api);api.Startup(800,600);api.Startup(1024,768);uint32_t root=(uint32_t)(uintptr_t)api.GetPanel();assert(root);assert(offsets==0);screen_ready=1;
 uint32_t str=bhl_heap_alloc(32,1);memcpy(G_MEM+str,"Native VGUI abc 123",19);uint32_t label=bhl_heap_alloc(208,1);
 bhl_thiscall(bhl_export("vgui.dll","??0Label@vgui@@QAE@PBD@Z"),label,&str,1);
 call(label,16,&root,1);call(label,4,(uint32_t[]){10,20,220,80},4);api.MouseMove(30,40);api.Paint();
 printf("VGUI quads=%d textures=%d ink=%d\n",quads,textures,ink);assert(quads>0&&textures>0&&ink>0);assert(offsets>0&&last_offset[0]==0&&last_offset[1]==0);
 api.Shutdown();assert(starts==1&&stops==1);screen_ready=0;api.Startup(640,480);screen_ready=1;api.Paint();api.Shutdown();assert(starts==2&&stops==2);puts("VGUI lifecycle passed");return 0;
}
