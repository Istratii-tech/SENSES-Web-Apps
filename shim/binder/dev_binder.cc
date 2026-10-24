// /dev/binder for posix_translation: the Android binder driver (the 3.4
// kernel's drivers/staging/android/binder.c, protocol version 7) emulated
// inside the single WebAssembly process. Part of the arc-wasm shim layer.
//
// Each open() is a binder process (Proc): its nodes (objects it serves),
// refs (handles it holds to other processes' nodes), threads and
// transaction buffers. libbinder in the Java process opens it once;
// in-process stand-ins for other Android processes (servicemanager, test
// services) open it again and so are separate binder processes. The
// driver does what the kernel does between them: it copies transaction
// data, translates objects (a node of the sender becomes a handle in the
// receiver, a handle to a node of the receiver becomes the node again,
// a file descriptor is duplicated), keeps the reference counts that make
// the owner hold its objects alive (BR_INCREFS/BR_ACQUIRE/BR_RELEASE/
// BR_DECREFS), routes replies back along the transaction stacks (a nested
// call back into a waiting process goes to the waiting thread), queues
// one-way calls per node in order, asks for looper threads (BR_SPAWN_LOOPER)
// and delivers death notifications when a process closes its descriptor.
//
// Light processes (topic multiproc, shim/multiproc/include/arc_lproc.h):
// the threads of one open() can belong to different Android processes of
// the VM (the app's main process and those of its other android:process
// components share libbinder's one ProcessState, i.e. one descriptor). The
// descriptor (File) holds what libbinder sees per descriptor: the handles
// (refs), the objects it sent (nodes) and the transaction buffers; each
// light process using it is a binder process (Proc) of its own: its threads,
// its incoming work, its pid and uid. A node belongs to the process that
// serves it (who sent it first, or what arc_binder_note_owner says), so a
// call to it goes to that process's threads, is answered with the caller's
// pid, and fails with BR_DEAD_REPLY once that process was killed
// (arc_lproc_kill -> KillProc), when holders of its handles get
// BR_DEAD_BINDER. A handle to a node of another process of the same
// descriptor is a handle like any other; a handle to a node of the
// receiving process becomes the object again. Without light processes
// every descriptor has one Proc, as before.
//
// Everything runs under the VFS mutex, like every other stream; a thread
// waiting for work sleeps on the VFS condition variable (which releases
// the mutex), as pipes and sockets do. Since all processes share one
// address space, transaction buffers are plain heap blocks handed to the
// receiver by pointer; mmap() of the device only has to succeed.

#include "binder/dev_binder.h"

#include <linux/binder.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <vector>

#include "arc_binder.h"
#include "common/alog.h"
#include "../multiproc/include/arc_lproc.h"
#include "posix_translation/dir.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

// Looper threads a process may be asked to start (BR_SPAWN_LOOPER), at most.
// libbinder asks for 15; each one is a Web Worker here, so fewer.
const int kMaxLoopers = 4;

enum LooperState {
  kLooperRegistered = 0x01,
  kLooperEntered = 0x02,
  kLooperExited = 0x04,
  kLooperInvalid = 0x08,
  kLooperWaiting = 0x10,
  kLooperNeedReturn = 0x20,
};

enum WorkType {
  kWorkTransaction,
  kWorkTransactionComplete,
  kWorkNode,
  kWorkDeadBinder,
  kWorkDeadBinderAndClear,
  kWorkClearDeathNotification,
};

struct Work;
typedef std::list<Work*> WorkList;

struct Work {
  explicit Work(WorkType t) : type(t), list(NULL) {}
  virtual ~Work() {}
  WorkType type;
  WorkList* list;  // where it is queued, if anywhere
};

void Enqueue(WorkList* list, Work* w) {
  ALOG_ASSERT(!w->list);
  list->push_back(w);
  w->list = list;
}

void Dequeue(Work* w) {
  if (w->list) {
    w->list->remove(w);
    w->list = NULL;
  }
}

struct File;
struct Proc;
struct Thread;
struct Ref;
struct Transaction;

struct Buffer {
  uint8_t* data;
  size_t data_size;
  size_t offsets_size;
  Transaction* transaction;
  struct Node* target_node;
  bool async;
  bool allow_user_free;
  size_t* offsets() const {
    return reinterpret_cast<size_t*>(
        data + ((data_size + sizeof(void*) - 1) & ~(sizeof(void*) - 1)));
  }
};

struct Node : Work {
  Node() : Work(kWorkNode) {}
  Proc* proc;  // the process that serves it; NULL once it is dead
  File* file;  // whose nodes map holds it; NULL once it is dead
  uintptr_t ptr;
  uintptr_t cookie;
  int internal_strong_refs = 0;
  int local_strong_refs = 0;
  int local_weak_refs = 0;
  std::set<Ref*> refs;
  bool has_strong_ref = false;
  bool pending_strong_ref = false;
  bool has_weak_ref = false;
  bool pending_weak_ref = false;
  bool has_async_transaction = false;
  bool accept_fds = false;
  WorkList async_todo;
};

struct Death : Work {
  Death() : Work(kWorkDeadBinder) {}
  uintptr_t cookie;
  Proc* proc;  // the process that asked for it: where BR_DEAD_BINDER goes
};

struct Ref {
  File* file;
  Node* node;
  uint32_t desc;
  int strong = 0;
  int weak = 0;
  Death* death = NULL;
};

struct Transaction : Work {
  Transaction() : Work(kWorkTransaction) {}
  Thread* from = NULL;
  Transaction* from_parent = NULL;
  Proc* to_proc = NULL;
  Thread* to_thread = NULL;
  Transaction* to_parent = NULL;
  bool need_reply = false;
  Buffer* buffer = NULL;
  uint32_t code = 0;
  uint32_t flags = 0;
  uid_t sender_euid = 0;
};

struct Thread {
  Proc* proc;
  pthread_t id;
  int looper = kLooperNeedReturn;
  Transaction* transaction_stack = NULL;
  WorkList todo;
  uint32_t return_error = BR_OK;
  uint32_t return_error2 = BR_OK;
};

// A binder process: the threads of one light process on one descriptor.
struct Proc {
  File* file;
  pid_t pid;
  uid_t euid;
  std::map<pthread_t, Thread*> threads;
  WorkList todo;
  int max_threads = 0;
  int requested_threads = 0;
  int requested_threads_started = 0;
  int ready_threads = 0;
  bool dead = false;
};

// One open() of the device: what libbinder's ProcessState sees.
struct File {
  Proc* main;                  // the opener's process (the VM's, or a stand-in's)
  std::map<int, Proc*> light;  // light pid -> its binder process here
  std::map<uintptr_t, Node*> nodes;
  std::map<uint32_t, Ref*> refs_by_desc;
  std::map<Node*, Ref*> refs_by_node;
  std::map<uintptr_t, Buffer*> buffers;
  WorkList delivered_death;
  int max_threads = 0;
  bool dead = false;
};

// Driver-wide state, guarded by the VFS mutex.
Node* g_context_mgr_node = NULL;
std::set<Node*> g_dead_nodes;
std::set<File*> g_files;
// The VM's own descriptor (libbinder's ProcessState): the first one opened
// without a stand-in identity.
File* g_vm_file = NULL;
// JavaBBinder (cookie) -> the light process its Java object belongs to
// (arc_binder_note_owner): a node first sent by another process's thread
// still belongs to its own.
std::map<uintptr_t, int> g_owners;

// See arc_binder_set_process_identity; -1: the real process's.
thread_local int g_open_pid = -1;
thread_local int g_open_uid = -1;

VirtualFileSystem* Vfs() {
  return VirtualFileSystem::GetVirtualFileSystem();
}

void Wake() {
  Vfs()->Broadcast();
}

// --- nodes and refs ------------------------------------------------------

Node* GetNode(File* file, uintptr_t ptr) {
  std::map<uintptr_t, Node*>::iterator it = file->nodes.find(ptr);
  return it == file->nodes.end() ? NULL : it->second;
}

Node* NewNode(Proc* proc, uintptr_t ptr, uintptr_t cookie) {
  Node* node = new Node;
  node->proc = proc;
  node->file = proc->file;
  node->ptr = ptr;
  node->cookie = cookie;
  proc->file->nodes[ptr] = node;
  return node;
}

void DeleteNode(Node* node) {
  Dequeue(node);
  if (node->file)
    node->file->nodes.erase(node->ptr);
  else
    g_dead_nodes.erase(node);
  delete node;
}

void IncNode(Node* node, bool strong, bool internal, WorkList* target_list) {
  if (strong) {
    if (internal)
      node->internal_strong_refs++;
    else
      node->local_strong_refs++;
    if (!node->has_strong_ref && target_list && !node->list)
      Enqueue(target_list, node);
  } else {
    if (!internal)
      node->local_weak_refs++;
    if (!node->has_weak_ref && target_list && !node->list)
      Enqueue(target_list, node);
  }
}

void DecNode(Node* node, bool strong, bool internal) {
  if (strong) {
    if (internal)
      node->internal_strong_refs--;
    else
      node->local_strong_refs--;
    if (node->local_strong_refs || node->internal_strong_refs)
      return;
  } else {
    if (!internal)
      node->local_weak_refs--;
    if (node->local_weak_refs || !node->refs.empty())
      return;
  }
  if (node->proc && (node->has_strong_ref || node->has_weak_ref)) {
    // Let the owner drop the references it took for the driver.
    if (!node->list) {
      Enqueue(&node->proc->todo, node);
      Wake();
    }
  } else if (node->refs.empty() && !node->local_strong_refs &&
             !node->local_weak_refs) {
    DeleteNode(node);
  }
}

Ref* GetRef(File* file, uint32_t desc) {
  std::map<uint32_t, Ref*>::iterator it = file->refs_by_desc.find(desc);
  return it == file->refs_by_desc.end() ? NULL : it->second;
}

Ref* GetRefForNode(File* file, Node* node) {
  std::map<Node*, Ref*>::iterator it = file->refs_by_node.find(node);
  if (it != file->refs_by_node.end())
    return it->second;
  Ref* ref = new Ref;
  ref->file = file;
  ref->node = node;
  // Handle 0 is the context manager; the rest take the lowest free number.
  uint32_t desc = node == g_context_mgr_node ? 0 : 1;
  for (std::map<uint32_t, Ref*>::iterator r = file->refs_by_desc.begin();
       r != file->refs_by_desc.end(); ++r) {
    if (r->first > desc)
      break;
    desc = r->first + 1;
  }
  ref->desc = desc;
  file->refs_by_desc[desc] = ref;
  file->refs_by_node[node] = ref;
  node->refs.insert(ref);
  return ref;
}

void DeleteRef(Ref* ref) {
  Node* node = ref->node;
  ref->file->refs_by_desc.erase(ref->desc);
  ref->file->refs_by_node.erase(node);
  if (ref->strong)
    DecNode(node, true, true);
  node->refs.erase(ref);
  DecNode(node, false, true);
  if (ref->death) {
    Dequeue(ref->death);
    delete ref->death;
  }
  delete ref;
}

void IncRef(Ref* ref, bool strong, WorkList* target_list) {
  if (strong) {
    if (ref->strong == 0)
      IncNode(ref->node, true, true, target_list);
    ref->strong++;
  } else {
    if (ref->weak == 0)
      IncNode(ref->node, false, true, target_list);
    ref->weak++;
  }
}

int DecRef(Ref* ref, bool strong) {
  if (strong) {
    if (ref->strong == 0) {
      ALOGE("binder: invalid dec strong, ref %u", ref->desc);
      return -EINVAL;
    }
    if (--ref->strong == 0)
      DecNode(ref->node, true, true);
  } else {
    if (ref->weak == 0) {
      ALOGE("binder: invalid dec weak, ref %u", ref->desc);
      return -EINVAL;
    }
    ref->weak--;
  }
  if (ref->strong == 0 && ref->weak == 0)
    DeleteRef(ref);
  return 0;
}

// --- buffers and transactions --------------------------------------------

Buffer* AllocBuffer(File* file, size_t data_size, size_t offsets_size,
                    bool async) {
  Buffer* b = new Buffer;
  const size_t aligned = (data_size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
  b->data = static_cast<uint8_t*>(malloc(aligned + offsets_size + 1));
  b->data_size = data_size;
  b->offsets_size = offsets_size;
  b->transaction = NULL;
  b->target_node = NULL;
  b->async = async;
  b->allow_user_free = false;
  file->buffers[reinterpret_cast<uintptr_t>(b->data)] = b;
  return b;
}

void FreeBuffer(File* file, Buffer* b) {
  file->buffers.erase(reinterpret_cast<uintptr_t>(b->data));
  free(b->data);
  delete b;
}

// Drops the references a buffer holds on the objects in it (and on its
// target). |failed_at| is the end of the translated objects when a
// transaction failed half-way, else NULL.
void BufferRelease(File* file, Buffer* buffer, size_t* failed_at) {
  if (buffer->target_node)
    DecNode(buffer->target_node, true, false);
  size_t* offp = buffer->offsets();
  size_t* off_end = failed_at
      ? failed_at
      : offp + buffer->offsets_size / sizeof(size_t);
  for (; offp < off_end; offp++) {
    if (*offp > buffer->data_size - sizeof(flat_binder_object) ||
        buffer->data_size < sizeof(flat_binder_object) ||
        (*offp & (sizeof(uint32_t) - 1)))
      continue;
    flat_binder_object* fp =
        reinterpret_cast<flat_binder_object*>(buffer->data + *offp);
    switch (fp->type) {
      case BINDER_TYPE_BINDER:
      case BINDER_TYPE_WEAK_BINDER: {
        Node* node = GetNode(file, reinterpret_cast<uintptr_t>(fp->binder));
        if (node)
          DecNode(node, fp->type == BINDER_TYPE_BINDER, false);
        break;
      }
      case BINDER_TYPE_HANDLE:
      case BINDER_TYPE_WEAK_HANDLE: {
        Ref* ref = GetRef(file, fp->handle);
        if (ref)
          DecRef(ref, fp->type == BINDER_TYPE_HANDLE);
        break;
      }
      case BINDER_TYPE_FD:
        if (failed_at)
          Vfs()->CloseLocked(fp->handle);
        break;
    }
  }
}

void PopTransaction(Thread* target_thread, Transaction* t) {
  if (target_thread) {
    ALOG_ASSERT(target_thread->transaction_stack == t);
    target_thread->transaction_stack = t->from_parent;
    t->from = NULL;
  }
  t->need_reply = false;
  if (t->buffer)
    t->buffer->transaction = NULL;
  delete t;
}

void SendFailedReply(Transaction* t, uint32_t error_code) {
  while (true) {
    Thread* target_thread = t->from;
    if (target_thread) {
      if (target_thread->return_error != BR_OK &&
          target_thread->return_error2 == BR_OK) {
        target_thread->return_error2 = target_thread->return_error;
        target_thread->return_error = BR_OK;
      }
      if (target_thread->return_error == BR_OK) {
        PopTransaction(target_thread, t);
        target_thread->return_error = error_code;
        Wake();
      } else {
        ALOGE("binder: reply failed, target thread has error %u",
              target_thread->return_error);
      }
      return;
    }
    Transaction* next = t->from_parent;
    PopTransaction(NULL, t);
    if (!next)
      return;
    t = next;
  }
}

// --- light processes ------------------------------------------------------

// The binder process of light process |pid| on |file| (the opener's for
// the VM's pid), made on first use.
Proc* ProcForPid(File* file, int pid) {
  if (pid == arc_lproc_main_pid())
    return file->main;
  std::map<int, Proc*>::iterator it = file->light.find(pid);
  if (it != file->light.end())
    return it->second;
  Proc* proc = new Proc;
  proc->file = file;
  proc->pid = pid;
  proc->euid = arc_lproc_uid(pid);
  proc->max_threads = file->max_threads;
  proc->dead = file->dead || !arc_lproc_alive(pid);
  file->light[pid] = proc;
  return proc;
}

// Who serves a node first sent by |sender|: the process its object belongs
// to, if known, else the sender's.
Proc* OwnerOf(Proc* sender, uintptr_t cookie) {
  std::map<uintptr_t, int>::iterator it = g_owners.find(cookie);
  if (it == g_owners.end() || it->second == sender->pid)
    return sender;
  return ProcForPid(sender->file, it->second);
}

void DoTransaction(Proc* proc, Thread* thread,
                   const binder_transaction_data* tr, bool reply) {
  Transaction* in_reply_to = NULL;
  Proc* target_proc = NULL;
  Thread* target_thread = NULL;
  Node* target_node = NULL;
  WorkList* target_list;
  uint32_t return_error = BR_OK;
  Transaction* t = NULL;
  Work* tcomplete = NULL;
  size_t* offp = NULL;
  size_t* off_end = NULL;

  if (reply) {
    in_reply_to = thread->transaction_stack;
    if (!in_reply_to) {
      ALOGE("binder: reply with no transaction stack");
      return_error = BR_FAILED_REPLY;
      goto err_no_stack;
    }
    if (in_reply_to->to_thread != thread) {
      ALOGE("binder: reply on a transaction of another thread");
      return_error = BR_FAILED_REPLY;
      in_reply_to = NULL;
      goto err_no_stack;
    }
    thread->transaction_stack = in_reply_to->to_parent;
    target_thread = in_reply_to->from;
    if (!target_thread) {
      return_error = BR_DEAD_REPLY;
      goto err_no_stack;
    }
    if (target_thread->transaction_stack != in_reply_to) {
      ALOGE("binder: reply target is not waiting for it");
      return_error = BR_FAILED_REPLY;
      in_reply_to = NULL;
      target_thread = NULL;
      goto err_no_stack;
    }
    target_proc = target_thread->proc;
  } else {
    if (tr->target.handle) {
      Ref* ref = GetRef(proc->file, tr->target.handle);
      if (!ref) {
        ALOGE("binder: transaction to invalid handle %u",
              static_cast<unsigned>(tr->target.handle));
        return_error = BR_FAILED_REPLY;
        goto err_no_stack;
      }
      target_node = ref->node;
    } else {
      target_node = g_context_mgr_node;
      if (!target_node) {
        return_error = BR_DEAD_REPLY;
        goto err_no_stack;
      }
    }
    target_proc = target_node->proc;
    if (!target_proc || target_proc->dead) {
      return_error = BR_DEAD_REPLY;
      goto err_no_stack;
    }
    if (!(tr->flags & TF_ONE_WAY) && thread->transaction_stack) {
      Transaction* tmp = thread->transaction_stack;
      if (tmp->to_thread != thread) {
        ALOGE("binder: new transaction while not handling the top one");
        return_error = BR_FAILED_REPLY;
        goto err_no_stack;
      }
      // A call back into a process that waits on this thread goes to the
      // thread that waits.
      for (; tmp; tmp = tmp->from_parent) {
        if (tmp->from && tmp->from->proc == target_proc)
          target_thread = tmp->from;
      }
    }
  }
  target_list = target_thread ? &target_thread->todo : &target_proc->todo;

  t = new Transaction;
  tcomplete = new Work(kWorkTransactionComplete);
  if (!reply && !(tr->flags & TF_ONE_WAY))
    t->from = thread;
  t->sender_euid = proc->euid;
  t->to_proc = target_proc;
  t->to_thread = target_thread;
  t->code = tr->code;
  t->flags = tr->flags;
  t->buffer = AllocBuffer(target_proc->file, tr->data_size, tr->offsets_size,
                          !reply && (t->flags & TF_ONE_WAY));
  t->buffer->transaction = t;
  t->buffer->target_node = target_node;
  if (target_node)
    IncNode(target_node, true, false, NULL);

  offp = t->buffer->offsets();
  if (tr->data_size)
    memcpy(t->buffer->data, tr->data.ptr.buffer, tr->data_size);
  if (tr->offsets_size)
    memcpy(offp, tr->data.ptr.offsets, tr->offsets_size);
  if (tr->offsets_size % sizeof(size_t)) {
    ALOGE("binder: bad offsets size %zu", tr->offsets_size);
    return_error = BR_FAILED_REPLY;
    goto err_bad_offset;
  }
  off_end = offp + tr->offsets_size / sizeof(size_t);
  for (; offp < off_end; offp++) {
    if (t->buffer->data_size < sizeof(flat_binder_object) ||
        *offp > t->buffer->data_size - sizeof(flat_binder_object) ||
        (*offp & (sizeof(uint32_t) - 1))) {
      ALOGE("binder: bad object offset %zu", *offp);
      return_error = BR_FAILED_REPLY;
      goto err_bad_offset;
    }
    flat_binder_object* fp =
        reinterpret_cast<flat_binder_object*>(t->buffer->data + *offp);
    switch (fp->type) {
      case BINDER_TYPE_BINDER:
      case BINDER_TYPE_WEAK_BINDER: {
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(fp->binder);
        const uintptr_t cookie = reinterpret_cast<uintptr_t>(fp->cookie);
        Node* node = GetNode(proc->file, ptr);
        if (!node) {
          Proc* owner = OwnerOf(proc, cookie);
          if (owner->dead) {
            return_error = BR_FAILED_REPLY;
            goto err_bad_offset;
          }
          node = NewNode(owner, ptr, cookie);
          node->accept_fds = fp->flags & FLAT_BINDER_FLAG_ACCEPTS_FDS;
        }
        if (node->cookie != cookie) {
          ALOGE("binder: node %p sent with a different cookie",
                reinterpret_cast<void*>(ptr));
          return_error = BR_FAILED_REPLY;
          goto err_bad_offset;
        }
        if (node->proc == target_proc) {
          // An object of the receiver, sent by another process of its
          // descriptor that had it as an object (light processes): itself.
          // The owner must hold it before the sender lets its Parcel go (a
          // one-way call returns at once): BR_ACQUIRE comes first, as for
          // a new handle.
          IncNode(node, fp->type == BINDER_TYPE_BINDER, false, &thread->todo);
          break;
        }
        Ref* ref = GetRefForNode(target_proc->file, node);
        fp->type = fp->type == BINDER_TYPE_BINDER ? BINDER_TYPE_HANDLE
                                                  : BINDER_TYPE_WEAK_HANDLE;
        fp->handle = ref->desc;
        // The owner is told to take references before the sender continues
        // (by this thread: one address space, BR_ACQUIRE only counts).
        IncRef(ref, fp->type == BINDER_TYPE_HANDLE, &thread->todo);
        break;
      }
      case BINDER_TYPE_HANDLE:
      case BINDER_TYPE_WEAK_HANDLE: {
        Ref* ref = GetRef(proc->file, fp->handle);
        if (!ref) {
          ALOGE("binder: transaction with invalid handle %ld", fp->handle);
          return_error = BR_FAILED_REPLY;
          goto err_bad_offset;
        }
        if (ref->node->proc == target_proc) {
          fp->type = fp->type == BINDER_TYPE_HANDLE ? BINDER_TYPE_BINDER
                                                    : BINDER_TYPE_WEAK_BINDER;
          fp->binder = reinterpret_cast<void*>(ref->node->ptr);
          fp->cookie = reinterpret_cast<void*>(ref->node->cookie);
          IncNode(ref->node, fp->type == BINDER_TYPE_BINDER, false, NULL);
        } else {
          Ref* new_ref = GetRefForNode(target_proc->file, ref->node);
          fp->handle = new_ref->desc;
          IncRef(new_ref, fp->type == BINDER_TYPE_HANDLE, NULL);
        }
        break;
      }
      case BINDER_TYPE_FD: {
        if (reply ? !(in_reply_to->flags & TF_ACCEPT_FDS)
                  : !target_node->accept_fds) {
          ALOGE("binder: target does not accept file descriptors");
          return_error = BR_FAILED_REPLY;
          goto err_bad_offset;
        }
        // One address space and one descriptor table: the receiver gets
        // its own duplicate, as it would get a new descriptor in its
        // process.
        const int fd = Vfs()->DupLocked(fp->handle, -1);
        if (fd < 0) {
          ALOGE("binder: cannot pass descriptor %ld", fp->handle);
          return_error = BR_FAILED_REPLY;
          goto err_bad_offset;
        }
        fp->handle = fd;
        break;
      }
      default:
        ALOGE("binder: bad object type %lx", fp->type);
        return_error = BR_FAILED_REPLY;
        goto err_bad_offset;
    }
  }

  if (reply) {
    PopTransaction(target_thread, in_reply_to);
  } else if (!(t->flags & TF_ONE_WAY)) {
    t->need_reply = true;
    t->from_parent = thread->transaction_stack;
    thread->transaction_stack = t;
  } else if (target_node->has_async_transaction) {
    // One-way calls to a node run one at a time, in order: the next one is
    // released when the receiver frees the buffer of the previous one.
    target_list = &target_node->async_todo;
  } else {
    target_node->has_async_transaction = true;
  }
  Enqueue(target_list, t);
  Enqueue(&thread->todo, tcomplete);
  Wake();
  return;

err_bad_offset:
  BufferRelease(target_proc->file, t->buffer, offp);
  t->buffer->transaction = NULL;
  FreeBuffer(target_proc->file, t->buffer);
  delete t;
  delete tcomplete;
err_no_stack:
  if (in_reply_to) {
    thread->return_error = BR_TRANSACTION_COMPLETE;
    SendFailedReply(in_reply_to, return_error);
  } else {
    thread->return_error = return_error;
  }
}

// --- thread and process teardown -----------------------------------------

void ReleaseWorkItem(Work* w) {
  {
    switch (w->type) {
      case kWorkTransaction: {
        Transaction* t = static_cast<Transaction*>(w);
        if (t->buffer && t->buffer->target_node &&
            !(t->flags & TF_ONE_WAY)) {
          SendFailedReply(t, BR_DEAD_REPLY);
        } else {
          if (t->buffer)
            t->buffer->transaction = NULL;
          delete t;
        }
        break;
      }
      case kWorkTransactionComplete:
        delete w;
        break;
      case kWorkNode:
        break;
      case kWorkDeadBinder:
      case kWorkDeadBinderAndClear:
      case kWorkClearDeathNotification:
        // Owned by its ref unless cleared already; DeleteRef frees those.
        if (w->type != kWorkDeadBinder)
          delete w;
        break;
    }
  }
}

void ReleaseWork(WorkList* list) {
  while (!list->empty()) {
    Work* w = list->front();
    Dequeue(w);
    ReleaseWorkItem(w);
  }
}

void FreeThread(Proc* proc, Thread* thread) {
  Transaction* t = thread->transaction_stack;
  Transaction* send_reply = NULL;
  if (t && t->to_thread == thread)
    send_reply = t;
  while (t) {
    if (t->to_thread == thread) {
      t->to_proc = NULL;
      t->to_thread = NULL;
      if (t->buffer) {
        t->buffer->transaction = NULL;
        t->buffer = NULL;
      }
      t = t->to_parent;
    } else if (t->from == thread) {
      t->from = NULL;
      t = t->from_parent;
    } else {
      ALOG_ASSERT(false, "binder: bad transaction stack");
      break;
    }
  }
  thread->transaction_stack = NULL;
  if (send_reply)
    SendFailedReply(send_reply, BR_DEAD_REPLY);
  ReleaseWork(&thread->todo);
}

// Where a death notice goes: the process that asked, else (it is dead
// too) the opener's process of its descriptor, else nowhere.
Proc* DeathTarget(Ref* ref) {
  Death* death = ref->death;
  if (death->proc && !death->proc->dead)
    return death->proc;
  File* file = ref->file;
  return !file->dead && !file->main->dead ? file->main : NULL;
}

// A process died (its light process was killed, or its descriptor closed):
// its threads stop, its pending calls fail, other processes holding handles
// to its nodes get BR_DEAD_BINDER. Proc and Thread objects stay allocated:
// a thread may still be inside an ioctl on it (it returns EBADF).
void KillProc(Proc* proc) {
  if (proc->dead)
    return;
  proc->dead = true;
  File* file = proc->file;
  if (g_context_mgr_node && g_context_mgr_node->proc == proc)
    g_context_mgr_node = NULL;
  for (std::map<pthread_t, Thread*>::iterator it = proc->threads.begin();
       it != proc->threads.end(); ++it)
    FreeThread(proc, it->second);
  std::vector<Node*> nodes;
  for (std::map<uintptr_t, Node*>::iterator it = file->nodes.begin();
       it != file->nodes.end(); ++it) {
    if (it->second->proc == proc)
      nodes.push_back(it->second);
  }
  for (size_t i = 0; i < nodes.size(); i++) {
    Node* node = nodes[i];
    file->nodes.erase(node->ptr);
    Dequeue(node);
    ReleaseWork(&node->async_todo);
    node->proc = NULL;
    node->file = NULL;
    if (node->refs.empty()) {
      delete node;
      continue;
    }
    node->local_strong_refs = 0;
    node->local_weak_refs = 0;
    g_dead_nodes.insert(node);
    for (std::set<Ref*>::iterator r = node->refs.begin();
         r != node->refs.end(); ++r) {
      Death* death = (*r)->death;
      if (death && !death->list) {
        Proc* target = DeathTarget(*r);
        if (target) {
          death->type = kWorkDeadBinder;
          Enqueue(&target->todo, death);
        }
      }
    }
  }
  ReleaseWork(&proc->todo);
  Wake();
}

// The descriptor's last reference was closed: every process on it dies,
// its handles go away.
void ReleaseFile(File* file) {
  file->dead = true;
  for (std::map<int, Proc*>::iterator it = file->light.begin();
       it != file->light.end(); ++it)
    KillProc(it->second);
  KillProc(file->main);
  while (!file->refs_by_desc.empty())
    DeleteRef(file->refs_by_desc.begin()->second);
  while (!file->delivered_death.empty()) {
    Work* w = file->delivered_death.front();
    Dequeue(w);
    delete w;  // cleared ones; the others went with their refs
  }
  // Like the kernel, only freed: the nodes and refs their objects name are
  // gone already.
  while (!file->buffers.empty()) {
    Buffer* b = file->buffers.begin()->second;
    if (b->transaction) {
      b->transaction->buffer = NULL;
      b->transaction = NULL;
    }
    FreeBuffer(file, b);
  }
  g_files.erase(file);
  if (g_vm_file == file)
    g_vm_file = NULL;
  Wake();
}

// arc_lproc_kill's hook: light process |pid|'s binder processes die.
void KillLightProcess(int pid) {
  base::AutoLock lock(Vfs()->mutex());
  for (std::set<File*>::iterator it = g_files.begin(); it != g_files.end(); ++it) {
    std::map<int, Proc*>::iterator p = (*it)->light.find(pid);
    if (p != (*it)->light.end())
      KillProc(p->second);
  }
}

// --- the read/write protocol ---------------------------------------------

template <typename T>
bool Get(uint8_t** ptr, uint8_t* end, T* out) {
  if (end - *ptr < static_cast<ptrdiff_t>(sizeof(T)))
    return false;
  memcpy(out, *ptr, sizeof(T));
  *ptr += sizeof(T);
  return true;
}

template <typename T>
void Put(uint8_t** ptr, const T& v) {
  memcpy(*ptr, &v, sizeof(T));
  *ptr += sizeof(T);
}

WorkList* DeathList(Proc* proc, Thread* thread) {
  return (thread->looper & (kLooperRegistered | kLooperEntered))
      ? &thread->todo : &proc->todo;
}

int ThreadWrite(Proc* proc, Thread* thread, uint8_t* buffer, size_t size,
                signed long* consumed) {
  uint8_t* ptr = buffer + *consumed;
  uint8_t* end = buffer + size;
  while (ptr < end && thread->return_error == BR_OK) {
    uint32_t cmd;
    if (!Get(&ptr, end, &cmd))
      return -EFAULT;
    switch (cmd) {
      case BC_INCREFS:
      case BC_ACQUIRE:
      case BC_RELEASE:
      case BC_DECREFS: {
        uint32_t target;
        if (!Get(&ptr, end, &target))
          return -EFAULT;
        Ref* ref;
        if (target == 0 && g_context_mgr_node &&
            (cmd == BC_INCREFS || cmd == BC_ACQUIRE))
          ref = GetRefForNode(proc->file, g_context_mgr_node);
        else
          ref = GetRef(proc->file, target);
        if (!ref) {
          ALOGE("binder: refcount change on invalid handle %u", target);
          break;
        }
        if (cmd == BC_INCREFS)
          IncRef(ref, false, NULL);
        else if (cmd == BC_ACQUIRE)
          IncRef(ref, true, NULL);
        else
          DecRef(ref, cmd == BC_RELEASE);
        break;
      }
      case BC_INCREFS_DONE:
      case BC_ACQUIRE_DONE: {
        void* node_ptr;
        void* cookie;
        if (!Get(&ptr, end, &node_ptr) || !Get(&ptr, end, &cookie))
          return -EFAULT;
        Node* node = GetNode(proc->file, reinterpret_cast<uintptr_t>(node_ptr));
        if (!node || node->cookie != reinterpret_cast<uintptr_t>(cookie)) {
          ALOGE("binder: %s for unknown node %p",
                cmd == BC_ACQUIRE_DONE ? "BC_ACQUIRE_DONE" : "BC_INCREFS_DONE",
                node_ptr);
          break;
        }
        if (cmd == BC_ACQUIRE_DONE) {
          if (!node->pending_strong_ref) {
            ALOGE("binder: BC_ACQUIRE_DONE without a pending BR_ACQUIRE");
            break;
          }
          node->pending_strong_ref = false;
        } else {
          if (!node->pending_weak_ref) {
            ALOGE("binder: BC_INCREFS_DONE without a pending BR_INCREFS");
            break;
          }
          node->pending_weak_ref = false;
        }
        DecNode(node, cmd == BC_ACQUIRE_DONE, false);
        break;
      }
      case BC_FREE_BUFFER: {
        void* data_ptr;
        if (!Get(&ptr, end, &data_ptr))
          return -EFAULT;
        std::map<uintptr_t, Buffer*>::iterator it =
            proc->file->buffers.find(reinterpret_cast<uintptr_t>(data_ptr));
        if (it == proc->file->buffers.end() || !it->second->allow_user_free) {
          ALOGE("binder: BC_FREE_BUFFER of unknown buffer %p", data_ptr);
          break;
        }
        Buffer* b = it->second;
        if (b->transaction) {
          b->transaction->buffer = NULL;
          b->transaction = NULL;
        }
        if (b->async && b->target_node) {
          Node* node = b->target_node;
          if (node->async_todo.empty()) {
            node->has_async_transaction = false;
          } else {
            // The next one-way call goes to the node's process (the buffer
            // may be freed by another process's thread: a finalizer).
            Work* next = node->async_todo.front();
            Dequeue(next);
            if (node->proc == proc)
              Enqueue(&thread->todo, next);
            else if (node->proc)
              Enqueue(&node->proc->todo, next);
            else
              ReleaseWorkItem(next);
            Wake();
          }
        }
        BufferRelease(proc->file, b, NULL);
        FreeBuffer(proc->file, b);
        break;
      }
      case BC_TRANSACTION:
      case BC_REPLY: {
        binder_transaction_data tr;
        if (!Get(&ptr, end, &tr))
          return -EFAULT;
        DoTransaction(proc, thread, &tr, cmd == BC_REPLY);
        break;
      }
      case BC_REGISTER_LOOPER:
        if (thread->looper & kLooperEntered) {
          thread->looper |= kLooperInvalid;
        } else if (proc->requested_threads == 0) {
          thread->looper |= kLooperInvalid;
        } else {
          proc->requested_threads--;
          proc->requested_threads_started++;
        }
        thread->looper |= kLooperRegistered;
        break;
      case BC_ENTER_LOOPER:
        if (thread->looper & kLooperRegistered)
          thread->looper |= kLooperInvalid;
        thread->looper |= kLooperEntered;
        break;
      case BC_EXIT_LOOPER:
        thread->looper |= kLooperExited;
        break;
      case BC_REQUEST_DEATH_NOTIFICATION:
      case BC_CLEAR_DEATH_NOTIFICATION: {
        uint32_t target;
        void* cookie;
        if (!Get(&ptr, end, &target) || !Get(&ptr, end, &cookie))
          return -EFAULT;
        Ref* ref = GetRef(proc->file, target);
        if (!ref) {
          ALOGE("binder: death notification on invalid handle %u", target);
          break;
        }
        if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
          if (ref->death) {
            ALOGE("binder: death notification already set");
            break;
          }
          Death* death = new Death;
          death->cookie = reinterpret_cast<uintptr_t>(cookie);
          death->proc = proc;
          ref->death = death;
          if (!ref->node->proc) {
            Enqueue(DeathList(proc, thread), death);
            Wake();
          }
        } else {
          Death* death = ref->death;
          if (!death || death->cookie != reinterpret_cast<uintptr_t>(cookie)) {
            ALOGE("binder: clearing an unknown death notification");
            break;
          }
          ref->death = NULL;
          if (!death->list) {
            death->type = kWorkClearDeathNotification;
            Enqueue(DeathList(proc, thread), death);
            Wake();
          } else {
            death->type = kWorkDeadBinderAndClear;
          }
        }
        break;
      }
      case BC_DEAD_BINDER_DONE: {
        void* cookie;
        if (!Get(&ptr, end, &cookie))
          return -EFAULT;
        Death* death = NULL;
        for (WorkList::iterator it = proc->file->delivered_death.begin();
             it != proc->file->delivered_death.end(); ++it) {
          if (static_cast<Death*>(*it)->cookie ==
              reinterpret_cast<uintptr_t>(cookie)) {
            death = static_cast<Death*>(*it);
            break;
          }
        }
        if (!death) {
          ALOGE("binder: BC_DEAD_BINDER_DONE for unknown cookie %p", cookie);
          break;
        }
        Dequeue(death);
        if (death->type == kWorkDeadBinderAndClear) {
          death->type = kWorkClearDeathNotification;
          Enqueue(DeathList(proc, thread), death);
          Wake();
        }
        break;
      }
      default:
        ALOGE("binder: unknown command %x", cmd);
        return -EINVAL;
    }
    *consumed = ptr - buffer;
  }
  return 0;
}

bool HasThreadWork(Thread* thread) {
  return !thread->todo.empty() || thread->return_error != BR_OK ||
         (thread->looper & kLooperNeedReturn);
}

bool HasProcWork(Proc* proc, Thread* thread) {
  return !proc->todo.empty() || HasThreadWork(thread);
}

int ThreadRead(Proc* proc, Thread* thread, uint8_t* buffer, size_t size,
               signed long* consumed) {
  uint8_t* ptr = buffer + *consumed;
  uint8_t* end = buffer + size;
  if (*consumed == 0) {
    if (end - ptr < 4)
      return -EFAULT;
    Put<uint32_t>(&ptr, BR_NOOP);
  }

retry:
  {
    const bool wait_for_proc_work =
        thread->transaction_stack == NULL && thread->todo.empty();
    if (thread->return_error != BR_OK && end - ptr >= 4) {
      if (thread->return_error2 != BR_OK) {
        Put<uint32_t>(&ptr, thread->return_error2);
        thread->return_error2 = BR_OK;
        if (end - ptr < 4)
          goto done;
      }
      Put<uint32_t>(&ptr, thread->return_error);
      thread->return_error = BR_OK;
      goto done;
    }
    thread->looper |= kLooperWaiting;
    if (wait_for_proc_work)
      proc->ready_threads++;
    while (!proc->dead && !(wait_for_proc_work ? HasProcWork(proc, thread)
                                               : HasThreadWork(thread)))
      Vfs()->Wait();
    if (wait_for_proc_work)
      proc->ready_threads--;
    thread->looper &= ~kLooperWaiting;
    if (proc->dead)
      return -EBADF;

    while (true) {
      Work* w;
      if (!thread->todo.empty()) {
        w = thread->todo.front();
      } else if (!proc->todo.empty() && wait_for_proc_work) {
        w = proc->todo.front();
      } else {
        if (ptr - buffer == 4 && !(thread->looper & kLooperNeedReturn))
          goto retry;
        break;
      }
      if (end - ptr < static_cast<ptrdiff_t>(sizeof(binder_transaction_data) + 4))
        break;

      Transaction* t = NULL;
      switch (w->type) {
        case kWorkTransaction:
          t = static_cast<Transaction*>(w);
          break;
        case kWorkTransactionComplete:
          Put<uint32_t>(&ptr, BR_TRANSACTION_COMPLETE);
          Dequeue(w);
          delete w;
          break;
        case kWorkNode: {
          Node* node = static_cast<Node*>(w);
          const bool strong =
              node->internal_strong_refs || node->local_strong_refs;
          const bool weak =
              !node->refs.empty() || node->local_weak_refs || strong;
          uint32_t cmd = BR_NOOP;
          if (weak && !node->has_weak_ref) {
            cmd = BR_INCREFS;
            node->has_weak_ref = true;
            node->pending_weak_ref = true;
            node->local_weak_refs++;
          } else if (strong && !node->has_strong_ref) {
            cmd = BR_ACQUIRE;
            node->has_strong_ref = true;
            node->pending_strong_ref = true;
            node->local_strong_refs++;
          } else if (!strong && node->has_strong_ref) {
            cmd = BR_RELEASE;
            node->has_strong_ref = false;
          } else if (!weak && node->has_weak_ref) {
            cmd = BR_DECREFS;
            node->has_weak_ref = false;
          }
          if (cmd != BR_NOOP) {
            // Stays queued: the next round may have more to say about it.
            Put<uint32_t>(&ptr, cmd);
            Put<void*>(&ptr, reinterpret_cast<void*>(node->ptr));
            Put<void*>(&ptr, reinterpret_cast<void*>(node->cookie));
          } else {
            Dequeue(node);
            if (!weak && !strong)
              DeleteNode(node);
          }
          break;
        }
        case kWorkDeadBinder:
        case kWorkDeadBinderAndClear:
        case kWorkClearDeathNotification: {
          Death* death = static_cast<Death*>(w);
          const uint32_t cmd = w->type == kWorkClearDeathNotification
              ? BR_CLEAR_DEATH_NOTIFICATION_DONE : BR_DEAD_BINDER;
          Put<uint32_t>(&ptr, cmd);
          Put<void*>(&ptr, reinterpret_cast<void*>(death->cookie));
          Dequeue(death);
          if (cmd == BR_CLEAR_DEATH_NOTIFICATION_DONE)
            delete death;
          else
            Enqueue(&proc->file->delivered_death, death);
          if (cmd == BR_DEAD_BINDER)
            goto done;  // one death per read, as the kernel does
          break;
        }
      }
      if (!t)
        continue;

      binder_transaction_data tr;
      memset(&tr, 0, sizeof(tr));
      uint32_t cmd;
      if (t->buffer->target_node) {
        Node* target = t->buffer->target_node;
        tr.target.ptr = reinterpret_cast<void*>(target->ptr);
        tr.cookie = reinterpret_cast<void*>(target->cookie);
        cmd = BR_TRANSACTION;
      } else {
        cmd = BR_REPLY;
      }
      tr.code = t->code;
      tr.flags = t->flags;
      tr.sender_euid = t->sender_euid;
      tr.sender_pid = t->from ? t->from->proc->pid : 0;
      tr.data_size = t->buffer->data_size;
      tr.offsets_size = t->buffer->offsets_size;
      tr.data.ptr.buffer = t->buffer->data;
      tr.data.ptr.offsets = t->buffer->offsets();
      Put<uint32_t>(&ptr, cmd);
      Put(&ptr, tr);

      Dequeue(t);
      t->buffer->allow_user_free = true;
      if (cmd == BR_TRANSACTION && !(t->flags & TF_ONE_WAY)) {
        t->to_parent = thread->transaction_stack;
        t->to_thread = thread;
        thread->transaction_stack = t;
      } else {
        t->buffer->transaction = NULL;
        delete t;
      }
      break;
    }
  }

done:
  *consumed = ptr - buffer;
  if (proc->requested_threads + proc->ready_threads == 0 &&
      proc->requested_threads_started < proc->max_threads &&
      (thread->looper & (kLooperRegistered | kLooperEntered))) {
    proc->requested_threads++;
    const uint32_t spawn = BR_SPAWN_LOOPER;
    memcpy(buffer, &spawn, sizeof(spawn));
  }
  return 0;
}

int StatDevice(const std::string& pathname, struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  out->st_dev = 11;
  out->st_ino =
      VirtualFileSystem::GetVirtualFileSystem()->GetInodeLocked(pathname);
  out->st_mode = S_IFCHR | 0666;
  out->st_nlink = 1;
  out->st_rdev = DeviceHandler::GetDeviceId(pathname);
  out->st_blksize = 4096;
  return 0;
}

class DevBinder : public DeviceStream {
 public:
  DevBinder(const std::string& pathname, int oflag)
      : DeviceStream(oflag, pathname), file_(new File) {
    Proc* proc = new Proc;
    proc->file = file_;
    proc->pid = g_open_pid >= 0 ? g_open_pid : getpid();
    proc->euid = g_open_uid >= 0 ? g_open_uid : geteuid();
    file_->main = proc;
    g_files.insert(file_);
    if (!g_vm_file && g_open_pid < 0 && getpid() == arc_lproc_main_pid())
      g_vm_file = file_;
  }

  virtual int fstat(struct stat* out) OVERRIDE {
    return StatDevice(pathname(), out);
  }
  virtual ssize_t read(void* buf, size_t count) OVERRIDE {
    errno = EINVAL;
    return -1;
  }
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE {
    errno = EINVAL;
    return -1;
  }
  virtual void* mmap(void* addr, size_t length, int prot, int flags,
                     off_t offset) OVERRIDE {
    if (prot & PROT_WRITE) {
      errno = EPERM;
      return MAP_FAILED;
    }
    // Buffers are heap blocks (see the top of the file); the mapping only
    // reserves what the caller expects to exist.
    return ::mmap(addr, length, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1,
                  0);
  }
  virtual int munmap(void* addr, size_t length) OVERRIDE {
    return ::munmap(addr, length);
  }
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual const char* GetStreamType() const OVERRIDE { return "binder"; }

 protected:
  virtual ~DevBinder() {}
  virtual void OnLastFileRef() OVERRIDE { ReleaseFile(file_); }

 private:
  int WriteRead(Proc* proc, Thread* thread, binder_write_read* bwr);

  File* file_;
  DISALLOW_COPY_AND_ASSIGN(DevBinder);
};

Thread* GetThread(Proc* proc) {
  const pthread_t self = pthread_self();
  std::map<pthread_t, Thread*>::iterator it = proc->threads.find(self);
  if (it != proc->threads.end())
    return it->second;
  Thread* thread = new Thread;
  thread->proc = proc;
  thread->id = self;
  proc->threads[self] = thread;
  return thread;
}

int DevBinder::WriteRead(Proc* proc, Thread* thread, binder_write_read* bwr) {
  if (bwr->write_size > 0) {
    const int ret = ThreadWrite(proc, thread,
                                reinterpret_cast<uint8_t*>(bwr->write_buffer),
                                bwr->write_size, &bwr->write_consumed);
    if (ret < 0) {
      bwr->read_consumed = 0;
      return ret;
    }
  }
  if (bwr->read_size > 0) {
    const int ret = ThreadRead(proc, thread,
                               reinterpret_cast<uint8_t*>(bwr->read_buffer),
                               bwr->read_size, &bwr->read_consumed);
    if (!proc->todo.empty())
      Wake();
    if (ret < 0)
      return ret;
  }
  return 0;
}

int DevBinder::ioctl(int request, va_list ap) {
  void* arg = va_arg(ap, void*);
  // The calling thread's process: its light process's binder process here.
  Proc* proc = ProcForPid(file_, arc_lproc_current());
  if (proc->dead || file_->dead) {
    errno = EBADF;
    return -1;
  }
  Thread* thread = GetThread(proc);
  int ret = 0;
  switch (static_cast<unsigned>(request)) {
    case BINDER_WRITE_READ:
      if (!arg) {
        ret = -EFAULT;
        break;
      }
      ret = WriteRead(proc, thread, static_cast<binder_write_read*>(arg));
      break;
    case BINDER_SET_MAX_THREADS:
      file_->max_threads = std::min<int>(*static_cast<size_t*>(arg),
                                         kMaxLoopers);
      file_->main->max_threads = file_->max_threads;
      for (std::map<int, Proc*>::iterator it = file_->light.begin();
           it != file_->light.end(); ++it)
        it->second->max_threads = file_->max_threads;
      break;
    case BINDER_SET_CONTEXT_MGR: {
      if (g_context_mgr_node) {
        ret = -EBUSY;
        break;
      }
      Node* node = NewNode(proc, 0, 0);
      node->local_weak_refs++;
      node->local_strong_refs++;
      node->has_strong_ref = true;
      node->has_weak_ref = true;
      g_context_mgr_node = node;
      break;
    }
    case BINDER_THREAD_EXIT:
      FreeThread(proc, thread);
      proc->threads.erase(thread->id);
      delete thread;
      return 0;
    case BINDER_VERSION:
      if (!arg) {
        ret = -EFAULT;
        break;
      }
      static_cast<binder_version*>(arg)->protocol_version =
          BINDER_CURRENT_PROTOCOL_VERSION;
      break;
    case BINDER_SET_IDLE_TIMEOUT:
    case BINDER_SET_IDLE_PRIORITY:
      break;
    default:
      ret = -EINVAL;
      break;
  }
  if (!proc->dead)
    thread->looper &= ~kLooperNeedReturn;
  if (ret < 0) {
    errno = -ret;
    return -1;
  }
  return 0;
}

}  // namespace

DevBinderHandler::DevBinderHandler() : DeviceHandler("DevBinderHandler") {
  // misc device, as on Android
  DeviceHandler::AddDeviceId("/dev/binder", 10, 54);
}

DevBinderHandler::~DevBinderHandler() {
}

scoped_refptr<FileStream> DevBinderHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }
  return new DevBinder(pathname, oflag);
}

int DevBinderHandler::stat(const std::string& pathname, struct stat* out) {
  return StatDevice(pathname, out);
}

}  // namespace posix_translation

extern "C" void arc_binder_set_process_identity(int pid, int uid) {
  posix_translation::g_open_pid = pid;
  posix_translation::g_open_uid = uid;
}

// --- light processes (topic multiproc) ---------------------------------------

extern "C" void arc_binder_note_owner(void* cookie, int pid) {
  if (!arc_lproc_any())
    return;
  base::AutoLock lock(posix_translation::Vfs()->mutex());
  if (pid <= 0 || pid == arc_lproc_main_pid())
    posix_translation::g_owners.erase(reinterpret_cast<uintptr_t>(cookie));
  else
    posix_translation::g_owners[reinterpret_cast<uintptr_t>(cookie)] = pid;
}

extern "C" void arc_binder_forget_owner(void* cookie) {
  if (!arc_lproc_any())
    return;
  base::AutoLock lock(posix_translation::Vfs()->mutex());
  posix_translation::g_owners.erase(reinterpret_cast<uintptr_t>(cookie));
}

extern "C" int arc_binder_import_local(void* ptr, void* cookie, int owner_pid,
                                       int* take) {
  using namespace posix_translation;
  *take = 0;
  base::AutoLock lock(Vfs()->mutex());
  File* file = g_vm_file;
  if (!file || file->dead)
    return -EBADF;
  const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  const uintptr_t c = reinterpret_cast<uintptr_t>(cookie);
  Node* node = GetNode(file, p);
  if (!node) {
    Proc* owner = ProcForPid(file, owner_pid);
    if (owner->dead)
      return -EPIPE;
    node = NewNode(owner, p, c);
    node->accept_fds = true;
    // The caller takes the references BR_INCREFS/BR_ACQUIRE would make the
    // owner take, at once (one address space): the node is held from now.
    node->has_weak_ref = true;
    node->has_strong_ref = true;
    *take = 3;
  } else if (node->cookie != c) {
    ALOGE("binder: imported node %p with a different cookie", ptr);
    return -EINVAL;
  } else if (!node->proc || node->proc->dead) {
    return -EPIPE;
  }
  return GetRefForNode(file, node)->desc;
}

namespace {
struct KillHookInstaller {
  KillHookInstaller() { arc_lproc_set_kill_hook(posix_translation::KillLightProcess); }
} g_kill_hook_installer;
}  // namespace
