#include <jni.h>
#include <pthread.h>

#include "airscreen.h"

static JavaVM *vm;
static jclass native_class;
static jmethodID on_event, register_service, unregister_service;
static pthread_key_t detach_key;

static void detach_thread(void *unused) {
    (*vm)->DetachCurrentThread(vm);
}

/* UxPlay calls back from its own pthreads; attach once per thread, detach at thread exit. */
static JNIEnv *env_for_thread(void) {
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
        return NULL;
    }
    pthread_setspecific(detach_key, env);
    return env;
}

void bridge_emit(native_event_t event, const char *text, int a, int b) {
    JNIEnv *env = env_for_thread();
    if (!env) return;
    jstring jtext = text ? (*env)->NewStringUTF(env, text) : NULL;
    (*env)->CallStaticVoidMethod(env, native_class, on_event, (jint) event, jtext, a, b);
    if (jtext) (*env)->DeleteLocalRef(env, jtext);
}

void bridge_register_service(int service, const char *name, unsigned short port,
                             const unsigned char *txt, int txt_len) {
    JNIEnv *env = env_for_thread();
    if (!env) return;
    jstring jname = (*env)->NewStringUTF(env, name);
    jbyteArray jtxt = (*env)->NewByteArray(env, txt_len);
    (*env)->SetByteArrayRegion(env, jtxt, 0, txt_len, (const jbyte *) txt);
    (*env)->CallStaticVoidMethod(env, native_class, register_service, service, jname, (jint) port, jtxt);
    (*env)->DeleteLocalRef(env, jname);
    (*env)->DeleteLocalRef(env, jtxt);
}

void bridge_unregister_service(int service) {
    JNIEnv *env = env_for_thread();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, native_class, unregister_service, service);
}

JNIEXPORT jint JNI_OnLoad(JavaVM *jvm, void *reserved) {
    JNIEnv *env;
    vm = jvm;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = (*env)->FindClass(env, "dev/airscreen/Native");
    if (!cls) return JNI_ERR;
    native_class = (*env)->NewGlobalRef(env, cls);
    on_event = (*env)->GetStaticMethodID(env, cls, "onEvent", "(ILjava/lang/String;II)V");
    register_service = (*env)->GetStaticMethodID(env, cls, "registerService", "(ILjava/lang/String;I[B)V");
    unregister_service = (*env)->GetStaticMethodID(env, cls, "unregisterService", "(I)V");
    if (!on_event || !register_service || !unregister_service) return JNI_ERR;
    pthread_key_create(&detach_key, detach_thread);
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_dev_airscreen_Native_nativeStart(JNIEnv *env, jclass cls, jstring name, jbyteArray hw_addr,
                                      jint width, jint height, jint refresh_hz, jint max_fps,
                                      jboolean h265, jstring keyfile) {
    receiver_config_t cfg = {
        .name = (*env)->GetStringUTFChars(env, name, NULL),
        .width = (uint16_t) width,
        .height = (uint16_t) height,
        .refresh_hz = (uint16_t) refresh_hz,
        .max_fps = (uint16_t) max_fps,
        .h265 = h265,
        .keyfile = (*env)->GetStringUTFChars(env, keyfile, NULL),
    };
    (*env)->GetByteArrayRegion(env, hw_addr, 0, sizeof(cfg.hw_addr), (jbyte *) cfg.hw_addr);
    int result = receiver_start(&cfg);
    (*env)->ReleaseStringUTFChars(env, name, cfg.name);
    (*env)->ReleaseStringUTFChars(env, keyfile, cfg.keyfile);
    return result;
}

JNIEXPORT void JNICALL
Java_dev_airscreen_Native_nativeStop(JNIEnv *env, jclass cls) {
    receiver_stop();
}
