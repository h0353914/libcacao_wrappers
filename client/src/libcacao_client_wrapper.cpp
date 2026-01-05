#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <binder/IMemory.h>
#include <binder/Parcel.h>
#include <log/log.h>
#include <utils/StrongPointer.h>

#include "alloc_fix.h"

namespace android::Cacao
{
    class CacaoClient
    {
    public:
        static android::sp<android::IMemory> allocMemory(unsigned long size);
    };
}

__attribute__((visibility("default")))
android::sp<android::IMemory>
android::Cacao::CacaoClient::allocMemory(unsigned long size)
{
    return android::allocMemory_common(size, "CacaoClient::allocMemory");
}

namespace cacao
{
    namespace ProcessCtrlCaps
    {
        struct CameraIndex;
    }
    struct Caps;
}

namespace android
{
    struct ICacaoService; // 不需要完整定義
}

// ---------- vtable helper ----------
static inline void **Vtbl(void *obj)
{
    return *reinterpret_cast<void ***>(obj);
}

// cacao::Caps vtable slots（依你反編譯：+0x20/+0x28/+0x30）
using CapsSizeFn = uint64_t (*)(cacao::Caps *self);     // slot 4
using CapsPrepFn = int (*)(cacao::Caps *self, void *);  // slot 5
using CapsFinalFn = int (*)(cacao::Caps *self, void *); // slot 6

// service vtable slot（依你反編譯：*mService +0x30）
using SvcGetCapsFn = int (*)(android::ICacaoService *svc,
                             const cacao::ProcessCtrlCaps::CameraIndex &idx,
                             android::sp<android::IMemory> *mem,
                             void *blob198);

namespace android::Cacao
{

    // 這三個在 real.so 是 GLOBAL（你 readelf 已確認）
    extern android::sp<android::ICacaoService> mService;
    extern int mServicePid;
    int getService();

    __attribute__((visibility("default"))) int getCaps(const cacao::ProcessCtrlCaps::CameraIndex &idx,
                                                       cacao::Caps *caps)
    {
        // 保持與原邏輯一致：確保 service 初始化
        getService();

        if (mService == nullptr)
            return 0;
        if (mServicePid != (int)getpid())
            return 0;
        if (!caps)
            return -0x67;

        // 取 raw size（caps vtable +0x20）
        uint64_t raw = reinterpret_cast<CapsSizeFn>(Vtbl(caps)[4])(caps);

        // 用你的 allocator 修正 raw（避免 &0xffffffff / sign-extend）
        android::sp<android::IMemory> mem =
            android::allocMemory_common<uint64_t>(raw, "Cacao::getCaps");
        if (mem == nullptr)
        {
            ALOGE("WRAP: getCaps alloc failed raw=%" PRIu64, raw);
            return -0x6f;
        }

        // 兩塊 buffer（你反編譯顯示 memcpy 0x198）
        alignas(8) uint8_t buf_1f0[0x198];
        alignas(8) uint8_t buf_388[0x198];
        memset(buf_1f0, 0, sizeof(buf_1f0));

        // caps vtable +0x28：prepare
        int rprep = reinterpret_cast<CapsPrepFn>(Vtbl(caps)[5])(caps, buf_1f0);
        if (rprep < 0)
            return rprep;

        // 呼叫 service vtable +0x30
        memcpy(buf_388, buf_1f0, sizeof(buf_388));
        android::ICacaoService *svc = mService.get();
        int rsvc = reinterpret_cast<SvcGetCapsFn>(Vtbl(svc)[6])(svc, idx, &mem, buf_388);
        if (rsvc == -0x6e)
            return -0x6e;
        if (rsvc != 0)
            return -0x6f;

        // caps vtable +0x30：finalize/commit
        return reinterpret_cast<CapsFinalFn>(Vtbl(caps)[6])(caps, buf_1f0);
    }

} // namespace android::Cacao

extern "C" long
_ZNK7android6Parcel10readIntPtrEv(const android::Parcel *thiz)
{
#ifdef __LP64__
    int64_t v = 0;
    (void)thiz->readInt64(&v);
    return (long)v;
#else
    int32_t v = 0;
    (void)thiz->readInt32(&v);
    return (long)v;
#endif
}