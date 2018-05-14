// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/svc/dir.h"

#include <string>

#include <fs/synchronous-vfs.h>
#include <fs/pseudo-dir.h>
#include <fs/service.h>

struct svc_dir {
  explicit svc_dir(async_t* async) : vfs(async) {}

  fs::SynchronousVfs vfs;
  fbl::RefPtr<fs::PseudoDir> root;
};

zx_status_t svc_dir_create(async_t* async,
                           zx_handle_t dir_request,
                           svc_dir_t** result) {
  svc_dir_t* dir = new svc_dir_t(async);
  dir->root = fbl::AdoptRef(new fs::PseudoDir());
  zx_status_t status = dir->vfs.ServeDirectory(
      dir->root, zx::channel(dir_request));
  if (status != ZX_OK) {
    delete dir;
    return status;
  }
  *result = dir;
  return ZX_OK;
}

zx_status_t svc_dir_add_service(svc_dir_t* dir,
                                const char* type,
                                const char* service_name,
                                void* context,
                                svc_connector_t handler) {
  fbl::RefPtr<fs::Vnode> child;
  zx_status_t status = dir->root->Lookup(&child, type);
  if (status == ZX_ERR_NOT_FOUND) {
      child = fbl::AdoptRef(new fs::PseudoDir());
      status = dir->root->AddEntry(type, child);
  }
  if (status != ZX_OK)
    return status;
  fs::PseudoDir* child_dir = static_cast<fs::PseudoDir*>(child.get());
  return child_dir->AddEntry(
    service_name,
    fbl::AdoptRef(new fs::Service([service_name = std::string(service_name),
                                   context,
                                   handler](zx::channel channel) {
      handler(context, service_name.c_str(), channel.release());
      return ZX_OK;
    })));
}

zx_status_t svc_dir_remove_service(svc_dir_t* dir,
                                   const char* type,
                                   const char* service_name) {
  fbl::RefPtr<fs::Vnode> child;
  zx_status_t status = dir->root->Lookup(&child, type);
  if (status != ZX_OK)
    return status;

  fs::PseudoDir* child_dir = static_cast<fs::PseudoDir*>(child.get());
  return child_dir->RemoveEntry(service_name);
}

zx_status_t svc_dir_destroy(svc_dir_t* dir) {
  delete dir;
  return ZX_OK;
}
