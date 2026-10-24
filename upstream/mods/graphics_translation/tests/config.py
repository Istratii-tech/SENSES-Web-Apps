# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from src.build import build_common
from src.build import ninja_generator
from src.build import open_source
from src.build import staging


def _add_compile_flags(n):
  n.add_compiler_flags('-Werror')
  n.emit_gl_common_flags()
  n.add_include_paths('mods',
                      'mods/graphics_translation',
                      'android/system/core/include',
                      'android/hardware/libhardware/include',
                      'android/frameworks/native/include',
                      'android/frameworks/native/opengl/include')


def _generate_unit_test_ninja():
  n = ninja_generator.TestNinjaGenerator('graphics_translation_unit_test',
                                         force_compiler='clang',
                                         enable_cxx11=True,
                                         base_path='graphics_translation')
  _add_compile_flags(n)
  n.add_ppapi_link_flags()
  n.add_library_deps('libcutils_static.a', 'libetc1_static.a',
                     'libgccdemangle_static.a', 'liblog_static.a',
                     'libutils_static.a', 'libppapi_mocks.a', 'libegl.a',
                     'libgles.a')
  sources = n.find_all_files('graphics_translation/gles', ['_test.cpp'],
                             include_tests=True)
  n.build_default(sources, base_path='mods')
  n.run(n.link())


def _generate_image_generator_ninja():
  n = ninja_generator.ExecNinjaGenerator(
      build_common.get_graphics_translation_image_generator_name(), host=True,
      base_path='mods/graphics_translation/tests')
  _add_compile_flags(n)
  n.add_defines('HAVE_PTHREADS')
  n.add_include_paths(
      staging.as_staging('android/external/chromium_org/testing/gtest/include'),
      staging.as_staging('android/external/chromium_org/testing/gtest/'))
  sources = build_common.find_all_files(
      ['mods/graphics_translation/tests'],
      suffixes='.cpp',
      include_tests=True,
      use_staging=False)
  sources.extend(
      ['mods/graphics_translation/gles/debug.cpp',
       'mods/graphics_translation/gles/texture_codecs.cpp',
       'src/common/vector.cc',
       'src/common/matrix.cc',
       staging.as_staging(
           'android/external/chromium_org/testing/gtest/src/gtest-all.cc')])
  sources = [x for x in sources if x.find('apk') < 0]
  n.build_default(sources, base_path=None)
  variables = {'my_static_libs': '-lX11 -lGL'}
  n.link(variables=variables)


def _generate_integration_test_ninja():
  n = ninja_generator.ApkFromSdkNinjaGenerator(
      build_common.get_graphics_translation_test_name(),
      base_path='graphics_translation/tests/apk',
      use_ndk=True, use_gtest=True)
  sources = build_common.find_all_files(
      base_paths=['graphics_translation/tests',
                  'graphics_translation/tests/util'],
      suffixes=['.cpp', '.h'],
      include_tests=True,
      recursive=False)

  # Additional source code dependencies to outside of
  # graphics_translation/tests.
  implicit = [
      'android_libcommon/common/alog.h',
      'graphics_translation/gles/debug.cpp',
      'graphics_translation/gles/debug.h',
      'graphics_translation/gles/texture_codecs.cpp',
      'graphics_translation/gles/texture_codecs.h',
      'src/common/math_test_helpers.h',
      'src/common/matrix.cc',
      'src/common/matrix.h',
      'src/common/vector.cc',
      'src/common/vector.h',
  ]

  n.build_default_all_sources(
      implicit=[staging.as_staging(path) for path in (sources + implicit)])
  n.build_google_test_list(sources)


def generate_test_ninjas():
  if open_source.is_open_source_repo():
    return
  _generate_unit_test_ninja()
  _generate_image_generator_ninja()
  _generate_integration_test_ninja()
