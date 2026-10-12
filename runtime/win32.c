/* Native implementations of the Windows services used by the recompiled DLLs.
 * Unknown APIs stop with their name and call site. They never return success.
 * This runtime currently supports a single guest thread; TLS is thread-local.
 */
#include "win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <iconv.h>
#include <locale.h>
#include <xlocale.h>
#include <wctype.h>
#include <fcntl.h>
#include <limits.h>
#include <CoreFoundation/CoreFoundation.h>
#include "nls_tables.h"

extern const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[];
extern const unsigned rt_imptab_n;
extern uint8_t *rt_test_mem(void);
extern int rt_cpuid_sse2;
static int deterministic, trace_enabled;
static uint32_t heap_next = BHL_HEAP_BASE;
typedef struct Block { uint32_t address, size, capacity, heap; int used; struct Block *next; } Block;
static Block *blocks;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t last_error, tls_values[1088];
static unsigned char tls_used[1088];
static uint32_t fls_destructors[1088];
static uint32_t exception_filter;
static unsigned disabled_thread_notifications;
static unsigned heaps[256] = {1};
static uint32_t virtual_next=0x80000000u;
static uint32_t command_a, command_w, environment_w;
typedef struct Lock { uint32_t address; pthread_mutex_t mutex; struct Lock *next; } Lock;
static Lock *locks;
typedef struct Thunk { char *name; void (*fn)(Ctx *); } Thunk;
static Thunk dynamic[8192];
static unsigned dynamic_count;
static void (*import_observer)(Ctx *, uint32_t, int);
void bhl_set_import_observer(void (*fn)(Ctx *, uint32_t, int)) { import_observer = fn; }
#define A(n) rt_r32(G_MEM, c->esp + 4 + 4 * (n))
#define RETURN(value, nargs) do { c->eax = (uint32_t)(value); c->esp += 4 + 4 * (nargs); return; } while (0)

void bhl_set_deterministic(int enabled) { deterministic = enabled; }
void bhl_set_trace(int enabled) { trace_enabled = enabled; }
uint32_t bhl_heap_used(void) { return heap_next - BHL_HEAP_BASE; }
static Block *block_at(uint32_t p) { for (Block *b = blocks; b; b = b->next) if (b->used && b->address == p) return b; return NULL; }
uint32_t bhl_heap_alloc(uint32_t bytes, int zero)
{
    uint64_t cap64 = ((uint64_t)bytes + 15) & ~15ull;
    if (!cap64) cap64 = 16;
    if (cap64 > BHL_HEAP_END - BHL_HEAP_BASE) { last_error = 8; return 0; }
    uint32_t cap = (uint32_t)cap64;
    pthread_mutex_lock(&heap_lock);
    Block *b;
    for (b = blocks; b; b = b->next) if (!b->used && b->capacity >= cap) break;
    if (!b) {
        if (cap > BHL_HEAP_END - heap_next || !(b = calloc(1, sizeof *b))) {
            pthread_mutex_unlock(&heap_lock); last_error = 8; return 0;
        }
        b->address = heap_next; b->capacity = cap; heap_next += cap;
        b->next = blocks; blocks = b;
    }
    b->size = bytes; b->used = 1; b->heap=0x01000000;
    if (zero) memset(G_MEM + b->address, 0, bytes);
    uint32_t p = b->address;
    pthread_mutex_unlock(&heap_lock);
    return p;
}
static int heap_free(uint32_t p)
{
    if (!p) return 1;
    pthread_mutex_lock(&heap_lock);
    Block *b = block_at(p); if (b) b->used = 0;
    pthread_mutex_unlock(&heap_lock);
    if (!b) last_error = 87;
    return b != NULL;
}
int bhl_heap_free(uint32_t p) { return heap_free(p); }
static Lock *lock_at(uint32_t p) { for (Lock *l = locks; l; l = l->next) if (l->address == p) return l; return NULL; }
static void missing(Ctx *c, const char *name)
{
    static __thread char message[256];
    snprintf(message, sizeof message, "unimplemented Windows behavior: %s", name);
    rt_unhandled(c, rt_r32(G_MEM, c->esp), message);
}
const char *bhl_import_name(uint32_t index)
{
    if (index < rt_imptab_n) return rt_imptab[index].name;
    index -= rt_imptab_n;
    return index < dynamic_count ? dynamic[index].name : "invalid import index";
}
uint32_t bhl_thunk(const char *name, void (*fn)(Ctx *))
{
    for (unsigned i = 0; i < rt_imptab_n; i++) if (!strcmp(rt_imptab[i].name, name) && !fn) return BHL_THUNK_BASE + 16 * i;
    for (unsigned i = 0; i < dynamic_count; i++) if (!strcmp(dynamic[i].name, name)) return BHL_THUNK_BASE + 16 * (i + rt_imptab_n);
    if (dynamic_count == sizeof dynamic / sizeof *dynamic) return 0;
    unsigned i = dynamic_count++;
    dynamic[i] = (Thunk){strdup(name), fn};
    return BHL_THUNK_BASE + 16 * (i + rt_imptab_n);
}
static int wide_ascii(uint32_t ptr, char *dst, size_t cap)
{
    if (!ptr || cap == 0) return 0;
    for (size_t i = 0; i < cap; i++) {
        uint16_t v = rt_r16(G_MEM, ptr + 2 * i);
        if (v > 127) return 0;
        dst[i] = (char)v;
        if (!v) return 1;
    }
    dst[cap - 1] = 0; return 0;
}
static uint32_t module_handle(const char *name)
{
    const char *base=name;for(const char *p=name;*p;p++)if(*p=='/'||*p=='\\')base=p+1;name=base;
    if (!strcasecmp(name, "kernel32.dll") || !strcasecmp(name, "kernel32")) return 0x7c800000;
    if (!strcasecmp(name, "hl.dll")) return 0x10000000;
    if (!strcasecmp(name, "client.dll")) return 0x30000000;
    if (!strcasecmp(name, "vgui.dll")) return 0x40000000;
    if (!strcasecmp(name, "fmod.dll")) return 0x50000000;
    return 0;
}
static const char *codepage(uint32_t cp)
{
    switch(cp) { case 0: case 1252:return "WINDOWS-1252";case 1:case 437:return "CP437";
    case 65001:return "UTF-8";case 932:return "CP932";case 936:return "CP936";
    case 949:return "CP949";case 950:return "CP950";default:return NULL; }
}
static uint32_t convert_text(Ctx *c,int wide)
{
    uint32_t cp=A(0),flags=A(1),src=A(2),dst=A(4),cap=A(5);int32_t count=(int32_t)A(3);
    const char *encoding=codepage(cp);
    if(!encoding||!src||!count||count< -1||(int32_t)cap<0||(!dst&&cap)) {last_error=87;return 0;}
    if(flags & ~(wide ? 0x6f0u : 9u)) { last_error=1004; return 0; }
    if(wide && cp==65001 && ((flags&~0x80u)||A(6)||A(7))) {last_error=(flags&~0x80u)?1004:87;return 0;}
    if(wide && cp!=65001 && (flags&0x80)) {last_error=1004;return 0;}
    if(src==dst&&cap){last_error=87;return 0;}
    size_t bytes;
    if(count==-1) { if(wide){bytes=0;while(rt_r16(G_MEM,src+(uint32_t)bytes))bytes+=2;bytes+=2;}
                   else bytes=strlen((char*)G_MEM+src)+1; }
    else bytes=(size_t)count*(wide?2:1);
    /* Windows maps the five undefined CP1252 bytes to C1 controls. iconv
       rejects those bytes, which breaks the CRT's 256-character table. */
    if(!wide&&(cp==0||cp==1252)) {
        if(cap&&bytes>cap){last_error=122;return 0;}
        if(flags&2){missing(c,"MB_COMPOSITE conversion");return 0;}
        if(cap)for(size_t i=0;i<bytes;i++)rt_w16(G_MEM,dst+2*i,cp1252_decode[G_MEM[src+i]]);
        return (uint32_t)bytes;
    }
    if(wide&&(cp==0||cp==1252)) {
        CFMutableStringRef normalized=NULL;
        const UniChar *input=(const UniChar *)(G_MEM+src);
        size_t length=bytes/2;
        UniChar *temporary=NULL;
        if(flags&0x200){
            /* The CRT requests WC_COMPOSITECHECK | WC_SEPCHARS. Use actual
               canonical composition, then the published Windows best-fit map. */
            if(flags&0x50){missing(c,"WC_DISCARDNS/WC_DEFAULTCHAR composition");return 0;}
            normalized=CFStringCreateMutable(kCFAllocatorDefault,0);
            CFStringAppendCharacters(normalized,input,length);
            CFStringNormalize(normalized,kCFStringNormalizationFormC);
            length=CFStringGetLength(normalized);
            temporary=malloc((length?length:1)*sizeof *temporary);
            if(!temporary){CFRelease(normalized);last_error=8;return 0;}
            CFStringGetCharacters(normalized,CFRangeMake(0,length),temporary);input=temporary;
        }
        uint8_t *converted=malloc(length?length:1);
        if(!converted){free(temporary);if(normalized)CFRelease(normalized);last_error=8;return 0;}
        int used_default=0;uint8_t fallback=A(6)?G_MEM[A(6)]:'?';
        for(size_t i=0;i<length;i++){
            uint16_t w=input[i];size_t lo=0,hi=sizeof cp1252_encode/sizeof *cp1252_encode;
            while(lo<hi){size_t mid=lo+(hi-lo)/2;if(cp1252_encode[mid].unicode<w)lo=mid+1;else hi=mid;}
            int found=lo<sizeof cp1252_encode/sizeof *cp1252_encode&&cp1252_encode[lo].unicode==w;
            uint8_t b=found?cp1252_encode[lo].byte:fallback;
            if(found&&(flags&0x400)&&cp1252_decode[b]!=w)found=0;
            converted[i]=found?b:fallback;if(!found)used_default=1;
        }
        if(A(7))rt_w32(G_MEM,A(7),used_default);
        uint32_t result=(uint32_t)length;
        if(cap&&length>cap){last_error=122;result=0;}else if(cap)memcpy(G_MEM+dst,converted,length);
        free(converted);free(temporary);if(normalized)CFRelease(normalized);return result;
    }
    size_t buffer_size=bytes*4+8,left=buffer_size,inleft=bytes;
    char *buffer=malloc(buffer_size),*out=buffer,*in=(char*)G_MEM+src;
    if(!buffer) {last_error=8;return 0;}
    iconv_t cd=iconv_open(wide?encoding:"UTF-16LE",wide?"UTF-16LE":encoding);
    if(cd==(iconv_t)-1) {free(buffer);last_error=87;return 0;}
    size_t result=iconv(cd,&in,&inleft,&out,&left);iconv_close(cd);
    if(result==(size_t)-1) {free(buffer);last_error=1113;return 0;}
    size_t n=buffer_size-left,units=wide?n:n/2;
    if(cap&&units>cap){free(buffer);last_error=122;return 0;}
    if(cap)memcpy(G_MEM+dst,buffer,n);
    if(wide&&A(7))rt_w32(G_MEM,A(7),0);
    free(buffer);return (uint32_t)units;
}
typedef struct FileHandle { int used,fd; uint32_t access,share; char path[4096]; } FileHandle;
static FileHandle files[256];
static uint32_t file_error(int e)
{
    switch(e){case ENOENT:return 2;case ENOTDIR:return 3;case EACCES:case EPERM:return 5;
    case EBADF:return 6;case ENOMEM:return 8;case EEXIST:return 183;case ENOSPC:return 112;
    case EPIPE:return 109;case EINVAL:return 87;default:return 31;}
}
static int host_fd(uint32_t h)
{
    if(h>=0x100&&h<=0x102)return (int)(0x102-h);
    h-=0x1000;return h<256&&files[h].used?files[h].fd:-1;
}
static int guest_path(uint32_t p,int wide,char dest[4096])
{
    char path[4096];
    if(!p){last_error=87;return 0;}
    if(wide){
        size_t bytes=0;while(bytes<65536&&rt_r16(G_MEM,p+(uint32_t)bytes))bytes+=2;
        if(bytes>=65536){last_error=206;return 0;}
        char *in=(char*)G_MEM+p,*out=path;size_t left=sizeof path-1;
        iconv_t cd=iconv_open("UTF-8","UTF-16LE");if(cd==(iconv_t)-1)abort();
        size_t result=iconv(cd,&in,&bytes,&out,&left);iconv_close(cd);
        if(result==(size_t)-1){last_error=1113;return 0;}*out=0;
    }else{size_t n=strnlen((char*)G_MEM+p,sizeof path);if(n==sizeof path){last_error=206;return 0;}memcpy(path,G_MEM+p,n+1);}
    for(char *q=path;*q;q++)if(*q=='\\')*q='/';
    const char *relative=path;
    if(path[0]&&path[1]==':'){if(path[0]!='C'&&path[0]!='c'){last_error=3;return 0;}relative=path+2;while(*relative=='/')relative++;}
    char root[4096],candidate[4096],parent[4096],resolved[4096];if(!getcwd(root,sizeof root))abort();
    if(relative[0]=='/')snprintf(candidate,sizeof candidate,"%s",relative);
    else if(snprintf(candidate,sizeof candidate,"%s/%s",root,relative)>=(int)sizeof candidate){last_error=206;return 0;}
    if(realpath(candidate,resolved)){
        size_t n=strlen(root);if(strncmp(resolved,root,n)||(resolved[n]&&resolved[n]!='/')){last_error=5;return 0;}
        strcpy(dest,resolved);return 1;
    }
    /* For a new file, validate its existing parent before creating anything. */
    strcpy(parent,candidate);char *slash=strrchr(parent,'/');if(!slash){last_error=3;return 0;}*slash=0;
    if(!realpath(parent,resolved)){last_error=file_error(errno);return 0;}
    size_t n=strlen(root);if(strncmp(resolved,root,n)||(resolved[n]&&resolved[n]!='/')){last_error=5;return 0;}
    const char *leaf=strrchr(candidate,'/')+1;
    if(!strcmp(leaf,"..")||!strcmp(leaf,".")){last_error=5;return 0;}
    if(snprintf(dest,4096,"%s/%s",resolved,leaf)>=4096){last_error=206;return 0;}return 1;
}
void bhl_process_init(Ctx *c)
{
    if (!rt_test_mem()) abort();
    memset(c, 0, sizeof *c);
    c->esp = BHL_STACK_BASE + BHL_STACK_SIZE - 0x100;
    c->fs_base = BHL_TEB; c->fcw = 0x27f; c->mxcsr = 0x1f80; c->f.eflags = 0x202;
    memset(G_MEM + BHL_TEB, 0, 0x2000);
    rt_w32(G_MEM, BHL_TEB, 0xffffffff);
    rt_w32(G_MEM, BHL_TEB + 4, BHL_STACK_BASE + BHL_STACK_SIZE);
    rt_w32(G_MEM, BHL_TEB + 8, BHL_STACK_BASE);
    rt_w32(G_MEM, BHL_TEB + 0x18, BHL_TEB);
    rt_w32(G_MEM, BHL_TEB + 0x20, deterministic ? 100 : (uint32_t)getpid());
    rt_w32(G_MEM, BHL_TEB + 0x24, 1);
    rt_w32(G_MEM, BHL_TEB + 0x30, BHL_TEB + 0x1000);
    rt_w32(G_MEM, BHL_TEB + 0x1018, 0x01000000); /* process heap handle */
    if (!command_a) {
        command_a = bhl_heap_alloc(16, 1); memcpy(G_MEM + command_a, "bhl-native", 11);
        command_w = bhl_heap_alloc(32, 1);
        for (unsigned i = 0; i < 11; i++) rt_w16(G_MEM, command_w + 2 * i, G_MEM[command_a + i]);
        environment_w = bhl_heap_alloc(4, 1); /* isolated process has no guest environment entries */
    }
    rt_cpuid_sse2 = 1;
    for (unsigned i = 0; i < rt_imptab_n; i++) {
        uint32_t target=BHL_THUNK_BASE+i*16;
        if(!strcasecmp(rt_imptab[i].name,"MSVCRT.dll!_adjust_fdiv"))target=bhl_heap_alloc(4,1);
        else if(!strcasecmp(rt_imptab[i].name,"MSVCRT.dll!__mb_cur_max")){target=bhl_heap_alloc(4,1);rt_w32(G_MEM,target,1);}
        rt_w32(G_MEM,rt_imptab[i].iat,target);
    }
    /* Function thunks are dispatch tokens, not readable zero-filled data.
       This also exposes missing data-import adapters instead of hiding them. */
    if(mprotect(G_MEM+BHL_THUNK_BASE,0x100000,PROT_NONE))abort();
}

static void dispatch_import(Ctx *c, uint32_t index)
{
    const char *qualified = bhl_import_name(index);
    if (trace_enabled) { fprintf(stderr, "import %08x %s(%08x,%08x,%08x,%08x)\n", rt_r32(G_MEM,c->esp), qualified, A(0), A(1), A(2), A(3)); fflush(stderr); }
    if (index >= rt_imptab_n && index - rt_imptab_n < dynamic_count && dynamic[index - rt_imptab_n].fn) {
        dynamic[index - rt_imptab_n].fn(c); return;
    }
    const char *name = strchr(qualified, '!');
    extern int bhl_platform_import(Ctx *,const char *);
    if(bhl_platform_import(c,qualified))return;
    if(!strcmp(qualified,"USER32.dll!SystemParametersInfoA")&&(A(0)==3||A(0)==4)){
        /* Windows global mouse acceleration settings have no per-process Mac
           equivalent. The original client checks this failure and uses its
           SDL/raw input path. Never change the user's macOS mouse settings. */
        last_error=120;RETURN(0,4);
    }
    if(name&&!strncasecmp(qualified,"MSVCRT.dll!",11)){
        name++;
#define CRT(n) (!strcmp(name,n))
        if(CRT("malloc")||CRT("??2@YAPAXI@Z"))RETURN(bhl_heap_alloc(A(0),0),0);
        if(CRT("free")||CRT("??3@YAXPAX@Z")){heap_free(A(0));RETURN(0,0);}
        if(CRT("memcpy")||CRT("memmove")){memmove(G_MEM+A(0),G_MEM+A(1),A(2));RETURN(A(0),0);}
        if(CRT("memset")){memset(G_MEM+A(0),(unsigned char)A(1),A(2));RETURN(A(0),0);}
        if(CRT("memcmp"))RETURN(memcmp(G_MEM+A(0),G_MEM+A(1),A(2)),0);
        if(CRT("strlen"))RETURN(strlen((char*)G_MEM+A(0)),0);
        if(CRT("strcmp"))RETURN(strcmp((char*)G_MEM+A(0),(char*)G_MEM+A(1)),0);
        if(CRT("strcpy")){strcpy((char*)G_MEM+A(0),(char*)G_MEM+A(1));RETURN(A(0),0);}
        if(CRT("_initterm")){
            uint32_t first=A(0),last=A(1);
            if(last<first||last-first>0x10000||(last-first)%4){missing(c,"invalid CRT constructor range");return;}
            for(uint32_t p=first;p<last;p+=4){uint32_t fn=rt_r32(G_MEM,p);if(fn){Ctx child=*c;child.esp-=4;rt_w32(G_MEM,child.esp,0x00100000);rt_call(&child,fn);}}
            RETURN(0,0);
        }
        missing(c,qualified);return;
    }
    if(name && (!strncasecmp(qualified,"VGUI.dll!",9)||!strncasecmp(qualified,"fmod.dll!",9))){
        extern uint32_t bhl_export(const char *,const char *);
        uint32_t target=bhl_export(!strncasecmp(qualified,"VGUI",4)?"vgui.dll":"fmod.dll",name+1);
        if(!target){missing(c,qualified);return;}rt_call(c,target);return;
    }
    if (!name || strncasecmp(qualified, "KERNEL32.dll!", 13)) { missing(c, qualified); return; }
    name++;
#define IS(n) (!strcmp(name, n))
    if (IS("GetLastError")) RETURN(last_error, 0);
    if (IS("SetLastError")) { last_error = A(0); rt_w32(G_MEM, c->fs_base + 0x34, last_error); RETURN(0, 1); }
    if (IS("GetCurrentProcess")) RETURN(0xffffffff, 0);
    if (IS("DisableThreadLibraryCalls")) {
        uint32_t h=A(0);unsigned bit=h==0x10000000?1:h==0x30000000?2:h==0x50000000?4:h==0x40000000?8:0;
        if(!bit){last_error=126;RETURN(0,1);}disabled_thread_notifications|=bit;RETURN(1,1);
    }
    if (IS("GetCurrentProcessId")) RETURN(deterministic ? 100 : getpid(), 0);
    if (IS("GetCurrentThreadId")) RETURN(rt_r32(G_MEM, c->fs_base + 0x24), 0);
    if (IS("IsDebuggerPresent")) RETURN(0, 0);
    if (IS("IsProcessorFeaturePresent")) {
        /* MMX, CMPXCHG8B, SSE, SSE2; never advertise SSSE3/AVX. */
        uint32_t f = A(0); RETURN(f == 2 || f == 3 || f == 6 || f == 10, 1);
    }
    if (IS("GetSystemTimeAsFileTime")) {
        struct timeval tv; gettimeofday(&tv, NULL);
        uint64_t t = deterministic ? 133700000000000000ull : ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000 + tv.tv_usec * 10;
        rt_w64(G_MEM, A(0), t); RETURN(0, 1);
    }
    if (IS("QueryPerformanceCounter")) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        rt_w64(G_MEM, A(0), deterministic ? 123456789ull : (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec); RETURN(1, 1);
    }
    if (IS("QueryPerformanceFrequency")) { rt_w64(G_MEM, A(0), 1000000000); RETURN(1, 1); }
    if (IS("EncodePointer") || IS("DecodePointer")) RETURN(A(0) ^ 0x79a4c32d, 1);
    if (IS("SetUnhandledExceptionFilter")) { uint32_t old = exception_filter; exception_filter = A(0); RETURN(old, 1); }
    if (IS("InitializeSListHead")) { rt_w64(G_MEM, A(0), 0); RETURN(0, 1); }
    if (IS("InterlockedFlushSList")) {
        uint32_t p = A(0), old = rt_r32(G_MEM, p); uint64_t value = rt_r64(G_MEM, p);
        rt_w64(G_MEM, p, ((value >> 48) + 1) << 48); RETURN(old, 1);
    }
    if (IS("InitializeCriticalSectionAndSpinCount") || IS("InitializeCriticalSectionEx") || IS("InitializeCriticalSection")) {
        unsigned nargs=IS("InitializeCriticalSectionEx")?3:IS("InitializeCriticalSection")?1:2;
        uint32_t p = A(0); if (!p || lock_at(p)) { last_error = 87; RETURN(0,nargs); }
        Lock *l = calloc(1, sizeof *l); if (!l) { last_error = 8; RETURN(0,nargs); }
        pthread_mutexattr_t attr; pthread_mutexattr_init(&attr); pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        int result = pthread_mutex_init(&l->mutex, &attr); pthread_mutexattr_destroy(&attr);
        if (result) { free(l); last_error = 8; RETURN(0,nargs); }
        l->address = p; l->next = locks; locks = l;
        memset(G_MEM + p, 0, 24); rt_w32(G_MEM,p+4,0xffffffff); rt_w32(G_MEM,p+20,nargs>1?A(1):0);
        RETURN(1,nargs);
    }
    if (IS("EnterCriticalSection") || IS("LeaveCriticalSection") || IS("DeleteCriticalSection")) {
        Lock *l = lock_at(A(0)); if (!l) { missing(c, "uninitialized critical section"); return; }
        int result;
        uint32_t address=l->address;
        if (IS("EnterCriticalSection")) {
            result = pthread_mutex_lock(&l->mutex);
            if(!result){rt_w32(G_MEM,address+4,rt_r32(G_MEM,address+4)+1);rt_w32(G_MEM,address+8,rt_r32(G_MEM,address+8)+1);rt_w32(G_MEM,address+12,rt_r32(G_MEM,c->fs_base+0x24));}
        }
        else if (IS("LeaveCriticalSection")) {
            uint32_t depth=rt_r32(G_MEM,address+8);
            if(!depth||rt_r32(G_MEM,address+12)!=rt_r32(G_MEM,c->fs_base+0x24)){missing(c,"critical section not owned by current thread");return;}
            rt_w32(G_MEM,address+4,rt_r32(G_MEM,address+4)-1);rt_w32(G_MEM,address+8,depth-1);
            if(depth==1)rt_w32(G_MEM,address+12,0);
            result = pthread_mutex_unlock(&l->mutex);
        }
        else {
            result = pthread_mutex_destroy(&l->mutex);
            if (!result) { Lock **p = &locks; while (*p != l) p = &(*p)->next; *p = l->next; free(l); }
        }
        if (result) missing(c, "critical section operation failed");
        RETURN(0, 1);
    }
    if (IS("TlsAlloc") || IS("FlsAlloc")) {
        for (unsigned i=0;i<1088;i++) if (!tls_used[i]) { tls_used[i]=1; tls_values[i]=0; fls_destructors[i]=IS("FlsAlloc")?A(0):0; RETURN(i, IS("FlsAlloc") ? 1 : 0); }
        last_error=8; RETURN(0xffffffff, IS("FlsAlloc") ? 1 : 0);
    }
    if (IS("TlsGetValue") || IS("FlsGetValue")) { uint32_t i=A(0); last_error=i<1088&&tls_used[i]?0:87; RETURN(last_error?0:tls_values[i],1); }
    if (IS("TlsSetValue") || IS("FlsSetValue")) { uint32_t i=A(0); if(i>=1088||!tls_used[i]) {last_error=87;RETURN(0,2);} tls_values[i]=A(1); RETURN(1,2); }
    if (IS("TlsFree") || IS("FlsFree")) {
        uint32_t i=A(0); if(i>=1088||!tls_used[i]) {last_error=87;RETURN(0,1);}
        if(IS("FlsFree") && fls_destructors[i] && tls_values[i]) {
            Ctx child=*c; child.esp-=8;
            rt_w32(G_MEM,child.esp,0x00100000);rt_w32(G_MEM,child.esp+4,tls_values[i]);
            tls_values[i]=0;rt_call(&child,fls_destructors[i]);
        }
        tls_used[i]=0;tls_values[i]=0;fls_destructors[i]=0;RETURN(1,1);
    }
    if (IS("GetProcessHeap")) RETURN(0x01000000, 0);
    if (IS("HeapCreate")) {
        if(A(0)&~1u||A(2)){missing(c,"HeapCreate flags/maximum size");return;}
        for(unsigned i=1;i<256;i++)if(!heaps[i]){heaps[i]=1;RETURN(0x01000000+i,3);}last_error=8;RETURN(0,3);
    }
    if (IS("HeapDestroy")) {
        uint32_t h=A(0);if(h<=0x01000000||h>=0x01000100||!heaps[h-0x01000000]){last_error=6;RETURN(0,1);}
        for(Block *b=blocks;b;b=b->next)if(b->heap==h)b->used=0;heaps[h-0x01000000]=0;RETURN(1,1);
    }
    if (IS("GetVersionExA")) {
        uint32_t p=A(0),size=rt_r32(G_MEM,p);if(size!=148&&size!=156){last_error=87;RETURN(0,1);}
        memset(G_MEM+p+4,0,size-4);rt_w32(G_MEM,p+4,6);rt_w32(G_MEM,p+8,1);rt_w32(G_MEM,p+12,7601);rt_w32(G_MEM,p+16,2);
        memcpy(G_MEM+p+20,"Service Pack 1",15);if(size==156){rt_w16(G_MEM,p+148,1);G_MEM[p+154]=1;}RETURN(1,1);
    }
    if (IS("GetVersion")) RETURN(0x1db10106,0); /* Windows 6.1 compatibility API surface */
    if (IS("SetHandleCount")) RETURN(A(0),1);
    if (IS("InterlockedIncrement")||IS("InterlockedDecrement")) {
        uint32_t p=A(0);rt_bus_lock();uint32_t v=rt_r32(G_MEM,p)+(IS("InterlockedIncrement")?1u:0xffffffffu);rt_w32(G_MEM,p,v);rt_bus_unlock();RETURN(v,1);
    }
    if (IS("VirtualAlloc")) {
        uint32_t p=A(0),size=A(1),type=A(2),protect=A(3);
        if(!size||size>0x10000000||protect!=4||(type&~0x3000u)){missing(c,"VirtualAlloc mode");return;}
        uint32_t rounded=(size+65535)&~65535u;
        if(!p){if(rounded>0xf0000000u-virtual_next){last_error=8;RETURN(0,4);}p=virtual_next;virtual_next+=rounded;memset(G_MEM+p,0,rounded);}
        else if(p<0x80000000u||(uint64_t)p+size>virtual_next){last_error=487;RETURN(0,4);}
        RETURN(p,4);
    }
    if (IS("HeapAlloc")) {
        if (A(0)<0x01000000 || A(0)>=0x01000100 || !heaps[A(0)-0x01000000] || (A(1)&~9u)) { missing(c,"HeapAlloc flags/heap"); return; }
        uint32_t p=bhl_heap_alloc(A(2),A(1)&8);if(p)block_at(p)->heap=A(0);RETURN(p,3);
    }
    if (IS("HeapFree")) { if(A(0)<0x01000000||A(0)>=0x01000100||!heaps[A(0)-0x01000000]||(A(1)&~1u)) {missing(c,"HeapFree flags/heap");return;} Block *b=block_at(A(2));if(A(2)&&(!b||b->heap!=A(0))){last_error=87;RETURN(0,3);}RETURN(heap_free(A(2)),3); }
    if (IS("HeapSize")) { Block *b=block_at(A(2)); if(!b||b->heap!=A(0)){last_error=87;RETURN(0xffffffff,3);}RETURN(b->size,3); }
    if (IS("HeapReAlloc")) {
        if(A(0)<0x01000000||A(0)>=0x01000100||!heaps[A(0)-0x01000000]||(A(1)&~25u)) {missing(c,"HeapReAlloc flags/heap");return;}
        Block *b=block_at(A(2)); if(!b||b->heap!=A(0)) {last_error=87;RETURN(0,4);}
        uint32_t size=A(3),old=b->size;
        if(size<=b->capacity) {if((A(1)&8)&&size>old)memset(G_MEM+b->address+old,0,size-old);b->size=size;RETURN(b->address,4);}
        if(A(1)&16) {last_error=8;RETURN(0,4);}
        uint32_t p=bhl_heap_alloc(size,A(1)&8);if(p){block_at(p)->heap=A(0);memcpy(G_MEM+p,G_MEM+b->address,old);heap_free(b->address);} RETURN(p,4);
    }
    if (IS("GetModuleHandleW") || IS("LoadLibraryExW")) {
        if (!A(0)) RETURN(0x00400000, IS("LoadLibraryExW")?3:1);
        char path[512]; if(!wide_ascii(A(0),path,sizeof path)) {missing(c,"Unicode module path");return;}
        uint32_t h=module_handle(path);if(!h)last_error=126;
        if(trace_enabled)fprintf(stderr,"  module %s -> %08x\n",path,h);
        RETURN(h,IS("LoadLibraryExW")?3:1);
    }
    if (IS("GetModuleHandleExW")) {
        uint32_t flags=A(0),arg=A(1),p=A(2),h=0;
        if(flags&4) { if(arg>=0x10000000&&arg<0x10280000)h=0x10000000;else if(arg>=0x30000000&&arg<0x30200000)h=0x30000000; }
        else if(!arg)h=0x00400000;
        else {char namebuf[512];if(!wide_ascii(arg,namebuf,sizeof namebuf)){missing(c,"Unicode module name");return;}h=module_handle(namebuf);}
        if(!h||!p){last_error=126;RETURN(0,3);}rt_w32(G_MEM,p,h);RETURN(1,3);
    }
    if (IS("GetModuleHandleA")||IS("LoadLibraryA")) {
        if(!A(0)){if(IS("GetModuleHandleA"))RETURN(0x00400000,1);last_error=87;RETURN(0,1);}
        uint32_t h=module_handle((char*)G_MEM+A(0));if(!h)last_error=126;
        if(trace_enabled)fprintf(stderr,"  module %s -> %08x\n",G_MEM+A(0),h);
        if(h&&h!=0x7c800000&&IS("LoadLibraryA")){
            extern void bhl_attach(const char *);const char *module=h==0x10000000?"hl.dll":h==0x30000000?"client.dll":h==0x40000000?"vgui.dll":"fmod.dll";
            bhl_attach(module);
        }
        RETURN(h,1);
    }
    if (IS("GetModuleFileNameW")||IS("GetModuleFileNameA")) {
        uint32_t h=A(0),p=A(1),cap=A(2);
        const char *path=h==0||h==0x00400000?"C:\\bhl\\bhl-native.exe":h==0x10000000?"C:\\bhl\\dlls\\hl.dll":h==0x30000000?"C:\\bhl\\cl_dlls\\client.dll":h==0x40000000?"C:\\bhl\\vgui.dll":h==0x50000000?"C:\\bhl\\fmod.dll":NULL;
        if(!path||!p||!cap){last_error=87;RETURN(0,3);}
        uint32_t n=(uint32_t)strlen(path),copy=n<cap?n:cap-1;
        if(IS("GetModuleFileNameW")){for(uint32_t i=0;i<copy;i++)rt_w16(G_MEM,p+2*i,path[i]);rt_w16(G_MEM,p+2*copy,0);}
        else{memcpy(G_MEM+p,path,copy);G_MEM[p+copy]=0;}
        if(n>=cap){last_error=122;RETURN(cap,3);}RETURN(n,3);
    }
    if (IS("GetProcAddress")) {
        if(A(1)<0x10000) {last_error=127;RETURN(0,2);}
        if(A(0)!=0x7c800000) {
            extern uint32_t bhl_export(const char *,const char *);
            uint32_t h=A(0);const char *module=h==0x10000000?"hl.dll":h==0x30000000?"client.dll":h==0x40000000?"vgui.dll":h==0x50000000?"fmod.dll":NULL;
            if(!module){last_error=126;RETURN(0,2);}uint32_t target=bhl_export(module,(char*)G_MEM+A(1));if(!target)last_error=127;RETURN(target,2);
        }
        char full[256]; snprintf(full,sizeof full,"KERNEL32.dll!%s",G_MEM+A(1));
        if(trace_enabled)fprintf(stderr,"  resolve %s\n",full);
        RETURN(bhl_thunk(full,NULL),2);
    }
    if (IS("FreeLibrary")) { if(A(0)!=0x7c800000) {last_error=6;RETURN(0,1);} RETURN(1,1); }
    if (IS("GetStartupInfoW")||IS("GetStartupInfoA")) { uint32_t p=A(0);memset(G_MEM+p,0,68);rt_w32(G_MEM,p,68); RETURN(0,1); }
    if (IS("GetCommandLineA")) RETURN(command_a,0);
    if (IS("GetCommandLineW")) RETURN(command_w,0);
    if (IS("GetEnvironmentStringsW")||IS("GetEnvironmentStrings")||IS("GetEnvironmentStringsA")) { uint32_t p=bhl_heap_alloc(4,1); RETURN(p,0); }
    if (IS("FreeEnvironmentStringsW")||IS("FreeEnvironmentStringsA")) RETURN(heap_free(A(0)),1);
    if (IS("IsValidCodePage")) RETURN(codepage(A(0)) != NULL && A(0)>1,1);
    if (IS("GetCPInfo")) {
        uint32_t cp=A(0),p=A(1);if(!codepage(cp)||!p){last_error=87;RETURN(0,2);}
        memset(G_MEM+p,0,20);rt_w32(G_MEM,p,cp==65001?4:(cp>=932&&cp<=950?2:1));
        G_MEM[p+4]='?';
        if(cp==932){G_MEM[p+6]=0x81;G_MEM[p+7]=0x9f;G_MEM[p+8]=0xe0;G_MEM[p+9]=0xfc;}
        else if(cp==936||cp==949||cp==950){G_MEM[p+6]=0x81;G_MEM[p+7]=0xfe;}
        RETURN(1,2);
    }
    if (IS("MultiByteToWideChar")) RETURN(convert_text(c,0),6);
    if (IS("WideCharToMultiByte")) RETURN(convert_text(c,1),8);
    if (IS("LCMapStringEx") || IS("LCMapStringW")) {
        uint32_t flags=A(1),src=A(2),dst=A(4),cap=A(5);int32_t count=(int32_t)A(3);
        if(flags!=0x100&&flags!=0x200){missing(c,"LCMapString flags");return;}
        if(!src||!count||count< -1||(!dst&&cap)){last_error=87;RETURN(0,IS("LCMapStringEx")?9:6);}
        if(count==-1){count=0;while(rt_r16(G_MEM,src+2*count))count++;count++;}
        if(cap&&(uint32_t)count>cap){last_error=122;RETURN(0,IS("LCMapStringEx")?9:6);}
        if(cap){locale_t loc=newlocale(LC_CTYPE_MASK,"en_US.UTF-8",NULL);
            for(int32_t i=0;i<count;i++){wint_t w=rt_r16(G_MEM,src+2*i);w=flags==0x100?towlower_l(w,loc):towupper_l(w,loc);rt_w16(G_MEM,dst+2*i,w);}
            freelocale(loc);}
        RETURN(count,IS("LCMapStringEx")?9:6);
    }
    if (IS("GetStringTypeW")) {
        if(A(0)!=1){missing(c,"GetStringTypeW non-CT_CTYPE1");return;}
        uint32_t src=A(1),dst=A(3);int32_t count=(int32_t)A(2);
        if(!src||!dst||!count||count< -1){last_error=87;RETURN(0,4);}
        if(count==-1){count=0;while(rt_r16(G_MEM,src+2*count))count++;count++;}
        locale_t loc=newlocale(LC_CTYPE_MASK,"en_US.UTF-8",NULL);
        for(int32_t i=0;i<count;i++){
            wint_t w=rt_r16(G_MEM,src+2*i);uint16_t bits=0;
            if(iswupper_l(w,loc))bits|=1;if(iswlower_l(w,loc))bits|=2;
            if(iswdigit_l(w,loc))bits|=4;if(iswspace_l(w,loc))bits|=8;
            if(iswpunct_l(w,loc))bits|=16;if(iswcntrl_l(w,loc))bits|=32;
            if(iswblank_l(w,loc))bits|=64;if(iswxdigit_l(w,loc))bits|=128;
            if(iswalpha_l(w,loc))bits|=256;if(iswprint_l(w,loc)||iswcntrl_l(w,loc))bits|=512;
            rt_w16(G_MEM,dst+2*i,bits);
        }
        freelocale(loc);RETURN(1,4);
    }
    if (IS("GetLocalTime")) {
        struct timeval tv;gettimeofday(&tv,NULL);
        time_t now=deterministic?(time_t)1700000000:tv.tv_sec;struct tm tm;localtime_r(&now,&tm);uint32_t p=A(0);
        uint16_t fields[]={tm.tm_year+1900,tm.tm_mon+1,tm.tm_wday,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,deterministic?0:tv.tv_usec/1000};
        memcpy(G_MEM+p,fields,sizeof fields);RETURN(0,1);
    }
    if (IS("CreateDirectoryA")) {
        if(A(1)){missing(c,"directory security attributes");return;}
        char path[4096];if(!guest_path(A(0),0,path))RETURN(0,2);
        int result=mkdir(path,0755);if(result)last_error=file_error(errno);RETURN(result==0,2);
    }
    if (IS("CreateFileW")||IS("CreateFileA")) {
        char path[4096];if(!guest_path(A(0),IS("CreateFileW"),path))RETURN(0xffffffff,7);
        uint32_t access=A(1),share=A(2),disposition=A(4),attributes=A(5);
        if(A(3)||A(6)||(attributes&0x40000000u)){missing(c,"CreateFile security/template/overlapped mode");return;}
        /* Normal/archive and sequential/random access are supported hints.
           Do not silently ignore delete-on-close, encryption or backup mode. */
        if(attributes&~0x180000a0u){missing(c,"CreateFile attributes");return;}
        if(access&~0xc0000000u||share&~7u){missing(c,"CreateFile access/share mode");return;}
        if((disposition==2||disposition==5)&&!(access&0x40000000u)){last_error=5;RETURN(0xffffffff,7);}
        int mode=(access&0x40000000)?((access&0x80000000)?O_RDWR:O_WRONLY):O_RDONLY;
        if(disposition==1)mode|=O_CREAT|O_EXCL;else if(disposition==2)mode|=O_CREAT|O_TRUNC;
        else if(disposition==4)mode|=O_CREAT;else if(disposition==5)mode|=O_TRUNC;else if(disposition!=3){last_error=87;RETURN(0xffffffff,7);}
        for(unsigned i=0;i<256;i++)if(files[i].used&&!strcmp(files[i].path,path)){
            if(((access&0x80000000)&&!(files[i].share&1))||((access&0x40000000)&&!(files[i].share&2))||
               ((files[i].access&0x80000000)&&!(share&1))||((files[i].access&0x40000000)&&!(share&2))){last_error=32;RETURN(0xffffffff,7);}
        }
        unsigned slot=0;while(slot<256&&files[slot].used)slot++;if(slot==256){last_error=4;RETURN(0xffffffff,7);}
        struct stat previous_stat;int existed=stat(path,&previous_stat)==0;
        int fd=open(path,mode,0644);if(fd<0){last_error=file_error(errno);RETURN(0xffffffff,7);}
        files[slot].used=1;files[slot].fd=fd;files[slot].access=access;files[slot].share=share;strcpy(files[slot].path,path);
        last_error=existed&&(disposition==2||disposition==4)?183:0;RETURN(0x1000+slot,7);
    }
    if (IS("ReadFile")||IS("WriteFile")) {
        int fd=host_fd(A(0));if(A(4)){missing(c,"overlapped file operation");return;}
        if(A(3))rt_w32(G_MEM,A(3),0);if(fd<0){last_error=6;RETURN(0,5);}
        ssize_t count;do{count=IS("ReadFile")?read(fd,G_MEM+A(1),A(2)):write(fd,G_MEM+A(1),A(2));}while(count<0&&errno==EINTR);
        if(count<0){last_error=file_error(errno);RETURN(0,5);}if(A(3))rt_w32(G_MEM,A(3),(uint32_t)count);RETURN(1,5);
    }
    if (IS("CloseHandle")) {
        uint32_t i=A(0)-0x1000;if(i>=256||!files[i].used){last_error=6;RETURN(0,1);}
        int result=close(files[i].fd);files[i].used=0;if(result)last_error=file_error(errno);RETURN(result==0,1);
    }
    if (IS("GetFileSizeEx")) {
        struct stat st;int fd=host_fd(A(0));if(fd<0||fstat(fd,&st)){last_error=fd<0?6:file_error(errno);RETURN(0,2);}
        rt_w64(G_MEM,A(1),(uint64_t)st.st_size);RETURN(1,2);
    }
    if (IS("SetFilePointerEx")) {
        int fd=host_fd(A(0));uint32_t method=A(4);if(fd<0||method>2){last_error=fd<0?6:87;RETURN(0,5);}
        int64_t distance=(int64_t)rt_r64(G_MEM,c->esp+8);off_t position=lseek(fd,distance,method==0?SEEK_SET:method==1?SEEK_CUR:SEEK_END);
        if(position<0){last_error=file_error(errno);RETURN(0,5);}if(A(3))rt_w64(G_MEM,A(3),(uint64_t)position);RETURN(1,5);
    }
    if (IS("FlushFileBuffers")||IS("SetEndOfFile")) {
        int fd=host_fd(A(0));if(fd<0){last_error=6;RETURN(0,1);}
        int result=IS("FlushFileBuffers")?fsync(fd):ftruncate(fd,lseek(fd,0,SEEK_CUR));if(result)last_error=file_error(errno);RETURN(result==0,1);
    }
    if (IS("GetACP")) RETURN(1252,0);
    if (IS("AreFileApisANSI")) RETURN(1,0);
    if (IS("GetOEMCP")) RETURN(437,0);
    if (IS("GetStdHandle")) { uint32_t n=A(0); if(n<0xfffffff4||n>0xfffffff6){last_error=87;RETURN(0xffffffff,1);} RETURN(0x100+n-0xfffffff4,1); }
    if (IS("GetFileType")) {struct stat st;int fd=host_fd(A(0));if(fd<0||fstat(fd,&st)){last_error=6;RETURN(0,1);}RETURN(S_ISCHR(st.st_mode)?2:S_ISFIFO(st.st_mode)||S_ISSOCK(st.st_mode)?3:1,1);}
    if (IS("GetConsoleMode")) { last_error=6;RETURN(0,2); } /* host process uses pipes, not a Windows console */
    if (IS("GetConsoleOutputCP")) RETURN(0,0);
    missing(c, qualified);
}

void rt_import(Ctx *c, uint32_t index)
{
    if(import_observer)import_observer(c,index,0);
    dispatch_import(c,index);
    if(import_observer)import_observer(c,index,1);
}
