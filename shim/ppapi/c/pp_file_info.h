// Subset of ppapi/c/pp_file_info.h. ARC's VFS still uses PP_FileInfo as its
// stat-cache record type, so it is kept as a plain struct. Part of the
// arc-wasm shim layer.
#ifndef PPAPI_C_PP_FILE_INFO_H_
#define PPAPI_C_PP_FILE_INFO_H_

#include <stdint.h>

#include "ppapi/c/pp_types.h"

typedef enum {
  PP_FILETYPE_REGULAR = 0,
  PP_FILETYPE_DIRECTORY = 1,
  PP_FILETYPE_OTHER = 2,
} PP_FileType;

typedef enum {
  PP_FILESYSTEMTYPE_INVALID = 0,
  PP_FILESYSTEMTYPE_EXTERNAL = 1,
  PP_FILESYSTEMTYPE_LOCALPERSISTENT = 2,
  PP_FILESYSTEMTYPE_LOCALTEMPORARY = 3,
  PP_FILESYSTEMTYPE_ISOLATED = 4,
} PP_FileSystemType;

struct PP_FileInfo {
  int64_t size;
  PP_FileType type;
  PP_FileSystemType system_type;
  PP_Time creation_time;
  PP_Time last_access_time;
  PP_Time last_modified_time;
};

#endif  // PPAPI_C_PP_FILE_INFO_H_
