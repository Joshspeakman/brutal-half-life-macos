/* Benchmark-only SDL presentation probe, injected into an isolated test run.
 * It calls the real swap unchanged and records intervals only while armed. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static double intervals[200000];
static unsigned count;
static uint64_t previous;
static const char *gate,*output;
extern void SDL_GL_SwapWindow(void *);
static uint64_t now(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
__attribute__((constructor)) static void setup(void)
{gate=getenv("BHL_BENCH_GATE");output=getenv("BHL_BENCH_TIMES");}
static void measured_swap(void *window)
{
    /* dyld excludes references from the interposer itself. dlsym would
       resolve the replacement again on current macOS and recurse. */
    SDL_GL_SwapWindow(window);uint64_t time=now();
    if(!gate||access(gate,F_OK)){previous=0;return;}
    if(previous&&count<sizeof intervals/sizeof *intervals)intervals[count++]=(time-previous)/1e6;
    previous=time;
}
__attribute__((used,section("__DATA,__interpose"))) static struct {void *replacement,*original;} hook={measured_swap,SDL_GL_SwapWindow};
__attribute__((destructor)) static void finish(void)
{
    if(!output)return;FILE *f=fopen(output,"w");if(!f)return;
    fprintf(f,"[");for(unsigned i=0;i<count;i++)fprintf(f,"%s%.6f",i?",":"",intervals[i]);fprintf(f,"]\n");fclose(f);
}
