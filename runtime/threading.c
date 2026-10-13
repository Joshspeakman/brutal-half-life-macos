/* Native pthread workers with separate guest stacks and TEBs. Cancellation
 * is cooperative at API boundaries outside guest critical sections. */
#include "loader.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define RET(v,n) do{c->eax=(uint32_t)(v);c->esp+=4+4*(n);return 1;}while(0)
extern void bhl_set_last_error(uint32_t);
extern int bhl_heap_free(uint32_t);
typedef struct Worker {
    pthread_t native;Ctx context;uint32_t handle,id,start,argument,stack,teb,result;
    int closed,done,suspended,cancel,normal_exit,critical_depth,base_priority;
    jmp_buf escape;
    struct Worker *next;
} Worker;
typedef struct Event {uint32_t handle;int closed,manual,signaled;char *name;struct Event *next;} Event;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static Worker *workers;
static Event *events;
static uint32_t next_worker=2,next_event=1;
static __thread Worker *current;
static Worker *find_worker(uint32_t h){if(h==0xfffffffeu)return current;for(Worker *w=workers;w;w=w->next)if(w->handle==h&&!w->closed)return w;return NULL;}
static Event *find_event(uint32_t h){for(Event *e=events;e;e=e->next)if(e->handle==h&&!e->closed)return e;return NULL;}
static uint64_t milliseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
void bhl_thread_checkpoint(void)
{
    if(!current)return;pthread_mutex_lock(&mutex);int cancel=current->cancel&&!current->critical_depth;pthread_mutex_unlock(&mutex);
    if(cancel)longjmp(current->escape,1);
}
void bhl_thread_critical(int delta)
{
    if(!current)return;pthread_mutex_lock(&mutex);current->critical_depth+=delta;if(current->critical_depth<0)abort();pthread_mutex_unlock(&mutex);
}
/* Timed wake-ups allow a terminated worker to leave waits without abandoning
 * a native pthread mutex or touching another thread's stack. */
static void tick_wait(void)
{
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_nsec+=20000000;
    if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
    pthread_cond_timedwait(&changed,&mutex,&deadline);
}
static void *run_worker(void *argument)
{
    Worker *w=argument;current=w;
    if(!setjmp(w->escape)){
        pthread_mutex_lock(&mutex);while(w->suspended&&!w->cancel)tick_wait();pthread_mutex_unlock(&mutex);bhl_thread_checkpoint();
        bhl_enter_guest_thread(&w->context);
        w->result=bhl_invoke(w->start,&w->argument,1,2);w->normal_exit=1;
    }
    if(w->normal_exit)bhl_leave_guest_thread();
    bhl_heap_free(w->stack);bhl_heap_free(w->teb);
    pthread_mutex_lock(&mutex);w->done=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);current=NULL;return NULL;
}
static void fail(Ctx *c,const char *name){rt_unhandled(c,rt_r32(G_MEM,c->esp),name);}
int bhl_thread_import(Ctx *c,const char *name)
{
    if(!strcmp(name,"CreateThread")){
        uint32_t stack_size=A(1),flags=A(4);if(A(0)||flags&~4u||!A(2)){fail(c,"CreateThread security/flags/start");return 1;}
        if(stack_size>16*1024*1024){bhl_set_last_error(8);RET(0,6);}if(stack_size<4*1024*1024)stack_size=4*1024*1024;
        stack_size=(stack_size+65535)&~65535u;
        Worker *w=calloc(1,sizeof *w);if(!w){bhl_set_last_error(8);RET(0,6);}
        w->stack=bhl_heap_alloc(stack_size,1);w->teb=bhl_heap_alloc(0x2000,1);
        if(!w->stack||!w->teb){bhl_heap_free(w->stack);bhl_heap_free(w->teb);free(w);bhl_set_last_error(8);RET(0,6);}
        pthread_mutex_lock(&mutex);w->id=next_worker++;w->handle=0x02000000+w->id*4;pthread_mutex_unlock(&mutex);
        w->start=A(2);w->argument=A(3);w->suspended=!!(flags&4);
        Ctx *t=&w->context;t->esp=w->stack+stack_size-0x100;t->fs_base=w->teb;t->fcw=0x27f;t->mxcsr=0x1f80;t->f.eflags=0x202;
        rt_w32(G_MEM,w->teb,0xffffffff);rt_w32(G_MEM,w->teb+4,w->stack+stack_size);rt_w32(G_MEM,w->teb+8,w->stack);rt_w32(G_MEM,w->teb+0x18,w->teb);
        rt_w32(G_MEM,w->teb+0x20,rt_r32(G_MEM,c->fs_base+0x20));rt_w32(G_MEM,w->teb+0x24,w->id);rt_w32(G_MEM,w->teb+0x30,rt_r32(G_MEM,c->fs_base+0x30));
        pthread_mutex_lock(&mutex);w->next=workers;workers=w;
        int error=pthread_create(&w->native,NULL,run_worker,w);
        if(error){workers=w->next;pthread_mutex_unlock(&mutex);bhl_heap_free(w->stack);bhl_heap_free(w->teb);free(w);bhl_set_last_error(8);RET(0,6);}
        int policy;struct sched_param param;if(!pthread_getschedparam(w->native,&policy,&param))w->base_priority=param.sched_priority;
        pthread_detach(w->native);pthread_mutex_unlock(&mutex);if(A(5))rt_w32(G_MEM,A(5),w->id);RET(w->handle,6);
    }
    if(!strcmp(name,"GetCurrentThread"))RET(0xfffffffeu,0);
    if(!strcmp(name,"ResumeThread")){
        pthread_mutex_lock(&mutex);Worker *w=find_worker(A(0));if(!w){pthread_mutex_unlock(&mutex);bhl_set_last_error(6);RET(0xffffffff,1);}
        int previous=w->suspended;w->suspended=0;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);RET(previous,1);
    }
    if(!strcmp(name,"SetThreadPriority")){
        pthread_mutex_lock(&mutex);Worker *w=find_worker(A(0));pthread_mutex_unlock(&mutex);
        if(!w){bhl_set_last_error(6);RET(0,2);}int32_t priority=A(1);
        if((priority< -2||priority>2)&&priority!=15&&priority!=-15){bhl_set_last_error(87);RET(0,2);}
        int policy;struct sched_param param;int error=pthread_getschedparam(w->native,&policy,&param);
        if(!error){int low=sched_get_priority_min(policy),high=sched_get_priority_max(policy);
            param.sched_priority=priority==15?high:priority==-15?low:w->base_priority+priority;
            if(param.sched_priority<low)param.sched_priority=low;if(param.sched_priority>high)param.sched_priority=high;
            error=pthread_setschedparam(w->native,policy,&param);}
        if(error)bhl_set_last_error(5);RET(error==0,2);
    }
    if(!strcmp(name,"TerminateThread")){
        pthread_mutex_lock(&mutex);Worker *w=find_worker(A(0));if(!w){pthread_mutex_unlock(&mutex);bhl_set_last_error(6);RET(0,2);}
        if(w==current){w->result=A(1);w->cancel=1;pthread_mutex_unlock(&mutex);bhl_thread_checkpoint();fail(c,"self termination while holding critical sections");return 1;}
        if(!w->done){w->result=A(1);w->cancel=1;pthread_cond_broadcast(&changed);}
        uint64_t limit=milliseconds()+2000;while(!w->done&&milliseconds()<limit)tick_wait();int done=w->done;pthread_mutex_unlock(&mutex);
        if(!done){bhl_set_last_error(1460);fprintf(stderr,"BHL native thread: cooperative termination timed out\n");}RET(done,2);
    }
    if(!strcmp(name,"ExitThread")){
        if(!current){fail(c,"ExitThread on engine thread");return 1;}current->result=A(0);current->normal_exit=1;longjmp(current->escape,1);
    }
    if(!strcmp(name,"GetExitCodeThread")){
        pthread_mutex_lock(&mutex);Worker *w=find_worker(A(0));if(!w||!A(1)){pthread_mutex_unlock(&mutex);bhl_set_last_error(6);RET(0,2);}
        rt_w32(G_MEM,A(1),w->done?w->result:259);pthread_mutex_unlock(&mutex);RET(1,2);
    }
    if(!strcmp(name,"CreateEventA")){
        if(A(0)){fail(c,"CreateEvent security attributes");return 1;}const char *name=A(3)?(char*)G_MEM+A(3):NULL;
        pthread_mutex_lock(&mutex);
        if(name)for(Event *e=events;e;e=e->next)if(!e->closed&&e->name&&!strcmp(name,e->name)){pthread_mutex_unlock(&mutex);fail(c,"shared named event handles");return 1;}
        Event *e=calloc(1,sizeof *e);if(!e)abort();e->handle=0x03000000+4*next_event++;e->manual=!!A(1);e->signaled=!!A(2);e->name=name?strdup(name):NULL;e->next=events;events=e;
        pthread_mutex_unlock(&mutex);RET(e->handle,4);
    }
    if(!strcmp(name,"SetEvent")||!strcmp(name,"ResetEvent")){
        pthread_mutex_lock(&mutex);Event *e=find_event(A(0));if(!e){pthread_mutex_unlock(&mutex);bhl_set_last_error(6);RET(0,1);}
        e->signaled=!strcmp(name,"SetEvent");pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);RET(1,1);
    }
    if(!strcmp(name,"WaitForSingleObject")){
        uint32_t handle=A(0),timeout=A(1);uint64_t limit=milliseconds()+timeout;
        for(;;){
            bhl_thread_checkpoint();pthread_mutex_lock(&mutex);Worker *w=find_worker(handle);Event *e=find_event(handle);
            if(!w&&!e){pthread_mutex_unlock(&mutex);bhl_set_last_error(6);RET(0xffffffff,2);}
            if((w&&w->done)||(e&&e->signaled)){if(e&&!e->manual)e->signaled=0;pthread_mutex_unlock(&mutex);RET(0,2);}
            if(timeout!=0xffffffff&&milliseconds()>=limit){pthread_mutex_unlock(&mutex);RET(258,2);}
            tick_wait();pthread_mutex_unlock(&mutex);
        }
    }
    if(!strcmp(name,"CloseHandle")){
        pthread_mutex_lock(&mutex);Worker *w=find_worker(A(0));Event *e=find_event(A(0));if(w)w->closed=1;if(e)e->closed=1;pthread_mutex_unlock(&mutex);
        if(w||e)RET(1,1);return 0;
    }
    if(!strcmp(name,"Sleep")){
        uint64_t limit=milliseconds()+A(0);
        do{bhl_thread_checkpoint();uint64_t now=milliseconds();if(now>=limit)break;uint64_t left=limit-now;if(left>20)left=20;struct timespec t={0,(long)left*1000000};nanosleep(&t,NULL);}while(1);
        RET(0,1);
    }
    return 0;
}
