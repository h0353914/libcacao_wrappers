#include <gui/Surface.h>
#include <gui/IGraphicBufferProducer.h>
#include <binder/IBinder.h>
#include <utils/StrongPointer.h> // 提供 sp<> 的定義

using namespace android; // 或者在代碼中確保 android:: 前綴正確

extern "C" __attribute__((visibility("default")))
const char* libimageprocessorjni_wrapper_build_tag() {
    return "libimageprocessorjni wrapper: Surface 2-arg ctor shim";
}

// 3-arg ctor（系統 libgui.so 內有的符號）
// 你 nm 已確認：
// _ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEbRKNS1_INS_7IBinderEEE
// _ZN7android7SurfaceC2ERKNS_2spINS_22IGraphicBufferProducerEEEbRKNS1_INS_7IBinderEEE

extern "C" void Surface_ctor3_C1(android::Surface* thiz,
                                const sp<android::IGraphicBufferProducer>& bp,
                                bool controlledByApp,
                                const sp<android::IBinder>& handle)
    __asm__("_ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEbRKNS1_INS_7IBinderEEE");

extern "C" void Surface_ctor3_C2(android::Surface* thiz,
                                const sp<android::IGraphicBufferProducer>& bp,
                                bool controlledByApp,
                                const sp<android::IBinder>& handle)
    __asm__("_ZN7android7SurfaceC2ERKNS_2spINS_22IGraphicBufferProducerEEEbRKNS1_INS_7IBinderEEE");

// 2-arg ctor（blob 缺的符號）
// 你 readelf 已確認缺：
// _ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEb
//
// 另外我建議也一起補 C2（有些情況會吃到）

extern "C" void
_ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEb(android::Surface* thiz,
                                                              const sp<android::IGraphicBufferProducer>& bp,
                                                              bool controlledByApp) {
    sp<android::IBinder> nullHandle;
    Surface_ctor3_C1(thiz, bp, controlledByApp, nullHandle);
}

extern "C" void
_ZN7android7SurfaceC2ERKNS_2spINS_22IGraphicBufferProducerEEEb(android::Surface* thiz,
                                                              const sp<android::IGraphicBufferProducer>& bp,
                                                              bool controlledByApp) {
    sp<android::IBinder> nullHandle;
    Surface_ctor3_C2(thiz, bp, controlledByApp, nullHandle);
}
