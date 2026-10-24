# Copyright (C) 2009 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Build the graphics translation tests library
LOCAL_PATH := $(ARC_ROOT)/mods/graphics_translation
include $(CLEAR_VARS)

LOCAL_MODULE     := graphics_translation_tests
LOCAL_SRC_FILES  := gles/debug.cpp \
                    gles/texture_codecs.cpp \
                    tests/apk/jni/jni.cpp \
                    tests/util/log.cpp \
                    tests/util/mesh.cpp \
                    tests/util/shader.cpp \
                    tests/util/texture.cpp \
                    tests/graphics_test.cpp \
                    tests/test_capabilities.cpp \
                    tests/test_draw.cpp \
                    tests/test_lights.cpp \
                    tests/test_matrix.cpp \
                    tests/test_misc.cpp \
                    tests/test_shader.cpp \
                    tests/test_texenv.cpp \
                    tests/test_textures.cpp \
                    $(ARC_ROOT)/src/common/matrix.cc \
                    $(ARC_ROOT)/src/common/vector.cc
LOCAL_CFLAGS     := -fexceptions \
                    -DGRAPHICS_TRANSLATION_APK \
                    -DHAVE_PTHREADS
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../ \
                    $(ARC_ROOT)/src \
                    $(ARC_ROOT)/out/staging/android_libcommon \
                    $(ARC_ROOT)/out/staging/android/system/core/include \
                    $(ARC_ROOT)/out/staging/android/external/chromium_org \
                    $(ARC_ROOT)/out/staging/android/frameworks/native/opengl/include \
                    $(ARC_ROOT)/out/staging/android/external/chromium_org/testing/gtest/include
LOCAL_LDLIBS     := -llog -landroid -lGLESv1_CM -lGLESv2 -lEGL
LOCAL_STATIC_LIBRARIES += gtest integration_tests_common
include $(BUILD_SHARED_LIBRARY)

include $(ARC_ROOT)/out/staging/android/external/chromium_org/testing/gtest/Android.mk
include $(ARC_ROOT)/src/integration_tests/common/Android.mk
