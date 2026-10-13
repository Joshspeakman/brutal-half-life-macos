/* Native implementations of the Windows services used by the recompiled DLLs.
 * Unknown APIs stop with their name and call site. They never return success.
 * Native guest workers have independent stacks, TEBs and thread-local values.
 */
#include "win32.h"
#include "loader.h"
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
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <CoreFoundation/CoreFoundation.h>
#include "nls_tables.h"

extern const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[];
extern const unsigned rt_imptab_n;
extern uint8_t *rt_test_mem(void);
extern int rt_cpuid_sse2;
static int deterministic, trace_enabled;
static uint32_t heap_next = BHL_HEAP_BASE;
/* Physical neighbors allow coalescing; free bins and a live-address index keep
 * per-frame allocation independent of the number of model/entity mirrors. */
typedef struct Block {
    uint32_t address, size, capacity, heap; int used;
    struct Block *next, *prev, *free_next, *free_prev, *hash_next;
} Block;
static Block *blocks, *last_block, *free_bins[32], *live_blocks[4096];
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t last_error, tls_values[1088];
static __thread uint32_t tls_generations[1088];
static uint32_t slot_generations[1088];
static __thread unsigned com_initializations;
static unsigned char tls_used[1088];
static uint32_t fls_destructors[1088];
static pthread_mutex_t tls_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t exception_filter;
static unsigned disabled_thread_notifications;
void bhl_set_last_error(uint32_t error){last_error=error;}
int bhl_thread_notifications_enabled(uint32_t base)
{
    unsigned bit=base==0x10000000?1:base==0x30000000?2:base==0x50000000?4:base==0x40000000?8:0;
    return bit&&!(disabled_thread_notifications&bit);
}
static unsigned heaps[256] = {1};
static uint32_t virtual_next=0x80000000u;
/* Guest protection metadata uses Windows 4 KiB pages. The backing mapping
 * remains host-readable data: generated ARM64 lives outside this mapping.
 * This is not a hardware MMU and does not yet raise every guest access fault. */
static uint8_t page_protection[1u<<20];
typedef struct VirtualRegion {uint32_t base,size;} VirtualRegion;
static VirtualRegion regions[256];
static void protect_pages(uint32_t p,uint32_t size,uint8_t protection)
{
    uint64_t end=(uint64_t)p+size;
    for(uint64_t page=p>>12;page<(end+4095)/4096;page++)page_protection[page]=protection;
}
static uint32_t command_a, command_w, environment_w;
typedef struct Lock { uint32_t address; pthread_mutex_t mutex; struct Lock *next; } Lock;
static Lock *locks;
static pthread_mutex_t lock_registry=PTHREAD_MUTEX_INITIALIZER;
typedef struct Thunk { char *name; void (*fn)(Ctx *); } Thunk;
static Thunk dynamic[8192];
static _Atomic unsigned dynamic_count;
static pthread_mutex_t thunk_lock=PTHREAD_MUTEX_INITIALIZER;
static void (*import_observer)(Ctx *, uint32_t, int);
void bhl_set_import_observer(void (*fn)(Ctx *, uint32_t, int)) { import_observer = fn; }
#define A(n) rt_r32(G_MEM, c->esp + 4 + 4 * (n))
#define RETURN(value, nargs) do { c->eax = (uint32_t)(value); c->esp += 4 + 4 * (nargs); return; } while (0)
static char *gs(uint32_t p){return p?(char*)G_MEM+p:NULL;}
#include "guest_format.inc"

void bhl_set_deterministic(int enabled) { deterministic = enabled; }
void bhl_set_trace(int enabled) { trace_enabled = enabled; }
uint32_t bhl_heap_used(void) { return heap_next - BHL_HEAP_BASE; }
static unsigned heap_bin(uint32_t size){return 31u-__builtin_clz(size);}
static unsigned heap_hash(uint32_t address){return ((address>>4)*2654435761u)>>20;}
static Block *block_at_locked(uint32_t p)
{for(Block *b=live_blocks[heap_hash(p)];b;b=b->hash_next)if(b->address==p)return b;return NULL;}
static Block *block_at(uint32_t p)
{pthread_mutex_lock(&heap_lock);Block *b=block_at_locked(p);pthread_mutex_unlock(&heap_lock);return b;}
static void free_remove(Block *b)
{
    if(b->free_prev)b->free_prev->free_next=b->free_next;else free_bins[heap_bin(b->capacity)]=b->free_next;
    if(b->free_next)b->free_next->free_prev=b->free_prev;b->free_next=b->free_prev=NULL;
}
static void free_insert(Block *b)
{
    unsigned bin=heap_bin(b->capacity);b->free_next=free_bins[bin];b->free_prev=NULL;
    if(b->free_next)b->free_next->free_prev=b;free_bins[bin]=b;
}
uint32_t bhl_heap_alloc(uint32_t bytes, int zero)
{
    uint64_t cap64=((uint64_t)bytes+15)&~15ull;if(!cap64)cap64=16;
    if(cap64>BHL_HEAP_END-BHL_HEAP_BASE){last_error=8;return 0;}
    uint32_t cap=(uint32_t)cap64;pthread_mutex_lock(&heap_lock);Block *b=NULL;
    for(unsigned bin=heap_bin(cap);bin<32&&!b;bin++)
        for(Block *candidate=free_bins[bin];candidate;candidate=candidate->free_next)
            if(candidate->capacity>=cap&&(!b||candidate->capacity<b->capacity)){b=candidate;if(b->capacity==cap)break;}
    if(b){
        /* Never let a tiny request consume an entire freed model allocation. */
        if(b->capacity>cap){
            Block *tail=calloc(1,sizeof *tail);if(!tail)goto failed;
            tail->address=b->address+cap;tail->capacity=b->capacity-cap;tail->prev=b;tail->next=b->next;
            if(tail->next)tail->next->prev=tail;else last_block=tail;
            free_remove(b);b->next=tail;b->capacity=cap;free_insert(tail);
        }else free_remove(b);
    }else{
        if(cap>BHL_HEAP_END-heap_next||!(b=calloc(1,sizeof *b)))goto failed;
        b->address=heap_next;b->capacity=cap;heap_next+=cap;b->prev=last_block;
        if(last_block)last_block->next=b;else blocks=b;last_block=b;
    }
    b->size=bytes;b->used=1;b->heap=0x01000000;
    unsigned hash=heap_hash(b->address);b->hash_next=live_blocks[hash];live_blocks[hash]=b;
    if(zero)memset(G_MEM+b->address,0,bytes);uint32_t p=b->address;pthread_mutex_unlock(&heap_lock);return p;
failed:
    fprintf(stderr,"BHL heap exhausted: request=%u extent=%u\n",bytes,heap_next-BHL_HEAP_BASE);
    pthread_mutex_unlock(&heap_lock);last_error=8;return 0;
}
/* Caller owns heap_lock. Returns the coalesced span's next physical block. */
static Block *heap_release(Block *b)
{
    Block **at=&live_blocks[heap_hash(b->address)];while(*at!=b)at=&(*at)->hash_next;*at=b->hash_next;
    b->used=0;b->size=0;
    if(b->prev&&!b->prev->used){Block *previous=b->prev;free_remove(previous);previous->capacity+=b->capacity;previous->next=b->next;
        if(b->next)b->next->prev=previous;else last_block=previous;free(b);b=previous;}
    if(b->next&&!b->next->used){Block *next=b->next;free_remove(next);b->capacity+=next->capacity;b->next=next->next;
        if(b->next)b->next->prev=b;else last_block=b;free(next);}
    free_insert(b);return b->next;
}
static int heap_free(uint32_t p)
{
    if(!p)return 1;pthread_mutex_lock(&heap_lock);Block *b=block_at_locked(p);int found=b!=NULL;
    if(b)heap_release(b);pthread_mutex_unlock(&heap_lock);if(!found)last_error=87;return found;
}
int bhl_heap_free(uint32_t p){return heap_free(p);}
static Lock *lock_at(uint32_t p) { pthread_mutex_lock(&lock_registry);Lock *found=NULL;for (Lock *l = locks; l; l = l->next) if (l->address == p){found=l;break;}pthread_mutex_unlock(&lock_registry);return found; }
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
    pthread_mutex_lock(&thunk_lock);
    for (unsigned i = 0; i < dynamic_count; i++) if (!strcmp(dynamic[i].name, name)){uint32_t p=BHL_THUNK_BASE+16*(i+rt_imptab_n);pthread_mutex_unlock(&thunk_lock);return p;}
    if (dynamic_count == sizeof dynamic / sizeof *dynamic){pthread_mutex_unlock(&thunk_lock);return 0;}
    unsigned i = dynamic_count;
    dynamic[i] = (Thunk){strdup(name), fn};
    dynamic_count=i+1;pthread_mutex_unlock(&thunk_lock);
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
    if (!strcasecmp(name, "dsound.dll")) return 0x7c900000;
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
typedef struct StdioHandle {FILE *file;uint32_t handle;} StdioHandle;
static StdioHandle stdio_files[256];
static uint32_t next_stdio=0x06000000;
static pthread_mutex_t stdio_lock=PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t crt_errno;
static void set_crt_errno(int value)
{if(!crt_errno)crt_errno=bhl_heap_alloc(4,1);rt_w32(G_MEM,crt_errno,value);}
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
    }else{
        size_t bytes=strnlen((char*)G_MEM+p,sizeof path);if(bytes==sizeof path){last_error=206;return 0;}
        UniChar unicode[4096];for(size_t i=0;i<bytes;i++)unicode[i]=cp1252_decode[G_MEM[p+i]];
        CFStringRef text=CFStringCreateWithCharacters(NULL,unicode,bytes);
        int ok=CFStringGetCString(text,path,sizeof path,kCFStringEncodingUTF8);CFRelease(text);
        if(!ok){last_error=206;return 0;}
    }
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
/* Directory searches own their native stream, distinct from file handles.
   macOS has no DOS short-name namespace; cAlternateFileName remains empty. */
typedef struct FindHandle {
    DIR *directory;uint32_t handle;int directories_only,case_sensitive;
    UniChar pattern[1024];size_t length;
} FindHandle;
static FindHandle searches[256];
static uint32_t next_search=0x05000000;
static pthread_mutex_t search_lock=PTHREAD_MUTEX_INITIALIZER;
static size_t find_unicode(const char *text,UniChar *out,size_t capacity,int fold)
{
    CFStringRef source=CFStringCreateWithCString(NULL,text,kCFStringEncodingUTF8);
    if(!source)return SIZE_MAX;
    CFMutableStringRef s=CFStringCreateMutableCopy(NULL,0,source);CFRelease(source);
    CFStringNormalize(s,kCFStringNormalizationFormC);
    if(fold)CFStringFold(s,kCFCompareCaseInsensitive,NULL);
    size_t length=CFStringGetLength(s);
    if(length<capacity){CFStringGetCharacters(s,CFRangeMake(0,length),out);out[length]=0;}
    else length=SIZE_MAX;
    CFRelease(s);return length;
}
static int find_match(const UniChar *pattern,size_t m,const UniChar *name,size_t n)
{
    /* Win32's common DOS wildcard rules: *.* includes extensionless names,
       trailing .* is optional, and ? may match no character at a dot/end. */
    if(m==3&&pattern[0]=='*'&&pattern[1]=='.'&&pattern[2]=='*')m=1;
    unsigned char current[1024]={1},next[1024];
    for(size_t i=0;i<m;i++){
        memset(next,0,n+1);
        if(pattern[i]=='*'){
            next[0]=current[0];
            for(size_t j=1;j<=n;j++)next[j]=current[j]||next[j-1];
        }else for(size_t j=0;j<=n;j++)if(current[j]){
            if(j<n&&(pattern[i]=='?'||pattern[i]==name[j]))next[j+1]=1;
            if(pattern[i]=='?'&&(j==n||name[j]=='.'))next[j]=1;
            if(pattern[i]=='.'&&j==n&&(i+1==m||(i+2==m&&pattern[i+1]=='*')))next[j]=1;
        }
        memcpy(current,next,n+1);
    }
    return current[n];
}
static uint64_t find_time(struct timespec t)
{return ((uint64_t)t.tv_sec+11644473600ull)*10000000ull+t.tv_nsec/100;}
static void write_system_time(uint32_t p,const struct tm *tm,unsigned milliseconds)
{
    uint16_t fields[]={tm->tm_year+1900,tm->tm_mon+1,tm->tm_wday,tm->tm_mday,tm->tm_hour,tm->tm_min,tm->tm_sec,milliseconds};
    memcpy(G_MEM+p,fields,sizeof fields);
}
static int read_system_time(uint32_t p,time_t *seconds,unsigned *milliseconds)
{
    if(!p)return 0;uint16_t f[8];memcpy(f,G_MEM+p,sizeof f);
    if(f[0]<1601||f[0]>30827||f[1]<1||f[1]>12||f[3]<1||f[3]>31||f[4]>23||f[5]>59||f[6]>59||f[7]>999)return 0;
    struct tm tm={.tm_year=f[0]-1900,.tm_mon=f[1]-1,.tm_mday=f[3],.tm_hour=f[4],.tm_min=f[5],.tm_sec=f[6]};
    time_t result=timegm(&tm);
    if(tm.tm_mon!=f[1]-1||tm.tm_mday!=f[3])return 0;
    *seconds=result;*milliseconds=f[7];return 1;
}
static int zone_transition(uint32_t p,int year,int bias,time_t *result)
{
    uint16_t f[8];memcpy(f,G_MEM+p,sizeof f);
    if(f[0]&&f[0]!=year)return 0;
    if(f[1]<1||f[1]>12||f[4]>23||f[5]>59||f[6]>59)return 0;
    struct tm tm={.tm_year=year-1900,.tm_mon=f[1]-1,.tm_mday=1,.tm_hour=f[4],.tm_min=f[5],.tm_sec=f[6]};
    timegm(&tm);
    if(f[0])tm.tm_mday=f[3];
    else{
        if(f[2]>6||f[3]<1||f[3]>5)return 0;
        tm.tm_mday=1+(f[2]+7-tm.tm_wday)%7+7*(f[3]-1);
        int day=tm.tm_mday;timegm(&tm);if(tm.tm_mon!=f[1]-1){tm.tm_mon=f[1]-1;tm.tm_mday=day-7;}
    }
    *result=timegm(&tm)+(int64_t)bias*60;return 1;
}
static uint32_t local_zone_information(uint32_t p)
{
    if(!p){last_error=87;return 0xffffffff;}
    time_t now=deterministic?1700000000:time(NULL);struct tm current;localtime_r(&now,&current);
    struct tm start={.tm_year=current.tm_year,.tm_mday=1};time_t first=timegm(&start);
    start.tm_year++;time_t end=timegm(&start);struct tm previous;localtime_r(&first,&previous);
    int standard_offset=previous.tm_gmtoff,daylight_offset=previous.tm_gmtoff,have_standard=0,have_daylight=0;
    uint16_t transitions[2][8]={{0}};char names[2][32]={{0}};
    for(time_t t=first;t<=end;t+=86400){
        struct tm local;localtime_r(&t,&local);int dst=local.tm_isdst>0;
        if(dst){daylight_offset=local.tm_gmtoff;have_daylight=1;}else{standard_offset=local.tm_gmtoff;have_standard=1;}
        if(local.tm_zone)snprintf(names[dst],sizeof names[dst],"%s",local.tm_zone);
        if((local.tm_isdst>0)!=(previous.tm_isdst>0)){
            time_t lo=t-86400,hi=t;
            while(hi-lo>1){time_t mid=lo+(hi-lo)/2;struct tm probe;localtime_r(&mid,&probe);if((probe.tm_isdst>0)==dst)hi=mid;else lo=mid;}
            time_t wall=hi+previous.tm_gmtoff;struct tm transition;gmtime_r(&wall,&transition);
            uint16_t f[]={transition.tm_year+1900,transition.tm_mon+1,transition.tm_wday,transition.tm_mday,transition.tm_hour,transition.tm_min,transition.tm_sec,0};
            memcpy(transitions[dst],f,sizeof f);
        }
        previous=local;
    }
    memset(G_MEM+p,0,172);
    if(!have_standard)standard_offset=daylight_offset;
    rt_w32(G_MEM,p,-standard_offset/60);
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<31&&names[i][j];j++)rt_w16(G_MEM,p+(i?88:4)+2*j,(unsigned char)names[i][j]);
    if(have_standard&&have_daylight&&transitions[0][1]&&transitions[1][1]){
        memcpy(G_MEM+p+68,transitions[0],16);memcpy(G_MEM+p+152,transitions[1],16);
        rt_w32(G_MEM,p+168,(standard_offset-daylight_offset)/60);return current.tm_isdst>0?2:1;
    }
    return 0;
}
static int find_next(FindHandle *search,uint32_t output,int wide)
{
    if(!output){last_error=87;return 0;}
    struct dirent *entry;
    while((entry=readdir(search->directory))){
        UniChar name[1024],matched[1024];
        size_t length=find_unicode(entry->d_name,name,1024,0);
        size_t folded=find_unicode(entry->d_name,matched,1024,!search->case_sensitive);
        if(length==SIZE_MAX||folded==SIZE_MAX||!find_match(search->pattern,search->length,matched,folded))continue;
        struct stat st;if(fstatat(dirfd(search->directory),entry->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;
        int directory=S_ISDIR(st.st_mode);if(search->directories_only&&!directory)continue;
        if(length>=260){last_error=206;return 0;}
        memset(G_MEM+output,0,wide?592:320);
        uint32_t attributes=directory?0x10:0x20;
        if(!(st.st_mode&S_IWUSR))attributes|=1;
        if(S_ISLNK(st.st_mode)){attributes|=0x400;rt_w32(G_MEM,output+36,0xa000000c);}
        rt_w32(G_MEM,output,attributes);
        rt_w64(G_MEM,output+4,find_time(st.st_birthtimespec));
        rt_w64(G_MEM,output+12,find_time(st.st_atimespec));
        rt_w64(G_MEM,output+20,find_time(st.st_mtimespec));
        uint64_t size=directory?0:st.st_size;rt_w32(G_MEM,output+28,size>>32);rt_w32(G_MEM,output+32,size);
        if(wide)memcpy(G_MEM+output+44,name,length*2);
        else for(size_t j=0;j<length;j++){
            uint8_t byte='?';for(unsigned k=0;k<256;k++)if(cp1252_decode[k]==name[j]){byte=k;break;}
            G_MEM[output+44+j]=byte;
        }
        return 1;
    }
    last_error=18;return 0;
}
static uint32_t find_first(uint32_t path,int wide,uint32_t info,uint32_t output,uint32_t op,uint32_t filter,uint32_t flags)
{
    if(info>1||op>1||filter||(flags&~7u)||!output){last_error=87;return 0xffffffff;}
    if(!path){last_error=87;return 0xffffffff;}
    size_t length=0;uint16_t ch=0,previous=0;
    while(length<32768&&(ch=wide?rt_r16(G_MEM,path+2*length):G_MEM[path+length])){previous=ch;length++;}
    if(!length||previous=='/'||previous=='\\'){last_error=2;return 0xffffffff;}
    char full[4096];if(!guest_path(path,wide,full))return 0xffffffff;
    char *leaf=strrchr(full,'/');if(!leaf||!leaf[1]){last_error=2;return 0xffffffff;}
    FindHandle fresh={0};fresh.length=find_unicode(leaf+1,fresh.pattern,1024,!(flags&1));
    if(fresh.length==SIZE_MAX){last_error=206;return 0xffffffff;}
    *leaf=0;fresh.directory=opendir(full);if(!fresh.directory){last_error=file_error(errno);return 0xffffffff;}
    fresh.case_sensitive=flags&1;fresh.directories_only=op==1;
    pthread_mutex_lock(&search_lock);
    unsigned slot=0;while(slot<256&&searches[slot].directory)slot++;
    if(slot==256){last_error=4;closedir(fresh.directory);pthread_mutex_unlock(&search_lock);return 0xffffffff;}
    if(!find_next(&fresh,output,wide)){
        if(last_error==18)last_error=2;closedir(fresh.directory);pthread_mutex_unlock(&search_lock);return 0xffffffff;
    }
    fresh.handle=next_search++;searches[slot]=fresh;pthread_mutex_unlock(&search_lock);return fresh.handle;
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
    protect_pages(BHL_HEAP_BASE,BHL_HEAP_END-BHL_HEAP_BASE,4);
    protect_pages(BHL_STACK_BASE,BHL_STACK_SIZE,4);protect_pages(BHL_TEB,0x2000,4);
    for(unsigned k=0;k<bhl_module_count;k++){
        uint32_t base=bhl_modules[k].base,pe=base+rt_r32(G_MEM,base+0x3c),optional=pe+24;
        protect_pages(base,rt_r32(G_MEM,optional+60),2);
        unsigned count=rt_r16(G_MEM,pe+6);uint32_t section=optional+rt_r16(G_MEM,pe+20);
        for(unsigned i=0;i<count;i++,section+=40){
            uint32_t flags=rt_r32(G_MEM,section+36);int x=!!(flags&0x20000000),r=!!(flags&0x40000000),w=!!(flags&0x80000000);
            uint8_t protection=x?(w?0x40:r?0x20:0x10):(w?4:r?2:1);
            protect_pages(base+rt_r32(G_MEM,section+12),rt_r32(G_MEM,section+8),protection);
        }
    }
    for (unsigned i = 0; i < rt_imptab_n; i++) {
        uint32_t target=BHL_THUNK_BASE+i*16;
        if(!strcasecmp(rt_imptab[i].name,"MSVCRT.dll!_adjust_fdiv"))target=bhl_heap_alloc(4,1);
        else if(!strcasecmp(rt_imptab[i].name,"MSVCRT.dll!__mb_cur_max")){target=bhl_heap_alloc(4,1);rt_w32(G_MEM,target,1);}
        rt_w32(G_MEM,rt_imptab[i].iat,target);
    }
    /* Function thunks are dispatch tokens, not readable zero-filled data.
       This also exposes missing data-import adapters instead of hiding them. */
    if(mprotect(G_MEM+BHL_THUNK_BASE,0x100000,PROT_NONE))abort();
    /* FILE* values are opaque handles; inline Windows FILE structure access
       needs its own adapter and must not read native libc internals. */
    if(mprotect(G_MEM+0x06000000,0x100000,PROT_NONE))abort();
}

/* Pure CRT math consumes register/stack values and cannot borrow entities.
 * Keep the active memory scope; the stack reads still use watched helpers. */
static int scalar_crt_import(Ctx *c,const char *qualified)
{
    if(strncasecmp(qualified,"MSVCRT.dll!",11))return 0;
    const char *name=qualified+11;
#define SCALAR(n) (!strcmp(name,n))
#define SCALAR_RETURN(v,n) do{c->eax=(uint32_t)(v);c->esp+=4;return 1;}while(0)
        if(SCALAR("abs"))SCALAR_RETURN(A(0)>>31?0u-A(0):A(0),0);
        if(SCALAR("_ftol")){
            double value=c->st[c->top];c->top=(c->top+1)&7;uint64_t result;
            if(!isfinite(value)||value>=9223372036854775808.0||value< -9223372036854775808.0){result=0x8000000000000000ull;c->fsw|=1;}
            else result=(uint64_t)(int64_t)trunc(value);
            c->edx=(uint32_t)(result>>32);SCALAR_RETURN((uint32_t)result,0);
        }
        if(SCALAR("floor")||SCALAR("ceil")||SCALAR("sin")||SCALAR("cos")||SCALAR("sqrt")||SCALAR("fabs")||SCALAR("log")||SCALAR("atan")){
            double value=rt_rf64(G_MEM,c->esp+4);
            value=SCALAR("floor")?floor(value):SCALAR("ceil")?ceil(value):SCALAR("sin")?sin(value):SCALAR("cos")?cos(value):SCALAR("sqrt")?sqrt(value):SCALAR("fabs")?fabs(value):SCALAR("log")?log(value):atan(value);
            FPUSH(value);SCALAR_RETURN(0,0);
        }
        if(SCALAR("pow")||SCALAR("fmod")){double a=rt_rf64(G_MEM,c->esp+4),b=rt_rf64(G_MEM,c->esp+12);FPUSH(SCALAR("pow")?pow(a,b):fmod(a,b));SCALAR_RETURN(0,0);}
        if(SCALAR("ldexp")){FPUSH(ldexp(rt_rf64(G_MEM,c->esp+4),(int32_t)A(2)));SCALAR_RETURN(0,0);}
        if(SCALAR("_CIpow")){double exponent=c->st[c->top];c->top=(c->top+1)&7;c->st[c->top]=pow(c->st[c->top],exponent);SCALAR_RETURN(0,0);}
#undef SCALAR_RETURN
#undef SCALAR
    return 0;
}

static void dispatch_import(Ctx *c, uint32_t index)
{
    const char *qualified = bhl_import_name(index);
    if (trace_enabled) { fprintf(stderr, "import %08x %s(%08x,%08x,%08x,%08x)\n", rt_r32(G_MEM,c->esp), qualified, A(0), A(1), A(2), A(3)); fflush(stderr); }
    if (index >= rt_imptab_n && index - rt_imptab_n < dynamic_count && dynamic[index - rt_imptab_n].fn) {
        dynamic[index - rt_imptab_n].fn(c); return;
    }
    if(scalar_crt_import(c,qualified))return;
    /* Imported native services may retain or bulk-access borrowed pointers. */
    rt_watch_materialize();
    const char *name = strchr(qualified, '!');
    extern int bhl_platform_import(Ctx *,const char *);
    if(bhl_platform_import(c,qualified))return;
    extern int bhl_gdi_import(Ctx *,const char *);
    if(bhl_gdi_import(c,qualified))return;
    if(!strcmp(qualified,"ole32.dll!CoInitialize")){
        if(A(0))RETURN(0x80070057u,1);unsigned previous=com_initializations++;RETURN(previous?1:0,1);
    }
    if(!strcmp(qualified,"ole32.dll!CoUninitialize")){
        if(!com_initializations){missing(c,"unbalanced CoUninitialize");return;}com_initializations--;RETURN(0,0);
    }
    if(!strcmp(qualified,"WINMM.dll!mixerGetNumDevs")){
        /* The native backend exposes a software playback device, with no
           legacy system mixer/control-panel devices. Buffer volume remains
           available through its DirectSound interface. */
        RETURN(0,0);
    }
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
        if(CRT("realloc")){
            uint32_t p=A(0),size=A(1);if(!p)RETURN(bhl_heap_alloc(size,0),0);if(!size){heap_free(p);RETURN(0,0);}
            Block *b=block_at(p);if(!b){set_crt_errno(22);RETURN(0,0);}uint32_t result=bhl_heap_alloc(size,0);
            if(result){memcpy(G_MEM+result,G_MEM+p,size<b->size?size:b->size);heap_free(p);}else set_crt_errno(12);RETURN(result,0);
        }
        if(CRT("_errno")){if(!crt_errno)crt_errno=bhl_heap_alloc(4,1);RETURN(crt_errno,0);}
        if(CRT("sprintf")||CRT("_vsnprintf")||CRT("printf")){
            char text[8192];int print=CRT("printf"),bounded=CRT("_vsnprintf");
            format_guest(text,gs(A(print?0:bounded?2:1)),bounded?A(3):c->esp+(print?8:12));size_t length=strlen(text);
            if(print){fwrite(text,1,length,stdout);RETURN(length,0);}
            if(bounded){size_t cap=A(1),copied=length<cap?length:cap;if(copied)memcpy(G_MEM+A(0),text,copied);if(length<cap)G_MEM[A(0)+length]=0;RETURN(length>cap?0xffffffff:length,0);}
            memcpy(G_MEM+A(0),text,length+1);RETURN(length,0);
        }
        if(CRT("fopen")){
            char path[4096];const char *mode=(char*)G_MEM+A(1);
            if(strcmp(mode,"rb")&&strcmp(mode,"wb")&&strcmp(mode,"ab")&&strcmp(mode,"r+b")&&strcmp(mode,"w+b")&&strcmp(mode,"a+b")&&strcmp(mode,"rb+")&&strcmp(mode,"wb+")&&strcmp(mode,"ab+")){
                missing(c,"CRT fopen text/extended mode");return;
            }
            if(!guest_path(A(0),0,path)){set_crt_errno(last_error==5?13:last_error==2||last_error==3?2:22);RETURN(0,0);}
            pthread_mutex_lock(&stdio_lock);unsigned slot=0;while(slot<256&&stdio_files[slot].file)slot++;
            if(slot==256||next_stdio>=0x06100000){pthread_mutex_unlock(&stdio_lock);set_crt_errno(24);RETURN(0,0);}
            FILE *file=fopen(path,mode);int error=errno;uint32_t h=0;
            if(file){h=next_stdio;next_stdio+=16;stdio_files[slot]=(StdioHandle){file,h};}
            pthread_mutex_unlock(&stdio_lock);if(!file)set_crt_errno(error);RETURN(h,0);
        }
        if(CRT("fread")||CRT("fwrite")||CRT("fseek")||CRT("ftell")||CRT("fclose")||CRT("feof")||CRT("ferror")||CRT("fflush")){
            uint32_t h=(CRT("fread")||CRT("fwrite"))?A(3):A(0);pthread_mutex_lock(&stdio_lock);StdioHandle *entry=NULL;
            for(unsigned i=0;i<256;i++)if(stdio_files[i].file&&stdio_files[i].handle==h){entry=&stdio_files[i];break;}
            uint32_t result=0;
            if(!entry){set_crt_errno(9);result=(CRT("fread")||CRT("fwrite"))?0:0xffffffff;}
            else if(CRT("fclose")){result=fclose(entry->file);entry->file=NULL;if(result)set_crt_errno(errno);}
            else if(CRT("fseek")){
                int origin=A(2);if(origin<0||origin>2){set_crt_errno(22);result=0xffffffff;}
                else{result=fseek(entry->file,(int32_t)A(1),origin==0?SEEK_SET:origin==1?SEEK_CUR:SEEK_END);if(result)set_crt_errno(errno);}
            }else if(CRT("ftell")){
                long offset=ftell(entry->file);if(offset>INT32_MAX){set_crt_errno(22);result=0xffffffff;}else{result=offset;if(offset<0)set_crt_errno(errno);}
            }else if(CRT("feof"))result=feof(entry->file);
            else if(CRT("ferror"))result=ferror(entry->file);
            else if(CRT("fflush")){result=fflush(entry->file);if(result)set_crt_errno(errno);}
            else{
                uint64_t bytes=(uint64_t)A(1)*A(2);
                if(bytes>UINT32_MAX||(uint64_t)A(0)+bytes>0x100000000ull)set_crt_errno(22);
                else{result=CRT("fread")?fread(G_MEM+A(0),A(1),A(2),entry->file):fwrite(G_MEM+A(0),A(1),A(2),entry->file);if(ferror(entry->file))set_crt_errno(errno);}
            }
            pthread_mutex_unlock(&stdio_lock);RETURN(result,0);
        }
        if(CRT("memcpy")||CRT("memmove")){memmove(G_MEM+A(0),G_MEM+A(1),A(2));RETURN(A(0),0);}
        if(CRT("memset")){memset(G_MEM+A(0),(unsigned char)A(1),A(2));RETURN(A(0),0);}
        if(CRT("memcmp"))RETURN(memcmp(G_MEM+A(0),G_MEM+A(1),A(2)),0);
        if(CRT("memchr")){const void *p=memchr(G_MEM+A(0),A(1),A(2));RETURN(p?(const uint8_t*)p-G_MEM:0,0);}
        if(CRT("atoi"))RETURN(atoi((char*)G_MEM+A(0)),0);
        if(CRT("strlen"))RETURN(strlen((char*)G_MEM+A(0)),0);
        if(CRT("strcmp"))RETURN(strcmp((char*)G_MEM+A(0),(char*)G_MEM+A(1)),0);
        if(CRT("strcpy")){strcpy((char*)G_MEM+A(0),(char*)G_MEM+A(1));RETURN(A(0),0);}
        if(CRT("frexp")){int exponent;double result=frexp(rt_rf64(G_MEM,c->esp+4),&exponent);rt_w32(G_MEM,A(2),exponent);FPUSH(result);RETURN(0,0);}
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
    extern int bhl_thread_import(Ctx *,const char *);
    if(bhl_thread_import(c,name))return;
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
    if(IS("FileTimeToSystemTime")){
        if(!A(0)||!A(1)){last_error=87;RETURN(0,2);}uint64_t value=rt_r64(G_MEM,A(0));
        if(value>>63){last_error=87;RETURN(0,2);}
        time_t seconds=(int64_t)(value/10000000)-11644473600ll;struct tm tm;
        if(!gmtime_r(&seconds,&tm)){last_error=87;RETURN(0,2);}
        write_system_time(A(1),&tm,(value%10000000)/10000);RETURN(1,2);
    }
    if(IS("SystemTimeToFileTime")){
        time_t seconds;unsigned ms;if(!A(1)||!read_system_time(A(0),&seconds,&ms)){last_error=87;RETURN(0,2);}
        rt_w64(G_MEM,A(1),((uint64_t)seconds+11644473600ull)*10000000ull+ms*10000);RETURN(1,2);
    }
    if(IS("GetTimeZoneInformation"))RETURN(local_zone_information(A(0)),1);
    if(IS("SystemTimeToTzSpecificLocalTime")){
        time_t seconds;unsigned ms;struct tm tm;
        if(!A(2)||!read_system_time(A(1),&seconds,&ms)){last_error=87;RETURN(0,3);}
        if(!A(0))localtime_r(&seconds,&tm);
        else{
            uint32_t zone=A(0);int bias=(int32_t)rt_r32(G_MEM,zone),standard=bias+(int32_t)rt_r32(G_MEM,zone+84);
            int daylight=bias+(int32_t)rt_r32(G_MEM,zone+168),offset=standard;
            if(rt_r16(G_MEM,zone+70)&&rt_r16(G_MEM,zone+154)){
                gmtime_r(&seconds,&tm);time_t begin,end;
                if(!zone_transition(zone+152,tm.tm_year+1900,standard,&begin)||!zone_transition(zone+68,tm.tm_year+1900,daylight,&end)){last_error=87;RETURN(0,3);}
                if(begin<end?(seconds>=begin&&seconds<end):(seconds>=begin||seconds<end))offset=daylight;
            }
            seconds-=(int64_t)offset*60;gmtime_r(&seconds,&tm);
        }
        write_system_time(A(2),&tm,ms);RETURN(1,3);
    }
    if (IS("QueryPerformanceCounter")) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        rt_w64(G_MEM, A(0), deterministic ? 123456789ull : (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec); RETURN(1, 1);
    }
    if (IS("QueryPerformanceFrequency")) { rt_w64(G_MEM, A(0), 1000000000); RETURN(1, 1); }
    if (IS("GetTickCount")) { struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);RETURN(deterministic?123456:(uint32_t)((uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000),0); }
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
        l->address = p;pthread_mutex_lock(&lock_registry);l->next = locks; locks = l;pthread_mutex_unlock(&lock_registry);
        memset(G_MEM + p, 0, 24); rt_w32(G_MEM,p+4,0xffffffff); rt_w32(G_MEM,p+20,nargs>1?A(1):0);
        RETURN(1,nargs);
    }
    if (IS("EnterCriticalSection") || IS("LeaveCriticalSection") || IS("DeleteCriticalSection")) {
        Lock *l = lock_at(A(0)); if (!l) { missing(c, "uninitialized critical section"); return; }
        int result;
        uint32_t address=l->address;
        if (IS("EnterCriticalSection")) {
            result = pthread_mutex_lock(&l->mutex);
            if(!result){extern void bhl_thread_critical(int);bhl_thread_critical(1);rt_w32(G_MEM,address+4,rt_r32(G_MEM,address+4)+1);rt_w32(G_MEM,address+8,rt_r32(G_MEM,address+8)+1);rt_w32(G_MEM,address+12,rt_r32(G_MEM,c->fs_base+0x24));}
        }
        else if (IS("LeaveCriticalSection")) {
            uint32_t depth=rt_r32(G_MEM,address+8);
            if(!depth||rt_r32(G_MEM,address+12)!=rt_r32(G_MEM,c->fs_base+0x24)){missing(c,"critical section not owned by current thread");return;}
            rt_w32(G_MEM,address+4,rt_r32(G_MEM,address+4)-1);rt_w32(G_MEM,address+8,depth-1);
            if(depth==1)rt_w32(G_MEM,address+12,0);
            result = pthread_mutex_unlock(&l->mutex);
            if(!result){extern void bhl_thread_critical(int);bhl_thread_critical(-1);}
        }
        else {
            result = pthread_mutex_destroy(&l->mutex);
            if (!result) {pthread_mutex_lock(&lock_registry);Lock **p = &locks; while (*p != l) p = &(*p)->next; *p = l->next;pthread_mutex_unlock(&lock_registry);free(l);}
        }
        if (result) missing(c, "critical section operation failed");
        RETURN(0, 1);
    }
    if (IS("TlsAlloc") || IS("FlsAlloc")) {
        pthread_mutex_lock(&tls_lock);
        for (unsigned i=0;i<1088;i++) if (!tls_used[i]) { tls_used[i]=1;tls_generations[i]=++slot_generations[i];tls_values[i]=0;fls_destructors[i]=IS("FlsAlloc")?A(0):0;pthread_mutex_unlock(&tls_lock);RETURN(i, IS("FlsAlloc") ? 1 : 0); }
        pthread_mutex_unlock(&tls_lock);
        last_error=8; RETURN(0xffffffff, IS("FlsAlloc") ? 1 : 0);
    }
    if (IS("TlsGetValue") || IS("FlsGetValue")) { uint32_t i=A(0);pthread_mutex_lock(&tls_lock);last_error=i<1088&&tls_used[i]?0:87;uint32_t generation=last_error?0:slot_generations[i];pthread_mutex_unlock(&tls_lock);if(!last_error&&tls_generations[i]!=generation){tls_values[i]=0;tls_generations[i]=generation;}RETURN(last_error?0:tls_values[i],1); }
    if (IS("TlsSetValue") || IS("FlsSetValue")) { uint32_t i=A(0);pthread_mutex_lock(&tls_lock);int valid=i<1088&&tls_used[i];uint32_t generation=valid?slot_generations[i]:0;pthread_mutex_unlock(&tls_lock);if(!valid){last_error=87;RETURN(0,2);}tls_generations[i]=generation;tls_values[i]=A(1);RETURN(1,2); }
    if (IS("TlsFree") || IS("FlsFree")) {
        uint32_t i=A(0);pthread_mutex_lock(&tls_lock);int valid=i<1088&&tls_used[i];uint32_t destructor=valid?fls_destructors[i]:0;pthread_mutex_unlock(&tls_lock);
        if(!valid){last_error=87;RETURN(0,1);}
        if(IS("FlsFree") && destructor && tls_values[i]) {
            Ctx child=*c; child.esp-=8;
            rt_w32(G_MEM,child.esp,0x00100000);rt_w32(G_MEM,child.esp+4,tls_values[i]);
            tls_values[i]=0;rt_call(&child,destructor);
        }
        pthread_mutex_lock(&tls_lock);tls_used[i]=0;fls_destructors[i]=0;pthread_mutex_unlock(&tls_lock);tls_values[i]=0;RETURN(1,1);
    }
    if (IS("GetProcessHeap")) RETURN(0x01000000, 0);
    if (IS("HeapCreate")) {
        if(A(0)&~1u||A(2)){missing(c,"HeapCreate flags/maximum size");return;}
        for(unsigned i=1;i<256;i++)if(!heaps[i]){heaps[i]=1;RETURN(0x01000000+i,3);}last_error=8;RETURN(0,3);
    }
    if (IS("HeapDestroy")) {
        uint32_t h=A(0);if(h<=0x01000000||h>=0x01000100||!heaps[h-0x01000000]){last_error=6;RETURN(0,1);}
        pthread_mutex_lock(&heap_lock);
        for(Block *b=blocks;b;)if(b->used&&b->heap==h)b=heap_release(b);else b=b->next;
        heaps[h-0x01000000]=0;pthread_mutex_unlock(&heap_lock);RETURN(1,1);
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
        if(!size||size>0x10000000||protect!=4||!type||(type&~0x3000u)){missing(c,"VirtualAlloc mode");return;}
        VirtualRegion *region=NULL;
        if(!p){
            uint32_t rounded=(size+65535)&~65535u;
            if(rounded>0xf0000000u-virtual_next){last_error=8;RETURN(0,4);}
            for(unsigned i=0;i<256;i++)if(!regions[i].size){region=&regions[i];break;}if(!region){last_error=8;RETURN(0,4);}
            p=virtual_next;virtual_next+=rounded;*region=(VirtualRegion){p,rounded};
        }else{
            for(unsigned i=0;i<256;i++)if(regions[i].size&&p>=regions[i].base&&(uint64_t)p+size<=(uint64_t)regions[i].base+regions[i].size){region=&regions[i];break;}
            if(!region||type&0x2000){last_error=487;RETURN(0,4);}
        }
        uint32_t first=p&~4095u;uint64_t end=((uint64_t)p+size+4095)&~4095ull;
        if(type&0x1000)for(uint64_t page=first;page<end;page+=4096)if(!page_protection[page>>12]){memset(G_MEM+page,0,4096);page_protection[page>>12]=protect;}
        RETURN(first,4);
    }
    if(IS("VirtualFree")){
        uint32_t p=A(0),size=A(1),type=A(2);VirtualRegion *region=NULL;
        for(unsigned i=0;i<256;i++)if(regions[i].size&&p>=regions[i].base&&(uint64_t)p+size<=(uint64_t)regions[i].base+regions[i].size){region=&regions[i];break;}
        if(!region){last_error=487;RETURN(0,3);}
        if(type==0x8000){if(p!=region->base||size){last_error=87;RETURN(0,3);}protect_pages(p,region->size,0);region->size=0;RETURN(1,3);}
        if(type==0x4000){if(!size){if(p!=region->base){last_error=87;RETURN(0,3);}size=region->size;}protect_pages(p,size,0);RETURN(1,3);}
        last_error=87;RETURN(0,3);
    }
    if(IS("VirtualProtect")){
        uint32_t p=A(0),size=A(1),mode=A(2),old=A(3);uint64_t end=(uint64_t)p+size;
        if(!size||end>0x100000000ull||!old||!(mode==1||mode==2||mode==4||mode==0x10||mode==0x20||mode==0x40)){last_error=87;RETURN(0,4);}
        if(p>=0x80000000u&&p<virtual_next){int contained=0;for(unsigned i=0;i<256;i++)if(regions[i].size&&p>=regions[i].base&&end<=(uint64_t)regions[i].base+regions[i].size){contained=1;break;}if(!contained){last_error=487;RETURN(0,4);}}
        for(uint64_t page=p>>12;page<(end+4095)/4096;page++)if(!page_protection[page]){last_error=487;RETURN(0,4);}
        uint32_t previous=page_protection[p>>12];protect_pages(p,size,mode);rt_w32(G_MEM,old,previous);RETURN(1,4);
    }
    if(IS("VirtualLock")||IS("VirtualUnlock")){
        uint32_t p=A(0),size=A(1);if(!size||(uint64_t)p+size>0x100000000ull){last_error=87;RETURN(0,2);}
        int result=IS("VirtualLock")?mlock(G_MEM+p,size):munlock(G_MEM+p,size);if(result)last_error=1453;RETURN(result==0,2);
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
        if(h>=0x10000000&&h<=0x50000000&&IS("LoadLibraryA")){
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
        if(A(0)==0x7c900000){extern uint32_t bhl_dsound_export(const char *);uint32_t target=bhl_dsound_export((char*)G_MEM+A(1));if(!target)last_error=127;RETURN(target,2);}
        if(A(0)!=0x7c800000) {
            extern uint32_t bhl_export(const char *,const char *);
            uint32_t h=A(0);const char *module=h==0x10000000?"hl.dll":h==0x30000000?"client.dll":h==0x40000000?"vgui.dll":h==0x50000000?"fmod.dll":NULL;
            if(!module){last_error=126;RETURN(0,2);}uint32_t target=bhl_export(module,(char*)G_MEM+A(1));if(!target)last_error=127;RETURN(target,2);
        }
        char full[256]; snprintf(full,sizeof full,"KERNEL32.dll!%s",G_MEM+A(1));
        if(trace_enabled)fprintf(stderr,"  resolve %s\n",full);
        RETURN(bhl_thunk(full,NULL),2);
    }
    if (IS("FreeLibrary")) { if(A(0)!=0x7c800000&&A(0)!=0x7c900000) {last_error=6;RETURN(0,1);} RETURN(1,1); }
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
    if(IS("FindFirstFileExW")||IS("FindFirstFileExA"))RETURN(find_first(A(0),IS("FindFirstFileExW"),A(1),A(2),A(3),A(4),A(5)),6);
    if(IS("FindFirstFileW")||IS("FindFirstFileA"))RETURN(find_first(A(0),IS("FindFirstFileW"),0,A(1),0,0,0),2);
    if(IS("FindNextFileW")||IS("FindNextFileA")||IS("FindClose")){
        pthread_mutex_lock(&search_lock);FindHandle *search=NULL;
        for(unsigned i=0;i<256;i++)if(searches[i].directory&&searches[i].handle==A(0)){search=&searches[i];break;}
        int result=0;
        if(!search)last_error=6;
        else if(IS("FindClose")){closedir(search->directory);search->directory=NULL;result=1;}
        else result=find_next(search,A(1),IS("FindNextFileW"));
        pthread_mutex_unlock(&search_lock);RETURN(result,IS("FindClose")?1:2);
    }
    if (IS("CreateFileW")||IS("CreateFileA")) {
        char path[4096];if(!guest_path(A(0),IS("CreateFileW"),path))RETURN(0xffffffff,7);
        uint32_t access=A(1),share=A(2),disposition=A(4),attributes=A(5);
        int inherit=0;
        if(A(3)){
            uint32_t sa=A(3);if(rt_r32(G_MEM,sa)!=12){last_error=87;RETURN(0xffffffff,7);}
            if(rt_r32(G_MEM,sa+4)){missing(c,"CreateFile security descriptor");return;}inherit=!!rt_r32(G_MEM,sa+8);
        }
        if(A(6)||(attributes&0x40000000u)){missing(c,"CreateFile template/overlapped mode");return;}
        /* Normal/archive and sequential/random access are supported hints.
           Do not silently ignore delete-on-close, encryption or backup mode. */
        if(attributes&~0x180000a0u){missing(c,"CreateFile attributes");return;}
        if(access&~0xc0000000u||share&~7u){missing(c,"CreateFile access/share mode");return;}
        if((disposition==2||disposition==5)&&!(access&0x40000000u)){last_error=5;RETURN(0xffffffff,7);}
        int mode=(access&0x40000000)?((access&0x80000000)?O_RDWR:O_WRONLY):O_RDONLY;
        if(!inherit)mode|=O_CLOEXEC;
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
    /* The engine TEB is never a cancellable worker. Preserve checkpoints on
       every worker import, including native callbacks. */
    extern void bhl_thread_checkpoint(void);if(c->fs_base!=BHL_TEB)bhl_thread_checkpoint();
    unsigned callback=index-rt_imptab_n;
    if(!import_observer&&!trace_enabled&&callback<dynamic_count&&dynamic[callback].fn){
        dynamic[callback].fn(c);return;
    }
    if(import_observer)import_observer(c,index,0);
    dispatch_import(c,index);
    if(import_observer)import_observer(c,index,1);
}
void bhl_fls_thread_exit(void)
{
    for(unsigned i=0;i<1088;i++){
        pthread_mutex_lock(&tls_lock);uint32_t fn=tls_used[i]&&tls_generations[i]==slot_generations[i]?fls_destructors[i]:0;pthread_mutex_unlock(&tls_lock);
        uint32_t value=tls_values[i];tls_values[i]=0;if(fn&&value)bhl_invoke(fn,&value,1,1);
    }
}
