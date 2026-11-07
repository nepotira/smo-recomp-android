/* Android port: the OpenGL 1.1 entry points nvn_gl.c calls directly (gl_legacy_enums.h), for hosts without OpenGL. The
 * Vulkan back end is the only renderer there; reaching one of these is a bug and stops the program with its name. */
#ifndef _WIN32
#include <stdio.h>
#include "plat.h"
#include "third_party/khronos/GL/glcorearb.h"
#include "gl_legacy_enums.h"

static void gl_missing(const char *name) {
    printf("nvn_gl: %s called, but there is no OpenGL renderer on this host\n", name);
    fflush(stdout);
    plat_exit(5);
}
#define SMO_GL11_STUB(ret, name, args) ret name args { gl_missing(#name); return (ret)0; }
#define SMO_GL11_STUBV(ret, name, args) void name args { gl_missing(#name); }
SMO_GL11(SMO_GL11_STUB)
#endif
