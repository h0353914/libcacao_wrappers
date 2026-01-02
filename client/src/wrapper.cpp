// ...前面 include 不變...
#include <stdint.h>
#include <new>
#include <dlfcn.h>
#include <string.h>    // memset
#include <stddef.h>    // size_t
#include <sys/types.h> // ssize_t

#include <log/log.h>

#include <utils/StrongPointer.h>
#include <binder/IMemory.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>

// ✅ 不要 include <binder/IMemoryHeap.h>，你的 tree 找不到
namespace android { class IMemoryHeap; class Parcel; }

/* =========================
 * watchdog: start 卡住就 SIGQUIT dump stack
 * ========================= */
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdatomic.h>

static atomic_int g_start_inflight = 0; // 0/1
static atomic_int g_start_pid = 0;
static atomic_int g_start_tid = 0;

struct WatchdogArgs {
    int seconds;
};

static void* start_watchdog_thread(void* arg) {
    WatchdogArgs* a = (WatchdogArgs*)arg;
    int sec = a ? a->seconds : 3;
    delete a;

    sleep((unsigned)sec);

    if (atomic_load(&g_start_inflight)) {
        int pid = atomic_load(&g_start_pid);
        int tid = atomic_load(&g_start_tid);
        ALOGE("libcacao_wrap: start still inflight after %ds -> SIGQUIT pid=%d tid=%d", sec, pid, tid);
        if (pid > 0) kill((pid_t)pid, SIGQUIT);
        // if (pid > 0 && tid > 0) syscall(SYS_tgkill, pid, tid, SIGQUIT);
    }
    return nullptr;
}

static inline void start_watchdog_arm(int seconds) {
    atomic_store(&g_start_inflight, 1);
    atomic_store(&g_start_pid, (int)getpid());
    atomic_store(&g_start_tid, (int)syscall(SYS_gettid));

    pthread_t th;
    WatchdogArgs* a = new WatchdogArgs();
    a->seconds = seconds;
    if (pthread_create(&th, nullptr, start_watchdog_thread, a) == 0) {
        pthread_detach(th);
    } else {
        delete a;
    }
}

static inline void start_watchdog_disarm() {
    atomic_store(&g_start_inflight, 0);
}

extern "C" __attribute__((visibility("default")))
const char* libcacao_client_wrapper_build_tag() {
    return "libcacao_client wrapper: log start/getCaps + allocMemory(static ABI) + hook binder ioctl + SIGQUIT watchdog";
}

/* =========================
 * getCaps: 轉呼叫 real + log
 * ========================= */
#define ENABLE_WRAP_GETCAPS 1

#if ENABLE_WRAP_GETCAPS
#define MANGLED_GETCAPS \
"_ZN7android5Cacao7getCapsERKN5cacao15ProcessCtrlCaps11CameraIndexEPNS1_4CapsE"

extern "C" __attribute__((visibility("default")))
int cacao_getCaps_entry(const void* cameraIndex, void* caps) __asm__(MANGLED_GETCAPS);

extern "C" __attribute__((visibility("default")))
int cacao_getCaps_entry(const void* cameraIndex, void* caps) {
    using Fn = int (*)(const void*, void*);
    static Fn real = nullptr;
    if (!real) {
        real = (Fn)dlsym(RTLD_NEXT, MANGLED_GETCAPS);
        ALOGE("libcacao_wrap: dlsym getCaps real=%p", (void*)real);
    }
    if (!real) return -38;

    int r = real(cameraIndex, caps);
    ALOGE("libcacao_wrap: getCaps -> %d (0x%x)", r, (unsigned)r);
    return r;
}
#endif

/* =========================
 * start: 轉呼叫 real + 印回傳值（含 thunk）+ watchdog
 * ========================= */
#define ENABLE_WRAP_START 1

#if ENABLE_WRAP_START
#define MANGLED_START \
"_ZN7android5Cacao11CacaoClient5startEPKN5cacao15ProcessModeBaseE"
#define MANGLED_START_THUNK \
"_ZThn40_N7android5Cacao11CacaoClient5startEPKN5cacao15ProcessModeBaseE"

static inline const char* errno_name(int r) {
    switch (-r) {
        case 38:  return "ENOSYS";
        case 101: return "ENETUNREACH";
        case 102: return "ENETRESET";
        case 103: return "ECONNABORTED";
        case 104: return "ECONNRESET";
        case 110: return "ETIMEDOUT";
        case 111: return "ECONNREFUSED";
        default:  return "";
    }
}

static int start_call_real(void* thiz, const void* mode, const char* tag) {
    using Fn = int (*)(void*, const void*);
    static Fn real = nullptr;

    if (!real) {
        real = (Fn)dlsym(RTLD_NEXT, MANGLED_START);
        ALOGE("libcacao_wrap: dlsym start real=%p", (void*)real);
    }

    void* remote = nullptr;
    if (thiz) remote = *(void**)((char*)thiz + 0x30);

    ALOGE("libcacao_wrap: %s enter thiz=%p mode=%p remote=%p", tag, thiz, mode, remote);

    if (!real) {
        ALOGE("libcacao_wrap: %s start real missing -> -38(ENOSYS)", tag);
        return -38;
    }

    start_watchdog_arm(3);
    int r = real(thiz, mode);
    start_watchdog_disarm();

    if (r < 0) {
        ALOGE("libcacao_wrap: %s leave -> %d (0x%x) %s", tag, r, (unsigned)r, errno_name(r));
    } else {
        ALOGE("libcacao_wrap: %s leave -> %d (0x%x)", tag, r, (unsigned)r);
    }
    return r;
}

extern "C" __attribute__((visibility("default")))
int cacao_start_entry(void* thiz, const void* mode) __asm__(MANGLED_START);

extern "C" __attribute__((visibility("default")))
int cacao_start_entry(void* thiz, const void* mode) {
    return start_call_real(thiz, mode, "start");
}

extern "C" __attribute__((visibility("default")))
int cacao_start_thunk_entry(void* thiz, const void* mode) __asm__(MANGLED_START_THUNK);

extern "C" __attribute__((visibility("default")))
int cacao_start_thunk_entry(void* thiz, const void* mode) {
    return start_call_real(thiz, mode, "start(thunk)");
}
#endif

/* =========================
 * allocMemory: ✅ 依你現在觀察到的 ABI（x0=size, x8=sret）=> 這必須是 static
 * mangled: _ZN7android5Cacao11CacaoClient11allocMemoryEm
 * ========================= */
namespace android {
namespace Cacao {

class CacaoClient {
public:
    static android::sp<android::IMemory> allocMemory(unsigned long size);
};

} // namespace Cacao
} // namespace android

static inline unsigned long fix_size_ul(unsigned long raw) {
    if ((raw & 0xffffffff00000000ULL) == 0xffffffff00000000ULL) {
        return (unsigned long)(uint32_t)raw;
    }
    return raw;
}

__attribute__((visibility("default")))
android::sp<android::IMemory> android::Cacao::CacaoClient::allocMemory(unsigned long raw_size) {
    using android::sp;
    using android::IMemory;
    using android::IMemoryHeap;
    using android::MemoryBase;
    using android::MemoryHeapBase;

    const unsigned long kMin = 0x198;                  // 408
    const unsigned long kMax = 64UL * 1024UL * 1024UL; // 64MB

    unsigned long size = fix_size_ul(raw_size);
    void* lr = __builtin_return_address(0);

    // 注意：這裡不再印 this（因為沒有 this）
    ALOGE("libcacao_wrap: allocMemory ENTER raw=%lu(0x%lx)->size=%lu(0x%lx) LR=%p",
          raw_size, raw_size, size, size, lr);

    sp<IMemory> mem;

    if (size == 0) {
        ALOGE("libcacao_wrap: allocMemory size==0 -> nullptr");
        ALOGE("libcacao_wrap: allocMemory LEAVE");
        return mem;
    }

    if (size < kMin) {
        ALOGE("libcacao_wrap: allocMemory bump %lu(0x%lx) -> %lu(0x%lx)",
              size, size, kMin, kMin);
        size = kMin;
    }

    if (size > kMax) {
        ALOGE("libcacao_wrap: allocMemory guard too big size=%lu -> nullptr", size);
        ALOGE("libcacao_wrap: allocMemory LEAVE");
        return mem;
    }

    sp<IMemoryHeap> heap = sp<IMemoryHeap>(new MemoryHeapBase((size_t)size, 0, nullptr));
    mem = sp<IMemory>(new MemoryBase(heap, 0, (size_t)size));

    void*  p = nullptr;
    size_t n = 0;
    ssize_t off = 0;

    if (mem != nullptr) {
        sp<IMemoryHeap> hh = mem->getMemory(&off, &n);
        (void)hh;
        p = mem->unsecurePointer();
    }

    ALOGE("libcacao_wrap: allocMemory OK size=%lu heap=%p mem=%p ptr=%p off=%zd n=%zu",
          size, heap.get(), mem.get(), p, off, n);

    if (p && n) {
        memset(p, 0, n);
        ALOGE("libcacao_wrap: allocMemory memset(0) done n=%zu", n);
    } else {
        ALOGE("libcacao_wrap: allocMemory memset skipped (ptr=%p n=%zu)", p, n);
    }

    ALOGE("libcacao_wrap: allocMemory LEAVE");
    return mem;
}

/* =========================
 * binder/hwbinder open + ioctl(BINDER_WRITE_READ) hook
 * ========================= */
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#if __has_include(<linux/android/binder.h>)
#include <linux/android/binder.h>
#elif __has_include(<linux/binder.h>)
#include <linux/binder.h>
#endif

#ifndef BINDER_WRITE_READ
struct binder_write_read {
  uint64_t write_size;
  uint64_t write_consumed;
  uint64_t write_buffer;
  uint64_t read_size;
  uint64_t read_consumed;
  uint64_t read_buffer;
};
#define BINDER_WRITE_READ _IOWR('b', 1, struct binder_write_read)
#endif

static int g_hwbinder_fd = -1;
static int g_binder_fd   = -1;

static inline void mark_fd(int fd, const char* path) {
    if (fd < 0 || !path) return;
    if (!strcmp(path, "/dev/hwbinder")) g_hwbinder_fd = fd;
    if (!strcmp(path, "/dev/binder"))   g_binder_fd = fd;
}

static inline const char* which_binder(int fd) {
    if (fd == g_hwbinder_fd) return "hwbinder";
    if (fd == g_binder_fd)   return "binder";
    return nullptr;
}

static inline void dump_bwr(int fd, binder_write_read* bwr) {
    if (!bwr) return;
    const char* w = which_binder(fd);
    if (!w) return;

    ALOGE("libcacao_wrap: %s ioctl(BINDER_WRITE_READ) w=%llu r=%llu wc=%llu rc=%llu",
          w,
          (unsigned long long)bwr->write_size,
          (unsigned long long)bwr->read_size,
          (unsigned long long)bwr->write_consumed,
          (unsigned long long)bwr->read_consumed);

    if (bwr->write_size >= sizeof(uint32_t) && bwr->write_buffer) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)bwr->write_buffer;
        uint32_t cmd = *(const uint32_t*)p;
        ALOGE("libcacao_wrap: %s write cmd=0x%x", w, cmd);
    }
}

/* ---- open/open64 ---- */
extern "C" __attribute__((visibility("default")))
int open(const char* pathname, int flags, ...) {
    using Fn = int (*)(const char*, int, mode_t);
    static Fn real = nullptr;
    if (!real) real = (Fn)dlsym(RTLD_NEXT, "open");

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    int fd = real ? real(pathname, flags, mode) : -1;
    mark_fd(fd, pathname);

    if (pathname && (!strcmp(pathname, "/dev/hwbinder") || !strcmp(pathname, "/dev/binder"))) {
        ALOGE("libcacao_wrap: open(%s) -> fd=%d", pathname, fd);
    }
    return fd;
}

extern "C" __attribute__((visibility("default")))
int open64(const char* pathname, int flags, ...) {
    using Fn = int (*)(const char*, int, mode_t);
    static Fn real = nullptr;
    if (!real) real = (Fn)dlsym(RTLD_NEXT, "open64");

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    int fd = real ? real(pathname, flags, mode) : -1;
    mark_fd(fd, pathname);

    if (pathname && (!strcmp(pathname, "/dev/hwbinder") || !strcmp(pathname, "/dev/binder"))) {
        ALOGE("libcacao_wrap: open64(%s) -> fd=%d", pathname, fd);
    }
    return fd;
}

/* ---- openat/openat64 + __openat/__openat64 ---- */
static inline int do_openat(const char* sym, int dirfd, const char* pathname, int flags, va_list* ap_in) {
    using Fn = int (*)(int, const char*, int, mode_t);
    Fn real = (Fn)dlsym(RTLD_NEXT, sym);

    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = (mode_t)va_arg(*ap_in, int);
    }

    int fd = real ? real(dirfd, pathname, flags, mode) : -1;
    mark_fd(fd, pathname);

    if (pathname && (!strcmp(pathname, "/dev/hwbinder") || !strcmp(pathname, "/dev/binder"))) {
        ALOGE("libcacao_wrap: %s(%s) -> fd=%d", sym, pathname, fd);
    }
    return fd;
}

extern "C" __attribute__((visibility("default")))
int openat(int dirfd, const char* pathname, int flags, ...) {
    va_list ap; va_start(ap, flags);
    int fd = do_openat("openat", dirfd, pathname, flags, &ap);
    va_end(ap);
    return fd;
}

extern "C" __attribute__((visibility("default")))
int openat64(int dirfd, const char* pathname, int flags, ...) {
    va_list ap; va_start(ap, flags);
    int fd = do_openat("openat64", dirfd, pathname, flags, &ap);
    va_end(ap);
    return fd;
}

extern "C" __attribute__((visibility("default")))
int __openat(int dirfd, const char* pathname, int flags, ...) {
    va_list ap; va_start(ap, flags);
    int fd = do_openat("__openat", dirfd, pathname, flags, &ap);
    va_end(ap);
    return fd;
}

extern "C" __attribute__((visibility("default")))
int __openat64(int dirfd, const char* pathname, int flags, ...) {
    va_list ap; va_start(ap, flags);
    int fd = do_openat("__openat64", dirfd, pathname, flags, &ap);
    va_end(ap);
    return fd;
}

/* ---- ioctl ----
 * ✅ 必須符合 bionic: int ioctl(int, int, ...)
 */
extern "C" __attribute__((visibility("default")))
int ioctl(int fd, int req, ...) {
    using Fn = int (*)(int, int, void*);
    static Fn real = nullptr;
    if (!real) real = (Fn)dlsym(RTLD_NEXT, "ioctl");

    va_list ap; va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    int r = real ? real(fd, req, arg) : -1;
    int e = errno;

    const char* w = which_binder(fd);
    if (w && req == (int)BINDER_WRITE_READ) {
        dump_bwr(fd, (binder_write_read*)arg);
        if (r < 0) {
            ALOGE("libcacao_wrap: %s ioctl(BINDER_WRITE_READ) -> %d errno=%d", w, r, e);
        }
    }
    return r;
}
