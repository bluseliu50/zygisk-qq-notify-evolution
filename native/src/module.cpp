#include <sys/types.h>
#include "zygisk.hpp"

#include <jni.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <algorithm>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <android/log.h>

#include "parser.hpp"

#define TAG "QQNotifyEvo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Not declared in NDK public headers for all API levels.
extern "C" int __system_property_get(const char* name, char* value);

static JavaVM* g_vm = nullptr;
static int g_api = 0;
static jobject g_app_context = nullptr;  // global ref to android.app.Application

// ============================================================
// Hook strategy (framework-free — no Pine/SandHook/Dobby/LSPlant)
//
// We native-ize exactly ONE ArtMethod:
//   android.app.NotificationManager.notify(String,int,Notification)
// using the same direct-ArtMethod-edit technique as zygisk-qq-native-emoji:
// set kAccNative, point data_ at our JNI function, point entry_point at
// art_quick_generic_jni_trampoline (read from Object.hashCode()'s ArtMethod).
//
// We do NOT call the original notify() back from the hook. Restoring an
// ArtMethod's fields and re-dispatching through it is unreliable on
// Android 16 (API 37): the invoke returns but the notification is silently
// lost (deopt'd callers / stale dispatch caches never get re-invalidated).
//
// Instead, we deliver the (rebuilt) Notification by calling the UNHOOKED
// public method
//   NotificationManager.notifyAsUser(String,int,Notification,UserHandle)
// directly via JNI. notifyAsUser is exactly what notify() calls internally;
// its ArtMethod is pristine, so dispatch reaches NotificationManagerService
// normally. No restore, no QRoute proxy, no call-original dance.
//
// NotificationManager.notify(int,Notification) is implemented in framework
// as notify(null,id,notification), so QQ traffic — regardless of which
// overload QQ calls — funnels through the 3-arg form we hook.
// ============================================================

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

struct SavedMethod {
    uint8_t* art;         // ArtMethod* address
    uint32_t orig_flags;  // access_flags_ before native-ization
    void* orig_data;      // data_  before hook
    void* orig_entry;     // entry_point before hook
};

static std::mutex g_hook_mtx;                // serializes hook body + history
static thread_local bool g_in_hook = false;  // reentrancy guard

static SavedMethod g_saved_notify;                 // notify(String,int,Notification)
static jmethodID g_notifyAsUser_mid = nullptr;     // unhooked delivery path
static jobject   g_userhandle_all = nullptr;       // UserHandle.ALL (global ref)

// Forward declaration of the native hook callback.
static void jni_notify_tag(JNIEnv* env, jobject thiz, jstring tag, jint id, jobject notif);

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
// saving the original (flags/data/entry) for diagnostics.
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
// Cached JNI ids + notification rebuild layer.
//
// NotificationManager.notify is a low-frequency, human-paced event, so we
// lazily cache class/method/field ids on first use. If caching or building
// fails for any reason we return null and the caller posts the original
// notification — a notification is NEVER dropped.
// ============================================================

static bool g_ids_cached = false;

// classes (global refs)
static jclass g_Notif_cls;
static jclass g_Bundle_cls;
static jclass g_Builder_cls;       // android.app.Notification$Builder
static jclass g_MsgStyle_cls;      // android.app.Notification$MessagingStyle
static jclass g_Person_cls;        // android.app.Person                (API 28+)
static jclass g_PersonB_cls;       // android.app.Person$Builder        (API 28+)
static jclass g_System_cls;
static jclass g_ShortcutInfo_cls;       // API 25+
static jclass g_ShortcutInfoB_cls;      // API 25+
static jclass g_LocusId_cls;            // API 30+

// Notification fields
static jfieldID f_Notif_extras;
static jfieldID f_Notif_tickerText;
static jfieldID f_Notif_when;
static jfieldID f_Notif_smallIcon;
static jfieldID f_Notif_color;
static jfieldID f_Notif_contentIntent;
static jfieldID f_Notif_deleteIntent;
// Notification methods
static jmethodID m_Notif_getLargeIcon;
// Bundle
static jmethodID m_Bundle_getCharSequence;
// Object
static jmethodID m_Object_toString;
// System
static jmethodID m_System_currentTimeMillis;

// Builder methods
static jmethodID m_B_ctor;          // Builder(Context, String)
static jmethodID m_B_setSmallIcon;  // setSmallIcon(Icon)
static jmethodID m_B_setLargeIcon;  // setLargeIcon(Icon)
static jmethodID m_B_setStyle;
static jmethodID m_B_setColor;
static jmethodID m_B_setWhen;
static jmethodID m_B_setShowWhen;
static jmethodID m_B_setContentIntent;
static jmethodID m_B_setDeleteIntent;
static jmethodID m_B_setAutoCancel;
static jmethodID m_B_setCategory;
static jmethodID m_B_setShortcutId;
static jmethodID m_B_build;

// MessagingStyle (API 28+ path)
static jmethodID m_MS_ctor_person;     // MessagingStyle(Person)
static jmethodID m_MS_addMessage3;     // addMessage(CharSequence, long, Person)
static jmethodID m_MS_setConvTitle;
static jmethodID m_MS_setGroupConv;
// MessagingStyle (deprecated API 26/27 path)
static jmethodID m_MS_ctor_cs;         // MessagingStyle(CharSequence)
static jmethodID m_MS_addMessage_msg;  // addMessage(Message)
static jclass     g_MsgStyle_Msg_cls;  // MessagingStyle$Message
static jmethodID  m_MS_Msg_ctor_cs;    // Message(CharSequence, long, CharSequence)

// Person.Builder (API 28+)
static jmethodID m_PB_ctor;
static jmethodID m_PB_setName;
static jmethodID m_PB_setIcon;
static jmethodID m_PB_build;

// ShortcutInfo.Builder (API 30+)
static jmethodID m_SI_ctor;         // Builder(Context, String)
static jmethodID m_SI_setShortLabel;
static jmethodID m_SI_setIcon;
static jmethodID m_SI_setLongLived;
static jmethodID m_SI_setLocusId;
static jmethodID m_SI_build;
// ShortcutManager.addDynamicShortcuts(List)
static jmethodID m_Context_getSystemService_str;

static jclass makeGlobalClass(JNIEnv* env, const char* name) {
    jclass local = env->FindClass(name);
    if (!local || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

// Cache the Application context (android.app.Application).
// postAppSpecialize may run before Application is created (early fork), so we
// fetch it lazily on first hook fire when the app is fully up.
static bool ensureAppContext(JNIEnv* env) {
    if (g_app_context) return true;
    jclass at = env->FindClass("android/app/ActivityThread");
    if (!at || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jmethodID cur = env->GetStaticMethodID(at, "currentApplication",
                                            "()Landroid/app/Application;");
    if (cur && !env->ExceptionCheck()) {
        jobject app = env->CallStaticObjectMethod(at, cur);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (app) {
            g_app_context = env->NewGlobalRef(app);
            env->DeleteLocalRef(app);
            LOGI("app context cached lazily: %p", g_app_context);
        }
    } else {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(at);
    return g_app_context != nullptr;
}

static bool cacheJniIds(JNIEnv* env) {
    if (g_ids_cached) return true;
    if (!ensureAppContext(env)) { LOGE("cacheJniIds: g_app_context still null"); return false; }

    g_Notif_cls   = makeGlobalClass(env, "android/app/Notification");
    g_Bundle_cls  = makeGlobalClass(env, "android/os/Bundle");
    g_Builder_cls = makeGlobalClass(env, "android/app/Notification$Builder");
    g_MsgStyle_cls = makeGlobalClass(env, "android/app/Notification$MessagingStyle");
    g_System_cls  = makeGlobalClass(env, "java/lang/System");
    if (!g_Notif_cls || !g_Bundle_cls || !g_Builder_cls || !g_MsgStyle_cls || !g_System_cls) {
        LOGE("cacheJniIds: core class missing");
        return false;
    }

    // Mandatory fields (present on all supported API levels):
    f_Notif_extras        = env->GetFieldID(g_Notif_cls, "extras",    "Landroid/os/Bundle;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    f_Notif_when          = env->GetFieldID(g_Notif_cls, "when",      "J");
    if (env->ExceptionCheck()) env->ExceptionClear();
    f_Notif_contentIntent = env->GetFieldID(g_Notif_cls, "contentIntent", "Landroid/app/PendingIntent;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    f_Notif_deleteIntent  = env->GetFieldID(g_Notif_cls, "deleteIntent",  "Landroid/app/PendingIntent;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!f_Notif_extras)        { LOGE("cacheJniIds: Notification.extras missing"); return false; }
    if (!f_Notif_when)          { LOGE("cacheJniIds: Notification.when missing"); return false; }
    if (!f_Notif_contentIntent) { LOGE("cacheJniIds: Notification.contentIntent missing"); return false; }
    if (!f_Notif_deleteIntent)  { LOGE("cacheJniIds: Notification.deleteIntent missing"); return false; }
    // Optional / version-variable fields (may be absent on newer Android):
    f_Notif_tickerText = env->GetFieldID(g_Notif_cls, "tickerText", "Ljava/lang/CharSequence;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    f_Notif_smallIcon  = env->GetFieldID(g_Notif_cls, "smallIcon",  "Landroid/graphics/drawable/Icon;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    f_Notif_color      = env->GetFieldID(g_Notif_cls, "color",      "I");
    if (env->ExceptionCheck()) env->ExceptionClear();
    m_Notif_getLargeIcon = env->GetMethodID(g_Notif_cls, "getLargeIcon", "()Landroid/graphics/drawable/Icon;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGI("cacheJniIds: optional fields — ticker=%d smallIcon=%d color=%d largeIcon=%d",
         f_Notif_tickerText?1:0, f_Notif_smallIcon?1:0, f_Notif_color?1:0, m_Notif_getLargeIcon?1:0);

    m_Bundle_getCharSequence = env->GetMethodID(g_Bundle_cls, "getCharSequence",
                                                "(Ljava/lang/String;)Ljava/lang/CharSequence;");
    jclass objCls = env->FindClass("java/lang/Object");
    m_Object_toString = env->GetMethodID(objCls, "toString", "()Ljava/lang/String;");
    env->DeleteLocalRef(objCls);
    m_System_currentTimeMillis = env->GetStaticMethodID(g_System_cls, "currentTimeMillis", "()J");
    if (env->ExceptionCheck() || !m_Bundle_getCharSequence || !m_Object_toString ||
        !m_System_currentTimeMillis) {
        env->ExceptionClear();
        LOGE("cacheJniIds: Bundle/Object/System missing");
        return false;
    }

    // Builder methods (all return Notification$Builder)
    m_B_ctor          = env->GetMethodID(g_Builder_cls, "<init>", "(Landroid/content/Context;Ljava/lang/String;)V");
    m_B_setSmallIcon  = env->GetMethodID(g_Builder_cls, "setSmallIcon", "(Landroid/graphics/drawable/Icon;)Landroid/app/Notification$Builder;");
    m_B_setLargeIcon  = env->GetMethodID(g_Builder_cls, "setLargeIcon", "(Landroid/graphics/drawable/Icon;)Landroid/app/Notification$Builder;");
    m_B_setStyle      = env->GetMethodID(g_Builder_cls, "setStyle", "(Landroid/app/Notification$Style;)Landroid/app/Notification$Builder;");
    m_B_setColor      = env->GetMethodID(g_Builder_cls, "setColor", "(I)Landroid/app/Notification$Builder;");
    m_B_setWhen       = env->GetMethodID(g_Builder_cls, "setWhen", "(J)Landroid/app/Notification$Builder;");
    m_B_setShowWhen   = env->GetMethodID(g_Builder_cls, "setShowWhen", "(Z)Landroid/app/Notification$Builder;");
    m_B_setContentIntent = env->GetMethodID(g_Builder_cls, "setContentIntent", "(Landroid/app/PendingIntent;)Landroid/app/Notification$Builder;");
    m_B_setDeleteIntent  = env->GetMethodID(g_Builder_cls, "setDeleteIntent",  "(Landroid/app/PendingIntent;)Landroid/app/Notification$Builder;");
    m_B_setAutoCancel  = env->GetMethodID(g_Builder_cls, "setAutoCancel", "(Z)Landroid/app/Notification$Builder;");
    m_B_setCategory    = env->GetMethodID(g_Builder_cls, "setCategory", "(Ljava/lang/String;)Landroid/app/Notification$Builder;");
    m_B_setShortcutId  = env->GetMethodID(g_Builder_cls, "setShortcutId", "(Ljava/lang/String;)Landroid/app/Notification$Builder;");
    m_B_build          = env->GetMethodID(g_Builder_cls, "build", "()Landroid/app/Notification;");
    if (env->ExceptionCheck() || !m_B_ctor || !m_B_setSmallIcon || !m_B_setStyle ||
        !m_B_setColor || !m_B_setWhen || !m_B_setShowWhen || !m_B_setContentIntent ||
        !m_B_setDeleteIntent || !m_B_setAutoCancel || !m_B_setCategory || !m_B_build) {
        env->ExceptionClear();
        LOGE("cacheJniIds: Builder methods missing");
        return false;
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (g_api >= 28) {
        g_Person_cls  = makeGlobalClass(env, "android/app/Person");
        g_PersonB_cls = makeGlobalClass(env, "android/app/Person$Builder");
        if (!g_Person_cls || !g_PersonB_cls) { LOGE("cacheJniIds: Person missing"); return false; }
        m_MS_ctor_person = env->GetMethodID(g_MsgStyle_cls, "<init>", "(Landroid/app/Person;)V");
        m_MS_addMessage3 = env->GetMethodID(g_MsgStyle_cls, "addMessage",
            "(Ljava/lang/CharSequence;JLandroid/app/Person;)Landroid/app/Notification$MessagingStyle;");
        m_MS_setConvTitle = env->GetMethodID(g_MsgStyle_cls, "setConversationTitle",
            "(Ljava/lang/CharSequence;)Landroid/app/Notification$MessagingStyle;");
        m_MS_setGroupConv = env->GetMethodID(g_MsgStyle_cls, "setGroupConversation",
            "(Z)Landroid/app/Notification$MessagingStyle;");
        m_PB_ctor  = env->GetMethodID(g_PersonB_cls, "<init>", "()V");
        m_PB_setName = env->GetMethodID(g_PersonB_cls, "setName", "(Ljava/lang/CharSequence;)Landroid/app/Person$Builder;");
        m_PB_setIcon = env->GetMethodID(g_PersonB_cls, "setIcon", "(Landroid/graphics/drawable/Icon;)Landroid/app/Person$Builder;");
        m_PB_build = env->GetMethodID(g_PersonB_cls, "build", "()Landroid/app/Person;");
        if (env->ExceptionCheck() || !m_MS_ctor_person || !m_MS_addMessage3 ||
            !m_MS_setConvTitle || !m_MS_setGroupConv || !m_PB_ctor || !m_PB_setName ||
            !m_PB_setIcon || !m_PB_build) {
            env->ExceptionClear();
            LOGE("cacheJniIds: API28 MessagingStyle/Person missing");
            return false;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    } else {
        // Deprecated API 26/27 path
        g_MsgStyle_Msg_cls = makeGlobalClass(env, "android/app/Notification$MessagingStyle$Message");
        if (!g_MsgStyle_Msg_cls) { LOGE("cacheJniIds: Message class missing"); return false; }
        m_MS_ctor_cs = env->GetMethodID(g_MsgStyle_cls, "<init>", "(Ljava/lang/CharSequence;)V");
        m_MS_Msg_ctor_cs = env->GetMethodID(g_MsgStyle_Msg_cls, "<init>",
            "(Ljava/lang/CharSequence;JLjava/lang/CharSequence;)V");
        m_MS_addMessage_msg = env->GetMethodID(g_MsgStyle_cls, "addMessage",
            "(Landroid/app/Notification$MessagingStyle$Message;)V");
        if (env->ExceptionCheck() || !m_MS_ctor_cs || !m_MS_Msg_ctor_cs || !m_MS_addMessage_msg) {
            env->ExceptionClear();
            LOGE("cacheJniIds: deprecated MessagingStyle missing");
            return false;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (g_api >= 30) {
        g_ShortcutInfo_cls  = makeGlobalClass(env, "android/content/pm/ShortcutInfo");
        g_ShortcutInfoB_cls = makeGlobalClass(env, "android/content/pm/ShortcutInfo$Builder");
        g_LocusId_cls       = makeGlobalClass(env, "android/content/LocusId");
        if (g_ShortcutInfo_cls && g_ShortcutInfoB_cls) {
            m_SI_ctor = env->GetMethodID(g_ShortcutInfoB_cls, "<init>",
                "(Landroid/content/Context;Ljava/lang/String;)V");
            m_SI_setShortLabel = env->GetMethodID(g_ShortcutInfoB_cls, "setShortLabel",
                "(Ljava/lang/CharSequence;)Landroid/content/pm/ShortcutInfo$Builder;");
            m_SI_setIcon = env->GetMethodID(g_ShortcutInfoB_cls, "setIcon",
                "(Landroid/graphics/drawable/Icon;)Landroid/content/pm/ShortcutInfo$Builder;");
            m_SI_setLongLived = env->GetMethodID(g_ShortcutInfoB_cls, "setLongLived",
                "(Z)Landroid/content/pm/ShortcutInfo$Builder;");
            m_SI_setLocusId = env->GetMethodID(g_ShortcutInfoB_cls, "setLocusId",
                "(Landroid/content/LocusId;)Landroid/content/pm/ShortcutInfo$Builder;");
            m_SI_build = env->GetMethodID(g_ShortcutInfoB_cls, "build",
                "()Landroid/content/pm/ShortcutInfo;");
        }
        jclass ctxCls = env->FindClass("android/content/Context");
        if (ctxCls) {
            m_Context_getSystemService_str = env->GetMethodID(ctxCls, "getSystemService",
                "(Ljava/lang/String;)Ljava/lang/Object;");
            env->DeleteLocalRef(ctxCls);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    g_ids_cached = true;
    LOGI("JNI ids cached (api=%d, person=%d shortcut=%d)",
         g_api, g_api >= 28 ? 1 : 0, g_api >= 30 ? 1 : 0);
    return true;
}

// --- small JNI helpers ----------------------------------------------------

static std::string jstrToStd(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? std::string(c) : std::string();
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

static std::string csToString(JNIEnv* env, jobject cs) {
    if (!cs) return std::string();
    jstring s = static_cast<jstring>(env->CallObjectMethod(cs, m_Object_toString));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return std::string(); }
    std::string out = jstrToStd(env, s);
    if (s) env->DeleteLocalRef(s);
    return out;
}

static std::string extrasGetStr(JNIEnv* env, jobject extras, const char* key) {
    if (!extras) return std::string();
    jstring k = env->NewStringUTF(key);
    jobject cs = env->CallObjectMethod(extras, m_Bundle_getCharSequence, k);
    env->DeleteLocalRef(k);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return std::string(); }
    std::string out = csToString(env, cs);
    if (cs) env->DeleteLocalRef(cs);
    return out;
}

// ============================================================
// Notification channels — ported from QAuxiliary/Utils.kt.
// Channels default to the system notification sound when unset.
// Idempotent: safe to call each process start.
// ============================================================

static bool g_channels_created = false;

static const char* channelId(const ParsedMsg& pm) {
    switch (pm.type) {
        case MsgType::Private: return pm.special ? "QQ_Friend_Special" : "QQ_Friend";
        case MsgType::Group:   return "QQ_Group";
        case MsgType::QZone:
        case MsgType::QZoneSpecial: return "QQ_Zone";
        default: return "QQ_Friend";
    }
}

static void createChannelsOnce(JNIEnv* env) {
    if (g_channels_created || !g_app_context) return;
    if (env->PushLocalFrame(32) != 0) { env->ExceptionClear(); return; }

    jclass ctxCls = env->FindClass("android/content/Context");
    jclass nmCls  = env->FindClass("android/app/NotificationManager");
    jclass grpCls = env->FindClass("android/app/NotificationChannelGroup");
    jclass chnCls = env->FindClass("android/app/NotificationChannel");
    jclass listCls= env->FindClass("java/util/ArrayList");
    if (!ctxCls || !nmCls || !grpCls || !chnCls || !listCls || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("createChannelsOnce: classes missing");
        env->PopLocalFrame(nullptr);
        return;
    }

    jmethodID getSystemService = env->GetMethodID(ctxCls, "getSystemService",
        "(Ljava/lang/Class;)Ljava/lang/Object;");
    jmethodID grpCtor = env->GetMethodID(grpCls, "<init>",
        "(Ljava/lang/String;Ljava/lang/CharSequence;)V");
    jmethodID chnCtor = env->GetMethodID(chnCls, "<init>",
        "(Ljava/lang/String;Ljava/lang/CharSequence;I)V");
    jmethodID setGroup = env->GetMethodID(chnCls, "setGroup", "(Ljava/lang/String;)V");
    jmethodID enableVibration = env->GetMethodID(chnCls, "enableVibration", "(Z)V");
    jmethodID enableLights = env->GetMethodID(chnCls, "enableLights", "(Z)V");
    jmethodID createGrp = env->GetMethodID(nmCls, "createNotificationChannelGroup",
        "(Landroid/app/NotificationChannelGroup;)V");
    jmethodID createChans = env->GetMethodID(nmCls, "createNotificationChannels",
        "(Ljava/util/List;)V");
    jmethodID listCtor = env->GetMethodID(listCls, "<init>", "()V");
    jmethodID listAdd  = env->GetMethodID(listCls, "add", "(Ljava/lang/Object;)Z");
    if (env->ExceptionCheck() || !getSystemService || !grpCtor || !chnCtor || !setGroup ||
        !enableVibration || !enableLights || !createGrp || !createChans || !listCtor || !listAdd) {
        env->ExceptionClear();
        LOGE("createChannelsOnce: methods missing");
        env->PopLocalFrame(nullptr);
        return;
    }

    jobject nm = env->CallObjectMethod(g_app_context, getSystemService, nmCls);
    if (env->ExceptionCheck() || !nm) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return; }

    // group
    jstring gid = env->NewStringUTF("qq_evolution");
    jstring gname = env->NewStringUTF("QQ通知进化");
    jobject group = env->NewObject(grpCls, grpCtor, gid, gname);
    env->CallVoidMethod(nm, createGrp, group);
    if (env->ExceptionCheck()) env->ExceptionClear();

    // channels: {id, name, importance, vibrate, lights}
    struct ChDef { const char* id; const char* name; int importance; bool vib; bool lights; };
    const ChDef defs[] = {
        {"QQ_Friend",         "联系人消息",   4 /*IMPORTANCE_HIGH*/,     true,  true},
        {"QQ_Friend_Special", "特别关心消息", 4 /*IMPORTANCE_HIGH*/,     true,  true},
        {"QQ_Group",          "群消息",       4 /*IMPORTANCE_HIGH*/,     true,  true},
        {"QQ_Zone",           "空间动态",     3 /*IMPORTANCE_DEFAULT*/,  false, true},
    };

    jobject list = env->NewObject(listCls, listCtor);
    for (const auto& d : defs) {
        jstring cid = env->NewStringUTF(d.id);
        jstring cname = env->NewStringUTF(d.name);
        jobject ch = env->NewObject(chnCls, chnCtor, cid, cname, d.importance);
        env->CallVoidMethod(ch, setGroup, gid);
        if (d.vib)   env->CallVoidMethod(ch, enableVibration, JNI_TRUE);
        if (d.lights)env->CallVoidMethod(ch, enableLights, JNI_TRUE);
        env->CallBooleanMethod(list, listAdd, ch);
        env->DeleteLocalRef(ch);
        env->DeleteLocalRef(cname);
        env->DeleteLocalRef(cid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
    }
    env->CallVoidMethod(nm, createChans, list);
    if (env->ExceptionCheck()) env->ExceptionClear();

    g_channels_created = true;
    LOGI("Notification channels created");
    env->PopLocalFrame(nullptr);
}

// ============================================================
// Conversation history — rolling window per conversation.
// ============================================================

struct MsgEntry {
    std::string sender;
    std::string text;
    int64_t when;
};

static std::unordered_map<std::string, std::vector<MsgEntry>> g_history;

static std::string historyKey(const ParsedMsg& pm) {
    switch (pm.type) {
        case MsgType::Group:
            return "g:" + (pm.groupName.empty() ? pm.name : pm.groupName);
        case MsgType::Private: return "p:" + pm.name;
        case MsgType::Binding: return "b:" + pm.name;
        case MsgType::QZone:
        case MsgType::QZoneSpecial: return "z:qzone";
        default: return "x:other";
    }
}

// Caller MUST hold g_hook_mtx.
static void updateHistory(const ParsedMsg& pm, const std::string& senderName, int64_t when) {
    std::string key = historyKey(pm);
    auto& v = g_history[key];
    // trim to num (keep most recent), then append, then cap at 5
    while (v.size() > static_cast<size_t>(pm.num) && !v.empty()) v.erase(v.begin());
    v.push_back({senderName, pm.content, when});
    while (v.size() > 5) v.erase(v.begin());
}

// ============================================================
// Build a Person (API 28+). Returns a local ref (may be null).
// ============================================================

static jobject buildPerson(JNIEnv* env, const std::string& name, jobject icon) {
    jobject b = env->NewObject(g_PersonB_cls, m_PB_ctor);
    if (env->ExceptionCheck() || !b) { env->ExceptionClear(); return nullptr; }
    jstring n = env->NewStringUTF(name.c_str());
    jobject r = env->CallObjectMethod(b, m_PB_setName, n);   // returns Builder
    if (r) env->DeleteLocalRef(r);
    env->DeleteLocalRef(n);
    if (icon) {
        r = env->CallObjectMethod(b, m_PB_setIcon, icon);
        if (r) env->DeleteLocalRef(r);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject p = env->CallObjectMethod(b, m_PB_build);
    env->DeleteLocalRef(b);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return p;
}

// ============================================================
// Best-effort conversation shortcut for API 30+.
// On any failure this is a no-op (the notification is still built).
// ============================================================

static void applyShortcut(JNIEnv* env, jobject builder, const std::string& id,
                          const std::string& label, jobject icon) {
    if (!g_ShortcutInfo_cls || !g_ShortcutInfoB_cls) return;
    jobject sb = env->NewObject(g_ShortcutInfoB_cls, m_SI_ctor, g_app_context,
                                env->NewStringUTF(id.c_str()));
    if (env->ExceptionCheck() || !sb) { env->ExceptionClear(); return; }
    jstring jlabel = env->NewStringUTF(label.c_str());
    jobject r = env->CallObjectMethod(sb, m_SI_setShortLabel, jlabel);
    if (r) env->DeleteLocalRef(r);
    env->DeleteLocalRef(jlabel);
    if (icon) {
        r = env->CallObjectMethod(sb, m_SI_setIcon, icon);
        if (r) env->DeleteLocalRef(r);
    }
    r = env->CallObjectMethod(sb, m_SI_setLongLived, JNI_TRUE);
    if (r) env->DeleteLocalRef(r);
    if (g_LocusId_cls) {
        jstring lid = env->NewStringUTF(id.c_str());
        jobject locus = env->NewObject(g_LocusId_cls,
            env->GetMethodID(g_LocusId_cls, "<init>", "(Ljava/lang/String;)V"), lid);
        env->DeleteLocalRef(lid);
        if (!env->ExceptionCheck() && locus) {
            r = env->CallObjectMethod(sb, m_SI_setLocusId, locus);
            if (r) env->DeleteLocalRef(r);
            env->DeleteLocalRef(locus);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    jobject shortcut = env->CallObjectMethod(sb, m_SI_build);
    env->DeleteLocalRef(sb);
    if (env->ExceptionCheck() || !shortcut) { env->ExceptionClear(); return; }

    // publish via framework ShortcutManager.addDynamicShortcuts (best-effort)
    if (m_Context_getSystemService_str) {
        jstring svc = env->NewStringUTF("shortcut");
        jobject sm = env->CallObjectMethod(g_app_context, m_Context_getSystemService_str, svc);
        env->DeleteLocalRef(svc);
        if (!env->ExceptionCheck() && sm) {
            jclass smCls = env->GetObjectClass(sm);
            jmethodID addDyn = env->GetMethodID(smCls, "addDynamicShortcuts", "(Ljava/util/List;)Z");
            if (addDyn) {
                jclass listCls = env->FindClass("java/util/Arrays");
                jmethodID asList = env->GetMethodID(listCls, "asList", "([Ljava/lang/Object;)Ljava/util/List;");
                jclass objCls = env->FindClass("java/lang/Object");
                jobjectArray arr = env->NewObjectArray(1, objCls, shortcut);
                jobject oneList = env->CallStaticObjectMethod(listCls, asList, arr);
                env->CallBooleanMethod(sm, addDyn, oneList);
                env->DeleteLocalRef(oneList);
                env->DeleteLocalRef(arr);
                env->DeleteLocalRef(objCls);
                env->DeleteLocalRef(listCls);
            }
            env->DeleteLocalRef(smCls);
            env->DeleteLocalRef(sm);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    jstring jid = env->NewStringUTF(id.c_str());
    r = env->CallObjectMethod(builder, m_B_setShortcutId, jid);
    if (r) env->DeleteLocalRef(r);
    env->DeleteLocalRef(jid);
    env->DeleteLocalRef(shortcut);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// ============================================================
// Build a new MessagingStyle Notification from a parsed message.
// Returns a new Notification local ref, or nullptr on any failure.
// ============================================================

static jobject buildMessagingNotification(JNIEnv* env, const ParsedMsg& pm, jobject oldNotif) {
    if (!cacheJniIds(env)) return nullptr;
    if (env->PushLocalFrame(96) != 0) { env->ExceptionClear(); return nullptr; }
    jobject result = nullptr;  // popped out at the end

    // --- read fields from the original notification ---
    jobject extras        = env->GetObjectField(oldNotif, f_Notif_extras);
    jlong  when           = env->GetLongField(oldNotif, f_Notif_when);
    jobject smallIcon     = f_Notif_smallIcon ? env->GetObjectField(oldNotif, f_Notif_smallIcon) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject largeIcon     = m_Notif_getLargeIcon ? env->CallObjectMethod(oldNotif, m_Notif_getLargeIcon) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jint   color          = f_Notif_color ? env->GetIntField(oldNotif, f_Notif_color) : 0;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject contentIntent = env->GetObjectField(oldNotif, f_Notif_contentIntent);
    jobject deleteIntent  = env->GetObjectField(oldNotif, f_Notif_deleteIntent);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (when <= 0) when = env->CallStaticLongMethod(g_System_cls, m_System_currentTimeMillis);
    if (env->ExceptionCheck()) env->ExceptionClear();

    // --- resolve conversation / sender names ---
    std::string convName, senderName;
    bool isGroup = false;
    switch (pm.type) {
        case MsgType::Group:
            isGroup = true;
            convName   = pm.groupName.empty() ? pm.name : pm.groupName;
            senderName = pm.name.empty() ? convName : pm.name;
            break;
        case MsgType::Private:
            convName = pm.name; senderName = pm.name; break;
        case MsgType::Binding:
            convName = "关联QQ号";
            senderName = pm.name.empty() ? convName : pm.name; break;
        case MsgType::QZone:
        case MsgType::QZoneSpecial:
            convName = "QQ空间动态"; senderName = "QQ空间"; break;
        default:
            convName = "QQ"; senderName = "QQ";
    }
    if (convName.empty()) convName = "QQ";

    // --- update history ---
    updateHistory(pm, senderName, when);
    const std::vector<MsgEntry>& hist = g_history[historyKey(pm)];

    // --- build MessagingStyle ---
    jobject style = nullptr;
    if (g_api >= 28) {
        // user Person (group uses the group avatar = largeIcon)
        jobject userPerson = buildPerson(env, convName, isGroup ? largeIcon : nullptr);
        style = env->NewObject(g_MsgStyle_cls, m_MS_ctor_person, userPerson);
        if (env->ExceptionCheck() || !style) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return nullptr; }
        jstring t = env->NewStringUTF(convName.c_str());
        jobject r = env->CallObjectMethod(style, m_MS_setConvTitle, t);
        if (r) env->DeleteLocalRef(r);
        env->DeleteLocalRef(t);
        r = env->CallObjectMethod(style, m_MS_setGroupConv, isGroup ? JNI_TRUE : JNI_FALSE);
        if (r) env->DeleteLocalRef(r);
        if (env->ExceptionCheck()) env->ExceptionClear();
        for (const auto& m : hist) {
            // sender Person: private reuses the friend avatar (largeIcon)
            jobject mp = buildPerson(env, m.sender,
                                     (!isGroup && pm.type == MsgType::Private) ? largeIcon : nullptr);
            jstring txt = env->NewStringUTF(m.text.c_str());
            r = env->CallObjectMethod(style, m_MS_addMessage3, txt, (jlong)m.when, mp);
            if (r) env->DeleteLocalRef(r);
            env->DeleteLocalRef(txt);
            if (mp) env->DeleteLocalRef(mp);
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        }
    } else {
        // deprecated API 26/27 path
        jstring user = env->NewStringUTF(convName.c_str());
        style = env->NewObject(g_MsgStyle_cls, m_MS_ctor_cs, user);
        env->DeleteLocalRef(user);
        if (env->ExceptionCheck() || !style) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return nullptr; }
        for (const auto& m : hist) {
            jstring txt = env->NewStringUTF(m.text.c_str());
            jstring snd = env->NewStringUTF(m.sender.c_str());
            jobject msg = env->NewObject(g_MsgStyle_Msg_cls, m_MS_Msg_ctor_cs, txt, (jlong)m.when, snd);
            env->CallVoidMethod(style, m_MS_addMessage_msg, msg);
            env->DeleteLocalRef(msg);
            env->DeleteLocalRef(txt);
            env->DeleteLocalRef(snd);
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        }
    }

    // --- build the notification ---
    jstring chanIdStr = env->NewStringUTF(channelId(pm));
    jobject builder = env->NewObject(g_Builder_cls, m_B_ctor, g_app_context, chanIdStr);
    if (env->ExceptionCheck() || !builder) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return nullptr; }

    jobject r;
    if (smallIcon) { r = env->CallObjectMethod(builder, m_B_setSmallIcon, smallIcon); if (r) env->DeleteLocalRef(r); }
    if (largeIcon) { r = env->CallObjectMethod(builder, m_B_setLargeIcon, largeIcon); if (r) env->DeleteLocalRef(r); }
    r = env->CallObjectMethod(builder, m_B_setStyle, style);          if (r) env->DeleteLocalRef(r);
    r = env->CallObjectMethod(builder, m_B_setColor, color);          if (r) env->DeleteLocalRef(r);
    r = env->CallObjectMethod(builder, m_B_setWhen, when);            if (r) env->DeleteLocalRef(r);
    r = env->CallObjectMethod(builder, m_B_setShowWhen, JNI_TRUE);    if (r) env->DeleteLocalRef(r);
    if (contentIntent) { r = env->CallObjectMethod(builder, m_B_setContentIntent, contentIntent); if (r) env->DeleteLocalRef(r); }
    if (deleteIntent)  { r = env->CallObjectMethod(builder, m_B_setDeleteIntent,  deleteIntent);  if (r) env->DeleteLocalRef(r); }
    r = env->CallObjectMethod(builder, m_B_setAutoCancel, JNI_TRUE);  if (r) env->DeleteLocalRef(r);
    jstring cat = env->NewStringUTF("msg");  // Notification.CATEGORY_MESSAGE
    r = env->CallObjectMethod(builder, m_B_setCategory, cat);        if (r) env->DeleteLocalRef(r);
    env->DeleteLocalRef(cat);
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (g_api >= 30) {
        std::string sid = std::string("qqevo_") + historyKey(pm);
        applyShortcut(env, builder, sid, convName, largeIcon);
    }

    result = env->CallObjectMethod(builder, m_B_build);
    if (env->ExceptionCheck() || !result) { env->ExceptionClear(); env->PopLocalFrame(nullptr); return nullptr; }

    LOGI("built MessagingStyle type=%d conv=%s", (int)pm.type, convName.c_str());
    return env->PopLocalFrame(result);
}

// ============================================================
// Parse + build wrapper. Returns a new Notification local ref
// (caller owns it) or nullptr to indicate "post the original".
// ============================================================

static jobject processAndBuild(JNIEnv* env, jobject notif) {
    if (!notif) return nullptr;
    if (!cacheJniIds(env)) { LOGE("processAndBuild: cacheJniIds failed"); return nullptr; }

    jobject extras = env->GetObjectField(notif, f_Notif_extras);
    std::string title = extrasGetStr(env, extras, "android.title");
    std::string text  = extrasGetStr(env, extras, "android.text");
    jobject tickerCs  = f_Notif_tickerText ? env->GetObjectField(notif, f_Notif_tickerText) : nullptr;
    std::string ticker = csToString(env, tickerCs);
    if (tickerCs) env->DeleteLocalRef(tickerCs);
    if (extras) env->DeleteLocalRef(extras);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    ParsedMsg pm = parseNotification(title, ticker, text);
    if (pm.type == MsgType::None || pm.type == MsgType::Hidden) {
        return nullptr;  // passthrough — not a QQ chat notification
    }
    LOGI("parse: type=%d name=%s group=%s special=%d num=%d",
         (int)pm.type, pm.name.c_str(), pm.groupName.c_str(), (int)pm.special, pm.num);

    jobject built = buildMessagingNotification(env, pm, notif);
    if (env->ExceptionCheck()) { env->ExceptionClear(); if (built) env->DeleteLocalRef(built); return nullptr; }
    return built;
}

// ============================================================
// callNotifyAsUserDirect: deliver a Notification via the UNHOOKED
// NotificationManager.notifyAsUser(String,int,Notification,UserHandle).
//
// This replaces the unreliable "restore original notify and re-invoke"
// path. notifyAsUser is the method notify() calls internally; its
// ArtMethod is pristine, so JNI dispatch reaches the original bytecode
// and ultimately NotificationManagerService. We never touch the hooked
// notify() ArtMethod again after install.
// ============================================================

static bool resolveDeliveryIds(JNIEnv* env) {
    if (g_notifyAsUser_mid && g_userhandle_all) return true;

    if (!g_notifyAsUser_mid) {
        jclass nmCls = env->FindClass("android/app/NotificationManager");
        if (!nmCls || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("resolveDeliveryIds: NotificationManager missing");
            return false;
        }
        g_notifyAsUser_mid = env->GetMethodID(nmCls, "notifyAsUser",
            "(Ljava/lang/String;ILandroid/app/Notification;Landroid/os/UserHandle;)V");
        env->DeleteLocalRef(nmCls);
        if (env->ExceptionCheck() || !g_notifyAsUser_mid) {
            env->ExceptionClear();
            g_notifyAsUser_mid = nullptr;
            LOGE("resolveDeliveryIds: notifyAsUser not found");
            return false;
        }
    }

    if (!g_userhandle_all) {
        jclass uhCls = env->FindClass("android/os/UserHandle");
        if (!uhCls || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("resolveDeliveryIds: UserHandle missing");
            return false;
        }
        jfieldID allFid = env->GetStaticFieldID(uhCls, "ALL", "Landroid/os/UserHandle;");
        if (env->ExceptionCheck() || !allFid) {
            env->ExceptionClear();
            env->DeleteLocalRef(uhCls);
            LOGE("resolveDeliveryIds: UserHandle.ALL missing");
            return false;
        }
        jobject all = env->GetStaticObjectField(uhCls, allFid);
        env->DeleteLocalRef(uhCls);
        if (env->ExceptionCheck() || !all) {
            env->ExceptionClear();
            LOGE("resolveDeliveryIds: UserHandle.ALL null");
            return false;
        }
        g_userhandle_all = env->NewGlobalRef(all);
        env->DeleteLocalRef(all);
    }
    return true;
}

static void callNotifyAsUserDirect(JNIEnv* env, jobject thiz,
                                   jstring tag, jint id, jobject notif) {
    if (!resolveDeliveryIds(env)) {
        LOGE("callNotifyAsUserDirect: ids unresolved — notification LOST");
        return;
    }
    env->CallVoidMethod(thiz, g_notifyAsUser_mid, tag, id, notif, g_userhandle_all);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("callNotifyAsUserDirect: notifyAsUser threw — notification lost");
    }
}

// ============================================================
// jni_notify_tag: the native callback installed on
// NotificationManager.notify(String,int,Notification).
//
// We rebuild the Notification as MessagingStyle when the parser
// recognises a QQ chat notification, then deliver via the unhooked
// notifyAsUser. If parsing/building fails, we deliver the original
// notification the same way — a notification is never dropped.
// ============================================================

static void jni_notify_tag(JNIEnv* env, jobject thiz, jstring tag, jint id, jobject notif) {
    // Reentrancy guard: notifyAsUser (the delivery path below) does not
    // itself call notify(), so re-entry shouldn't happen. Guard anyway
    // against any unexpected side-effect; on re-entry, just deliver the
    // original directly without rebuilding.
    if (g_in_hook) {
        callNotifyAsUserDirect(env, thiz, tag, id, notif);
        return;
    }
    g_in_hook = true;
    std::lock_guard<std::mutex> lk(g_hook_mtx);

    jobject toPost = notif;   // default: pass-through
    jobject built  = nullptr;

    if (ensureAppContext(env) && cacheJniIds(env)) {
        built = processAndBuild(env, notif);
        if (built) toPost = built;
    } else {
        LOGE("jni_notify_tag: app context / jni ids not ready — passthrough");
    }

    callNotifyAsUserDirect(env, thiz, tag, id, toPost);

    if (built) env->DeleteLocalRef(built);
    g_in_hook = false;
}

// ============================================================
// installNotifyHook: native-ize NotificationManager.notify(
// String,int,Notification). NotificationManager is a boot framework
// class, available immediately at process start, so we can install
// synchronously in postAppSpecialize — no polling thread.
// ============================================================

static void installNotifyHook(JNIEnv* env) {
    jclass nmCls = env->FindClass("android/app/NotificationManager");
    if (!nmCls || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("installNotifyHook: NotificationManager class missing");
        return;
    }
    jmethodID mid = env->GetMethodID(nmCls, "notify",
        "(Ljava/lang/String;ILandroid/app/Notification;)V");
    if (env->ExceptionCheck() || !mid) {
        env->ExceptionClear();
        LOGE("installNotifyHook: notify(String,int,Notification) missing");
        env->DeleteLocalRef(nmCls);
        return;
    }
    jobject method = env->ToReflectedMethod(nmCls, mid, JNI_FALSE);
    env->DeleteLocalRef(nmCls);
    if (env->ExceptionCheck() || !method) {
        env->ExceptionClear();
        LOGE("installNotifyHook: ToReflectedMethod failed");
        return;
    }

    bool ok = installHook(env, method,
        reinterpret_cast<void*>(jni_notify_tag), g_saved_notify);
    env->DeleteLocalRef(method);
    LOGI("notify(String,int,Notification) hook installed: %d", ok);
}

// ============================================================
// Zygisk module entry point
// ============================================================

// Match QQ/TIM main process or a child process (:MSF, :pushservice, ...).
// Returns true iff `name` equals "com.tencent.mobileqq"/"com.tencent.tim"
// or starts with that prefix followed by ':'.
static bool isQqTimProcess(const char* name) {
    if (!name) return false;
    static const char* kPkgs[] = {"com.tencent.mobileqq", "com.tencent.tim"};
    for (const char* pkg : kPkgs) {
        size_t n = strlen(pkg);
        if (strncmp(name, pkg, n) == 0 && (name[n] == '\0' || name[n] == ':')) {
            return true;
        }
    }
    return false;
}


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
        // Match QQ/TIM main process AND child processes (:MSF, :pushservice, etc).
        // MiPush delivers notifications in the :pushservice child process, so a
        // strict == comparison would miss it and the hook would never fire.
        is_target = isQqTimProcess(name);
        env->ReleaseStringUTFChars(args->nice_name, name);

        if (!is_target) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!is_target) return;

        JNIEnv* env;
        g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);

        const char* pname = env->GetStringUTFChars(args->nice_name, nullptr);
        LOGI("loaded into process: %s", pname ? pname : "(null)");
        env->ReleaseStringUTFChars(args->nice_name, pname);

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

        // Cache Application context (for channel creation + builder ctor).
        // If Application isn't ready yet here, ensureAppContext() will
        // resolve it lazily on the first hook fire.
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

        if (g_app_context) createChannelsOnce(env);

        // Install the notify hook synchronously. NotificationManager is a
        // boot framework class — always available, no polling needed.
        installNotifyHook(env);
    }

private:
    zygisk::Api* api = nullptr;
    bool is_target = false;
};

REGISTER_ZYGISK_MODULE(QQNotifyEvoModule)
