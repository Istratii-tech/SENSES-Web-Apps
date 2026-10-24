# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import pipes

from src.build import ninja_generator
from src.build import staging

_MY_DIR = 'src/ppapi_mocks'


def _generate_libppapi_mocks():
  ppapi_dir = staging.as_staging('chromium-ppapi/ppapi')
  api_dir = os.path.join(ppapi_dir, 'api')
  out_dir = os.path.join(
      ninja_generator.PpapiTestNinjaGenerator.get_ppapi_mocks_generated_dir(),
      'ppapi_mocks')
  stamp_file = os.path.join(out_dir, 'STAMP')

  # Generate PPAPI mock sources from IDL files.
  rule_name = 'gen_ppapi_mock'
  n = ninja_generator.PythonNinjaGenerator(rule_name)
  script_path = os.path.join(_MY_DIR, 'gen_ppapi_mock.py')
  args = ['--wnone',  # Suppress all warnings.
          '--range=start,end',  # Generate code for all revisions.
          '--ppapicgen',  # Generate PpapiMock source files.
          '--ppapihgen',  # Generate PpapiMock header files.
          '--srcroot', api_dir,
          '--dstroot', out_dir]
  n.emit_python_rule(
      rule_name, script_path, args, extra_command='&& touch $stamp')

  generated_files = []
  idl_list = n.find_all_files('chromium-ppapi/ppapi/api', '.idl')
  for idl_path in idl_list:
    if 'finish_writing_these' in idl_path:
      # Files under ppapi/api/private/finish_writing_these/ directory are not
      # found by the parser of the idl generator library.
      continue
    path_stem = os.path.splitext(os.path.basename(idl_path))[0]
    # We are interested in only PPB files.
    if not path_stem.startswith('ppb_'):
      continue
    generated_files.append(os.path.join(out_dir, path_stem + '.cc'))
    generated_files.append(os.path.join(out_dir, path_stem + '.h'))

  n.run_python(generated_files + [stamp_file], rule_name,
               variables={'stamp': pipes.quote(stamp_file)},
               implicit=map(staging.as_staging, idl_list))

  # Build libppapi_mocks. libart-gtest depends on libppapi_mocks.
  n = ninja_generator.ArchiveNinjaGenerator('libppapi_mocks',
                                            instances=0,
                                            force_compiler='clang',
                                            enable_cxx11=True)
  n.add_ppapi_compile_flags()
  n.add_libchromium_base_compile_flags()
  n.add_include_paths(
      staging.as_staging('testing/gmock/include'),
      _MY_DIR,
      ninja_generator.PpapiTestNinjaGenerator.get_ppapi_mocks_generated_dir())

  n.build_default(
      [path for path in generated_files if path.endswith('.cc')] +
      n.find_all_files([_MY_DIR], '.cc', include_tests=True),
      implicit=[stamp_file])
  n.archive()


def generate_ninjas():
  _generate_libppapi_mocks()
