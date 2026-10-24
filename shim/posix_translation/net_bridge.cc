// See net_bridge.h.

#include "posix_translation/net_bridge.h"

#include <arpa/inet.h>
#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "common/alog.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {
namespace net {

namespace {

// Keep in sync with net_transport.js.inc.
enum Event { kOpen = 1, kOpenFailed = 2, kData = 3, kEof = 4, kError = 5,
             kSent = 6 };
enum Op { kTcpOpen = 1, kUdpOpen = 2, kSend = 3, kSendTo = 4, kShutWr = 5,
          kClose = 6 };
enum DnsResult { kDnsOk = 0, kDnsNotFound = 1, kDnsAgain = 2 };

const char kTransportJs[] =
#include "posix_translation/net_transport.js.inc"
    ;  // NOLINT

}  // namespace

}  // namespace net
}  // namespace posix_translation

extern "C" {
// Called by the transport (on the net thread) for every connection event.
// Takes ownership of |data| and |str| (allocated with ArcNetAlloc).
static void ArcNetOnEvent(int id, int type, int a, int b, char* data, int len,
                          char* str);
static void* ArcNetAlloc(int size) { return malloc(size ? size : 1); }
static void ArcNetDnsDone(em_proxying_ctx* ctx) { emscripten_proxy_finish(ctx); }
// shim/kernel/arc_kernel.cc, when linked.
__attribute__((weak)) void arc_kernel_mark_kernel_thread();
}

// The JS side reaches C only through these function pointers (no exports
// needed, so any program linking posix_translation gets the network).
EM_JS_DEPS(arc_net, "$ERRNO_CODES,$UTF8ToString,$stringToUTF8,"
                    "$lengthBytesUTF8,$getWasmTableEntry");

// Runs the transport source with its context; returns 1 if there is one.
EM_JS(int, arc_net_js_init, (const char* src, void* event_fn, void* alloc_fn,
                             void* dns_done_fn), {
  var event = getWasmTableEntry(event_fn);
  var alloc = getWasmTableEntry(alloc_fn);
  var dnsDone = getWasmTableEntry(dns_done_fn);
  var newString = function(str) {
    var n = lengthBytesUTF8(str) + 1;
    var p = alloc(n) >>> 0;
    stringToUTF8(str, p, n);
    return p;
  };
  var ctx = {
    errno: ERRNO_CODES,
    event: function(id, type, a, b, bytes, str) {
      var p = 0, n = 0, s = 0;
      if (bytes && bytes.length) {
        n = bytes.length;
        p = alloc(n) >>> 0;
        HEAPU8.set(bytes, p);
      }
      if (str !== null && str !== undefined) s = newString(str);
      event(id, type, a, b, p, n, s);
    },
    require: (typeof require === "function") ? require : null,
    log: function(m) { err("arc-net: " + m); },
  };
  try {
    var t = new Function("ctx", UTF8ToString(src))(ctx);
    if (t) {
      t.newString = newString;
      t.dnsDone = dnsDone;
    }
    globalThis.arcNetTransport = t;
  } catch (e) {
    err("arc-net: transport failed to start: " + e);
    globalThis.arcNetTransport = null;
  }
  return globalThis.arcNetTransport ? 1 : 0;
});

EM_JS(void, arc_net_js_command, (int op, int id, const char* host, int port,
                                 int family, const char* data, int len), {
  var bytes = len ? HEAPU8.slice(data >>> 0, (data >>> 0) + len) : null;
  globalThis.arcNetTransport.command(op, id, host ? UTF8ToString(host) : "",
                                     port, family, bytes);
});

// Resolves on the net thread, then writes *result / *addrs (a malloc'ed
// space-separated list) and finishes the proxied call |ctx|.
EM_JS(void, arc_net_js_resolve, (void* ctx, const char* name, int family,
                                 int* result, char** addrs), {
  var t = globalThis.arcNetTransport;
  t.resolve(UTF8ToString(name), family, function(code, list) {
    HEAP32[result >>> 2] = code;
    HEAPU32[addrs >>> 2] = list.length ? t.newString(list.join(" ")) : 0;
    t.dnsDone(ctx);
  });
});

EM_JS(const char*, arc_net_js_kind, (), {
  return globalThis.arcNetTransport.newString(globalThis.arcNetTransport.kind);
});

namespace posix_translation {
namespace net {

namespace {

pthread_once_t g_once = PTHREAD_ONCE_INIT;
pthread_t g_thread;
pthread_mutex_t g_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_ready_cond = PTHREAD_COND_INITIALIZER;
bool g_ready = false;
bool g_available = false;

// id -> endpoint; guarded by the VFS mutex.
std::map<int, Endpoint*>* g_endpoints = NULL;
int g_next_id = 1;

struct Command {
  int op;
  int id;
  int port;
  int family;
  char host[INET6_ADDRSTRLEN];
  char* data;
  int len;
};

void* ThreadMain(void*) {
  // This thread holds the VFS mutex in its callbacks: none of its libc calls
  // (ALOG* writes to stderr) may route back into the VFS.
  if (arc_kernel_mark_kernel_thread)
    arc_kernel_mark_kernel_thread();
  pthread_mutex_lock(&g_ready_mutex);
  g_available =
      arc_net_js_init(kTransportJs, reinterpret_cast<void*>(&ArcNetOnEvent),
                      reinterpret_cast<void*>(&ArcNetAlloc),
                      reinterpret_cast<void*>(&ArcNetDnsDone)) != 0;
  if (g_available) {
    const char* kind = arc_net_js_kind();
    ALOGI("net: transport %s", kind);
    free(const_cast<char*>(kind));
  }
  g_ready = true;
  pthread_cond_broadcast(&g_ready_cond);
  pthread_mutex_unlock(&g_ready_mutex);
  // Serve proxied commands and the transport's callbacks from the worker's
  // event loop (see opfs_bridge.cc for why the keepalive is needed).
  emscripten_runtime_keepalive_push();
  emscripten_unwind_to_js_event_loop();
  return NULL;
}

void StartOnce() {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  const bool started = pthread_create(&g_thread, &attr, ThreadMain, NULL) == 0;
  pthread_attr_destroy(&attr);
  if (!started) {
    ALOGE("net: cannot start the net thread");
    return;
  }
  pthread_mutex_lock(&g_ready_mutex);
  while (!g_ready)
    pthread_cond_wait(&g_ready_cond, &g_ready_mutex);
  pthread_mutex_unlock(&g_ready_mutex);
  if (!g_available)
    ALOGW("net: no transport (no WebSocket relay and not Node)");
}

void RunCommand(void* arg) {
  Command* cmd = static_cast<Command*>(arg);
  arc_net_js_command(cmd->op, cmd->id, cmd->host, cmd->port, cmd->family,
                     cmd->data, cmd->len);
  free(cmd->data);
  delete cmd;
}

void Post(Command* cmd) {
  if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(), g_thread,
                              RunCommand, cmd)) {
    ALOGE("net: cannot post to the net thread");
    free(cmd->data);
    delete cmd;
  }
}

Command* NewCommand(int op, int id) {
  Command* cmd = new Command();
  cmd->op = op;
  cmd->id = id;
  return cmd;
}

int Register(Endpoint* endpoint) {
  VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
  if (!g_endpoints)
    g_endpoints = new std::map<int, Endpoint*>;
  const int id = g_next_id++;
  (*g_endpoints)[id] = endpoint;
  return id;
}

struct DnsRequest {
  const char* name;
  int family;
  int result;
  char* addrs;
};

void DnsDispatch(em_proxying_ctx* ctx, void* arg) {
  DnsRequest* req = static_cast<DnsRequest*>(arg);
  arc_net_js_resolve(ctx, req->name, req->family, &req->result, &req->addrs);
}

// ARC_NET and ARC_NET_DENY (net_bridge.h), read once.
pthread_once_t g_policy_once = PTHREAD_ONCE_INIT;
bool g_enabled = true;
std::vector<std::string>* g_deny = NULL;

std::string HostKey(const char* name) {
  std::string s(name ? name : "");
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z')
      s[i] = s[i] - 'A' + 'a';
  }
  while (!s.empty() && s[s.size() - 1] == '.')
    s.resize(s.size() - 1);
  return s;
}

void ReadPolicy() {
  const char* net = getenv("ARC_NET");
  g_enabled = !(net && strcmp(net, "0") == 0);
  g_deny = new std::vector<std::string>;
  const char* deny = getenv("ARC_NET_DENY");
  // Names separated by commas, spaces or semicolons; "*.x" and ".x" are x.
  std::string item;
  for (const char* p = deny ? deny : "";; ++p) {
    if (*p && *p != ',' && *p != ' ' && *p != ';' && *p != '\n') {
      item += *p;
      continue;
    }
    size_t start = 0;
    while (start < item.size() && (item[start] == '*' || item[start] == '.'))
      ++start;
    std::string n = HostKey(item.c_str() + start);
    if (!n.empty())
      g_deny->push_back(n);
    item.clear();
    if (!*p)
      break;
  }
  if (!g_enabled)
    ALOGI("net: no network for this app (ARC_NET=0)");
  if (!g_deny->empty())
    ALOGI("net: %zu names denied (ARC_NET_DENY=%s)", g_deny->size(), deny);
}

}  // namespace

bool Enabled() {
  pthread_once(&g_policy_once, ReadPolicy);
  return g_enabled;
}

bool Denied(const char* name) {
  pthread_once(&g_policy_once, ReadPolicy);
  if (g_deny->empty())
    return false;
  const std::string n = HostKey(name);
  for (size_t i = 0; i < g_deny->size(); ++i) {
    const std::string& d = (*g_deny)[i];
    if (n == d || (n.size() > d.size() && n[n.size() - d.size() - 1] == '.' &&
                   n.compare(n.size() - d.size(), d.size(), d) == 0)) {
      return true;
    }
  }
  return false;
}

bool Start() {
  if (!Enabled())
    return false;
  pthread_once(&g_once, StartOnce);
  return g_available;
}

int OpenTcp(Endpoint* endpoint, const sockaddr_storage& peer) {
  const int id = Register(endpoint);
  Command* cmd = NewCommand(kTcpOpen, id);
  cmd->port = FormatAddress(peer, cmd->host, sizeof(cmd->host));
  Post(cmd);
  return id;
}

int OpenUdp(Endpoint* endpoint, int family) {
  const int id = Register(endpoint);
  Command* cmd = NewCommand(kUdpOpen, id);
  cmd->family = (family == AF_INET6) ? 6 : 4;
  Post(cmd);
  return id;
}

void Send(int id, const void* data, size_t len, const sockaddr_storage* to) {
  Command* cmd = NewCommand(to ? kSendTo : kSend, id);
  size_t header = 0;
  uint8_t prefix[19];
  if (to) {
    // [4|6][port BE][address]: the relay's datagram framing.
    if (to->ss_family == AF_INET6) {
      const sockaddr_in6* sin6 = reinterpret_cast<const sockaddr_in6*>(to);
      if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
        prefix[0] = 4;
        memcpy(prefix + 3, &sin6->sin6_addr.s6_addr[12], 4);
        header = 7;
      } else {
        prefix[0] = 6;
        memcpy(prefix + 3, &sin6->sin6_addr, 16);
        header = 19;
      }
      memcpy(prefix + 1, &sin6->sin6_port, 2);
    } else {
      const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(to);
      prefix[0] = 4;
      memcpy(prefix + 1, &sin->sin_port, 2);
      memcpy(prefix + 3, &sin->sin_addr, 4);
      header = 7;
    }
  }
  cmd->len = header + len;
  cmd->data = static_cast<char*>(malloc(cmd->len ? cmd->len : 1));
  memcpy(cmd->data, prefix, header);
  memcpy(cmd->data + header, data, len);
  Post(cmd);
}

void ShutdownWrite(int id) {
  Post(NewCommand(kShutWr, id));
}

void Close(int id) {
  VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
  if (g_endpoints)
    g_endpoints->erase(id);
  Post(NewCommand(kClose, id));
}

int Resolve(const char* name, int family, std::vector<sockaddr_storage>* out) {
  // The app's own policy first (net_bridge.h): nothing leaves the process.
  if (Denied(name)) {
    static int logged;
    if (logged++ < 50)
      ALOGI("net: %s: not found (ARC_NET_DENY)", name);
    return EAI_NONAME;
  }
  if (!Enabled())
    return EAI_AGAIN;
  if (emscripten_is_main_browser_thread()) {
    ALOGE("net: DNS lookups block; not allowed on the main thread (%s)", name);
    return EAI_AGAIN;
  }
  if (!Start())
    return EAI_AGAIN;
  DnsRequest req = {name, family == AF_INET ? 4 : family == AF_INET6 ? 6 : 0,
                    kDnsAgain, NULL};
  if (!emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(),
                                      g_thread, DnsDispatch, &req)) {
    return EAI_AGAIN;
  }
  if (req.result == kDnsOk && req.addrs) {
    char* save = NULL;
    for (char* tok = strtok_r(req.addrs, " ", &save); tok;
         tok = strtok_r(NULL, " ", &save)) {
      sockaddr_storage ss;
      if (ParseAddress(tok, 0, &ss) &&
          (family == AF_UNSPEC || ss.ss_family == family)) {
        out->push_back(ss);
      }
    }
  }
  free(req.addrs);
  if (req.result == kDnsOk)
    return out->empty() ? EAI_NONAME : 0;
  return req.result == kDnsNotFound ? EAI_NONAME : EAI_AGAIN;
}

int FormatAddress(const sockaddr_storage& addr, char* host, size_t host_len) {
  if (addr.ss_family == AF_INET) {
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(&addr);
    inet_ntop(AF_INET, &sin->sin_addr, host, host_len);
    return ntohs(sin->sin_port);
  }
  if (addr.ss_family == AF_INET6) {
    const sockaddr_in6* sin6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
      inet_ntop(AF_INET, &sin6->sin6_addr.s6_addr[12], host, host_len);
    else
      inet_ntop(AF_INET6, &sin6->sin6_addr, host, host_len);
    return ntohs(sin6->sin6_port);
  }
  if (host_len)
    host[0] = '\0';
  return -1;
}

bool ParseAddress(const char* host, int port, sockaddr_storage* out) {
  memset(out, 0, sizeof(*out));
  sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(out);
  if (inet_pton(AF_INET, host, &sin->sin_addr) == 1) {
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    return true;
  }
  sockaddr_in6* sin6 = reinterpret_cast<sockaddr_in6*>(out);
  if (inet_pton(AF_INET6, host, &sin6->sin6_addr) == 1) {
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(port);
    return true;
  }
  return false;
}

}  // namespace net
}  // namespace posix_translation

static void ArcNetOnEvent(int id, int type, int a, int b, char* data, int len,
                          char* str) {
  using posix_translation::VirtualFileSystem;
  using posix_translation::net::Endpoint;
  using posix_translation::net::g_endpoints;
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  if (sys) {
    base::AutoLock lock(sys->mutex());
    Endpoint* endpoint = NULL;
    if (g_endpoints) {
      std::map<int, Endpoint*>::iterator it = g_endpoints->find(id);
      if (it != g_endpoints->end())
        endpoint = it->second;
    }
    if (endpoint) {
      sockaddr_storage addr;
      if (!str || !posix_translation::net::ParseAddress(str, a, &addr)) {
        memset(&addr, 0, sizeof(addr));
        addr.ss_family = AF_UNSPEC;
      }
      switch (type) {
        case posix_translation::net::kOpen:
          endpoint->OnOpen(addr);
          break;
        case posix_translation::net::kOpenFailed:
          endpoint->OnOpenFailed(a);
          break;
        case posix_translation::net::kData:
          endpoint->OnData(data, len, addr);
          break;
        case posix_translation::net::kEof:
          endpoint->OnEof();
          break;
        case posix_translation::net::kError:
          endpoint->OnError(a);
          break;
        case posix_translation::net::kSent:
          endpoint->OnSent(a);
          break;
      }
    }
    sys->Broadcast();
  }
  free(data);
  free(str);
}
