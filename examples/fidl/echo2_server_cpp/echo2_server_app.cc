// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "echo2_server_app.h"

#include "lib/app/cpp/startup_context.h"

namespace echo2 {

EchoServerApp::EchoServerApp()
    : context_(fuchsia::sys::StartupContext::CreateFromStartupInfo()) {
  context_->outgoing().AddPublicService<Echo>(
      [this](fidl::InterfaceRequest<Echo> request) {
    bindings_.AddBinding(this, std::move(request));
  });
}

void EchoServerApp::EchoString(fidl::StringPtr value, EchoStringCallback callback) {
  printf("EchoString: %s\n", value->data());
  callback(std::move(value));
}

}  // namespace echo2