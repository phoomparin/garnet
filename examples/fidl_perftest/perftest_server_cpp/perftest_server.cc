// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "perftest_server_app.h"
#include "lib/fidl/cpp/binding.h"

#include <lib/async-loop/cpp/loop.h>

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigMakeDefault);
  perftest::PerfTestServer server(fidl::StringPtr("zircon_benchmarks"));
  loop.Run();
  return 0;
}
