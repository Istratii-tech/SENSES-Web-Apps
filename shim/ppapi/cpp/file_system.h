// Placeholder pp::FileSystem. ARC's VFS hands these to Pepper-backed handlers,
// which the wasm port replaces (OPFS etc.), so it carries no state. Part of
// the arc-wasm shim layer.
#ifndef PPAPI_CPP_FILE_SYSTEM_H_
#define PPAPI_CPP_FILE_SYSTEM_H_

namespace pp {

class FileSystem {};

}  // namespace pp

#endif  // PPAPI_CPP_FILE_SYSTEM_H_
