#!src/build/run_python

# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Generator of PPAPI mock source files."""

import os
import sys

import idl_c_proto
import idl_generator
import idl_option
import idl_parser

idl_option.Option('dstroot', 'Base directory of output', default='')

MAX_GMOCK_ARGS = 10


def _GetMockStructName(cgen, interface):
  return cgen.GetStructName(interface, None) + '_Mock'


def _GetPpapiCHeaderPath(filenode):
  path = filenode.GetProperty('NAME')
  path = os.path.join('ppapi/c', os.path.splitext(path)[0] + '.h')
  return os.path.normpath(path)


def _GetMockPathInternal(filenode, relpath, ext):
  basename = os.path.basename(filenode.GetProperty('NAME'))
  path = os.path.join(relpath, os.path.splitext(basename)[0] + ext)
  return os.path.normpath(path)


def _GetMockHeaderPath(filenode, relpath):
  return _GetMockPathInternal(filenode, relpath, '.h')


def _GetMockSourcePath(filenode, relpath):
  return _GetMockPathInternal(filenode, relpath, '.cc')


def _GetGuardMacro(filenode, relpath):
  header_path = _GetMockHeaderPath(filenode, relpath)
  return header_path.replace('/', '_').replace('.', '_').upper() + '_'


class PpapiMockHeaderGenerator(idl_generator.GeneratorByFile):
  HEADER_TEMPLATE = '\n'.join([
      '// Copyright 2014 The Chromium Authors. All rights reserved.',
      '// Use of this source code is governed by a BSD-style license ' +
      'that can be',
      '// found in the LICENSE file.',
      '//',
      '// Provide %(mock_list)s.',
      '//',
      '// Auto-generated. Do not edit!',
      '',
      '#ifndef %(guard_macro)s',
      '#define %(guard_macro)s',
      '',
      '#include "gmock/gmock.h"',
      '',
      '#include "ppapi/c/pp_completion_callback.h"',
      '#include "%(path)s"',
      ''])

  MOCK_CLASS_TEMPLATE = '\n'.join([
      'class %(mock_name)s {',
      ' public:',
      '  %(mock_name)s();',
      '  virtual ~%(mock_name)s();',
      '',
      '%(method_list)s',
      '};'])

  def __init__(self):
    idl_generator.Generator.__init__(
        self, 'PPAPI Mock Header', 'ppapihgen',
        'Generate the PPAPI Mock headers')
    self.cgen = idl_c_proto.CGen()

  def GenerateFile(self, filenode, releases, options):
    if not os.path.basename(filenode.GetName()).startswith('ppb_'):
      return

    # Generator library has an issue that some getters changes its object's
    # internal status, and the main usage of the generator in Chrome (i.e.
    # generating PPAPI C/C++ headers and shims) relies on that behavior.
    # So, here we mimic the key part of what the main usage relies on.
    # Without this, it actually causes a problem that it produces the
    # un-compilable source code.
    for node in filenode.GetListOf('Typedef'):
      node.GetUniqueReleases(releases)

    # Generate file content.
    file_header = self.GenerateFileHeader(filenode, releases)
    file_body = self.GenerateFileBody(filenode, releases)
    file_footer = self.GenerateFileFooter(filenode, releases)

    # Write the content to the file.
    path = _GetMockHeaderPath(filenode, idl_option.GetOption('dstroot'))
    with open(path, 'w') as stream:
      stream.writelines([
          file_header, '\n', file_body, '\n', file_footer, '\n'])

  def GenerateFileHeader(self, filenode, releases):
    struct_name_list = [
        self.cgen.GetStructName(interface, release, include_version=True)
        for interface in filenode.GetListOf('Interface')
        for release in interface.GetUniqueReleases(releases)]

    return PpapiMockHeaderGenerator.HEADER_TEMPLATE % {
        'mock_list': ', '.join(name + ' mock' for name in struct_name_list),
        'guard_macro': _GetGuardMacro(filenode, 'ppapi_mocks'),
        'path': _GetPpapiCHeaderPath(filenode),
    }

  def GenerateFileBody(self, filenode, releases):
    # Generate mock class declaration list.
    cgen = self.cgen
    mock_class_list = []
    for interface in filenode.GetListOf('Interface'):
      mock_name = _GetMockStructName(cgen, interface)
      method_list = []
      signature_set = set()
      unique_releases = interface.GetUniqueReleases(releases)
      for member in interface.GetListOf('Member'):
        for release in unique_releases:
          if not member.InReleases([release]):
            continue
          return_type, name, arrayspec, callspec = cgen.GetComponents(
              member, release, 'ref')
          if len(callspec) > MAX_GMOCK_ARGS:
            # We do not generate methods which have more than MAX_GMOCK_ARGS
            # arguments.
            continue
          signature = cgen.Compose(
              return_type, '', arrayspec, callspec, '', False, False, False)
          if (name, signature) in signature_set:
            continue
          signature_set.add((name, signature))
          method_list.append(
              '  MOCK_METHOD%d(%s, %s);' % (len(callspec), name, signature))
      mock_class_list.append(PpapiMockHeaderGenerator.MOCK_CLASS_TEMPLATE % {
          'mock_name': mock_name,
          'method_list': '\n'.join(method_list)
      })

    return '\n\n'.join(mock_class_list) + '\n\n'

  def GenerateFileFooter(self, filenode, release):
    return '#endif  // %s' % _GetGuardMacro(filenode, 'ppapi_mocks')


header_generator = PpapiMockHeaderGenerator()


class PpapiMockSourceGenerator(idl_generator.GeneratorByFile):
  HEADER_TEMPLATE = '\n'.join([
      '// Copyright 2014 The Chromium Authors. All rights reserved.',
      '// Use of this source code is governed by a BSD-style license ' +
      'that can be',
      '// found in the LICENSE file.',
      '//',
      '// Provide %(mock_list)s.',
      '//',
      '// Auto-generated. Do not edit!',
      '',
      '#include "%(mock_header_path)s"',
      ''
      '#include "ppapi_mocks/ppapi_mock_factory.h"',
      '#include "ppapi_mocks/ppapi_mock_impl.h"',
      ''])

  MOCK_FUNCTION_TEMPLATE = '\n'.join([
      'static %(signature)s {',
      '  return ppapi_mock::MockHolder<%(mock_name)s>::GetMock()->',
      '      %(name)s(%(args)s);',
      '}'])

  EMPTY_MOCK_FUNCTION_TEMPLATE = '\n'.join([
      'static %(signature)s {',
      '  // FUNCTION CANNOT BE MOCKED.',
      '  return %(return_type)s();',
      '}'])

  MOCK_SOURCE_TEMPLATE = '\n'.join([
      '%(mock_name)s::%(mock_name)s() {',
      '}',
      '%(mock_name)s::~%(mock_name)s() {',
      '}',
      '',
      'namespace ppapi_mock {',
      'template<> ::testing::NiceMock<%(mock_name)s>*',
      'MockHolder<%(mock_name)s>::instance_ = NULL;',
      '}  // namespace ppapi_mock',
      '',
      'template<> void PpapiMockFactory::GetMock<%(mock_name)s>(',
      '    ::testing::NiceMock<%(mock_name)s>** result) {',
      '  *result = ppapi_mock::MockHolder<%(mock_name)s>::GetMock();',
      '}',
      '',
      '%(struct_definition_list)s',
      '',
      'INVOKE_AT_OBJECT_LOAD_TIME(%(mock_name)s, {',
      '  ppapi_mock::MockRegistry::GetInstance()->Register<%(mock_name)s>();',
      '  %(struct_registration_list)s',
      '});'])

  INJECTED_STRUCT_TEMPLATE = '\n'.join([
      '%(method_list)s',
      '',
      'static %(struct_name)s s_%(struct_name)s = {',
      '%(struct_field_list)s',
      '};',
      ''])

  STRUCT_REGISTRATION_TEMPLATE = '\n'.join([
      '  ppapi_mock::InterfaceRegistry::GetInstance()->Register(',
      '      %(macro_name)s, &s_%(struct_name)s);'])

  def __init__(self):
    idl_generator.Generator.__init__(
        self, 'PPAPI Mock source', 'ppapicgen',
        'Generate the PPAPI Mock sources')
    self.cgen = idl_c_proto.CGen()

  def GenerateFile(self, filenode, releases, options):
    if not os.path.basename(filenode.GetName()).startswith('ppb_'):
      return

    # Similar to PpapiMockHeaderGenerator, we call to GetUniqueReleases
    # as a work around. Please see also PpapiMockHeaderGenerator.GenerateFile.
    for node in filenode.GetListOf('Typedef'):
      node.GetUniqueReleases(releases)

    # Generate file content.
    file_header = self.GenerateFileHeader(filenode, releases)
    file_body = self.GenerateFileBody(filenode, releases)

    # Write the content to the file.
    path = _GetMockSourcePath(filenode, idl_option.GetOption('dstroot'))
    with open(path, 'w') as stream:
      stream.writelines([file_header, '\n', file_body, '\n\n'])

  def GenerateFileHeader(self, filenode, releases):
    struct_name_list = [
        self.cgen.GetStructName(interface, release, include_version=True)
        for interface in filenode.GetListOf('Interface')
        for release in interface.GetUniqueReleases(releases)]
    return PpapiMockSourceGenerator.HEADER_TEMPLATE % {
        'mock_list': ', '.join(name + ' mock' for name in struct_name_list),
        'mock_header_path': _GetMockHeaderPath(filenode, 'ppapi_mocks'),
    }

  def GenerateFileBody(self, filenode, releases):
    cgen = self.cgen
    result = []
    for interface in filenode.GetListOf('Interface'):
      mock_name = _GetMockStructName(cgen, interface)
      struct_definition_list = []
      struct_registration_list = []
      for release in interface.GetUniqueReleases(releases):
        struct_name = cgen.GetStructName(
            interface, release, include_version=True)
        macro_name = cgen.GetInterfaceMacro(
            interface, interface.GetVersion(release))

        method_list = []
        method_name_list = []
        for member in interface.GetListOf('Member'):
          if not member.InReleases([release]):
            continue
          return_type, name, arrayspec, callspec = cgen.GetComponents(
              member, release, 'ref')
          signature = cgen.GetSignature(
              member, release, 'ref', prefix=struct_name + '_',
              func_as_ptr=False, include_name=True, include_version=False)

          if len(callspec) <= MAX_GMOCK_ARGS:
            method_template = PpapiMockSourceGenerator.MOCK_FUNCTION_TEMPLATE
          else:
            method_template = (
                PpapiMockSourceGenerator.EMPTY_MOCK_FUNCTION_TEMPLATE)
          method_list.append(method_template % {
              'signature': signature,
              'mock_name': mock_name,
              'return_type': return_type,
              'name': name,
              'args': ', '.join(arg for _, arg, _, _ in callspec)
          })
          method_name_list.append(struct_name + '_' + name)

        injected_struct = PpapiMockSourceGenerator.INJECTED_STRUCT_TEMPLATE % {
            'method_list': '\n\n'.join(method_list),
            'struct_field_list': '\n'.join(
                '    %s,' % method_name for method_name in method_name_list),
            'struct_name': struct_name,
        }
        struct_definition_list.append(injected_struct)
        registration = PpapiMockSourceGenerator.STRUCT_REGISTRATION_TEMPLATE % {
            'macro_name': macro_name,
            'struct_name': struct_name,
        }
        struct_registration_list.append(registration)
      result.append(PpapiMockSourceGenerator.MOCK_SOURCE_TEMPLATE % {
          'mock_name': mock_name,
          'struct_definition_list': '\n\n'.join(struct_definition_list),
          'struct_registration_list': '\n\n'.join(struct_registration_list)
      })
    return '\n\n'.join(result)


source_generator = PpapiMockSourceGenerator()


def main():
  filenames = idl_option.ParseOptions(sys.argv[1:])
  ast = idl_parser.ParseFiles(filenames)
  assert not ast.errors, ast.errors
  return idl_generator.Generator.Run(ast)

if __name__ == '__main__':
  main()
