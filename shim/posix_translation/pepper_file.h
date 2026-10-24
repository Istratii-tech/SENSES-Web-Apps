// Overrides ARC's posix_translation/pepper_file.h. The Pepper/NaCl-backed handler it
// declares is not built in the wasm port; files that include it only refer to
// it by name. Part of the arc-wasm shim layer.
#ifndef POSIX_TRANSLATION_PEPPER_FILE_H_
#define POSIX_TRANSLATION_PEPPER_FILE_H_

// The real header pulled these in; virtual_file_system.cc relies on that.
#include "ppapi/cpp/module.h"

namespace posix_translation {
class PepperFileHandler;
class NaClManifestFileHandler;
}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_PEPPER_FILE_H_
