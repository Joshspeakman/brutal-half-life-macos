/* DirectSound's guest COM boundary backed by a native SDL PCM device.
 * This implements software PCM buffers, not hardware acceleration or EAX.
 * Unsupported method bodies trap rather than pretending to have succeeded.
 */
#include "win32.h"
#include <SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define RET(v,n) do{c->eax=(uint32_t)(v);c->esp+=4+4*(n);return;}while(0)
#define INVALID 0x80070057u
#define NOINTERFACE 0x80004002u
#define CONTROLUNAVAILABLE 0x8878001eu
extern int bhl_heap_free(uint32_t);
static SDL_AudioDeviceID device;
static SDL_AudioSpec output;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
typedef struct SoundLock {uint32_t first,nfirst,second,nsecond;int used;} SoundLock;
typedef struct SoundObject {uint32_t guest;_Atomic uint32_t refs;int buffer,playing,looping;uint32_t flags,data,size,rate,frequency,channels,bits,align;int32_t volume,pan;double position;uint8_t *pcm;SoundLock locks[16];struct SoundObject *next;} SoundObject;
static _Atomic(SoundObject *) objects;
static uint32_t sound_vtable,buffer_vtable;
static uint64_t mixed_frames;
static uint64_t audible_samples;
static void sound_method(Ctx *,unsigned);
static void buffer_method(Ctx *,unsigned);
static void missing(Ctx *c,const char *name){rt_unhandled(c,rt_r32(G_MEM,c->esp),name);}
static SoundObject *object(uint32_t p,int buffer)
{
    for(SoundObject *s=objects;s;s=s->next)if(s->guest==p&&s->refs&&s->buffer==buffer)return s;return NULL;
}
static float sample(SoundObject *s,uint32_t frame,unsigned channel)
{
    if(s->channels==1)channel=0;uint32_t p=frame*s->align+channel*(s->bits/8);
    int16_t value;if(s->bits==16){memcpy(&value,s->pcm+p,2);return value/32768.0f;}return ((int)s->pcm[p]-128)/128.0f;
}
static void audio_callback(void *unused,Uint8 *stream,int bytes)
{
    (void)unused;float *out=(float*)stream;int frames=bytes/(2*sizeof(float));memset(stream,0,bytes);
    pthread_mutex_lock(&mutex);
    for(SoundObject *s=objects;s;s=s->next){
        if(!s->refs||!s->buffer||!s->playing||!s->size||(s->flags&1))continue;
        uint32_t length=s->size/s->align;if(!length)continue;
        float volume=powf(10.0f,s->volume/2000.0f),left=volume,right=volume;
        if(s->pan<0)right*=powf(10.0f,s->pan/2000.0f);else if(s->pan>0)left*=powf(10.0f,-s->pan/2000.0f);
        double advance=(double)s->frequency/output.freq;
        for(int i=0;i<frames;i++){
            if(s->position>=length){if(s->looping)s->position=fmod(s->position,length);else{s->playing=0;s->position=0;break;}}
            uint32_t at=(uint32_t)s->position,next=at+1<length?at+1:s->looping?0:at;float fraction=s->position-at;
            float a=sample(s,at,0),b=sample(s,next,0);out[2*i]+=(a+(b-a)*fraction)*left;
            a=sample(s,at,1);b=sample(s,next,1);out[2*i+1]+=(a+(b-a)*fraction)*right;s->position+=advance;
        }
    }
    for(int i=0;i<frames*2;i++){if(out[i]>1)out[i]=1;if(out[i]<-1)out[i]=-1;if(isfinite(out[i])&&fabsf(out[i])>0.00001f)audible_samples++;}
    mixed_frames+=frames;pthread_mutex_unlock(&mutex);
}
uint64_t bhl_audio_frames(void){pthread_mutex_lock(&mutex);uint64_t n=mixed_frames;pthread_mutex_unlock(&mutex);return n;}
uint64_t bhl_audio_nonzero_samples(void){pthread_mutex_lock(&mutex);uint64_t n=audible_samples;pthread_mutex_unlock(&mutex);return n;}
void bhl_audio_test_pause(void){if(device)SDL_PauseAudioDevice(device,1);}
void bhl_audio_test_render(float *samples,unsigned frames){audio_callback(NULL,(Uint8*)samples,frames*2*sizeof(float));}
static void close_if_unused(void)
{
    pthread_mutex_lock(&mutex);int used=0;for(SoundObject *s=objects;s;s=s->next)if(s->refs){used=1;break;}
    SDL_AudioDeviceID old=used?0:device;if(old)device=0;pthread_mutex_unlock(&mutex);
    if(old)SDL_CloseAudioDevice(old);
}
static int open_device(void)
{
    if(device)return 1;
    if(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO)&&SDL_InitSubSystem(SDL_INIT_AUDIO))return 0;
    SDL_AudioSpec wanted={0};wanted.freq=44100;wanted.format=AUDIO_F32SYS;wanted.channels=2;wanted.samples=512;wanted.callback=audio_callback;
    device=SDL_OpenAudioDevice(NULL,0,&wanted,&output,0);if(!device)return 0;
    fprintf(stderr,"BHL native audio: SDL %s, %d Hz stereo PCM device opened\n",SDL_GetCurrentAudioDriver(),output.freq);
    SDL_PauseAudioDevice(device,0);return 1;
}
#define METHOD(kind,n) static void kind##_##n(Ctx *c){kind##_method(c,n);}
METHOD(sound,0) METHOD(sound,1) METHOD(sound,2) METHOD(sound,3) METHOD(sound,4)
METHOD(sound,5) METHOD(sound,6) METHOD(sound,7) METHOD(sound,8) METHOD(sound,9) METHOD(sound,10)
METHOD(buffer,0) METHOD(buffer,1) METHOD(buffer,2) METHOD(buffer,3) METHOD(buffer,4)
METHOD(buffer,5) METHOD(buffer,6) METHOD(buffer,7) METHOD(buffer,8) METHOD(buffer,9)
METHOD(buffer,10) METHOD(buffer,11) METHOD(buffer,12) METHOD(buffer,13) METHOD(buffer,14)
METHOD(buffer,15) METHOD(buffer,16) METHOD(buffer,17) METHOD(buffer,18) METHOD(buffer,19) METHOD(buffer,20)
static void (*sound_methods[])(Ctx *)={sound_0,sound_1,sound_2,sound_3,sound_4,sound_5,sound_6,sound_7,sound_8,sound_9,sound_10};
static void (*buffer_methods[])(Ctx *)={buffer_0,buffer_1,buffer_2,buffer_3,buffer_4,buffer_5,buffer_6,buffer_7,buffer_8,buffer_9,buffer_10,buffer_11,buffer_12,buffer_13,buffer_14,buffer_15,buffer_16,buffer_17,buffer_18,buffer_19,buffer_20};
static SoundObject *create(int buffer)
{
    uint32_t *vt=buffer?&buffer_vtable:&sound_vtable;unsigned count=buffer?21:11;
    if(!*vt){*vt=bhl_heap_alloc(count*4,1);for(unsigned i=0;i<count;i++){char name[64];snprintf(name,sizeof name,"dsound-%s!%u",buffer?"buffer":"device",i);rt_w32(G_MEM,*vt+4*i,bhl_thunk(name,buffer?buffer_methods[i]:sound_methods[i]));}}
    SoundObject *s=calloc(1,sizeof *s);if(!s)abort();s->guest=bhl_heap_alloc(4,1);s->refs=1;s->buffer=buffer;
    s->rate=s->frequency=44100;s->channels=2;s->bits=16;s->align=4;rt_w32(G_MEM,s->guest,*vt);
    pthread_mutex_lock(&mutex);s->next=objects;objects=s;pthread_mutex_unlock(&mutex);return s;
}
static int format(SoundObject *s,uint32_t p)
{
    uint16_t tag=rt_r16(G_MEM,p),channels=rt_r16(G_MEM,p+2),align=rt_r16(G_MEM,p+12),bits=rt_r16(G_MEM,p+14);uint32_t rate=rt_r32(G_MEM,p+4);
    if(tag!=1||(channels!=1&&channels!=2)||(bits!=8&&bits!=16)||rate<100||rate>200000||align!=channels*(bits/8)||rt_r32(G_MEM,p+8)!=rate*align)return 0;
    s->rate=s->frequency=rate;s->channels=channels;s->bits=bits;s->align=align;return 1;
}
static int query_interface(SoundObject *s,uint32_t guid,uint32_t result)
{
    if(!guid||!result)return INVALID;rt_w32(G_MEM,result,0);
    static const uint8_t unknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    static const uint8_t ds[16]={0x83,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0,0x20,0xaf,0x0b,0xe5,0x60};
    static const uint8_t buffer[16]={0x85,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0,0x20,0xaf,0x0b,0xe5,0x60};
    if(memcmp(G_MEM+guid,unknown,16)&&memcmp(G_MEM+guid,s->buffer?buffer:ds,16))return NOINTERFACE;
    s->refs++;rt_w32(G_MEM,result,s->guest);return 0;
}
static void sound_method(Ctx *c,unsigned method)
{
    SoundObject *s=object(A(0),0);if(!s){missing(c,"invalid DirectSound object");return;}
    switch(method){
    case 0:RET(query_interface(s,A(1),A(2)),3);
    case 1:RET(++s->refs,1);
    case 2:{unsigned refs=--s->refs;close_if_unused();RET(refs,1);}
    case 3:{
        uint32_t p=A(1),result=A(2);if(!p||!result||A(3))RET(INVALID,4);rt_w32(G_MEM,result,0);
        uint32_t size=rt_r32(G_MEM,p),flags=rt_r32(G_MEM,p+4),bytes=rt_r32(G_MEM,p+8),fmt=rt_r32(G_MEM,p+16);
        if((size!=20&&size!=36)||bytes>32*1024*1024||(!(flags&1)&&(!bytes||!fmt)))RET(INVALID,4);
        if(flags&4)RET(CONTROLUNAVAILABLE,4); /* No hardware buffers are advertised. */
        if(flags&0x10){missing(c,"DirectSound 3D buffer adapter");return;}
        SoundObject *b=create(1);b->flags=(flags&~4u)|8u;b->size=bytes;
        if(fmt&&!format(b,fmt)){b->refs=0;RET(INVALID,4);}if(bytes&&bytes%b->align){b->refs=0;RET(INVALID,4);}
        if(bytes){b->data=bhl_heap_alloc(bytes,1);b->pcm=calloc(1,bytes);if(!b->pcm)abort();if(b->bits==8){memset(G_MEM+b->data,128,bytes);memset(b->pcm,128,bytes);}}
        rt_w32(G_MEM,result,b->guest);RET(0,4);
    }
    case 4:{uint32_t p=A(1);if(!p||rt_r32(G_MEM,p)!=96)RET(INVALID,2);memset(G_MEM+p+4,0,92);rt_w32(G_MEM,p+4,0xf1f);rt_w32(G_MEM,p+8,100);rt_w32(G_MEM,p+12,200000);rt_w32(G_MEM,p+16,1);RET(0,2);}
    case 6:if(A(2)!=1&&A(2)!=2)RET(CONTROLUNAVAILABLE,3);RET(0,3);
    case 7:RET(0,1); /* Software allocation has no hardware heap to compact. */
    case 8:if(!A(1))RET(INVALID,2);rt_w32(G_MEM,A(1),4);RET(0,2);
    case 9:if(A(1)!=4)RET(CONTROLUNAVAILABLE,2);RET(0,2);
    default:{char text[96];snprintf(text,sizeof text,"DirectSound method %u",method);missing(c,text);}
    }
}
static void buffer_method(Ctx *c,unsigned method)
{
    SoundObject *s=object(A(0),1);if(!s){missing(c,"invalid DirectSoundBuffer object");return;}
    switch(method){
    case 0:RET(query_interface(s,A(1),A(2)),3);
    case 1:RET(++s->refs,1);
    case 2:{pthread_mutex_lock(&mutex);unsigned refs=--s->refs;if(!refs){s->playing=0;bhl_heap_free(s->data);s->data=0;free(s->pcm);s->pcm=NULL;}pthread_mutex_unlock(&mutex);close_if_unused();RET(refs,1);}
    case 3:{uint32_t p=A(1);if(!p||rt_r32(G_MEM,p)!=20)RET(INVALID,2);rt_w32(G_MEM,p+4,s->flags);rt_w32(G_MEM,p+8,s->size);rt_w32(G_MEM,p+12,0);rt_w32(G_MEM,p+16,0);RET(0,2);}
    case 4:{pthread_mutex_lock(&mutex);uint32_t position=(uint32_t)s->position*s->align;if(s->size)position%=s->size;if(A(1))rt_w32(G_MEM,A(1),position);if(A(2))rt_w32(G_MEM,A(2),s->size?(position+output.samples*s->align)%s->size:0);pthread_mutex_unlock(&mutex);RET(0,3);}
    case 5:{uint32_t p=A(1),cap=A(2);if(A(3))rt_w32(G_MEM,A(3),18);if(!p)RET(0,4);if(cap<18)RET(INVALID,4);memset(G_MEM+p,0,18);rt_w16(G_MEM,p,1);rt_w16(G_MEM,p+2,s->channels);rt_w32(G_MEM,p+4,s->rate);rt_w32(G_MEM,p+8,s->rate*s->align);rt_w16(G_MEM,p+12,s->align);rt_w16(G_MEM,p+14,s->bits);RET(0,4);}
    case 6:case 7:case 8:{if(!A(1))RET(INVALID,2);uint32_t flag=method==6?0x80:method==7?0x40:0x20;if(!(s->flags&flag))RET(CONTROLUNAVAILABLE,2);rt_w32(G_MEM,A(1),method==6?s->volume:method==7?s->pan:s->frequency);RET(0,2);}
    case 9:if(!A(1))RET(INVALID,2);pthread_mutex_lock(&mutex);rt_w32(G_MEM,A(1),s->playing?(1|(s->looping?4:0)):0);pthread_mutex_unlock(&mutex);RET(0,2);
    case 11:{
        uint32_t offset=A(1),bytes=A(2),flags=A(7);if(!s->size||flags&~3u||!A(3)||!A(4))RET(INVALID,8);
        if(flags&1){pthread_mutex_lock(&mutex);offset=((uint32_t)s->position*s->align+output.samples*s->align)%s->size;pthread_mutex_unlock(&mutex);}
        if(flags&2){offset=0;bytes=s->size;}if(offset>=s->size||bytes>s->size)RET(INVALID,8);
        uint32_t first=bytes<s->size-offset?bytes:s->size-offset,second=bytes-first;if(second&&(!A(5)||!A(6)))RET(INVALID,8);
        pthread_mutex_lock(&mutex);unsigned i=0;while(i<16&&s->locks[i].used)i++;
        if(i==16){pthread_mutex_unlock(&mutex);RET(INVALID,8);}
        s->locks[i]=(SoundLock){s->data+offset,first,second?s->data:0,second,1};pthread_mutex_unlock(&mutex);
        rt_w32(G_MEM,A(3),s->data+offset);rt_w32(G_MEM,A(4),first);if(A(5))rt_w32(G_MEM,A(5),second?s->data:0);if(A(6))rt_w32(G_MEM,A(6),second);RET(0,8);
    }
    case 12:if(A(1)||A(2)||A(3)&~1u)RET(INVALID,4);pthread_mutex_lock(&mutex);s->looping=A(3)&1;s->playing=1;pthread_mutex_unlock(&mutex);RET(0,4);
    case 13:if(!s->size||A(1)>=s->size)RET(INVALID,2);pthread_mutex_lock(&mutex);s->position=A(1)/s->align;pthread_mutex_unlock(&mutex);RET(0,2);
    case 14:if(!(s->flags&1)||!A(1))RET(INVALID,2);pthread_mutex_lock(&mutex);int valid=format(s,A(1));pthread_mutex_unlock(&mutex);RET(valid?0:INVALID,2);
    case 15:case 16:case 17:{
        uint32_t flag=method==15?0x80:method==16?0x40:0x20;if(!(s->flags&flag))RET(CONTROLUNAVAILABLE,2);int32_t value=A(1);
        if((method==15&&(value< -10000||value>0))||(method==16&&(value< -10000||value>10000))||(method==17&&value&&(value<100||value>200000)))RET(INVALID,2);
        pthread_mutex_lock(&mutex);if(method==15)s->volume=value;else if(method==16)s->pan=value;else s->frequency=value?value:s->rate;pthread_mutex_unlock(&mutex);RET(0,2);
    }
    case 18:pthread_mutex_lock(&mutex);s->playing=0;pthread_mutex_unlock(&mutex);RET(0,1);
    case 19:{
        pthread_mutex_lock(&mutex);SoundLock *found=NULL;
        for(unsigned i=0;i<16;i++){SoundLock *l=&s->locks[i];if(l->used&&l->first==A(1)&&l->nfirst==A(2)&&l->second==A(3)&&l->nsecond==A(4)){found=l;break;}}
        if(!found){pthread_mutex_unlock(&mutex);RET(INVALID,5);}
        /* Publish complete writes under the mixer lock. The SDL callback never
           reads guest memory while the FMOD worker is modifying a lock. */
        memcpy(s->pcm+found->first-s->data,G_MEM+found->first,found->nfirst);
        if(found->nsecond)memcpy(s->pcm,G_MEM+found->second,found->nsecond);
        found->used=0;pthread_mutex_unlock(&mutex);RET(0,5);
    }
    case 20:RET(0,1); /* Software buffers never lose their backing storage. */
    default:{char text[96];snprintf(text,sizeof text,"DirectSoundBuffer method %u",method);missing(c,text);}
    }
}
static void direct_sound_create(Ctx *c)
{
    if(!A(1)||A(2))RET(INVALID,3);rt_w32(G_MEM,A(1),0);
    static const uint8_t zero_guid[16]={0};
    if(A(0)&&memcmp(G_MEM+A(0),zero_guid,16))RET(0x88780078u,3);
    if(!open_device())RET(0x88780078u,3);
    SoundObject *s=create(0);rt_w32(G_MEM,A(1),s->guest);RET(0,3);
}
static void direct_sound_enumerate(Ctx *c)
{
    if(!A(0))RET(INVALID,2);
    const char *description="Native SDL default audio device",*driver="bhl-native-sdl";
    uint32_t text=bhl_heap_alloc(96,1);strcpy((char*)G_MEM+text,description);strcpy((char*)G_MEM+text+64,driver);
    Ctx child=*c;child.esp-=20;rt_w32(G_MEM,child.esp,0x00100000);rt_w32(G_MEM,child.esp+4,0);rt_w32(G_MEM,child.esp+8,text);rt_w32(G_MEM,child.esp+12,text+64);rt_w32(G_MEM,child.esp+16,A(1));
    rt_call(&child,A(0));if(child.esp!=c->esp){missing(c,"DirectSound enumeration callback stack mismatch");return;}bhl_heap_free(text);RET(0,2);
}
uint32_t bhl_dsound_export(const char *name)
{
    if(!strcmp(name,"DirectSoundCreate"))return bhl_thunk("dsound.dll!DirectSoundCreate",direct_sound_create);
    if(!strcmp(name,"DirectSoundEnumerateA"))return bhl_thunk("dsound.dll!DirectSoundEnumerateA",direct_sound_enumerate);
    return 0;
}
