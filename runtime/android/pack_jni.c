/* libpack.so: the launcher's way to lite_make (runtime/src/lite_make.c), the compressed copy of the game's archive.
 * Packer.make(ipk, out, threads) runs it on the caller's thread (the launcher's worker) and returns null or an
 * error text; Packer.progress() is the part done, in thousandths, for the progress bar. */
#include <jni.h>
#include <stdatomic.h>

#include "lite.h"

static atomic_int done_k;
static void progress(uint64_t done, uint64_t total) { atomic_store(&done_k, (int)(total ? done * 1000 / total : 0)); }

JNIEXPORT jstring JNICALL Java_io_github_belmantegu_raymanpc_Packer_make(JNIEnv *env, jclass c, jstring ipk, jstring out, jint threads) {
    (void)c;
    const char *i = (*env)->GetStringUTFChars(env, ipk, NULL), *o = (*env)->GetStringUTFChars(env, out, NULL);
    atomic_store(&done_k, 0);
    const char *e = lite_make(i, o, threads, progress);
    (*env)->ReleaseStringUTFChars(env, ipk, i);
    (*env)->ReleaseStringUTFChars(env, out, o);
    return e ? (*env)->NewStringUTF(env, e) : NULL;
}
JNIEXPORT jint JNICALL Java_io_github_belmantegu_raymanpc_Packer_progress(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    return atomic_load(&done_k);
}
