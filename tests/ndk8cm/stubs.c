// Link-time stand-ins for libmediandk.so and libaaudio.so, which the NDK r9
// sysroot does not have: built with those sonames so that libndk8cmtest.so
// names them in DT_NEEDED. At run time the NDK bridge's host libraries
// answer (shim/ndk/ndk_media8.cc). One file for both: each stand-in library
// defines every name; the dynamic linker of the bridge looks names up by
// symbol, not by library.
void AMediaExtractor_new(void) {}
void AMediaExtractor_delete(void) {}
void AMediaExtractor_setDataSourceFd(void) {}
void AMediaExtractor_getTrackFormat(void) {}
void AMediaExtractor_selectTrack(void) {}
void AMediaExtractor_readSampleData(void) {}
void AMediaExtractor_getSampleFlags(void) {}
void AMediaExtractor_getSampleTime(void) {}
void AMediaExtractor_advance(void) {}
void AMediaExtractor_seekTo(void) {}
void AMediaFormat_getString(void) {}
void AMediaFormat_getInt32(void) {}
void AMediaFormat_getInt64(void) {}
void AMediaFormat_delete(void) {}
void AMediaCodec_createDecoderByType(void) {}
void AMediaCodec_configure(void) {}
void AMediaCodec_start(void) {}
void AMediaCodec_stop(void) {}
void AMediaCodec_delete(void) {}
void AMediaCodec_dequeueInputBuffer(void) {}
void AMediaCodec_getInputBuffer(void) {}
void AMediaCodec_queueInputBuffer(void) {}
void AMediaCodec_dequeueOutputBuffer(void) {}
void AMediaCodec_releaseOutputBuffer(void) {}
const char* AMEDIAFORMAT_KEY_MIME;
const char* AMEDIAFORMAT_KEY_WIDTH;
const char* AMEDIAFORMAT_KEY_HEIGHT;
const char* AMEDIAFORMAT_KEY_DURATION;
void AAudio_createStreamBuilder(void) {}
void AAudio_convertResultToText(void) {}
void AAudio_convertStreamStateToText(void) {}
void AAudioStreamBuilder_setFormat(void) {}
void AAudioStreamBuilder_setChannelCount(void) {}
void AAudioStreamBuilder_setDataCallback(void) {}
void AAudioStreamBuilder_openStream(void) {}
void AAudioStreamBuilder_delete(void) {}
void AAudioStream_requestStart(void) {}
void AAudioStream_getState(void) {}
void AAudioStream_getTimestamp(void) {}
void AAudioStream_waitForStateChange(void) {}
void AAudioStream_getSampleRate(void) {}
void AAudioStream_getFramesPerBurst(void) {}
void AAudioStream_close(void) {}
