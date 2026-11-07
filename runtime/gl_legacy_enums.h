/* Android port: the OpenGL names the shared NVN front end (nvn_gl.c) uses that glcorearb.h lacks: compatibility-profile
 * enums (values from the OpenGL 4.6 compatibility specification) and the OpenGL 1.1 entry points nvn_gl.c calls directly.
 * No OpenGL exists off Windows here: gl_stubs_posix.c defines those entry points to stop the program with a message, and
 * nvn_gl.c always selects the Vulkan back end on these hosts, so they are never reached. */
#ifndef GL_LEGACY_ENUMS_H
#define GL_LEGACY_ENUMS_H
#define GL_CLAMP 0x2900
#define GL_QUAD_STRIP 0x0008
#define GL_QUADS 0x0007
#define GL_POLYGON 0x0009
#define GL_ALPHA_TEST 0x0BC0
#define GL_ALPHA_TEST_FUNC 0x0BC1
#define GL_ALPHA_TEST_REF 0x0BC2

#define SMO_GL11(X) \
    X(void, glAlphaFunc, (GLenum func, GLfloat ref)) \
    X(void, glBindTexture, (GLenum target, GLuint texture)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glClearDepth, (GLdouble depth)) \
    X(void, glClearStencil, (GLint s)) \
    X(void, glCullFace, (GLenum mode)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *textures)) \
    X(void, glDepthFunc, (GLenum func)) \
    X(void, glDepthMask, (GLboolean flag)) \
    X(void, glDisable, (GLenum cap)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glEnable, (GLenum cap)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glFrontFace, (GLenum mode)) \
    X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
    X(void, glGetBooleanv, (GLenum pname, GLboolean *data)) \
    X(GLenum, glGetError, (void)) \
    X(void, glGetFloatv, (GLenum pname, GLfloat *data)) \
    X(void, glGetIntegerv, (GLenum pname, GLint *data)) \
    X(const GLubyte *, glGetString, (GLenum name)) \
    X(void, glGetTexImage, (GLenum target, GLint level, GLenum format, GLenum type, void *pixels)) \
    X(void, glGetTexLevelParameteriv, (GLenum target, GLint level, GLenum pname, GLint *params)) \
    X(GLboolean, glIsEnabled, (GLenum cap)) \
    X(void, glLineWidth, (GLfloat width)) \
    X(void, glLogicOp, (GLenum opcode)) \
    X(void, glPixelStorei, (GLenum pname, GLint param)) \
    X(void, glPointSize, (GLfloat size)) \
    X(void, glPolygonMode, (GLenum face, GLenum mode)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, glTexParameteriv, (GLenum target, GLenum pname, const GLint *params)) \
    X(void, glTexSubImage1D, (GLenum target, GLint level, GLint x, GLsizei w, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels))
#define SMO_GL11_DECL(ret, name, args) ret name args;
SMO_GL11(SMO_GL11_DECL)
#undef SMO_GL11_DECL
#endif
