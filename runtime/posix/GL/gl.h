#ifdef __ANDROID__
#include "gl_android.h"          /* no desktop GL header in the NDK */
#else
/* Linux build only: Mesa's gl.h declares these GL 1.2 / 1.3 functions, which nvn_gl.c defines itself as pointers it
 * loads (Windows' gl.h stops at 1.1); the system's declarations get other names */
#define glTexSubImage3D sys_glTexSubImage3D
#define glCompressedTexSubImage2D sys_glCompressedTexSubImage2D
#define glCompressedTexSubImage3D sys_glCompressedTexSubImage3D
#define glBlendColor sys_glBlendColor
#include_next <GL/gl.h>
#undef glTexSubImage3D
#undef glCompressedTexSubImage2D
#undef glCompressedTexSubImage3D
#undef glBlendColor
#ifndef APIENTRY
#define APIENTRY
#endif
#endif
