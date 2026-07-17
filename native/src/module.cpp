#include <sys/types.h>
#include "zygisk.hpp"

#include <jni.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <mutex>
#include <android/log.h>

#define TAG "QQNotifyEvo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Not declared in NDK public headers for all API levels.
extern "C" int __system_property_get(const char* name, char* value);

static JavaVM* g_vm = nullptr;
static int g_api = 0;
static jobject g_app_context = nullptr;  // global ref to android.app.Application

// --- ArtMethod field offsets (determined at runtime) ---
static uint32_t g_access_flags_offset = 4;  // Fixed at 4 on Android M+ (API 23+)
static uint32_t g_data_offset = 0;          // data_ / entry_point_from_jni_
static uint32_t g_entry_point_offset = 0;   // entry_point_from_quick_compiled_code_
static void* g_jni_trampoline = nullptr;    // art_quick_generic_jni_trampoline

// --- Access flag constants ---
// Source: art/modifiers.h in AOSP (stable across all versions)
static constexpr uint32_t kAccNative = 0x0100;
// Version-dependent flags
static uint32_t kAccCompileDontBother = 0;
static uint32_t kAccPreCompiled = 0;

// ============================================================
// (1c) Saved-method state + call-original mechanism
//
// We hook BOTH NotificationManager.notify overloads:
//   notify(String tag, int id, Notification)   -> jni_notify_tag
//   notify(int id, Notification)                -> jni_notify
// NotificationManager.notify(int,Notification) is implemented as
// notify(null, id, notification), so every original dispatch ultimately
// funnels through notify(String,int,Notification). callOriginalStr()
// therefore always invokes the 3-arg original (after temporarily
// restoring its ArtMethod), which is sufficient to post any notification.
// ============================================================

struct SavedMethod {
    uint8_t* art;         // ArtMethod* address
    uint32_t orig_flags;  // access_flags_ before native-ization
    void* orig_data;      // data_  before hook
    void* orig_entry;     // entry_point before hook
};

static std::mutex g_hook_mtx;              // serializes the whole hook body
static thread_local bool g_in_hook = false;  // reentrancy guard

static jmethodID g_notify_str_mid = nullptr;  // notify(String,int,Notification)
static jmethodID g_notify_int_mid = nullptr;  // notify(int,Notification)
static SavedMethod g_saved_tag;
static SavedMethod g_saved_int;

// Forward declarations of native callbacks
static void jni_notify_tag(JNIEnv* env, jobject thiz, jstring tag, jint id, jobject notif);
static void jni_notify(JNIEnv* env, jobject thiz, jint id, jobject notif);

// ============================================================
// Initialize version-dependent access flag values
// ============================================================

static void initAccessFlags(int api_level) {
    // kAccCompileDontBother value depends on Android version
    if (api_level >= 27) {
        kAccCompileDontBother = 0x02000000;  // O_MR1+
    } else if (api_level >= 24) {
        kAccCompileDontBother = 0x00080000;  // N
    }

    // kAccPreCompiled only on Android 11+
    if (api_level == 30) {
        kAccPreCompiled = 0x00200000;  // R
    } else if (api_level >= 31) {
        kAccPreCompiled = 0x01000000;  // S+
    }

    LOGI("API level=%d, kAccCompileDontBother=0x%x, kAccPreCompiled=0x%x",
         api_level, kAccCompileDontBother, kAccPreCompiled);
}

// ============================================================
// Determine ArtMethod field offsets AND JNI trampoline at runtime.
//
// Key insight: Object.hashCode() is a NATIVE method. Its ArtMethod's
// entry_point_from_compiled_code_ already points to
// art_quick_generic_jni_trampoline. We read it directly — no dlopen,
// no ELF parsing, no symbol resolution needed.
//
// Offset strategy (same as Pine/epic):
// 1. access_flags_ is at offset 4 (GcRoot<Class> is 4 bytes, then access_flags_)
// 2. sizeof(ArtMethod) = gap between adjacent methods
// 3. data_ and entry_point are the last two pointer-sized fields
// ============================================================

static bool determineOffsets(JNIEnv* env) {
    jclass objClass = env->FindClass("java/lang/Object");
    if (!objClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("Cannot find java.lang.Object");
        return false;
    }

    // Get hashCode — a native method whose entry_point IS the JNI trampoline
    jmethodID hashCodeMid = env->GetMethodID(objClass, "hashCode", "()I");
    jmethodID equalsMid = env->GetMethodID(objClass, "equals", "(Ljava/lang/Object;)Z");
    jmethodID toStringMid = env->GetMethodID(objClass, "toString", "()Ljava/lang/String;");

    if (env->ExceptionCheck() || !hashCodeMid || !equalsMid || !toStringMid) {
        env->ExceptionClear();
        LOGE("Cannot get Object methods");
        env->DeleteLocalRef(objClass);
        return false;
    }

    // Convert jmethodID → real ArtMethod* via reflection.
    // FromReflectedMethod reads Method.artMethod field directly — works on all
    // Android versions including R+ where jmethodID may be index-encoded.
    uintptr_t ptrs[3];
    jmethodID mids[3] = {hashCodeMid, equalsMid, toStringMid};
    for (int i = 0; i < 3; i++) {
        jobject ref = env->ToReflectedMethod(objClass, mids[i], JNI_FALSE);
        ptrs[i] = reinterpret_cast<uintptr_t>(env->FromReflectedMethod(ref));
        env->DeleteLocalRef(ref);
    }

    // Keep hashCode's ArtMethod pointer for trampoline extraction
    uintptr_t hashCodeArtMethod = ptrs[0];

    env->DeleteLocalRef(objClass);

    LOGI("hashCode ArtMethod @ %p", (void*)hashCodeArtMethod);

    // Sort to find adjacent method pairs → sizeof(ArtMethod)
    if (ptrs[0] > ptrs[1]) std::swap(ptrs[0], ptrs[1]);
    if (ptrs[1] > ptrs[2]) std::swap(ptrs[1], ptrs[2]);
    if (ptrs[0] > ptrs[1]) std::swap(ptrs[0], ptrs[1]);

    size_t gap1 = ptrs[1] - ptrs[0];
    size_t gap2 = ptrs[2] - ptrs[1];
    size_t artMethodSize = 0;
    if (gap1 > 0 && gap2 > 0) {
        artMethodSize = std::min(gap1, gap2);
    } else if (gap1 > 0) {
        artMethodSize = gap1;
    } else if (gap2 > 0) {
        artMethodSize = gap2;
    }

    if (artMethodSize < sizeof(void*) * 2 || artMethodSize > 128) {
        LOGE("Unusual ArtMethod size %zu, using fallback", artMethodSize);
        artMethodSize = (sizeof(void*) == 8) ? 32 : 24;
    }

    // PtrSizedFields are the last two pointer-sized fields:
    //   { void* data_, void* entry_point_from_quick_compiled_code_ }
    g_access_flags_offset = 4;
    g_data_offset = artMethodSize - 2 * sizeof(void*);
    g_entry_point_offset = artMethodSize - sizeof(void*);

    // Read the JNI trampoline from hashCode's entry_point.
    // hashCode() is native, so entry_point = art_quick_generic_jni_trampoline.
    g_jni_trampoline = *reinterpret_cast<void**>(
        hashCodeArtMethod + g_entry_point_offset);

    LOGI("ArtMethod: size=%zu, access_flags@%u, data@%u, entry@%u, trampoline=%p",
         artMethodSize, g_access_flags_offset, g_data_offset,
         g_entry_point_offset, g_jni_trampoline);

    if (!g_jni_trampoline) {
        LOGE("Trampoline is null — cannot proceed");
        return false;
    }

    return true;
}

// ============================================================
// installHook: native-ize an ArtMethod to dispatch to jniFn,
// saving the original (flags/data/entry) so it can be restored.
// ============================================================

static bool installHook(JNIEnv* env, jobject method, void* jniFn, SavedMethod& s) {
    auto* artMethod = reinterpret_cast<uint8_t*>(env->FromReflectedMethod(method));
    if (!artMethod) {
        LOGE("FromReflectedMethod returned null");
        return false;
    }

    // Make ArtMethod memory writable
    long pageSize = sysconf(_SC_PAGE_SIZE);
    uintptr_t pageStart = reinterpret_cast<uintptr_t>(artMethod) & ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(pageStart), pageSize * 2,
                 PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect failed for ArtMethod at %p", artMethod);
        return false;
    }

    // Save original state BEFORE native-izing
    s.art = artMethod;
    s.orig_flags = *reinterpret_cast<uint32_t*>(artMethod + g_access_flags_offset);
    s.orig_data  = *reinterpret_cast<void**>(artMethod + g_data_offset);
    s.orig_entry = *reinterpret_cast<void**>(artMethod + g_entry_point_offset);

    // 1. Set access flags: add kAccNative, clear kAccPreCompiled
    uint32_t newFlags = s.orig_flags;
    newFlags |= kAccNative;
    if (kAccCompileDontBother) newFlags |= kAccCompileDontBother;
    if (kAccPreCompiled)       newFlags &= ~kAccPreCompiled;
    *reinterpret_cast<uint32_t*>(artMethod + g_access_flags_offset) = newFlags;

    // 2. Set data_ to our native function
    *reinterpret_cast<void**>(artMethod + g_data_offset) = jniFn;

    // 3. Set entry_point to the JNI bridge trampoline
    *reinterpret_cast<void**>(artMethod + g_entry_point_offset) = g_jni_trampoline;

    LOGI("Method hooked: art=%p old flags=0x%x → new flags=0x%x, native=%p",
         artMethod, s.orig_flags, newFlags, jniFn);
    return true;
}

// ============================================================
// callOriginalStr: temporarily restore notify(String,int,Notification),
// invoke it, then re-hook. Caller MUST hold g_hook_mtx.
//
// Restoring the 3-arg method and invoking it via g_notify_str_mid runs
// the framework's original bytecode (which performs the actual post),
// independent of which overload QQ originally called.
// ============================================================

static void callOriginalStr(JNIEnv* env, jobject thiz, jint id, jstring tag, jobject notif) {
    SavedMethod& s = g_saved_tag;
    if (!s.art) {
        LOGE("callOriginalStr: no saved method");
        return;
    }

    // Temporarily restore original fields
    *reinterpret_cast<uint32_t*>(s.art + g_access_flags_offset) = s.orig_flags;
    *reinterpret_cast<void**>(s.art + g_data_offset) = s.orig_data;
    *reinterpret_cast<void**>(s.art + g_entry_point_offset) = s.orig_entry;

    // Invoke original notify(String,int,Notification)
    env->CallVoidMethod(thiz, g_notify_str_mid, tag, id, notif);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    // Re-hook (rewrite native fields)
    uint32_t f = s.orig_flags;
    f |= kAccNative;
    if (kAccCompileDontBother) f |= kAccCompileDontBother;
    if (kAccPreCompiled) f &= ~kAccPreCompiled;
    *reinterpret_cast<uint32_t*>(s.art + g_access_flags_offset) = f;
    *reinterpret_cast<void**>(s.art + g_data_offset) = reinterpret_cast<void*>(jni_notify_tag);
    *reinterpret_cast<void**>(s.art + g_entry_point_offset) = g_jni_trampoline;
}

// ============================================================
// Native callbacks for the two notify overloads.
//
// Commit-2 milestone: pass-through (proves the hook + call-original
// mechanism without altering notifications). Parse + rebuild lands later.
// ============================================================

static void jni_notify_tag(JNIEnv* env, jobject thiz, jstring tag, jint id, jobject notif) {
    std::lock_guard<std::mutex> lk(g_hook_mtx);
    if (g_in_hook) {
        // Re-entrant dispatch while we are already processing on this thread:
        // do not re-parse, just post the (already-built) notification.
        callOriginalStr(env, thiz, id, tag, notif);
        return;
    }
    callOriginalStr(env, thiz, id, tag, notif);
}

static void jni_notify(JNIEnv* env, jobject thiz, jint id, jobject notif) {
    std::lock_guard<std::mutex> lk(g_hook_mtx);
    if (g_in_hook) {
        callOriginalStr(env, thiz, id, nullptr, notif);
        return;
    }
    // notify(int,Notification) == notify(null,id,Notification)
    callOriginalStr(env, thiz, id, nullptr, notif);
}

// ============================================================
// installNotifyHooks: locate both NotificationManager.notify overloads
// and native-ize them.
// ============================================================

static void installNotifyHooks(JNIEnv* env) {
    jclass nmClass = env->FindClass("android/app/NotificationManager");
    if (!nmClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("NotificationManager not found");
        return;
    }

    g_notify_str_mid = env->GetMethodID(nmClass, "notify",
        "(Ljava/lang/String;ILandroid/app/Notification;)V");
    g_notify_int_mid = env->GetMethodID(nmClass, "notify",
        "(ILandroid/app/Notification;)V");
    if (env->ExceptionCheck() || !g_notify_str_mid || !g_notify_int_mid) {
        env->ExceptionClear();
        LOGE("notify method ids not found");
        env->DeleteLocalRef(nmClass);
        return;
    }

    jobject mStr = env->ToReflectedMethod(nmClass, g_notify_str_mid, JNI_FALSE);
    jobject mInt = env->ToReflectedMethod(nmClass, g_notify_int_mid, JNI_FALSE);
    env->DeleteLocalRef(nmClass);
    if (!mStr || !mInt) {
        LOGE("ToReflectedMethod failed");
        return;
    }

    bool ok1 = installHook(env, mStr, reinterpret_cast<void*>(jni_notify_tag), g_saved_tag);
    bool ok2 = installHook(env, mInt, reinterpret_cast<void*>(jni_notify), g_saved_int);
    env->DeleteLocalRef(mStr);
    env->DeleteLocalRef(mInt);

    LOGI("notify hooks installed: tag=%d int=%d", ok1, ok2);
}

// ============================================================
// Zygisk module entry point
// ============================================================

class QQNotifyEvoModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        env->GetJavaVM(&g_vm);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        JNIEnv* env;
        g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);

        const char* name = env->GetStringUTFChars(args->nice_name, nullptr);
        is_target = (name && (strcmp(name, "com.tencent.mobileqq") == 0 ||
                              strcmp(name, "com.tencent.tim") == 0));
        env->ReleaseStringUTFChars(args->nice_name, name);

        if (!is_target) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!is_target) return;

        JNIEnv* env;
        g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);

        char sdkStr[8] = {0};
        __system_property_get("ro.build.version.sdk", sdkStr);
        g_api = atoi(sdkStr);
        if (g_api < 26) {
            LOGE("Android API %d < 26, not supported", g_api);
            return;
        }

        initAccessFlags(g_api);

        if (!determineOffsets(env)) {
            LOGE("Failed to determine offsets/trampoline");
            return;
        }

        // Cache Application context (for channel creation later)
        jclass at = env->FindClass("android/app/ActivityThread");
        if (at) {
            jmethodID cur = env->GetStaticMethodID(at,
                "currentApplication", "()Landroid/app/Application;");
            if (cur) {
                jobject app = env->CallStaticObjectMethod(at, cur);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (app) {
                    g_app_context = env->NewGlobalRef(app);
                    env->DeleteLocalRef(app);
                }
            }
            env->DeleteLocalRef(at);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();

        // createChannelsOnce(env);  // introduced when the rebuild layer lands

        installNotifyHooks(env);
    }

private:
    zygisk::Api* api = nullptr;
    bool is_target = false;
};

REGISTER_ZYGISK_MODULE(QQNotifyEvoModule)
