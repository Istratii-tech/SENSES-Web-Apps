# Topic net: TCP/UDP sockets and DNS for the kernel through a WebSocket relay
# (tools/net_relay.py, served by tools/serve.py) or, under Node, Node's own
# net/dgram/dns. Notes: docs/topics/net.md.
#
#   make net-test        # kernel network gtest under Node (direct transport;
#                        # ARC_NET_RELAY=ws://127.0.0.1:PORT/__net and
#                        # ARC_NET_ECHO_PORT=PORT+1 to go through serve.py)
#   make net-test-web    # build/net-test/: open /net-test/index.html through
#                        # serve.py (echo server at its port + 1)

# build/obj has no header dependencies: rebuild what embeds the socket
# classes' layout (virtual_file_system.cc news them) when their headers change.
NET_HEADERS := shim/posix_translation/tcp_socket.h shim/posix_translation/udp_socket.h \
               shim/posix_translation/net_bridge.h
$(call obj,$(UP)/posix_translation/virtual_file_system.cc shim/kernel/arc_kernel.cc \
           shim/posix_translation/net_bridge.cc shim/posix_translation/net_sockets.cc \
           shim/posix_translation/net_resolver.cc shim/posix_translation/net_interfaces.cc): $(NET_HEADERS)
$(call obj,shim/posix_translation/net_bridge.cc): shim/posix_translation/net_transport.js.inc

NET_TEST_OBJS := $(call obj,shim/kernel/arc_kernel.cc shim/kernel/arc_net_test.cc)
NET_TEST_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8 \
    -sINITIAL_MEMORY=256MB -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
    -sEXIT_RUNTIME=1 -sASSERTIONS=1

$(BUILD)/net_test.js: $(NET_TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) mk/topic-net.mk
	$(EMXX) $(NET_TEST_LDFLAGS) -sENVIRONMENT=node,worker \
	    $(NET_TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) -o $@

.PHONY: net-test net-test-web
net-test: $(BUILD)/net_test.js
	cd $(BUILD) && $(NODE) net_test.js $(GTEST_ARGS)

$(BUILD)/net-test/net_test.js: $(NET_TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) mk/topic-net.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(NET_TEST_LDFLAGS) -sENVIRONMENT=web,worker \
	    $(NET_TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) -o $@

$(BUILD)/net-test/index.html: tests/browser/index.html
	@mkdir -p $(dir $@)
	sed -e 's/ARC OPFS tests/ARC network tests/' -e 's/opfs_test.js/net_test.js/' $< > $@

$(BUILD)/net-test/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

net-test-web: $(BUILD)/net-test/net_test.js $(BUILD)/net-test/index.html $(BUILD)/net-test/report.js

# /system/etc/security/cacerts for conscrypt's TrustManager (HTTPS): the
# build host's CA bundle in Android's <subject_hash_old>.0 layout.
NET_CACERTS := $(BUILD)/dalvik/cacerts
$(NET_CACERTS).stamp: tools/make_cacerts.py
	python3 tools/make_cacerts.py $(NET_CACERTS)
	@touch $@
DSYSTEM_ADD += --add $(NET_CACERTS):system/etc/security/cacerts
$(BUILD)/dalvik/system.img: $(NET_CACERTS).stamp
