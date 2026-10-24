// Which pages of a typed array's memory are resident (mincore), for Node: the
// wasm memory's pages the VM touched -- read or written -- since an untouched
// page of anonymous memory is not resident (topic wasm-heap-trim; used by
// arc_mem_scan_print in shim/membudget/mem_budget.cc with ARC_MINCORE set to
// this addon's path).
//
//   cc -O2 -shared -undefined dynamic_lookup -I/opt/homebrew/include/node \
//       mincore_addon.c -o mincore.node
//
// residency(heapU8, out, pages, pageSize): heapU8[out + i] = 1 if page i of
// heapU8's memory (pages of pageSize bytes from its start) is resident or was
// (compressed or paged out: macOS compresses pages a while untouched).
#include <node_api.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static napi_value Residency(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_typedarray_type type;
  size_t length, offset;
  void* data;
  napi_value buffer;
  if (argc < 4 || napi_get_typedarray_info(env, argv[0], &type, &length, &data, &buffer, &offset) != napi_ok)
    return NULL;
  uint32_t out, pages, page;
  napi_get_value_uint32(env, argv[1], &out);
  napi_get_value_uint32(env, argv[2], &pages);
  napi_get_value_uint32(env, argv[3], &page);
  const size_t sys = (size_t)getpagesize();
  const size_t n = (size_t)pages * page / sys;
  char* vec = malloc(n);
  int ok = vec && mincore(data, n * sys, vec) == 0;
  if (ok) {
    unsigned char* base = data;
    const size_t per = page / sys;
    for (size_t i = 0; i < pages; i++) {
      unsigned char r = 0;
      for (size_t j = 0; j < per; j++)
        r |= (vec[i * per + j] & (MINCORE_INCORE | MINCORE_PAGED_OUT)) != 0;
      base[out + i] = r;
    }
  }
  free(vec);
  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  napi_create_function(env, "residency", NAPI_AUTO_LENGTH, Residency, NULL, &fn);
  napi_set_named_property(env, exports, "residency", fn);
  return exports;
}
