#!src/build/run_python
#
# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Creates a test file system image for posix_translation_tests.

Usage: create_test_fs_image.py output_dir/

Example:
$ mkdir /tmp/test_image
$ ./src/posix_translation/scripts/create_test_fs_image.py /tmp/test_image
$ ls -s /tmp/test_image
 384 test_readonly_image.img
$ ./src/posix_translation/scripts/dump_readonly_fs_image.py \
    /tmp/test_image/test_readonly_image.img
[file] /test/a.odex 4 bytes at 0x00000000 (page 0, "Sat May 10 11:12:13 2014")
[file] /test/big.odex 100000 bytes at 0x00010000 (page 1, "...")
[file] /test/b.odex 1 bytes at 0x00030000 (page 3, "...")
[file] /test/c0.odex 0 bytes at 0x00040000 (page 4, "...")
[file] /test/c.odex 0 bytes at 0x00040000 (page 4, "...")
[file] /test/dir/c.odex 1 bytes at 0x00040000 (page 4, "...")
[file] /test/dir/empty.odex 0 bytes at 0x00050000 (page 5, "...")
[symlink] /test/symlink2 0 bytes at 0x00050000 (page 5, "...") -> /test/b.odex
[symlink] /test/symlink1 0 bytes at 0x00050000 (page 5, "...") -> /test/a.odex
[empty_dir] /test/emptydir 0 bytes at 0x00050000 (page 5, "...")
[file] /test/emptyfile 0 bytes at 0x00050000 (page 5, "...")
"""

import os
import subprocess
import sys
import tempfile

_ARC_ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.realpath(__file__)), '..', '..', '..'))


def main(args):
  py_script = os.path.join(
      _ARC_ROOT, 'src', 'posix_translation', 'scripts',
      'create_readonly_fs_image.py')
  run_python = os.path.join(
      _ARC_ROOT, 'src', 'build', 'run_python')
  outdir = os.path.realpath(args[1])
  extra_args = ' '.join(args[2:])
  if not os.access(outdir, os.F_OK):
    os.makedirs(outdir)
  workdir = tempfile.mkdtemp()

  os.chdir(workdir)
  os.makedirs('test/dir/')
  files = ['test/a.odex', 'test/big.odex', 'test/b.odex', 'test/c0.odex',
           'test/c.odex', 'test/dir/c.odex', 'test/dir/empty.odex']
  symlink_map = {
      '/test/symlink1': '/test/a.odex',
      '/test/symlink2': '/test/b.odex',
  }
  encoded_symlink_map = ','.join(
      [x + ':' + y for x, y in symlink_map.iteritems()])

  empty_dirs = ['/test/emptydir']
  encoded_empty_dirs = ','.join(empty_dirs)

  empty_files = ['/test/emptyfile']
  encoded_empty_files = ','.join(empty_files)

  page_size = 1 << 16
  expected_file_size = 0
  with open(files[0], 'w') as f:
    f.write('123\n')
    expected_file_size += page_size
  with open(files[1], 'w') as f:
    for _ in xrange(90000):
      f.write(chr(0))
    for _ in xrange(10000):
      f.write('X')
    expected_file_size += page_size * 2
  with open(files[2], 'w') as f:
    f.write('Z')
    expected_file_size += page_size
  with open(files[3], 'w') as f:
    pass
  with open(files[4], 'w') as f:
    pass
  with open(files[5], 'w') as f:
    f.write('A')
    expected_file_size += page_size
  with open(files[6], 'w') as f:
    pass
  expected_file_size += page_size  # For the metadata at the beginning.

  subprocess.call('%s %s %s -o %s/test_readonly_fs_image.img -s "%s" -d "%s" '
                  '-f "%s" %s' %
                  (run_python, py_script, extra_args, outdir,
                   encoded_symlink_map, encoded_empty_dirs,
                   encoded_empty_files, ' '.join(files)),
                  shell=True)
  subprocess.call('rm -rf %s' % workdir, shell=True)

  # Check the output image size. This ensures that empty files don't consume
  # 64KB pages.
  file_size = os.path.getsize('%s/test_readonly_fs_image.img' % outdir)
  assert file_size == expected_file_size

  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv))
