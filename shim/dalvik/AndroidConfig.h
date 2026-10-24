// AOSP's per-platform build config, force-included into every Dalvik-stack
// source (as build/core/combo/include/arch/*/AndroidConfig.h is in AOSP).
// Based on linux-x86 (a host build: no HAVE_ANDROID_OS), minus what the
// browser cannot do: fork/exec, SysV IPC, OOM adjust, execinfo backtraces.
// Part of the arc-wasm shim layer.
#ifndef _ANDROID_CONFIG_H
#define _ANDROID_CONFIG_H

#define HAVE_PTHREADS
// clock_gettime: CLOCK_MONOTONIC is performance.now(); Emscripten serves the
// CPU-time clocks from it too (there is no per-thread CPU time).
#define HAVE_POSIX_CLOCKS
#define HAVE_FUTEX
#define HAVE_POSIX_FILEMAP
#define HAVE_SYS_UIO_H 1
#define HAVE_SYMLINKS
#define HAVE_LOCALTIME_R 1
#define HAVE_IOCTL
#define HAVE_EPOLL
#define HAVE_ENDIAN_H
#define HAVE_LITTLE_ENDIAN
#define _LARGEFILE_SOURCE 1
#define HAVE_OFF64_T
#define HAVE_CXXABI 0
#define HAVE_MALLOC_H
#define HAVE_MADVISE 1
#define HAVE_TM_GMTOFF 1
#define HAVE_DIRENT_D_TYPE 1
#define OS_SHARED_LIB_FORMAT_STR "lib%s.so"
#define MINCORE_POINTER_TYPE unsigned char *
#define OS_PATH_SEPARATOR '/'
#define OS_CASE_SENSITIVE
#define HAVE_SYS_SOCKET_H 1
#define HAVE_OPEN_MEMSTREAM 1
#define HAVE_WRITEV 1
#define HAVE_STDINT_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_SCHED_H 1
#define HAVE_PREAD 1
#define HAVE_STAT_ST_MTIM 1
#define HAVE_PRINTF_ZD 1
#define HAVE_GNU_QSORT_R 0
#define HAVE_BSD_QSORT_R 0

#endif  // _ANDROID_CONFIG_H
