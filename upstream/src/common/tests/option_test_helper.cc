// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/tests/option_test_helper.h"

#include "common/options.h"

namespace arc {

void PopulateOptionsForTest() {
  Options *options = Options::GetInstance();
  options->Put("app_launch_time", "0");
  options->Put("embed_time", "0");
  options->Put("enable_arc_strace", "false");
  options->Put("enable_external_directory", "false");
  options->Put("enable_synthesize_touch_events_on_click", "false");
  options->Put("enable_synthesize_touch_events_on_wheel", "false");
  options->Put("package_name", "a.package.name");
  options->Put("pause_on_blur", "false");
  options->Put("resize", "disabled");
  options->Put("save_logs_to_file", "false");
  // instance_->DidChangeFocus() will post work to the utility thread if track
  // focus is enabled. It will cause memory leak, because the utility thread
  // is not started for this test.
  options->Put("sleep_on_blur", "false");
  options->Put("stderr_log", "W");
}

}  // namespace arc
