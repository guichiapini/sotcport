/* libSceFiber: cooperative user-mode contexts (CUSA08809's job system runs on them).
 *
 * A fiber is a guest-allocated OrbisFiber (layout kept for sceFiberGetInfo) with an optional
 * guest stack ("context"). sceFiberRun enters a fiber from a thread; fibers pass control with
 * sceFiberSwitch and give it back to the thread with sceFiberReturnToThread. A fiber resumed
 * later may run on another host thread: per-thread state is therefore always read through
 * fiber_tls() (never cached across a switch), and the guest TCB follows the host thread as on
 * the PS4. A fiber without context runs below the current stack pointer and cannot be
 * suspended: leaving it abandons its frames and the next run starts the entry again.
 *
 * fiber_swap saves the System V callee-saved registers, MXCSR and the x87 control word on the
 * current stack, stores the stack pointer and resumes the other one. The fiber that was left
 * becomes Idle only once its stack pointer is stored (finish_switch on the resuming side). */
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_NULL ((int32_t)0x80590001)
#define ERR_ALIGNMENT ((int32_t)0x80590002)
#define ERR_RANGE ((int32_t)0x80590003)
#define ERR_INVALID ((int32_t)0x80590004)
#define ERR_PERMISSION ((int32_t)0x80590005)
#define ERR_STATE ((int32_t)0x80590006)
#define SIGNATURE0 UINT32_C(0xdef1649c)
#define SIGNATURE1 UINT32_C(0xb37592a0)
#define OPT_SIGNATURE UINT32_C(0xbb40e64d)
#define STACK_SIGNATURE UINT64_C(0x7149f2ca7149f2ca)
#define MIN_CONTEXT 512
#define FLAG_SET_FPU 0x100
#define NO_CONTEXT_GAP 4096 /* left to the caller below its stack pointer */
enum { STATE_RUN=1, STATE_IDLE=2, STATE_TERMINATED=3 };

typedef void (ABI *FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);
typedef struct Fiber {
    uint32_t magic_start;
    _Atomic uint32_t state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[32];
    void *saved_sp;            /* OrbisFiber.context: suspended stack pointer, NULL = not started */
    uint32_t flags;
    void *context_start, *context_end;
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber)<=256,"OrbisFiber size");
typedef struct {
    uint64_t size; FiberEntry entry; uint64_t arg_on_initialize; void *addr_context; uint64_t size_context;
    char name[32]; uint64_t size_context_margin; uint8_t pad[48];
} FiberInfo;
_Static_assert(sizeof(FiberInfo)==128,"OrbisFiberInfo size");

typedef struct {
    Fiber *current;      /* fiber running on this thread, NULL outside sceFiberRun */
    Fiber *previous;     /* fiber left by the last switch, made Idle by the side that resumes */
    Fiber *starting;     /* fiber whose entry fiber_main calls */
    void *thread_sp;     /* thread stack inside sceFiberRun */
    uint64_t transfer;   /* argument handed to the side that resumes */
} FiberTls;
static _Thread_local FiberTls tls_state;
static __attribute__((noinline)) FiberTls *fiber_tls(void) {
    FiberTls *t=&tls_state;
    __asm__ volatile("" : "+r"(t)); /* the address is this thread's, computed here */
    return t;
}
static atomic_size_t runs, switches, started;

void ABI fiber_swap(void **save_sp, void *new_sp);
void ABI fiber_thunk(void);
__asm__(
    ".text\n"
    ".globl fiber_swap\n"
    "fiber_swap:\n"
    "    pushq %rbp\n    pushq %rbx\n    pushq %r12\n    pushq %r13\n    pushq %r14\n    pushq %r15\n"
    "    subq $16, %rsp\n"
    "    stmxcsr 8(%rsp)\n"
    "    fnstcw (%rsp)\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    fldcw (%rsp)\n"
    "    ldmxcsr 8(%rsp)\n"
    "    addq $16, %rsp\n"
    "    popq %r15\n    popq %r14\n    popq %r13\n    popq %r12\n    popq %rbx\n    popq %rbp\n"
    "    ret\n"
    ".globl fiber_thunk\n"
    "fiber_thunk:\n"
    "    call fiber_main\n"
    "    ud2\n");

static void finish_switch(void) {
    FiberTls *t=fiber_tls();
    if (t->previous) { atomic_store(&t->previous->state,STATE_IDLE); t->previous=NULL; }
}
ABI __attribute__((used, noreturn)) void fiber_main(void) {
    FiberTls *t=fiber_tls();
    Fiber *f=t->starting;
    uint64_t argument=t->transfer;
    t->starting=NULL;
    finish_switch();
    atomic_fetch_add(&started,1);
    f->entry(f->arg_on_initialize,argument);
    fprintf(stderr,"STOP: fiber '%s' returned from its entry function\n",f->name);
    runtime_report();
    exit(22);
}
/* First stack of a fiber: fiber_swap's frame returning into fiber_thunk, 16-byte aligned there. */
static void *initial_stack(Fiber *f, uintptr_t top) {
    top&=~(uintptr_t)15;
    uint64_t *slot=(uint64_t *)(top-88);
    uint32_t mxcsr=0x9fc0; uint16_t fpucw=0x037f;
    if (!(f->flags&FLAG_SET_FPU)) { __asm__ volatile("stmxcsr %0" : "=m"(mxcsr)); __asm__ volatile("fnstcw %0" : "=m"(fpucw)); }
    memset(slot,0,88);
    memcpy(slot,&fpucw,2); memcpy((char *)slot+8,&mxcsr,4);
    slot[8]=(uint64_t)(uintptr_t)fiber_thunk;  /* top-24 */
    return slot;
}
/* Stack pointer to enter f with; prepares its first frame when it has not started. */
static void *enter_sp(Fiber *f, uintptr_t below) {
    FiberTls *t=fiber_tls();
    if (f->saved_sp && f->addr_context) return f->saved_sp;
    t->starting=f;
    uintptr_t top=f->addr_context ? (uintptr_t)f->addr_context+f->size_context : below-NO_CONTEXT_GAP;
    return initial_stack(f,top);
}
static int valid(const Fiber *f) { return f->magic_start==SIGNATURE0 && f->magic_end==SIGNATURE1; }
static int32_t attach(Fiber *f, void *addr, uint64_t size) {
    if ((size && size<MIN_CONTEXT) ) return ERR_RANGE;
    if ((size&15) || !addr || !size || f->addr_context) return ERR_INVALID;
    f->addr_context=addr; f->size_context=size; f->context_start=addr; f->context_end=(char *)addr+size;
    *(uint64_t *)addr=STACK_SIGNATURE;
    return 0;
}

static ABI int32_t fiber_initialize(Fiber *f, const char *name, FiberEntry entry, uint64_t arg,
                                    void *addr, uint64_t size, const uint32_t *opt, uint32_t flags, uint32_t build) {
    if (!f || !name || !entry) return ERR_NULL;
    if (((uintptr_t)f&7) || ((uintptr_t)addr&15) || (opt && ((uintptr_t)opt&7))) return ERR_ALIGNMENT;
    if (size && size<MIN_CONTEXT) return ERR_RANGE;
    if ((size&15) || (!addr)!=(!size) || (opt && *opt!=OPT_SIGNATURE)) return ERR_INVALID;
    memset(f,0,sizeof(*f));
    snprintf(f->name,sizeof(f->name),"%s",name);
    f->entry=entry; f->arg_on_initialize=arg;
    f->flags=flags|(build>=0x3500000 ? FLAG_SET_FPU : 0);
    if (addr) attach(f,addr,size);
    f->magic_start=SIGNATURE0; f->magic_end=SIGNATURE1;
    atomic_store(&f->state,STATE_IDLE);
    return 0;
}
static ABI int32_t fiber_opt_init(uint32_t *opt) {
    if (!opt) return ERR_NULL;
    if ((uintptr_t)opt&7) return ERR_ALIGNMENT;
    *opt=OPT_SIGNATURE; return 0;
}
static ABI int32_t fiber_finalize(Fiber *f) {
    if (!f) return ERR_NULL;
    if ((uintptr_t)f&7) return ERR_ALIGNMENT;
    if (!valid(f)) return ERR_INVALID;
    uint32_t idle=STATE_IDLE;
    return atomic_compare_exchange_strong(&f->state,&idle,STATE_TERMINATED) ? 0 : ERR_STATE;
}
static ABI int32_t fiber_run(Fiber *f, uint64_t arg_on_run_to, uint64_t *arg_on_return) {
    if (!f) return ERR_NULL;
    if ((uintptr_t)f&7) return ERR_ALIGNMENT;
    if (!valid(f)) return ERR_INVALID;
    FiberTls *t=fiber_tls();
    if (t->current) return ERR_PERMISSION;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&f->state,&idle,STATE_RUN)) return ERR_STATE;
    atomic_fetch_add(&runs,1);
    t->current=f; t->previous=NULL; t->transfer=arg_on_run_to;
    void *sp=enter_sp(f,(uintptr_t)__builtin_frame_address(0));
    fiber_swap(&t->thread_sp,sp);
    /* Back from sceFiberReturnToThread, possibly of another fiber than f. */
    t=fiber_tls();
    finish_switch();
    t->current=NULL;
    if (arg_on_return) *arg_on_return=t->transfer;
    return 0;
}
static ABI int32_t fiber_switch(Fiber *f, uint64_t arg_on_run_to, uint64_t *arg_on_run) {
    if (!f) return ERR_NULL;
    if ((uintptr_t)f&7) return ERR_ALIGNMENT;
    if (!valid(f)) return ERR_INVALID;
    FiberTls *t=fiber_tls();
    Fiber *self=t->current;
    if (!self) return ERR_PERMISSION;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&f->state,&idle,STATE_RUN)) return ERR_STATE;
    atomic_fetch_add(&switches,1);
    t->previous=self; t->current=f; t->transfer=arg_on_run_to;
    void *sp=enter_sp(f,(uintptr_t)__builtin_frame_address(0));
    void *discard;
    fiber_swap(self->addr_context ? &self->saved_sp : &discard,sp);
    /* Resumed by sceFiberRun or sceFiberSwitch on whichever thread. */
    finish_switch();
    if (arg_on_run) *arg_on_run=fiber_tls()->transfer;
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return, uint64_t *arg_on_run) {
    FiberTls *t=fiber_tls();
    Fiber *self=t->current;
    if (!self) return ERR_PERMISSION;
    t->previous=self; t->current=NULL; t->transfer=arg_on_return;
    void *discard;
    fiber_swap(self->addr_context ? &self->saved_sp : &discard,t->thread_sp);
    finish_switch();
    if (arg_on_run) *arg_on_run=fiber_tls()->transfer;
    return 0;
}
static ABI int32_t fiber_get_self(Fiber **out) {
    if (!out) return ERR_NULL;
    Fiber *self=fiber_tls()->current;
    if (!self) return ERR_PERMISSION;
    *out=self; return 0;
}
static ABI int32_t fiber_get_info(Fiber *f, FiberInfo *info) {
    if (!f || !info) return ERR_NULL;
    if (((uintptr_t)f&7) || ((uintptr_t)info&7)) return ERR_ALIGNMENT;
    if (info->size!=sizeof(FiberInfo) || !valid(f)) return ERR_INVALID;
    info->entry=f->entry; info->arg_on_initialize=f->arg_on_initialize;
    info->addr_context=f->addr_context; info->size_context=f->size_context;
    memcpy(info->name,f->name,sizeof(info->name));
    info->size_context_margin=UINT64_MAX;
    return 0;
}
static ABI int32_t fiber_rename(Fiber *f, const char *name) {
    if (!f || !name) return ERR_NULL;
    if ((uintptr_t)f&7) return ERR_ALIGNMENT;
    if (!valid(f)) return ERR_INVALID;
    snprintf(f->name,sizeof(f->name),"%s",name);
    return 0;
}

static const RuntimeExport exports[]={
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberOptParamInitialize",fiber_opt_init},
    {"sceFiberFinalize",fiber_finalize}, {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch},
    {"sceFiberReturnToThread",fiber_return_to_thread}, {"sceFiberGetSelf",fiber_get_self},
    {"sceFiberGetInfo",fiber_get_info}, {"sceFiberRename",fiber_rename},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_fiber_report(void) {
    printf("Runtime: fibers started=%zu, runs=%zu, switches=%zu\n",
           atomic_load(&started),atomic_load(&runs),atomic_load(&switches));
}
