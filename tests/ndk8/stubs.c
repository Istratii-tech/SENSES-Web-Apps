// Link-time stand-ins for the system libraries of API 26 that the NDK r9
// sysroot does not have (libsync, libnativewindow, libmediandk): built with
// their sonames so that libndk8test.so names them in DT_NEEDED, as
// Firefox's libxul does. At run time the NDK bridge's host libraries answer.
void sync_merge(void) {}
void AHardwareBuffer_allocate(void) {}
void AHardwareBuffer_release(void) {}
void AHardwareBuffer_describe(void) {}
void AHardwareBuffer_lock(void) {}
void AHardwareBuffer_unlock(void) {}
void AMediaCodec_createDecoderByType(void) {}
void AMediaFormat_new(void) {}
const char* AMEDIAFORMAT_KEY_MIME;
