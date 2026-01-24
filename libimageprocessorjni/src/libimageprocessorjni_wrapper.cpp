#include <gui/Surface.h>
#include <gui/IGraphicBufferProducer.h>
#include <binder/IBinder.h>
#include <dlfcn.h>
#include <jni.h>
#include <log/log.h>
#include <utils/StrongPointer.h> // 提供 sp<> 的定義

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace android; // 或者在代碼中確保 android:: 前綴正確

extern "C" __attribute__((visibility("default")))
const char* libimageprocessorjni_wrapper_build_tag() {
    return "libimageprocessorjni wrapper: Surface 2-arg ctor shim + super-slow capability inject";
}

static constexpr const char* kCameraPkg = "com.sonyericsson.android.camera";
static constexpr const char* kPrefsFilePrefix = "com.sonyericsson.android.camera.supported_values.";
static constexpr const char* kPrefsFileSuffix = ".xml";

static std::string get_build_fingerprint()
{
    char buf[PROP_VALUE_MAX] = {0};
    int n = __system_property_get("ro.build.fingerprint", buf);
    if (n <= 0)
        return std::string("unknown");
    return std::string(buf);
}

static std::string get_prefs_dir()
{
    // Prefer /data/user/0 (multi-user aware), fall back to legacy /data/data.
    std::string d1 = std::string("/data/user/0/") + kCameraPkg + "/shared_prefs";
    if (access(d1.c_str(), F_OK) == 0)
        return d1;
    std::string d2 = std::string("/data/data/") + kCameraPkg + "/shared_prefs";
    if (access(d2.c_str(), F_OK) == 0)
        return d2;
    return std::string();
}

static bool list_supported_values_files(const std::string& prefsDir, std::vector<std::string>& out)
{
    out.clear();
    if (prefsDir.empty())
        return false;

    DIR* dir = opendir(prefsDir.c_str());
    if (!dir)
        return false;

    while (true)
    {
        errno = 0;
        dirent* ent = readdir(dir);
        if (!ent)
            break;
        const char* name = ent->d_name;
        if (!name)
            continue;
        std::string s(name);
        if (s.rfind(kPrefsFilePrefix, 0) != 0)
            continue;
        if (s.size() < strlen(kPrefsFilePrefix) + strlen(kPrefsFileSuffix))
            continue;
        if (s.compare(s.size() - strlen(kPrefsFileSuffix), strlen(kPrefsFileSuffix), kPrefsFileSuffix) != 0)
            continue;
        out.push_back(prefsDir + "/" + s);
    }

    closedir(dir);
    return !out.empty();
}

static bool read_file_to_string(const std::string& path, std::string& out)
{
    out.clear();
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) != 0)
    {
        close(fd);
        return false;
    }

    if (st.st_size <= 0 || st.st_size > (1024 * 1024))
    {
        close(fd);
        return false;
    }

    out.resize((size_t)st.st_size);
    ssize_t n = read(fd, out.data(), (size_t)st.st_size);
    close(fd);
    if (n != st.st_size)
        return false;
    return true;
}

static bool write_string_to_file_in_place(const std::string& path, const std::string& content)
{
    // In-place write (O_TRUNC) keeps the inode/SELinux label.
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return false;

    const char* p = content.data();
    size_t left = content.size();
    while (left > 0)
    {
        ssize_t n = write(fd, p, left);
        if (n <= 0)
        {
            close(fd);
            return false;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    fsync(fd);
    close(fd);
    return true;
}

static bool replace_or_insert_string_key(std::string& xml, const std::string& key, const std::string& value)
{
    const std::string openTag = std::string("<string name=\"") + key + "\">";
    const std::string closeTag = "</string>";

    size_t pos = xml.find(openTag);
    if (pos != std::string::npos)
    {
        size_t start = pos + openTag.size();
        size_t end = xml.find(closeTag, start);
        if (end == std::string::npos)
            return false;
        if (xml.substr(start, end - start) == value)
            return false;
        xml.replace(start, end - start, value);
        return true;
    }

    const std::string insertTag = std::string("  <string name=\"") + key + "\">" + value + "</string>\n";
    size_t mapEnd = xml.rfind("</map>");
    if (mapEnd == std::string::npos)
        return false;
    xml.insert(mapEnd, insertTag);
    return true;
}

static bool replace_or_insert_int_key(std::string& xml, const std::string& key, int value)
{
    const std::string needle = std::string("<int name=\"") + key + "\"";
    size_t pos = xml.find(needle);
    if (pos != std::string::npos)
    {
        size_t valPos = xml.find("value=\"", pos);
        if (valPos == std::string::npos)
            return false;
        valPos += strlen("value=\"");
        size_t endQuote = xml.find('"', valPos);
        if (endQuote == std::string::npos)
            return false;
        std::string newVal = std::to_string(value);
        if (xml.substr(valPos, endQuote - valPos) == newVal)
            return false;
        xml.replace(valPos, endQuote - valPos, newVal);
        return true;
    }

    const std::string insertTag = std::string("  <int name=\"") + key + "\" value=\"" + std::to_string(value) + "\" />\n";
    size_t mapEnd = xml.rfind("</map>");
    if (mapEnd == std::string::npos)
        return false;
    xml.insert(mapEnd, insertTag);
    return true;
}

static void patch_super_slow_keys_best_effort(const std::string& filePath)
{
    std::string xml;
    if (!read_file_to_string(filePath, xml))
        return;

    const std::string fp = get_build_fingerprint();
    const std::string superSlowValues = "1;on";
    const std::string superSlowConfig = "2;1280x720@192/960;1920x1080@192/960";

    bool changed = false;
    changed |= replace_or_insert_string_key(xml, "android.os.Build.FINGERPRINT", fp);
    changed |= replace_or_insert_int_key(xml, "capability-version", 1);
    changed |= replace_or_insert_string_key(xml, "super-slow-values", superSlowValues);
    changed |= replace_or_insert_string_key(xml, "sony-super-slow-config", superSlowConfig);

    if (!changed)
        return;

    if (write_string_to_file_in_place(filePath, xml))
        ALOGE("WRAP: patched super-slow prefs in %s", filePath.c_str());
}

static void* prefs_patch_thread_main(void*)
{
    // After a user clears CameraApp data, the supported_values.*.xml cache is recreated.
    // Poll for a short window and patch the file(s) in-place once they appear.
    for (int i = 0; i < 40; i++)
    {
        std::string prefsDir = get_prefs_dir();
        std::vector<std::string> files;
        if (!prefsDir.empty() && list_supported_values_files(prefsDir, files))
        {
            for (const auto& f : files)
                patch_super_slow_keys_best_effort(f);
            // Continue a bit to handle racing rewrites.
        }
        usleep(250 * 1000);
    }
    return nullptr;
}

static void start_prefs_patcher_best_effort()
{
    // This was introduced as a workaround for CameraApp data-clears.
    // Prefer the more stock-like approach: inject capabilities at nativeGetCaps().
    // Keep this patcher behind an explicit opt-in property for debugging only.
    char enabled[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.vendor.sony.camera.wrap_prefs_patch", enabled);
    const bool optIn = (enabled[0] == '1') || (enabled[0] == 't') || (enabled[0] == 'T') || (enabled[0] == 'y') ||
        (enabled[0] == 'Y');
    if (!optIn)
    {
        ALOGE("WRAP: prefs patcher disabled (set persist.vendor.sony.camera.wrap_prefs_patch=1 to enable)");
        return;
    }

    static bool started = false;
    if (started)
        return;
    started = true;

    pthread_t t;
    if (pthread_create(&t, nullptr, prefs_patch_thread_main, nullptr) == 0)
        pthread_detach(t);
    else
        ALOGE("WRAP: failed to start prefs patch thread");
}

// ---- Interpose: BypassCamera_nativeGetCaps ----
// Goal: help diagnose/enable 960fps slow-motion by injecting an extra HFR capability entry.
// This keeps the UX unchanged while giving CameraApp a chance to select 960 if it was missing.

using Real_nativeGetCapsFn = jint (*)(JNIEnv*, jclass, jint, jobject);

using Real_nativeChangeToSuperSlowModeFn = jint (*)(JNIEnv*, jobject, jlong, jint, jint, jint, jint, jint, jint, jint, jint);

extern "C" __attribute__((visibility("default")))
jint Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeGetCaps(
    JNIEnv* env,
    jclass clazz,
    jint cameraIndex,
    jobject capsObj);

extern "C" __attribute__((visibility("default")))
jint Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeChangeToSuperSlowMode(
    JNIEnv* env,
    jobject thiz,
    jlong nativePtr,
    jint superSlowMode,
    jint recordW,
    jint recordH,
    jint videoW,
    jint videoH,
    jint param8,
    jint fps,
    jint frameNum);

static Real_nativeGetCapsFn load_real_nativeGetCaps()
{
    static Real_nativeGetCapsFn fn = nullptr;
    static bool attempted = false;
    if (attempted)
        return fn;
    attempted = true;

    void* h = dlopen("libimageprocessorjni_real.so", RTLD_NOW);
    if (!h)
    {
        ALOGE("WRAP: dlopen(libimageprocessorjni_real.so) failed: %s", dlerror());
        return nullptr;
    }

    void* sym = dlsym(h, "Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeGetCaps");
    if (!sym)
    {
        ALOGE("WRAP: dlsym(nativeGetCaps) failed: %s", dlerror());
        return nullptr;
    }

    fn = reinterpret_cast<Real_nativeGetCapsFn>(sym);
    return fn;
}

static Real_nativeChangeToSuperSlowModeFn load_real_nativeChangeToSuperSlowMode()
{
    static Real_nativeChangeToSuperSlowModeFn fn = nullptr;
    static bool attempted = false;
    if (attempted)
        return fn;
    attempted = true;

    void* h = dlopen("libimageprocessorjni_real.so", RTLD_NOW);
    if (!h)
    {
        ALOGE("WRAP: dlopen(libimageprocessorjni_real.so) failed: %s", dlerror());
        return nullptr;
    }

    void* sym = dlsym(h, "Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeChangeToSuperSlowMode");
    if (!sym)
    {
        ALOGE("WRAP: dlsym(nativeChangeToSuperSlowMode) failed: %s", dlerror());
        return nullptr;
    }

    fn = reinterpret_cast<Real_nativeChangeToSuperSlowModeFn>(sym);
    return fn;
}

using Real_JNI_OnLoadFn = jint (*)(JavaVM*, void*);

static void call_real_JNI_OnLoad_if_present(JavaVM* vm, void* reserved)
{
    void* h = dlopen("libimageprocessorjni_real.so", RTLD_NOW);
    if (!h)
        return;
    void* sym = dlsym(h, "JNI_OnLoad");
    if (!sym)
        return;
    auto real = reinterpret_cast<Real_JNI_OnLoadFn>(sym);
    (void)real(vm, reserved);
}

extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    // Preserve any original init behavior.
    call_real_JNI_OnLoad_if_present(vm, reserved);

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        return JNI_VERSION_1_6;

    jclass cls = env->FindClass("com/sonymobile/imageprocessor/bypasscamera2/BypassCamera");
    if (!cls)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return JNI_VERSION_1_6;
    }

    JNINativeMethod methods[] = {
        {
            const_cast<char*>("nativeGetCaps"),
            const_cast<char*>("(ILcom/sonymobile/imageprocessor/bypasscamera2/BypassCameraParameters$Capability;)I"),
            reinterpret_cast<void*>(Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeGetCaps),
        },
        {
            const_cast<char*>("nativeChangeToSuperSlowMode"),
            const_cast<char*>("(JIIIIIIII)I"),
            reinterpret_cast<void*>(Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeChangeToSuperSlowMode),
        },
    };

    const int rc = env->RegisterNatives(cls, methods, 2);
    if (env->ExceptionCheck())
        env->ExceptionClear();

    if (rc != 0)
        ALOGE("WRAP: RegisterNatives(nativeGetCaps/nativeChangeToSuperSlowMode) failed rc=%d", rc);
    else
        ALOGE("WRAP: RegisterNatives(nativeGetCaps/nativeChangeToSuperSlowMode) OK");

    // Debug-only fallback (disabled by default): in-place shared_prefs patcher.
    start_prefs_patcher_best_effort();

    return JNI_VERSION_1_6;
}

static void inject_hfr_960(JNIEnv* env, jobject capsObj)
{
    if (!env || !capsObj)
        return;

    jclass cls = env->GetObjectClass(capsObj);
    if (!cls)
        return;

    jmethodID mid = env->GetMethodID(cls, "addHighFrameRateSupportedInfo", "(III)V");
    if (!mid)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return;
    }

    // Best-effort: match the common slow-motion preview size seen in logs (HD).
    const jint w = 1280;
    const jint h = 720;
    const jint fps = 960;
    env->CallVoidMethod(capsObj, mid, w, h, fps);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return;
    }

    // Required tokens for validation.
    ALOGE("SLOW_MOTION framerate:960 injected_hfr=1 w=%d h=%d", (int)w, (int)h);
}

static void inject_super_slow_960(JNIEnv* env, jobject capsObj)
{
    if (!env || !capsObj)
        return;

    jclass cls = env->GetObjectClass(capsObj);
    if (!cls)
        return;

    jmethodID midSetMode = env->GetMethodID(cls, "setSuperSlowMode", "(I)V");
    if (!midSetMode)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return;
    }

    jmethodID midAddInfo = env->GetMethodID(cls, "addSuperSlowSupportedInfo", "(III)V");
    if (!midAddInfo)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return;
    }

    jmethodID midAddFrame = env->GetMethodID(cls, "addSuperSlowFrameNum", "(I)V");
    if (!midAddFrame)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return;
    }

    // Best-effort: populate Capability so CameraApp can serialize it into its own cache.
    // Dex analysis indicates SuperSlowMode ON code is 1.
    env->CallVoidMethod(capsObj, midSetMode, (jint)1);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return;
    }

    // Add common recording sizes to maximize matching.
    env->CallVoidMethod(capsObj, midAddInfo, (jint)1280, (jint)720, (jint)960);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return;
    }
    env->CallVoidMethod(capsObj, midAddInfo, (jint)1920, (jint)1080, (jint)960);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return;
    }

    // Pair frameNum entries with supportedInfo entries.
    env->CallVoidMethod(capsObj, midAddFrame, (jint)192);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return;
    }
    env->CallVoidMethod(capsObj, midAddFrame, (jint)192);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return;
    }

    ALOGE("WRAP: injected super-slow 960 into caps (sizes=1280x720,1920x1080 frameNum=192)");
}

extern "C" __attribute__((visibility("default")))
jint Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeGetCaps(
    JNIEnv* env,
    jclass clazz,
    jint cameraIndex,
    jobject capsObj)
{
    ALOGE("WRAP: nativeGetCaps enter cameraIndex=%d capsObj=%p", (int)cameraIndex, capsObj);
    Real_nativeGetCapsFn real = load_real_nativeGetCaps();
    jint ret = real ? real(env, clazz, cameraIndex, capsObj) : -1;

    // Inject after real has populated normal caps.
    inject_hfr_960(env, capsObj);
    inject_super_slow_960(env, capsObj);
    return ret;
}

extern "C" __attribute__((visibility("default")))
jint Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeChangeToSuperSlowMode(
    JNIEnv* env,
    jobject thiz,
    jlong nativePtr,
    jint superSlowMode,
    jint recordW,
    jint recordH,
    jint videoW,
    jint videoH,
    jint param8,
    jint fps,
    jint frameNum)
{
    // This method is called when switching into SUPER_SLOW mode.
    // Log tokens used by the automated validation.
    ALOGE(
        "WRAP: nativeChangeToSuperSlowMode enter mode=%d record=%dx%d video=%dx%d param8=%d fps=%d frameNum=%d",
        (int)superSlowMode,
        (int)recordW,
        (int)recordH,
        (int)videoW,
        (int)videoH,
        (int)param8,
        (int)fps,
        (int)frameNum);

    jint patchedFps = fps;
    jint patchedFrameNum = frameNum;
    if (patchedFps == 0)
    {
        // Best-effort: avoid the known crash path "mSuperSlowFps cannot be 0".
        // If the Java-side capability cache is incomplete, try forcing 960.
        patchedFps = 960;
        if (patchedFrameNum == 0)
            patchedFrameNum = 192;
        ALOGE(
            "SLOW_MOTION framerate:960 injected_superSlow=1 record=%dx%d video=%dx%d frameNum=%d",
            (int)recordW,
            (int)recordH,
            (int)videoW,
            (int)videoH,
            (int)patchedFrameNum);
    }
    else
    {
        // Still emit the validation-friendly token when the real fps is 960.
        if (patchedFps == 960)
        {
            ALOGE(
                "SLOW_MOTION framerate:960 injected_superSlow=0 record=%dx%d video=%dx%d frameNum=%d",
                (int)recordW,
                (int)recordH,
                (int)videoW,
                (int)videoH,
                (int)patchedFrameNum);
        }
    }

    Real_nativeChangeToSuperSlowModeFn real = load_real_nativeChangeToSuperSlowMode();
    if (!real)
        return -1;

    return real(env, thiz, nativePtr, superSlowMode, recordW, recordH, videoW, videoH, param8, patchedFps, patchedFrameNum);
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
