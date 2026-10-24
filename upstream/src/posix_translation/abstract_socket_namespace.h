// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_ABSTRACT_SOCKET_NAMESPACE_H_
#define POSIX_TRANSLATION_ABSTRACT_SOCKET_NAMESPACE_H_

#include <string>
#include <vector>

#include "base/containers/hash_tables.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/basictypes.h"
#include "common/update_tracking.h"
#include "posix_translation/local_socket.h"

namespace posix_translation {

class AbstractSocketNamespace {
 public:
  typedef std::vector<scoped_refptr<LocalSocket> > Streams;
  explicit AbstractSocketNamespace(base::Lock* mutex) : mutex_(mutex) {}

  // Bind the given UNIX address family socket with the given abstract name.
  int Bind(const std::string& name, LocalSocket* stream);

  // Get the stream associated with the given abstract name.
  scoped_refptr<LocalSocket> GetByName(const std::string& name);

  void GetAllStreams(Streams* out_streams) const;

  arc::UpdateProducer* GetUpdateProducer() { return &update_producer_; }

 private:
  typedef base::hash_map<std::string, scoped_refptr<LocalSocket> > Map;  // NOLINT
  Map map_;
  base::Lock* mutex_;
  arc::UpdateProducer update_producer_;
  DISALLOW_COPY_AND_ASSIGN(AbstractSocketNamespace);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_ABSTRACT_SOCKET_NAMESPACE_H_
