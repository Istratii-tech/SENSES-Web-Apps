// Force-included into libgui's GLConsumer.cpp (topic fw8-graphics): its GL
// and EGL calls go to shim/fw8-graphics/glconsumer_gl.cc, which sends them
// to hwui's own GL/EGL (WebGL2 of the RenderThread, shim/fw6-hwui) when
// hwui's context is current on the calling thread -- TextureView's layer,
// latched by DeferredLayerUpdater -- and to the program's GL/EGL otherwise
// (an app's EGL14 context: graphics_translation in the page). One
// GLConsumer serves both. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_GRAPHICS_GLCONSUMER_GL_H_
#define ARC_WASM_FW8_GRAPHICS_GLCONSUMER_GL_H_

#define eglClientWaitSyncKHR arc_glc_eglClientWaitSyncKHR
#define eglCreateImageKHR arc_glc_eglCreateImageKHR
#define eglCreateSyncKHR arc_glc_eglCreateSyncKHR
#define eglDestroyImageKHR arc_glc_eglDestroyImageKHR
#define eglDestroySyncKHR arc_glc_eglDestroySyncKHR
#define eglDupNativeFenceFDANDROID arc_glc_eglDupNativeFenceFDANDROID
#define eglGetCurrentContext arc_glc_eglGetCurrentContext
#define eglGetCurrentDisplay arc_glc_eglGetCurrentDisplay
#define eglGetDisplay arc_glc_eglGetDisplay
#define eglGetError arc_glc_eglGetError
#define eglInitialize arc_glc_eglInitialize
#define eglQueryString arc_glc_eglQueryString
#define eglQueryStringImplementationANDROID arc_glc_eglQueryStringImplementationANDROID
#define eglTerminate arc_glc_eglTerminate
#define eglWaitSyncKHR arc_glc_eglWaitSyncKHR
#define glBindTexture arc_glc_glBindTexture
#define glDeleteTextures arc_glc_glDeleteTextures
#define glEGLImageTargetTexture2DOES arc_glc_glEGLImageTargetTexture2DOES
#define glFlush arc_glc_glFlush
#define glGetError arc_glc_glGetError
#define glPixelStorei arc_glc_glPixelStorei
#define glTexImage2D arc_glc_glTexImage2D

#endif  // ARC_WASM_FW8_GRAPHICS_GLCONSUMER_GL_H_
