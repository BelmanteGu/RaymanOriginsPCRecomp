/* The Adreno's clock on Android: the game keeps the kernel's GPU driver (KGSL) from leaving the GPU in a low state.
 *
 * Why: measured on a Redmi 10C (Adreno 610) with Turnip's own GPU timings: the same title-screen frame took 25.7 ms of
 * GPU time in one run and 85 ms in another (the "10 fps until you pull the status bar down" state): the work did not
 * change, the GPU's clock did. Neither libadrenotools' turbo nor power constraints on the Vulkan driver's contexts
 * changed it (those contexts are not made with KGSL_CONTEXT_PWR_CONSTRAINT).
 * What it does: a thread opens /dev/kgsl-3d0 (any app may, no root), makes a context of its own with the power
 * constraint flag, asks for the highest power level on it, and submits a tiny command buffer every 30 ms with the
 * constraint flag (each such submission renews the constraint). Thermal limits still apply.
 * The fallback of the KGSL hook below (on by default where it finds nothing to hook); "gpuboost=1" in port_settings or
 * USF_GPU_BOOST=1 asks for it anyway, 0 turns it off, 2 makes the same submissions without the constraint (a test:
 * on the Redmi 10C they alone end the slow state).
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
#include <elf.h>
#include <link.h>

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

/* the counters' copy into the IB's memory (USF_GPU_CLOCK); returns the dwords written. Slot 0: RBBM always-count (GPU
 * cycles), 1: CP busy, 2: the always-on counter, then USF_GPU_COUNTERS="<group>:<countable>,..." (KGSL group ids:
 * UCHE 8, TP 9, SP 10, RB 11, VBIF/GBIF 13, CCU 24, LRZ 25; countables: Mesa's a6xx_perfcntrs.xml) */
#define MAXC 32
static unsigned nc, cgrp[MAXC], ccnt[MAXC];
static unsigned clock_ib(int fd, uint32_t *ib, unsigned long gpuaddr) {
    unsigned reg[MAXC] = {0}, n = 0;
    cgrp[0] = 0x1; ccnt[0] = 0; cgrp[1] = 0x0; ccnt[1] = 2; nc = 3;
    const char *l = getenv("USF_GPU_COUNTERS");
    while (l && *l && nc < MAXC) {
        unsigned g, c; int used = 0;
        if (sscanf(l, "%u:%u%n", &g, &c, &used) != 2) break;
        cgrp[nc] = g; ccnt[nc] = c; nc++;
        l += used;
        if (*l == ',') l++;
    }
    for (unsigned i = 0; i < nc; i++) {
        if (i == 2) { reg[i] = A6XX_CP_ALWAYS_ON_COUNTER; continue; }
        struct kgsl_pc_get g = {cgrp[i], ccnt[i], 0, 0, 0};
        if (ioctl(fd, IOCTL_KGSL_PERFCOUNTER_GET, &g)) fprintf(stderr, "usf: gpu clock: counter %u:%u refused (%d)\n", cgrp[i], ccnt[i], errno);
        else reg[i] = g.offset;
    }
    for (unsigned i = 0; i < nc; i++) {
        uint64_t dst = gpuaddr + 2048 + 8 * i;
        if (!reg[i]) continue;
        ib[n++] = pkt7(CP_REG_TO_MEM, 3);
        ib[n++] = reg[i] | 2u << 18 | 1u << 30;          /* 2 registers, 64-bit */
        ib[n++] = (uint32_t)dst;
        ib[n++] = (uint32_t)(dst >> 32);
    }
    fprintf(stderr, "usf: gpu clock: %u counters\n", nc);
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
    uint64_t last[MAXC] = {0};
    for (unsigned k = 0;; k++) {
        struct kgsl_cmd_obj o = {0, a.gpuaddr, 4 * n, KGSL_CMDLIST_IB, a.id};
        struct kgsl_gpu_cmd g = {constrain ? KGSL_CMDBATCH_PWR_CONSTRAINT : 0, (uintptr_t)&o, sizeof o, 1, 0, 0, 0, 0, 0, 0, cc.id, 0};
        if (ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &g) && fails++ < 3) fprintf(stderr, "usf: GPU boost: submit failed (%d)\n", errno);
        usleep(30000);
        if (n > 1 && k % 66 == 65) {                     /* every ~2 s */
            uint64_t v[MAXC];
            for (unsigned i = 0; i < nc; i++) v[i] = out[i];
            double t = (double)(v[2] - last[2]) / 19.2e6, cyc = (double)(v[0] - last[0]);
            if (last[2] && t > 0 && cyc > 0) {
                fprintf(stderr, "usf: gpu clock: %.0f MHz average over %.1f s, CP busy %.0f%% of those cycles\n",
                        cyc / t / 1e6, t, 100.0 * (double)(v[1] - last[1]) / cyc);
                for (unsigned i = 3; i < nc; i++)
                    fprintf(stderr, "usf: gpu counter %u:%u: %.1f%% of GPU cycles, %.2f M a second\n", cgrp[i], ccnt[i],
                            100.0 * (double)(v[i] - last[i]) / cyc, (double)(v[i] - last[i]) / t / 1e6);
            }
            for (unsigned i = 0; i < nc; i++) last[i] = v[i];
        }
    }
    return NULL;
}

/* The driver's own contexts with the power constraint: the
 * driver's ioctl calls (its GOT entry for "ioctl", in the driver's libraries) go through hook_ioctl, which adds
 * KGSL_CONTEXT_PWR_CONSTRAINT to each context it makes and asks for the highest power level on it, and flags each of
 * its submissions so (the constraint is renewed by each). No submissions of our own. USF_KGSL_HOOK=0 turns it off. */
static int (*real_ioctl)(int, unsigned long, ...);
static int hooked_ctx;
static int hook_ioctl(int fd, unsigned long req, void *arg) {
    unsigned nr = (unsigned)(req & 0xFFFF);
    if (arg && nr == 0x0913) ((struct kgsl_ctx_create *)arg)->flags |= KGSL_CONTEXT_PWR_CONSTRAINT;   /* DRAWCTXT_CREATE */
    if (arg && nr == 0x094A) ((struct kgsl_gpu_cmd *)arg)->flags |= KGSL_CMDBATCH_PWR_CONSTRAINT;     /* GPU_COMMAND */
    int r = real_ioctl(fd, req, arg);
    if (r == 0 && arg && nr == 0x0913) {
        struct kgsl_constraint_pwrlevel lvl = {KGSL_CONSTRAINT_PWR_MAX};
        struct kgsl_constraint c = {KGSL_CONSTRAINT_PWRLEVEL, ((struct kgsl_ctx_create *)arg)->id, &lvl, sizeof lvl};
        struct kgsl_prop p = {KGSL_PROP_PWR_CONSTRAINT, &c, sizeof c};
        int e = real_ioctl(fd, IOCTL_KGSL_SETPROPERTY, &p);
        if (hooked_ctx++ < 4) fprintf(stderr, "usf: kgsl hook: driver context %u with the power constraint (%s)\n", c.context_id, e ? "refused" : "max level");
    }
    return r;
}
static int patch_lib(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    int *patched = data;
    const char *n = info->dlpi_name;
    if (!n || !(strstr(n, "vulkan") || strstr(n, "freedreno") || strstr(n, "adreno") || strstr(n, "gsl"))) return 0;
    const ElfW(Dyn) *dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (!dyn) return 0;
    const ElfW(Sym) *sym = NULL; const char *str = NULL; const ElfW(Rela) *rel[2] = {NULL, NULL}; size_t rsz[2] = {0, 0};
    for (; dyn->d_tag != DT_NULL; dyn++) {
        uintptr_t v = dyn->d_un.d_ptr;
        if (v && v < info->dlpi_addr) v += info->dlpi_addr;    /* (some linkers leave them relative) */
        if (dyn->d_tag == DT_SYMTAB) sym = (const ElfW(Sym) *)v;
        else if (dyn->d_tag == DT_STRTAB) str = (const char *)v;
        else if (dyn->d_tag == DT_JMPREL) rel[0] = (const ElfW(Rela) *)v;
        else if (dyn->d_tag == DT_PLTRELSZ) rsz[0] = dyn->d_un.d_val;
        else if (dyn->d_tag == DT_RELA) rel[1] = (const ElfW(Rela) *)v;
        else if (dyn->d_tag == DT_RELASZ) rsz[1] = dyn->d_un.d_val;
    }
    if (!sym || !str) return 0;
    for (int t = 0; t < 2; t++)
        for (size_t k = 0; rel[t] && k < rsz[t] / sizeof(ElfW(Rela)); k++) {
            unsigned type = (unsigned)ELF64_R_TYPE(rel[t][k].r_info), si = (unsigned)ELF64_R_SYM(rel[t][k].r_info);
            if ((type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) || !si) continue;
            if (strcmp(str + sym[si].st_name, "ioctl")) continue;
            void **slot = (void **)(info->dlpi_addr + rel[t][k].r_offset);
            if (*slot == (void *)hook_ioctl) continue;
            uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)4095;
            if (mprotect((void *)pg, 4096, PROT_READ | PROT_WRITE)) continue;
            if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))*slot;
            *slot = (void *)hook_ioctl;
            fprintf(stderr, "usf: kgsl hook: ioctl of %s\n", n);
            (*patched)++;
        }
    return 0;
}
/* after the driver is loaded, before DXVK makes its device: 1 if any driver library was hooked */
int gpu_kgsl_hook(void);
int gpu_kgsl_hook(void) {
    const char *e = getenv("USF_KGSL_HOOK");
    if ((e && *e == '0') || access("/dev/kgsl-3d0", F_OK)) return 0;
    int patched = 0;
    dl_iterate_phdr(patch_lib, &patched);
    if (!patched) fprintf(stderr, "usf: kgsl hook: no driver library calls ioctl through its GOT\n");
    return patched > 0;
}

/* DXVK's instance is made (the system's driver is loaded only then): the hook on the driver's ioctl; the pinger
 * (boost_loop) when the hook found nothing, or when USF_GPU_BOOST / "gpuboost" in port_settings ask for it
 * (0 off, 1 with the constraint, 2 without). */
void gpu_boost_start(void);
void gpu_boost_start(void) {
    static int done;
    if (done++) return;
    int hooked = gpu_kgsl_hook();
    int port_setting(const char *key, int def);
    const char *e = getenv("USF_GPU_BOOST");
    int mode = e ? atoi(e) : port_setting("gpuboost", hooked ? 0 : 1);
    if (mode != 1 && mode != 2) return;
    pthread_t t;
    if (pthread_create(&t, NULL, boost_loop, mode == 1 ? (void *)1 : NULL) == 0) pthread_detach(t);
}
#endif
