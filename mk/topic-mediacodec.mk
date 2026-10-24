# Topic "mediacodec": video and audio decoding for Android 6.0.1 apps on ART
# through the browser's WebCodecs (docs/topics/mediacodec.md).
#
#   libstagefright_foundation  AOSP 6.0.1 (ALooper, AMessage, ABuffer, AString...)
#   libstagefright_extract     AOSP 6.0.1 extractors: MP4 (MPEG4Extractor),
#                              Matroska/WebM (MatroskaExtractor + libwebm's
#                              mkvparser), MP3, WAV, AAC (ADTS), AMR, MPEG-2 TS;
#                              NuMediaExtractor, MetaData, MediaBuffer, Utils;
#                              DataSource/MediaExtractor's sniffers are ours
#                              (shim/media/data_source.cc: the formats built here)
#   libmedia_codec6            shim/media: a lean android::MediaCodec on
#                              VideoDecoder/AudioDecoder of WebCodecs (the decoders
#                              live on one "media" pthread that returns to its
#                              event loop, webcodecs.cc), MediaCodecList with what
#                              this browser can decode (isConfigSupported), a lean
#                              android::MediaPlayer (extractor + codecs +
#                              AudioTrack, video into the Surface);
#                              AOSP's libmedia_jni (MediaCodec, MediaCodecList,
#                              MediaExtractor, MediaPlayer, MediaCrypto/MediaDrm
#                              without DRM) unchanged over them
#
#   make mediacodec-libs     # the libraries
#   make mediacodec-test     # MediaTest on ART in Node (decoding: browser only)
#
# Included after mk/topic-fw6-system.mk (alphabetical order): FW6_*, FG*, FS*.

MC     := $(BUILD)/media
MCOBJ  := $(MC)/obj
MCLIB  := $(MC)/lib
mcobj   = $(patsubst %,$(MCOBJ)/%.o,$(1))
MC_AV  := $(A60)/frameworks/av
MC_SF  := $(MC_AV)/media/libstagefright

$(MCOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FG_CXXFLAGS) $(MCFLAGS) -c $< -o $@
$(MCOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FG_CXXFLAGS) $(MCFLAGS) -c $< -o $@
-include $(shell find $(MCOBJ) -name '*.d' 2>/dev/null)

# shim/media/include first: our MediaCodec.h, MediaCodecList.h, mediaplayer.h
# over frameworks/av's; then the 6.0 framework (FG_FW_INCLUDES_BASE: libgui,
# hardware), the lean AudioTrack/AudioSystem of topic gles-audio
# (shim/audio/include, ARC_WASM_FW6 as everywhere it is included).
MC_INCLUDES := -Ishim/media/include -Ishim/audio/include $(FG_FW_INCLUDES_BASE) \
    -I$(MC_AV)/include -I$(MC_AV)/include/media/stagefright/foundation \
    -I$(A60)/frameworks/native/include/media/openmax \
    -I$(A60)/frameworks/native/include/media/hardware \
    -I$(A60)/system/media/audio/include -I$(A60)/system/media/audio_utils/include \
    -DARC_WASM_FW6=1 -include shim/media/media_compat.h \
    -Wno-format -Wno-unused-variable -Wno-sign-compare

# --- libstagefright_foundation ---------------------------------------------------------
# (ANetworkSession, ParsedMessage: Wi-Fi display/RTSP; AWakeLock: ours, no
# power manager to hold a wake lock with.)
MC_FOUNDATION_SRCS := $(addprefix $(MC_SF)/foundation/,AAtomizer.cpp ABitReader.cpp ABuffer.cpp \
    AHandler.cpp AHierarchicalStateMachine.cpp ALooper.cpp ALooperRoster.cpp \
    AMessage.cpp AString.cpp AStringUtils.cpp base64.cpp hexdump.cpp)
$(call mcobj,$(MC_FOUNDATION_SRCS)): MCFLAGS := $(MC_INCLUDES) -DLOG_NDEBUG=1
$(MCLIB)/libstagefright_foundation.a: $(call mcobj,$(MC_FOUNDATION_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- extractors ---------------------------------------------------------------------------
MC_WEBM := $(A60)/external/libvpx/libwebm
MC_EXTRACT_SRCS := $(addprefix $(MC_SF)/,MPEG4Extractor.cpp SampleTable.cpp SampleIterator.cpp \
    ESDS.cpp MetaData.cpp MediaBuffer.cpp MediaBufferGroup.cpp MediaSource.cpp MediaDefs.cpp \
    FileSource.cpp CallbackDataSource.cpp NuMediaExtractor.cpp Utils.cpp avc_utils.cpp \
    MP3Extractor.cpp XINGSeeker.cpp VBRISeeker.cpp WAVExtractor.cpp AACExtractor.cpp \
    AMRExtractor.cpp id3/ID3.cpp matroska/MatroskaExtractor.cpp \
    mpeg2ts/MPEG2TSExtractor.cpp mpeg2ts/ATSParser.cpp mpeg2ts/AnotherPacketSource.cpp \
    mpeg2ts/ESQueue.cpp mpeg2ts/MPEG2PSExtractor.cpp \
    DataURISource.cpp NuCachedSource2.cpp HTTPBase.cpp http/MediaHTTP.cpp) \
    $(addprefix $(MC_AV)/media/libmedia/,IMediaHTTPConnection.cpp IMediaHTTPService.cpp \
    IDataSource.cpp MediaCodecInfo.cpp IMediaCodecList.cpp IStreamSource.cpp) \
    $(MC_WEBM)/mkvparser.cpp shim/media/media_extractor.cc
$(call mcobj,$(MC_EXTRACT_SRCS)): MCFLAGS := $(MC_INCLUDES) -I$(MC_SF) -I$(MC_WEBM) -DLOG_NDEBUG=1
$(MCLIB)/libstagefright_extract.a: $(call mcobj,$(MC_EXTRACT_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

MC_LIBS := $(MCLIB)/libstagefright_extract.a $(MCLIB)/libstagefright_foundation.a


# --- MediaCodec, MediaCodecList, MediaPlayer on WebCodecs (shim/media) ---------------------
MC_SHIM_SRCS := $(addprefix shim/media/,webcodecs.cc bitstream.cc yuv.cc codecs.cc \
    media_codec_list.cc media_codec.cc media_player.cc media_drm_jni.cc media_jni.cc drm_stubs.cc)
# AOSP 6.0.1's libmedia_jni, unchanged: its JNI_OnLoad (in
# android_media_MediaPlayer.cpp) is the static library's entry point.
MC_JNI := android_media_MediaCodec android_media_MediaCodecList android_media_MediaExtractor \
    android_media_Utils android_media_MediaDataSource android_media_MediaHTTPConnection \
    android_media_MediaPlayer android_media_SyncParams
MC_JNI_SRCS := $(addprefix $(F6FWB)/media/jni/,$(addsuffix .cpp,$(MC_JNI)))
$(call mcobj,$(MC_SHIM_SRCS) $(MC_JNI_SRCS)): MCFLAGS := $(MC_INCLUDES) -I$(F6FWB)/core/jni \
    -I$(F6FWB)/media/jni -I$(MC_SF) -Ishim/media -DLOG_NDEBUG=1
$(call mcobj,$(F6FWB)/media/jni/android_media_MediaPlayer.cpp): MCFLAGS += \
    -DJNI_OnLoad=arc_wasm_media_jni6_JNI_OnLoad
$(MCLIB)/libmedia_jni6.a: $(call mcobj,$(MC_SHIM_SRCS) $(MC_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
MC_LIBS := $(MCLIB)/libmedia_jni6.a $(MC_LIBS)

# --- into artvm ----------------------------------------------------------------------------
# libmedia_jni6.a whole (static_jni_libs_art.cc names its JNI_OnLoad weakly;
# webcodecs.js calls the arc_media_* exports), the extractors after it.
FW6_JNI_LIBS += $(MCLIB)/libmedia_jni6.a
FW6_LIBS += $(MCLIB)/libstagefright_extract.a $(MCLIB)/libstagefright_foundation.a
ART_LDFLAGS += --pre-js shim/media/webcodecs.js
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(MC_LIBS) shim/media/webcodecs.js

.PHONY: mediacodec-libs
mediacodec-libs: $(MC_LIBS)

# --- tests ---------------------------------------------------------------------------------
# tests/media (tools/gen_media_tests.sh) in /system/media/arcwasm/media;
# MediaTest (tests/java6) in arc-tests.jar.
MC_MEDIA := $(wildcard tests/media/*)
ASYSTEM_ADD += $(foreach f,$(MC_MEDIA),--add $(f):system/media/arcwasm/media/$(notdir $(f)))
$(ASYSTEM_IMAGE): $(MC_MEDIA)
MC_TEST_SRCS := tests/java6/arcwasm/MediaTest.java
ART_JAVA_TESTS += $(MC_TEST_SRCS)
$(AFW)/arc-tests.jar: $(MC_TEST_SRCS)

# The same files over HTTP (MediaTest's HTTP checks; tools/serve.py serves build/).
MC_HTTP_MEDIA := $(patsubst tests/media/%,$(BUILD)/media-http/%,$(MC_MEDIA))
$(BUILD)/media-http/%: tests/media/%
	@mkdir -p $(dir $@)
	cp $< $@
art art-web fw6-graphics-web: $(MC_HTTP_MEDIA)

.PHONY: mediacodec-test
mediacodec-test: art
	VM=art tools/run_java_tests.sh node MediaTest

# --- the demo: an app playing video in a SurfaceView on the page -----------------------------
#   fw6-graphics-web/app.html?pkg=org.arcwasm.mediademo   (make fw6-system-web)
# tests/apk6-media: VideoView (MediaPlayer into a SurfaceView) with its own
# clip, tests/media/demo.mp4 as res/raw/demo.mp4 (stored: aapt does not
# compress .mp4).
MC_DEMO_APK := $(MC)/apk6-media/ArcWasmMediaDemo.apk
MC_DEMO_FILES := $(shell find tests/apk6-media -type f) tests/media/demo.mp4
$(MC_DEMO_APK): $(MC_DEMO_FILES) tools/build_test_apk.sh
	rm -rf $(MC)/apk6-media/src && mkdir -p $(MC)/apk6-media
	cp -R tests/apk6-media $(MC)/apk6-media/src
	mkdir -p $(MC)/apk6-media/src/res/raw && cp tests/media/demo.mp4 $(MC)/apk6-media/src/res/raw/
	MIN_SDK=23 TARGET_SDK=23 AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    tools/build_test_apk.sh $(MC)/apk6-media/src $(MC)/apk6-media/work $@
ASYSTEM_ADD += --add $(MC_DEMO_APK):system/app/ArcWasmMediaDemo.apk
$(ASYSTEM_IMAGE): $(MC_DEMO_APK)
