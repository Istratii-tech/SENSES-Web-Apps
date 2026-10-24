// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_LOGD_SOCKET_NAMESPACE_H_
#define POSIX_TRANSLATION_LOGD_SOCKET_NAMESPACE_H_

// This file contains temporary implementation required to support UNIX
// domain sockets used for logd.
// TODO(crbug/513081): Implement UNIX domain socket with names and remove this.

#include <map>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "posix_translation/local_socket.h"

namespace posix_translation {

class LogdSocketNamespace {
 public:
  explicit LogdSocketNamespace(base::Lock* mutex) : mutex_(mutex) {}

  int Bind(const std::string& name, LocalSocket* stream);

  scoped_refptr<LocalSocket> GetByName(const std::string& name);

 private:
  typedef std::map<std::string, scoped_refptr<LocalSocket> > Map;
  Map map_;
  base::Lock* mutex_;
  DISALLOW_COPY_AND_ASSIGN(LogdSocketNamespace);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_LOGD_SOCKET_NAMESPACE_H_
