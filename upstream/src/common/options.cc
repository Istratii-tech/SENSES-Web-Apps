// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/options.h"

#include <string.h>

#include "base/memory/singleton.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "common/alog.h"

namespace arc {

Options::Options() {
}

Options::~Options() {
}

// static
Options* Options::GetInstance() {
  return Singleton<Options, LeakySingletonTraits<Options> >::get();
}

void Options::Put(const std::string& name, const std::string& value) {
  options_map_[name] = value;
}

std::string Options::GetString(const std::string& name) const {
  std::map<std::string, std::string>::const_iterator iter =
      options_map_.find(name);
  LOG_ALWAYS_FATAL_IF(iter == options_map_.end(),
                      "Option has not been set: %s", name.c_str());
  return iter->second;
}


std::string Options::GetString(const std::string& name,
                               const std::string& default_value) const {
  std::map<std::string, std::string>::const_iterator iter =
      options_map_.find(name);
  if (iter == options_map_.end()) {
    return default_value;
  }
  return iter->second;
}

std::vector<std::string> Options::GetStringVector(
      const std::string& name) const {
  std::vector<std::string> strings;
  base::SplitString(GetString(name), '\1', &strings);
  return strings;
}

bool Options::GetBool(const std::string& name) const {
  // Bool values are only converted to strings in JavaScript so
  // we only need to check for one type of bool string which is
  // JavaScript's default serialization of a bool.
  return GetString(name) == "true";
}

bool Options::GetBool(const std::string& name, bool default_value) const {
  std::map<std::string, std::string>::const_iterator iter =
      options_map_.find(name);
  if (iter == options_map_.end())
    return default_value;
  return iter->second == "true";
}

double Options::GetDouble(const std::string& name) const {
  double result;
  LOG_ALWAYS_FATAL_IF(!base::StringToDouble(GetString(name), &result));
  return result;
}

int Options::GetInt(const std::string& name) const {
  int result;
  LOG_ALWAYS_FATAL_IF(!base::StringToInt(GetString(name), &result));
  return result;
}

}  // namespace arc
