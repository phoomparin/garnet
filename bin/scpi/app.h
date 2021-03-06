// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_SCPI_APP_H_
#define GARNET_BIN_SCPI_APP_H_

#include <fcntl.h>
#include <fuchsia/scpi/cpp/fidl.h>
#include <lib/fxl/files/unique_fd.h>
#include "lib/component/cpp/startup_context.h"

namespace scpi {

class App : public fuchsia::scpi::SystemController {
 public:
  App();
  ~App() override;
  explicit App(std::unique_ptr<component::StartupContext> context);
  void GetDvfsInfo(uint32_t power_domain, GetDvfsInfoCallback callback) final;
  void GetSystemStatus(GetSystemStatusCallback callback) final;
  zx_status_t Start();

 private:
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  std::unique_ptr<component::StartupContext> context_;
  fidl::BindingSet<fuchsia::scpi::SystemController> bindings_;
  fxl::UniqueFD fd_;
};

}  // namespace scpi

#endif  // GARNET_BIN_SCPI_APP_H_
