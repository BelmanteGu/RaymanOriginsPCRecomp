/* The Adreno's clock on Android: the game keeps the kernel's GPU driver (KGSL) from leaving the GPU in a low state.
 *
 * Why: measured on a Redmi 10C (Adreno 610) with Turnip's own GPU timings: the same title-screen frame took 25.7 ms of
 * GPU time in one run and 85 ms in another (the "10 fps until you pull the status bar down" state): the work did not
 * change, the GPU's clock did. Neither libadrenotools' turbo nor power constraints on the Vulkan driver's contexts
 * changed it (those contexts are not made with KGSL_CONTEXT_PWR_CONSTRAINT).
 * What it does: a thread opens /dev/kgsl-3d0 (any app may, no root), makes a context of its own with the power
 * constraint flag, asks for the highest power level on it, and submits a tiny command buffer every 30 ms with the
 * constraint flag (each such submission renews the constraint). Thermal limits still apply.
 * On by default where there is an Adreno; "gpuboost=0" in port_settings or USF_GPU_BOOST=0 turns it off, 2 makes the
 * same submissions without the constraint (a test: on the Redmi 10C they alone end the slow state).
 * Measured on the Redmi 10C, title screen: 10.8 fps (3 runs of 3) without it, 35.8 with it; the GPU then at 1115 MHz.
 * USF_GPU_CLOCK=1 (analysis): the same command buffer copies the GPU's cycle counters to memory (CP_REG_TO_MEM:
 * RBBM always-count, CP busy cycles, the 19.2 MHz always-on counter); every 2 s: "gpu clock: <MHz> ...".
 * What it does not do: anything on the PC or on a GPU that is not an Adreno (no /dev/kgsl-3d0).
 */
#ifdef __ANDROID__
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* the kernel's KGSL interface (msm_kgsl.h, UAPI) */
struct kgsl_prop { unsigned int type; void *value; size_t sizebytes; };
struct kgsl_constraint { unsigned int type; unsigned int context_id; void *data; size_t size; };
struct kgsl_constraint_pwrlevel { unsigned int level; };
struct kgsl_ctx_create { unsigned int flags; unsigned int id; };
struct kgsl_alloc_id { unsigned int id; unsigned int flags; size_t size; size_t mmapsize; unsigned long gpuaddr; unsigned long pad[2]; };
struct kgsl_cmd_obj { unsigned long long offset, gpuaddr, size; unsigned int flags, id; };
struct kgsl_gpu_cmd { unsigned long long flags, cmdlist; unsigned int cmdsize, numcmds; unsigned long long objlist;
                      unsigned int objsize, numobjs; unsigned long long synclist; unsigned int syncsize, numsyncs, context_id, timestamp; };
struct kgsl_pc_get { unsigned int groupid, countable, offset, offset_hi, pad; };
#define KGSL_IOC_TYPE 0x09
#define IOCTL_KGSL_DRAWCTXT_CREATE _IOWR(KGSL_IOC_TYPE, 0x13, struct kgsl_ctx_create)
#define IOCTL_KGSL_SETPROPERTY _IOW(KGSL_IOC_TYPE, 0x32, struct kgsl_prop)
#define IOCTL_KGSL_GPUMEM_ALLOC_ID _IOWR(KGSL_IOC_TYPE, 0x34, struct kgsl_alloc_id)
#define IOCTL_KGSL_PERFCOUNTER_GET _IOWR(KGSL_IOC_TYPE, 0x38, struct kgsl_pc_get)
#define IOCTL_KGSL_GPU_COMMAND _IOWR(KGSL_IOC_TYPE, 0x4A, struct kgsl_gpu_cmd)
#define KGSL_PROP_PWR_CONSTRAINT 0x12
#define KGSL_CONSTRAINT_PWRLEVEL 1
#define KGSL_CONSTRAINT_PWR_MAX 1
#define KGSL_CONTEXT_NO_GMEM_ALLOC 0x2
#define KGSL_CONTEXT_PREAMBLE 0x10
#define KGSL_CONTEXT_PWR_CONSTRAINT 0x800
#define KGSL_CMDBATCH_PWR_CONSTRAINT KGSL_CONTEXT_PWR_CONSTRAINT
#define KGSL_CONTEXT_TYPE_GL (1u << 20)
#define KGSL_CMDLIST_IB 1
#define CP_NOP_PKT7 0x70108000u                /* type-7 CP_NOP, no payload (with its parity bits) */
#define CP_REG_TO_MEM 0x3e
#define A6XX_CP_ALWAYS_ON_COUNTER 0x0980

static unsigned parity(unsigned v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; return (~(0x6996u >> (v & 0xf))) & 1; }
static uint32_t pkt7(unsigned op, unsigned cnt) { return 0x70000000u | cnt | parity(cnt) << 15 | (op & 0x7f) << 16 | parity(op) << 23; }

/* the counters' copy into the IB's memory (USF_GPU_CLOCK); returns the dwords written */
static unsigned clock_ib(int fd, uint32_t *ib, unsigned long gpuaddr) {
    unsigned grp[2] = {0x1, 0x0}, cnt[2] = {0, 2}, reg[3] = {0, 0, A6XX_CP_ALWAYS_ON_COUNTER}, n = 0;
    for (int i = 0; i < 2; i++) {
        struct kgsl_pc_get g = {grp[i], cnt[i], 0, 0, 0};
        if (ioctl(fd, IOCTL_KGSL_PERFCOUNTER_GET, &g)) fprintf(stderr, "usf: gpu clock: counter refused (%d)\n", errno);
        else reg[i] = g.offset;
    }
    for (int i = 0; i < 3; i++) {
        uint64_t dst = gpuaddr + 2048 + 8 * i;
        ib[n++] = pkt7(CP_REG_TO_MEM, 3);
        ib[n++] = reg[i] | 2u << 18 | 1u << 30;          /* 2 registers, 64-bit */
        ib[n++] = (uint32_t)dst;
        ib[n++] = (uint32_t)(dst >> 32);
    }
    fprintf(stderr, "usf: gpu clock: counters at %#x %#x\n", reg[0], reg[1]);
    return n;
}

static void *boost_loop(void *arg) {
    int constrain = arg != NULL;
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "usf: GPU boost: no /dev/kgsl-3d0\n"); return NULL; }
    struct kgsl_ctx_create cc = {(constrain ? KGSL_CONTEXT_PWR_CONSTRAINT : 0) | KGSL_CONTEXT_NO_GMEM_ALLOC |
                                 KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_TYPE_GL, 0};
    if (ioctl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &cc)) { fprintf(stderr, "usf: GPU boost: context refused\n"); close(fd); return NULL; }
    struct kgsl_constraint_pwrlevel lvl = {KGSL_CONSTRAINT_PWR_MAX};
    struct kgsl_constraint c = {KGSL_CONSTRAINT_PWRLEVEL, cc.id, &lvl, sizeof lvl};
    struct kgsl_prop p = {KGSL_PROP_PWR_CONSTRAINT, &c, sizeof c};
    int set = constrain ? ioctl(fd, IOCTL_KGSL_SETPROPERTY, &p) : -1;
    struct kgsl_alloc_id a = {0, 0, 4096, 0, 0, {0, 0}};
    if (ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &a)) { fprintf(stderr, "usf: GPU boost: no GPU memory\n"); close(fd); return NULL; }
    uint32_t *ib = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)a.id << 12);
    if (ib == MAP_FAILED) { fprintf(stderr, "usf: GPU boost: no mapping\n"); close(fd); return NULL; }
    unsigned n = getenv("USF_GPU_CLOCK") ? clock_ib(fd, ib, a.gpuaddr) : 0;
    if (!n) ib[n++] = CP_NOP_PKT7;
    volatile uint64_t *out = (volatile uint64_t *)(ib + 512);
    fprintf(stderr, "usf: GPU boost: context %u (constraint %s), %u dwords every 30 ms\n", cc.id, set ? "off" : "set", n);
    int fails = 0;
    uint64_t last[3] = {0, 0, 0};
    for (unsigned k = 0;; k++) {
        struct kgsl_cmd_obj o = {0, a.gpuaddr, 4 * n, KGSL_CMDLIST_IB, a.id};
        struct kgsl_gpu_cmd g = {constrain ? KGSL_CMDBATCH_PWR_CONSTRAINT : 0, (uintptr_t)&o, sizeof o, 1, 0, 0, 0, 0, 0, 0, cc.id, 0};
        if (ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &g) && fails++ < 3) fprintf(stderr, "usf: GPU boost: submit failed (%d)\n", errno);
        usleep(30000);
        if (n > 1 && k % 66 == 65) {                     /* every ~2 s */
            uint64_t v[3] = {out[0], out[1], out[2]};
            double t = (double)(v[2] - last[2]) / 19.2e6;
            if (last[2] && t > 0)
                fprintf(stderr, "usf: gpu clock: %.0f MHz average over %.1f s, CP busy %.0f%% of those cycles\n",
                        (double)(v[0] - last[0]) / t / 1e6, t, v[0] > last[0] ? 100.0 * (double)(v[1] - last[1]) / (double)(v[0] - last[0]) : 0);
            for (int i = 0; i < 3; i++) last[i] = v[i];
        }
    }
    return NULL;
}

void gpu_boost_start(void);
void gpu_boost_start(void) {
    int port_setting(const char *key, int def);
    const char *e = getenv("USF_GPU_BOOST");
    int mode = e ? atoi(e) : port_setting("gpuboost", 1);
    if (mode != 1 && mode != 2) return;
    pthread_t t;
    if (pthread_create(&t, NULL, boost_loop, mode == 1 ? (void *)1 : NULL) == 0) pthread_detach(t);
}
#endif
