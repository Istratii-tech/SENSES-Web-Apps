# Topic "fw8-crypto-media": cryptography, XML, sound and media of the
# Android 8.1 framework on ART 8.1, and the NDK media libraries for apps
# (docs/topics/fw8-crypto-media.md). What topics fw6-crypto, gles-audio and
# mediacodec did for 6.0.1, for 8.1:
#
#   crypto   BoringSSL 8.1's libssl (libcrypto is topic art8's libcrypto8.a),
#            conscrypt 8.1's natives (libjavacrypto: org.conscrypt.NativeCrypto
#            behind the "AndroidOpenSSL" provider; the Java half is topic
#            fw8-core's conscrypt.jar), /system/etc/security/cacerts.
#            NativeBN (BigInteger) and ExpatParser (SAX) are in topic art8's
#            libjavacore8.a already.
#
#   sound    AOSP 8.1's AudioTrack, AudioRecord and DeviceCallback JNI,
#            unchanged, over topic gles-audio's lean AudioTrack (shim/audio,
#            with its 8.1 API under ARC_WASM_FW8) and in-process mixer, whose
#            sink is the page's AudioWorklet (Node: a clock); AudioRecord is
#            a device without an input (shim/fw8cm/audio_record8.cc);
#            AudioSystem's natives are ours (shim/fw8cm/audio_system8_jni.cc).
#
#   make fw8cm-libs        # the libraries
#   make fw8cm-test        # this topic's tests on ART 8.1 in Node
#
# Objects in build/fw8cm/obj, libraries in build/fw8cm/lib. Included after
# mk/topic-art8.mk and mk/topic-fw8-core.mk (alphabetical order): A8*, F8*,
# FW8_INCLUDES, ART8_LIBS, ASYSTEM8_ADD and ART8_JAVA_TESTS are defined.

FCM    := $(BUILD)/fw8cm
FCMOBJ := $(FCM)/obj
FCMLIB := $(FCM)/lib
fcmobj  = $(patsubst %,$(FCMOBJ)/%.o,$(1))

# Two kinds of sources: ART/libcore-side ones (topic art8's flags, A8_*) and
# framework-side ones (topic fw8-core's, F8_* with FW8_INCLUDES). FCM_C and
# FCM_CXX pick the base per target; FCMFLAGS adds to it.
FCM_C   = $(F8_CFLAGS) $(F8_FLAGS)
FCM_CXX = $(F8_CXXFLAGS) $(F8_FLAGS)
$(FCMOBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(FCM_C) $(FCMFLAGS) -c $< -o $@
$(FCMOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FCM_CXX) $(FCMFLAGS) -c $< -o $@
$(FCMOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FCM_CXX) $(FCMFLAGS) -c $< -o $@
-include $(shell find $(FCMOBJ) -name '*.d' 2>/dev/null)

ifneq ($(A8LIB),)

# === crypto ==========================================================================

# --- libssl (BoringSSL 8.1) ------------------------------------------------------------
# Same flags as topic art8's libcrypto8.a (BSSL8_INCLUDES: <openssl/base.h>
# treats wasm32 as its 32-bit __myriad2__ target, no CPU-specific code).
FCM_SSL_SRCS := $(call bp,$(BSSL8)/sources.bp,libssl_sources)
$(call fcmobj,$(FCM_SSL_SRCS)): FCM_CXX = $(A8_CXXFLAGS)
$(call fcmobj,$(FCM_SSL_SRCS)): FCMFLAGS := $(BSSL8_INCLUDES) -DOPENSSL_NO_ASM \
    -DBORINGSSL_IMPLEMENTATION -DOPENSSL_SMALL -DBORINGSSL_ANDROID_SYSTEM
$(FCMLIB)/libssl8.a: $(call fcmobj,$(FCM_SSL_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libjavacrypto (conscrypt 8.1) ------------------------------------------------------
# As AOSP's libjavacrypto (external/conscrypt/Android.mk): classes are
# com.android.org.conscrypt (JNI_JARJAR_PREFIX, the jarjar of conscrypt.jar).
# Linked in: its JNI_OnLoad is renamed and added to topic art8's table of
# static JNI libraries (shim/fw8cm/javacrypto8_lib.cc); System.loadLibrary
# wants an empty file in /system/lib. Its include directory comes first: it
# has a JniConstants.h of its own, as libnativehelper does. ANDROID as for
# the device: libcore's FileDescriptor field "descriptor" (not OpenJDK's
# "fd"), AttachCurrentThread(JNIEnv**), liblog. (Its compat.h undefines
# _GNU_SOURCE for <string.h>; musl's then lacks strcasecmp, which bionic's
# <string.h> gets from <strings.h>.)
FCM_CONSCRYPT := $(A81)/external/conscrypt/common/src/jni/main
FCM_JCRYPTO_SRCS := $(wildcard $(FCM_CONSCRYPT)/cpp/*.cpp) shim/fw8cm/javacrypto8_lib.cc
$(call fcmobj,$(FCM_JCRYPTO_SRCS)): FCM_CXX = $(A8_CXXFLAGS)
$(call fcmobj,$(FCM_JCRYPTO_SRCS)): FCMFLAGS := -I$(FCM_CONSCRYPT)/include $(A8_SYS_FLAGS) \
    $(BSSL8_INCLUDES) -I$(A81)/libcore/include -I$(A81)/libcore/luni/src/main/native \
    '-DJNI_JARJAR_PREFIX=com/android/' -DJNI_OnLoad=arc_wasm_javacrypto8_JNI_OnLoad -DANDROID \
    -include strings.h
$(FCMLIB)/libjavacrypto8.a: $(call fcmobj,$(FCM_JCRYPTO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

FCM_CRYPTO_LIBS := $(FCMLIB)/libjavacrypto8.a $(FCMLIB)/libssl8.a

# === sound ===========================================================================
# ARC_WASM_FW6 and ARC_WASM_FW8: the lean AudioTrack with its 6.0 and 8.1 API
# (in every file that includes its header: the class layout depends on it).
# shim/fw8cm/include (AudioRecord) and shim/audio/include come before
# frameworks/av's headers (VolumeShaper.h, AudioResamplerPublic.h,
# AudioTimestamp.h are 8.1's own).
FCM_AUDIO_INCLUDES := -Ishim/fw8cm/include -Ishim/audio/include -Ishim/audio \
    -I$(A81)/frameworks/av/include -I$(A81)/system/media/audio/include \
    -I$(A81)/system/media/audio_utils/include \
    -I$(F8_JNI) -I$(F8_JNI)/include -I$(F8FWB)/media/jni \
    -DARC_WASM_FW6=1 -DARC_WASM_FW8=1 -DLOG_NDEBUG=1
FCM_AUDIO_SRCS := $(addprefix $(F8_JNI)/,android_media_AudioTrack.cpp android_media_AudioRecord.cpp \
                    android_media_DeviceCallback.cpp) \
                  shim/audio/audio_flinger.cc shim/audio/audio_track.cc \
                  shim/fw8cm/audio_record8.cc shim/fw8cm/audio_system8_jni.cc
$(call fcmobj,$(FCM_AUDIO_SRCS)): FCMFLAGS := $(FCM_AUDIO_INCLUDES)
$(FCMLIB)/libaudio8.a: $(call fcmobj,$(FCM_AUDIO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- /system --------------------------------------------------------------------------
$(FCM)/empty.so:
	@mkdir -p $(dir $@) && : > $@
# The CA store of conscrypt's TrustManager: the build host's roots, as in
# the 4.4 and 6.0.1 images (mk/topic-net.mk builds them; AOSP 8.1's own set
# in system/ca-certificates is from 2017).
FCM_CACERTS := $(BUILD)/dalvik/cacerts
ASYSTEM8_ADD += --add $(FCM)/empty.so:system/lib/libjavacrypto.so \
                --add $(FCM)/empty.so:system/lib/libmedia_jni.so \
                --add $(FCM_CACERTS):system/etc/security/cacerts
$(A8SYSTEM_IMAGE): $(FCM)/empty.so $(FCM_CACERTS).stamp

# === media ===========================================================================
# What topic mediacodec did for 6.0.1 (mk/topic-mediacodec.mk), for 8.1:
# libstagefright_foundation 8.1, the extractors of 8.1's libstagefright
# (MediaExtractor.cpp itself: extraction in process, the property
# media.stagefright.extractremote is false), the parts of libmedia they and
# the JNI need, and shim/media -- MediaCodec, MediaCodecList, MediaPlayer on
# WebCodecs -- built for 8.1 (ARC_WASM_FW8: MediaCodecBuffer, 8.1's
# MediaCodecInfo) under AOSP 8.1's libmedia_jni, unchanged.
FCM_AV := $(A81)/frameworks/av
FCM_SF := $(FCM_AV)/media/libstagefright
FCM_WEBM := $(A81)/external/libvpx/libwebm
# shim/media/include first (our MediaCodec.h, MediaCodecList.h,
# mediaplayer.h over frameworks/av's), then the sound includes (lean
# AudioTrack/AudioSystem, AudioRecord), frameworks/av's (its include/media
# links to every library's headers), libgui's and the HAL headers.
# (shim/fw8cm/include, in FCM_AUDIO_INCLUDES, also has DataURISource.h:
# frameworks/av 8.1 has it and DataUriSource.h, which a case-insensitive
# file system unpacks into one file.)
FCM_MEDIA_INCLUDES = -Ishim/media/include $(FCM_AUDIO_INCLUDES) \
    -I$(FCM_SF)/foundation/include -I$(FCM_SF)/include \
    -I$(FCM_AV)/media/libmediametrics/include -I$(FCM_AV)/media/libmedia/aidl \
    -I$(A81)/frameworks/native/include/media/openmax \
    -I$(A81)/frameworks/native/headers/media_plugin \
    $(addprefix -I$(A81)/frameworks/native/libs/,gui/include ui/include nativewindow/include \
      nativebase/include arect/include) \
    -I$(A81)/hardware/libhardware/include -I$(FCM_HIDL) -I$(F8_HIDL_GENCPP) \
    -include shim/media/media_compat.h -Wno-sign-compare -Wno-unused-variable
# C++ of the HIDL interfaces media's headers name, from topic fw8-core's
# hidl-gen: headers of OMX and what it imports (constants and types only:
# nothing here talks to an OMX HAL), headers and sources of CAS (ATSParser
# and the MediaDescrambler/MediaCas JNI hold IDescrambler/ICas objects) and
# of android.hidl.token (libhidl's HybridInterface: tokens of HAL objects;
# no token manager runs, so none is ever made or found).
FCM_HIDL := $(FCM)/gen/hidl-cpp
FCM_HIDL_HDR_PKGS := android.hardware.graphics.common@1.0 android.hardware.media@1.0 \
    android.hardware.graphics.bufferqueue@1.0 android.hardware.media.omx@1.0
FCM_HIDL_SRC_PKGS := android.hardware.cas@1.0 android.hardware.cas.native@1.0 android.hidl.token@1.0
FCM_HIDL_SRCS := $(FCM_HIDL)/android/hardware/cas/1.0/types.cpp \
    $(addprefix $(FCM_HIDL)/android/hardware/cas/1.0/,CasAll.cpp CasListenerAll.cpp \
      DescramblerBaseAll.cpp MediaCasServiceAll.cpp) \
    $(addprefix $(FCM_HIDL)/android/hardware/cas/native/1.0/,types.cpp DescramblerAll.cpp) \
    $(FCM_HIDL)/android/hidl/token/1.0/TokenManagerAll.cpp
FCM_HIDL_GEN = $(F8H)/hidl-gen -o $(FCM_HIDL) -randroid.hardware:$(A81)/hardware/interfaces \
    -randroid.hidl:$(A81)/system/libhidl/transport
$(FCM_HIDL)/hidl.stamp: $(F8H)/hidl-gen mk/topic-fw8-crypto-media.mk
	rm -rf $(FCM_HIDL) && mkdir -p $(FCM_HIDL)
	for p in $(FCM_HIDL_HDR_PKGS) $(FCM_HIDL_SRC_PKGS); do \
	  $(FCM_HIDL_GEN) -Lc++-headers $$p || exit 1; \
	done
	for p in $(FCM_HIDL_SRC_PKGS); do $(FCM_HIDL_GEN) -Lc++-sources $$p || exit 1; done
	@touch $@
$(FCM_HIDL_SRCS): $(FCM_HIDL)/hidl.stamp ;
$(call fcmobj,$(FCM_HIDL_SRCS)): FCMFLAGS := -I$(FCM_HIDL) -I$(F8_HIDL_GENCPP)
$(FCMLIB)/libcas_hidl8.a: $(call fcmobj,$(FCM_HIDL_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

FCM_FOUNDATION_SRCS := $(addprefix $(FCM_SF)/foundation/,AAtomizer.cpp ABitReader.cpp ABuffer.cpp \
    ADebug.cpp AHandler.cpp AHierarchicalStateMachine.cpp ALooper.cpp ALooperRoster.cpp \
    AMessage.cpp AString.cpp AStringUtils.cpp ColorUtils.cpp MediaBuffer.cpp \
    MediaBufferGroup.cpp MetaData.cpp base64.cpp hexdump.cpp)
FCM_EXTRACT_SRCS := $(addprefix $(FCM_SF)/,MPEG4Extractor.cpp SampleTable.cpp SampleIterator.cpp \
    ItemTable.cpp ESDS.cpp MediaSource.cpp FileSource.cpp CallbackDataSource.cpp DataSource.cpp \
    MediaExtractor.cpp NuMediaExtractor.cpp Utils.cpp avc_utils.cpp HevcUtils.cpp \
    MP3Extractor.cpp XINGSeeker.cpp VBRISeeker.cpp WAVExtractor.cpp AACExtractor.cpp \
    AMRExtractor.cpp OggExtractor.cpp FLACExtractor.cpp id3/ID3.cpp \
    matroska/MatroskaExtractor.cpp mpeg2ts/MPEG2TSExtractor.cpp mpeg2ts/ATSParser.cpp \
    mpeg2ts/AnotherPacketSource.cpp mpeg2ts/ESQueue.cpp mpeg2ts/MPEG2PSExtractor.cpp \
    mpeg2ts/HlsSampleDecryptor.cpp mpeg2ts/CasManager.cpp DataURISource.cpp NuCachedSource2.cpp HTTPBase.cpp \
    http/MediaHTTP.cpp flac/dec/FLACDecoder.cpp) \
    $(addprefix $(FCM_AV)/media/libmedia/,IMediaHTTPConnection.cpp IMediaHTTPService.cpp \
    IDataSource.cpp IMediaExtractor.cpp IMediaExtractorService.cpp IMediaSource.cpp \
    MediaCodecInfo.cpp IMediaCodecList.cpp IStreamSource.cpp MediaCodecBuffer.cpp \
    MediaDefs.cpp MediaUtils.cpp BufferingSettings.cpp) \
    $(FCM_AV)/media/libmediametrics/MediaAnalyticsItem.cpp \
    $(FCM_AV)/media/libmediametrics/IMediaAnalyticsService.cpp \
    $(FCM_WEBM)/mkvparser/mkvparser.cc $(A81)/system/media/audio_utils/primitives.c \
    $(A81)/system/libhidl/transport/token/1.0/utils/HybridInterface.cpp
$(call fcmobj,$(FCM_FOUNDATION_SRCS) $(FCM_EXTRACT_SRCS)): FCMFLAGS := $(FCM_MEDIA_INCLUDES) \
    -I$(FCM_SF) -I$(FCM_SF)/foundation/include/media/stagefright/foundation -I$(FCM_WEBM) \
    -I$(A81)/external/tremolo -I$(A81)/external/flac/include -I$(FCM_SF)/flac/dec \
    $(BSSL8_INCLUDES) -I$(A81)/system/libhidl/transport/token/1.0/utils/include
$(call fcmobj,$(FCM_FOUNDATION_SRCS) $(FCM_EXTRACT_SRCS)): $(FCM_HIDL)/hidl.stamp
$(FCMLIB)/libstagefright_foundation8.a: $(call fcmobj,$(FCM_FOUNDATION_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(FCMLIB)/libstagefright_extract8.a: $(call fcmobj,$(FCM_EXTRACT_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# Tremolo (the Vorbis headers OggExtractor parses) and libFLAC, C only.
FCM_TREMOLO_SRCS := $(addprefix $(A81)/external/tremolo/Tremolo/,bitwise.c codebook.c dsp.c floor0.c \
    floor1.c floor_lookup.c framing.c mapping0.c mdct.c misc.c res012.c treminfo.c vorbisfile.c)
$(call fcmobj,$(FCM_TREMOLO_SRCS)): FCMFLAGS := -I$(A81)/external/tremolo/Tremolo -DONLY_C \
    -funsigned-char -Wno-implicit-function-declaration
FCM_FLAC := $(A81)/external/flac
FCM_FLAC_SRCS := $(addprefix $(FCM_FLAC)/libFLAC/,bitmath.c bitreader.c bitwriter.c cpu.c crc.c \
    fixed.c float.c format.c lpc.c memory.c md5.c stream_decoder.c stream_encoder.c \
    stream_encoder_framing.c window.c)
$(call fcmobj,$(FCM_FLAC_SRCS)): FCMFLAGS := -I$(FCM_FLAC)/libFLAC/include -I$(FCM_FLAC)/include \
    -I$(FCM_FLAC) -DHAVE_CONFIG_H -DFLAC__NO_MD5 -DFLAC__INTEGER_ONLY_LIBRARY -D_REENTRANT
$(FCMLIB)/libvorbis_flac8.a: $(call fcmobj,$(FCM_TREMOLO_SRCS) $(FCM_FLAC_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- MediaCodec, MediaCodecList, MediaPlayer on WebCodecs; libmedia_jni 8.1 ---------------
# shim/media built for 8.1 (ARC_WASM_FW8) and AOSP 8.1's libmedia_jni,
# unchanged: its JNI_OnLoad (android_media_MediaPlayer.cpp) is the static
# library's entry point (shim/fw8cm/media_jni8.cc adds it to art8's table
# and registers what this build does not have as missing).
FCM_MC_SHIM_SRCS := $(addprefix shim/media/,webcodecs.cc bitstream.cc yuv.cc codecs.cc \
    media_codec_list.cc media_codec.cc media_player.cc media_drm_jni.cc drm_stubs.cc) \
    shim/fw8cm/media_jni8.cc shim/fw8cm/gui_fallback8.cc
FCM_MC_JNI := android_media_MediaCodec android_media_MediaCodecList android_media_MediaExtractor \
    android_media_Utils android_media_MediaDataSource android_media_MediaHTTPConnection \
    android_media_MediaPlayer android_media_SyncParams android_media_MediaMetricsJNI \
    android_media_MediaDescrambler
FCM_MC_JNI_SRCS := $(addprefix $(F8FWB)/media/jni/,$(addsuffix .cpp,$(FCM_MC_JNI)))
$(call fcmobj,$(FCM_MC_SHIM_SRCS) $(FCM_MC_JNI_SRCS)): FCMFLAGS = $(FCM_MEDIA_INCLUDES) \
    -I$(FCM_SF) -Ishim/media -I$(A81)/system/libhidl/transport/token/1.0/utils/include \
    -I$(A81)/frameworks/av/drm/libmediadrm/include -I$(A81)/frameworks/av/include/drm \
    -I$(A81)/external/piex -I$(A81)/system/media/camera/include \
    $(addprefix -I$(A81)/external/skia/include/,core config) -DARC_WASM_FW8_MEDIA_JNI
$(call fcmobj,$(F8FWB)/media/jni/android_media_MediaPlayer.cpp): FCMFLAGS += \
    -DJNI_OnLoad=arc_wasm_media_jni8_JNI_OnLoad
$(call fcmobj,$(FCM_MC_SHIM_SRCS) $(FCM_MC_JNI_SRCS)): $(FCM_HIDL)/hidl.stamp
$(FCMLIB)/libmedia_jni8.a: $(call fcmobj,$(FCM_MC_SHIM_SRCS) $(FCM_MC_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# === NDK media: libmediandk ===========================================================
# AOSP 8.1's libmediandk (frameworks/av/media/ndk: AMediaCodec, AMediaFormat,
# AMediaExtractor), unchanged, over shim/media's MediaCodec and 8.1's
# extractors; AMediaCrypto/AMediaDrm of a device without DRM
# (shim/fw8cm/ndk_media_drm8.cc). Plain host functions: guest code reaches
# them through the NDK bridge's table (shim/ndk/ndk_media8.cc, below).
# __USE_FILE_OFFSET64: the header's off_t arguments become int32_t, as in
# the NDK's 32-bit ABI (musl's off_t is 64 bits), so that the functions take
# what ARM code passes.
FCM_NDK_MEDIA := $(FCM_AV)/media/ndk
FCM_MEDIANDK_SRCS := $(addprefix $(FCM_NDK_MEDIA)/,NdkMediaCodec.cpp NdkMediaFormat.cpp \
                       NdkMediaExtractor.cpp) shim/fw8cm/ndk_media_drm8.cc
$(call fcmobj,$(FCM_MEDIANDK_SRCS)): FCMFLAGS = $(FCM_MEDIA_INCLUDES) -I$(FCM_NDK_MEDIA)/include \
    -I$(FCM_NDK_MEDIA) -DEXPORT='__attribute__((visibility("default")))' -D__ANDROID_API__=27 \
    -D__USE_FILE_OFFSET64 -I$(A81)/system/libhidl/transport/token/1.0/utils/include
$(call fcmobj,$(FCM_MEDIANDK_SRCS)): $(FCM_HIDL)/hidl.stamp
$(FCMLIB)/libmediandk8.a: $(call fcmobj,$(FCM_MEDIANDK_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# === NDK sound: libaaudio, OpenSL ES's output =========================================
# AAudio (shim/fw8cm/aaudio8.cc) on the lean AudioTrack, and the sound half
# of the guest's OpenSL ES (topic gles-audio's shim/ndk/ndk_audio_fw.cc:
# the ndkfw_audio_* functions ndk_opensles.cc calls) built for 8.1. Plain
# host functions, like libmediandk's.
FCM_NDK_AUDIO_SRCS := shim/fw8cm/aaudio8.cc shim/ndk/ndk_audio_fw.cc
$(call fcmobj,$(FCM_NDK_AUDIO_SRCS)): FCMFLAGS := $(FCM_AUDIO_INCLUDES) \
    -I$(FCM_AV)/media/libaaudio/include
$(FCMLIB)/libndk_audio8.a: $(call fcmobj,$(FCM_NDK_AUDIO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- the NDK bridge's tables of libmediandk.so and libaaudio.so ---------------------------
# shim/ndk/ndk_media8.cc registers them with topic fw8-system's registry of
# host libraries (shim/ndk/ndk_host_libs.h): built and linked where that is
# (once both topics are merged); before, native code of the tests calls the
# functions directly (NdkMediaTest).
ifneq ($(wildcard shim/ndk/ndk_host_libs.h),)
FCM_NDK_TABLE_SRCS := shim/ndk/ndk_media8.cc
$(call fcmobj,$(FCM_NDK_TABLE_SRCS)): FCMFLAGS := -std=gnu++17 -Ishim/ndk \
    -I$(FCM_AV)/media/libaaudio/include -I$(FCM_AV)/media/ndk/include \
    -I$(A81)/frameworks/native/libs/nativewindow/include \
    -I$(A81)/frameworks/native/libs/arect/include -D__ANDROID_API__=27 -D__USE_FILE_OFFSET64
$(FCMLIB)/libndk_media8.a: $(call fcmobj,$(FCM_NDK_TABLE_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FCM_NDK_TABLE_LIBS := $(FCMLIB)/libndk_media8.a
# NdkMediaArmTest's ARM library (tests/ndk8cm), built like topic
# ndk-imports' libndk6test.so (NDK r9's android-18 sysroot) with AOSP 8.1's
# NDK media headers, linking stand-ins of libmediandk.so and libaaudio.so
# (tests/ndk8cm/stubs.c with their sonames), in /system/lib.
# (NDK18_SYSROOT, NDK6_CC, NDKT_LD: mk/topic-ndk-imports.mk, included later.)
FCM_NDK8 := $(FCM)/ndk8cm
FCM_NDK8_FLAGS = -I$(FCM_AV)/media/ndk/include -I$(FCM_AV)/media/libaaudio/include \
    -Wno-implicit-function-declaration
$(FCM_NDK8)/%.c.o: tests/ndk8cm/%.c | $$(NDK18_SYSROOT)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDK6_CC) $(FCM_NDK8_FLAGS) -c $< -o $@
$(FCM_NDK8)/stubs/%.so: $(FCM_NDK8)/stubs.c.o
	@mkdir -p $(dir $@)
	$(NDKT_LD) -soname $*.so -o $@ $<
FCM_NDK8_STUBS := $(addprefix $(FCM_NDK8)/stubs/,libmediandk.so libaaudio.so)
FCM_NDK8_LIB := $(FCM_NDK8)/libndk8cmtest.so
$(FCM_NDK8_LIB): $(FCM_NDK8)/ndk8cmtest.c.o $(BUILD)/ndk/armeabi-v7a/libndkrt.a $(FCM_NDK8_STUBS)
	$(NDKT_LD) -soname libndk8cmtest.so -o $@ $(NDK18_SYSROOT)/lib/crtbegin_so.o \
	    $(FCM_NDK8)/ndk8cmtest.c.o $(BUILD)/ndk/armeabi-v7a/libndkrt.a $(FCM_NDK8_STUBS) \
	    -L$(NDK18_SYSROOT)/lib -llog -lc -lm -ldl $(NDK18_SYSROOT)/lib/crtend_so.o
ASYSTEM8_ADD += --add $(FCM_NDK8_LIB):system/lib/libndk8cmtest.so
$(A8SYSTEM_IMAGE): $(FCM_NDK8_LIB)
endif

# === into artvm8 ======================================================================
# The libraries with constructors that add themselves to the static JNI
# table are linked whole.
FCM_JNI_LIBS := $(FCMLIB)/libjavacrypto8.a $(FCMLIB)/libaudio8.a $(FCMLIB)/libmedia_jni8.a \
                $(FCMLIB)/libmediandk8.a $(FCMLIB)/libndk_audio8.a $(FCMLIB)/libarcwasm_fw8cm_test.a
FCM_LIBS := $(FCMLIB)/libssl8.a $(FCMLIB)/libstagefright_extract8.a \
            $(FCMLIB)/libstagefright_foundation8.a $(FCMLIB)/libvorbis_flac8.a $(FCMLIB)/libcas_hidl8.a
ART8_LIBS += -Wl,--whole-archive $(FCM_JNI_LIBS) $(FCM_NDK_TABLE_LIBS) -Wl,--no-whole-archive \
             $(FCM_LIBS)
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(FCM_JNI_LIBS) $(FCM_NDK_TABLE_LIBS) $(FCM_LIBS)
# WebCodecs on the media thread (shim/media/webcodecs.js; in Node it reports
# no WebCodecs).
ART8_LDFLAGS += --pre-js shim/media/webcodecs.js
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: shim/media/webcodecs.js
# The page's audio sink (the AudioWorklet of topic gles-audio).
$(A8WEB)/artvm8.js: ART8_LDFLAGS += --pre-js shim/audio/web_audio.js
$(A8WEB)/artvm8.js: shim/audio/web_audio.js

.PHONY: fw8cm-libs
fw8cm-libs: $(FCM_JNI_LIBS) $(FCM_NDK_TABLE_LIBS) $(FCM_LIBS)

# === tests ============================================================================
# CryptoTest and NetTest (tests/java, as on Dalvik and ART 6.0.1) and
# XmlTest (tests/java6), unchanged sources; AudioTest (tests/fw8cm: sound of
# 8.1, with fake "audio"/"appops" services when fw8-system's are not there).
# Their natives (arcwasm.audio.AudioStats, the mixer's counters) are the
# static JNI library libarcwasm_fw8cm_test.so (an empty file in /system/lib).
FCM_TESTS := CryptoTest NetTest XmlTest AudioTest MediaTest NdkMediaTest NdkMediaArmTest
FCM_TEST_SRCS := tests/java/arcwasm/CryptoTest.java tests/java/arcwasm/NetTest.java tests/java/arcwasm/NetPolicyTest.java \
                 tests/java6/arcwasm/XmlTest.java tests/java6/arcwasm/MediaTest.java \
                 $(wildcard tests/fw8cm/arcwasm/*.java) \
                 framework/java-arcwasm/arcwasm/audio/AudioStats.java
FCM_TEST_JNI_SRCS := tests/native/fw8cm/fw8cm_test_jni.cc tests/native/fw8cm/ndk_media_test_jni.cc
$(call fcmobj,$(FCM_TEST_JNI_SRCS)): FCMFLAGS := $(FCM_AUDIO_INCLUDES) \
    -I$(FCM_AV)/media/libaaudio/include -I$(FCM_AV)/media/ndk/include -D__ANDROID_API__=27 \
    -D__USE_FILE_OFFSET64
$(FCMLIB)/libarcwasm_fw8cm_test.a: $(call fcmobj,$(FCM_TEST_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ASYSTEM8_ADD += --add $(FCM)/empty.so:system/lib/libarcwasm_fw8cm_test.so
# MediaTest (topic mediacodec's): the clips of tests/media in
# /system/media/arcwasm/media, and over HTTP from build/media-http (whose
# rule is in mk/topic-mediacodec.mk, included later).
FCM_MEDIA_CLIPS := $(wildcard tests/media/*)
ASYSTEM8_ADD += $(foreach f,$(FCM_MEDIA_CLIPS),--add $(f):system/media/arcwasm/media/$(notdir $(f)))
$(A8SYSTEM_IMAGE): $(FCM_MEDIA_CLIPS)
art8 art8-web: $(patsubst tests/media/%,$(BUILD)/media-http/%,$(FCM_MEDIA_CLIPS))
ART8_JAVA_TESTS += $(FCM_TEST_SRCS)
# (the makefile too: the list of tests is in it)
$(A8FW)/arc-tests.jar: $(FCM_TEST_SRCS) mk/topic-fw8-crypto-media.mk

.PHONY: fw8cm-test
fw8cm-test: art8
	VM=art8 tools/run_java_tests.sh node $(FCM_TESTS)

endif
