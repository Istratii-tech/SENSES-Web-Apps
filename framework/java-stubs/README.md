# framework stand-ins

Hand-written stand-ins for AOSP 4.4.4 framework classes whose real version
cannot work in arc-wasm yet. They are compiled into framework.jar /
framework2.jar together with the whole of frameworks/base
(tools/build_framework_jar.sh), so a stand-in must replace the real file,
not add to it: list the real file it replaces in this README and say why.

Currently there are none: framework.jar is the real frameworks/base. The
stand-ins of the first framework slice (Process, StrictMode,
TextUtils, TimeUtils, RuntimeInit, ParcelFileDescriptor) and
those added by later topics (Resources, StringBlock, ActivityInfo, View,
HardwareRenderer) went away with topic framework-jar
(docs/topics/framework-jar.md).
