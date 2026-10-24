// NaCl ABI stat layout (native_client/src/trusted/service_runtime/include/
// sys/stat.h), needed only so arc_strace.cc can still format it. The wasm
// port never produces one. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NACL_STAT_H_
#define ARC_WASM_SHIM_NACL_STAT_H_

#include <stdint.h>

struct nacl_abi_stat {
  int64_t nacl_abi_st_dev;
  uint64_t nacl_abi_st_ino;
  uint32_t nacl_abi_st_mode;
  uint32_t nacl_abi_st_nlink;
  uint32_t nacl_abi_st_uid;
  uint32_t nacl_abi_st_gid;
  int64_t nacl_abi_st_rdev;
  int64_t nacl_abi_st_size;
  int32_t nacl_abi_st_blksize;
  int32_t nacl_abi_st_blocks;
  int64_t nacl_abi_st_atime;
  int64_t nacl_abi_st_atimensec;
  int64_t nacl_abi_st_mtime;
  int64_t nacl_abi_st_mtimensec;
  int64_t nacl_abi_st_ctime;
  int64_t nacl_abi_st_ctimensec;
};

#endif  // ARC_WASM_SHIM_NACL_STAT_H_
