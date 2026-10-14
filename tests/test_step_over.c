/* The read step-over of the GPU page tracker (page_manager.cpp, probe.c) on Windows: a read
 * of a read-protected page is let through for one instruction (trap flag) and the page is
 * protected again on the single-step trap. Checks that every read sees the data, the page
 * ends protected after each one, and loops, wide loads and a read crossing into a second
 * protected page all work. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>

static unsigned char *pages;            /* two pages */
static __thread uintptr_t stepped[2];
static volatile long faults, steps;

static LONG CALLBACK handler(EXCEPTION_POINTERS *info) {
    DWORD code=info->ExceptionRecord->ExceptionCode;
    if (code==EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->ExceptionInformation[0]==0) {
        uintptr_t page=info->ExceptionRecord->ExceptionInformation[1]&~(uintptr_t)4095;
        if (page!=(uintptr_t)pages && page!=(uintptr_t)pages+4096) return EXCEPTION_CONTINUE_SEARCH;
        int slot=stepped[0] ? 1 : 0;
        if (stepped[slot]) return EXCEPTION_CONTINUE_SEARCH;
        DWORD old; VirtualProtect((void *)page,4096,PAGE_READONLY,&old);
        stepped[slot]=page;
        info->ContextRecord->EFlags|=0x100;
        InterlockedIncrement(&faults);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code==EXCEPTION_SINGLE_STEP && stepped[0]) {
        for (int i=0;i<2;++i) if (stepped[i]) {
            DWORD old; VirtualProtect((void *)stepped[i],4096,PAGE_NOACCESS,&old);
            stepped[i]=0;
        }
        info->ContextRecord->EFlags&=~0x100u;
        InterlockedIncrement(&steps);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)

static int protected_now(void *p) {
    MEMORY_BASIC_INFORMATION mbi; VirtualQuery(p,&mbi,sizeof(mbi));
    return mbi.Protect==PAGE_NOACCESS;
}

int main(void) {
    pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    for (int i=0;i<8192;++i) pages[i]=(unsigned char)(i*7);
    DWORD old; VirtualProtect(pages,8192,PAGE_NOACCESS,&old);
    AddVectoredExceptionHandler(1,handler);

    /* Byte reads in a loop: one fault and one step per read. */
    unsigned sum=0;
    for (int i=0;i<4096;i+=97) sum+=((volatile unsigned char *)pages)[i];
    unsigned want=0; for (int i=0;i<4096;i+=97) want+=(unsigned char)(i*7);
    CHECK(sum==want,"sum %u, wanted %u",sum,want);
    CHECK(protected_now(pages),"page protected again");
    CHECK(faults==steps,"%ld faults, %ld steps",faults,steps);

    /* A 32-byte load (as a descriptor read). */
    __m256i v=_mm256_loadu_si256((const __m256i *)(pages+64));
    unsigned char bytes[32]; _mm256_storeu_si256((__m256i *)bytes,v);
    for (int i=0;i<32;++i) CHECK(bytes[i]==(unsigned char)((64+i)*7),"wide load byte %d",i);

    /* A load crossing into the second page: two faults, one step. */
    long f=faults, s=steps;
    uint64_t x; memcpy(&x,(const void *)(pages+4092),8);
    uint64_t y=0; for (int i=7;i>=0;--i) y=y<<8|(unsigned char)((4092+i)*7);
    CHECK(x==y,"crossing load %llx, wanted %llx",(unsigned long long)x,(unsigned long long)y);
    CHECK(protected_now(pages) && protected_now(pages+4096),"both pages protected again");
    printf("crossing load: %ld faults, %ld steps\n",faults-f,steps-s);

    /* memcpy of a whole page (rep movs or vector loops). */
    static unsigned char copy[4096];
    memcpy(copy,pages+4096,4096);
    for (int i=0;i<4096;++i) if (copy[i]!=(unsigned char)((4096+i)*7)) { CHECK(0,"copy byte %d",i); break; }
    CHECK(protected_now(pages+4096),"page protected after memcpy");
    printf("total: %ld faults, %ld steps\n%s (%d failures)\n",faults,steps,failures ? "FAILED" : "ok",failures);
    return failures!=0;
}
