// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ppapi_mocks/ppapi_mock_impl.h"

#include <utility>

#include "base/memory/singleton.h"

namespace ppapi_mock {

InterfaceRegistry::InterfaceRegistry() {
}
InterfaceRegistry::~InterfaceRegistry() {
}

InterfaceRegistry* InterfaceRegistry::GetInstance() {
  return Singleton<InterfaceRegistry,
                   LeakySingletonTraits<InterfaceRegistry> >::get();
}

void InterfaceRegistry::Register(const char* interface_name,
                                 const void* ppapi_interface) {
  const bool is_inserted = interface_map_.insert(
      std::make_pair(interface_name, ppapi_interface)).second;
  LOG_ALWAYS_FATAL_IF(!is_inserted,
                      "\"%s\" is registered twice", interface_name);
}

const void* InterfaceRegistry::GetInterface(const char* interface_name) const {
  InterfaceMap::const_iterator iter = interface_map_.find(interface_name);
  LOG_ALWAYS_FATAL_IF(iter == interface_map_.end(),
                      "Requesting unmocked interface: %s\n", interface_name);
  return iter->second;
}

MockRegistry::MockRegistry() {
}
MockRegistry::~MockRegistry() {
}

MockRegistry* MockRegistry::GetInstance() {
  return Singleton<MockRegistry,
                   LeakySingletonTraits<MockRegistry> >::get();
}

void MockRegistry::CreateAllMocks() {
  for (size_t i = 0; i < mock_creators_.size(); ++i) {
    mock_creators_[i]();
  }
}

void MockRegistry::DeleteAllMocks() {
  for (size_t i = 0; i < mock_deleters_.size(); ++i) {
    mock_deleters_[i]();
  }
}

const void* GetInterface(const char* interface_name) {
  return InterfaceRegistry::GetInstance()->GetInterface(interface_name);
}

}  // namespace ppapi_mock
