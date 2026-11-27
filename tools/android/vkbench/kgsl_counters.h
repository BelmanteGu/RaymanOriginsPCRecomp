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
#define KGSL_CONTEXT_TYPE_GL (1u << 20)
#define KGSL_CMDLIST_IB 1
#define CP_REG_TO_MEM 0x3e
#define A6XX_CP_ALWAYS_ON_COUNTER 0x0980
#define MAXC 32

static unsigned parity(unsigned v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; return (~(0x6996u >> (v & 0xf))) & 1; }
static uint32_t pkt7(unsigned op, unsigned cnt) { return 0x70000000u | cnt | parity(cnt) << 15 | (op & 0x7f) << 16 | parity(op) << 23; }
/* the counters read by a command buffer of this process (other processes' reads stay frozen on some kernels) */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
typedef struct { int fd; unsigned ctx, id, n, nc; unsigned long gpuaddr; uint32_t *ib; unsigned grp[MAXC], cnt[MAXC]; } kc;
static int kc_open(kc *k, const char *list) {
    memset(k, 0, sizeof *k);
    k->fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (k->fd < 0) return 0;
    struct kgsl_ctx_create cc = {KGSL_CONTEXT_NO_GMEM_ALLOC | KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_TYPE_GL, 0};
    if (ioctl(k->fd, IOCTL_KGSL_DRAWCTXT_CREATE, &cc)) return 0;
    k->ctx = cc.id;
    struct kgsl_alloc_id a = {0, 0, 4096, 0, 0, {0, 0}};
    if (ioctl(k->fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &a)) return 0;
    k->id = a.id; k->gpuaddr = a.gpuaddr;
    k->ib = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, (off_t)a.id << 12);
    k->grp[0] = 1; k->cnt[0] = 0; k->nc = 1;                     /* RBBM: GPU cycles */
    while (list && *list && k->nc < MAXC) {
        unsigned g, c; int used = 0;
        if (sscanf(list, "%u:%u%n", &g, &c, &used) != 2) break;
        k->grp[k->nc] = g; k->cnt[k->nc] = c; k->nc++; list += used; if (*list == ',') list++;
    }
    for (unsigned i = 0; i < k->nc; i++) {
        struct kgsl_pc_get g = {k->grp[i], k->cnt[i], 0, 0, 0};
        if (ioctl(k->fd, IOCTL_KGSL_PERFCOUNTER_GET, &g)) { fprintf(stderr, "counter %u:%u refused\n", k->grp[i], k->cnt[i]); continue; }
        uint64_t dst = a.gpuaddr + 2048 + 8 * i;
        k->ib[k->n++] = pkt7(CP_REG_TO_MEM, 3); k->ib[k->n++] = g.offset | 2u << 18 | 1u << 30;
        k->ib[k->n++] = (uint32_t)dst; k->ib[k->n++] = (uint32_t)(dst >> 32);
    }
    return 1;
}
static void kc_read(kc *k, uint64_t *v) {          /* submit the copy, let it land, read */
    struct kgsl_cmd_obj o = {0, k->gpuaddr, 4 * k->n, KGSL_CMDLIST_IB, k->id};
    struct kgsl_gpu_cmd g = {0, (uintptr_t)&o, sizeof o, 1, 0, 0, 0, 0, 0, 0, k->ctx, 0};
    ioctl(k->fd, IOCTL_KGSL_GPU_COMMAND, &g);
    usleep(20000);
    volatile uint64_t *out = (volatile uint64_t *)(k->ib + 512);
    for (unsigned i = 0; i < k->nc; i++) v[i] = out[i];
}
