/* macOS build only (-I runtime/macos, after runtime/posix, whose GL/gl.h includes this as the system's): the legacy OpenGL
 * framework's header, for the GL types and the GL 1.1 names nvn_gl.c is written with. That back end is Windows only (WGL);
 * macOS renders with Vulkan on MoltenVK. Apple's gl.h (with its glext.h) declares GL up to 2.1 and many extensions: the
 * functions of it that nvn_gl.c defines itself as pointers it loads (its GLF lists) get other names here. A function
 * added to those lists which Apple declares fails to compile ("redefinition as different kind of symbol", "not
 * assignable"): add it below. */
#ifndef WT_MACOS_GL_H
#define WT_MACOS_GL_H
#define GL_SILENCE_DEPRECATION 1
#define glAttachShader sys_glAttachShader
#define glBeginQuery sys_glBeginQuery
#define glBindBuffer sys_glBindBuffer
#define glBindFramebuffer sys_glBindFramebuffer
#define glBindProgramARB sys_glBindProgramARB
#define glBlendColor sys_glBlendColor
#define glBlitFramebuffer sys_glBlitFramebuffer
#define glBufferData sys_glBufferData
#define glBufferSubData sys_glBufferSubData
#define glCheckFramebufferStatus sys_glCheckFramebufferStatus
#define glClientWaitSync sys_glClientWaitSync
#define glCompileShader sys_glCompileShader
#define glCompressedTexSubImage2D sys_glCompressedTexSubImage2D
#define glCompressedTexSubImage3D sys_glCompressedTexSubImage3D
#define glCreateProgram sys_glCreateProgram
#define glCreateShader sys_glCreateShader
#define glDeleteBuffers sys_glDeleteBuffers
#define glDeleteFramebuffers sys_glDeleteFramebuffers
#define glDeleteProgram sys_glDeleteProgram
#define glDeleteProgramsARB sys_glDeleteProgramsARB
#define glDeleteShader sys_glDeleteShader
#define glDeleteSync sys_glDeleteSync
#define glDepthBoundsEXT sys_glDepthBoundsEXT
#define glDetachShader sys_glDetachShader
#define glDisableVertexAttribArray sys_glDisableVertexAttribArray
#define glDrawBuffers sys_glDrawBuffers
#define glEnableVertexAttribArray sys_glEnableVertexAttribArray
#define glEndQuery sys_glEndQuery
#define glFenceSync sys_glFenceSync
#define glFramebufferTextureLayer sys_glFramebufferTextureLayer
#define glGenBuffers sys_glGenBuffers
#define glGenFramebuffers sys_glGenFramebuffers
#define glGenProgramsARB sys_glGenProgramsARB
#define glGenQueries sys_glGenQueries
#define glGetProgramInfoLog sys_glGetProgramInfoLog
#define glGetProgramiv sys_glGetProgramiv
#define glGetQueryObjectuiv sys_glGetQueryObjectuiv
#define glGetShaderiv sys_glGetShaderiv
#define glLinkProgram sys_glLinkProgram
#define glProgramStringARB sys_glProgramStringARB
#define glShaderSource sys_glShaderSource
#define glStencilFuncSeparate sys_glStencilFuncSeparate
#define glStencilMaskSeparate sys_glStencilMaskSeparate
#define glStencilOpSeparate sys_glStencilOpSeparate
#define glTexSubImage3D sys_glTexSubImage3D
#define glUseProgram sys_glUseProgram
#define glWaitSync sys_glWaitSync
#include <OpenGL/gl.h>
#undef glAttachShader
#undef glBeginQuery
#undef glBindBuffer
#undef glBindFramebuffer
#undef glBindProgramARB
#undef glBlendColor
#undef glBlitFramebuffer
#undef glBufferData
#undef glBufferSubData
#undef glCheckFramebufferStatus
#undef glClientWaitSync
#undef glCompileShader
#undef glCompressedTexSubImage2D
#undef glCompressedTexSubImage3D
#undef glCreateProgram
#undef glCreateShader
#undef glDeleteBuffers
#undef glDeleteFramebuffers
#undef glDeleteProgram
#undef glDeleteProgramsARB
#undef glDeleteShader
#undef glDeleteSync
#undef glDepthBoundsEXT
#undef glDetachShader
#undef glDisableVertexAttribArray
#undef glDrawBuffers
#undef glEnableVertexAttribArray
#undef glEndQuery
#undef glFenceSync
#undef glFramebufferTextureLayer
#undef glGenBuffers
#undef glGenFramebuffers
#undef glGenProgramsARB
#undef glGenQueries
#undef glGetProgramInfoLog
#undef glGetProgramiv
#undef glGetQueryObjectuiv
#undef glGetShaderiv
#undef glLinkProgram
#undef glProgramStringARB
#undef glShaderSource
#undef glStencilFuncSeparate
#undef glStencilMaskSeparate
#undef glStencilOpSeparate
#undef glTexSubImage3D
#undef glUseProgram
#undef glWaitSync
#endif
