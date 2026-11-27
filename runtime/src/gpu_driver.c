/* The Vulkan driver on Android: the system's, or one of the player's (Turnip), and BC textures on older Adreno drivers.
 *
 * What it does, once, before DXVK is loaded (dxvk_bridge.c):
 *   - a driver of the player's: the launcher (GpuDrivers.java) unpacks a driver zip (meta.json + the library) into
 *     files/gpu_drivers/<folder>/ and writes files/gpu_drivers/selected (the folder, then the library's file name).
 *     libadrenotools opens it in a private copy of libvulkan.so (hooks beside libmain.so, from the app's library
 *     folder). A driver that never draws must not leave the game unable to start: a trial file is written before it
 *     is opened and removed once frames have been drawn (gpu_driver_good); found at the next start, the choice moves to
 *     files/gpu_drivers/failed (the launcher says so) and the system's driver is used. (The scheme of the MHGU port.)
 *   - BCeNabler (libadrenotools): Qualcomm's drivers 512.x before 514 hide the BC (DXT) formats the Adreno 6xx hardware
 *     has; patched, DXVK makes the game's DXT textures as they are, not decoded to RGBA8 on the CPU (dxvk_bcemu.c): a
 *     quarter to an eighth of the memory and of the texture bandwidth, the scarce thing on an Adreno 610. USF_NO_BCN=1
 *     leaves the driver alone.
 *   - DXVK gets the chosen vkGetInstanceProcAddr as DXVK_VK_GIPA (tools/android/dxvk_mobile_gipa.patch).
 * What it does not do: anything on the PC; drivers for other GPUs than Adreno (libadrenotools hooks Qualcomm's stack).
 */
#ifdef __ANDROID__
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "adrenotools/bcenabler.h"
#include "adrenotools/driver.h"

static int custom;
static char name[128], dir_part[600];

static void chomp(char *s) { s[strcspn(s, "\r\n")] = 0; }
static void paths(char *root, size_t n, const char *leaf, char *out, size_t m) {
    const char *home = SDL_GetAndroidInternalStoragePath();
    snprintf(root, n, "%s/gpu_drivers", home ? home : ".");
    if (leaf) snprintf(out, m, "%s/%s", root, leaf);
}

static PFN_vkGetInstanceProcAddr open_driver(void) {
    char root[400], selected[512], trial[512], failed[512];
    paths(root, sizeof root, "selected", selected, sizeof selected);
    snprintf(trial, sizeof trial, "%s/trial", root);
    snprintf(failed, sizeof failed, "%s/failed", root);
    void *lib = NULL;
    FILE *f = getenv("USF_GPU_DRIVER") && !strcmp(getenv("USF_GPU_DRIVER"), "system") ? NULL : fopen(selected, "r");   /* (tests) */
    if (f) {
        char folder[128] = "", file[128] = "";
        if (!fgets(folder, sizeof folder, f)) folder[0] = 0;
        if (!fgets(file, sizeof file, f)) file[0] = 0;
        fclose(f);
        chomp(folder); chomp(file);
        if (access(trial, F_OK) == 0) {                         /* the last run with it drew nothing */
            fprintf(stderr, "usf: gpu driver %s failed in the last run: the system's instead\n", folder);
            rename(selected, failed);
            unlink(trial);
        } else if (folder[0] && file[0]) {
            char dir[560], hooks[512] = "";
            snprintf(dir, sizeof dir, "%s/%s/", root, folder);
            Dl_info me;                                         /* the hooks are beside this library */
            if (dladdr((void *)open_driver, &me) && me.dli_fname) {
                snprintf(hooks, sizeof hooks, "%s", me.dli_fname);
                char *sl = strrchr(hooks, '/');
                if (sl) sl[1] = 0;
            }
            FILE *t = fopen(trial, "w");
            if (t) fclose(t);
            lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, dir, file, NULL, NULL);
            if (lib) {
                custom = 1;
                snprintf(name, sizeof name, "%s", folder);
                snprintf(dir_part, sizeof dir_part, "/gpu_drivers/%s/", folder);
                fprintf(stderr, "usf: gpu driver of the player's: %s (%s)\n", folder, file);
            } else {
                fprintf(stderr, "usf: gpu driver %s could not be opened (%s): the system's instead\n", folder, dlerror());
                rename(selected, failed);
                unlink(trial);
            }
        }
    }
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
}

/* BCeNabler on the system's Qualcomm driver, through a throwaway instance */
static void enable_bcn(PFN_vkGetInstanceProcAddr gipa) {
    if (getenv("USF_NO_BCN")) return;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst = VK_NULL_HANDLE;
    if (!create || create(&ci, NULL, &inst) != VK_SUCCESS) return;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFormatProperties fmt = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (en && props && fmt && en(inst, &n, &pd) >= 0 && n && pd) {
        VkPhysicalDeviceProperties p;
        props(pd, &p);
        uint32_t major = p.driverVersion >> 22, minor = (p.driverVersion >> 12) & 0x3FF;
        enum adrenotools_bcn_type t = adrenotools_get_bcn_type(major, minor, p.vendorID);
        VkFormatProperties fp;
        fmt(pd, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &fp);
        int had = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        fprintf(stderr, "usf: gpu %s, driver %u.%u: BC %s, BCeNabler %s\n", p.deviceName, major, minor, had ? "yes" : "no",
                t == ADRENOTOOLS_BCN_PATCH ? "can patch" : t == ADRENOTOOLS_BCN_BLOB ? "not needed" : "not for this driver");
        if (!had && t == ADRENOTOOLS_BCN_PATCH) {
            int ok = adrenotools_patch_bcn((void *)fmt);
            fmt(pd, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &fp);
            fprintf(stderr, "usf: BCeNabler %s: BC1 %s\n", ok ? "patched the driver" : "failed",
                    (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "now available" : "still missing");
        }
    }
    if (destroy) destroy(inst, NULL);
}

void gpu_driver_prepare(void);
void gpu_driver_prepare(void) {
    static int done;
    if (done++) return;
    PFN_vkGetInstanceProcAddr gipa = open_driver();
    if (!gipa) return;
    /* USF_GPU_TURBO=1 (test): ask the kernel's GPU driver (KGSL) to keep the Adreno at its highest clock, within its
     * thermal limits (libadrenotools; for any driver, the system's or Turnip) */
    if (getenv("USF_GPU_TURBO")) { adrenotools_set_turbo(true); fprintf(stderr, "usf: GPU turbo on\n"); }
    if (!custom) enable_bcn(gipa);
    char hex[32];
    snprintf(hex, sizeof hex, "%llx", (unsigned long long)(uintptr_t)gipa);
    setenv("DXVK_VK_GIPA", hex, 1);
}

/* the device is made: whether the player's driver is the one running (a library from its folder is loaded) */
static int found_in_dir(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    return info->dlpi_name && strstr(info->dlpi_name, (const char *)data) != NULL;
}
/* frames have been drawn: the trial is over (a driver that loaded something else goes to failed) */
void gpu_driver_good(void);
void gpu_driver_good(void) {
    if (!custom) return;
    char root[400], trial[512], selected[512], failed[512];
    paths(root, sizeof root, "trial", trial, sizeof trial);
    snprintf(selected, sizeof selected, "%s/selected", root);
    snprintf(failed, sizeof failed, "%s/failed", root);
    if (!dl_iterate_phdr(found_in_dir, dir_part)) {
        fprintf(stderr, "usf: gpu driver %s did not load: the system's driver ran instead\n", name);
        rename(selected, failed);
    } else fprintf(stderr, "usf: gpu driver %s works\n", name);
    unlink(trial);
    custom = 0;
}
#endif
