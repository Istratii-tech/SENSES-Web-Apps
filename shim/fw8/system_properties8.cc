// What bionic 8.0 added to the system property API, on top of the
// in-process store of shim/fw6/system_properties.cc (shared with the 6.0.1
// stack): __system_property_read_callback (libcutils 8.1's property_list,
// libbase's GetProperty) and __system_property_wait (libbase's
// WaitForProperty). Part of the arc-wasm shim layer.

#include <sys/_system_properties.h>

#include <errno.h>
#include <pthread.h>
#include <time.h>

extern "C" {

void __system_property_read_callback(const prop_info* pi,
                                     void (*callback)(void* cookie, const char* name,
                                                      const char* value, uint32_t serial),
                                     void* cookie) {
  char name[PROP_NAME_MAX];
  char value[PROP_VALUE_MAX];
  uint32_t serial = __system_property_serial(pi);
  __system_property_read(pi, name, value);
  callback(cookie, name, value, serial);
}

// Polls: the store signals changes only through __system_property_wait_any,
// which cannot time out. Property waits are rare (libbase WaitForProperty,
// for init's services) and short here.
bool __system_property_wait(const prop_info* pi, uint32_t old_serial, uint32_t* new_serial_ptr,
                            const struct timespec* relative_timeout) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    uint32_t serial = pi ? __system_property_serial(pi) : __system_property_area_serial();
    if (serial != old_serial) {
      if (new_serial_ptr) *new_serial_ptr = serial;
      return true;
    }
    if (relative_timeout) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      int64_t waited = (now.tv_sec - start.tv_sec) * 1000000000LL + (now.tv_nsec - start.tv_nsec);
      if (waited >= relative_timeout->tv_sec * 1000000000LL + relative_timeout->tv_nsec) {
        return false;
      }
    }
    struct timespec tick = {0, 10 * 1000 * 1000};
    nanosleep(&tick, nullptr);
  }
}

}  // extern "C"
