/* getenv for the runtime, remembered (hle.h maps getenv / setenv here).
 *
 * Why: the runtime reads its analysis switches (USF_HIDE_TEX, USF_GEO_DUMP, ...) in its hot paths, some on every
 * Direct3D call; glibc's / bionic's getenv walks the whole environment comparing strings each time. Measured on the
 * title screen: 14% of the game thread in getenv and most of another 20% in libc's string compares (2026-11-22,
 * USF_PROF); a phone's environment is longer, and its cores slower.
 * What it does: one lookup per name; the answer is kept (name and value) in a small table until the runtime itself
 * calls setenv (usf_setenv: everything is looked up again, e.g. RAYMAN_FPS from the touch settings).
 * What it does not do: notice an environment change made by another library (none of the names it is asked about
 * are set by one after start-up).
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define N 256
static struct { char name[48]; const char *value; unsigned gen; } tab[N];
static unsigned gen = 1;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static unsigned hash(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }

char *usf_getenv(const char *name) {
    size_t len = strlen(name);
    if (len >= sizeof tab[0].name) return getenv(name);
    unsigned h = hash(name) & (N - 1);
    pthread_mutex_lock(&m);
    for (unsigned k = 0; k < N; k++, h = (h + 1) & (N - 1)) {
        if (tab[h].gen == gen && !strcmp(tab[h].name, name)) { const char *v = tab[h].value; pthread_mutex_unlock(&m); return (char *)v; }
        if (tab[h].gen != gen) {                           /* free (or stale): look it up and keep it here */
            const char *v = getenv(name);
            memcpy(tab[h].name, name, len + 1);
            tab[h].value = v;
            tab[h].gen = gen;
            pthread_mutex_unlock(&m);
            return (char *)v;
        }
    }
    pthread_mutex_unlock(&m);
    return getenv(name);
}

int usf_setenv(const char *name, const char *value, int overwrite) {
    pthread_mutex_lock(&m);
    int r = setenv(name, value, overwrite);
    gen++;                                                 /* every remembered answer is looked up again */
    pthread_mutex_unlock(&m);
    return r;
}
