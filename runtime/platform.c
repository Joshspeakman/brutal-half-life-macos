/* Native SDL input and renderer-dispatched graphics calls from the client. */
#include "win32.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define CDECL(v) do { c->eax=(uint32_t)(v);c->esp+=4;return 1; } while(0)
#define STDCALL(v,n) do { c->eax=(uint32_t)(v);c->esp+=4+4*(n);return 1; } while(0)
static SDL_GameController *controllers[32];
static void *(*graphics_proc)(const char *);
void bhl_set_graphics_proc(void *(*resolve)(const char *)) { graphics_proc=resolve; }
static uint32_t controller_names[32];
static SDL_GameController *controller(uint32_t h)
{
    uint32_t i=h-0x7d000000u;return i<32?controllers[i]:NULL;
}
static void fail(Ctx *c,const char *reason){rt_unhandled(c,rt_r32(G_MEM,c->esp),reason);}
/* Optional mouse diagnostics (BHL_MOUSE_TRACE=1): every cursor read, warp,
   relative-motion read and relative-mode change, with SDL's tick in ms. */
static int mouse_trace=-1;
static void mouse_log(const char *what,int a,int b)
{
    if(mouse_trace<0)mouse_trace=getenv("BHL_MOUSE_TRACE")!=NULL;
    if(mouse_trace)fprintf(stderr,"BHL_MOUSE %.3f %s %d %d rel=%d\n",SDL_GetPerformanceCounter()*1000.0/SDL_GetPerformanceFrequency(),what,a,b,(int)SDL_GetRelativeMouseMode());
}
int bhl_platform_import(Ctx *c,const char *qualified)
{
    const char *name=strchr(qualified,'!');if(!name)return 0;name++;
    if(!strncasecmp(qualified,"SDL2.dll!",9)){
        if(!strcmp(name,"SDL_NumJoysticks"))CDECL(SDL_NumJoysticks());
        if(!strcmp(name,"SDL_SetRelativeMouseMode")){int r=SDL_SetRelativeMouseMode(A(0)?SDL_TRUE:SDL_FALSE);mouse_log("set-relative",A(0),r);CDECL(r);}
        if(!strcmp(name,"SDL_IsGameController"))CDECL(SDL_IsGameController(A(0)));
        if(!strcmp(name,"SDL_GameControllerOpen")){
            SDL_GameController *p=SDL_GameControllerOpen(A(0));if(!p)CDECL(0);
            for(unsigned i=0;i<32;i++)if(controllers[i]==p){SDL_GameControllerClose(p);CDECL(0x7d000000u+i);}
            for(unsigned i=0;i<32;i++)if(!controllers[i]){controllers[i]=p;CDECL(0x7d000000u+i);}
            SDL_GameControllerClose(p);fail(c,"native game-controller handle capacity");return 1;
        }
        if(!strcmp(name,"SDL_GameControllerGetAxis"))CDECL((int32_t)SDL_GameControllerGetAxis(controller(A(0)),A(1)));
        if(!strcmp(name,"SDL_GameControllerGetButton"))CDECL(SDL_GameControllerGetButton(controller(A(0)),A(1)));
        if(!strcmp(name,"SDL_GameControllerName")){
            SDL_GameController *p=controller(A(0));const char *s=SDL_GameControllerName(p);if(!s)CDECL(0);unsigned i=A(0)-0x7d000000u;
            if(!controller_names[i]){size_t n=strlen(s)+1;controller_names[i]=bhl_heap_alloc(n,0);memcpy(G_MEM+controller_names[i],s,n);}CDECL(controller_names[i]);
        }
        if(!strcmp(name,"SDL_GetRelativeMouseState")){
            int x=0,y=0;uint32_t r=SDL_GetRelativeMouseState(&x,&y);
            if(A(0))rt_w32(G_MEM,A(0),x);if(A(1))rt_w32(G_MEM,A(1),y);mouse_log("relative",x,y);CDECL(r);
        }
        if(!strcmp(name,"SDL_JoystickUpdate")){SDL_JoystickUpdate();CDECL(0);}
        return 0;
    }
    if(!strncasecmp(qualified,"USER32.dll!",11)){
        if(!strcmp(name,"GetForegroundWindow"))STDCALL(SDL_GetKeyboardFocus()?0x7e100000u:0,0);
        if(!strcmp(name,"GetCursorPos")){if(!A(0))STDCALL(0,1);int x,y;SDL_GetGlobalMouseState(&x,&y);rt_w32(G_MEM,A(0),x);rt_w32(G_MEM,A(0)+4,y);mouse_log("cursor",x,y);STDCALL(1,1);}
        if(!strcmp(name,"SetCursorPos")){
            SDL_Window *window=SDL_GetMouseFocus();if(!window)STDCALL(0,2);int x,y;SDL_GetWindowPosition(window,&x,&y);
            SDL_WarpMouseInWindow(window,(int)A(0)-x,(int)A(1)-y);mouse_log("warp",(int)A(0),(int)A(1));STDCALL(1,2);
        }
        return 0;
    }
    if(!strncasecmp(qualified,"OPENGL32.dll!",13)){
        if(!SDL_GL_GetCurrentContext()){fail(c,"OpenGL call without a native OpenGL context");return 1;}
        /* GLES/Metal emulates fog in the renderer's shaders. Resolving this
           directly from SDL would bypass that state (GLES has no glFog). */
        void *symbol=graphics_proc?graphics_proc(name):SDL_GL_GetProcAddress(name);
        if(!symbol){fail(c,"native renderer entry missing");return 1;}
        if(!strcmp(name,"glEnable")||!strcmp(name,"glDisable")){((void(*)(unsigned))symbol)(A(0));STDCALL(0,1);}
        if(!strcmp(name,"glFogi")){((void(*)(unsigned,int))symbol)(A(0),A(1));STDCALL(0,2);}
        if(!strcmp(name,"glFogf")){((void(*)(unsigned,float))symbol)(A(0),rt_rf32(G_MEM,c->esp+8));STDCALL(0,2);}
        if(!strcmp(name,"glFogfv")){((void(*)(unsigned,const float *))symbol)(A(0),(void*)(G_MEM+A(1)));STDCALL(0,2);}
    }
    return 0;
}
