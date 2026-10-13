/* Benchmark-only presentation timeline probe, injected into an isolated run.
 * Superset of frame_times.c: records the time spent inside SDL_GL_SwapWindow,
 * time blocked in -[CAMetalLayer nextDrawable], and each drawable's actual
 * presentedTime from Metal. Optional overrides change the layer's
 * maximumDrawableCount/displaySyncEnabled for presentation experiments only. */
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { MAX_FRAMES = 200000 };
typedef struct { double swap_begin, swap_end, wait, presented, gpu_start, gpu_end, gpu_busy; int waits, buffers; } frame_t;
static frame_t frames[MAX_FRAMES];
static _Atomic unsigned count;      /* completed swaps while armed */
static int armed_previous;
static const char *gate, *output;
static double timebase;
static int force_drawables = -1, force_sync = -1, logged_layer;
static unsigned sync_calls[2];
static IMP original_next_drawable, original_set_sync;
/* Optional in-process sampler (BHL_PROBE_SAMPLES): an external profiler cannot
 * attach in some environments, so suspend the engine thread briefly, walk its
 * frame-pointer chain and tag the stack with the current frame index. */
enum { DEPTH = 48, MAX_SAMPLES = 400000 };
typedef struct { unsigned frame; unsigned short depth; uint64_t pc[DEPTH]; } sample_t;
static sample_t *samples;
static _Atomic unsigned sample_count;
static const char *sample_output;
static thread_act_t engine_thread;
static uintptr_t stack_low, stack_high;
static volatile int sampling;
static double then(void);
/* Libraries can be unloaded before exit, so record images as they load. */
enum { MAX_IMAGES = 2048 };
static struct { uintptr_t header; char *name; } loaded[MAX_IMAGES];
static _Atomic unsigned loaded_count;
static void image_added(const struct mach_header *header, intptr_t slide)
{
    (void)slide; Dl_info info;
    unsigned n = atomic_fetch_add(&loaded_count, 1);
    if (n < MAX_IMAGES && dladdr(header, &info) && info.dli_fname) {
        loaded[n].header = (uintptr_t)header; loaded[n].name = strdup(info.dli_fname);
    }
}
static void *sampler(void *unused)
{
    (void)unused;
    while (sampling) {
        usleep(500);
        if (!(gate && !access(gate, F_OK))) continue;
        unsigned n = atomic_load(&sample_count);
        if (n >= MAX_SAMPLES) break;
        arm_thread_state64_t state; mach_msg_type_number_t size = ARM_THREAD_STATE64_COUNT;
        if (thread_suspend(engine_thread) != KERN_SUCCESS) continue;
        if (thread_get_state(engine_thread, ARM_THREAD_STATE64, (thread_state_t)&state, &size) == KERN_SUCCESS) {
            sample_t *s = &samples[n]; s->frame = atomic_load(&count);
            s->pc[0] = arm_thread_state64_get_pc(state); s->pc[1] = arm_thread_state64_get_lr(state);
            unsigned depth = 2; uintptr_t fp = arm_thread_state64_get_fp(state);
            while (depth < DEPTH && fp >= stack_low && fp + 16 <= stack_high && !(fp & 7)) {
                uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
                if (!ret) break;
                s->pc[depth++] = ret & 0x0000ffffffffffffull;
                if (next <= fp) break;
                fp = next;
            }
            s->depth = depth; atomic_store(&sample_count, n + 1);
        }
        thread_resume(engine_thread);
    }
    return NULL;
}

static void write_samples(void)
{
    FILE *f = fopen(sample_output, "w"); if (!f) return;
    fprintf(f, "{\"images\":[");
    unsigned images = atomic_load(&loaded_count);
    for (unsigned i = 0, first = 1; i < images && i < MAX_IMAGES; i++) {
        if (!loaded[i].name) continue;
        fprintf(f, "%s[\"%s\",%llu]", first ? "" : ",", loaded[i].name, (unsigned long long)loaded[i].header); first = 0;
    }
    fprintf(f, "],\"samples\":[");
    unsigned n = atomic_load(&sample_count);
    for (unsigned i = 0; i < n; i++) {
        fprintf(f, "%s[%u", i ? "," : "", samples[i].frame);
        for (unsigned d = 0; d < samples[i].depth; d++) fprintf(f, ",%llu", (unsigned long long)samples[i].pc[d]);
        fprintf(f, "]");
    }
    fprintf(f, "]}\n"); fclose(f);
}

static double then(void) { return mach_absolute_time() * timebase; }
static int armed(void) { return gate && !access(gate, F_OK); }
extern void SDL_GL_SwapWindow(void *);
extern int SDL_GL_GetSwapInterval(void);

/* GPU timing: wrap -commit on every concrete MTLCommandBuffer class that
 * implements it, and attribute each buffer's GPU interval to the frame being
 * built (the next swap). Nested super calls are counted once per thread. */
enum { MAX_COMMIT_CLASSES = 32 };
static struct { Class cls; IMP original; } commits[MAX_COMMIT_CLASSES];
static unsigned commit_classes;
static __thread int in_commit;
static void probe_commit(id self, SEL cmd)
{
    IMP original = NULL;
    for (Class c = object_getClass(self); c && !original; c = class_getSuperclass(c))
        for (unsigned i = 0; i < commit_classes; i++) if (commits[i].cls == c) { original = commits[i].original; break; }
    if (!in_commit && armed()) {
        unsigned index = atomic_load(&count);
        if (index < MAX_FRAMES) {
            frames[index].buffers++;
            [(id<MTLCommandBuffer>)self addCompletedHandler:^(id<MTLCommandBuffer> done) {
                double start = done.GPUStartTime, end = done.GPUEndTime;
                if (start <= 0 || end < start) return;
                frame_t *f = &frames[index];
                if (!f->gpu_start || start < f->gpu_start) f->gpu_start = start;
                if (end > f->gpu_end) f->gpu_end = end;
                f->gpu_busy += end - start;
            }];
        }
    }
    in_commit++; ((void (*)(id, SEL))original)(self, cmd); in_commit--;
}
static void hook_command_buffers(void)
{
    Protocol *protocol = objc_getProtocol("MTLCommandBuffer");
    unsigned n = 0; Class *classes = objc_copyClassList(&n);
    for (unsigned i = 0; i < n && commit_classes < MAX_COMMIT_CLASSES; i++) {
        int conforms = 0;
        for (Class c = classes[i]; c && !conforms; c = class_getSuperclass(c)) conforms = class_conformsToProtocol(c, protocol);
        if (!conforms) continue;
        unsigned methods = 0; Method *list = class_copyMethodList(classes[i], &methods);
        for (unsigned m = 0; m < methods; m++) if (method_getName(list[m]) == @selector(commit)) {
            commits[commit_classes].cls = classes[i];
            commits[commit_classes++].original = method_setImplementation(list[m], (IMP)probe_commit);
            break;
        }
        free(list);
    }
    free(classes);
    fprintf(stderr, "BHL_PRESENT_GPU_HOOKS %u\n", commit_classes);
}

static void describe(CAMetalLayer *layer)
{
    CGSize size = layer.drawableSize;
    fprintf(stderr, "BHL_PRESENT_LAYER displaySync=%d maxDrawables=%lu transaction=%d framebufferOnly=%d "
            "drawable=%.0fx%.0f scale=%.2f\n", layer.displaySyncEnabled, (unsigned long)layer.maximumDrawableCount,
            layer.presentsWithTransaction, layer.framebufferOnly, size.width, size.height, layer.contentsScale);
}

static void apply_overrides(CAMetalLayer *layer)
{
    if (force_drawables > 0 && layer.maximumDrawableCount != (NSUInteger)force_drawables)
        layer.maximumDrawableCount = force_drawables;
    if (force_sync >= 0 && layer.displaySyncEnabled != force_sync)
        ((void (*)(id, SEL, BOOL))original_set_sync)(layer, @selector(setDisplaySyncEnabled:), force_sync);
}

static id probe_next_drawable(CAMetalLayer *self, SEL cmd)
{
    apply_overrides(self);
    if (!logged_layer) { logged_layer = 1; describe(self); hook_command_buffers(); }
    double start = then();
    id<CAMetalDrawable> drawable = ((id (*)(id, SEL))original_next_drawable)(self, cmd);
    double waited = then() - start;
    unsigned index = atomic_load(&count);
    if (armed() && index < MAX_FRAMES) {
        frames[index].wait += waited; frames[index].waits++;
        if (drawable) [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            /* presentedTime is 0 when the drawable was never displayed. */
            if (index < MAX_FRAMES) frames[index].presented = shown.presentedTime;
        }];
    }
    return drawable;
}

static void probe_set_sync(CAMetalLayer *self, SEL cmd, BOOL enabled)
{
    sync_calls[enabled != 0]++;
    if (sync_calls[0] + sync_calls[1] <= 8)
        fprintf(stderr, "BHL_PRESENT_SET_DISPLAY_SYNC %d%s\n", enabled, force_sync >= 0 ? " (overridden)" : "");
    ((void (*)(id, SEL, BOOL))original_set_sync)(self, cmd, force_sync >= 0 ? force_sync : enabled);
}

__attribute__((constructor)) static void setup(void)
{
    mach_timebase_info_data_t info; mach_timebase_info(&info);
    timebase = (double)info.numer / info.denom / 1e9;
    gate = getenv("BHL_BENCH_GATE"); output = getenv("BHL_PRESENT_TRACE");
    if (getenv("BHL_PROBE_MAX_DRAWABLES")) force_drawables = atoi(getenv("BHL_PROBE_MAX_DRAWABLES"));
    if (getenv("BHL_PROBE_DISPLAY_SYNC")) force_sync = atoi(getenv("BHL_PROBE_DISPLAY_SYNC")) != 0;
    sample_output = getenv("BHL_PROBE_SAMPLES");
    if (sample_output && (samples = calloc(MAX_SAMPLES, sizeof *samples))) {
        pthread_t self = pthread_self();
        engine_thread = pthread_mach_thread_np(self);
        stack_high = (uintptr_t)pthread_get_stackaddr_np(self);
        stack_low = stack_high - pthread_get_stacksize_np(self);
        _dyld_register_func_for_add_image(image_added);
        sampling = 1; pthread_t worker; pthread_create(&worker, NULL, sampler, NULL); pthread_detach(worker);
    }
    Class layer = objc_getClass("CAMetalLayer");
    Method next = class_getInstanceMethod(layer, @selector(nextDrawable));
    Method sync = class_getInstanceMethod(layer, @selector(setDisplaySyncEnabled:));
    original_next_drawable = method_setImplementation(next, (IMP)probe_next_drawable);
    original_set_sync = method_setImplementation(sync, (IMP)probe_set_sync);
}

static void measured_swap(void *window)
{
    /* dyld excludes references from the interposer itself. */
    double begin = then();
    SDL_GL_SwapWindow(window);
    double end = then();
    int on = armed();
    if (on && !armed_previous) fprintf(stderr, "BHL_BENCH_SWAP_INTERVAL %d\n", SDL_GL_GetSwapInterval());
    armed_previous = on;
    unsigned index = atomic_load(&count);
    if (!on || index >= MAX_FRAMES) return;
    frames[index].swap_begin = begin; frames[index].swap_end = end;
    atomic_store(&count, index + 1);
}
__attribute__((used, section("__DATA,__interpose"))) static struct { void *replacement, *original; } hook = {measured_swap, SDL_GL_SwapWindow};

__attribute__((destructor)) static void finish(void)
{
    if (samples) { sampling = 0; usleep(5000); write_samples(); }
    if (!output) return;
    FILE *f = fopen(output, "w"); if (!f) return;
    unsigned n = atomic_load(&count);
    fprintf(f, "{\"display_sync_calls\":[%u,%u],\"force_drawables\":%d,\"force_sync\":%d,\"frames\":[",
            sync_calls[0], sync_calls[1], force_drawables, force_sync);
    for (unsigned i = 0; i < n; i++)
        fprintf(f, "%s[%.6f,%.6f,%.6f,%d,%.6f,%.6f,%.6f,%.6f,%d]", i ? "," : "", frames[i].swap_begin, frames[i].swap_end,
                frames[i].wait, frames[i].waits, frames[i].presented, frames[i].gpu_start, frames[i].gpu_end,
                frames[i].gpu_busy, frames[i].buffers);
    fprintf(f, "]}\n"); fclose(f);
}
