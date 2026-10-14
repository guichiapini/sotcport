/* libSceNgs2 for CUSA08809 (SndX on Ngs2): a software voice graph.
 *
 * Racks hold voices. Sampler voices play waveform blocks (PCM, VAG ADPCM, ATRAC9 through
 * LibAtrac9) at a pitch, and send each port's output, through an optional channel matrix and
 * a volume, to the input of the voice it is patched to. Submixer voices sum their inputs and
 * send them on the same way; mastering voices write the sum into the buffers the game passes
 * to sceNgs2SystemRender (which it then hands to AudioOut). Effects (filters, envelopes,
 * reverb, compressors, limiters) are accepted and not applied; custom racks (0x400x) act as
 * the plain rack of their class.
 *
 * Submixer and sampler voices may carry a user effect: a handler in the game that processes
 * (or produces) the voice's signal each grain; SndX mixes its sounds there. The first
 * parameters of each ID are logged (BB_NGS2_TRACE). */
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
enum { MAX_CH=8, MAX_PORTS=8, MAX_MATRICES=4, MAX_BLOCKS=32, MAX_GRAIN=1024, RATE=48000 };

/* Voice parameter IDs, as CUSA08809 sends them: common ones from 1; per rack class, numbered
 * from 0 in the order of the SDK's parameter structures (a submixer's setup is 0x20000000, its
 * user effect 0x20000004 with a handler in the game, a mastering voice's output 0x30000005). */
enum {
    P_MATRIX_LEVELS=1, P_PORT_VOLUME=2, P_PORT_MATRIX=3, P_PORT_DELAY=4, P_PATCH=5, P_EVENT=6,
    P_CALLBACK=7,
    S_SETUP=0x10000000, S_BLOCKS=0x10000001, S_ADDRESS=0x10000002, S_FRAME_OFFSET=0x10000003,
    S_EXIT_LOOP=0x10000004, S_PITCH=0x10000005, S_USER_FX=0x10000008,
    X_SETUP=0x20000000, X_USER_FX=0x20000004,
    M_SETUP=0x30000000, M_OUTPUT=0x30000005,
    C_SETUP=0x40020000,   /* custom submixer: input and output channels */
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
typedef struct {
    float **channels; uintptr_t user_data[3]; uint32_t flags, num_channels, num_grain_samples, sample_rate;
} UserFxContext;
typedef int32_t (ABI *UserFxFn)(UserFxContext *);

typedef struct Ngs2Handle Ngs2Handle;
typedef struct Voice {
    int kind, channels;                 /* output channels */
    uint32_t state;
    struct { Ngs2Handle *dest; float volume; int matrix, used; } port[MAX_PORTS];
    struct { int levels; float level[MAX_CH*MAX_CH]; } matrix[MAX_MATRICES];
    CallbackFn callback; uintptr_t callback_data; uint32_t callback_flags;
    UserFxFn fx; uintptr_t fx_data[3];  /* the game's own processing of this voice's signal */
    int fx_started;                      /* the first call carries the start flag */
    float input[MAX_CH][MAX_GRAIN];     /* submixer/mastering: sum of the patched voices */
    int level;                           /* graph depth (render order) */
    uint32_t output_id;                  /* mastering */
    /* sampler */
    WaveFormat format;
    const uint8_t *data;
    WaveBlock block[MAX_BLOCKS]; const uint8_t *block_base[MAX_BLOCKS]; int blocks, current;
    uint64_t decoded_bytes;             /* waveform bytes consumed (the game streams by it) */
    uint32_t repeated, byte_pos, sample_pos;   /* within the current block */
    float pitch; double frac;
    float prev[MAX_CH], next[MAX_CH]; int primed;
    float decoded[256*MAX_CH]; int decoded_count, decoded_pos;   /* frames of the codec */
    int16_t vag_hist[MAX_CH][2];
    void *at9; int at9_frame_samples, at9_channels, at9_superframe_size, at9_frames_per_superframe;
    uint32_t at9_superframe_start; int at9_frame_in_superframe;   /* superframes have a fixed size */
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
/* BB_NGS2_TRACE=N: the first N parameters of each ID per rack type are logged (default 12). */
static int tracing(uint32_t rack_id, uint32_t id) {
    static long limit=-1;
    static struct { uint32_t rack, id, count; } seen[256];
    if (limit<0) { const char *v=getenv("BB_NGS2_TRACE"); limit=v ? atol(v) : 12; }
    for (int i=0;i<256;++i) {
        if ((seen[i].id==id && seen[i].rack==rack_id) || !seen[i].count) {
            seen[i].id=id; seen[i].rack=rack_id;
            return (long)seen[i].count++<limit;
        }
    }
    return 0;
}

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
static int trace_on(void) { const char *t=getenv("BB_NGS2_TRACE"); return !t || atol(t)!=0; }
static void trace_param(const Ngs2Handle *voice, const ParamHeader *p) {
    const uint32_t *w=(const uint32_t *)(p+1);
    unsigned words=p->size>sizeof(*p) ? (unsigned)(p->size-sizeof(*p))/4 : 0;
    char line[256]; int n=snprintf(line,sizeof(line),"Ngs2 param: rack %#x voice %u id %#x size %u next %d:",
                                   voice->rack_id,voice->index,p->id,p->size,p->next);
    for (unsigned i=0;i<words && i<12 && n<(int)sizeof(line)-12;++i) n+=snprintf(line+n,sizeof(line)-(size_t)n," %08x",w[i]);
    puts(line);
}
/* ATRAC9 configuration bytes: the format holds them with the first (0xFE) in the top byte. */
static void at9_config(const Voice *v, unsigned char out[4]) {
    uint32_t c=v->format.config;
    out[0]=(unsigned char)(c>>24); out[1]=(unsigned char)(c>>16); out[2]=(unsigned char)(c>>8); out[3]=(unsigned char)c;
}
static void voice_event(Voice *v, uint32_t event) {
    switch (event) {
    case EV_PLAY:
        v->state=ST_INUSE|ST_PLAYING;
        if (v->kind==KIND_SAMPLER) {
            v->current=0; v->repeated=0; v->byte_pos=0; v->sample_pos=0; v->frac=0; v->primed=0;
            v->decoded_bytes=0; v->decoded_samples=0;
            v->decoded_count=v->decoded_pos=0; memset(v->vag_hist,0,sizeof(v->vag_hist));
            v->at9_frame_in_superframe=0;
            if (v->at9) { unsigned char config[4]; at9_config(v,config); Atrac9InitDecoder(v->at9,config); }
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
        unsigned char config[4]; at9_config(v,config);
        if (Atrac9InitDecoder(v->at9,config)==0) {
            Atrac9CodecInfo info; Atrac9GetCodecInfo(v->at9,&info);
            v->at9_frame_samples=info.frameSamples; v->at9_channels=info.channels;
            v->at9_superframe_size=info.superframeSize; v->at9_frames_per_superframe=info.framesInSuperframe;
            static int shown;
            if (shown++<4) printf("Runtime: Ngs2 ATRAC9 voice: %d ch, %d Hz, %d samples per frame\n",
                                  info.channels,info.samplingRate,info.frameSamples);
        } else {
            v->at9_frame_samples=0;
            static int failed;
            if (failed++<4) printf("Runtime: Ngs2 ATRAC9 config %08x rejected\n",v->format.config);
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
            static int shown;
            if (trace_on() && shown++<6) {
                char line[512]; int n=snprintf(line,sizeof(line),"Ngs2 matrix: rack %#x voice %u id %u, %u levels:",voice->rack_id,voice->index,id,count);
                for (uint32_t k=0;k<count && n<(int)sizeof(line)-8;++k) n+=snprintf(line+n,sizeof(line)-(size_t)n," %.2g",levels[k]);
                puts(line);
            }
        }
        break;
    }
    case P_PORT_MATRIX: { uint32_t port=*(const uint32_t *)body; if (port<MAX_PORTS) v->port[port].matrix=*(const int32_t *)(body+4); break; }
    case P_PORT_VOLUME: { uint32_t port=*(const uint32_t *)body; if (port<MAX_PORTS) v->port[port].volume=*(const float *)(body+4); break; }
    case P_PORT_DELAY: break;
    case P_PATCH: {
        uint32_t port=*(const uint32_t *)body;
        Ngs2Handle *dest=*(Ngs2Handle *const *)(body+8);
        /* A rack as the destination (SndX patches samplers to its one-voice submixer racks)
         * means its first voice. */
        Ngs2Handle *target=ngs2_valid(dest,HANDLE_VOICE) ? dest :
                           ngs2_valid(dest,HANDLE_RACK) && dest->voices ? &dest->voice[0] : NULL;
        if (port<MAX_PORTS) { v->port[port].dest=target; v->port[port].used=1; }
        static int shown[4];
        if (trace_on() && shown[voice->kind&3]++<12)
            printf("Ngs2 patch: rack %#x voice %u port %u -> %p (%s%s%#x, state %#x)\n",voice->rack_id,voice->index,port,
                   (void *)dest,ngs2_valid(dest,HANDLE_VOICE) ? "voice of rack " : ngs2_valid(dest,HANDLE_RACK) ? "RACK " : "invalid ",
                   "",dest && (ngs2_valid(dest,HANDLE_VOICE)||ngs2_valid(dest,HANDLE_RACK)) ? dest->rack_id : 0,
                   ngs2_valid(dest,HANDLE_VOICE) ? dest->v->state : 0);
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
        /* A playing voice queues them after its own (streaming: SndX adds blocks as it sees
         * the decoded size grow); otherwise they replace. Consumed blocks are dropped first. */
        int append=(v->state & ST_PLAYING) && !(v->state & ST_STOPPED);
        if (!append) { v->blocks=0; v->current=0; v->repeated=0; v->byte_pos=0; v->sample_pos=0; }
        if (v->current>0) {
            memmove(v->block,v->block+v->current,(size_t)(v->blocks-v->current)*sizeof(*v->block));
            memmove(v->block_base,v->block_base+v->current,(size_t)(v->blocks-v->current)*sizeof(*v->block_base));
            v->blocks-=v->current; v->current=0;
        }
        v->data=data;
        static int shown;
        for (uint32_t i=0;i<count && v->blocks<MAX_BLOCKS;++i) {
            if (trace_on() && shown++<24)
                printf("Ngs2 block: rack %#x voice %u %s %p+%#x size %#x repeats %d skip %u samples %u (flags %#x)\n",
                       voice->rack_id,voice->index,append ? "append" : "set",(const void *)data,blocks[i].offset,
                       blocks[i].size,(int)blocks[i].repeats,blocks[i].skip,blocks[i].samples,*(const uint32_t *)(body+8));
            v->block_base[v->blocks]=data;
            v->block[v->blocks++]=blocks[i];
        }
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
    case S_USER_FX: case X_USER_FX:
        v->fx=*(UserFxFn const *)body;
        for (int i=0;i<3;++i) v->fx_data[i]=*(const uintptr_t *)(body+8+8*i);
        v->fx_started=0;
        break;
    case C_SETUP: { uint32_t ch=*(const uint32_t *)(body+4); v->channels=ch>=1 && ch<=MAX_CH ? (int)ch : 2; v->state|=ST_INUSE; break; }
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
        if (tracing(voice->rack_id,p->id)) trace_param(voice,p);
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
                    .decoded_size=v->decoded_bytes,.waveform=v->data};
    if (v->current<v->blocks) {
        s.user_data=v->block[v->current].user;
        s.waveform=v->block_base[v->current]+v->block[v->current].offset+v->byte_pos;
    }
    static int shown;
    if (voice->kind==KIND_SAMPLER && trace_on() && shown++<12)
        printf("Ngs2 state query: voice %u, %llu bytes asked, flags %#x, decoded %llu samples / %llu bytes\n",
               voice->index,(unsigned long long)size,v->state,(unsigned long long)v->decoded_samples,
               (unsigned long long)v->decoded_bytes);
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
    if (shift>12) shift=9;
    if (filter>4) filter=0;
    int16_t *h=v->vag_hist[ch];
    for (int i=0;i<28;++i) {
        int nibble=(frame[2+i/2]>>((i&1)*4))&0xF;
        int s=(int16_t)(nibble<<12)>>shift;
        s+=(h[0]*coef[filter][0]+h[1]*coef[filter][1]+32)>>6;
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
                           .data=v->block_base[v->current]+b->offset,.data_size=b->size,.repeated=v->repeated};
        v->callback(&info);
    }
    if (b->repeats==UINT32_MAX || v->repeated<b->repeats) {
        ++v->repeated;
    } else {
        ++v->current; v->repeated=0;
    }
    v->byte_pos=0; v->sample_pos=0; v->decoded_count=v->decoded_pos=0;
    memset(v->vag_hist,0,sizeof(v->vag_hist));
    v->at9_frame_in_superframe=0;
    if (v->current>=v->blocks) v->state=(v->state&~(uint32_t)ST_PLAYING)|ST_STOPPED|ST_EMPTY;
}
/* Fills v->decoded with the next frames of the current block; 0 at the end of the waveform. */
static int decode_more(Ngs2Handle *voice) {
    Voice *v=voice->v;
    for (int guard=0;guard<8;++guard) {
        if (v->current>=v->blocks) return 0;
        WaveBlock *b=&v->block[v->current];
        const uint8_t *base=v->block_base[v->current]+b->offset;
        uint32_t limit=b->samples ? b->samples : UINT32_MAX;
        int ch=v->channels;
        v->decoded_pos=0; v->decoded_count=0;
        if (v->sample_pos<limit && v->byte_pos<b->size) {
            uint32_t type=v->format.type, bytes=pcm_bytes(type);
            if (bytes) {
                uint32_t frame=bytes*(uint32_t)ch, n=0;
                while (n<256 && v->byte_pos+frame<=b->size && v->sample_pos<limit) {
                    for (int c=0;c<ch;++c) v->decoded[n*ch+c]=pcm_sample(type,base+v->byte_pos+(uint32_t)c*bytes);
                    v->byte_pos+=frame; v->decoded_bytes+=frame; ++v->sample_pos; ++n;
                }
                v->decoded_count=(int)n;
            } else if (type==W_VAG) {
                uint32_t need=16u*(uint32_t)ch;
                if (v->byte_pos+need<=b->size) {
                    for (int c=0;c<ch;++c) vag_decode(v,base+v->byte_pos+16u*(uint32_t)c,c,v->decoded+c,ch);
                    v->byte_pos+=need; v->decoded_bytes+=need;
                    uint32_t n=28; if (v->sample_pos+n>limit) n=limit-v->sample_pos;
                    v->sample_pos+=n; v->decoded_count=(int)n;
                } else v->byte_pos=b->size;
            } else if (type==W_AT9 && v->at9 && v->at9_frame_samples>0 && v->at9_frame_samples<=256) {
                int16_t pcm[256*MAX_CH]; int used=0;
                int e=Atrac9Decode(v->at9,base+v->byte_pos,pcm,&used);
                if (e || used<=0) {
                    static int shown;
                    if (trace_on() && shown++<8)
                    {
                        const uint8_t *at=base+v->byte_pos;
                        printf("Ngs2 ATRAC9 decode error %d at byte %#x of %#x (voice %u, frame %d ch, decoded %llu bytes): "
                               "%02x %02x %02x %02x %02x %02x %02x %02x | -16: %02x %02x %02x %02x\n",e,v->byte_pos,b->size,
                               voice->index,v->at9_channels,(unsigned long long)v->decoded_bytes,at[0],at[1],at[2],at[3],
                               at[4],at[5],at[6],at[7],at[-16],at[-15],at[-14],at[-13]);
                    }
                }
                if (e==0 && used>0) {
                    const uint32_t start=v->byte_pos;
                    if (v->at9_frame_in_superframe==0) v->at9_superframe_start=v->byte_pos;
                    v->byte_pos+=(uint32_t)used;
                    /* A superframe has a fixed size; its frames may end before it (padding):
                     * the next superframe starts at its boundary. */
                    if (++v->at9_frame_in_superframe>=v->at9_frames_per_superframe) {
                        v->at9_frame_in_superframe=0;
                        if (v->at9_superframe_size>0)
                            v->byte_pos=v->at9_superframe_start+(uint32_t)v->at9_superframe_size;
                    }
                    v->decoded_bytes+=v->byte_pos-start;   /* padding included */
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
/* Signal levels per stage since the last report (BB_NGS2_TRACE != 0), for silence hunts. */
static struct {
    uint32_t inuse, playing; uint64_t decoded; float sampler, premix, mixer, master; uint32_t mixers, masters, fx;
    uint32_t sent, no_dest, idle_dest, unpatched, sampler_sends; float max_gain, sampler_out;
} stage;
/* Effects whose process reads channel pointers 8..15 take them as outputs. They get buffers of
 * their own holding a copy of the input; if the effect changed them, they are its result (an
 * aliased output cleared before accumulating erased the input: silence), else the input,
 * possibly changed in place, is. */
static float fx_out[MAX_CH][MAX_GRAIN], fx_copy[MAX_CH][MAX_GRAIN];
static void run_user_fx(Voice *v, float buffers[MAX_CH][MAX_GRAIN], uint32_t grain) {
    if (!v->fx) return;
    float *channels[2*MAX_CH];
    for (int c=0;c<MAX_CH;++c) {
        channels[c]=buffers[c]; channels[MAX_CH+c]=fx_out[c];
        memcpy(fx_out[c],buffers[c],grain*sizeof(float));
        memcpy(fx_copy[c],buffers[c],grain*sizeof(float));
    }
    /* Flags: 0x1 on the first call (SndX's effects set up their state there; without it they
     * processed through null buffers), 0x4/0x8 would bypass. */
    UserFxContext context={channels,{v->fx_data[0],v->fx_data[1],v->fx_data[2]},
                           v->fx_started ? 0u : 1u,(uint32_t)v->channels,grain,RATE};
    v->fx_started=1;
    v->fx(&context);
    for (int c=0;c<v->channels;++c)
        if (memcmp(fx_out[c],fx_copy[c],grain*sizeof(float))!=0) {
            for (int k=0;k<v->channels;++k) memcpy(buffers[k],fx_out[k],grain*sizeof(float));
            return;
        }
}
static void send_ports(Voice *v, float src[MAX_CH][MAX_GRAIN], uint32_t grain) {
    for (int p=0;p<MAX_PORTS;++p) {
        Ngs2Handle *dest=v->port[p].dest;
        if (!dest) { if (v->port[p].used) ++stage.no_dest; else if (p==0) ++stage.unpatched; continue; }
        if (!ngs2_valid(dest,HANDLE_VOICE) || !(dest->v->state & ST_INUSE)) { ++stage.idle_dest; continue; }
        ++stage.sent;
        if (v->kind==KIND_SAMPLER) ++stage.sampler_sends;
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
                if (v->kind==KIND_SAMPLER && fabsf(g)>stage.max_gain) stage.max_gain=fabsf(g);
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

static float peak_of(float b[MAX_CH][MAX_GRAIN], int channels, uint32_t n) {
    float p=0;
    for (int c=0;c<channels;++c) for (uint32_t i=0;i<n;++i) { float x=fabsf(b[c][i]); if (x>p) p=x; }
    return p;
}
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
                uint64_t before=v->decoded_samples;
                sampler_render(&r->voice[i],scratch,n);
                stage.decoded+=v->decoded_samples-before;
                { float p=peak_of(scratch,v->channels,n); if (p>stage.sampler) stage.sampler=p; }
                run_user_fx(v,scratch,n);
                send_ports(v,scratch,n);
            }
        }
        for (int level=max_level;level>=0;--level)
            for (Ngs2Handle *r=h->next_rack;r;r=r->next_rack) {
                if (r->kind==KIND_SAMPLER) continue;
                for (uint32_t i=0;i<r->voices;++i) {
                    Voice *v=r->voice[i].v;
                    if (!(v->state & ST_INUSE) || v->level!=level) continue;
                    if (v->fx) ++stage.fx;
                    { float p=peak_of(v->input,v->channels,n); if (p>stage.premix) stage.premix=p; }
                    run_user_fx(v,v->input,n);
                    {
                        float p=peak_of(v->input,v->channels,n);
                        if (r->kind==KIND_MASTERING || level==0) { ++stage.masters; if (p>stage.master) stage.master=p; }
                        else { ++stage.mixers; if (p>stage.mixer) stage.mixer=p; }
                    }
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
    for (Ngs2Handle *r=h->next_rack;r;r=r->next_rack)
        if (r->kind==KIND_SAMPLER)
            for (uint32_t i=0;i<r->voices;++i) {
                if (r->voice[i].v->state & ST_INUSE) ++stage.inuse;
                if (r->voice[i].v->state & ST_PLAYING) ++stage.playing;
            }
    size_t renders=atomic_fetch_add(&ngs2_renders,1)+1;
    static long report=-1;
    if (report<0) { const char *t=getenv("BB_NGS2_TRACE"); report=t && atol(t)==0 ? 0 : 1; }
    if (report && renders%1000==0) {
        printf("Ngs2 levels (1000 renders): samplers in use %u playing %u (avg), decoded %llu, peaks sampler %.4f "
               "mixer inputs %.4f submixer %.4f mastering %.4f; %u submixer, %u mastering, %u effect runs; sends %u, to nothing %u, to idle %u, unpatched %u; sampler sends %u max gain %.3f (vol %s)\n",
               stage.inuse/1000,stage.playing/1000,(unsigned long long)stage.decoded,stage.sampler,stage.premix,stage.mixer,
               stage.master,stage.mixers,stage.masters,stage.fx,stage.sent,stage.no_dest,stage.idle_dest,stage.unpatched,stage.sampler_sends,stage.max_gain,"");
        memset(&stage,0,sizeof(stage));
        int shown=0;
        for (Ngs2Handle *r=h->next_rack;r && shown<4;r=r->next_rack)
            if (r->kind==KIND_SAMPLER)
                for (uint32_t i=0;i<r->voices && shown<4;++i) {
                    Voice *v=r->voice[i].v;
                    if (!(v->state & ST_PLAYING)) continue;
                    ++shown;
                    char line[512]; int n=snprintf(line,sizeof(line),"  voice %u: %d ch type %#x pitch %.2f:",i,v->channels,v->format.type,v->pitch);
                    for (int p=0;p<MAX_PORTS && n<(int)sizeof(line)-60;++p) {
                        if (!v->port[p].dest) continue;
                        float sum=0; int m=v->port[p].matrix;
                        if (m>=0 && m<MAX_MATRICES) for (int k=0;k<v->matrix[m].levels;++k) sum+=fabsf(v->matrix[m].level[k]);
                        n+=snprintf(line+n,sizeof(line)-(size_t)n," port %d vol %.3f matrix %d (%d levels, sum %.3f) -> rack %#x ch %d;",
                                    p,v->port[p].volume,m,m>=0 && m<MAX_MATRICES ? v->matrix[m].levels : 0,sum,
                                    v->port[p].dest->rack_id,v->port[p].dest->v->channels);
                    }
                    puts(line);
                }
    }
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
