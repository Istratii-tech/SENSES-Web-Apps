# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os

from src.build import build_common
from src.build import ninja_generator
from src.build import ninja_generator_runner
from src.build import staging
from src.build.build_options import OPTIONS
from src.build.util import python_deps


def _add_compile_flags(ninja):
  if OPTIONS.is_memory_usage_logging():
    ninja.add_defines('MEMORY_USAGE_LOGGING')
  if OPTIONS.log_thread_ids():
    ninja.add_defines('LOG_THREAD_IDS')
  if OPTIONS.log_timestamps():
    ninja.add_defines('LOG_TIMESTAMPS')

  ninja.add_ppapi_compile_flags()  # for mprotect_rwx.cc
  ninja.add_libchromium_base_compile_flags()
  ninja.add_cxx_flags('-isystem',
                      staging.as_staging('android/external/stlport/stlport'))


def _get_generated_file(ninja, out_name, script_name):
  script_path = os.path.join('src/common', script_name)
  rule_name = os.path.splitext(os.path.basename(script_path))[0]
  ninja.rule(rule_name,
             command='%s > $out.tmp && mv $out.tmp $out' % script_path,
             description=rule_name + ' $in')

  out_path = os.path.join(build_common.get_build_dir(), 'common_gen_sources',
                          out_name)
  implicit = python_deps.find_deps(script_path)
  ninja.build(out_path, rule_name, implicit=implicit)
  return out_path


def _get_wrapped_functions_cc(ninja):
  return _get_generated_file(
      ninja,
      out_name='wrapped_functions.cc',
      script_name='gen_wrapped_functions_cc.py')


def _get_real_syscall_aliases_s(ninja):
  return _get_generated_file(
      ninja,
      out_name='real_syscall_aliases.S',
      script_name='gen_real_syscall_aliases_s.py')


def _generate_libpluginhandle_ninja_common(
    module_name, instances, enable_libcxx):
  n = ninja_generator.ArchiveNinjaGenerator(module_name,
                                            instances=instances,
                                            enable_libcxx=enable_libcxx)
  return n.build_default(['src/common/plugin_handle.cc']).archive()


def _generate_libpluginhandle_ninja():
  return _generate_libpluginhandle_ninja_common('libpluginhandle',
                                                instances=1,
                                                enable_libcxx=False)


def _generate_libpluginhandle_libcxx_ninja():
  return _generate_libpluginhandle_ninja_common('libpluginhandle_libc++',
                                                instances=0,
                                                enable_libcxx=True)


def _generate_libcommon_ninja(
    module_name, instances, enable_libcxx, extra_sources):
  n = ninja_generator.ArchiveNinjaGenerator(module_name,
                                            instances=instances,
                                            force_compiler='clang',
                                            enable_cxx11=True,
                                            enable_libcxx=enable_libcxx,
                                            base_path='src/common')
  n.add_compiler_flags('-Werror')
  if build_common.use_ndk_direct_execution():
    n.add_compiler_flags('-DUSE_NDK_DIRECT_EXECUTION')
  # Common code really should not reach out into external.
  n.add_include_paths('android_libcommon')
  _add_compile_flags(n)
  sources = n.find_all_sources()
  sources.remove('src/common/plugin_handle.cc')
  sources.extend(build_common.as_list(extra_sources))
  return n.build_default(sources, base_path=None).archive()


def _generate_libcommon_ninjas():
  n = ninja_generator.NinjaGenerator('libcommon_gen_sources')
  extra_sources = [_get_wrapped_functions_cc(n)]
  _generate_libcommon_ninja(module_name='libcommon',
                            instances=1,
                            enable_libcxx=False,
                            extra_sources=extra_sources)
  _generate_libcommon_ninja(module_name='libcommon_libc++',
                            instances=0,
                            enable_libcxx=True,
                            extra_sources=extra_sources)


def _generate_libcommon_test_main_ninja():
  n = ninja_generator.ArchiveNinjaGenerator(
      'libcommon_test_main',
      base_path='src/common/tests',
      instances=0)  # Should not be used by production binary.
  sources = n.find_all_sources(include_tests=True)
  n.build_default(sources, base_path=None).archive()


def _generate_real_syscall_aliases_ninja():
  n = ninja_generator.ArchiveNinjaGenerator(
      'libcommon_real_syscall_aliases',
      instances=0)  # Should not be used by production binary.
  sources = [_get_real_syscall_aliases_s(n)]
  n.build_default(sources, base_path=None).archive()


# Generate libcommon.a, the library that should be linked into everything.
def generate_ninjas():
  ninja_generator_runner.request_run_in_parallel(
      _generate_libcommon_test_main_ninja,
      _generate_libpluginhandle_libcxx_ninja,
      _generate_libpluginhandle_ninja,
      _generate_libcommon_ninjas,
      _generate_real_syscall_aliases_ninja)


def generate_test_ninjas():
  n = ninja_generator.TestNinjaGenerator('libcommon_test',
                                         force_compiler='clang',
                                         enable_cxx11=True,
                                         base_path='src/common')
  n.build_default_all_test_sources()
  if build_common.use_ndk_direct_execution():
    n.add_compiler_flags('-DUSE_NDK_DIRECT_EXECUTION')
  n.add_compiler_flags('-Werror')
  n.add_library_deps('libgccdemangle_static.a')
  _add_compile_flags(n)
  n.run(n.link())
