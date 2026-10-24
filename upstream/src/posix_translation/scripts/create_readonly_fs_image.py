#!src/build/run_python
#
# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Generates a read-only file system image.

Image file format:

[Number of files]     ; 32bit unsigned, big endian
[Offset of file #1]   ; 32bit unsigned, big endian (always 0x00000000)
[Size of file #1]     ; 32bit unsigned, big endian
[mtime of file #1]    ; 32bit unsigned, big endian
[Type of file #1]     ; 32bit unsigned, big endian
[Name of file #1]     ; Variable length, zero terminated, full path w/ slashes
(optional) [Link target of file #1] if file #1 is a symlink
[Zero padding to a 4-byte boundary]
[Offset of file #2]   ; 32bit unsigned, big endian
[Size of file #2]     ; 32bit unsigned, big endian
[mtime of file #2]    ; 32bit unsigned, big endian
[Type of file #2] ; 32bit unsigned, big endian
[Name of file #2]     ; Variable length, zero terminated, full path w/ slashes
(optional) [Link target of file #2] if file #1 is a symlink
[Zero padding to a 4-byte boundary]
...
[Zero padding to a 4-byte boundary]
[Offset of file #n]   ; 32bit unsigned, big endian
[Size of file #n]     ; 32bit unsigned, big endian
[mtime of file #n]    ; 32bit unsigned, big endian
[Name of file #n]     ; Variable length, zero terminated, full path w/ slashes
[Zero padding to a 4k or 64k page boundary]
[Content of file #1]  ; Variable length, page aligned
[Zero padding to a 4k or 64k page boundary]
[Content of file #2]  ; Variable length, page aligned
[Zero padding to a 4k or 64k page boundary]
...
[Content of file #n]  ; Variable length, page aligned
EOF

* All offset values are relative to the beginning of the content of file #1.
* Each file's content is aligned to a 64k page so our mmap() implementation
  can return page aligned address on both 4k-page and 64k-page environments.
* The image file itself should be mapped on a native (4k or 64k) page
  boundary.
"""

import argparse
import array
import os
import re
import struct
import sys
import time


_PAGE_SIZE = 64 * 1024  # NaCl uses 64k page.

# File type constants, which should be consistent with ones in
# readonly_fs_reader.h.
_REGULAR_FILE = 0
_SYMBOLIC_LINK = 1
_EMPTY_DIRECTORY = 2


def _normalize_path(input_filename):
  """Remove leading dots and adds / if the first character is not /."""
  input_filename = re.sub(r'^\.+', '', input_filename)
  input_filename = re.sub(r'^([^/])', r'/\1', input_filename)
  input_filename = re.sub(r'^/out/target/.*/root', '', input_filename)
  return input_filename


def _update_metadata(metadata, content, filename, file_size, file_mtime,
                     file_type, link_target):
  """Adds name, size, and offset of the |filename| to |metadata|."""
  # |content| is all the content up to this point.
  padded_content_size = content.buffer_info()[1]
  _pad_array(metadata, 4)
  metadata.fromstring(struct.pack('>iiii',
                                  padded_content_size,
                                  file_size,
                                  file_mtime,
                                  file_type)
                      + _normalize_path(filename).encode('utf_8')
                      + '\0')
  if link_target:
    metadata.fromstring(link_target.encode('utf_8') + '\0')


def _update_content(content, filename, size):
  """Adds the content of the |filename| to |content|."""
  with open(filename, 'r') as f:
    content.fromfile(f, size)


def _pad_array(array, boundary):
  """Adds padding to the array so the array size aligns to a next boundary."""
  size = array.buffer_info()[1]
  pad_bytes = boundary - (size % boundary)
  if pad_bytes == boundary:
    return
  for _ in xrange(pad_bytes):
    array.append(0)


def _write_image(image, output_filename):
  with open(output_filename, 'w') as f:
    image.tofile(f)


def _format_message(i, num_files, size, mtime, file_type, filename,
                    link_target):
  if file_type == _REGULAR_FILE:
    file_type_name = 'file'
  elif file_type == _SYMBOLIC_LINK:
    file_type_name = 'symlink'
  elif file_type == _EMPTY_DIRECTORY:
    file_type_name = 'empty_dir'
  message = 'VERBOSE: [%d/%d] [%s] %s: %d bytes (stored as %s)' % (
      i + 1, num_files, file_type_name, filename, size,
      _normalize_path(filename))
  if file_type == _SYMBOLIC_LINK:
    message += '-> %s' % link_target
  return message


def _get_metadata(filename, input_filenames, symlink_map, empty_dirs,
                  empty_files):
  if filename in symlink_map:
    file_type = _SYMBOLIC_LINK
    link_target = symlink_map[filename]
    size = 0
    mtime = time.time()  # Using the current time for a symlink.
  elif filename in empty_dirs:
    file_type = _EMPTY_DIRECTORY
    children = filter(lambda x: x.startswith(filename + '/'),
                      input_filenames)
    if children:
      print '%s is not empty' % filename
      sys.exit(1)
    link_target = None
    size = 0
    mtime = time.time()  # Using the current time for an empty directory.
  elif filename in empty_files:
    file_type = _REGULAR_FILE
    link_target = None
    size = 0
    mtime = time.time()  # Using the current time for an empty file.
  else:
    file_type = _REGULAR_FILE
    link_target = None
    try:
      size = os.stat(filename).st_size
      mtime = os.stat(filename).st_mtime
    except OSError, e:
      sys.exit(e)
  return file_type, link_target, size, mtime


def _generate_readonly_image(input_filenames, symlink_map, empty_dirs,
                             empty_files, verbose, output_filename):
  metadata = array.array('B')
  content = array.array('B')

  input_filenames.extend(symlink_map.keys())
  input_filenames.extend(empty_dirs)
  input_filenames.extend(empty_files)

  num_files = len(input_filenames)
  _pad_array(metadata, 4)
  metadata.fromstring(struct.pack('>i', num_files))
  for i in xrange(num_files):
    filename = input_filenames[i]
    if filename.endswith('/'):
      print '%s should not end with /' % filename
      sys.exit(1)
    file_type, link_target, size, mtime = _get_metadata(
        filename, input_filenames, symlink_map, empty_dirs, empty_files)
    if verbose:
      print _format_message(i, num_files, size, mtime, file_type, filename,
                            link_target)
    _update_metadata(metadata, content, filename, size, mtime, file_type,
                     link_target)
    if file_type == _REGULAR_FILE and size > 0:
      _update_content(content, filename, size)
    if i < num_files - 1:
      _pad_array(content, _PAGE_SIZE)
  _pad_array(metadata, _PAGE_SIZE)
  image = metadata + content
  _write_image(image, output_filename)


def main(args):
  parser = argparse.ArgumentParser()
  parser.add_argument('-o', '--output', metavar='FILE', required=True,
                      help='Write the output to filename.')
  parser.add_argument('-s', '--symlink-map', metavar='SYMLINK_MAP',
                      required=True, help='Map of symlinks to create.')
  parser.add_argument('-d', '--empty-dirs', metavar='EMPTY_DIRS',
                      required=True, help='List of empty directories.')
  parser.add_argument('-f', '--empty-files', metavar='EMPTY_FILES',
                      required=True, help='List of empty files.')
  parser.add_argument('-v', '--verbose', action='store_true',
                      help='Emit verbose output.')
  parser.add_argument(dest='input', metavar='INPUT', nargs='+',
                      help='Input file(s) to process.')
  args = parser.parse_args()

  empty_dirs = args.empty_dirs.split(',') if args.empty_dirs else []
  empty_files = args.empty_files.split(',') if args.empty_files else []
  symlink_map = dict([x.split(':') for x in args.symlink_map.split(',')])

  _generate_readonly_image(args.input, symlink_map, empty_dirs, empty_files,
                           args.verbose, args.output)
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
