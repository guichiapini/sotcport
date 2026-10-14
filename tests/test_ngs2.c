/* Ngs2 voice graph (runtime_sotc_ngs2.c) without the game: a sampler voice playing a 1 kHz
 * PCM sine through a submixer into a mastering voice must come out of sceNgs2SystemRender
 * at the right level and pitch, loop as its block says, report its states and call back at
 * block ends. */
#define _USE_MATH_DEFINES
#include "runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(nid,table[i].name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef struct { uint16_t size; int16_t next; uint32_t id; } Header;
typedef struct { void *buffer; uint64_t size; uint32_t type, channels; } RenderBuffer;
typedef struct { uint64_t size; char name[16]; uint32_t flags, max_grain, grain, rate; uint32_t r[6]; } SystemOption;
typedef struct { uint64_t size; char name[16]; uint32_t flags, max_grain, max_voices; uint32_t r[23]; } RackOption;
typedef struct { uint32_t offset, size, repeats, skip, samples, reserved; uintptr_t user; } Block;

typedef int32_t (ABI *SystemCreate)(const SystemOption *, const void *, void **);
typedef int32_t (ABI *RackCreate)(void *, uint32_t, const RackOption *, const void *, void **);
typedef int32_t (ABI *GetVoice)(void *, uint32_t, void **);
typedef int32_t (ABI *Control)(void *, const void *);
typedef int32_t (ABI *Render)(void *, const RenderBuffer *, uint32_t);
typedef int32_t (ABI *Flags)(void *, uint32_t *);

static int failures, callbacks;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)
static ABI void on_block(const void *info) { (void)info; ++callbacks; }

static void *fn(const char *name) {
    void *f=(void *)runtime_ngs2_resolve(name);
    if (!f) { printf("missing %s\n",name); exit(1); }
    return f;
}
/* Parameter lists: headers chained by byte offsets. */
static unsigned char list[1024]; static size_t used; static Header *last;
static void *param(uint32_t id, size_t size) {
    Header *h=(Header *)(list+used);
    memset(h,0,size); h->size=(uint16_t)size; h->id=id;
    if (last) last->next=(int16_t)((unsigned char *)h-(unsigned char *)last);
    last=h; used+=(size+7)&~(size_t)7;
    return h+1;
}
static void begin(void) { used=0; last=NULL; memset(list,0,sizeof(list)); }

int main(void) {
    SystemCreate system_create=fn("sceNgs2SystemCreateWithAllocator");
    RackCreate rack_create=fn("sceNgs2RackCreate");
    GetVoice get_voice=fn("sceNgs2RackGetVoiceHandle");
    Control control=fn("sceNgs2VoiceControl");
    Render render=fn("sceNgs2SystemRender");
    Flags flags=fn("sceNgs2VoiceGetStateFlags");

    SystemOption so={.size=sizeof(so),.grain=256,.rate=48000};
    void *system=NULL;
    CHECK(system_create(&so,NULL,&system)==0 && system,"system");
    RackOption ro={.size=sizeof(ro),.max_voices=4};
    void *samplers,*mixers,*masters;
    CHECK(rack_create(system,0x1000,&ro,NULL,&samplers)==0,"sampler rack");
    CHECK(rack_create(system,0x2000,&ro,NULL,&mixers)==0,"submixer rack");
    CHECK(rack_create(system,0x3000,&ro,NULL,&masters)==0,"mastering rack");
    void *voice,*mixer,*master;
    get_voice(samplers,0,&voice); get_voice(mixers,0,&mixer); get_voice(masters,0,&master);

    /* 0.1 s of a 1 kHz mono sine, 16-bit, at 48 kHz: 100 cycles. */
    enum { N=4800 };
    static int16_t pcm[N];
    for (int i=0;i<N;++i) pcm[i]=(int16_t)lrint(16384.0*sin(2*M_PI*1000.0*i/48000.0));

    begin(); { uint32_t *p=param(0x30000001,16); p[0]=2; }          /* mastering setup: 2 ch */
    { uint32_t *p=param(6,16); p[0]=0; }                              /* play */
    CHECK(control(master,list)==0,"mastering control");
    begin(); { uint32_t *p=param(0x20000001,16); p[0]=2; }          /* submixer setup: 2 ch */
    { uint32_t *p=param(5,24); p[0]=0; p[1]=0; *(void **)(p+2)=master; }   /* port 0 -> master */
    { uint32_t *p=param(6,16); p[0]=0; }
    CHECK(control(mixer,list)==0,"submixer control");

    begin();
    { uint32_t *p=param(0x10000001,40); p[0]=0x12; p[1]=1; p[2]=48000; }   /* I16 mono 48 kHz */
    Block block={.offset=0,.size=sizeof(pcm),.repeats=1,.samples=N,.user=0x1234};
    { unsigned char *p=param(0x10000002,32); const void *d=pcm; const void *b=&block;
      memcpy(p,&d,8); uint32_t n=1; memcpy(p+12,&n,4); memcpy(p+16,&b,8); }
    { uint32_t *p=param(5,24); p[0]=0; *(void **)(p+2)=mixer; }      /* port 0 -> submixer */
    { uint32_t *p=param(3,16); p[0]=0; float v=0.5f; memcpy(p+1,&v,4); }   /* port 0 volume 0.5 */
    { unsigned char *p=param(7,32); void *f=(void *)on_block; memcpy(p,&f,8); }
    { uint32_t *p=param(6,16); p[0]=0; }                              /* play */
    CHECK(control(voice,list)==0,"sampler control");
    uint32_t state=0; flags(voice,&state);
    CHECK(state & 0x2,"sampler playing (flags %#x)",state);

    /* Render 0.25 s in 1024-frame buffers of interleaved stereo float. */
    static float out[48000/4*2];
    for (int at=0;at<12000;at+=1024) {
        int frames=12000-at<1024 ? 12000-at : 1024;
        RenderBuffer rb={out+at*2,(uint64_t)frames*2*sizeof(float),0x18,2};
        CHECK(render(system,&rb,1)==0,"render");
    }
    /* First 0.2 s (block played twice): amplitude 0.25, zero crossings of a 1 kHz tone. */
    float peak=0; int crossings=0;
    for (int i=0;i<9600;++i) {
        float l=out[i*2], r=out[i*2+1];
        if (fabsf(l)>peak) peak=fabsf(l);
        CHECK(fabsf(l-r)<1e-6f,"stereo copy at %d",i);
        if (i && ((out[(i-1)*2]<0)!=(l<0))) ++crossings;
        if (failures>8) break;
    }
    CHECK(fabsf(peak-0.25f)<0.01f,"peak %f, wanted 0.25",peak);
    CHECK(crossings>=395 && crossings<=405,"%d zero crossings in 0.2 s, wanted ~400",crossings);
    float tail=0; for (int i=9700;i<12000;++i) if (fabsf(out[i*2])>tail) tail=fabsf(out[i*2]);
    CHECK(tail<1e-4f,"silence after the block's repeats (peak %f)",tail);
    CHECK(callbacks==2,"%d block-end callbacks, wanted 2",callbacks);
    flags(voice,&state);
    CHECK(!(state & 0x2) && (state & 0x8),"stopped after the waveform (flags %#x)",state);
    /* NGS2_TEST_VAG=<SndX .bd bank>: its PS ADPCM through a second voice decodes to sound
     * that is neither silent nor clipped. */
    const char *bank=getenv("NGS2_TEST_VAG");
    if (bank) {
        FILE *f=fopen(bank,"rb");
        static unsigned char vag[65536];
        size_t n=f ? fread(vag,1,sizeof(vag),f) : 0;
        if (f) fclose(f);
        CHECK(n==sizeof(vag),"read %s",bank);
        void *voice2; get_voice(samplers,1,&voice2);
        begin();
        { uint32_t *p=param(0x10000001,40); p[0]=0x1C; p[1]=1; p[2]=48000; }
        Block vb={.offset=0,.size=(uint32_t)n,.repeats=0,.samples=0};
        { unsigned char *p=param(0x10000002,32); const void *d=vag; const void *b=&vb;
          memcpy(p,&d,8); uint32_t one=1; memcpy(p+12,&one,4); memcpy(p+16,&b,8); }
        { uint32_t *p=param(5,24); p[0]=0; *(void **)(p+2)=mixer; }
        { uint32_t *p=param(6,16); p[0]=0; }
        control(voice2,list);
        static float vout[(65536/16*28+2048)*2];
        int total=(int)(n/16*28);
        for (int at=0;at<total;at+=1024) {
            int frames=total-at<1024 ? total-at : 1024;
            RenderBuffer rb={vout+at*2,(uint64_t)frames*2*sizeof(float),0x18,2};
            render(system,&rb,1);
        }
        double energy=0; int clipped=0, nans=0;
        for (int i=0;i<total;++i) {
            float x=vout[i*2];
            if (x!=x) ++nans;
            if (fabsf(x)>=0.999f) ++clipped;
            energy+=(double)x*x;
        }
        double rms=sqrt(energy/total);
        printf("VAG bank: %d samples, rms %.4f, %d clipped, %d NaN\n",total,rms,clipped,nans);
        CHECK(nans==0 && rms>0.001 && clipped<total/100,"VAG decode looks wrong");
    }
    printf("%s (%d failures)\n",failures ? "FAILED" : "ok",failures);
    return failures!=0;
}
