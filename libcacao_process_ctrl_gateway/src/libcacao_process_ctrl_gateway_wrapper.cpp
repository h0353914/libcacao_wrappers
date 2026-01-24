#include <gui/Surface.h>
#include <gui/IGraphicBufferProducer.h>
#include <binder/IBinder.h>
#include <utils/StrongPointer.h>

using namespace android;

extern "C" __attribute__((visibility("default")))
const char* libcacao_process_ctrl_gateway_wrapper_build_tag() {
    return "libcacao_process_ctrl_gateway wrapper: Surface 2-arg ctor shim";
}

// 3-arg ctor（系統 libgui.so 內有的符號）
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

// 2-arg ctor（blob 缺的符號）：轉呼叫 3-arg ctor，handle 傳 null
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
