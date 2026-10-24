# Topic "fw10-crypto-media": cryptography, sqlite, sound and media of the
# Android 10 framework on ART 10 (docs/topics/fw10-crypto-media.md). What
# topic fw8-crypto-media did for 8.1 (mk/topic-fw8-crypto-media.mk), for 10:
#
#   crypto   BoringSSL 10's libssl (libcrypto is topic art10's libcrypto10.a),
#            conscrypt 10's natives (libjavacrypto: NativeCrypto behind the
#            "AndroidOpenSSL" provider; the Java half is topic fw10-core's
#            conscrypt.jar), android.security.Scrypt, /system/etc/security/
#            cacerts. NativeBN (BigInteger) and ExpatParser (SAX) are in topic
#            art10's libjavacore10.a already.
#
#   sqlite   SQLite of 10 with Android's extensions and the JNI of
#            android.database.sqlite and CursorWindow.
#
#   sound    AOSP 10's AudioTrack, AudioRecord, AudioAttributes JNI,
#            unchanged, over topic gles-audio's lean AudioTrack (shim/audio,
#            with its 10 API under ARC_WASM_FW10) and in-process mixer;
#            AudioSystem's natives are ours.
#
#   media    MediaCodec, MediaCodecList, MediaExtractor, MediaPlayer of 10
#            on WebCodecs: AOSP 10's libmedia_jni, unchanged, over
#            shim/media (ARC_WASM_FW10) and 10's extraction with the
#            extractor plugins linked in; libmediandk 10, AAudio, OpenSL ES's
#            output for native code.
#
#   make fw10cm-libs        # the libraries
#   make fw10cm-test        # this topic's tests on ART 10 in Node
#
# Objects in build/fw10cm/obj, libraries in build/fw10cm/lib. Included after
# mk/topic-art10.mk and mk/topic-fw10-core.mk (alphabetical order): A10*,
# F10*, FW10_INCLUDES, ART10_LIBS, ASYSTEM10_ADD and ART10_JAVA_TESTS are
# defined.

F10C    := $(BUILD)/fw10cm
F10COBJ := $(F10C)/obj
F10CLIB := $(F10C)/lib
f10cobj  = $(patsubst %,$(F10COBJ)/%.o,$(1))

# Two kinds of sources: ART/libcore-side ones (topic art10's flags, A10_*)
# and framework-side ones (topic fw10-core's, F10_* with FW10_INCLUDES).
# F10C_C and F10C_CXX pick the base per target; F10CFLAGS adds to it.
F10C_C   = $(F10_CFLAGS) $(F10_FLAGS)
F10C_CXX = $(F10_CXXFLAGS) $(F10_FLAGS)
$(F10COBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(F10C_C) $(F10CFLAGS) -c $< -o $@
$(F10COBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10C_CXX) $(F10CFLAGS) -c $< -o $@
$(F10COBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10C_CXX) $(F10CFLAGS) -c $< -o $@
-include $(shell find $(F10COBJ) -name '*.d' 2>/dev/null)

ifneq ($(A10LIB),)

# === crypto ==========================================================================

# --- libssl (BoringSSL 10) -------------------------------------------------------------
# Same flags as topic art10's libcrypto10.a.
F10C_SSL_SRCS := $(call bp10,$(BSSL10)/sources.bp,libssl_sources)
$(call f10cobj,$(F10C_SSL_SRCS)): F10C_CXX = $(A10_CXXFLAGS)
$(call f10cobj,$(F10C_SSL_SRCS)): F10CFLAGS := $(BSSL10_INCLUDES) -DOPENSSL_NO_ASM \
    -DBORINGSSL_IMPLEMENTATION -DOPENSSL_SMALL -DBORINGSSL_ANDROID_SYSTEM
$(F10CLIB)/libssl10.a: $(call f10cobj,$(F10C_SSL_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libjavacrypto (conscrypt 10) -------------------------------------------------------
# As AOSP's libjavacrypto (external/conscrypt/Android.bp): classes are
# com.android.org.conscrypt (JNI_JARJAR_PREFIX; in 10 the Java sources are
# repackaged already). Linked in: its JNI_OnLoad is renamed and added to
# topic art10's table of static JNI libraries (shim/fw10cm/
# javacrypto10_lib.cc); System.loadLibrary wants an empty file in
# /system/lib. ANDROID as for the device (as for 8.1): libcore's
# FileDescriptor field "descriptor" (not OpenJDK's "fd"), liblog. (Its
# compat.h undefines _GNU_SOURCE for <string.h>; musl's then lacks
# strcasecmp, which bionic's <string.h> gets from <strings.h>.)
F10C_CONSCRYPT := $(A10)/external/conscrypt/common/src/jni/main
F10C_JCRYPTO_SRCS := $(wildcard $(F10C_CONSCRYPT)/cpp/conscrypt/*.cc) shim/fw10cm/javacrypto10_lib.cc
$(call f10cobj,$(F10C_JCRYPTO_SRCS)): F10C_CXX = $(A10_CXXFLAGS)
$(call f10cobj,$(F10C_JCRYPTO_SRCS)): F10CFLAGS := -I$(F10C_CONSCRYPT)/include $(A10_SYS_FLAGS) \
    $(BSSL10_INCLUDES) -I$(A10)/libcore/include -I$(A10)/libcore/luni/src/main/native \
    '-DJNI_JARJAR_PREFIX=com/android/' -DJNI_OnLoad=arc_wasm_javacrypto10_JNI_OnLoad -DANDROID \
    -include strings.h
$(F10CLIB)/libjavacrypto10.a: $(call f10cobj,$(F10C_JCRYPTO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- android.security.Scrypt (core/jni, on libcrypto10's EVP_PBE_scrypt) ------------------
# external/scrypt is not fetched: its crypto_scrypt() is BoringSSL's
# EVP_PBE_scrypt (shim/fw10cm/crypto_scrypt.c).
F10C_FWJNI_SRCS := $(F10_JNI)/android_security_Scrypt.cpp shim/fw10cm/crypto_scrypt.c
$(call f10cobj,$(F10C_FWJNI_SRCS)): F10CFLAGS := $(F10_JNI_FLAGS) -Ishim/fw10cm/include $(BSSL10_INCLUDES)
$(F10CLIB)/libandroid_runtime10_cm.a: $(call f10cobj,$(F10C_FWJNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# === sqlite ==========================================================================
# SQLite of 10 (external/sqlite: the device flags of dist/Android.bp, as
# topic fw8-system did for 8.1; WAL's -shm index is a MAP_SHARED mmap shared
# by one process's connections) with Android's extensions (sqlite3_android:
# ICU collations LOCALIZED/UNICODE, PHONE_NUMBERS_EQUAL...) on topic art10's
# ICU 63, and the JNI of android.database (CursorWindow from fw10-core's
# libandroidfw10).
F10C_SQLITE := $(A10)/external/sqlite
F10C_SQLITE_FLAGS := -DNDEBUG=1 -DHAVE_USLEEP=1 -DSQLITE_HAVE_ISNAN \
    -DSQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576 -DSQLITE_THREADSAFE=2 -DSQLITE_TEMP_STORE=3 \
    -DSQLITE_POWERSAFE_OVERWRITE=1 -DSQLITE_DEFAULT_FILE_FORMAT=4 -DSQLITE_DEFAULT_AUTOVACUUM=1 \
    -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_BACKWARDS \
    -DSQLITE_ENABLE_FTS4 -DSQLITE_OMIT_BUILTIN_TEST -DSQLITE_OMIT_COMPILEOPTION_DIAGS \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_FILE_PERMISSIONS=0600 -DSQLITE_SECURE_DELETE \
    -DSQLITE_ENABLE_BATCH_ATOMIC_WRITE -DHAVE_POSIX_FALLOCATE=1 -DSQLITE_ENABLE_ICU \
    -include shim/app6/sqlite_config.h -Wno-everything
F10C_SQLITE_SRCS := $(F10C_SQLITE)/dist/sqlite3.c \
    $(addprefix $(F10C_SQLITE)/android/,sqlite3_android.cpp PhoneNumberUtils.cpp OldPhoneNumberUtils.cpp)
$(call f10cobj,$(F10C_SQLITE_SRCS)): shim/app6/sqlite_config.h
$(call f10cobj,$(F10C_SQLITE_SRCS)): F10CFLAGS := $(F10C_SQLITE_FLAGS) -idirafter $(F10C_SQLITE)/dist \
    -I$(F10C_SQLITE)/android $(ICU63_INCLUDES) -DU_USING_ICU_NAMESPACE=0 -DLOG_TAG=\"sqlite3_android\"
F10C_SQLJNI_SRCS := $(addprefix $(F10_JNI)/,android_database_SQLiteConnection.cpp \
    android_database_SQLiteGlobal.cpp android_database_SQLiteDebug.cpp \
    android_database_SQLiteCommon.cpp android_database_CursorWindow.cpp)
$(call f10cobj,$(F10C_SQLJNI_SRCS)): F10CFLAGS := $(F10_JNI_FLAGS) -idirafter $(F10C_SQLITE)/dist \
    -I$(F10C_SQLITE)/android
$(F10CLIB)/libandroid_runtime10_sqlite.a: $(call f10cobj,$(F10C_SQLITE_SRCS) $(F10C_SQLJNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# === sound ===========================================================================
# AOSP 10's AudioTrack, AudioRecord, DeviceCallback, AudioAttributes,
# MicrophoneInfo and MediaMetrics JNI over topic gles-audio's lean
# AudioTrack (shim/audio) with its 6.0, 8.1 and 10 API (ARC_WASM_FW6,
# ARC_WASM_FW8, ARC_WASM_FW10: in every file that includes its header, the
# class layout depends on them) and in-process mixer, whose sink is the
# page's AudioWorklet (Node: a clock); AudioRecord is a device without an
# input (shim/fw10cm/include/media/AudioRecord.h, shim/fw8cm/
# audio_record8.cc); AudioSystem's, AudioProductStrategy's and
# AudioVolumeGroup's natives are ours (shim/fw10cm/audio_system10_jni.cc).
# libmediametrics 10 (MediaAnalyticsItem: AudioTrack.getMetrics; nothing
# collects them, there is no media.metrics service); libmedia's
# AudioParameter (AudioTrack.setOffloadDelayPadding's parameters).
# shim/fw10cm/include (AudioRecord) and shim/audio/include come before
# frameworks/av's headers (VolumeShaper.h, AudioTimestamp.h,
# MicrophoneInfo.h are 10's own).
F10C_AV := $(A10)/frameworks/av
F10C_AUDIO_INCLUDES := -Ishim/fw10cm/include -Ishim/audio/include -Ishim/audio \
    -I$(F10C_AV)/include -I$(A10)/system/media/audio/include \
    -I$(A10)/system/media/audio_utils/include -I$(F10C_AV)/media/libmediametrics/include \
    $(F10_JNI_FLAGS) -I$(F10FWB)/media/jni \
    -DARC_WASM_FW6=1 -DARC_WASM_FW8=1 -DARC_WASM_FW10=1
F10C_AUDIO_SRCS := $(addprefix $(F10_JNI)/,android_media_AudioTrack.cpp android_media_AudioRecord.cpp \
                     android_media_DeviceCallback.cpp android_media_AudioAttributes.cpp \
                     android_media_MicrophoneInfo.cpp android_media_MediaMetricsJNI.cpp) \
                   shim/audio/audio_flinger.cc shim/audio/audio_track.cc \
                   shim/fw8cm/audio_record8.cc shim/fw10cm/audio_system10_jni.cc \
                   $(addprefix $(F10C_AV)/media/libmediametrics/,MediaAnalyticsItem.cpp \
                     IMediaAnalyticsService.cpp) \
                   $(F10C_AV)/media/libmedia/AudioParameter.cpp
$(call f10cobj,$(F10C_AUDIO_SRCS)): F10CFLAGS := $(F10C_AUDIO_INCLUDES)
$(F10CLIB)/libaudio10.a: $(call f10cobj,$(F10C_AUDIO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# === media ===========================================================================
# What topic fw8-crypto-media did for 8.1, for 10: libstagefright_foundation
# 10, the extraction of 10's libstagefright (NuMediaExtractor over
# MediaExtractorFactory, in process: media.stagefright.extractremote is
# false) with the extractor plugins of frameworks/av/media/extractors linked
# in (each one's GETEXTRACTORDEF renamed; shim/fw10cm/extractors10.cc lists
# them for MediaExtractorFactory instead of dlopen of /system/lib/extractors),
# the parts of libmedia they and the JNI need, libmediandk 10 (the plugins'
# AMediaFormat, and native code's AMediaCodec/AMediaExtractor), and
# shim/media -- MediaCodec, MediaCodecList, MediaPlayer on WebCodecs --
# built for 10 (ARC_WASM_FW8 and ARC_WASM_FW10) under AOSP 10's
# libmedia_jni, unchanged.
F10C_SF := $(F10C_AV)/media/libstagefright
F10C_EX := $(F10C_AV)/media/extractors
F10C_HIDL := $(F10C)/gen/hidl-cpp
# shim/media/include first (our MediaCodec.h, MediaCodecList.h,
# mediaplayer.h over frameworks/av's), then the sound includes (lean
# AudioTrack/AudioSystem, AudioRecord), frameworks/av's (its include/media
# links to every library's headers), libgui's and the HAL headers.
# __ANDROID_API__ 29: the NDK media headers (AMEDIAFORMAT_KEY_* of
# MetaDataUtils and the extractor plugins) declare only what that API level
# has.
F10C_MEDIA_INCLUDES = -Ishim/media/include $(F10C_AUDIO_INCLUDES) \
    -I$(F10C_SF)/foundation/include -I$(F10C_SF)/include \
    -I$(F10C_AV)/media/libmedia/include -I$(F10C_AV)/media/ndk/include \
    -I$(A10)/frameworks/native/include/media/openmax \
    -I$(A10)/frameworks/native/headers/media_plugin \
    $(addprefix -I$(A10)/frameworks/native/libs/,gui/include ui/include nativewindow/include \
      nativebase/include arect/include) \
    -I$(A10)/hardware/libhardware/include -I$(F10C_HIDL) -I$(F10_HIDL_GENCPP) \
    -I$(A10)/system/libhidl/transport/token/1.0/utils/include \
    -I$(A10)/system/libhidl/transport/allocator/1.0/utils/include \
    -I$(A10)/system/libhidl/libhidlmemory/include \
    -I$(A10)/system/core/libnativeloader/include \
    -include shim/media/media_compat.h -include shim/fw10cm/media10_compat.h \
    -Wno-sign-compare -Wno-unused-variable \
    -D__ANDROID_API__=29
# C++ of the HIDL interfaces media's headers name, from topic fw10-core's
# hidl-gen: headers of OMX and what it imports (constants and types only:
# nothing here talks to an OMX HAL), headers and sources of CAS (the
# MediaDescrambler/MediaCas JNI hold IDescrambler/ICas objects) and of
# android.hidl.token (libhidl's HybridInterface: tokens of HAL objects; no
# token manager runs, so none is ever made or found), headers of
# android.hidl.memory and allocator (hidlmemory's FrameworkUtils).
F10C_HIDL_HDR_PKGS := android.hardware.graphics.common@1.0 android.hardware.graphics.common@1.1 \
    android.hardware.graphics.common@1.2 android.hardware.media@1.0 \
    android.hardware.graphics.bufferqueue@1.0 android.hardware.graphics.bufferqueue@2.0 \
    android.hardware.media.omx@1.0 android.hidl.memory@1.0 android.hidl.memory.token@1.0 \
    android.hidl.allocator@1.0
F10C_HIDL_SRC_PKGS := android.hardware.cas@1.0 android.hardware.cas.native@1.0 android.hidl.token@1.0
F10C_HIDL_SRCS := $(F10C_HIDL)/android/hardware/cas/1.0/types.cpp \
    $(addprefix $(F10C_HIDL)/android/hardware/cas/1.0/,CasAll.cpp CasListenerAll.cpp \
      DescramblerBaseAll.cpp MediaCasServiceAll.cpp) \
    $(addprefix $(F10C_HIDL)/android/hardware/cas/native/1.0/,types.cpp DescramblerAll.cpp) \
    $(F10C_HIDL)/android/hidl/token/1.0/TokenManagerAll.cpp
F10C_HIDL_GEN = $(F10H)/hidl-gen -o $(F10C_HIDL) -randroid.hardware:$(A10)/hardware/interfaces \
    -randroid.hidl:$(A10)/system/libhidl/transport
$(F10C_HIDL)/hidl.stamp: $(F10H)/hidl-gen mk/topic-fw10-crypto-media.mk
	rm -rf $(F10C_HIDL) && mkdir -p $(F10C_HIDL)
	for p in $(F10C_HIDL_HDR_PKGS) $(F10C_HIDL_SRC_PKGS); do \
	  $(F10C_HIDL_GEN) -Lc++-headers $$p || exit 1; \
	done
	for p in $(F10C_HIDL_SRC_PKGS); do $(F10C_HIDL_GEN) -Lc++-sources $$p || exit 1; done
	@touch $@
$(F10C_HIDL_SRCS): $(F10C_HIDL)/hidl.stamp ;
$(call f10cobj,$(F10C_HIDL_SRCS)): F10CFLAGS := -I$(F10C_HIDL) -I$(F10_HIDL_GENCPP)
$(F10CLIB)/libcas_hidl10.a: $(call f10cobj,$(F10C_HIDL_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

F10C_FOUNDATION_SRCS := $(addprefix $(F10C_SF)/foundation/, \
    $(shell tools/bp_srcs.py $(F10C_SF)/foundation/Android.bp libstagefright_foundation_defaults 2>/dev/null))
# The extraction of libstagefright 10 and what it needs of libmedia (binder
# interfaces of extractors, sources, HTTP; MediaCodecInfo/MediaCodecBuffer
# for the codec JNI), libstagefright's esds, id3, metadatautils, flacdec;
# libhidl's HybridInterface and hidlmemory's fromHeap (MediaDescrambler).
F10C_EXTRACT_SRCS := $(addprefix $(F10C_SF)/,CallbackDataSource.cpp DataSourceBase.cpp \
    DataSourceFactory.cpp DataURISource.cpp ClearFileSource.cpp FileSource.cpp HTTPBase.cpp \
    HevcUtils.cpp InterfaceUtils.cpp MediaExtractor.cpp MediaExtractorFactory.cpp MediaSource.cpp \
    MediaTrack.cpp http/ClearMediaHTTP.cpp http/MediaHTTP.cpp NuCachedSource2.cpp \
    NuMediaExtractor.cpp RemoteMediaExtractor.cpp RemoteMediaSource.cpp Utils.cpp \
    MetaDataUtils.cpp ESDS.cpp id3/ID3.cpp flac/dec/FLACDecoder.cpp) \
    $(addprefix $(F10C_AV)/media/libmedia/,IMediaHTTPConnection.cpp IMediaHTTPService.cpp \
    IDataSource.cpp IMediaExtractor.cpp IMediaExtractorService.cpp IMediaSource.cpp \
    MediaCodecInfo.cpp IMediaCodecList.cpp IStreamSource.cpp MediaCodecBuffer.cpp \
    MediaUtils.cpp BufferingSettings.cpp NdkMediaFormatPriv.cpp NdkMediaErrorPriv.cpp) \
    $(A10)/system/media/audio_utils/primitives.c \
    $(A10)/system/libhidl/transport/token/1.0/utils/HybridInterface.cpp \
    $(A10)/system/libhidl/transport/allocator/1.0/utils/FrameworkUtils.cpp
F10C_EXTRACT_FLAGS = $(F10C_MEDIA_INCLUDES) -I$(F10C_SF) \
    -I$(F10C_SF)/foundation/include/media/stagefright/foundation \
    -I$(A10)/external/flac/include -I$(F10C_SF)/flac/dec $(BSSL10_INCLUDES)
$(call f10cobj,$(F10C_FOUNDATION_SRCS) $(F10C_EXTRACT_SRCS)): F10CFLAGS = $(F10C_EXTRACT_FLAGS)
$(call f10cobj,$(F10C_FOUNDATION_SRCS) $(F10C_EXTRACT_SRCS)): $(F10C_HIDL)/hidl.stamp
$(F10CLIB)/libstagefright_foundation10.a: $(call f10cobj,$(F10C_FOUNDATION_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(F10CLIB)/libstagefright_extract10.a: $(call f10cobj,$(F10C_EXTRACT_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- the extractor plugins ------------------------------------------------------------
# Each is a library of its own on a device (dlopen'ed, its GETEXTRACTORDEF
# looked up): here each one's GETEXTRACTORDEF is GETEXTRACTORDEF_<name>
# (shim/fw10cm/extractors10.cc: keep its table like this list). No MIDI
# (sonivox) and no MPEG-2 TS/PS.
F10C_EXTRACTORS := mp4 mkv mp3 aac amr wav ogg flac
F10C_EX_SRCS_mp4 := AC4Parser.cpp ItemTable.cpp MPEG4Extractor.cpp SampleIterator.cpp SampleTable.cpp
F10C_EX_SRCS_mkv := MatroskaExtractor.cpp
F10C_EX_SRCS_mp3 := MP3Extractor.cpp VBRISeeker.cpp XINGSeeker.cpp
F10C_EX_SRCS_aac := AACExtractor.cpp
F10C_EX_SRCS_amr := AMRExtractor.cpp
F10C_EX_SRCS_wav := WAVExtractor.cpp
F10C_EX_SRCS_ogg := OggExtractor.cpp
F10C_EX_SRCS_flac := FLACExtractor.cpp
F10C_EX_SRCS := $(foreach e,$(F10C_EXTRACTORS),$(addprefix $(F10C_EX)/$(e)/,$(F10C_EX_SRCS_$(e))))
define F10C_EXTRACTOR_FLAGS
$(call f10cobj,$(addprefix $(F10C_EX)/$(1)/,$(F10C_EX_SRCS_$(1)))): F10CFLAGS = $$(F10C_EXTRACT_FLAGS) \
    -I$(F10C_EX)/$(1) -DGETEXTRACTORDEF=GETEXTRACTORDEF_$(1) \
    -I$(A10)/external/libvpx/libwebm -I$(A10)/external/tremolo \
    -I$(A10)/frameworks/native/libs/binder/ndk/include_ndk
endef
$(foreach e,$(F10C_EXTRACTORS),$(eval $(call F10C_EXTRACTOR_FLAGS,$(e))))
$(call f10cobj,$(F10C_EX_SRCS)): $(F10C_HIDL)/hidl.stamp
# Tremolo (the Vorbis headers OggExtractor parses) and libFLAC, C only.
F10C_TREMOLO_SRCS := $(addprefix $(A10)/external/tremolo/Tremolo/,bitwise.c codebook.c dsp.c floor0.c \
    floor1.c floor_lookup.c framing.c mapping0.c mdct.c misc.c res012.c treminfo.c vorbisfile.c)
$(call f10cobj,$(F10C_TREMOLO_SRCS)): F10CFLAGS := -I$(A10)/external/tremolo/Tremolo -DONLY_C \
    -funsigned-char -Wno-implicit-function-declaration
F10C_FLAC := $(A10)/external/flac
F10C_FLAC_SRCS := $(addprefix $(F10C_FLAC)/libFLAC/,bitmath.c bitreader.c bitwriter.c cpu.c crc.c \
    fixed.c float.c format.c lpc.c memory.c md5.c stream_decoder.c stream_encoder.c \
    stream_encoder_framing.c window.c)
$(call f10cobj,$(F10C_FLAC_SRCS)): F10CFLAGS := -I$(F10C_FLAC)/libFLAC/include -I$(F10C_FLAC)/include \
    -I$(F10C_FLAC) -DHAVE_CONFIG_H -DFLAC__NO_MD5 -DFLAC__INTEGER_ONLY_LIBRARY -D_REENTRANT
F10C_WEBM_SRCS := $(A10)/external/libvpx/libwebm/mkvparser/mkvparser.cc
$(call f10cobj,$(F10C_WEBM_SRCS)): F10CFLAGS := -I$(A10)/external/libvpx/libwebm
$(F10CLIB)/libextractors10.a: $(call f10cobj,$(F10C_EX_SRCS) shim/fw10cm/extractors10.cc \
                                shim/fw10cm/binder_ndk_uid.cc \
                                $(F10C_TREMOLO_SRCS) $(F10C_FLAC_SRCS) $(F10C_WEBM_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(call f10cobj,shim/fw10cm/extractors10.cc): F10CFLAGS = $(F10C_EXTRACT_FLAGS)
# (AIBinder_getCallingUid of the WAV and FLAC plugins)
$(call f10cobj,shim/fw10cm/binder_ndk_uid.cc): F10CFLAGS = -include shim/fw10cm/media10_compat.h \
    -I$(A10)/frameworks/native/libs/binder/ndk/include_ndk

# --- MediaCodec, MediaCodecList, MediaPlayer on WebCodecs; libmedia_jni 10 ----------------
# shim/media built for 10 (ARC_WASM_FW8 and ARC_WASM_FW10) and AOSP 10's
# libmedia_jni, unchanged: its JNI_OnLoad (android_media_MediaPlayer.cpp) is
# the static library's entry point (shim/fw10cm/media_jni10.cc adds it to
# art10's table and registers what this build does not have as missing).
F10C_MC_SHIM_SRCS := $(addprefix shim/media/,webcodecs.cc bitstream.cc yuv.cc codecs.cc \
    media_codec_list.cc media_codec.cc media_player.cc media_drm_jni.cc drm_stubs.cc) \
    shim/fw10cm/media_jni10.cc shim/fw10cm/gui_fallback10.cc
F10C_MC_JNI := android_media_MediaCodec android_media_MediaCodecList android_media_MediaExtractor \
    android_media_Utils android_media_MediaDataSource android_media_MediaHTTPConnection \
    android_media_MediaPlayer android_media_SyncParams android_media_MediaDescrambler \
    android_media_Streams
F10C_MC_JNI_SRCS := $(addprefix $(F10FWB)/media/jni/,$(addsuffix .cpp,$(F10C_MC_JNI)))
$(call f10cobj,$(F10C_MC_SHIM_SRCS) $(F10C_MC_JNI_SRCS)): F10CFLAGS = $(F10C_MEDIA_INCLUDES) \
    -I$(F10C_SF) -Ishim/media -I$(F10C_AV)/drm/libmediadrm/include -I$(F10C_AV)/include/drm \
    -I$(A10)/external/piex -I$(A10)/system/media/camera/include -DARC_WASM_FW8_MEDIA_JNI \
    $(addprefix -I$(A10)/external/skia/include/,core config config/android) -DSK_BUILD_FOR_ANDROID
$(call f10cobj,$(F10FWB)/media/jni/android_media_MediaPlayer.cpp): F10CFLAGS += \
    -DJNI_OnLoad=arc_wasm_media_jni10_JNI_OnLoad
$(call f10cobj,shim/fw10cm/media_jni10.cc): F10CFLAGS += -I$(F10C_AV)/media/libmediaplayer2/include
$(call f10cobj,$(F10C_MC_SHIM_SRCS) $(F10C_MC_JNI_SRCS)): $(F10C_HIDL)/hidl.stamp
$(F10CLIB)/libmedia_jni10.a: $(call f10cobj,$(F10C_MC_SHIM_SRCS) $(F10C_MC_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# === NDK media: libmediandk ===========================================================
# AOSP 10's libmediandk (frameworks/av/media/ndk: AMediaCodec, AMediaFormat,
# AMediaExtractor, AMediaDataSource), unchanged, over shim/media's
# MediaCodec and 10's extraction; AMediaCrypto/AMediaDrm of a device
# without DRM (shim/fw8cm/ndk_media_drm8.cc). Plain host functions; the
# extractor plugins call AMediaFormat_* too. AMediaDataSource's Java
# callbacks find the VM through libmediaplayer2's JavaVMHelper (set by
# shim/fw10cm/media_jni10.cc); AMediaExtractor_setDataSource's HTTP goes
# through libmediaplayer2's JMedia2HTTPService/Connection (Java's
# MediaHTTPService of the app).
F10C_NDK_MEDIA := $(F10C_AV)/media/ndk
F10C_MEDIANDK_SRCS := $(addprefix $(F10C_NDK_MEDIA)/,NdkMediaCodec.cpp NdkMediaFormat.cpp \
                       NdkMediaExtractor.cpp NdkMediaDataSource.cpp NdkMediaDataSourceCallbacks.cpp) \
                      $(addprefix $(F10C_AV)/media/libmediaplayer2/,JavaVMHelper.cpp \
                        JMedia2HTTPService.cpp JMedia2HTTPConnection.cpp) shim/fw8cm/ndk_media_drm8.cc
$(call f10cobj,$(F10C_MEDIANDK_SRCS)): F10CFLAGS = $(F10C_MEDIA_INCLUDES) -I$(F10C_NDK_MEDIA)/include \
    -I$(F10C_AV)/media/libmediaplayer2/include \
    -I$(F10C_NDK_MEDIA) -DEXPORT='__attribute__((visibility("default")))' -D__ANDROID_API__=29 \
    -D__USE_FILE_OFFSET64
$(call f10cobj,$(F10C_MEDIANDK_SRCS)): $(F10C_HIDL)/hidl.stamp
$(F10CLIB)/libmediandk10.a: $(call f10cobj,$(F10C_MEDIANDK_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# === NDK sound: libaaudio, OpenSL ES's output =========================================
# AAudio of API 26 (shim/fw8cm/aaudio8.cc) on the lean AudioTrack, and the
# sound half of the guest's OpenSL ES (topic
# gles-audio's shim/ndk/ndk_audio_fw.cc: the ndkfw_audio_* functions
# ndk_opensles.cc calls) built for 10. Plain host functions, like
# libmediandk's.
F10C_NDK_AUDIO_SRCS := shim/fw8cm/aaudio8.cc shim/ndk/ndk_audio_fw.cc
$(call f10cobj,$(F10C_NDK_AUDIO_SRCS)): F10CFLAGS := $(F10C_AUDIO_INCLUDES) \
    -I$(F10C_AV)/media/libaaudio/include -include shim/fw10cm/media10_compat.h
$(F10CLIB)/libndk_audio10.a: $(call f10cobj,$(F10C_NDK_AUDIO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- the NDK bridge's tables of libmediandk.so and libaaudio.so ---------------------------
# shim/ndk/ndk_media8.cc (topic fw8-crypto-media: the functions of API 27,
# for ARM and arm64 guests) built against 10's headers, for the NDK bridge
# on ART 10, which is topic fw10-system's: it links this archive whole with
# -Wl,-u,ndk_host_library_fw8cm_media, as fw8-system does for 8.1. Not
# linked here (no bridge in artvm10 yet).
F10C_NDK_TABLE_SRCS := shim/ndk/ndk_media8.cc
$(call f10cobj,$(F10C_NDK_TABLE_SRCS)): F10CFLAGS := -std=gnu++17 -Ishim/ndk \
    -include shim/fw10cm/media10_compat.h \
    -I$(F10C_AV)/media/libaaudio/include -I$(F10C_AV)/media/ndk/include \
    -I$(A10)/frameworks/native/libs/nativewindow/include \
    -I$(A10)/frameworks/native/libs/arect/include -D__ANDROID_API__=27 -D__USE_FILE_OFFSET64
F10C_NDK_TABLE_LIBS := $(F10CLIB)/libndk_media10.a
$(F10C_NDK_TABLE_LIBS): $(call f10cobj,$(F10C_NDK_TABLE_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- /system --------------------------------------------------------------------------
$(F10C)/empty.so:
	@mkdir -p $(dir $@) && : > $@
# The CA store of conscrypt's TrustManager: the build host's roots, as in
# the 4.4, 6.0.1 and 8.1 images (mk/topic-net.mk builds them).
F10C_CACERTS := $(BUILD)/dalvik/cacerts
ASYSTEM10_ADD += --add $(F10C)/empty.so:system/lib/libjavacrypto.so \
                 --add $(F10C)/empty.so:system/lib/libmedia_jni.so \
                 --add $(F10C_CACERTS):system/etc/security/cacerts
$(A10SYSTEM_IMAGE): $(F10C)/empty.so $(F10C_CACERTS).stamp

# === into artvm10 =====================================================================
# The libraries with constructors that add themselves to the static JNI
# table, and the framework JNI (register_* of gRegJNI, referred to weakly),
# are linked whole.
F10C_JNI_LIBS := $(F10CLIB)/libjavacrypto10.a $(F10CLIB)/libandroid_runtime10_cm.a \
                 $(F10CLIB)/libandroid_runtime10_sqlite.a $(F10CLIB)/libaudio10.a \
                 $(F10CLIB)/libmedia_jni10.a $(F10CLIB)/libmediandk10.a $(F10CLIB)/libndk_audio10.a \
                 $(F10CLIB)/libarcwasm_fw10cm_test.a
F10C_LIBS := $(F10CLIB)/libssl10.a $(F10CLIB)/libstagefright_extract10.a $(F10CLIB)/libextractors10.a \
             $(F10CLIB)/libstagefright_foundation10.a $(F10CLIB)/libcas_hidl10.a
ART10_LIBS += -Wl,--whole-archive $(F10C_JNI_LIBS) -Wl,--no-whole-archive $(F10C_LIBS)
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: $(F10C_JNI_LIBS) $(F10C_LIBS)
# WebCodecs on the media thread (shim/media/webcodecs.js; in Node it reports
# no WebCodecs).
ART10_LDFLAGS += --pre-js shim/media/webcodecs.js
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: shim/media/webcodecs.js
# The page's audio sink (the AudioWorklet of topic gles-audio).
$(A10WEB)/artvm10.js: ART10_LDFLAGS += --pre-js shim/audio/web_audio.js
$(A10WEB)/artvm10.js: shim/audio/web_audio.js

.PHONY: fw10cm-libs
fw10cm-libs: $(F10C_JNI_LIBS) $(F10C_LIBS) $(F10C_NDK_TABLE_LIBS)

# === tests ============================================================================
# CryptoTest and NetTest (tests/java, as on Dalvik, ART 6.0.1 and 8.1) and
# XmlTest (tests/java6), unchanged sources; DatabaseTest (tests/java10:
# android.database.sqlite of 10); AudioTest (tests/fw8cm: sound of 8.1 and
# 10, with fake "audio"/"appops" services when fw10-system's are not
# there); MediaTest (tests/java6, topic mediacodec's: its display checks
# reach SurfaceControl through reflection, for 6.0.1, 8.1 and 10) and
# NdkMediaTest (tests/fw8cm: libmediandk and AAudio from host code). Their
# natives (arcwasm.audio.AudioStats, the mixer's counters; NdkMediaTest's)
# are the static JNI library libarcwasm_fw8cm_test.so (an empty file in
# /system/lib): 8.1's sources, added to art10's table.
F10C_TESTS := CryptoTest NetTest XmlTest DatabaseTest AudioTest MediaTest NdkMediaTest
F10C_TEST_SRCS := tests/java/arcwasm/CryptoTest.java tests/java/arcwasm/NetTest.java tests/java/arcwasm/NetPolicyTest.java \
                  tests/java6/arcwasm/XmlTest.java tests/java10/arcwasm/DatabaseTest.java \
                  tests/fw8cm/arcwasm/AudioTest.java tests/fw8cm/arcwasm/FakeServices.java \
                  tests/java6/arcwasm/MediaTest.java tests/fw8cm/arcwasm/NdkMediaTest.java \
                  framework/java-arcwasm/arcwasm/audio/AudioStats.java
F10C_TEST_JNI_SRCS := tests/native/fw8cm/fw8cm_test_jni.cc tests/native/fw8cm/ndk_media_test_jni.cc
$(call f10cobj,$(F10C_TEST_JNI_SRCS)): F10CFLAGS := $(F10C_AUDIO_INCLUDES) -include shim/fw10cm/media10_compat.h \
    -I$(F10C_AV)/media/libaaudio/include -I$(F10C_AV)/media/ndk/include -D__ANDROID_API__=29 \
    -D__USE_FILE_OFFSET64 -Darc_wasm_art8_add_static_jni_lib=arc_wasm_art10_add_static_jni_lib
$(F10CLIB)/libarcwasm_fw10cm_test.a: $(call f10cobj,$(F10C_TEST_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ASYSTEM10_ADD += --add $(F10C)/empty.so:system/lib/libarcwasm_fw8cm_test.so
# MediaTest (topic mediacodec's): the clips of tests/media in
# /system/media/arcwasm/media, and over HTTP from build/media-http (whose
# rule is in mk/topic-mediacodec.mk, included later).
F10C_MEDIA_CLIPS := $(wildcard tests/media/*)
ASYSTEM10_ADD += $(foreach f,$(F10C_MEDIA_CLIPS),--add $(f):system/media/arcwasm/media/$(notdir $(f)))
$(A10SYSTEM_IMAGE): $(F10C_MEDIA_CLIPS)
art10 art10-web: $(patsubst tests/media/%,$(BUILD)/media-http/%,$(F10C_MEDIA_CLIPS))
ART10_JAVA_TESTS += $(F10C_TEST_SRCS)
# (the makefile too: the list of tests is in it)
$(A10FW)/arc-tests.jar: $(F10C_TEST_SRCS) mk/topic-fw10-crypto-media.mk

.PHONY: fw10cm-test
fw10cm-test: art10
	VM=art10 tools/run_java_tests.sh node $(F10C_TESTS)

endif
