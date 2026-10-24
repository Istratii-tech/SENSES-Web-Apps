# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import pipes

from src.build import build_common
from src.build import ninja_generator
from src.build import staging
from src.build.build_options import OPTIONS


def _get_generated_sources_path():
  root = build_common.get_target_common_dir()
  return os.path.join(root, 'graphics_translation_gen_sources')


def _get_api_entries_impl_path():
  return os.path.join(_get_generated_sources_path(), 'graphics_translation',
                      'gles', 'api_entries_impl.cpp')


def _get_pass_through_h_path():
  return os.path.join(_get_generated_sources_path(), 'graphics_translation',
                      'gles', 'pass_through.h')


def _get_pass_through_impl_path():
  return os.path.join(_get_generated_sources_path(), 'graphics_translation',
                      'gles', 'pass_through.cpp')


def _generate_file(n, rule, target, src):
  base = os.path.join('mods', 'graphics_translation', 'gles')
  script = os.path.join(base, rule + '.py')
  n.rule(rule, command='python %s $in $out' % script,
         description='%s $in $out' % rule)
  n.build(target, rule, src, implicit=script)


def _generate_api_entries_extra_code(n):
  src = os.path.join('mods', 'graphics_translation', 'gles', 'api_entries.cpp')
  _generate_file(n, 'api_entries_gen', [_get_api_entries_impl_path()], src)


def _generate_pass_through_code(n):
  rule_name = 'pass_through_gen'

  api_dir = staging.as_staging('chromium-ppapi/ppapi/api')
  script_path = 'mods/graphics_translation/gles/pass_through_gen.py'

  gen_command = ['src/build/run_python', script_path,
                 '--wnone',
                 '--passthroughgen',
                 '--hpp=graphics_translation/gles/pass_through.h',
                 '--cpp=graphics_translation/gles/pass_through.cpp',
                 '--range=start,end',
                 '--srcroot', pipes.quote(api_dir),
                 '--dstroot', '$dstroot',
                 '>', '$logfile',
                 '|| (cat $logfile; rm $logfile; exit 1)']
  n.rule(rule_name,
         command=' '.join(gen_command),
         description='pass_through_gen')

  idls = n.find_all_files(api_dir, 'idl')
  n.build([_get_pass_through_h_path(), _get_pass_through_impl_path()],
          rule_name, idls, implicit=['src/build/run_python', script_path],
          variables={'logfile': os.path.join(_get_generated_sources_path(),
                                             'pass_through_gen.log'),
                     'dstroot': _get_generated_sources_path()})


def generate_ninjas():
  n = ninja_generator.ArchiveNinjaGenerator(
      'libgles',
      force_compiler='clang',
      enable_cxx11=True,
      base_path='graphics_translation/gles')
  n.add_compiler_flags('-Werror')
  n.add_notice_sources(['mods/graphics_translation/NOTICE'])
  _generate_api_entries_extra_code(n)
  _generate_pass_through_code(n)

  if OPTIONS.is_gles_api_tracing():
    n.add_defines('ENABLE_API_TRACING')
  if OPTIONS.is_gles_api_logging():
    n.add_defines('ENABLE_API_LOGGING')
  if OPTIONS.is_gles_passthrough_logging():
    n.add_defines('ENABLE_PASSTHROUGH_LOGGING')
  if OPTIONS.is_gles_passthrough_tracing():
    n.add_defines('ENABLE_PASSTHROUGH_TRACING')

  n.add_include_paths('mods',
                      'android/system/core/include',
                      'android/frameworks/native/opengl/include',
                      _get_generated_sources_path())

  n.emit_gl_common_flags(False)
  n.add_ppapi_compile_flags()

  all_sources = n.find_all_sources()
  all_sources.append(os.path.join('..', _get_pass_through_impl_path()))
  if (OPTIONS.is_gles_api_tracing() or
      OPTIONS.is_gles_api_logging()):
    all_sources.append(os.path.join('..', _get_api_entries_impl_path()))

  implicit = [_get_pass_through_h_path()]
  n.build_default(all_sources, base_path='mods', order_only=implicit)
  n.archive()
