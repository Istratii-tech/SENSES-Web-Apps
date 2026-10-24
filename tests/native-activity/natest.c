/*
 * libnatest.so: the NDK side of arcwasm.NativeActivityTest
 * (tests/java8/arcwasm/NativeActivityTest.java, docs/topics/native-activity.md).
 * An app without Java code: android.app.NativeActivity loads this library
 * and android_native_app_glue (glue/, AOSP's) runs android_main on a thread
 * of its own with an ALooper, the main thread's commands through a pipe and
 * the input queue attached to the looper -- what most NDK games do.
 *
 * What it does is written, one line per event, to natest.log in the
 * activity's internalDataPath (the test reads it):
 *   main sdk=N            android_main started (ANativeActivity's fields)
 *   mode soft|gl          the intent's "mode" extra (JNI on activity->clazz
 *                         from the app thread: AttachCurrentThread)
 *   config density=N orientation=N   AConfiguration_fromAssetManager
 *   asset TEXT            assets/hello.txt through activity->assetManager
 *   cmd start|resume|input|window WxH|focus 0/1|pause|stop|term|destroy
 *   frame soft WxH        the first frame through ANativeWindow_lock: top
 *                         half #2080e0, bottom half #e06020
 *   frame gl WxH          the same with EGL + GLES 2 (mode gl): #0080ff
 *   egl unavailable STEP  mode gl where the VM has no GL (Node)
 *   motion ACTION X Y N   a motion event (ACTION_DOWN = 0); after a down
 *                         the window is redrawn #30c050 (soft) / #ffff00 (gl):
 *   frame tap
 *   key ACTION CODE       a key event (not handled: Back finishes)
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/native_window.h>
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_native_app_glue.h"

/* (android-18 headers: AWINDOW_FLAG_* is in android/window.h) */
#define FLAG_KEEP_SCREEN_ON 0x00000080
#define FLAG_FULLSCREEN 0x00000400

struct state {
  struct android_app* app;
  FILE* log;
  int gl;
  EGLDisplay display;
  EGLSurface surface;
  EGLContext context;
  int tapped;
};

static void note(struct state* s, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  __android_log_vprint(ANDROID_LOG_INFO, "natest", fmt, ap);
  va_end(ap);
  if (s->log) {
    va_start(ap, fmt);
    vfprintf(s->log, fmt, ap);
    va_end(ap);
    fputc('\n', s->log);
    fflush(s->log);
  }
}

/* getIntent().getStringExtra("mode"), from this (the app's) thread. */
static void read_mode(struct state* s) {
  ANativeActivity* a = s->app->activity;
  JavaVM* vm = a->vm;
  JNIEnv* env = NULL;
  if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
    note(s, "mode ? (no JNIEnv)");
    return;
  }
  jclass cls = (*env)->GetObjectClass(env, a->clazz);
  jmethodID get_intent = (*env)->GetMethodID(env, cls, "getIntent", "()Landroid/content/Intent;");
  jobject intent = (*env)->CallObjectMethod(env, a->clazz, get_intent);
  jclass icls = (*env)->GetObjectClass(env, intent);
  jmethodID get_extra = (*env)->GetMethodID(env, icls, "getStringExtra",
                                            "(Ljava/lang/String;)Ljava/lang/String;");
  jstring key = (*env)->NewStringUTF(env, "mode");
  jstring mode = (jstring)(*env)->CallObjectMethod(env, intent, get_extra, key);
  if (mode != NULL) {
    const char* m = (*env)->GetStringUTFChars(env, mode, NULL);
    s->gl = strcmp(m, "gl") == 0;
    (*env)->ReleaseStringUTFChars(env, mode, m);
  }
  note(s, "mode %s", s->gl ? "gl" : "soft");
  (*vm)->DetachCurrentThread(vm);
}

static void fill(uint8_t* row, int n, uint32_t rgb) {
  for (int x = 0; x < n; x++) {
    row[4 * x] = rgb >> 16;
    row[4 * x + 1] = rgb >> 8;
    row[4 * x + 2] = rgb;
    row[4 * x + 3] = 0xff;
  }
}

static void draw_soft(struct state* s) {
  ANativeWindow* w = s->app->window;
  ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBA_8888);
  ANativeWindow_Buffer b;
  if (ANativeWindow_lock(w, &b, NULL) != 0) {
    note(s, "lock failed");
    return;
  }
  for (int y = 0; y < b.height; y++) {
    uint32_t c = s->tapped ? 0x30c050 : y < b.height / 2 ? 0x2080e0 : 0xe06020;
    fill((uint8_t*)b.bits + 4 * b.stride * y, b.width, c);
  }
  ANativeWindow_unlockAndPost(w);
  if (s->tapped)
    note(s, "frame tap");
  else
    note(s, "frame soft %dx%d", b.width, b.height);
}

static int init_gl(struct state* s) {
  static const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                                   EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                   EGL_BLUE_SIZE, 8, EGL_NONE};
  static const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLConfig config;
  EGLint n = 0;
  s->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(s->display, NULL, NULL)) {
    note(s, "egl unavailable eglInitialize 0x%x", eglGetError());
    return 0;
  }
  if (!eglChooseConfig(s->display, attribs, &config, 1, &n) || n == 0) {
    note(s, "egl unavailable eglChooseConfig 0x%x", eglGetError());
    return 0;
  }
  s->surface = eglCreateWindowSurface(s->display, config, s->app->window, NULL);
  if (s->surface == EGL_NO_SURFACE) {
    note(s, "egl unavailable eglCreateWindowSurface 0x%x", eglGetError());
    return 0;
  }
  s->context = eglCreateContext(s->display, config, EGL_NO_CONTEXT, ctx_attribs);
  if (s->context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(s->display, s->surface, s->surface, s->context)) {
    note(s, "egl unavailable eglMakeCurrent 0x%x", eglGetError());
    return 0;
  }
  return 1;
}

static void draw_gl(struct state* s) {
  if (s->tapped)
    glClearColor(1, 1, 0, 1);
  else
    glClearColor(0, 0.5f, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(s->display, s->surface);
  EGLint w = 0, h = 0;
  eglQuerySurface(s->display, s->surface, EGL_WIDTH, &w);
  eglQuerySurface(s->display, s->surface, EGL_HEIGHT, &h);
  if (s->tapped)
    note(s, "frame tap");
  else
    note(s, "frame gl %dx%d", w, h);
}

static void term_gl(struct state* s) {
  if (s->display == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(s->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s->context != EGL_NO_CONTEXT)
    eglDestroyContext(s->display, s->context);
  if (s->surface != EGL_NO_SURFACE)
    eglDestroySurface(s->display, s->surface);
  eglTerminate(s->display);
  s->display = EGL_NO_DISPLAY;
  s->surface = EGL_NO_SURFACE;
  s->context = EGL_NO_CONTEXT;
}

static void draw(struct state* s) {
  if (s->app->window == NULL)
    return;
  if (!s->gl)
    draw_soft(s);
  else if (s->surface != EGL_NO_SURFACE)
    draw_gl(s);
}

static void on_cmd(struct android_app* app, int32_t cmd) {
  struct state* s = (struct state*)app->userData;
  switch (cmd) {
    case APP_CMD_START: note(s, "cmd start"); break;
    case APP_CMD_RESUME: note(s, "cmd resume"); break;
    case APP_CMD_PAUSE: note(s, "cmd pause"); break;
    case APP_CMD_STOP: note(s, "cmd stop"); break;
    case APP_CMD_DESTROY: note(s, "cmd destroy"); break;
    case APP_CMD_GAINED_FOCUS: note(s, "cmd focus 1"); break;
    case APP_CMD_LOST_FOCUS: note(s, "cmd focus 0"); break;
    case APP_CMD_CONFIG_CHANGED: note(s, "cmd config"); break;
    case APP_CMD_INPUT_CHANGED:
      note(s, "cmd input %s", app->inputQueue ? "set" : "none");
      break;
    case APP_CMD_SAVE_STATE:
      app->savedState = strdup("natest-state");
      app->savedStateSize = strlen("natest-state") + 1;
      note(s, "cmd save");
      break;
    case APP_CMD_INIT_WINDOW:
      note(s, "cmd window %dx%d", ANativeWindow_getWidth(app->window),
           ANativeWindow_getHeight(app->window));
      if (s->gl && !init_gl(s))
        s->gl = 2;  /* no GL: nothing drawn */
      draw(s);
      break;
    case APP_CMD_TERM_WINDOW:
      note(s, "cmd term");
      term_gl(s);
      break;
  }
}

static int32_t on_input(struct android_app* app, AInputEvent* e) {
  struct state* s = (struct state*)app->userData;
  if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
    int32_t action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    note(s, "motion %d %.0f %.0f %d", action, AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0),
         (int)AMotionEvent_getPointerCount(e));
    if (action == AMOTION_EVENT_ACTION_DOWN && !s->tapped) {
      s->tapped = 1;
      draw(s);
    }
    return 1;
  }
  if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
    note(s, "key %d %d", AKeyEvent_getAction(e), AKeyEvent_getKeyCode(e));
    return 0;  /* Back: the activity's default (finish) */
  }
  return 0;
}

void android_main(struct android_app* app) {
  struct state s;
  memset(&s, 0, sizeof s);
  s.app = app;
  s.display = EGL_NO_DISPLAY;
  s.surface = EGL_NO_SURFACE;
  s.context = EGL_NO_CONTEXT;
  app_dummy();
  app->userData = &s;
  app->onAppCmd = on_cmd;
  app->onInputEvent = on_input;

  ANativeActivity* a = app->activity;
  char path[512];
  snprintf(path, sizeof path, "%s/natest.log", a->internalDataPath);
  s.log = fopen(path, "a");
  note(&s, "main sdk=%d %s", a->sdkVersion, app->savedState ? "restored" : "fresh");
  read_mode(&s);
  note(&s, "config density=%d orientation=%d sdk=%d", AConfiguration_getDensity(app->config),
       AConfiguration_getOrientation(app->config), AConfiguration_getSdkVersion(app->config));
  AAsset* asset = AAssetManager_open(a->assetManager, "hello.txt", AASSET_MODE_BUFFER);
  if (asset) {
    char text[64] = {0};
    AAsset_read(asset, text, sizeof text - 1);
    AAsset_close(asset);
    text[strcspn(text, "\n")] = 0;
    note(&s, "asset %s", text);
  } else {
    note(&s, "asset none");
  }
  ANativeActivity_setWindowFlags(a, FLAG_KEEP_SCREEN_ON | FLAG_FULLSCREEN, 0);

  for (;;) {
    int events;
    struct android_poll_source* source;
    while (ALooper_pollAll(-1, NULL, &events, (void**)&source) >= 0) {
      if (source != NULL)
        source->process(app, source);
      if (app->destroyRequested) {
        term_gl(&s);
        note(&s, "exit");
        if (s.log)
          fclose(s.log);
        return;
      }
    }
  }
}
