#include <binder/IMemory.h>
#include <utils/StrongPointer.h>

#include "alloc_fix.h"

namespace android
{
    class CacaoService
    {
    public:
        class Client
        {
        public:
            static android::sp<android::IMemory> allocMemory(unsigned int size);
        };
    };
}

__attribute__((visibility("default")))
android::sp<android::IMemory>
android::CacaoService::Client::allocMemory(unsigned int size)
{
    return android::allocMemory_common(size, "CacaoService::Client::allocMemory");
}
