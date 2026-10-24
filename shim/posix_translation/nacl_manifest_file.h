// Overrides ARC's posix_translation/nacl_manifest_file.h. The Pepper/NaCl-backed handler it
// declares is not built in the wasm port; files that include it only refer to
// it by name. Part of the arc-wasm shim layer.
#ifndef POSIX_TRANSLATION_NACL_MANIFEST_FILE_H_
#define POSIX_TRANSLATION_NACL_MANIFEST_FILE_H_

namespace posix_translation {
class PepperFileHandler;
class NaClManifestFileHandler;
}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_NACL_MANIFEST_FILE_H_
