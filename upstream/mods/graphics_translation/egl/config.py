# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
import os

from src.build import ninja_generator
from src.build.build_options import OPTIONS


def generate_ninjas():
  base_path = os.path.join('graphics_translation', 'egl')

  n = ninja_generator.ArchiveNinjaGenerator(
      'libegl', force_compiler='clang', enable_cxx11=True, base_path=base_path)
  n.add_compiler_flags('-Werror')
  n.add_notice_sources(['mods/graphics_translation/NOTICE'])
  n.add_include_paths('mods',
                      'android/system/core/include',
                      'android/hardware/libhardware/include',
                      'android/frameworks/native/include',
                      'android/frameworks/native/opengl/include')

  n.emit_gl_common_flags(False)
  n.add_ppapi_compile_flags()
  if OPTIONS.is_egl_api_tracing():
    n.add_defines('ENABLE_API_TRACING')
  if OPTIONS.is_egl_api_logging():
    n.add_defines('ENABLE_API_LOGGING')
  if OPTIONS.is_ansi_fb_logging():
    n.add_defines('ANSI_FB_LOGGING')

  sources = n.find_all_sources()
  n.build_default(sources, base_path='mods')
  n.archive()
