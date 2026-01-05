#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <new>

#include <binder/IMemory.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <log/log.h>
#include <utils/StrongPointer.h>

namespace android {

static inline unsigned long fix_size_ul(unsigned long raw) {
    if ((raw & 0xffffffff00000000ULL) == 0xffffffff00000000ULL) {
        return (unsigned long)(uint32_t)raw;
    }
    return raw;
}

template <typename U>
static inline sp<IMemory> allocMemory_common(U raw_size, const char* who) {
    const unsigned long kCapsMin = 0x198;
    const unsigned long kMax = 64UL * 1024UL * 1024UL;

    unsigned long req = fix_size_ul((unsigned long)raw_size);
    unsigned long size = req;

    // ✅ 只有「真的需要 0x198 blob」的路徑才保底到 0x198
    // 你目前 wrapper 內只有 getCaps() 會用到 0x198 的 buf/mem 對齊語意
    if (who && strcmp(who, "Cacao::getCaps") == 0) {
        if (size < kCapsMin) size = kCapsMin;
    }

    ALOGE("WRAP: %s allocMemory_common raw=%lu(0x%lx) fixed=%lu(0x%lx) req=%lu alloc=%lu",
          (who ? who : "(null)"),
          (unsigned long)raw_size, (unsigned long)raw_size,
          req, req,
          req, size);

    sp<IMemory> mem;
    if (size == 0) return mem;

    if (size > kMax || size >= 0xE0000000UL) {
        ALOGE("WRAP: %s reject size=%lu raw=%lu", (who ? who : "(null)"), size, (unsigned long)raw_size);
        return mem;
    }

    sp<IMemoryHeap> heap(new (std::nothrow) MemoryHeapBase((size_t)size, 0, nullptr));
    if (heap == nullptr) return sp<IMemory>();

    mem = sp<IMemory>(new (std::nothrow) MemoryBase(heap, 0, (size_t)size));
    if (mem == nullptr) return sp<IMemory>();

    void* p = mem->unsecurePointer();
    if (p) memset(p, 0, (size_t)size);

    ALOGE("WRAP: %s allocMemory_common OK sz=%lu ptr=%p", (who ? who : "(null)"), size, p);
    return mem;
}


} // namespace android
