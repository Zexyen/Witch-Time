/* [witchtime] Android: the NDK has no desktop OpenGL header. What the runtime's GL back end (nvn_gl.c, never the renderer
 * here: it needs a desktop GL the system lacks) is written against: the GL 1.1 types, the constants it uses, with Mesa's
 * values, and the GL 1.1 functions it calls directly (defined empty in gl_android.c). The newer functions are pointers
 * it looks up itself. Made from the "undeclared identifier" list of a build against the types alone. */
#ifndef WT_GL_ANDROID_H
#define WT_GL_ANDROID_H
#include <stdint.h>
#include <stddef.h>
typedef unsigned int GLenum; typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef signed char GLbyte;
typedef short GLshort; typedef int GLint; typedef int GLsizei; typedef unsigned char GLubyte; typedef unsigned short GLushort;
typedef unsigned int GLuint; typedef float GLfloat; typedef float GLclampf; typedef double GLdouble; typedef double GLclampd;
typedef void GLvoid; typedef char GLchar; typedef ptrdiff_t GLintptr; typedef ptrdiff_t GLsizeiptr; typedef int64_t GLint64;
typedef uint64_t GLuint64; typedef struct __GLsync *GLsync; typedef unsigned short GLhalf;
#define APIENTRY
#define GLAPI extern
#define GL_ALPHA 0x1906
#define GL_ALPHA_TEST 0x0BC0
#define GL_ALPHA_TEST_FUNC 0x0BC1
#define GL_ALPHA_TEST_REF 0x0BC2
#define GL_ALWAYS 0x0207
#define GL_BACK 0x0405
#define GL_BLEND 0x0BE2
#define GL_BLUE 0x1905
#define GL_CCW 0x0901
#define GL_CLAMP 0x2900
#define GL_CLEAR 0x1500
#define GL_COLOR 0x1800
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_CULL_FACE 0x0B44
#define GL_CW 0x0900
#define GL_DECR 0x1E03
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_DEPTH_FUNC 0x0B74
#define GL_DEPTH_TEST 0x0B71
#define GL_FRONT 0x0404
#define GL_FRONT_AND_BACK 0x0408
#define GL_GREEN 0x1904
#define GL_INCR 0x1E02
#define GL_INVERT 0x150A
#define GL_KEEP 0x1E00
#define GL_LESS 0x0201
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_LINE_LOOP 0x0002
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_NEAREST 0x2600
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_NEVER 0x0200
#define GL_NONE 0
#define GL_ONE 1
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_POINT 0x1B00
#define GL_POINTS 0x0000
#define GL_POLYGON 0x0009
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_POLYGON_OFFSET_LINE 0x2A02
#define GL_POLYGON_OFFSET_POINT 0x2A01
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_RED 0x1903
#define GL_RENDERER 0x1F01
#define GL_REPEAT 0x2901
#define GL_REPLACE 0x1E01
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_SCISSOR_BOX 0x0C10
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_STENCIL_TEST 0x0B90
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TRIANGLE_FAN 0x0006
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRUE 1
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_SHORT 0x1403
#define GL_VENDOR 0x1F00
#define GL_VERSION 0x1F02
#define GL_VIEWPORT 0x0BA2
#define GL_ZERO 0

/* the GL 1.1 functions nvn_gl.c and nvn_c4.c call directly, defined empty in gl_android.c (every other GL function nvn_gl.c looks up as a pointer) */
void glAlphaFunc(GLenum func, GLclampf ref);
void glBindTexture(GLenum target, GLuint texture);
void glClear(GLbitfield mask);
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepth(GLclampd depth);
void glClearStencil(GLint s);
void glCullFace(GLenum mode);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDisable(GLenum cap);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glEnable(GLenum cap);
void glFinish(void);
void glFlush(void);
void glFrontFace(GLenum mode);
void glGenTextures(GLsizei n, GLuint *textures);
void glGetBooleanv(GLenum pname, GLboolean *params);
GLenum glGetError(void);
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetIntegerv(GLenum pname, GLint *params);
const GLubyte * glGetString(GLenum name);
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
GLboolean glIsEnabled(GLenum cap);
void glLineWidth(GLfloat width);
void glLogicOp(GLenum opcode);
void glPixelStorei(GLenum pname, GLint param);
void glPointSize(GLfloat size);
void glPolygonMode(GLenum face, GLenum mode);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glStencilFunc(GLenum func, GLint ref, GLuint mask);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
#endif
