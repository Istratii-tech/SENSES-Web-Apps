# --- release: SENSES Web Apps to run on one's own computer (topic local-release) ---
#   make release        # build/release/SENSES-Web-Apps-VERSION-android10.zip: Android 10 only
#   make release-all    # build/release/SENSES-Web-Apps-VERSION-all.zip: 4.4, 6.0.1, 8.1 and 10
# One zip a person downloads and unpacks; "Start SENSES" (macOS .command,
# Windows .bat, Linux .sh) runs tools/release/senses.py: a private HTTPS server
# with its own CA for the phones on the home network (docs/topics/local-release.md).
# The site is the shell built again with only the variant's systems
# (SENSES_STACKS) into its own folder: none of build/senses-apps' test files
# (__test, __sweep, vm/, s.html) and its systems once each (vm/aNN/system.img
# is only the input the .data is made from).
RELEASE := $(BUILD)/release
RELEASE_VERSION ?= $(shell date +%Y.%m.%d)-$(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
RELEASE_FILES := tools/make_release.py $(wildcard tools/release/*.py tools/release/*.txt tools/release/setup/* \
                 tools/release/start/* tools/release/licenses/*) tools/serve.py tools/net_relay.py

# $(1): variant, $(2): its systems
define release_variant
.PHONY: release-site-$(1)
release-site-$(1):
	$$(MAKE) senses-apps-web SAPPS=$$(RELEASE)/site-$(1) SENSES_STACKS="$(2)"
	@rm -f $$(RELEASE)/site-$(1)/s.html
endef
$(eval $(call release_variant,android10,a10))
$(eval $(call release_variant,all,a81 a10 a60 a44))

.PHONY: release release-all
release: release-site-android10 $(RELEASE_FILES)
	python3 tools/make_release.py --site $(RELEASE)/site-android10 --variant android10 \
	    --version $(RELEASE_VERSION) --out $(RELEASE) --emsdk "$(EMSDK)"
release-all: release-site-all $(RELEASE_FILES)
	python3 tools/make_release.py --site $(RELEASE)/site-all --variant all \
	    --version $(RELEASE_VERSION) --out $(RELEASE) --emsdk "$(EMSDK)"

# The launcher's own checks (certificates against openssl when present, QR
# codes decoded back, mDNS packets, the server end to end on a free port).
.PHONY: release-test
release-test:
	python3 tests/release/release_test.py

# The source of the GPL parts in the systems, as built (an asset of each
# release; NOTICE, 1.1): libcore's ojluni (GPL v2 with the Classpath
# Exception) of Android 8.1 and 10 -- the AOSP tag with patches/ applied --
# and libcore's licence and notice.
RELEASE_GPL := $(RELEASE)/SENSES-Web-Apps-$(RELEASE_VERSION)-gpl-sources.tar.gz
.PHONY: release-gpl-sources
release-gpl-sources:
	@mkdir -p $(RELEASE)
	printf '%s\n' "libcore/ojluni of Android 8.1 (android-8.1.0_r81) and Android 10 (android-10.0.0_r47)" \
	    "as built into SENSES Web Apps $(RELEASE_VERSION): the AOSP sources with the project's patches" \
	    "applied. Licence: GNU GPL version 2 with the Classpath Exception (LICENSE in each libcore)." \
	    > $(RELEASE)/GPL-SOURCES-README.txt
	tar -czf $(RELEASE_GPL) -s ',^third_party/,SENSES-Web-Apps-gpl-sources/,' \
	    -s ',^$(RELEASE)/,SENSES-Web-Apps-gpl-sources/,' $(RELEASE)/GPL-SOURCES-README.txt \
	    $(foreach a,aosp81 aosp10,third_party/$(a)/libcore/ojluni third_party/$(a)/libcore/LICENSE third_party/$(a)/libcore/NOTICE)
	@ls -la $(RELEASE_GPL)

# The public repository (docs/topics/local-release.md, "Publication"): what is
# under git here, without the internal notes, with tools/release/publication's
# README and NOTICE, checked by the personal-data gate before it is committed
# there (fresh history, signed istratiitech). PUBLICATION=folder (not in this
# checkout). git push is for a person to do.
.PHONY: publication
publication:
	@test -n "$(PUBLICATION)" || { echo "make publication PUBLICATION=FOLDER"; exit 1; }
	tools/make_publication.sh "$(PUBLICATION)" "$(RELEASE_VERSION)"
