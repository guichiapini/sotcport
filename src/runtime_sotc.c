/* Functions Shadow of the Colossus (CUSA08809, SDK 4.50) imports beyond what CUSA03173 needed:
 * kernel event flags and queries, libc heap tracing, POSIX sockets, and system services that
 * have no PC counterpart (gestures, screenshots, sharing, live streaming, error dialogs).
 * Services without a PC counterpart report success and do nothing; each logs its first call. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

#define KERNEL_ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define EPERM_ 1
#define ESRCH_ 3
#define EBUSY_ 16
#define EINVAL_ 22
#define ENOMEM_ 12
#define ETIMEDOUT_ 60
#define ECANCELED_ 85
#define ENETDOWN_ 50

#define ONCE(what) do { static atomic_int said; if (!atomic_exchange(&said,1)) \
    printf("Runtime: %s (no PC counterpart; reports success)\n",what); } while (0)

/* ---- libSceLibcInternalExt: heap tracing tables libc.prx registers its mspaces in ---- */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *atomic_id_mask, *mstate_table; } HeapTraceInfo;
static uint64_t heap_atomic_id_mask, heap_mstate_table[64];
static ABI void libc_heap_trace_info(HeapTraceInfo *info) {
    if (!info) return;
    info->atomic_id_mask=&heap_atomic_id_mask; info->mstate_table=heap_mstate_table; info->get_segment_info=0;
}

/* ---- kernel queries ---- */
static ABI int32_t kernel_is_neo(void) { return 0; } /* base PS4 profile: 1080p rendering paths */
static ABI int32_t kernel_asan(void) { return 0; }
static ABI int32_t kernel_segment_info(void) { return KERNEL_ERR(ESRCH_); }
typedef struct { uint32_t time_low; uint16_t time_mid, time_hi; uint8_t clock_hi, clock_low, node[6]; } KernelUuid;
static ABI int32_t kernel_uuid(KernelUuid *uuid) {
    if (!uuid) return KERNEL_ERR(EINVAL_);
#ifdef _WIN32
    if (BCryptGenRandom(NULL,(PUCHAR)uuid,sizeof(*uuid),BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return KERNEL_ERR(14);
#else
    for (size_t i=0;i<sizeof(*uuid);++i) ((uint8_t *)uuid)[i]=(uint8_t)rand();
#endif
    uuid->time_hi=(uint16_t)((uuid->time_hi&0x0fff)|0x4000); uuid->clock_hi=(uint8_t)((uuid->clock_hi&0x3f)|0x80);
    return 0;
}
/* Local time: the host time zone, as gettimeofday reports it. */
static int64_t utc_offset(time_t at) {
#ifdef _WIN32
    return compat_utc_offset(at);
#else
    struct tm tm; localtime_r(&at,&tm); return tm.tm_gmtoff;
#endif
}
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } KernelTimesec;
typedef struct { int32_t minuteswest, dsttime; } KernelTimezone;
static ABI int32_t kernel_utc_to_local(int64_t utc, int64_t *local, KernelTimesec *sec, uint64_t *dst) {
    int64_t offset=utc_offset((time_t)utc);
    if (local) *local=utc+offset;
    if (sec) { sec->t=utc; sec->west_sec=(uint32_t)offset; sec->dst_sec=0; }
    if (dst) *dst=0;
    return 0;
}
static ABI int32_t kernel_local_to_utc(int64_t local, int64_t unused, int64_t *utc, KernelTimezone *zone, int32_t *dst) {
    (void)unused;
    int64_t offset=utc_offset((time_t)local);
    if (utc) *utc=local-offset;
    if (zone) { zone->minuteswest=(int32_t)(-offset/60); zone->dsttime=0; }
    if (dst) *dst=0;
    return 0;
}

/* ---- event flags (sceKernel*EventFlag): 64-bit pattern, AND/OR waits, optional clear ---- */
#define EF_WAIT_AND 1
#define EF_WAIT_OR 2
#define EF_CLEAR_ALL 0x10
#define EF_CLEAR_PAT 0x20
#define EF_ATTR_MULTI 0x20
typedef struct EventFlag {
    uint32_t magic;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t pattern;
    int multi, waiters, cancelled, deleted;
    uint64_t generation;
    char name[32];
} EventFlag;
#define EF_MAGIC UINT32_C(0x45464c47)
static int ef_valid(EventFlag *f) { return f && f->magic==EF_MAGIC && !f->deleted; }
static int ef_met(uint64_t pattern, uint64_t bits, uint32_t mode) {
    return (mode&EF_WAIT_AND) ? (pattern&bits)==bits : (pattern&bits)!=0;
}
static void ef_consume(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (result) *result=f->pattern;
    if (mode&EF_CLEAR_ALL) f->pattern=0;
    else if (mode&EF_CLEAR_PAT) f->pattern&=~bits;
}
static int ef_mode_ok(uint64_t bits, uint32_t mode) {
    uint32_t wait=mode&3, clear=mode&0x30;
    return bits && (wait==EF_WAIT_AND || wait==EF_WAIT_OR) && clear!=0x30 && !(mode&~0x33u);
}
static ABI int32_t ef_create(EventFlag **out, const char *name, uint32_t attr, uint64_t init, const void *opt) {
    (void)opt;
    if (!out || (attr&~0x33u)) return KERNEL_ERR(EINVAL_);
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return KERNEL_ERR(ENOMEM_);
    f->magic=EF_MAGIC; f->pattern=init; f->multi=(attr&EF_ATTR_MULTI)!=0;
    pthread_mutex_init(&f->lock,NULL); pthread_cond_init(&f->changed,NULL);
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    *out=f; return 0;
}
static ABI int32_t ef_delete(EventFlag *f) {
    if (!ef_valid(f)) return KERNEL_ERR(ESRCH_);
    pthread_mutex_lock(&f->lock);
    f->deleted=1; pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    /* Waiters may still be leaving; the object is kept (a few bytes per deleted flag). */
    return 0;
}
static ABI int32_t ef_set(EventFlag *f, uint64_t bits) {
    if (!ef_valid(f)) return KERNEL_ERR(ESRCH_);
    pthread_mutex_lock(&f->lock);
    f->pattern|=bits; pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t ef_clear(EventFlag *f, uint64_t bits) {
    if (!ef_valid(f)) return KERNEL_ERR(ESRCH_);
    pthread_mutex_lock(&f->lock); f->pattern&=bits; pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t ef_poll(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (!ef_valid(f)) return KERNEL_ERR(ESRCH_);
    if (!ef_mode_ok(bits,mode)) return KERNEL_ERR(EINVAL_);
    pthread_mutex_lock(&f->lock);
    int32_t r=0;
    if (!f->multi && f->waiters) r=KERNEL_ERR(EPERM_);
    else if (ef_met(f->pattern,bits,mode)) ef_consume(f,bits,mode,result);
    else { if (result) *result=f->pattern; r=KERNEL_ERR(EBUSY_); }
    pthread_mutex_unlock(&f->lock);
    return r;
}
static ABI int32_t ef_wait(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout_us) {
    if (!ef_valid(f)) return KERNEL_ERR(ESRCH_);
    if (!ef_mode_ok(bits,mode)) return KERNEL_ERR(EINVAL_);
    struct timespec deadline, start;
    clock_gettime(CLOCK_REALTIME,&start);
    if (timeout_us) {
        uint64_t ns=(uint64_t)start.tv_nsec+(uint64_t)*timeout_us*1000;
        deadline.tv_sec=start.tv_sec+(time_t)(ns/1000000000); deadline.tv_nsec=(long)(ns%1000000000);
    }
    pthread_mutex_lock(&f->lock);
    if (!f->multi && f->waiters) { pthread_mutex_unlock(&f->lock); return KERNEL_ERR(EPERM_); }
    ++f->waiters;
    uint64_t generation=f->generation;
    int32_t r=0;
    while (!ef_met(f->pattern,bits,mode)) {
        if (f->deleted) { r=KERNEL_ERR(EINVAL_); break; }       /* deleted while waiting */
        if (f->generation!=generation) { r=KERNEL_ERR(ECANCELED_); break; }
        int e=timeout_us ? pthread_cond_timedwait(&f->changed,&f->lock,&deadline) : pthread_cond_wait(&f->changed,&f->lock);
        if (e==ETIMEDOUT && !ef_met(f->pattern,bits,mode)) { r=KERNEL_ERR(ETIMEDOUT_); break; }
    }
    if (!r) ef_consume(f,bits,mode,result);
    else if (result) *result=f->pattern;
    --f->waiters;
    pthread_mutex_unlock(&f->lock);
    if (timeout_us) {
        struct timespec now; clock_gettime(CLOCK_REALTIME,&now);
        int64_t spent=(int64_t)(now.tv_sec-start.tv_sec)*1000000+(now.tv_nsec-start.tv_nsec)/1000;
        *timeout_us=spent>=(int64_t)*timeout_us ? 0 : *timeout_us-(uint32_t)spent;
    }
    return r;
}
static ABI int32_t ef_cancel(EventFlag *f, uint64_t pattern, int32_t *waiters) {
    if (!ef_valid(f)) return KERNEL_ERR(ESRCH_);
    pthread_mutex_lock(&f->lock);
    if (waiters) *waiters=f->waiters;
    f->pattern=pattern; ++f->generation; pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}

/* ---- POSIX sockets: offline, the network is down ---- */
static ABI int32_t socket_down(void) { *runtime_errno()=ENETDOWN_; return -1; }

/* ---- services with no PC counterpart ---- */
static ABI int32_t ok_zero(void) { return 0; }
static ABI int32_t gesture_open(void) { ONCE("SystemGesture"); return 1; }
static ABI int32_t gesture_events_count(void) { return 0; }
static ABI int32_t gesture_event_by_index(void) { return (int32_t)0x80B90002; } /* no event */
static ABI int32_t error_dialog_open(const void *param) { (void)param; ONCE("ErrorDialog"); return 0; }
static ABI int32_t error_dialog_status(void) { return 3; } /* finished */
static ABI int32_t screenshot(void) { ONCE("ScreenShot enable/disable"); return 0; }
static ABI int32_t share(void) { ONCE("ShareUtility"); return 0; }
static ABI int32_t live_streaming(void) { ONCE("GameLiveStreaming"); return 0; }
static ABI int32_t video_recording(void) { ONCE("VideoRecording"); return 0; }
static ABI int32_t ime_keyboard_open(void) { ONCE("ImeKeyboard"); return (int32_t)0x80BC0001; } /* not available */
/* PngEnc (screenshots for share/save icons): no encoder; callers fall back or skip the image. */
static ABI int32_t png_query_memory(void) { return 1024; }
static ABI int32_t png_create(const void *param, void *memory, uint64_t size, void **handle) {
    (void)param; (void)size; ONCE("PngEnc");
    if (handle) *handle=memory; return 0;
}
static ABI int32_t png_encode(void) { return (int32_t)0x80690004; } /* encode failed */

/* libc.prx support for its unwinder and exit path (SDK 4.50). No module program headers or
 * signal frames are published, so lookups by address find nothing. */
static ABI int32_t phdr_match_addr(const void *info, uint64_t address) { (void)info; (void)address; return 0; }
static ABI int32_t module_info_from_addr(uint64_t address, int32_t flags, void *info) {
    (void)address; (void)flags; (void)info; return KERNEL_ERR(ESRCH_);
}
static ABI int32_t is_signal_return(const void *pc) { (void)pc; return 0; }
static ABI int32_t sig_return(const void *context) { (void)context; return 0; }
static ABI void pthread_cxa_finalize(void *dso) { (void)dso; }
static const char *progname="eboot.bin";
uintptr_t runtime_sotc_resolve_data(const char *name) {
    if (!strcmp(name,"djxxOmW6-aw#p#J")) return (uintptr_t)&progname;  /* __progname */
    return 0;
}

/* C++ exception unwinding asks for a module's eh_frame_hdr; not provided yet, so a thrown
 * exception finds no handler. Logged, since that ends the game. */
static ABI int32_t kernel_unwind_info(uint64_t address, int32_t flags, void *info) {
    (void)flags; (void)info;
    printf("Runtime: unwind info requested for 0x%llx (not provided)\n",(unsigned long long)address);
    return KERNEL_ERR(ESRCH_);
}
static const RuntimeExport exports[]={
    {"sceLibcHeapGetTraceInfo",libc_heap_trace_info},
    {"sceKernelGetModuleInfoForUnwind",kernel_unwind_info},
    {"__elf_phdr_match_addr",phdr_match_addr}, {"sceKernelGetModuleInfoFromAddr",module_info_from_addr},
    {"_is_signal_return",is_signal_return}, {"sigreturn",sig_return},
    {"__pthread_cxa_finalize",pthread_cxa_finalize},
    {"scePadSetLightBar",ok_zero}, {"sceAudioOutSetMixLevelPadSpk",ok_zero},
    {"sceSysmoduleLoadModuleInternalWithArg",ok_zero},
    {"sceKernelIsNeoMode",kernel_is_neo}, {"sceKernelIsAddressSanitizerEnabled",kernel_asan},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",kernel_segment_info},
    {"sceKernelUuidCreate",kernel_uuid},
    {"sceKernelConvertUtcToLocaltime",kernel_utc_to_local}, {"sceKernelConvertLocaltimeToUtc",kernel_local_to_utc},
    {"sceKernelCreateEventFlag",ef_create}, {"sceKernelDeleteEventFlag",ef_delete},
    {"sceKernelSetEventFlag",ef_set}, {"sceKernelClearEventFlag",ef_clear},
    {"sceKernelPollEventFlag",ef_poll}, {"sceKernelWaitEventFlag",ef_wait},
    {"sceKernelCancelEventFlag",ef_cancel},
    {"socket",socket_down}, {"bind",socket_down}, {"connect",socket_down}, {"listen",socket_down},
    {"accept",socket_down}, {"send",socket_down}, {"recv",socket_down}, {"select",socket_down},
    {"setsockopt",socket_down}, {"shutdown",socket_down},
    {"sceSystemGestureOpen",gesture_open}, {"sceSystemGestureClose",ok_zero},
    {"sceSystemGestureCreateTouchRecognizer",ok_zero},
    {"sceSystemGestureInitializePrimitiveTouchRecognizer",ok_zero},
    {"sceSystemGestureFinalizePrimitiveTouchRecognizer",ok_zero},
    {"sceSystemGestureUpdatePrimitiveTouchRecognizer",ok_zero},
    {"sceSystemGestureUpdateTouchRecognizer",ok_zero},
    {"sceSystemGestureGetTouchEventsCount",gesture_events_count},
    {"sceSystemGestureGetTouchEventByIndex",gesture_event_by_index},
    {"sceErrorDialogInitialize",ok_zero}, {"sceErrorDialogTerminate",ok_zero},
    {"sceErrorDialogOpen",error_dialog_open}, {"sceErrorDialogClose",ok_zero},
    {"sceErrorDialogUpdateStatus",error_dialog_status},
    {"sceScreenShotEnable",screenshot}, {"sceScreenShotDisable",screenshot},
    {"sceShareUtilityInitializeEx2",share}, {"sceShareUtilityOpenShareMenuDefault",share},
    {"sceGameLiveStreamingEnableLiveStreaming",live_streaming},
    {"sceVideoRecordingSetInfo",video_recording},
    {"sceImeKeyboardOpen",ime_keyboard_open}, {"sceImeKeyboardClose",ok_zero}, {"sceImeUpdate",ok_zero},
    {"scePngEncQueryMemorySize",png_query_memory}, {"scePngEncCreate",png_create},
    {"scePngEncDelete",ok_zero}, {"scePngEncEncode",png_encode},
};
/* DiscMap query of SDK 4.50 Fios2 (no public name): the whole game is on the HDD. */
static ABI int32_t discmap_7c98(const char *path, int64_t offset, int64_t size, int32_t *flags, int32_t *a, int32_t *b) {
    (void)path; (void)offset; (void)size;
    if (flags) *flags=0;
    if (a) *a=0;
    if (b) *b=0;
    return 0;
}
/* Imports without a known symbol name, matched by their linked spelling. */
static const struct { const char *name; void *function; } unnamed[]={
    {"fJgP+wqifno#libSceDiscMap",discmap_7c98},
};
/* BB_HEARTBEAT=<seconds>: periodic progress lines (frames shown, input, files, fibers), to tell
 * a game that waits from one that hangs in automated runs. */
uint64_t bbgpu_flip_count(void);
static void *heartbeat(void *argument) {
    unsigned period=(unsigned)(uintptr_t)argument, elapsed=0;
    for (uint64_t last=0;;) {
        struct timespec ts={(time_t)period,0};
        nanosleep(&ts,NULL);
        elapsed+=period;
        uint64_t flips=bbgpu_flip_count();
        /* stderr: unbuffered, so the lines survive a run that is killed. */
        fprintf(stderr,"Heartbeat %us: frames=%llu (+%llu)\n",elapsed,(unsigned long long)flips,(unsigned long long)(flips-last));
        last=flips;
    }
    return NULL;
}
void runtime_sotc_start(void) {
    static atomic_int started;
    const char *v=getenv("BB_HEARTBEAT");
    if (!v || atoi(v)<=0 || atomic_exchange(&started,1)) return;
    pthread_t t;
    if (!pthread_create(&t,NULL,heartbeat,(void *)(uintptr_t)atoi(v))) {
        pthread_detach(t);
        fprintf(stderr,"Runtime: heartbeat every %d s\n",atoi(v));
    }
}
uintptr_t runtime_sotc_resolve(const char *name) {
    for (size_t i=0;i<sizeof(unnamed)/sizeof(*unnamed);++i)
        if (!strcmp(name,unnamed[i].name)) return (uintptr_t)unnamed[i].function;
    uintptr_t f=RUNTIME_LOOKUP(exports,name);
    if (!f) f=runtime_fiber_resolve(name);
    if (!f) f=runtime_ngs2_resolve(name);
    if (!f) f=runtime_audio3d_resolve(name);
    return f;
}
