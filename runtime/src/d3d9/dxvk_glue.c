/* What the DXVK bridge (dxvk_bridge.c, brought from the Sonic & All-Stars Racing Transformed PC port) expects from
 * the rest of the runtime, for this port.
 *
 * What it does: g_repo_dir (the repository, where the PC build of DXVK is: <repo>/private/deps/dxvk), worked out from
 * the executable's path (<repo>/private/build/rayman); usf_ms (milliseconds since start, for USF_SHOT_MS); the
 * per-frame and memory-statistics hooks that port's profiler has (nothing here).
 */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

char g_repo_dir[1024];
uint64_t usf_mem_bytes[3], usf_mem_calls[3];

__attribute__((constructor)) static void find_repo(void) {
    char p[1024];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) return;
    p[n] = 0;
    for (int up = 0; up < 3; up++) {                     /* rayman -> build -> private -> <repo> */
        char *s = strrchr(p, '/');
        if (!s) return;
        *s = 0;
    }
    snprintf(g_repo_dir, sizeof g_repo_dir, "%s", p);
}

uint32_t usf_ms(void);
uint32_t usf_ms(void) {
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec) t0 = t;
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

void usf_perf_frame(void);
void usf_perf_frame(void) {}
