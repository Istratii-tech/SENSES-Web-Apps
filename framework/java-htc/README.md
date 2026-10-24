# HTC Sense stand-ins

Java replacements for classes of HTC's framework libraries (HTC One m7,
Sense 5.5) that cannot work in arc-wasm as they are, mostly because their
natives are in HTC's ARM libraries (System.loadLibrary). Topic userspace,
docs/topics/userspace.md.

tools/fetch_system_apks_htc.sh compiles these against our framework and
puts each class in place of HTC's class of the same name in the boot jar
that has it (build/system-apkshtc/framework/htc-*.jar). A stand-in keeps
the HTC class's API: every member the HTC code uses.

| Class | HTC's needs | Here |
|---|---|---|
| com.htc.htcjavaflag.Native | libhtcflag-jni.so: build flags | Java: a user build, the image's locale |
