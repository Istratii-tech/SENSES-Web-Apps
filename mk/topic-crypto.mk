# Topic "crypto": OpenSSL 1.0.1 (external/openssl, AOSP 4.4.4) built the way
# its Android.mk builds it for an unknown architecture (C only, no asm),
# conscrypt's natives (libjavacrypto: org.conscrypt.NativeCrypto, behind
# the "AndroidOpenSSL" security provider) and libcore's NativeBN
# (java.math.BigInteger), which needs OpenSSL's BIGNUM.
OSSL := $(A44)/external/openssl
OSSL_CFLAGS := $(filter-out -DTERMIO,$(shell tools/mk_srcs.py $(OSSL)/build-config.mk openssl_cflags)) \
    -DNO_WINDOWS_BRAINDEATH '-DOPENSSLDIR="/system/lib/ssl"' \
    '-DENGINESDIR="/system/lib/ssl/engines"' -Wno-implicit-function-declaration \
    -Wno-int-conversion -Wno-incompatible-pointer-types
OSSL_INCLUDES := $(addprefix -I$(OSSL)/,. crypto crypto/asn1 crypto/evp crypto/modes include include/openssl)

OSSL_CRYPTO_SRCS := $(addprefix $(OSSL)/,$(shell tools/mk_srcs.py $(OSSL)/Crypto-config.mk common_src_files))
OSSL_SSL_SRCS := $(addprefix $(OSSL)/,$(shell tools/mk_srcs.py $(OSSL)/Ssl-config.mk common_src_files))
$(DOBJ)/$(OSSL)/%: XFLAGS := $(OSSL_INCLUDES) $(OSSL_CFLAGS)
$(BUILD)/libcrypto.a: $(call dobj,$(OSSL_CRYPTO_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^
$(BUILD)/libssl.a: $(call dobj,$(OSSL_SSL_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# libjavacrypto, linked in: its JNI_OnLoad is renamed so that
# shim/dalvik/static_jni_libs.cc can answer dlopen("libjavacrypto.so").
# JNI_JARJAR_PREFIX: conscrypt's classes live in com.android.org.conscrypt
# in core (jarjar), as in AOSP's libjavacrypto.
JCRYPTO_SRC := $(A44)/libcore/crypto/src/main/native/org_conscrypt_NativeCrypto.cpp
$(call dobj,$(JCRYPTO_SRC)): XFLAGS := $(D_INCLUDES) $(OSSL_INCLUDES) -I$(JC) \
    '-DJNI_JARJAR_PREFIX=com/android/' -DJNI_OnLoad=arc_wasm_javacrypto_JNI_OnLoad
# NativeBN belongs to libjavacore (shim/dalvik/javacore_register.cc
# registers it); it is built here because it needs OpenSSL.
NATIVEBN_SRC := $(JC)/java_math_NativeBN.cpp
$(call dobj,$(NATIVEBN_SRC)): XFLAGS := $(D_INCLUDES) $(ICU_INCLUDES) $(OSSL_INCLUDES) -I$(JC)
$(BUILD)/libjavacrypto.a: $(call dobj,$(JCRYPTO_SRC) $(NATIVEBN_SRC))
	@rm -f $@ && $(EMAR) rcs $@ $^

CRYPTO_LIBS := $(BUILD)/libjavacrypto.a $(BUILD)/libssl.a $(BUILD)/libcrypto.a
DALVIK_LIBS += $(CRYPTO_LIBS)

.PHONY: crypto-libs
crypto-libs: $(CRYPTO_LIBS)

# System.loadLibrary("javacrypto") looks for the file in /system/lib before
# it reaches the statically linked library; an empty placeholder, like
# libmedia_jni.so in mk/topic-gles-audio.mk.
CRYPTO_LIB_STUB := $(BUILD)/dalvik/empty-javacrypto.so
$(CRYPTO_LIB_STUB):
	@mkdir -p $(dir $@) && : > $@
DSYSTEM_ADD += --add $(CRYPTO_LIB_STUB):system/lib/libjavacrypto.so
$(BUILD)/dalvik/system.img: $(CRYPTO_LIB_STUB)
