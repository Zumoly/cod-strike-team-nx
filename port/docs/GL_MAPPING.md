# GLES/EGL import map

Generated from libunity.so: 104 imports. Status is tracked by which `shim_*` symbols exist.

| Import | Subsystem | deko3D mapping |
|---|---|---|
| `eglChooseConfig` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglCreateContext` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglCreateWindowSurface` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglDestroyContext` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglDestroySurface` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglGetConfigAttrib` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglGetDisplay` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglGetError` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglGetProcAddress` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglInitialize` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglMakeCurrent` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `eglSwapBuffers` | present | dkQueueAcquireImage / dkQueuePresentImage on a DkSwapchain |
| `eglTerminate` | context | fixed single config over nwindowGetDefault(); DkDevice/DkQueue owned by the renderer |
| `glActiveTexture` | texture | DkSampler / DkImageDescriptor tables, dkCmdBufBindTextures |
| `glAttachShader` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glBindAttribLocation` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glBindBuffer` | buffer | versioned ring in CPU-visible GPU memory, orphaned per update |
| `glBindFramebuffer` | target | dkCmdBufBindRenderTargets with DkImageView |
| `glBindRenderbuffer` | target | dkCmdBufBindRenderTargets with DkImageView |
| `glBindTexture` | texture | DkSampler / DkImageDescriptor tables, dkCmdBufBindTextures |
| `glBlendEquationSeparate` | state | DkBlendState, DkColorWriteState |
| `glBlendFuncSeparate` | state | DkBlendState, DkColorWriteState |
| `glBufferData` | buffer | versioned ring in CPU-visible GPU memory, orphaned per update |
| `glBufferSubData` | buffer | versioned ring in CPU-visible GPU memory, orphaned per update |
| `glCheckFramebufferStatus` | target | dkCmdBufBindRenderTargets with DkImageView |
| `glClear` | target | dkCmdBufClearColor / dkCmdBufClearDepthStencil |
| `glClearColor` | target | dkCmdBufClearColor / dkCmdBufClearDepthStencil |
| `glClearDepthf` | target | dkCmdBufClearColor / dkCmdBufClearDepthStencil |
| `glClearStencil` | target | dkCmdBufClearColor / dkCmdBufClearDepthStencil |
| `glColorMask` | state | DkBlendState, DkColorWriteState |
| `glCompileShader` | shader | lookup of precompiled DKSH by source hash (tools/translate_shaders.py) |
| `glCompressedTexImage2D` | texture | dkCmdBufCopyBufferToImage from staging; PVRTC decoded on CPU; mips via dkCmdBufBlitImage |
| `glCompressedTexSubImage2D` | texture | dkCmdBufCopyBufferToImage from staging; PVRTC decoded on CPU; mips via dkCmdBufBlitImage |
| `glCopyTexSubImage2D` | texture | dkCmdBufCopyBufferToImage from staging; PVRTC decoded on CPU; mips via dkCmdBufBlitImage |
| `glCreateProgram` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glCreateShader` | shader | lookup of precompiled DKSH by source hash (tools/translate_shaders.py) |
| `glCullFace` | state | DkRasterizerState |
| `glDeleteBuffers` | buffer | handle table of DkMemBlock suballocations |
| `glDeleteFramebuffers` | target | handle table of attachment sets |
| `glDeleteProgram` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glDeleteRenderbuffers` | target | handle table of attachment sets |
| `glDeleteShader` | shader | lookup of precompiled DKSH by source hash (tools/translate_shaders.py) |
| `glDeleteTextures` | texture | handle table of DkImage + descriptor slot |
| `glDepthFunc` | state | DkDepthStencilState, dkCmdBufSetStencil |
| `glDepthMask` | state | DkDepthStencilState, dkCmdBufSetStencil |
| `glDisable` | state | DkRasterizerState / DkDepthStencilState / DkColorState |
| `glDisableVertexAttribArray` | vertex | DkVtxAttribState + DkVtxBufferState; client pointers copied into the ring at draw |
| `glDrawArrays` | draw | dkCmdBufDraw / dkCmdBufDrawIndexed after flushing dirty state |
| `glDrawElements` | draw | dkCmdBufDraw / dkCmdBufDrawIndexed after flushing dirty state |
| `glEnable` | state | DkRasterizerState / DkDepthStencilState / DkColorState |
| `glEnableVertexAttribArray` | vertex | DkVtxAttribState + DkVtxBufferState; client pointers copied into the ring at draw |
| `glFinish` | sync | dkQueueFlush / dkQueueWaitIdle |
| `glFlush` | sync | dkQueueFlush / dkQueueWaitIdle |
| `glFramebufferRenderbuffer` | target | dkCmdBufBindRenderTargets with DkImageView |
| `glFramebufferTexture2D` | target | dkCmdBufBindRenderTargets with DkImageView |
| `glFrontFace` | state | DkRasterizerState |
| `glGenBuffers` | buffer | handle table of DkMemBlock suballocations |
| `glGenFramebuffers` | target | handle table of attachment sets |
| `glGenRenderbuffers` | target | handle table of attachment sets |
| `glGenTextures` | texture | handle table of DkImage + descriptor slot |
| `glGenerateMipmap` | texture | dkCmdBufCopyBufferToImage from staging; PVRTC decoded on CPU; mips via dkCmdBufBlitImage |
| `glGetActiveAttrib` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glGetActiveUniform` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glGetAttribLocation` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glGetError` | query | static caps table; reports ES 2.0 with a chosen extension list |
| `glGetFramebufferAttachmentParameteriv` | target | dkCmdBufBindRenderTargets with DkImageView |
| `glGetIntegerv` | query | static caps table; reports ES 2.0 with a chosen extension list |
| `glGetProgramInfoLog` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glGetProgramiv` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glGetShaderInfoLog` | shader | lookup of precompiled DKSH by source hash (tools/translate_shaders.py) |
| `glGetShaderSource` | shader | lookup of precompiled DKSH by source hash (tools/translate_shaders.py) |
| `glGetShaderiv` | shader | lookup of precompiled DKSH by source hash (tools/translate_shaders.py) |
| `glGetString` | query | static caps table; reports ES 2.0 with a chosen extension list |
| `glGetUniformLocation` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glGetVertexAttribPointerv` | vertex | DkVtxAttribState + DkVtxBufferState; client pointers copied into the ring at draw |
| `glGetVertexAttribiv` | vertex | DkVtxAttribState + DkVtxBufferState; client pointers copied into the ring at draw |
| `glLinkProgram` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glPixelStorei` | texture | tracked unpack alignment for staging copies |
| `glReadPixels` | target | dkCmdBufCopyImageToBuffer, then fence wait |
| `glRenderbufferStorage` | target | dkCmdBufBindRenderTargets with DkImageView |
| `glScissor` | state | dkCmdBufSetViewports / dkCmdBufSetScissors, Y flipped for FBOs as needed |
| `glShaderSource` | shader | lookup of precompiled DKSH by source hash (tools/translate_shaders.py) |
| `glStencilFunc` | state | DkDepthStencilState, dkCmdBufSetStencil |
| `glStencilFuncSeparate` | state | DkDepthStencilState, dkCmdBufSetStencil |
| `glStencilMask` | state | DkDepthStencilState, dkCmdBufSetStencil |
| `glStencilOp` | state | DkDepthStencilState, dkCmdBufSetStencil |
| `glStencilOpSeparate` | state | DkDepthStencilState, dkCmdBufSetStencil |
| `glTexImage2D` | texture | dkCmdBufCopyBufferToImage from staging; PVRTC decoded on CPU; mips via dkCmdBufBlitImage |
| `glTexParameteri` | texture | DkSampler / DkImageDescriptor tables, dkCmdBufBindTextures |
| `glTexSubImage2D` | texture | dkCmdBufCopyBufferToImage from staging; PVRTC decoded on CPU; mips via dkCmdBufBlitImage |
| `glUniform1fv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform1i` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform1iv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform2fv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform2iv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform3fv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform3iv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform4fv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniform4iv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniformMatrix3fv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUniformMatrix4fv` | uniform | writes std140 shadow block; dkCmdBufPushConstants before draw |
| `glUseProgram` | shader | DkShader pair + reflection from shader_db.json, dkCmdBufBindShaders |
| `glVertexAttribPointer` | vertex | DkVtxAttribState + DkVtxBufferState; client pointers copied into the ring at draw |
| `glViewport` | state | dkCmdBufSetViewports / dkCmdBufSetScissors, Y flipped for FBOs as needed |
