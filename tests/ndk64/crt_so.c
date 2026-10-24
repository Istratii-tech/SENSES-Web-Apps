/* What the NDK's crtbegin_so.o gives a shared library and the arm64 test
 * libraries need (no NDK here): __dso_handle, the library's identity for
 * __cxa_atexit (static destructors). */
void* __dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;
