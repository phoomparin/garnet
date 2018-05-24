// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <trace-provider/provider.h>

#include "lib/app/cpp/application_context.h"
#include "lib/fsl/syslogger/init.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/log_settings_command_line.h"
#include "lib/fxl/logging.h"

#include "garnet/bin/ui/scenic/app.h"

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;
  if (fsl::InitLoggerFromCommandLine(command_line, {"scenic"}) != ZX_OK)
    return 1;

  fsl::MessageLoop loop;
  trace::TraceProvider trace_provider(loop.async());
  std::unique_ptr<component::ApplicationContext> app_context(
      component::ApplicationContext::CreateFromStartupInfo());

  scenic::App app(app_context.get());

  loop.Run();

  return 0;
}
