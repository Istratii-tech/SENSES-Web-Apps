// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/abstract_socket_namespace.h"

#include <errno.h>

#include <utility>

namespace posix_translation {

// The lifetime of the stream must implicitly be managed elsewhere.  We assume
// that this function will be called with the same name and NULL in the
// stream's LocalSocket::OnLastFileRef.
int AbstractSocketNamespace::Bind(const std::string& name,
                                  LocalSocket* stream) {
  mutex_->AssertAcquired();
  update_producer_.ProduceUpdate();
  Map::iterator i = map_.find(name);
  if (stream == NULL) {
    if (i != map_.end())
      map_.erase(i);
    return 0;
  }

  if (map_.insert(make_pair(name, stream)).second)
    return 0;

  errno = EADDRINUSE;
  return -1;
}

scoped_refptr<LocalSocket> AbstractSocketNamespace::GetByName(
      const std::string& name) {
  mutex_->AssertAcquired();
  Map::iterator i = map_.find(name);
  if (i == map_.end()) return NULL;
  return i->second;
}

void AbstractSocketNamespace::GetAllStreams(Streams* streams) const {
  mutex_->AssertAcquired();
  streams->clear();
  streams->reserve(map_.size());
  for (Map::const_iterator i = map_.begin(); i != map_.end(); ++i) {
    streams->push_back(i->second);
  }
}

}  // namespace posix_translation
