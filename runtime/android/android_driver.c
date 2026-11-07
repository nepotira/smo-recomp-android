/* android_driver.c - an optional custom Vulkan driver (Mesa Turnip on Adreno), loaded through libadrenotools.
 *
 * The renderer asks android_vulkan_loader() for its Vulkan loader before it falls back to the system's libvulkan.so
 * (nvn_vk.c). A custom driver is used when the player has put an AdrenoTools driver package, unzipped, in <files>/driver
 * (meta.json names the library, for example K11MCH1's Turnip: vulkan.ad07xx.so; without meta.json the first *.so) (the app's
 * external files folder, where push_data.py puts the game's files): it is copied into the app's internal storage
 * (a driver may not be loaded from shared storage) and handed to adrenotools_open_libvulkan, which opens a private
 * copy of libvulkan.so whose driver lookup is redirected to it. The hook libraries adrenotools needs are in the APK's
 * native library folder (build_apk.py). SMO_DRIVER=system keeps the system's driver. A driver is code like the game's:
 * only a build the player trusts belongs there (BUILDING.md: built from Mesa's own source).
 * Without libadrenotools at build time (SMO_ADRENOTOOLS not defined) this file answers NULL: the system driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "plat.h"
#include "gfx_options.h"

#ifdef SMO_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif

const char *android_internal_path(void);              /* android_main.c: ANativeActivity::internalDataPath */

/* the driver package's library name: meta.json "libraryName", else the first *.so of the folder */
static int driver_name(const char *dir, char *out, size_t n) {
    char p[PLAT_MAX_PATH];
    snprintf(p, sizeof p, "%s/meta.json", dir);
    FILE *f = fopen(p, "rb");
    if (f) {
        char buf[4096];
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[got] = 0;
        char *k = strstr(buf, "\"libraryName\"");
        if (k && (k = strchr(k + 13, ':')) && (k = strchr(k, '"'))) {
            char *e = strchr(k + 1, '"');
            if (e && e - k - 1 > 0 && (size_t)(e - k - 1) < n && !memchr(k + 1, '/', (size_t)(e - k - 1))) { memcpy(out, k + 1, (size_t)(e - k - 1)); out[e - k - 1] = 0; return 1; }
        }
    }
    plat_dir *d = plat_dir_open(dir);
    const char *name;
    int isdir;
    int64_t sz;
    while (d && plat_dir_next(d, &name, &isdir, &sz))
        if (!isdir && strlen(name) > 3 && !strcmp(name + strlen(name) - 3, ".so")) { snprintf(out, n, "%s", name); plat_dir_close(d); return 1; }
    if (d) plat_dir_close(d);
    return 0;
}

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY), out = -1, ok = 0;
    if (in < 0) return 0;
    char tmp[PLAT_MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.part", to);
    out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (out >= 0) {
        char buf[1 << 16];
        ssize_t n;
        ok = 1;
        while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
        if (n < 0) ok = 0;
        close(out);
        if (ok) ok = rename(tmp, to) == 0; else unlink(tmp);
    }
    close(in);
    return ok;
}

void *android_vulkan_loader(void) {
    const char *e = getenv("SMO_DRIVER");
    if (e && !strcmp(e, "system")) { printf("nvn_vk: SMO_DRIVER=system: the system's Vulkan driver\n"); return NULL; }
    if (!e && gfx_options()->driver_system) { printf("nvn_vk: graphics.ini driver = system: the system's Vulkan driver\n"); return NULL; }
    char src[PLAT_MAX_PATH], dir[PLAT_MAX_PATH], dst[PLAT_MAX_PATH], pkg[PLAT_MAX_PATH], lib[128];
    snprintf(pkg, sizeof pkg, "%s/driver", plat_data_root());
    if (!driver_name(pkg, lib, sizeof lib)) return NULL;               /* no custom driver: the system's */
    snprintf(src, sizeof src, "%s/%s", pkg, lib);
    struct stat ss, ds;
    if (stat(src, &ss) != 0) return NULL;
#ifndef SMO_ADRENOTOOLS
    printf("nvn_vk: %s is there, but this build has no libadrenotools: the system's Vulkan driver\n", src);
    return NULL;
#else
    const char *internal = android_internal_path();
    if (!internal) return NULL;
    snprintf(dir, sizeof dir, "%s/driver", internal);
    plat_mkdirs(dir);
    snprintf(dst, sizeof dst, "%s/%s", dir, lib);
    if (stat(dst, &ds) != 0 || ds.st_size != ss.st_size || ds.st_mtime < ss.st_mtime) {
        if (!copy_file(src, dst)) { printf("nvn_vk: the custom driver could not be copied to %s: the system's Vulkan driver\n", dst); return NULL; }
    }
    Dl_info di;                                                         /* the hooks sit next to libsmo.so (the APK's native library folder) */
    char libdir[PLAT_MAX_PATH] = "";
    if (dladdr((void *)android_vulkan_loader, &di) && di.dli_fname) {
        snprintf(libdir, sizeof libdir, "%s", di.dli_fname);
        char *sl = strrchr(libdir, '/');
        if (sl) sl[1] = 0;
    }
    char tmpdir[PLAT_MAX_PATH];
    snprintf(tmpdir, sizeof tmpdir, "%s/driver_tmp/", internal);
    plat_mkdirs(tmpdir);
    strncat(dir, "/", sizeof dir - strlen(dir) - 1);
    void *h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmpdir, libdir, dir, lib, NULL, NULL);
    printf("nvn_vk: custom Vulkan driver %s (%lld bytes) through adrenotools, hooks in %s: %s\n", dst, (long long)ss.st_size, libdir, h ? "loaded" : "FAILED, the system's driver");
    return h;
#endif
}
