// The posix_translation "kernel" for wasm applications.
//
// Under NaCl, ARC hooked the IRT (NaCl's syscall interface) so every libc
// call landed in posix_translation's VirtualFileSystem. Here the equivalent
// hook is Emscripten's syscall layer: arc_kernel.cc defines the __syscall_*
// and WASI fd_* functions (and libc's mmap family) that musl calls, and
// routes them into the VFS. open(), fopen(), opendir(), mmap(), pipe(),
// socket() ... in application code then reach ARC's VFS without changes.
//
// Calls made while the kernel itself is running (e.g. ReadonlyFileHandler
// reading its image from the host), calls before Boot(), and calls on the
// browser main thread go to the original Emscripten implementations ("the
// host"). Applications must therefore run off the main thread
// (-sPROXY_TO_PTHREAD), which ARC required of Pepper plugins as well.
//
// Link shim/kernel/arc_kernel.cc instead of shim/wasm_mm/libc_mm_host.cc.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_KERNEL_ARC_KERNEL_H_
#define ARC_WASM_SHIM_KERNEL_ARC_KERNEL_H_

namespace posix_translation {
class VirtualFileSystem;
}

namespace arc_kernel {

// Creates the VirtualFileSystem, registers stdin/stdout/stderr (passed
// through to the host), mounts /dev/{null,zero,urandom,random,ashmem,alarm},
// and starts routing libc into it. Mount more file systems on the returned
// VFS (VirtualFileSystem::Mount is thread-safe). Call once, off the browser
// main thread.
posix_translation::VirtualFileSystem* Boot();

// True once Boot() has run.
bool IsBooted();

// Counts libc calls routed into the VFS so far (diagnostics/tests).
unsigned long RoutedCallCount();  // NOLINT

}  // namespace arc_kernel

#endif  // ARC_WASM_SHIM_KERNEL_ARC_KERNEL_H_
