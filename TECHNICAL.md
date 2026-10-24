# arc-wasm

Port of Google's ARC (App Runtime for Chrome) to WebAssembly, with Dalvik
from AOSP 4.4 on top: Java programs in DEX format run in the browser
(Chrome and Safari), with the first slice of the Android framework
(`android.util.Log`, `android.os.Looper`/`Handler`/`MessageQueue`). It started with `posix_translation`. posix_translation is ARC's userspace "Linux kernel": a VFS with
Android-specific devices (ashmem, alarm, logd), unix sockets, epoll, mmap
bookkeeping, and a read-only system image reader. ARC ran it on NaCl and
Pepper; this tree runs it under Emscripten with pthreads.

## Layout

| Path | Contents |
|---|---|
| `upstream/src/` | ARC sources from tag `arc-runtime-54.5021.668.0` (`posix_translation/`, `common/`, `ppapi_mocks/`). The first commit is the pristine import; `git diff` against it shows every local change. |
| `shim/base/` | Minimal stand-in for the 2014 chromium `base` APIs ARC uses (scoped_ptr, Lock, ConditionVariable, TimeTicks, StringPrintf, ...). |
| `shim/ppapi/` | Just enough Pepper types for the VFS core: `pp::Module/Core` (main-thread detection), `PP_FileInfo`, completion callbacks, a value-type `pp::NetAddress`. No browser behind it. |
| `shim/common/` | ARC headers missing from the open-source release (`alog.h`, `thread_priorities.h`, `scoped_pthread_mutex_locker.h`), logging to stderr, tracing disabled. |
| `shim/native_client/` | NaCl IRT replacements: random via `getentropy`, futex via Emscripten Atomics, `real_*` syscalls via libc. |
| `shim/posix_translation/` | Network-less `TCPSocket` / `UDPSocket` / `HostResolver` (sockets can be created, network operations fail with `ENETUNREACH`; `getnameinfo` and numeric `getaddrinfo` work), and an `Initialize()` without IRT hooks. |
| `shim/wasm_mm/` | Page-granular `mmap`/`munmap`/`mprotect`/`msync`/`madvise`/`mremap` replacing Emscripten's (see below). |
| `shim/linux/`, `shim/asm/`, `shim/nacl_stat.h`, `shim/arc_wasm_compat.h` | Android kernel UAPI headers (ashmem, alarm, sync, futex) missing from Emscripten's sysroot, and the transitive includes bionic provided but musl does not (force-included). |
| `shim/ppapi_mocks/` | `PpapiTest` / `BackgroundThread` test fixtures without gmock'ed Pepper interfaces. |
| `tools/make_test_fs_image.py` | Python 3 port of ARC's test read-only FS image generator. |

Headers under `shim/` take precedence over `upstream/src/` on the include
path, which is how Pepper-specific headers are replaced without editing ARC.

## What is not built

- Pepper-backed handlers: `pepper_file`, `tcp_socket`, `udp_socket`,
  `host_resolver`, `external_file`, `crx_file`, `nacl_manifest_file`, and
  their tests. In the browser these become OPFS and a WebSocket proxy.
- The NaCl IRT hook layer (`file_wrap.cc`, `initialization.cc`) and the
  `*_wrap.cc` libc entry points. Routing libc into the VFS is the next step.
- `ReadonlyFsReaderTest.TestParseImageProd` is skipped: the production image
  is built from Android binaries that are not part of the open-source tree.

## Upstream edits

Six small, commented edits (`git diff -- upstream` shows all 28 lines):

- `virtual_file_system_test.cc`, `readonly_fs_reader_test.cc`: no Pepper
  socket mocks; production image optional (guarded by `ARC_WASM`).
- `socket_util.cc`: compare `socklen_t` as `int`. Bionic (the ABI ARC and
  Android apps target) defines it as `int`, musl as `unsigned`, so negative
  lengths slipped through as `EFAULT` instead of `EINVAL`.
- `readonly_fs_reader.cc`: `uint32_t` instead of `size_t` (they differ on
  wasm32).
- `file_stream.cc`, `arc_strace.cc`: musl aliases `F_*LK64` to `F_*LK`
  (duplicate `case` values); `<sys/file.h>` for `LOCK_*`.

## Two wasm-specific pieces

**mmap.** Emscripten's `mmap` sits on `malloc` and supports neither partial
`munmap` nor `MAP_FIXED`; ashmem and the VFS need both. `shim/wasm_mm`
tracks every 64 KiB page: partial unmaps work (a heap block is freed when its
last page goes), `MAP_FIXED` works over pages it owns, `MAP_SHARED` file
pages are written back on `msync`/`munmap`. Protection is recorded but cannot
be enforced in wasm, and `MAP_FIXED` at arbitrary addresses is impossible
(those pages belong to `malloc`).

**Threads.** Emscripten reclaims a finished thread's stack on the main JS
thread's event loop. When `main()` blocks that thread (as every
`TEST_BACKGROUND_F` does), each thread leaks its 1 MiB stack, about 60 MB
per suite run. The test binary therefore links with `-sPROXY_TO_PTHREAD`:
`main()` runs on a worker and the real main thread stays free. The browser
build will need the same arrangement, matching ARC's own rule that blocking
calls never run on the main thread.

## libc -> VFS: the kernel layer

`shim/kernel/arc_kernel.cc` makes posix_translation the "kernel" for
application code. Under NaCl, ARC hooked the IRT (NaCl's syscall table).
The wasm equivalent is Emscripten's syscall layer: the file defines the
`__syscall_*` and WASI `fd_*` functions that musl calls (about 50: files,
directories, pipes, sockets, epoll, poll) plus libc's `mmap` family, and
routes them into `VirtualFileSystem`. `fopen`, `opendir`, `std::ifstream`,
`mmap`, `pipe`, `socketpair`, `ioctl` on `/dev/ashmem`... reach ARC's VFS
unmodified.

- **Host fallthrough.** The original Emscripten implementations stay
  reachable under other C names bound to the same import names (e.g.
  `import_name("__syscall_openat")`). Calls go to the host before `Boot()`,
  on the browser main thread, and while the kernel itself is running.
- **Zero-timeout polls.** Emscripten 6's musl sends `poll` and
  `epoll_wait` with timeout 0 to their own imports
  (`__syscall_poll_nonblocking`, `__syscall_epoll_pwait_nonblocking`);
  those are routed too, or such probes would see Emscripten's fd table.
- **Kernel mode.** A thread-local depth counter is set for the duration of
  each routed call. posix_translation's own libc calls (`HostFileHandler`
  opening the image, `PassthroughStream` writing to the real stdout, the
  page allocator behind anonymous mappings) therefore reach the host instead
  of recursing into the VFS, and never deadlock on its non-recursive mutex.
- **`arc_kernel::Boot()`** creates the VFS (fds 0-1023), registers stdio as
  passthrough streams, mounts `/dev/{null,zero,urandom,random,ashmem,alarm}`
  and starts routing. Callers mount the rest, e.g. `/system` via
  `ReadonlyFileHandler` over `HostFileHandler`.
- The page allocator in `shim/wasm_mm` is exported as `__wasm_mm_*`.
  Builds without the kernel bind libc's `mmap` to it directly through
  `shim/wasm_mm/libc_mm_host.cc`, as the posix_translation unit tests do.

`make kernel-test` runs `shim/kernel/arc_kernel_test.cc` in Node. All 18
tests pass (stable over `--gtest_repeat=10 --gtest_shuffle`), covering stdio,
iostreams, open/read/lseek/fstat, stat errors, read-only `/system`,
readdir, chdir with relative paths, file and anonymous `mmap` (partial
`munmap`), pipe + poll, epoll on a pipe (timeouts 0, 10 ms, none), AF_UNIX socketpair, AF_INET without network, `/dev`
devices, and ashmem `ioctl` + shared `mmap`, dup/dup2/fcntl. Roughly 900
libc calls per run are routed, gtest's own output included. The demo reads
`/system` with plain `fopen`/`fread`.

Not routed yet: the `*at()` calls with a real directory fd (`-ENOTSUP`) and
`chmod`/`fchmod`/`utimensat`.

## Writable /data on OPFS

`posix_translation::OpfsHandler` (`shim/posix_translation/opfs_file.h`,
`shim/opfs/`) is persistent, writable storage in the browser's Origin
Private File System. It is the wasm counterpart of ARC's Pepper-backed
`PepperFileHandler` (the HTML5 file system), mounted at `/data`. Files live
under the OPFS directory `arc-wasm/`.

OPFS is asynchronous, except `FileSystemSyncAccessHandle`, which only works
on the worker that created it. All OPFS work therefore runs on one
dedicated pthread that idles in its event loop
(`shim/opfs/opfs_bridge.cc`). A caller on any other thread proxies a
request there with `emscripten_proxy_sync_with_ctx()` and blocks;
`library_arc_opfs.js` performs the async work and calls
`emscripten_proxy_finish()`. The thread needs
`emscripten_runtime_keepalive_push()`, or Emscripten treats it as exited
and cancels requests in flight. Each file keeps one shared sync access
handle (OPFS allows one per file) with a reference count. Data is copied
through private buffers, so the code does not depend on OPFS accepting views
of shared wasm memory. Directory listings go through ARC's
`DirectoryManager`, which gives `.`/`..` and inode numbers consistent with
the rest of the VFS. Writable `MAP_SHARED` mappings are written back on
`munmap`.

Limitations (from OPFS): no permissions, owners or links; directories have
no mtime; an open file cannot be unlinked or renamed (`EBUSY`); renaming
needs `FileSystemHandle.move()`.

`make opfs-test` builds a browser test page (`build/opfs-test/`; serve
`build/` with `tools/serve.py`, or use the `arc-wasm-tests` preview config
on port 8124). 12 tests cover stdio and iostreams, `O_APPEND`, pread/pwrite
and holes, truncate, mkdir/readdir/rmdir, rename (including replacing),
errno values, `EBUSY` on unlinking an open file, two descriptors on one
file, shared `mmap` write-back, and a 1 MB file. They pass in Chrome and
stay stable over 5 shuffled repeats. `?arg=--opfs-phase=write`, then a new
page load with `?arg=--opfs-phase=verify`, checks persistence across
reloads; it passes too.

The demo mounts `/data` and keeps a launch counter in
`/data/arc_demo/launches` with `fopen`/`fscanf`/`fprintf`. The counter goes
up on every reload.

## Status

387 tests in 34 suites: 386 pass and 1 is skipped (`TestParseImageProd`).
The result is stable over `--gtest_repeat=10 --gtest_shuffle`. Heap after a
full run stays around 1.3 MB, growing by one 64 KiB mmap page per repeat
(not yet traced; likely a test that skips munmap by design, such as
`TestNoMunmap`). A run takes about 0.3 s under Node.js 24
with Emscripten 6.0.12.

`build/posix_translation_test.js` prints a `[ MEMORY ]` line for any test
that grows the heap or mmap by 256 KiB or more, plus a summary per iteration.

## graphics_translation on WebGL2

`make demo` builds one threaded runtime into `build/demo/`: ARC's EGL +
GLES 1.1/2.0 implementation (`upstream/mods/graphics_translation`: egl,
gles, gralloc), posix_translation, and a GLES 1.1 demo app. Serve it with
`tools/serve.py PORT build/demo` (or the `arc-wasm-demo` preview config,
port 8123). Wasm threads need `SharedArrayBuffer`, which needs the
COOP/COEP headers that script sends.

- `main()` runs on a pthread (`-sPROXY_TO_PTHREAD`). The canvas is
  transferred to it as an OffscreenCanvas, so GL, the VFS and the app share
  one worker and the browser main thread only presents frames.
- `/system` is an Android image in ARC's read-only format
  (`tools/make_system_image.py`: `build.prop` and a texture), embedded into
  the wasm file system. It is mounted in ARC's `VirtualFileSystem` through
  `ReadonlyFileHandler` over the new `HostFileHandler`
  (`shim/posix_translation/host_file.*`, the wasm counterpart of ARC's
  NaCl manifest handler). The demo prints `/system/build.prop` and reads
  its texture with VFS `open`/`read`. The image itself is parsed through
  `mmap`, i.e. `shim/wasm_mm`.

![GLES 1.1 demo](demo/screenshot.png)

The demo takes the same path an Android app took under ARC: GLES 1.1 calls
into ARC's GLES, whose fixed-function emulation generates GLES2 shaders.
EGL then renders into an `ANativeWindow`. `demo/gles1_demo.cpp` implements
that window like Android's `FramebufferNativeWindow`: two buffers from ARC's
gralloc HAL, posted to the framebuffer device on swap, which ends in
`ColorBuffer::Render` drawing a full-screen quad onto the canvas.

New pieces:

- `shim/graphics_translation/egl/native_webgl.cpp` replaces
  `native_pepper.cpp`. Pepper gave ARC one Graphics3D per EGL context in a
  shared group; WebGL contexts cannot share objects. So every host context
  is a **virtual context** on a single WebGL2 context. Each one owns a VAO,
  and the rest of the global GL state is saved and restored on switch,
  which is how Chromium handles drivers without working share groups. It
  also emulates `CHROMIUM_map_sub` (staging buffer + `glTexSubImage2D`).
- `shim/graphics_translation/gles/pass_through.h`, generated by
  `tools/gen_pass_through.py` (ARC generated it from Pepper IDL).
  `PASS_THROUGH(ctx, Name, ...)` switches to `ctx`'s virtual context and calls
  `emscripten_glName`. ARC defines `glName` itself, for apps, so host GL
  must be reached through Emscripten's `emscripten_gl*` aliases.
- Android headers: AOSP 5.0.2 `system/window.h`, `hardware/gralloc.h`,
  `hardware/fb.h`, `cutils/native_handle.h`, ETC1 (fetched into
  `third_party/aosp`). Small shims cover `utils/RefBase.h` (strong refs
  only), `Timers.h`, `Errors.h`, `ui/PixelFormat.h`, `sys/cdefs.h`, Android's
  `EGL/eglplatform.h` (window = `ANativeWindow*`), and libyuv's one routine.
- Upstream edit: one `size_t` -> `GLuint` cast in `gles/gles_context.cpp`.
  (`upstream/mods/` is not committed yet, so `git diff` cannot show it.)

ARC already copies client-side vertex and index arrays into buffer objects,
because Chromium's command buffer could not read client memory either. So
WebGL's lack of client arrays costs nothing, and Emscripten's `FULL_ES2`
emulation is not used.

Status: renders correctly in Chrome at display refresh rate (~120 fps on a
120 Hz screen), with `crossOriginIsolated` and GL on a worker. File access
goes through libc, routed into the VFS by the kernel layer below. No
automated graphics tests yet: ARC's gles tests (`*_test.cpp`) are not built.

## Dalvik: Java apps' runtime

`make dalvik` builds Dalvik from AOSP 4.4.4 (the runtime ARC Welder used)
on top of the kernel layer: Dalvik's portable C interpreter, libdex,
libcore's natives (libjavacore), libnativehelper, ICU 51 and fdlibm.
`build/dalvik/dalvikvm.js` runs under Node; `make dalvik-web` builds the
same for browsers into `build/dalvik-web/`.

```bash
tools/fetch_aosp44.sh   # AOSP sources into third_party/aosp44, patched
make dalvik-hello       # Hello.java on Dalvik under Node
cd build/dalvik && node dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.VmTest
```

In a browser: `tools/serve.py 8125 build`, then open
`http://localhost:8125/dalvik-web/index.html` (runs `arcwasm.VmTest`; other
`dalvikvm` arguments go in the URL as `?arg=...&arg=...`).

- **Java side.** `tools/build_core_jar.sh` compiles libcore's Java sources
  (`core.jar`: dalvik, dex, dom, json, luni, xml, libdvm) with javac
  (source/target 8) and converts them with d8 (`--min-api 19`). d8's
  desugaring moves the default-method bridges javac 8+ puts into interfaces
  into the implementing classes. Test programs in `tests/java/` become
  `arc-tests.jar`. Both go into the `/system` image with ICU's data file.
- **JNI calls** (`shim/dalvik/wasm_call.cc`). Dalvik's generic port calls
  native methods through libffi. Here the call goes through JavaScript:
  the function is taken from the wasm table and called with arguments
  decoded from the method's shorty. JS-to-wasm calls coerce arguments, so
  any C signature works, while a wasm `call_indirect` through the wrong
  type traps.
- **Function types must match.** For the same reason internal natives (two
  parameters, called as four-parameter bridges) go through a typed bridge
  in `Native.cpp`, and a few AOSP declarations that disagreed with their
  definitions were fixed (see `patches/aosp44.patch`).
- **No dexopt.** dexopt runs in a `fork()`ed process. `JarFile.cpp` now
  extracts `classes.dex` into memory and prepares it in place, as Dalvik
  already did for `DexClassLoader` byte arrays. Classes are verified at load
  time and the bytecode is not optimized (no quickening, no register maps,
  so the GC scans interpreter frames conservatively).
- **No dlopen.** libjavacore is linked in; `shim/dalvik/static_jni_libs.cc`
  answers Dalvik's `dlopen("libjavacore.so")` with its `JNI_OnLoad`.
- **Platform bits.** 64 KiB pages (`SYSTEM_PAGE_SIZE`, what `sysconf`
  reports), `cutils` atomics on compiler builtins, no zygote, no signal
  catcher (`-Xrs`), password entries synthesized like bionic does,
  `sendfile` as a read/write loop.
- Not built yet: `NativeBN` (`BigInteger`, needs OpenSSL), the Expat XML
  parser, raw packet sockets.

`tests/java/arcwasm/VmTest.java` checks arithmetic, recursion and stack
overflow, exceptions, strings, charsets and `String.format` (ICU),
regex, collections, reflection, threads with `wait`/`notify`, GC and weak
references, zlib, NIO buffers and file I/O. Results: 16/16 under Node
(stable over 10 runs, ~0.3 s), 17/17 in Safari 26.3 and Chromium, where
the extra check writes and reads a file in `/data` on OPFS.

## Android framework: first slice

The framework comes up the way ARC did it: real AOSP 4.4.4 framework
classes and their native layer, built for wasm, rather than a
reimplementation. The first slice is the message loop and logging, built
by `mk/framework.mk` into `dalvikvm`:

- **libutils** (`system/core/libutils`, the host source list plus
  `Looper.cpp`). `Looper` waits in `epoll_wait` on posix_translation's
  epoll and wakes through a pipe, exactly as on a device.
- **libandroid_runtime JNI** for `android.util.Log` (to liblog; levels from
  `log.tag.*` properties via libcutils' host `properties.c`),
  `android.os.SystemClock` and `android.os.MessageQueue`, unmodified apart
  from one host-path include.
- **`shim/framework/`**: `AndroidRuntime`'s static helpers; a `gRegJNI`
  table that `dalvikvm` runs right after `JNI_CreateJavaVM` (as
  `AndroidRuntime::startReg` does in `app_process`); `android.os.Binder`
  natives for a process without a binder driver (the calling identity is
  this process, per thread, so `Looper.loop()`'s
  `clearCallingIdentity`/`restoreCallingIdentity` pair up); and
  `android.os.Parcel` natives for plain data in libbinder's wire format, so
  `Bundle` and `Message` survive `marshall`/`unmarshall` (binder objects
  only as null, no file descriptors) until libbinder is built.
- **`framework-core.jar`** (`tools/build_framework_jar.sh`, javac 8 + d8
  like `core.jar`): the AOSP sources listed in
  `framework/framework-core.list` (Looper, Handler, HandlerThread, Message,
  MessageQueue, SystemClock, Log, Bundle, Parcel, Binder, ArrayMap, ...),
  compiled with empty source and class paths so that nothing else is pulled
  in, plus hand-written stand-ins in `framework/java-stubs/` for the classes
  whose real versions drag in most of the framework (`Process`,
  `StrictMode`, `TextUtils`, `TimeUtils`, `RuntimeInit`,
  `ParcelFileDescriptor`, `IMessenger`). It is in `/system/framework` and on
  the default `BOOTCLASSPATH`.

Two wasm pitfalls met here: wasm-ld runs an archive member's static
constructors only if something in that member is live (libutils'
`Static.cpp`, which creates the empty `String8`/`String16`, was dropped;
`register_framework.cc` keeps it), and `char16_t` is a C++11 keyword, so
`utils/Unicode.h` no longer typedefs it. The JNI registration table holds
`int (*)(JNIEnv*)` functions only, since a call through a pointer of another
type traps.

`tests/java/arcwasm/LooperTest.java` checks Log, SystemClock, Binder
identity, a `HandlerThread` (posts, ordered messages with arguments and
`Bundle` data, `postDelayed` ordering and timing, `removeMessages`,
`quitSafely`), `Message` pooling, a `Message` through `Parcel`, and a
`Looper` on the main thread woken from another thread while blocked in
epoll. 11/11 under Node (stable over 10 runs, ~0.8 s, mostly the test's own
delays), in Chromium and in Safari 26:

```bash
cd build/dalvik && node dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.LooperTest
# browser: http://localhost:8125/dalvik-web/index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.LooperTest
```

## A real Activity on the page

`make app-web`, then open `http://localhost:8125/graphics-web/app.html`
(served by `tools/serve.py 8125 build`): the in-process system_server
(`arcwasm.server.SystemServer`) installs the test APK with `PackageParser`
and starts its launcher Activity through `startActivity`, the way a
launcher does. The Activity's window is drawn by `ViewRootImpl` into a
Surface over ARC's gralloc and composited onto the canvas; clicks reach
its `OnClickListener` through the window's `InputChannel`. `?selftest=1`
taps the button with synthetic DOM events (passes in Chromium and Safari).
Topic notes, in Russian: `docs/topics/*.md`; the overall report is
`docs/HANDOFF.md`.

![MainActivity after three clicks](docs/app-chromium.png)

## Build and test

Requires [emsdk](https://emscripten.org) in `~/emsdk` (override with
`EMSDK=...`) and Node.js.

```bash
make test          # posix_translation unit tests (Node)
make kernel-test   # libc -> VFS routing tests (Node)
make opfs-test     # OPFS /data tests: build/opfs-test/, run in a browser
make demo          # GLES 1.1 demo: build/demo/, run in a browser
make dalvik        # Dalvik for Node (needs tools/fetch_aosp44.sh first)
make dalvik-web    # Dalvik for browsers: build/dalvik-web/
```

Browser builds need cross-origin isolation: serve them with
`tools/serve.py PORT DIR [LOGFILE]`. Pages POST their output to the server's
`/__log`, which prints it (and appends it to LOGFILE), so runs in browsers
that cannot be automated, like Safari, can still be checked. Everything
here was tested in Safari 26.3 as well as Chromium.

The Dalvik build also needs a JDK (javac 8 or newer) and the Android SDK's
d8 (`~/Library/Android/sdk/build-tools/*/d8`, or set `D8=`).

Pass gtest flags with `GTEST_ARGS`, e.g.
`make test GTEST_ARGS=--gtest_filter='FileSystemTest.*'`.
