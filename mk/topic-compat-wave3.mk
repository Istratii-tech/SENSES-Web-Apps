# Topic compat-wave3 (docs/topics/compat-wave3.md): more apps by general means.

# A wasm trap in a thread that runs ARM or arm64 code reports the guest block,
# registers and last host calls on Android 10 too (as on 6.0.1 and 8.1).
ART10_LDFLAGS += --pre-js shim/app6/guest_trap_report.js
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(G10WEB)/artvm10.js: shim/app6/guest_trap_report.js
