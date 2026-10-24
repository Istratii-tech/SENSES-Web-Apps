// The system property store of the Android 6.0.1 stack: bionic's
// __system_property_* (what libcutils 6.0's properties.c, SystemProperties'
// natives and the framework use) kept in this process. On a device init
// owns the store and processes read a shared memory area; here there is one
// process, so the store is a map behind a mutex, and setting a property
// changes it directly (the property service's job). prop_info entries are
// never freed: pointers from __system_property_find stay valid, as bionic's.
// arc_wasm_load_properties() loads /system/build.prop at startup as init's
// load_properties_from_file does. Part of the arc-wasm shim layer.

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_system_properties.h>

#include <map>
#include <string>
#include <vector>

struct prop_info {
  char name[PROP_NAME_MAX];
  char value[PROP_VALUE_MAX];
  unsigned serial;
};

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;
unsigned g_area_serial;

// Insertion order for __system_property_find_nth/foreach, by name for find.
std::vector<prop_info*>& Props() {
  static std::vector<prop_info*>* v = new std::vector<prop_info*>;
  return *v;
}
std::map<std::string, prop_info*>& Index() {
  static std::map<std::string, prop_info*>* m = new std::map<std::string, prop_info*>;
  return *m;
}

class Lock {
 public:
  Lock() { pthread_mutex_lock(&g_lock); }
  ~Lock() { pthread_mutex_unlock(&g_lock); }
};

}  // namespace

extern "C" {

const prop_info* __system_property_find(const char* name) {
  Lock l;
  auto it = Index().find(name);
  return it == Index().end() ? nullptr : it->second;
}

int __system_property_read(const prop_info* pi, char* name, char* value) {
  Lock l;
  if (name)
    strcpy(name, pi->name);
  strcpy(value, pi->value);
  return strlen(pi->value);
}

int __system_property_get(const char* name, char* value) {
  const prop_info* pi = __system_property_find(name);
  if (!pi) {
    value[0] = 0;
    return 0;
  }
  return __system_property_read(pi, nullptr, value);
}

int __system_property_set(const char* key, const char* value) {
  if (!key || !value)
    return -1;
  const size_t namelen = strlen(key), valuelen = strlen(value);
  if (namelen == 0 || namelen >= PROP_NAME_MAX || valuelen >= PROP_VALUE_MAX)
    return -1;
  Lock l;
  auto it = Index().find(key);
  prop_info* pi;
  if (it != Index().end()) {
    pi = it->second;
    // ro.* properties are set once (property_service.c).
    if (strncmp(key, "ro.", 3) == 0)
      return -1;
  } else {
    pi = new prop_info();
    memcpy(pi->name, key, namelen + 1);
    Props().push_back(pi);
    Index()[key] = pi;
  }
  memcpy(pi->value, value, valuelen + 1);
  pi->serial += 2;  // bionic: even = stable
  ++g_area_serial;
  pthread_cond_broadcast(&g_changed);
  return 0;
}

const prop_info* __system_property_find_nth(unsigned n) {
  Lock l;
  return n < Props().size() ? Props()[n] : nullptr;
}

int __system_property_foreach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
  std::vector<prop_info*> props;
  {
    Lock l;
    props = Props();
  }
  for (prop_info* pi : props)
    propfn(pi, cookie);
  return 0;
}

unsigned int __system_property_serial(const prop_info* pi) {
  Lock l;
  return pi->serial;
}

unsigned int __system_property_area_serial(void) {
  Lock l;
  return g_area_serial;
}

unsigned int __system_property_wait_any(unsigned int serial) {
  Lock l;
  while (g_area_serial == serial)
    pthread_cond_wait(&g_changed, &g_lock);
  return g_area_serial;
}

// Loads key=value lines ('#' comments) as init's load_properties_from_file
// (system/core/init/property_service.cpp) does. Returns the number of
// properties set, or -1 if the file cannot be read.
int arc_wasm_load_properties(const char* path) {
  // The page's switches that are properties, set before the first file as
  // init sets ro.boot.* from the kernel command line before build.prop:
  // ro.arcwasm.jit from $ARC_JIT -- "0": no code generated at run time
  // (SENSES Web Apps' per-app "without JIT", docs/topics/guest-jit.md),
  // otherwise "1"; ro.arcwasm.gms from $ARC_GMS -- "0": no Google Play
  // services (docs/topics/gms.md), otherwise "1"; ro.arcwasm.net from
  // $ARC_NET -- "0": the app has no network (SENSES Web Apps' per-app
  // "Сеть: выкл", docs/topics/app-network.md), otherwise "1".
  static bool switches_done;
  if (!switches_done) {
    switches_done = true;
    const char* jit = getenv("ARC_JIT");
    __system_property_set("ro.arcwasm.jit", jit && strcmp(jit, "0") == 0 ? "0" : "1");
    const char* gms = getenv("ARC_GMS");
    __system_property_set("ro.arcwasm.gms", gms && strcmp(gms, "0") == 0 ? "0" : "1");
    const char* net = getenv("ARC_NET");
    __system_property_set("ro.arcwasm.net", net && strcmp(net, "0") == 0 ? "0" : "1");
  }
  FILE* f = fopen(path, "r");
  if (!f)
    return -1;
  char line[PROP_NAME_MAX + PROP_VALUE_MAX + 64];
  int n = 0;
  while (fgets(line, sizeof(line), f)) {
    char* key = line;
    while (isspace(static_cast<unsigned char>(*key)))
      key++;
    if (*key == '#')
      continue;
    char* value = strchr(key, '=');
    if (!value)
      continue;
    *value++ = 0;
    for (char* t = value - 2; t >= key && isspace(static_cast<unsigned char>(*t)); t--)
      *t = 0;
    while (isspace(static_cast<unsigned char>(*value)))
      value++;
    for (char* t = value + strlen(value) - 1; t >= value && isspace(static_cast<unsigned char>(*t)); t--)
      *t = 0;
    if (__system_property_set(key, value) == 0)
      n++;
  }
  fclose(f);
  return n;
}

}  // extern "C"
