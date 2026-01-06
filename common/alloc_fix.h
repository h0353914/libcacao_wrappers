#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <new>

#include <binder/IMemory.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <log/log.h>
#include <utils/StrongPointer.h>

namespace android {

// 判斷是不是 0xffffffffXXXXXXXX 這種 sign-extend 污染
static inline unsigned long fix_size_ul(unsigned long raw, int* hi_ff_out) {
    int hi_ff = 0;
    if ((raw & 0xffffffff00000000ULL) == 0xffffffff00000000ULL) {
        hi_ff = 1;
        raw = (unsigned long)(uint32_t)raw;
    }
    if (hi_ff_out) *hi_ff_out = hi_ff;
    return raw;
}

// ra: 允許 caller 傳 __builtin_return_address(0)，沒傳就顯示 0
template <typename U>
static inline sp<IMemory> allocMemory_common(U raw_size, const char* who, void* ra = nullptr) {
    const unsigned long kCapsMin = 0x198;                 // 408
    const unsigned long kMax     = 64UL * 1024UL * 1024UL;

    const int pid = (int)getpid();
#if defined(__BIONIC__)
    const int tid = (int)gettid();
#else
    const int tid = pid;
#endif

    int hi_ff = 0;
    unsigned long raw_ul = (unsigned long)raw_size;
    unsigned long req = fix_size_ul(raw_ul, &hi_ff);
    unsigned long size = req;

    const int caps_path = (who && strcmp(who, "Cacao::getCaps") == 0) ? 1 : 0;
    int clamp_min = 0;

    if (caps_path && size < kCapsMin) {
        size = kCapsMin;
        clamp_min = 1;
    }

    int reject_max = 0;
    int reject_e000 = 0;
    if (size > kMax) reject_max = 1;
    if (size >= 0xE0000000UL) reject_e000 = 1;

    ALOGE("WRAP: %s allocMemory_common pid=%d tid=%d ra=%p raw=%lu(0x%lx) hi_ff=%d "
          "fixed=%lu(0x%lx) caps_path=%d clamp_min=%d kCapsMin=%lu kMax=%lu "
          "reject_max=%d reject_e000=%d req=%lu alloc=%lu",
          (who ? who : "(null)"),
          pid, tid, ra,
          raw_ul, raw_ul, hi_ff,
          req, req,
          caps_path, clamp_min, kCapsMin, kMax,
          reject_max, reject_e000,
          req, size);

    sp<IMemory> mem;
    if (size == 0) return mem;

    if (reject_max || reject_e000) {
        ALOGE("WRAP: %s allocMemory_common REJECT pid=%d tid=%d ra=%p size=%lu raw=%lu",
              (who ? who : "(null)"), pid, tid, ra, size, raw_ul);
        return mem;
    }

    sp<IMemoryHeap> heap(new (std::nothrow) MemoryHeapBase((size_t)size, 0, nullptr));
    if (heap == nullptr) return sp<IMemory>();

    mem = sp<IMemory>(new (std::nothrow) MemoryBase(heap, 0, (size_t)size));
    if (mem == nullptr) return sp<IMemory>();

    void* p = mem->unsecurePointer();
    if (p) memset(p, 0, (size_t)size);

    ALOGE("WRAP: %s allocMemory_common OK pid=%d tid=%d ra=%p sz=%lu ptr=%p",
          (who ? who : "(null)"), pid, tid, ra, size, p);

    return mem;
}

} // namespace android
