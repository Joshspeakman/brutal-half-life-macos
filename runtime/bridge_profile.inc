/* Optional inclusive boundary timings. Enable with BHL_BRIDGE_PROFILE=1;
 * measurements go to the game log at shutdown, never to an external service.
 * These adapters run on the engine thread. Nested totals intentionally overlap. */
#include <time.h>
/* These are launch options. Reading the process environment on every engine
 * callback takes a libc lock even when all diagnostics are disabled. */
static int bridge_sync_stats_enabled;
static int bridge_eager_entities,bridge_verify_entities,bridge_trace,bridge_view_trace,bridge_combat_trace;
__attribute__((constructor)) static void bridge_options_initialize(void)
{
    bridge_eager_entities=getenv("BHL_EAGER_ENTITIES")!=NULL;
    bridge_verify_entities=getenv("BHL_VERIFY_ENTITY_ACCESS")!=NULL;
    bridge_trace=getenv("BHL_BRIDGE_TRACE")!=NULL;
    bridge_view_trace=getenv("BHL_VIEW_TRACE")!=NULL;
    bridge_combat_trace=getenv("BHL_COMBAT_TRACE")!=NULL;
    bridge_sync_stats_enabled=getenv("BHL_SYNC_STATS")!=NULL;
}
typedef struct BridgeTiming { const char *name; uint32_t key; uint64_t calls,ns,max_ns; } BridgeTiming;
typedef struct BridgeTimer { BridgeTiming *entry; uint64_t start; } BridgeTimer;
static BridgeTiming bridge_timings[4096];
static int bridge_profile_enabled=-1;
static uint64_t bridge_clock(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static BridgeTimer bridge_timer_begin(const char *name,uint32_t key)
{
    if(bridge_profile_enabled<0){const char *v=getenv("BHL_BRIDGE_PROFILE");bridge_profile_enabled=v&&!strcmp(v,"1");}
    if(!bridge_profile_enabled)return (BridgeTimer){0};
    unsigned slot=(key*2654435761u)>>20;
    for(unsigned i=0;i<4096;i++,slot=(slot+1)&4095){
        BridgeTiming *p=bridge_timings+slot;
        if(!p->name){p->name=name;p->key=key;}
        if(p->key==key&&!strcmp(p->name,name))return (BridgeTimer){p,bridge_clock()};
    }
    return (BridgeTimer){0};
}
static void bridge_timer_end(BridgeTimer *timer)
{
    if(!timer->entry)return;uint64_t ns=bridge_clock()-timer->start;BridgeTiming *p=timer->entry;
    p->calls++;p->ns+=ns;if(ns>p->max_ns)p->max_ns=ns;
}
__attribute__((destructor)) static void bridge_profile_report(void)
{
    for(unsigned i=0;i<4096;i++){BridgeTiming *p=bridge_timings+i;if(p->calls)
        fprintf(stderr,"BHL_PROFILE %s 0x%08x calls=%llu total_ms=%.3f max_ms=%.3f\n",p->name,p->key,
            (unsigned long long)p->calls,p->ns/1e6,p->max_ns/1e6);}
}
/* Optional synchronization fallback attribution (BHL_SYNC_STATS=1): counts
 * complete materializations by the boundary or import that caused them. */
static int bridge_sync_stats=-1;
static const char *bridge_boundary;
typedef struct BridgeFallback { const char *scope,*boundary; uint64_t count; } BridgeFallback;
static BridgeFallback bridge_fallbacks[512];
static void bridge_fallback(const char *scope)
{
    if(bridge_sync_stats<0)bridge_sync_stats=getenv("BHL_SYNC_STATS")!=NULL;
    if(!bridge_sync_stats)return;
    const char *boundary=rt_watch_reason?rt_watch_reason:bridge_boundary?bridge_boundary:"(callback)";
    for(unsigned i=0;i<512;i++){
        BridgeFallback *p=bridge_fallbacks+i;
        if(!p->scope){p->scope=scope;p->boundary=boundary;}
        if(p->scope==scope&&p->boundary==boundary){p->count++;return;}
    }
}
/* Interned "reason:last-boundary" label, built only when statistics run. */
static const char *bridge_label(const char *reason)
{
    if(!bridge_sync_stats_enabled)return reason;
    static char labels[128][96];static const char *keys[128][2];
    const char *last=bridge_boundary?bridge_boundary:"(callback)";
    for(unsigned i=0;i<128;i++){
        if(!keys[i][0]){keys[i][0]=reason;keys[i][1]=last;snprintf(labels[i],sizeof labels[i],"%s:%s",reason,last);}
        if(keys[i][0]==reason&&keys[i][1]==last)return labels[i];
    }
    return reason;
}
__attribute__((destructor)) static void bridge_fallback_report(void)
{
    for(unsigned i=0;i<512;i++)if(bridge_fallbacks[i].count)
        fprintf(stderr,"BHL_SYNC_FALLBACK %s %s count=%llu\n",bridge_fallbacks[i].scope,bridge_fallbacks[i].boundary,(unsigned long long)bridge_fallbacks[i].count);
}
#define BHL_PROFILE(name,key) BridgeTimer bridge_timer __attribute__((cleanup(bridge_timer_end)))=bridge_timer_begin(name,key)
