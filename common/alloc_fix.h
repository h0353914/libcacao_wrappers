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
    const unsigned long kMin = 0x198;
    const unsigned long kMax = 64UL * 1024UL * 1024UL;

    unsigned long size = fix_size_ul((unsigned long)raw_size);
    sp<IMemory> mem;
    ALOGE("WRAP-HIT: allocMemory");
    if (size == 0) return mem;
    if (size < kMin) size = kMin;
    if (size > kMax || size >= 0xE0000000UL) {
        ALOGE("%s allocMemory reject size=%lu raw=%lu", who, size, (unsigned long)raw_size);
        return mem;
    }

    sp<IMemoryHeap> heap(new (std::nothrow) MemoryHeapBase((size_t)size, 0, nullptr));
    if (heap == nullptr) return sp<IMemory>();

    mem = sp<IMemory>(new (std::nothrow) MemoryBase(heap, 0, (size_t)size));
    if (mem == nullptr) return sp<IMemory>();

    void* p = mem->unsecurePointer();
    if (p) memset(p, 0, (size_t)size);
    return mem;
}

} // namespace android
