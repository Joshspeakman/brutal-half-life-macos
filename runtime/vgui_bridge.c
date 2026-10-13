/* VGUI 1 objects retain their MSVC x86 layout and original implementation.
 * Only the engine-owned surface and OS-facing App methods cross to native C.
 * Slot numbers are the pinned SDK's Windows ABI (overloads are not in header order). */
#include "loader.h"
#include "vgui_api.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define RET(v,n) do { c->eax=(uint32_t)(v);c->esp+=4+4*(n);return; } while(0)
extern int bhl_heap_free(uint32_t);
static vguiapi_t *api;
static uint32_t app,root,surface,app_table,surface_table,cursor;
static int screen[2],draw_color[4],text_color[4],text_pos[2],translation[2],clip[4],paint_depth;
typedef struct Paint { uint32_t panel;int translation[2],clip[4]; } Paint;
static Paint paint_stack[64];
typedef struct Font { uint32_t guest;int tall,abc[256][3],texture[256],size[256][2];struct Font *next; } Font;
static Font *fonts,*current_font;
static void fail(const char *name){fprintf(stderr,"BHL native VGUI: %s\n",name);abort();}
static uint32_t exported(const char *name){uint32_t fn=bhl_export("vgui.dll",name);if(!fn)fail(name);return fn;}
static uint32_t method(uint32_t object,const char *name,const uint32_t *args,unsigned count){return bhl_thiscall(exported(name),object,args,count);}
static uint32_t virtual_call(uint32_t object,unsigned slot,const uint32_t *args,unsigned count)
{ if(!object)fail("null object");return bhl_thiscall(rt_r32(G_MEM,rt_r32(G_MEM,object)+4*slot),object,args,count); }
#define VC(object,slot,...) virtual_call(object,slot,(uint32_t[]){__VA_ARGS__},sizeof((uint32_t[]){__VA_ARGS__})/4)
static uint32_t copy_vtable(uint32_t object,unsigned slots)
{ uint32_t p=bhl_heap_alloc(slots*4,0);memcpy(G_MEM+p,G_MEM+rt_r32(G_MEM,object),slots*4);rt_w32(G_MEM,object,p);return p; }
static void override(uint32_t table,unsigned slot,const char *name,GuestFn callback){rt_w32(G_MEM,table+4*slot,bhl_thunk(name,callback));}
static void app_main(Ctx *c){/* The engine owns the event loop. */RET(0,2);}
static void app_tick(Ctx *c){/* Input is delivered by the engine callbacks below. */RET(0,0);}
static void app_time(Ctx *c){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);RET((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000,0);}
static void app_cursor_get(Ctx *c){int x,y;api->GetCursorPos(&x,&y);rt_w32(G_MEM,A(0),x);rt_w32(G_MEM,A(1),y);RET(0,2);}
static void app_cursor_set(Ctx *c){SDL_Window *w=SDL_GetMouseFocus();if(w)SDL_WarpMouseInWindow(w,A(0),A(1));RET(0,2);}
static void app_clipboard_count(Ctx *c){char *s=SDL_GetClipboardText();size_t n=strlen(s)+1;SDL_free(s);RET(n,0);}
static void app_clipboard_set(Ctx *c){int n=A(1);if(n<0||n>1048576)fail("invalid clipboard length");char *s=malloc(n+1);memcpy(s,G_MEM+A(0),n);s[n]=0;api->SetClipboardText(s);free(s);RET(0,2);}
static void app_clipboard_get(Ctx *c){char *s=SDL_GetClipboardText();size_t len=strlen(s),offset=A(0);int cap=A(2),n=0;if(cap>0){if(offset<len){n=(int)(len-offset);if(n>=cap)n=cap-1;memcpy(G_MEM+A(1),s+offset,n);}G_MEM[A(1)+n]=0;}SDL_free(s);RET(n,3);}
static void app_arena(Ctx *c)
{ /* Restrict only the game window; never change a desktop-wide cursor boundary. */
    SDL_Window *w=SDL_GetMouseFocus();if(w){SDL_Rect rect={(int)A(0),(int)A(1),(int)A(2)-(int)A(0),(int)A(3)-(int)A(1)};SDL_SetWindowMouseRect(w,A(4)?&rect:NULL);}RET(0,5);
}
static void get_rect(uint32_t panel,unsigned slot,int out[4])
{ uint32_t p=bhl_heap_alloc(16,1);VC(panel,slot,p,p+4,p+8,p+12);memcpy(out,G_MEM+p,16);bhl_heap_free(p); }
static void apply_paint(void)
{
    if(paint_depth){Paint *p=&paint_stack[paint_depth-1];memcpy(translation,p->translation,8);memcpy(clip,p->clip,16);}
    else{translation[0]=translation[1]=0;clip[0]=clip[1]=0;clip[2]=screen[0];clip[3]=screen[1];}
    api->SetPaintOffset(translation[0],translation[1]);
}
static void quad(float x0,float y0,float x1,float y1,float s0,float t0,float s1,float t1)
{
    x0+=translation[0];x1+=translation[0];y0+=translation[1];y1+=translation[1];
    if(x1<=x0||y1<=y0)return;
    float left=fmaxf(x0,clip[0]),top=fmaxf(y0,clip[1]),right=fminf(x1,clip[2]),bottom=fminf(y1,clip[3]);if(right<=left||bottom<=top)return;
    vpoint_t a={{left,top},{s0+(s1-s0)*(left-x0)/(x1-x0),t0+(t1-t0)*(top-y0)/(y1-y0)}};
    vpoint_t b={{right,bottom},{s0+(s1-s0)*(right-x0)/(x1-x0),t0+(t1-t0)*(bottom-y0)/(y1-y0)}};
    api->DrawQuad(&a,&b);
}
static void filled(int x0,int y0,int x1,int y1){if(draw_color[3]>=255)return;api->SetupDrawingRect(draw_color);api->EnableTexture(0);quad(x0,y0,x1,y1,0,0,0,0);api->EnableTexture(1);}
static Font *font_cache(uint32_t object)
{
    if(!object)return NULL;for(Font *f=fonts;f;f=f->next)if(f->guest==object)return f;
    Font *f=calloc(1,sizeof *f);if(!f)abort();f->guest=object;f->tall=VC(object,4);if(f->tall<=0||f->tall>2048)fail("invalid font height");
    uint32_t p=bhl_heap_alloc(12,1);for(unsigned i=0;i<256;i++){VC(object,2,i,p,p+4,p+8);memcpy(f->abc[i],G_MEM+p,12);}bhl_heap_free(p);
    f->next=fonts;fonts=f;return f;
}
static void glyph(Font *f,unsigned ch)
{
    if(f->texture[ch]||f->abc[ch][1]<=0||ch<=32)return;
    int w=1,h=1;while(w<f->abc[ch][1])w*=2;while(h<f->tall)h*=2;if(w>2048||h>2048)fail("invalid glyph dimensions");
    uint32_t pixels=bhl_heap_alloc(w*h*4,1);VC(f->guest,1,ch,0,0,w,h,pixels);
    f->texture[ch]=api->GenerateTexture();f->size[ch][0]=w;f->size[ch][1]=h;api->UploadTexture(f->texture[ch],(char*)G_MEM+pixels,w,h);bhl_heap_free(pixels);
}
static void surface_dispatch(Ctx *c,unsigned slot)
{
    if(c->ecx!=surface)fail("unknown surface object");
    switch(slot){
    case 8: /* Engine window title remains owned by the engine. */RET(0,1);
    case 9: /* Video mode is selected through the engine, not a panel. */RET(0,3);
    case 10:RET(0,0);
    case 11:case 12:RET(0,1); /* Panels stay inside the game window. */
    case 13:RET(SDL_GetKeyboardFocus()!=NULL,0);
    case 14:RET((int)A(0)>=0&&(int)A(1)>=0&&(int)A(0)<screen[0]&&(int)A(1)<screen[1],2);
    case 15:RET(api->GenerateTexture(),0);
    case 16:{int x,y;api->GetCursorPos(&x,&y);rt_w32(G_MEM,A(0),x);rt_w32(G_MEM,A(1),y);RET(0,2);}
    case 18:for(unsigned i=0;i<4;i++)draw_color[i]=(int)A(i);RET(0,4);
    case 19:filled(A(0),A(1),A(2),A(3));RET(0,4);
    case 20:{int x=A(0),y=A(1),r=A(2),b=A(3);filled(x,y,r,y+1);filled(x,b-1,r,b);filled(x,y+1,x+1,b-1);filled(r-1,y+1,r,b-1);RET(0,4);}
    case 21:current_font=font_cache(A(0));RET(0,1);
    case 22:for(unsigned i=0;i<4;i++)text_color[i]=(int)A(i);RET(0,4);
    case 23:text_pos[0]=A(0);text_pos[1]=A(1);RET(0,2);
    case 24:{
        int count=A(1);if(count<0||count>1048576)fail("invalid text length");
        if(current_font){Font *f=current_font;for(int i=0;i<count;i++){unsigned ch=G_MEM[A(0)+i];int *abc=f->abc[ch];text_pos[0]+=abc[0];
            if(ch>32&&ch!=127&&text_color[3]<255&&abc[1]>0){glyph(f,ch);api->BindTexture(f->texture[ch]);api->SetupDrawingImage(text_color);
                quad(text_pos[0],text_pos[1],text_pos[0]+abc[1],text_pos[1]+f->tall,0,0,(float)abc[1]/f->size[ch][0],(float)f->tall/f->size[ch][1]);}
            text_pos[0]+=abc[1]+abc[2];}}
        RET(0,2);
    }
    case 25:api->UploadTexture(A(0),(char*)G_MEM+A(1),A(2),A(3));RET(0,4);
    case 26:api->BindTexture(A(0));RET(0,1);
    case 27:api->SetupDrawingImage(draw_color);quad((int)A(0),(int)A(1),(int)A(2),(int)A(3),0,0,1,1);RET(0,4);
    case 28: /* Original Panel::repaint tracks the dirty flag. */RET(0,1);
    case 29: /* All mouse events already arrive from the engine. */RET(0,1);
    case 30:{uint32_t p=A(0);if(p)api->CursorSelect(VC(p,3));RET(0,1);}
    case 31: /* The engine presents once after all panels. */RET(0,0);
    case 32:{if(paint_depth==64)fail("paint stack overflow");Paint *p=&paint_stack[paint_depth++];p->panel=A(0);int inset[4]={0},abs[4];if(A(1))get_rect(p->panel,57,inset);get_rect(p->panel,14,abs);get_rect(p->panel,15,p->clip);p->translation[0]=abs[0]+inset[0];p->translation[1]=abs[1]+inset[1];apply_paint();RET(0,2);}
    case 33:if(!paint_depth||paint_stack[paint_depth-1].panel!=A(0))fail("unbalanced paint stack");paint_depth--;apply_paint();RET(0,1);
    case 34:RET(0,0); /* There is no separate native window state to commit. */
    default:fail("unimplemented surface method");
    }
}
#define SURFACE(n) static void surface_##n(Ctx *c){surface_dispatch(c,n);}
SURFACE(8) SURFACE(9) SURFACE(10) SURFACE(11) SURFACE(12) SURFACE(13) SURFACE(14) SURFACE(15) SURFACE(16)
SURFACE(18) SURFACE(19) SURFACE(20) SURFACE(21) SURFACE(22) SURFACE(23) SURFACE(24) SURFACE(25) SURFACE(26) SURFACE(27)
SURFACE(28) SURFACE(29) SURFACE(30) SURFACE(31) SURFACE(32) SURFACE(33) SURFACE(34)
static GuestFn surface_methods[]={NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,surface_8,surface_9,surface_10,surface_11,surface_12,surface_13,surface_14,surface_15,surface_16,NULL,surface_18,surface_19,surface_20,surface_21,surface_22,surface_23,surface_24,surface_25,surface_26,surface_27,surface_28,surface_29,surface_30,surface_31,surface_32,surface_33,surface_34};
static void startup(int width,int height)
{
    screen[0]=width;screen[1]=height;if(root){VC(root,2,width,height);return;}
    bhl_initialize();bhl_attach("vgui.dll");
    if(!app){
        app=bhl_heap_alloc(0x22c,1);method(app,"??0App@vgui@@QAE@_N@Z",(uint32_t[]){1},1);app_table=copy_vtable(app,55);
        override(app_table,38,"vgui-app!main",app_main);override(app_table,53,"vgui-app!platTick",app_tick);
        override(app_table,23,"vgui-app!time",app_time);override(app_table,13,"vgui-app!cursorGet",app_cursor_get);override(app_table,12,"vgui-app!cursorSet",app_cursor_set);
        override(app_table,26,"vgui-app!clipboardCount",app_clipboard_count);override(app_table,27,"vgui-app!clipboardSet",app_clipboard_set);override(app_table,28,"vgui-app!clipboardGet",app_clipboard_get);override(app_table,30,"vgui-app!mouseArena",app_arena);
    }
    root=bhl_heap_alloc(188,1);method(root,"??0Panel@vgui@@QAE@HHHH@Z",(uint32_t[]){0,0,width,height},4);
    VC(root,54,0);VC(root,55,0);VC(root,9,1);
    cursor=bhl_heap_alloc(20,1);method(cursor,"??0Cursor@vgui@@QAE@W4DefaultCursor@01@@Z",(uint32_t[]){1},1);VC(root,37,cursor);
    VC(app,0);VC(app,37,0);
    surface=bhl_heap_alloc(64,1);method(surface,"??0SurfaceBase@vgui@@QAE@PAVPanel@1@@Z",&root,1);surface_table=copy_vtable(surface,35);
    for(unsigned i=0;i<35;i++)if(surface_methods[i]){char name[64];snprintf(name,sizeof name,"vgui-surface!%u",i);override(surface_table,i,name,surface_methods[i]);}
    VC(root,91,surface);api->DrawInit();apply_paint();fprintf(stderr,"BHL v3 VGUI App and native surface initialized\n");
}
static void shutdown(void)
{
    if(!root)return;VC(app,1);
    /* This SDK has no Panel or Dar destructor. Detach client-owned children;
       reclaim only the arrays and panels allocated by this support layer. */
    VC(root,66);
    method(surface,"??1SurfaceBase@vgui@@IAE@XZ",NULL,0);bhl_heap_free(surface);surface=0;bhl_heap_free(surface_table);surface_table=0;
    for(unsigned i=0;i<4;i++){const unsigned offsets[]={0x34,0x48,0x54,0x88};bhl_heap_free(rt_r32(G_MEM,root+offsets[i]));}
    bhl_heap_free(root);root=0;
    /* App focus fields refer to panels; no focus survives a surface shutdown. */
    memset(G_MEM+app+0x14,0,5*4);paint_depth=0;
    while(fonts){Font *next=fonts->next;free(fonts);fonts=next;}current_font=NULL;api->DrawShutdown();paint_depth=0;
}
static void *get_panel(void){return (void*)(uintptr_t)root;}
static void paint(void){if(!root||!surface||!api->IsInGame())return;VC(app,2);VC(root,4,0,0,screen[0],screen[1]);VC(root,12);VC(root,90);if(paint_depth)fail("paint traversal leaked state");}
static void mouse(enum VGUI_MouseAction action,int code){if(!surface)return;static const unsigned slots[]={41,43,42,44};if((unsigned)action>=4)fail("unknown mouse action");VC(app,slots[action],code,surface);}
static void key(enum VGUI_KeyAction action,enum VGUI_KeyCode code){if(!surface)return;static const unsigned slots[]={46,45,47};if((unsigned)action>=3)fail("unknown key action");VC(app,slots[action],code,surface);}
static void mouse_move(int x,int y){if(surface)VC(app,40,x,y,surface);}
static void text_input(const char *text){/* VGUI 1 receives typed keys through Key, not UTF-8 text events. */(void)text;}
__attribute__((visibility("default"))) void InitVGUISupportAPI(vguiapi_t *engine)
{ api=engine;api->Startup=startup;api->Shutdown=shutdown;api->GetPanel=get_panel;api->Paint=paint;api->Mouse=mouse;api->Key=key;api->MouseMove=mouse_move;api->TextInput=text_input; }
