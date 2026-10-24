# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os

from src.build import build_common
from src.build import ninja_generator
from src.build import ninja_generator_runner
from src.build import open_source
from src.build import staging
from src.build.build_options import OPTIONS
from src.build.util import python_deps


_CREATE_READONLY_FS_IMAGE_SCRIPT = (
    'src/posix_translation/scripts/create_readonly_fs_image.py')


# Mount points for directories.
_EMPTY_DIRECTORIES = ['/cache',
                      '/data',
                      '/storage',
                      '/sys/devices/system/cpu',
                      '/system/lib',
                      '/usr/lib',
                      '/vendor/chromium/crx',
                      '/vendor/lib']


# Mount points used for files.
_EMPTY_FILES = ['/dev/ashmem',
                '/dev/log/events',
                '/dev/log/main',
                '/dev/log/radio',
                '/dev/log/system',
                '/dev/null',
                '/dev/random',
                '/dev/urandom',
                '/dev/zero',
                '/proc/cpuinfo',
                '/sys/kernel/debug/tracing/trace_marker',
                '/system/bin/sh']


# A map of symlinks that'll be created in the read-only file system
# image, where keys are symlink paths and values are target paths. For
# example,
#
#   '/etc': '/system/etc'
#
# means that /etc is a symlink to /system/etc.
_SYMLINK_MAP = {
    '/d': '/sys/kernel/debug',
    '/etc': '/system/etc',
    # Android has /mnt/sdcard, /sdcard, and /storage/sdcard0 all as symlinks
    # to /storage/emulated/legacy.  For ARC we have /sdcard and /mnt/sdcard
    # as symlinks to /storage/sdcard.  This is compatible with Android
    # emulator as of KitKat.
    # TODO(crbug.com/196442): When we properly support migration between
    # different Android versions, we should move these files for all apps
    # into the same real location that Android currently uses and make the
    # rest of the files be symlinks.
    '/sdcard': '/storage/sdcard',
    '/mnt/sdcard': '/storage/sdcard',
    '/system/lib64': '/system/lib',
}


# Generate posix_translation. This library adds functions for converting POSIX
# API calls into PPAPI calls.
def _generate_libposix_translation():
  compiler_flags = [
      '-Werror', '-fvisibility=hidden', '-fvisibility-inlines-hidden']

  n = ninja_generator.ArchiveNinjaGenerator(
      'libposix_translation_static',
      # libart-gtest and libposix_translation need this
      force_compiler='clang',
      enable_cxx11=True)
  n.add_compiler_flags(*compiler_flags)
  if OPTIONS.is_posix_translation_debug():
    n.add_defines('DEBUG_POSIX_TRANSLATION')
  if OPTIONS.use_verbose_memory_viewer():
    n.add_defines('USE_VERBOSE_MEMORY_VIEWER')
  # For functions in chromium_org/base/ and private headers in ppapi/.
  # TODO(crbug.com/234789): Use public API so that we can depend on
  # nacl_pepper_path instead.
  n.add_ppapi_compile_flags()
  n.add_libchromium_base_compile_flags()
  all_files = build_common.find_all_files(['src/posix_translation'],
                                          ['.cc'])
  n.build_default(all_files).archive()

  n = ninja_generator.SharedObjectNinjaGenerator(
      'libposix_translation',
      is_system_library=True,
      force_compiler='clang',
      enable_cxx11=True)
  n.add_library_deps('libc.so', 'libm.so', 'libdl.so', 'libstlport.so')
  n.add_whole_archive_deps('libposix_translation_static.a')
  # Statically link libchromium_base.a so that we can use unwrapped version of
  # the library.
  # TODO(crbug.com/423063, crbug.com/336316): Statically link libcommon.a into
  # the DSO too for more safety.
  n.add_library_deps('libchromium_base.a')
  n.add_compiler_flags(*compiler_flags)
  n.add_ppapi_link_flags()
  n.build_default([]).link()


def _generate_libposix_translation_for_test():
  n = ninja_generator.SharedObjectNinjaGenerator(
      'libposix_translation_for_test',
      dt_soname='libposix_translation.so',
      is_system_library=True,
      is_for_test=True)
  # libposix_translation_for_test is built using generated code in the out/
  # directory. The logic we have to scan for NOTICES does not find one for this
  # generated code, and complains later. The libposix_translation.so code
  # normally inherits the project NOTICE from src/NOTICE, so we just explicitly
  # point to it here as the notice to use for this special version of the
  # library.
  n.add_notice_sources([staging.as_staging('src/NOTICE')])
  gen_rule_name = 'gen_wrap_syscall_aliases_s'
  gen_script_path = os.path.join('src/common', gen_rule_name + '.py')
  n.rule(gen_rule_name,
         command='%s > $out.tmp && mv $out.tmp $out' % gen_script_path,
         description=gen_rule_name + ' $in')
  gen_out_path = os.path.join(build_common.get_build_dir(),
                              'posix_translation_gen_sources',
                              'wrap_syscall_aliases.S')
  gen_implicit_deps = python_deps.find_deps(gen_script_path)
  n.build(gen_out_path, gen_rule_name, implicit=gen_implicit_deps)
  # Following deps order is important. art_libc_supplement_for_test.so should
  # be placed before libc.so.
  if not open_source.is_open_source_repo():
    # This is for ART unit tests which have not been opensourced. To not let
    # posix_translation depend on ART on arc_open, use is_open_source_repo().
    n.add_library_deps('art_libc_supplement_for_test.so')
  n.add_library_deps('libc.so', 'libdl.so')
  n.build_default([gen_out_path], base_path=None).link()

  # /test/libposix_translation.so should be a symbolic link to
  # ../lib/libposix_translation_for_test.so.
  n = ninja_generator.NinjaGenerator('test_libposix_translation')
  orig_so = os.path.join(
      build_common.get_load_library_path(), 'libposix_translation_for_test.so')
  link_so = os.path.join(
      build_common.get_load_library_path_for_test(), 'libposix_translation.so')
  command = 'ln -sf %s %s' % (
      os.path.join('../../lib/', os.path.basename(orig_so)), link_so)
  n.build(link_so, 'run_shell_command', implicit=orig_so,
          variables={'command': command})


def _generate_libposix_files():
  n = ninja_generator.NinjaGenerator('libposix_files')
  install_files = [
      ('proc', 'src/posix_translation/proc/cmdline'),
      ('proc', 'src/posix_translation/proc/loadavg'),
      ('proc', 'src/posix_translation/proc/meminfo'),
      ('proc/net', 'src/posix_translation/proc/net/tcp'),
      ('proc/net', 'src/posix_translation/proc/net/tcp6'),
      ('proc/net', 'src/posix_translation/proc/net/udp'),
      ('proc/net', 'src/posix_translation/proc/net/udp6'),
      ('proc', 'src/posix_translation/proc/stat'),
      ('proc', 'src/posix_translation/proc/version')]
  for dst, src in install_files:
    file_name = os.path.basename(src)
    dst = os.path.join(dst, file_name)
    n.install_to_root_dir(dst, src)


def generate_ninjas():
  ninja_generator_runner.request_run_in_parallel(
      _generate_libposix_files,
      _generate_libposix_translation,
      _generate_libposix_translation_for_test)


def generate_test_ninjas():
  n = ninja_generator.PpapiTestNinjaGenerator(
      'posix_translation_test',
      base_path='src/posix_translation',
      force_compiler='clang',
      enable_cxx11=True)
  # Build a rootfs image for tests.
  rule_name = 'gen_test_fs_image'
  script_path = 'src/posix_translation/scripts/create_test_fs_image.py'

  gen_prod_image = (
      build_common.get_posix_translation_readonly_fs_image_file_path())

  # This is a little odd, but we use the documented path to the production image
  # to also store a test image in the same location for simplicity.
  out_path = os.path.dirname(gen_prod_image)
  gen_test_image = os.path.join(out_path, 'test_readonly_fs_image.img')

  n.rule(rule_name,
         command=script_path + ' $out_path',
         description=rule_name + ' $in_real_path')
  n.add_ppapi_compile_flags()
  n.build([gen_test_image], rule_name,
          variables={'out_path': out_path},
          # The script calls create_readonly_fs_image.py.
          implicit=[script_path,
                    _CREATE_READONLY_FS_IMAGE_SCRIPT,
                    ])
  all_files = n.find_all_contained_test_sources()

  n.build_default(all_files, base_path=None)
  n.add_compiler_flags('-Werror')
  n.add_library_deps('libposix_translation_static.a',
                     'libchromium_base.a',
                     'libcommon.a',
                     'libgccdemangle_static.a')
  if build_common.use_ndk_direct_execution():
    n.add_defines('USE_NDK_DIRECT_EXECUTION')
  implicit = [gen_test_image]
  if open_source.is_open_source_repo():
    implicit.append(gen_prod_image)
    n.add_defines('PROD_READONLY_FS_IMAGE="%s"' % gen_prod_image)
  else:
    # Add runtime to implicit dependencies because ReadonlyFsReaderTest uses
    # the readonly FS image in runtime.
    implicit.append(build_common.get_runtime_build_stamp())
    n.add_defines('PROD_READONLY_FS_IMAGE="%s"' % os.path.join(
                  build_common.get_runtime_platform_specific_path(
                      build_common.get_runtime_out_dir(), OPTIONS.target()),
                  os.path.basename(gen_prod_image)))
    n.run(n.link(), implicit=implicit)

  # To be able to refer mock implementation from outside of posix_translation.
  # Setting instance count is zero because usage count verifier doesn't check
  # the reference from test executable. See verify_usage_counts in
  # ninja_generator.py
  n = ninja_generator.ArchiveNinjaGenerator(
      'mock_posix_translation',
      instances=0,
      force_compiler='clang',
      enable_cxx11=True)
  n.add_libchromium_base_compile_flags()
  n.add_compiler_flags('-Werror')
  all_files = ['src/posix_translation/test_util/mock_virtual_file_system.cc']
  n.build_default(all_files).archive()


def generate_binaries_depending_ninjas(root_dir_install_all_targets):
  n = ninja_generator.NinjaGenerator('readonly_fs_image')
  rule_name = 'gen_readonly_fs_image'
  encoded_symlink_map = ','.join([x + ':' + y for x, y in
                                  _SYMLINK_MAP.iteritems()])
  encoded_empty_dirs = ','.join(_EMPTY_DIRECTORIES)
  encoded_empty_files = ','.join(_EMPTY_FILES)

  n.rule(rule_name,
         command=_CREATE_READONLY_FS_IMAGE_SCRIPT + ' -o $out ' +
         '-s "' + encoded_symlink_map + '" '
         '-d "' + encoded_empty_dirs + '" '
         '-f "' + encoded_empty_files + '" '
         '$in', description=rule_name)
  gen_img = build_common.get_posix_translation_readonly_fs_image_file_path()

  my_dependencies = sorted(root_dir_install_all_targets)
  # The configure options file is a dependency as symlinks in the read-only
  # file system image changes per the configure options.
  implicit = [_CREATE_READONLY_FS_IMAGE_SCRIPT,
              OPTIONS.get_configure_options_file()]
  n.build([gen_img], rule_name, my_dependencies,
          implicit=implicit)
