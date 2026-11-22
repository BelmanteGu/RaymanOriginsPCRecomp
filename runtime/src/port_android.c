/* The game process and the app's Java side (GameActivity.java), both ways.
 *
 * What it does: port_open_menu() asks GameActivity to show the launcher's Options screen over the game (the gear
 * button, touch.c); port_haptic() a short vibration (a touch button pressed, when "Vibration" is on). From Java:
 * nativeSetFrozen holds the game at its next frame while that screen is open (port_frozen, read by game_pace in
 * d3d9_device.c), nativeReloadSettings re-reads port_settings.txt after it was saved there, nativeEditLayout starts
 * moving the on-screen buttons (touch.c).
 * On the PC there is no Java: port_open_menu() returns 0 and touch.c shows its own small panel instead.
 */
#include <stdint.h>

volatile int port_frozen;
void touch_reload(void);
void touch_edit_layout(void);

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

static int call_activity(const char *method) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return 0;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, cls, method, "()V");
    int ok = 0;
    if (m) { (*env)->CallVoidMethod(env, act, m); ok = !(*env)->ExceptionCheck(env); }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return ok;
}
int port_open_menu(void) { return call_activity("showPortMenu"); }
void port_haptic(void) { call_activity("haptic"); }

JNIEXPORT void JNICALL Java_io_github_belmantegu_raymanpc_GameActivity_nativeSetFrozen(JNIEnv *e, jclass c, jboolean on) {
    (void)e; (void)c;
    port_frozen = on ? 1 : 0;
}
JNIEXPORT void JNICALL Java_io_github_belmantegu_raymanpc_GameActivity_nativeReloadSettings(JNIEnv *e, jclass c) {
    (void)e; (void)c;
    touch_reload();
}
JNIEXPORT void JNICALL Java_io_github_belmantegu_raymanpc_GameActivity_nativeEditLayout(JNIEnv *e, jclass c) {
    (void)e; (void)c;
    touch_edit_layout();
}
#else
int port_open_menu(void) { return 0; }
void port_haptic(void) {}
#endif
