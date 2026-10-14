/* libSceNgs2 for CUSA08809 (SndX on Ngs2): a software voice graph.
 *
 * Racks hold voices. Sampler voices play waveform blocks (PCM, VAG ADPCM, ATRAC9 through
 * LibAtrac9) at a pitch, and send each port's output, through an optional channel matrix and
 * a volume, to the input of the voice it is patched to. Submixer voices sum their inputs and
 * send them on the same way; mastering voices write the sum into the buffers the game passes
 * to sceNgs2SystemRender (which it then hands to AudioOut). Effects (filters, envelopes,
 * reverb, compressors, limiters) are accepted and not applied.
 *
 * Parameter IDs follow the SDK (voice 1..7, sampler 0x1000xxxx, submixer 0x2000xxxx,
 * mastering 0x3000xxxx). The first parameter lists are logged (BB_NGS2_TRACE=N lists N of
 * them, default 200): what the game sends is checked against these assumptions there. */
#define _GNU_SOURCE
#include "runtime.h"
#include "../third_party/LibAtrac9/C/src/libatrac9.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NGS2_ERR_INVALID_HANDLE ((int32_t)0x804A0230)
#define NGS2_ERR_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)
#define NGS2_ERR_INVALID_BUFFER_INFO ((int32_t)0x804A0054)
#define NGS2_ERR_INVALID_VOICE_INDEX ((int32_t)0x804A0303)

enum { HANDLE_SYSTEM=1, HANDLE_RACK=2, HANDLE_VOICE=3 };
enum { KIND_SAMPLER, KIND_SUBMIXER, KIND_MASTERING, KIND_OTHER };
enum { MAX_CH=8, MAX_PORTS=4, MAX_MATRICES=4, MAX_BLOCKS=32, MAX_GRAIN=1024, RATE=48000 };

/* Voice parameter IDs. */
enum {
    P_MATRIX_LEVELS=1, P_PORT_MATRIX=2, P_PORT_VOLUME=3, P_PORT_DELAY=4, P_PATCH=5, P_EVENT=6,
    P_CALLBACK=7,
    S_SETUP=0x10000001, S_BLOCKS=0x10000002, S_ADDRESS=0x10000003, S_FRAME_OFFSET=0x10000004,
    S_EXIT_LOOP=0x10000005, S_PITCH=0x10000006,
    X_SETUP=0x20000001,
    M_SETUP=0x30000001, M_OUTPUT=0x30000006,
};
enum { EV_PLAY=0, EV_STOP=1, EV_STOP_IMM=2, EV_KILL=3, EV_PAUSE=4, EV_RESUME=5 };
enum { ST_INUSE=0x1, ST_PLAYING=0x2, ST_PAUSED=0x4, ST_STOPPED=0x8, ST_ERROR=0x10, ST_EMPTY=0x20 };
/* Waveform types. */
enum { W_I8=0x10, W_U8=0x11, W_I16L=0x12, W_I16B=0x13, W_I24L=0x14, W_I24B=0x15, W_I32L=0x16,
       W_I32B=0x17, W_F32L=0x18, W_F32B=0x19, W_F64L=0x1A, W_F64B=0x1B, W_VAG=0x1C, W_AT9=0x40 };

typedef struct { uint16_t size; int16_t next; uint32_t id; } ParamHeader;
typedef struct { uint32_t type, channels, rate, config, frame_offset, frame_margin; } WaveFormat;
typedef struct { uint32_t offset, size, repeats, skip, samples, reserved; uintptr_t user; } WaveBlock;
typedef struct { void *buffer; uint64_t size; uint32_t type, channels; } RenderBufferInfo;
typedef struct { void *host_buffer; uint64_t host_size; uintptr_t reserved[5]; uintptr_t user_data; } ContextBufferInfo;
typedef struct { uint64_t size; char name[16]; uint32_t flags, max_grain_samples, num_grain_samples, sample_rate; } SystemOption;
typedef struct { uint64_t size; char name[16]; uint32_t flags, max_grain_samples, max_voices; } RackOption;
typedef struct {
    uintptr_t callback_data; void *voice; uint32_t flag, reserved;
    uintptr_t user_data; const void *data; uint32_t data_size, repeated, attributes, reserved2;
} CallbackInfo;
typedef void (ABI *CallbackFn)(const CallbackInfo *);

typedef struct Ngs2Handle Ngs2Handle;
typedef struct Voice {
    int kind, channels;                 /* output channels */
    uint32_t state;
    struct { Ngs2Handle *dest; float volume; int matrix, used; } port[MAX_PORTS];
    struct { int levels; float level[MAX_CH*MAX_CH]; } matrix[MAX_MATRICES];
    CallbackFn callback; uintptr_t callback_data; uint32_t callback_flags;
    float input[MAX_CH][MAX_GRAIN];     /* submixer/mastering: sum of the patched voices */
    int level;                           /* graph depth (render order) */
    uint32_t output_id;                  /* mastering */
    /* sampler */
    WaveFormat format;
    const uint8_t *data;
    WaveBlock block[MAX_BLOCKS]; int blocks, current;
    uint32_t repeated, byte_pos, sample_pos;   /* within the current block */
    float pitch; double frac;
    float prev[MAX_CH], next[MAX_CH]; int primed;
    float decoded[256*MAX_CH]; int decoded_count, decoded_pos;   /* frames of the codec */
    int16_t vag_hist[MAX_CH][2];
    void *at9; int at9_frame_samples, at9_channels;
    uint64_t decoded_samples;
} Voice;
struct Ngs2Handle {
    uint32_t magic, type;
    Ngs2Handle *owner;
    uint32_t rack_id, index, voices, kind;
    Ngs2Handle *voice;          /* racks: their voices */
    Voice *v;                   /* voices: state */
    Ngs2Handle *next_rack;      /* systems: racks; racks: next */
    uint32_t grain;             /* systems */
};
#define NGS2_MAGIC UINT32_C(0x4e475332)

/* Recursive: block-end callbacks run inside a render and may control voices. */
static pthread_mutex_t ngs2_lock;
static pthread_once_t ngs2_lock_once=PTHREAD_ONCE_INIT;
static void ngs2_lock_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ngs2_lock,&attr);
    pthread_mutexattr_destroy(&attr);
}
static void lock_ngs2(void) { pthread_once(&ngs2_lock_once,ngs2_lock_init); pthread_mutex_lock(&ngs2_lock); }
static atomic_size_t ngs2_renders, ngs2_params;
static int ngs2_valid(const Ngs2Handle *h, uint32_t type) { return h && h->magic==NGS2_MAGIC && h->type==type; }
static long trace_limit(void) {
    static long limit=-1;
    if (limit<0) { const char *v=getenv("BB_NGS2_TRACE"); limit=v ? atol(v) : 200; }
    return limit;
}
static int tracing(void) { return (long)atomic_load(&ngs2_params)<trace_limit(); }

/* ---------------- handles ---------------- */
static int rack_kind(uint32_t id) {
    switch (id>>12) { case 1: return KIND_SAMPLER; case 2: return KIND_SUBMIXER; case 3: return KIND_MASTERING; }
    return (id&0xF000)==0x4000 ? (int)((id&0xF)-1)%3 : KIND_OTHER;   /* custom racks 0x4001..3 */
}
static Ngs2Handle *ngs2_new(uint32_t type, Ngs2Handle *owner) {
    Ngs2Handle *h=calloc(1,sizeof(*h));
    if (h) { h->magic=NGS2_MAGIC; h->type=type; h->owner=owner; }
    return h;
}
static ABI int32_t ngs2_system_create(const SystemOption *option, const void *allocator, Ngs2Handle **out) {
    (void)allocator;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    Ngs2Handle *h=ngs2_new(HANDLE_SYSTEM,NULL);
    h->grain=option && option->size>=offsetof(SystemOption,num_grain_samples)+4 && option->num_grain_samples ?
             option->num_grain_samples : 256;
    if (h->grain>MAX_GRAIN) h->grain=MAX_GRAIN;
    *out=h;
    printf("Runtime: Ngs2 system created (grain %u, rate %u)\n",h->grain,
           option && option->size>=offsetof(SystemOption,sample_rate)+4 ? option->sample_rate : 0);
    return 0;
}
static ABI int32_t ngs2_system_destroy(Ngs2Handle *h, ContextBufferInfo *out) {
    if (!ngs2_valid(h,HANDLE_SYSTEM)) return NGS2_ERR_INVALID_HANDLE;
    if (out) memset(out,0,sizeof(*out));
    lock_ngs2();
    h->magic=0;
    pthread_mutex_unlock(&ngs2_lock);
    return 0;
}
static ABI int32_t ngs2_rack_query(uint32_t rack_id, const RackOption *option, ContextBufferInfo *out) {
    (void)rack_id; (void)option;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    memset(out,0,sizeof(*out)); out->host_size=4096;
    return 0;
}
static void voice_reset(Voice *v, int kind) {
    void *at9=v->at9;
    if (at9) Atrac9ReleaseHandle(at9);
    memset(v,0,sizeof(*v));
    v->kind=kind; v->channels=kind==KIND_SAMPLER ? 1 : 2; v->pitch=1.0f; v->state=0;
    for (int p=0;p<MAX_PORTS;++p) { v->port[p].volume=1.0f; v->port[p].matrix=-1; }
}
static int32_t rack_create(Ngs2Handle *system, uint32_t rack_id, const RackOption *option, Ngs2Handle **out) {
    if (!ngs2_valid(system,HANDLE_SYSTEM)) return NGS2_ERR_INVALID_HANDLE;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    uint32_t voices=option && option->size>=offsetof(RackOption,max_voices)+4 && option->max_voices ? option->max_voices : 1;
    if (voices>4096) voices=4096;
    Ngs2Handle *rack=ngs2_new(HANDLE_RACK,system);
    if (!rack) return NGS2_ERR_INVALID_OUT_ADDRESS;
    rack->rack_id=rack_id; rack->voices=voices; rack->kind=(uint32_t)rack_kind(rack_id);
    rack->voice=calloc(voices,sizeof(Ngs2Handle));
    for (uint32_t i=0;i<voices;++i) {
        rack->voice[i]=(Ngs2Handle){.magic=NGS2_MAGIC,.type=HANDLE_VOICE,.owner=rack,.rack_id=rack_id,.index=i,
                                    .kind=rack->kind,.v=calloc(1,sizeof(Voice))};
        voice_reset(rack->voice[i].v,(int)rack->kind);
    }
    lock_ngs2();
    rack->next_rack=system->next_rack; system->next_rack=rack;
    pthread_mutex_unlock(&ngs2_lock);
    printf("Runtime: Ngs2 rack %#x (%s) with %u voices\n",rack_id,
           rack->kind==KIND_SAMPLER ? "sampler" : rack->kind==KIND_SUBMIXER ? "submixer" :
           rack->kind==KIND_MASTERING ? "mastering" : "other",voices);
    *out=rack; return 0;
}
static ABI int32_t ngs2_rack_create(Ngs2Handle *system, uint32_t rack_id, const RackOption *option,
                                    const ContextBufferInfo *buffer, Ngs2Handle **out) {
    (void)buffer; return rack_create(system,rack_id,option,out);
}
static ABI int32_t ngs2_rack_create_alloc(Ngs2Handle *system, uint32_t rack_id, const RackOption *option,
                                          const void *allocator, Ngs2Handle **out) {
    (void)allocator; return rack_create(system,rack_id,option,out);
}
static ABI int32_t ngs2_rack_destroy(Ngs2Handle *rack, ContextBufferInfo *out) {
    if (!ngs2_valid(rack,HANDLE_RACK)) return NGS2_ERR_INVALID_HANDLE;
    if (out) memset(out,0,sizeof(*out));
    lock_ngs2();
    Ngs2Handle *system=rack->owner;
    for (Ngs2Handle **p=&system->next_rack;*p;p=&(*p)->next_rack) if (*p==rack) { *p=rack->next_rack; break; }
    for (uint32_t i=0;i<rack->voices;++i) rack->voice[i].v->state=0;   /* handles stay valid for late callers */
    /* Voices of other racks patched here stop sending. */
    for (Ngs2Handle *r=system->next_rack;r;r=r->next_rack)
        for (uint32_t i=0;i<r->voices;++i)
            for (int p=0;p<MAX_PORTS;++p) if (r->voice[i].v->port[p].dest && r->voice[i].v->port[p].dest->owner==rack)
                r->voice[i].v->port[p].dest=NULL;
    rack->magic=0;
    pthread_mutex_unlock(&ngs2_lock);
    return 0;
}
static ABI int32_t ngs2_rack_voice(Ngs2Handle *rack, uint32_t index, Ngs2Handle **out) {
    if (!ngs2_valid(rack,HANDLE_RACK)) return NGS2_ERR_INVALID_HANDLE;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    if (index>=rack->voices) return NGS2_ERR_INVALID_VOICE_INDEX;
    *out=&rack->voice[index]; return 0;
}

/* ---------------- parameters ---------------- */
static void trace_param(const Ngs2Handle *voice, const ParamHeader *p) {
    const uint32_t *w=(const uint32_t *)(p+1);
    unsigned words=p->size>sizeof(*p) ? (unsigned)(p->size-sizeof(*p))/4 : 0;
    char line[256]; int n=snprintf(line,sizeof(line),"Ngs2 param: rack %#x voice %u id %#x size %u next %d:",
                                   voice->rack_id,voice->index,p->id,p->size,p->next);
    for (unsigned i=0;i<words && i<12 && n<(int)sizeof(line)-12;++i) n+=snprintf(line+n,sizeof(line)-(size_t)n," %08x",w[i]);
    puts(line);
}
static void voice_event(Voice *v, uint32_t event) {
    switch (event) {
    case EV_PLAY:
        v->state=ST_INUSE|ST_PLAYING;
        if (v->kind==KIND_SAMPLER) {
            v->current=0; v->repeated=0; v->byte_pos=0; v->sample_pos=0; v->frac=0; v->primed=0;
            v->decoded_count=v->decoded_pos=0; memset(v->vag_hist,0,sizeof(v->vag_hist));
            if (v->at9) Atrac9InitDecoder(v->at9,(unsigned char *)&v->format.config);
            if (!v->blocks) v->state|=ST_EMPTY;
        }
        break;
    case EV_STOP: case EV_STOP_IMM: case EV_KILL:
        v->state=(v->state & ST_INUSE)|ST_STOPPED;
        if (event==EV_KILL) v->state=0;
        break;
    case EV_PAUSE: v->state|=ST_PAUSED; break;
    case EV_RESUME: v->state&=~(uint32_t)ST_PAUSED; break;
    }
}
static void sampler_setup(Voice *v, const WaveFormat *f) {
    v->format=*f;
    v->channels=f->channels>=1 && f->channels<=MAX_CH ? (int)f->channels : 1;
    if (v->at9) { Atrac9ReleaseHandle(v->at9); v->at9=NULL; }
    if (f->type==W_AT9) {
        v->at9=Atrac9GetHandle();
        if (Atrac9InitDecoder(v->at9,(unsigned char *)&v->format.config)==0) {
            Atrac9CodecInfo info; Atrac9GetCodecInfo(v->at9,&info);
            v->at9_frame_samples=info.frameSamples; v->at9_channels=info.channels;
        } else {
            v->at9_frame_samples=0;
        }
    }
    v->blocks=0; v->state=ST_INUSE;
}
static void apply_param(Ngs2Handle *voice, const ParamHeader *p) {
    Voice *v=voice->v;
    const uint8_t *body=(const uint8_t *)(p+1);
    switch (p->id) {
    case P_MATRIX_LEVELS: {
        uint32_t id=*(const uint32_t *)body, count=*(const uint32_t *)(body+4);
        const float *levels=*(const float *const *)(body+8);
        if (id<MAX_MATRICES && levels) {
            if (count>MAX_CH*MAX_CH) count=MAX_CH*MAX_CH;
            v->matrix[id].levels=(int)count; memcpy(v->matrix[id].level,levels,count*sizeof(float));
        }
        break;
    }
    case P_PORT_MATRIX: { uint32_t port=*(const uint32_t *)body; if (port<MAX_PORTS) v->port[port].matrix=*(const int32_t *)(body+4); break; }
    case P_PORT_VOLUME: { uint32_t port=*(const uint32_t *)body; if (port<MAX_PORTS) v->port[port].volume=*(const float *)(body+4); break; }
    case P_PORT_DELAY: break;
    case P_PATCH: {
        uint32_t port=*(const uint32_t *)body;
        Ngs2Handle *dest=*(Ngs2Handle *const *)(body+8);
        if (port<MAX_PORTS) { v->port[port].dest=ngs2_valid(dest,HANDLE_VOICE) ? dest : NULL; v->port[port].used=1; }
        break;
    }
    case P_EVENT: voice_event(v,*(const uint32_t *)body); break;
    case P_CALLBACK:
        v->callback=*(CallbackFn const *)body; v->callback_data=*(const uintptr_t *)(body+8);
        v->callback_flags=*(const uint32_t *)(body+16);
        break;
    case S_SETUP: if (v->kind==KIND_SAMPLER) sampler_setup(v,(const WaveFormat *)body); break;
    case S_BLOCKS: {
        if (v->kind!=KIND_SAMPLER) break;
        const uint8_t *data=*(const uint8_t *const *)body;
        uint32_t count=*(const uint32_t *)(body+12);
        const WaveBlock *blocks=*(const WaveBlock *const *)(body+16);
        if (!blocks || !count) break;
        /* A playing voice queues them after its own (streaming); otherwise they replace. */
        int append=(v->state & ST_PLAYING) && !(v->state & ST_EMPTY) && data==v->data;
        if (!append) { v->blocks=0; v->current=0; v->repeated=0; v->byte_pos=0; v->sample_pos=0; }
        v->data=data;
        for (uint32_t i=0;i<count && v->blocks<MAX_BLOCKS;++i) v->block[v->blocks++]=blocks[i];
        v->state&=~(uint32_t)ST_EMPTY;
        break;
    }
    case S_ADDRESS: {
        const uint8_t *from=*(const uint8_t *const *)body, *to=*(const uint8_t *const *)(body+8);
        if (v->data==from) v->data=to;
        break;
    }
    case S_FRAME_OFFSET: v->format.frame_offset=*(const uint32_t *)body; break;
    case S_EXIT_LOOP: if (v->current<v->blocks) v->block[v->current].repeats=0; break;
    case S_PITCH: { float r=*(const float *)body; v->pitch=r>0.0f && r<16.0f ? r : 1.0f; break; }
    case X_SETUP: { uint32_t ch=*(const uint32_t *)body; v->channels=ch>=1 && ch<=MAX_CH ? (int)ch : 2; v->state|=ST_INUSE; break; }
    case M_SETUP: { uint32_t ch=*(const uint32_t *)body; v->channels=ch>=1 && ch<=MAX_CH ? (int)ch : 2; v->state|=ST_INUSE; break; }
    case M_OUTPUT: v->output_id=*(const uint32_t *)body; break;
    default: break;
    }
}
static ABI int32_t ngs2_voice_control(Ngs2Handle *voice, const ParamHeader *params) {
    if (!ngs2_valid(voice,HANDLE_VOICE)) return NGS2_ERR_INVALID_HANDLE;
    lock_ngs2();
    const ParamHeader *p=params;
    for (int n=0;p && n<256;++n) {
        if (tracing()) trace_param(voice,p);
        atomic_fetch_add(&ngs2_params,1);
        apply_param(voice,p);
        if (!p->next) break;
        p=(const ParamHeader *)((const uint8_t *)p+p->next);
    }
    pthread_mutex_unlock(&ngs2_lock);
    return 0;
}
typedef struct {
    uint32_t flags; float envelope, peak; uint32_t reserved;
    uint64_t decoded_samples, decoded_size, user_data; const void *waveform;
} SamplerState;
static ABI int32_t ngs2_voice_state(Ngs2Handle *voice, void *state, uint64_t size) {
    if (!ngs2_valid(voice,HANDLE_VOICE)) return NGS2_ERR_INVALID_HANDLE;
    if (!state) return NGS2_ERR_INVALID_OUT_ADDRESS;
    lock_ngs2();
    const Voice *v=voice->v;
    SamplerState s={.flags=v->state,.envelope=1.0f,.peak=0.0f,.decoded_samples=v->decoded_samples,
                    .waveform=v->data};
    if (v->current<v->blocks) s.user_data=v->block[v->current].user;
    pthread_mutex_unlock(&ngs2_lock);
    memset(state,0,size);
    memcpy(state,&s,size<sizeof(s) ? size : sizeof(s));
    return 0;
}
static ABI int32_t ngs2_voice_flags(Ngs2Handle *voice, uint32_t *flags) {
    if (!ngs2_valid(voice,HANDLE_VOICE)) return NGS2_ERR_INVALID_HANDLE;
    if (!flags) return NGS2_ERR_INVALID_OUT_ADDRESS;
    lock_ngs2();
    *flags=voice->v->state;
    pthread_mutex_unlock(&ngs2_lock);
    return 0;
}

/* ---------------- sampler decoding ---------------- */
static uint32_t pcm_bytes(uint32_t type) {
    switch (type) {
    case W_I8: case W_U8: return 1;
    case W_I16L: case W_I16B: return 2;
    case W_I24L: case W_I24B: return 3;
    case W_I32L: case W_I32B: case W_F32L: case W_F32B: return 4;
    case W_F64L: case W_F64B: return 8;
    }
    return 0;
}
static float pcm_sample(uint32_t type, const uint8_t *s) {
    switch (type) {
    case W_I8: return (int8_t)s[0]/128.0f;
    case W_U8: return (s[0]-128)/128.0f;
    case W_I16L: return (int16_t)(s[0]|s[1]<<8)/32768.0f;
    case W_I16B: return (int16_t)(s[1]|s[0]<<8)/32768.0f;
    case W_I24L: return (float)((int32_t)((uint32_t)s[0]<<8|(uint32_t)s[1]<<16|(uint32_t)s[2]<<24)>>8)/8388608.0f;
    case W_I24B: return (float)((int32_t)((uint32_t)s[2]<<8|(uint32_t)s[1]<<16|(uint32_t)s[0]<<24)>>8)/8388608.0f;
    case W_I32L: { int32_t x; memcpy(&x,s,4); return (float)x/2147483648.0f; }
    case W_I32B: { uint32_t x=(uint32_t)s[3]|(uint32_t)s[2]<<8|(uint32_t)s[1]<<16|(uint32_t)s[0]<<24; return (float)(int32_t)x/2147483648.0f; }
    case W_F32L: { float x; memcpy(&x,s,4); return x; }
    case W_F32B: { uint32_t x=(uint32_t)s[3]|(uint32_t)s[2]<<8|(uint32_t)s[1]<<16|(uint32_t)s[0]<<24; float f; memcpy(&f,&x,4); return f; }
    case W_F64L: { double x; memcpy(&x,s,8); return (float)x; }
    case W_F64B: { uint8_t r[8]; for (int i=0;i<8;++i) r[i]=s[7-i]; double x; memcpy(&x,r,8); return (float)x; }
    }
    return 0.0f;
}
/* VAG (PS ADPCM): 16-byte frames of 28 samples, mono per channel (interleaved by frame). */
static void vag_decode(Voice *v, const uint8_t *frame, int ch, float *out, int stride) {
    static const int coef[5][2]={{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
    int shift=frame[0]&0xF, filter=(frame[0]>>4)&0x7;
    if (filter>4) filter=0;
    int16_t *h=v->vag_hist[ch];
    for (int i=0;i<28;++i) {
        int nibble=(frame[2+i/2]>>((i&1)*4))&0xF;
        int s=(int16_t)(nibble<<12)>>shift;
        s+=(h[0]*coef[filter][0]+h[1]*coef[filter][1])>>6;
        if (s>32767) s=32767; else if (s<-32768) s=-32768;
        h[1]=h[0]; h[0]=(int16_t)s;
        out[i*stride]=s/32768.0f;
    }
}
static void block_end(Ngs2Handle *voice) {
    Voice *v=voice->v;
    WaveBlock *b=&v->block[v->current];
    if (v->callback) {
        CallbackInfo info={.callback_data=v->callback_data,.voice=voice,.flag=1,.user_data=b->user,
                           .data=v->data+b->offset,.data_size=b->size,.repeated=v->repeated};
        v->callback(&info);
    }
    if (b->repeats==UINT32_MAX || v->repeated<b->repeats) {
        ++v->repeated;
    } else {
        ++v->current; v->repeated=0;
    }
    v->byte_pos=0; v->sample_pos=0; v->decoded_count=v->decoded_pos=0;
    memset(v->vag_hist,0,sizeof(v->vag_hist));
    if (v->current>=v->blocks) v->state=(v->state&~(uint32_t)ST_PLAYING)|ST_STOPPED|ST_EMPTY;
}
/* Fills v->decoded with the next frames of the current block; 0 at the end of the waveform. */
static int decode_more(Ngs2Handle *voice) {
    Voice *v=voice->v;
    for (int guard=0;guard<8;++guard) {
        if (v->current>=v->blocks || !v->data) return 0;
        WaveBlock *b=&v->block[v->current];
        const uint8_t *base=v->data+b->offset;
        uint32_t limit=b->samples ? b->samples : UINT32_MAX;
        int ch=v->channels;
        v->decoded_pos=0; v->decoded_count=0;
        if (v->sample_pos<limit && v->byte_pos<b->size) {
            uint32_t type=v->format.type, bytes=pcm_bytes(type);
            if (bytes) {
                uint32_t frame=bytes*(uint32_t)ch, n=0;
                while (n<256 && v->byte_pos+frame<=b->size && v->sample_pos<limit) {
                    for (int c=0;c<ch;++c) v->decoded[n*ch+c]=pcm_sample(type,base+v->byte_pos+(uint32_t)c*bytes);
                    v->byte_pos+=frame; ++v->sample_pos; ++n;
                }
                v->decoded_count=(int)n;
            } else if (type==W_VAG) {
                uint32_t need=16u*(uint32_t)ch;
                if (v->byte_pos+need<=b->size) {
                    for (int c=0;c<ch;++c) vag_decode(v,base+v->byte_pos+16u*(uint32_t)c,c,v->decoded+c,ch);
                    v->byte_pos+=need;
                    uint32_t n=28; if (v->sample_pos+n>limit) n=limit-v->sample_pos;
                    v->sample_pos+=n; v->decoded_count=(int)n;
                } else v->byte_pos=b->size;
            } else if (type==W_AT9 && v->at9 && v->at9_frame_samples>0 && v->at9_frame_samples<=256) {
                int16_t pcm[256*MAX_CH]; int used=0;
                if (Atrac9Decode(v->at9,base+v->byte_pos,pcm,&used)==0 && used>0) {
                    v->byte_pos+=(uint32_t)used;
                    int n=v->at9_frame_samples, ach=v->at9_channels;
                    for (int i=0;i<n;++i) for (int c=0;c<ch;++c) v->decoded[i*ch+c]=(c<ach ? pcm[i*ach+c] : 0)/32768.0f;
                    if (v->sample_pos+(uint32_t)n>limit) n=(int)(limit-v->sample_pos);
                    v->sample_pos+=(uint32_t)n; v->decoded_count=n;
                } else v->byte_pos=b->size;
            } else {
                v->byte_pos=b->size;   /* unsupported format: silence */
            }
            /* Leading samples to skip (block skip and the codec's frame offset, first pass). */
            uint32_t skip=b->skip+(v->current==0 && v->repeated==0 ? v->format.frame_offset : 0);
            uint32_t done=v->sample_pos-(uint32_t)v->decoded_count;
            if (done<skip) {
                uint32_t drop=skip-done; if (drop>(uint32_t)v->decoded_count) drop=(uint32_t)v->decoded_count;
                v->decoded_pos=(int)drop;
            }
            if (v->decoded_pos<v->decoded_count) return 1;
            continue;
        }
        block_end(voice);
    }
    return 0;
}
static int next_frame(Ngs2Handle *voice, float *out) {
    Voice *v=voice->v;
    if (v->decoded_pos>=v->decoded_count && !decode_more(voice)) return 0;
    memcpy(out,v->decoded+v->decoded_pos*v->channels,(size_t)v->channels*sizeof(float));
    ++v->decoded_pos; ++v->decoded_samples;
    return 1;
}
static void sampler_render(Ngs2Handle *voice, float out[MAX_CH][MAX_GRAIN], uint32_t grain) {
    Voice *v=voice->v;
    int ch=v->channels;
    double step=(double)v->pitch*(v->format.rate ? v->format.rate : RATE)/RATE;
    if (!v->primed) {
        if (!next_frame(voice,v->prev)) memset(v->prev,0,sizeof(v->prev));
        if (!next_frame(voice,v->next)) memcpy(v->next,v->prev,sizeof(v->next));
        v->primed=1; v->frac=0;
    }
    for (uint32_t i=0;i<grain;++i) {
        float t=(float)v->frac;
        for (int c=0;c<ch;++c) out[c][i]=v->prev[c]+(v->next[c]-v->prev[c])*t;
        v->frac+=step;
        while (v->frac>=1.0) {
            v->frac-=1.0;
            memcpy(v->prev,v->next,sizeof(v->prev));
            if (!next_frame(voice,v->next)) {
                memset(v->next,0,sizeof(v->next));
                if (!(v->state & ST_PLAYING)) { for (uint32_t j=i+1;j<grain;++j) for (int c=0;c<ch;++c) out[c][j]=0; return; }
            }
        }
    }
}

/* ---------------- graph ---------------- */
static void send_ports(Voice *v, float src[MAX_CH][MAX_GRAIN], uint32_t grain) {
    for (int p=0;p<MAX_PORTS;++p) {
        Ngs2Handle *dest=v->port[p].dest;
        if (!dest || !ngs2_valid(dest,HANDLE_VOICE) || !(dest->v->state & ST_INUSE)) continue;
        Voice *d=dest->v;
        float vol=v->port[p].volume;
        int in=v->channels, out=d->channels;
        const float *m=NULL;
        if (v->port[p].matrix>=0 && v->port[p].matrix<MAX_MATRICES && v->matrix[v->port[p].matrix].levels>=in*out)
            m=v->matrix[v->port[p].matrix].level;
        for (int o=0;o<out;++o) {
            for (int i=0;i<in;++i) {
                float g;
                if (m) g=m[o*in+i];
                else if (in==1) g=o<2 ? 1.0f : 0.0f;               /* mono to the front pair */
                else g=o==i ? 1.0f : 0.0f;
                g*=vol;
                if (g==0.0f) continue;
                for (uint32_t s=0;s<grain;++s) d->input[o][s]+=src[i][s]*g;
            }
        }
    }
}
static int voice_level(Ngs2Handle *voice, int depth) {
    Voice *v=voice->v;
    if (depth>16) return 0;
    int level=0;
    for (int p=0;p<MAX_PORTS;++p)
        if (v->port[p].dest && ngs2_valid(v->port[p].dest,HANDLE_VOICE)) {
            int l=voice_level(v->port[p].dest,depth+1)+1;
            if (l>level) level=l;
        }
    return level;
}
static void write_output(const RenderBufferInfo *b, uint32_t offset, Voice *m, uint32_t grain) {
    uint32_t ch=b->channels ? b->channels : 2, bytes=b->type==W_I16L ? 2 : 4;
    for (uint32_t s=0;s<grain;++s) {
        uint64_t at=((uint64_t)(offset+s)*ch)*bytes;
        if (at+(uint64_t)ch*bytes>b->size) return;
        for (uint32_t c=0;c<ch;++c) {
            float x=(int)c<m->channels ? m->input[c][s] : 0.0f;
            if (m->channels==2 && ch>2 && c>=2) x=0.0f;
            uint8_t *dst=(uint8_t *)b->buffer+at+(uint64_t)c*bytes;
            if (bytes==2) {
                float y=x*32767.0f; int16_t v=(int16_t)(y>32767.0f ? 32767 : y<-32768.0f ? -32768 : (int)y);
                int16_t old; memcpy(&old,dst,2); int sum=old+v; if (sum>32767) sum=32767; else if (sum<-32768) sum=-32768;
                v=(int16_t)sum; memcpy(dst,&v,2);
            } else {
                float old; memcpy(&old,dst,4); old+=x; memcpy(dst,&old,4);
            }
        }
    }
}
static float scratch[MAX_CH][MAX_GRAIN];
static ABI int32_t ngs2_system_render(Ngs2Handle *h, const RenderBufferInfo *buffers, uint32_t count) {
    if (!ngs2_valid(h,HANDLE_SYSTEM)) return NGS2_ERR_INVALID_HANDLE;
    if (!buffers && count) return NGS2_ERR_INVALID_BUFFER_INFO;
    for (uint32_t i=0;i<count;++i) if (buffers[i].buffer) memset(buffers[i].buffer,0,buffers[i].size);
    static int described;
    if (!described && count) {
        described=1;
        for (uint32_t i=0;i<count;++i)
            printf("Runtime: Ngs2 render buffer %u: %llu bytes, type %#x, %u channels\n",i,
                   (unsigned long long)buffers[i].size,buffers[i].type,buffers[i].channels);
    }
    uint32_t frames=0;
    if (count) {
        uint32_t ch=buffers[0].channels ? buffers[0].channels : 2, bytes=buffers[0].type==W_I16L ? 2 : 4;
        frames=(uint32_t)(buffers[0].size/((uint64_t)ch*bytes));
    }
    lock_ngs2();
    uint32_t grain=h->grain;
    for (uint32_t offset=0;offset<frames;offset+=grain) {
        uint32_t n=frames-offset<grain ? frames-offset : grain;
        int max_level=0;
        for (Ngs2Handle *r=h->next_rack;r;r=r->next_rack)
            for (uint32_t i=0;i<r->voices;++i) {
                Voice *v=r->voice[i].v;
                if (r->kind!=KIND_SAMPLER) {
                    for (int c=0;c<MAX_CH;++c) memset(v->input[c],0,n*sizeof(float));
                    if (v->state & ST_INUSE) { v->level=voice_level(&r->voice[i],0); if (v->level>max_level) max_level=v->level; }
                }
            }
        /* Samplers first, then mixers from the farthest from the output. */
        for (Ngs2Handle *r=h->next_rack;r;r=r->next_rack) {
            if (r->kind!=KIND_SAMPLER) continue;
            for (uint32_t i=0;i<r->voices;++i) {
                Voice *v=r->voice[i].v;
                if (!(v->state & ST_PLAYING) || (v->state & ST_PAUSED)) continue;
                sampler_render(&r->voice[i],scratch,n);
                send_ports(v,scratch,n);
            }
        }
        for (int level=max_level;level>=0;--level)
            for (Ngs2Handle *r=h->next_rack;r;r=r->next_rack) {
                if (r->kind==KIND_SAMPLER) continue;
                for (uint32_t i=0;i<r->voices;++i) {
                    Voice *v=r->voice[i].v;
                    if (!(v->state & ST_INUSE) || v->level!=level) continue;
                    if (r->kind==KIND_MASTERING || level==0) {
                        uint32_t out=v->output_id<count ? v->output_id : 0;
                        if (count && buffers[out].buffer) write_output(&buffers[out],offset,v,n);
                    } else {
                        send_ports(v,v->input,n);
                    }
                }
            }
    }
    pthread_mutex_unlock(&ngs2_lock);
    atomic_fetch_add(&ngs2_renders,1);
    return 0;
}

static const RuntimeExport ngs2_exports[]={
    {"sceNgs2SystemCreateWithAllocator",ngs2_system_create}, {"sceNgs2SystemDestroy",ngs2_system_destroy},
    {"sceNgs2SystemRender",ngs2_system_render}, {"sceNgs2RackQueryBufferSize",ngs2_rack_query},
    {"sceNgs2RackCreate",ngs2_rack_create}, {"sceNgs2RackCreateWithAllocator",ngs2_rack_create_alloc},
    {"sceNgs2RackDestroy",ngs2_rack_destroy}, {"sceNgs2RackGetVoiceHandle",ngs2_rack_voice},
    {"sceNgs2VoiceControl",ngs2_voice_control}, {"sceNgs2VoiceGetState",ngs2_voice_state},
    {"sceNgs2VoiceGetStateFlags",ngs2_voice_flags},
};
uintptr_t runtime_ngs2_resolve(const char *name) { return RUNTIME_LOOKUP(ngs2_exports,name); }
size_t runtime_ngs2_renders(void) { return atomic_load(&ngs2_renders); }
