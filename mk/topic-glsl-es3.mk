# Topic glsl-es3: guest GLSL ES 1.00 that WebGL2 compiles only as ESSL 3.00
# (Flutter's Impeller shaders: standard derivatives, loops to a uniform) is
# rewritten as 3.00 (shim/ndk/ndk_glsl.cc, used by ndk_egl.cc; the sources
# are in the bridge's lists of mk/topic-ndk-imports.mk and the fw*-system ones).
#
#   make ndk-glsl-test    # the rewrites as strings, in Node
#
# Whether WebGL2 compiles and draws with them: Ndk6Test's pbuffer check in a
# browser (tools/run_java_tests.sh chrome Ndk6Test).

NDK_GLSL_TEST := $(BUILD)/ndk/glsl_test.js
$(NDK_GLSL_TEST): $(call dobj,tests/ndk/glsl_test.cc shim/ndk/ndk_glsl.cc)
	@mkdir -p $(dir $@)
	$(EMXX) -pthread -sENVIRONMENT=node -sEXIT_RUNTIME=1 $^ -o $@

.PHONY: ndk-glsl-test
ndk-glsl-test: $(NDK_GLSL_TEST)
	$(NODE) $(NDK_GLSL_TEST)
