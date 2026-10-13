/* libSceNgs2 and libSceAudio3d for CUSA08809, first stage: the API contracts and real-time
 * pacing, without sound. Ngs2 systems, racks and voices are host objects; rendering fills
 * the game's buffers with silence and voices report idle. Audio3d ports keep a virtual
 * output queue that drains at 48 kHz, so a synchronous sceAudio3dPortPush blocks as long
 * as the hardware would (the game paces its audio thread on it). Object PCM is accepted and
 * dropped. Mixing into AudioOut is the next stage. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- Ngs2 ---------------- */
#define NGS2_ERR_INVALID_HANDLE ((int32_t)0x804A0230)
#define NGS2_ERR_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)
#define NGS2_ERR_INVALID_BUFFER_INFO ((int32_t)0x804A0054)
#define NGS2_ERR_INVALID_VOICE_INDEX ((int32_t)0x804A0303)
#define NGS2_RACK_VOICES 256
enum { HANDLE_SYSTEM=1, HANDLE_RACK=2, HANDLE_VOICE=3 };
typedef struct { void *buffer; uint64_t size; uint32_t waveform_type, channels; } RenderBufferInfo;
typedef struct { void *host_buffer; uint64_t host_size; uintptr_t reserved[5]; uintptr_t user_data; } ContextBufferInfo;
typedef struct { uint64_t size; char name[16]; uint32_t flags, max_grain_samples, max_voices; } RackOption;
typedef struct Ngs2Handle {
    uint32_t magic, type;
    struct Ngs2Handle *owner;
    uint32_t rack_id, index, voices;
    struct Ngs2Handle *voice;     /* racks: their voices */
} Ngs2Handle;
#define NGS2_MAGIC UINT32_C(0x4e475332)
static atomic_size_t ngs2_renders;
static int ngs2_valid(const Ngs2Handle *h, uint32_t type) { return h && h->magic==NGS2_MAGIC && h->type==type; }
static Ngs2Handle *ngs2_new(uint32_t type, Ngs2Handle *owner) {
    Ngs2Handle *h=calloc(1,sizeof(*h));
    if (h) { h->magic=NGS2_MAGIC; h->type=type; h->owner=owner; }
    return h;
}
static ABI int32_t ngs2_system_create(const void *option, const void *allocator, Ngs2Handle **out) {
    (void)option; (void)allocator;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    *out=ngs2_new(HANDLE_SYSTEM,NULL);
    printf("Runtime: Ngs2 system created (silent renderer)\n");
    return 0;
}
static ABI int32_t ngs2_system_destroy(Ngs2Handle *h, ContextBufferInfo *out) {
    if (!ngs2_valid(h,HANDLE_SYSTEM)) return NGS2_ERR_INVALID_HANDLE;
    if (out) memset(out,0,sizeof(*out));
    h->magic=0; free(h); return 0;
}
static ABI int32_t ngs2_system_render(Ngs2Handle *h, const RenderBufferInfo *buffers, uint32_t count) {
    if (!ngs2_valid(h,HANDLE_SYSTEM)) return NGS2_ERR_INVALID_HANDLE;
    if (!buffers && count) return NGS2_ERR_INVALID_BUFFER_INFO;
    for (uint32_t i=0;i<count;++i) if (buffers[i].buffer) memset(buffers[i].buffer,0,buffers[i].size);
    atomic_fetch_add(&ngs2_renders,1);
    return 0;
}
static ABI int32_t ngs2_rack_query(uint32_t rack_id, const RackOption *option, ContextBufferInfo *out) {
    (void)rack_id; (void)option;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    memset(out,0,sizeof(*out)); out->host_size=4096;
    return 0;
}
static int32_t rack_create(Ngs2Handle *system, uint32_t rack_id, const RackOption *option, Ngs2Handle **out) {
    if (!ngs2_valid(system,HANDLE_SYSTEM)) return NGS2_ERR_INVALID_HANDLE;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    uint32_t voices=option && option->size>=offsetof(RackOption,max_voices)+4 && option->max_voices ? option->max_voices : NGS2_RACK_VOICES;
    Ngs2Handle *rack=ngs2_new(HANDLE_RACK,system);
    if (!rack) return NGS2_ERR_INVALID_OUT_ADDRESS;
    rack->rack_id=rack_id; rack->voices=voices; rack->voice=calloc(voices,sizeof(Ngs2Handle));
    for (uint32_t i=0;i<voices;++i) rack->voice[i]=(Ngs2Handle){NGS2_MAGIC,HANDLE_VOICE,rack,rack_id,i,0,NULL};
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
    rack->magic=0; /* voice handles may still be queried by late callers: kept */
    return 0;
}
static ABI int32_t ngs2_rack_voice(Ngs2Handle *rack, uint32_t index, Ngs2Handle **out) {
    if (!ngs2_valid(rack,HANDLE_RACK)) return NGS2_ERR_INVALID_HANDLE;
    if (!out) return NGS2_ERR_INVALID_OUT_ADDRESS;
    if (index>=rack->voices) return NGS2_ERR_INVALID_VOICE_INDEX;
    *out=&rack->voice[index]; return 0;
}
static ABI int32_t ngs2_voice_control(Ngs2Handle *voice, const void *params) {
    (void)params;
    return ngs2_valid(voice,HANDLE_VOICE) ? 0 : NGS2_ERR_INVALID_HANDLE;
}
static ABI int32_t ngs2_voice_state(Ngs2Handle *voice, void *state, uint64_t size) {
    if (!ngs2_valid(voice,HANDLE_VOICE)) return NGS2_ERR_INVALID_HANDLE;
    if (!state) return NGS2_ERR_INVALID_OUT_ADDRESS;
    memset(state,0,size); return 0;  /* flags 0: not in use, nothing playing */
}
static ABI int32_t ngs2_voice_flags(Ngs2Handle *voice, uint32_t *flags) {
    if (!ngs2_valid(voice,HANDLE_VOICE)) return NGS2_ERR_INVALID_HANDLE;
    if (!flags) return NGS2_ERR_INVALID_OUT_ADDRESS;
    *flags=0; return 0;
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

/* ---------------- Audio3d ---------------- */
#define A3D_ERR_INVALID_PORT ((int32_t)0x80EA0002)
#define A3D_ERR_INVALID_OBJECT ((int32_t)0x80EA0003)
#define A3D_ERR_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define A3D_ERR_OUT_OF_RESOURCES ((int32_t)0x80EA0005)
#define A3D_ERR_NOT_READY ((int32_t)0x80EA0006)
#define A3D_ERR_NOT_SUPPORTED ((int32_t)0x80EA0007)
#define A3D_PORTS 4
#define A3D_RATE 48000
enum { BUFFER_NO_ADVANCE=0, BUFFER_ADVANCE_NO_PUSH=1, BUFFER_ADVANCE_AND_PUSH=2 };
typedef struct {
    uint64_t size; uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, num_beds;
} A3dOpenParams;
typedef struct {
    int open;
    A3dOpenParams params;
    uint32_t objects, next_object;
    uint32_t queued;            /* frames advanced and not yet played */
    uint64_t played_ns;         /* virtual clock: when the oldest queued frame finishes */
} A3dPort;
static pthread_mutex_t a3d_lock=PTHREAD_MUTEX_INITIALIZER;
static A3dPort ports[A3D_PORTS];
static int a3d_initialized;
static atomic_size_t a3d_pushes;
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static uint64_t frame_ns(const A3dPort *p) { return (uint64_t)p->params.granularity*1000000000/A3D_RATE; }
/* Frames the output has consumed since the last look. */
static void a3d_drain(A3dPort *p) {
    uint64_t now=now_ns(), step=frame_ns(p);
    if (!p->queued) { p->played_ns=now; return; }
    while (p->queued && p->played_ns+step<=now) { p->played_ns+=step; --p->queued; }
    if (!p->queued) p->played_ns=now;
}
static A3dPort *a3d_port(uint32_t id) { return id<A3D_PORTS && ports[id].open ? &ports[id] : NULL; }
static ABI int32_t a3d_initialize(int64_t reserved) {
    if (reserved) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    int already=a3d_initialized; a3d_initialized=1;
    pthread_mutex_unlock(&a3d_lock);
    if (!already) printf("Runtime: Audio3d initialized (paced, silent)\n");
    return already ? A3D_ERR_NOT_READY : 0;
}
static ABI int32_t a3d_terminate(void) {
    pthread_mutex_lock(&a3d_lock);
    a3d_initialized=0; memset(ports,0,sizeof(ports));
    pthread_mutex_unlock(&a3d_lock);
    return 0;
}
static ABI int32_t a3d_port_open(int32_t user, const A3dOpenParams *params, uint32_t *id) {
    (void)user;
    if (id) *id=UINT32_MAX;
    if (!params || !id || !params->granularity) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    int32_t r=A3D_ERR_OUT_OF_RESOURCES;
    if (!a3d_initialized) r=A3D_ERR_NOT_READY;
    else for (uint32_t i=0;i<A3D_PORTS;++i) if (!ports[i].open) {
        ports[i]=(A3dPort){.open=1};
        memcpy(&ports[i].params,params,params->size && params->size<sizeof(A3dOpenParams) ? params->size : sizeof(A3dOpenParams));
        if (!ports[i].params.queue_depth) ports[i].params.queue_depth=2;
        if (!ports[i].params.max_objects) ports[i].params.max_objects=512;
        ports[i].played_ns=now_ns();
        *id=i; r=0;
        printf("Runtime: Audio3d port %u: granularity=%u, objects=%u, queue=%u, mode=%u\n",i,
               ports[i].params.granularity,ports[i].params.max_objects,ports[i].params.queue_depth,ports[i].params.buffer_mode);
        break;
    }
    pthread_mutex_unlock(&a3d_lock);
    return r;
}
static ABI int32_t a3d_port_close(uint32_t id) {
    pthread_mutex_lock(&a3d_lock);
    A3dPort *p=a3d_port(id);
    if (p) p->open=0;
    pthread_mutex_unlock(&a3d_lock);
    return p ? 0 : A3D_ERR_INVALID_PORT;
}
static ABI int32_t a3d_port_set_attribute(uint32_t id, uint32_t attribute, const void *value, uint64_t size) {
    (void)attribute; (void)size;
    if (!a3d_port(id)) return A3D_ERR_INVALID_PORT;
    return value ? 0 : A3D_ERR_INVALID_PARAMETER;
}
static ABI int32_t a3d_object_reserve(uint32_t id, uint32_t *object) {
    if (!object) return A3D_ERR_INVALID_PARAMETER;
    *object=UINT32_MAX;
    pthread_mutex_lock(&a3d_lock);
    A3dPort *p=a3d_port(id);
    int32_t r=!p ? A3D_ERR_INVALID_PORT : p->objects>=p->params.max_objects ? A3D_ERR_OUT_OF_RESOURCES : 0;
    if (!r) { ++p->objects; *object=++p->next_object; }
    pthread_mutex_unlock(&a3d_lock);
    return r;
}
static ABI int32_t a3d_object_unreserve(uint32_t id, uint32_t object) {
    pthread_mutex_lock(&a3d_lock);
    A3dPort *p=a3d_port(id);
    int32_t r=!p ? A3D_ERR_INVALID_PORT : !object || object>p->next_object || !p->objects ? A3D_ERR_INVALID_OBJECT : 0;
    if (!r) --p->objects;
    pthread_mutex_unlock(&a3d_lock);
    return r;
}
static ABI int32_t a3d_object_set_attributes(uint32_t id, uint32_t object, uint64_t count, const void *attributes) {
    if (!a3d_port(id)) return A3D_ERR_INVALID_PORT;
    if (count && !attributes) return A3D_ERR_INVALID_PARAMETER;
    (void)object; return 0;
}
static ABI int32_t a3d_port_advance(uint32_t id) {
    pthread_mutex_lock(&a3d_lock);
    A3dPort *p=a3d_port(id);
    int32_t r=0;
    if (!p) r=A3D_ERR_INVALID_PORT;
    else if (p->params.buffer_mode==BUFFER_NO_ADVANCE) r=A3D_ERR_NOT_SUPPORTED;
    else {
        a3d_drain(p);
        if (p->queued>=p->params.queue_depth+1) r=A3D_ERR_NOT_READY;
        else ++p->queued;
    }
    pthread_mutex_unlock(&a3d_lock);
    return r;
}
/* Synchronous push: returns once the queue has room again, as the PS4 output would. */
static ABI int32_t a3d_port_push(uint32_t id, uint32_t blocking) {
    atomic_fetch_add(&a3d_pushes,1);
    for (;;) {
        pthread_mutex_lock(&a3d_lock);
        A3dPort *p=a3d_port(id);
        if (!p) { pthread_mutex_unlock(&a3d_lock); return A3D_ERR_INVALID_PORT; }
        if (p->params.buffer_mode!=BUFFER_ADVANCE_AND_PUSH) { pthread_mutex_unlock(&a3d_lock); return A3D_ERR_NOT_SUPPORTED; }
        a3d_drain(p);
        uint64_t wait=0;
        if (blocking && p->queued>=p->params.queue_depth) wait=p->played_ns+frame_ns(p)-now_ns();
        pthread_mutex_unlock(&a3d_lock);
        if (!wait || wait>1000000000) return 0;
        struct timespec ts={(time_t)(wait/1000000000),(long)(wait%1000000000)};
        nanosleep(&ts,NULL);
    }
}
static ABI int32_t a3d_queue_level(uint32_t id, uint32_t *level, uint32_t *available) {
    if (!level && !available) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    A3dPort *p=a3d_port(id);
    if (p) {
        a3d_drain(p);
        if (level) *level=p->queued;
        if (available) *available=p->queued<p->params.queue_depth ? p->params.queue_depth-p->queued : 0;
    }
    pthread_mutex_unlock(&a3d_lock);
    return p ? 0 : A3D_ERR_INVALID_PORT;
}
static const RuntimeExport a3d_exports[]={
    {"sceAudio3dInitialize",a3d_initialize}, {"sceAudio3dTerminate",a3d_terminate},
    {"sceAudio3dPortOpen",a3d_port_open}, {"sceAudio3dPortClose",a3d_port_close},
    {"sceAudio3dPortSetAttribute",a3d_port_set_attribute},
    {"sceAudio3dObjectReserve",a3d_object_reserve}, {"sceAudio3dObjectUnreserve",a3d_object_unreserve},
    {"sceAudio3dObjectSetAttributes",a3d_object_set_attributes},
    {"sceAudio3dPortAdvance",a3d_port_advance}, {"sceAudio3dPortPush",a3d_port_push},
    {"sceAudio3dPortGetQueueLevel",a3d_queue_level},
};
uintptr_t runtime_audio3d_resolve(const char *name) { return RUNTIME_LOOKUP(a3d_exports,name); }
void runtime_sotc_audio_report(void) {
    printf("Runtime: Ngs2 renders=%zu, Audio3d pushes=%zu\n",atomic_load(&ngs2_renders),atomic_load(&a3d_pushes));
}
