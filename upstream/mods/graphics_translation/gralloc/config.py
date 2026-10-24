# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from src.build import ninja_generator


def _generate_lib_ninja():
  n = ninja_generator.ArchiveNinjaGenerator(
      'libgralloc',
      force_compiler='clang',
      enable_cxx11=True,
      base_path='graphics_translation/gralloc')
  n.add_compiler_flags('-Werror')
  n.add_notice_sources(['mods/graphics_translation/NOTICE'])
  n.emit_gl_common_flags(False)
  n.add_include_paths('mods',
                      'android/system/core/include',
                      'android/hardware/libhardware/include',
                      'android/frameworks/native/opengl/include',
                      'libyuv/include')
  sources = n.find_all_sources()
  sources.remove('graphics_translation/gralloc/gralloc_main.cpp')
  n.build_default(sources, base_path='mods')
  n.archive()


def _generate_so_ninja():
  n = ninja_generator.SharedObjectNinjaGenerator(
      'gralloc.arc',
      install_path='/lib/hw',
      force_compiler='clang',
      enable_cxx11=True,
      base_path='graphics_translation/gralloc')

  # gralloc_main.cpp uses gcc-style struct initialization "member: value"
  # which clang warns by default. We cannot switch to the C++11 style
  # ".member = value" yet as some of our compilers do not support it.
  # TODO(crbug.com/365178): Switch to ".member = value" style and remove
  # -Wno-gnu-designator.
  n.add_compiler_flags('-Werror', '-Wno-gnu-designator')

  n.add_notice_sources(['mods/graphics_translation/NOTICE'])
  n.emit_framework_common_flags()
  n.add_include_paths('mods')
  sources = ['graphics_translation/gralloc/gralloc_main.cpp']
  n.build_default(sources, base_path='mods')
  n.link()


def generate_ninjas():
  _generate_lib_ninja()
  _generate_so_ninja()
