/* Test-only SDL queue injection. Never posts macOS/global keyboard events. */
#include <SDL.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int (*push)(SDL_Event*);
static void key(SDL_Scancode scan,SDL_Keycode code,int down)
{
    SDL_Event event={0};event.type=down?SDL_KEYDOWN:SDL_KEYUP;event.key.windowID=1;
    event.key.state=down?SDL_PRESSED:SDL_RELEASED;event.key.keysym.scancode=scan;event.key.keysym.sym=code;
    fprintf(stderr,"BHL SDL injected scan=%d down=%d: %d\n",scan,down,push(&event));
}
static void position(void){key(SDL_SCANCODE_F1,SDLK_F1,1);key(SDL_SCANCODE_F1,SDLK_F1,0);}
static void *drive(void *unused)
{
    const char *path=getenv("BHL_TEST_INPUT_LOG");if(!path)return NULL;int ready=0;
    for(unsigned attempt=0;attempt<120000&&!ready;attempt++,usleep(1000)){
        push=dlsym(RTLD_DEFAULT,"SDL_PushEvent");if(!push)continue;
        FILE *file=fopen(path,"r");if(!file)continue;fseek(file,0,SEEK_END);long size=ftell(file);if(size>32768)fseek(file,-32768,SEEK_END);else rewind(file);
        char text[32769];size_t n=fread(text,1,sizeof text-1,file);text[n]=0;fclose(file);ready=strstr(text,"BHL_INPUT_READY")!=NULL;
    }
    if(!ready)return NULL;
    /* The initial config has now drained. Input-bound commands append to the
       engine queue, so they must not sit behind a queued wait/quit script. */
    key(SDL_SCANCODE_W,SDLK_w,1);usleep(1500000);key(SDL_SCANCODE_W,SDLK_w,0);usleep(250000);position();
    key(SDL_SCANCODE_SPACE,SDLK_SPACE,1);usleep(200000);key(SDL_SCANCODE_SPACE,SDLK_SPACE,0);position();usleep(2000000);
    key(SDL_SCANCODE_LCTRL,SDLK_LCTRL,1);usleep(500000);position();usleep(200000);
    key(SDL_SCANCODE_LCTRL,SDLK_LCTRL,0);usleep(500000);position();usleep(200000);
    key(SDL_SCANCODE_F12,SDLK_F12,1);key(SDL_SCANCODE_F12,SDLK_F12,0);return NULL;
}
__attribute__((constructor)) static void start(void){pthread_t thread;if(!pthread_create(&thread,NULL,drive,NULL))pthread_detach(thread);}
