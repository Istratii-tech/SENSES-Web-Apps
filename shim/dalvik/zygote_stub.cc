// Replaces dalvik/vm/native/dalvik_system_Zygote.cpp. The zygote forks app
// processes and sets up SELinux contexts, mount namespaces and capabilities;
// none of that exists in a browser, where each app runs in its own page.
// With no entries here, calls to dalvik.system.Zygote natives throw
// UnsatisfiedLinkError. Part of the arc-wasm shim layer.

#include "Dalvik.h"
#include "native/InternalNativePriv.h"

const DalvikNativeMethod dvm_dalvik_system_Zygote[] = {
    { NULL, NULL, NULL },
};
