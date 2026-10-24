# Topic "fw6-crypto": cryptography and XML for ART and the Android 6.0.1
# framework (docs/topics/fw6-crypto.md). Builds into build/art and links
# into artvm (topic fw6-core's FW6_JNI_LIBS / FW6_LIBS):
#
#   BoringSSL      external/boringssl of 6.0.1, C only (OPENSSL_NO_ASM), as
#                  libcrypto/libssl for wasm32
#   libjavacrypto  conscrypt 6.0's natives (org.conscrypt.NativeCrypto behind
#                  the "AndroidOpenSSL" provider), linked in as for 4.4: its
#                  JNI_OnLoad is renamed (shim/art/static_jni_libs_art.cc
#                  answers dlopen("libjavacrypto.so")), an empty file in
#                  /system/lib for System.loadLibrary
#   libjavacore    the two modules topic art left out: NativeBN (BigInteger
#                  on BoringSSL's BIGNUM) and ExpatParser (SAX: expat, which
#                  topic fw6-graphics builds for Skia)
#   /system        etc/security/cacerts (the build host's CA bundle, the same
#                  as the Dalvik image's: tools/make_cacerts.py)
#
#   make fw6-crypto-libs     # the libraries
#   make fw6-crypto-test     # CryptoTest NetTest XmlTest on ART in Node
#
# Included by the Makefile after mk/topic-fw6-core.mk (alphabetical order).

# --- BoringSSL -----------------------------------------------------------------------
BSSL := $(A60)/external/boringssl
BSSL_INCLUDES := -I$(BSSL)/src/include
BSSL_CRYPTO_SRCS := $(addprefix $(BSSL)/,$(shell tools/mk_srcs.py $(BSSL)/sources.mk crypto_sources))
BSSL_SSL_SRCS := $(addprefix $(BSSL)/,$(shell tools/mk_srcs.py $(BSSL)/sources.mk ssl_sources))
$(AOBJ)/$(BSSL)/%: AFLAGS := $(BSSL_INCLUDES) -I$(BSSL)/src/crypto -DOPENSSL_NO_ASM \
    -Wno-unused-parameter -Wno-implicit-function-declaration
$(ALIB)/libcrypto.a: $(call aobj,$(BSSL_CRYPTO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(ALIB)/libssl.a: $(call aobj,$(BSSL_SSL_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libjavacrypto (conscrypt 6.0) and libjavacore's NativeBN, ExpatParser ------------
# JNI_JARJAR_PREFIX: conscrypt's classes are com.android.org.conscrypt in
# conscrypt.jar (jarjar), as in AOSP's libjavacrypto. NO_KEYSTORE_ENGINE:
# no keystore (system/security's keystore-engine): ENGINE_load_private_key
# throws, as conscrypt does unbundled.
JCRYPTO6_SRC := $(A60)/external/conscrypt/src/main/native/org_conscrypt_NativeCrypto.cpp
$(call aobj,$(JCRYPTO6_SRC)): AFLAGS := $(AJC_FLAGS) $(BSSL_INCLUDES) \
    '-DJNI_JARJAR_PREFIX=com/android/' -DJNI_OnLoad=arc_wasm_javacrypto_JNI_OnLoad \
    -DNO_KEYSTORE_ENGINE
# shim/art/javacore_register.cc refers to their register_* weakly, so the
# archive is linked whole (FW6_JNI_LIBS).
AJC_CRYPTO_SRCS := $(AJC)/java_math_NativeBN.cpp $(AJC)/org_apache_harmony_xml_ExpatParser.cpp
# <libexpat/expat.h>: AOSP copies expat's headers there (common_COPY_HEADERS_TO).
FC_EXPAT := $(A60)/external/expat
FC_EXPAT_HDRS := $(ABLD)/include/libexpat/expat.h $(ABLD)/include/libexpat/expat_external.h
$(ABLD)/include/libexpat/%.h: $(FC_EXPAT)/lib/%.h
	@mkdir -p $(dir $@) && cp $< $@
$(call aobj,$(AJC_CRYPTO_SRCS)): AFLAGS := $(AJC_FLAGS) $(BSSL_INCLUDES) -I$(ABLD)/include
$(call aobj,$(AJC)/org_apache_harmony_xml_ExpatParser.cpp): $(FC_EXPAT_HDRS)
$(ALIB)/libjavacrypto.a: $(call aobj,$(JCRYPTO6_SRC) $(AJC_CRYPTO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# expat itself: topic fw6-graphics' build of it (mk/topic-fw6-graphics.mk,
# included later; the same library Skia's font config parser uses).
FC_LIBEXPAT := $(BUILD)/fw6g/lib/libexpat.a
FC_LIBS := $(ALIB)/libssl.a $(ALIB)/libcrypto.a $(FC_LIBEXPAT)
FW6_JNI_LIBS += $(ALIB)/libjavacrypto.a
FW6_LIBS += $(FC_LIBS)
$(ABLD)/artvm.js $(AWEB)/artvm.js: $(ALIB)/libjavacrypto.a $(FC_LIBS)

.PHONY: fw6-crypto-libs
fw6-crypto-libs: $(ALIB)/libjavacrypto.a $(FC_LIBS)

# --- /system -----------------------------------------------------------------------
# System.loadLibrary("javacrypto") looks for the file in /system/lib before it
# reaches the statically linked library (as mk/topic-crypto.mk for Dalvik).
FC_LIB_STUB := $(ABLD)/empty-javacrypto.so
$(FC_LIB_STUB):
	@mkdir -p $(dir $@) && : > $@
# The CA store of conscrypt's TrustManager: the Dalvik image's (topic net,
# mk/topic-net.mk builds it; AOSP 6.0.1's own set in libcore is from 2015
# and lacks today's roots, e.g. ISRG Root X1).
FC_CACERTS := $(BUILD)/dalvik/cacerts
ASYSTEM_ADD += --add $(FC_LIB_STUB):system/lib/libjavacrypto.so \
               --add $(FC_CACERTS):system/etc/security/cacerts
$(ASYSTEM_IMAGE): $(FC_LIB_STUB) $(FC_CACERTS).stamp

# --- tests on ART ----------------------------------------------------------------------
# CryptoTest and NetTest: the Dalvik tests, unchanged sources. XmlTest
# (tests/java6): SAX through ExpatParser, XmlPullParser, DOM, android.util.Xml.
FC_TEST_SRCS := tests/java/arcwasm/CryptoTest.java tests/java/arcwasm/NetTest.java tests/java/arcwasm/NetPolicyTest.java \
                tests/java6/arcwasm/XmlTest.java
ART_JAVA_TESTS += $(FC_TEST_SRCS)
$(AFW)/arc-tests.jar: $(FC_TEST_SRCS)

.PHONY: fw6-crypto-test
fw6-crypto-test: art
	VM=art tools/run_java_tests.sh node CryptoTest NetTest XmlTest
