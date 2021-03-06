// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_ENGINE_OBJECT_LINKER_H_
#define GARNET_LIB_UI_GFX_ENGINE_OBJECT_LINKER_H_

#include <lib/async/cpp/wait.h>
#include <lib/fit/function.h>
#include <lib/zx/eventpair.h>
#include <zircon/types.h>
#include <memory>
#include <unordered_map>

#include "garnet/lib/ui/scenic/util/error_reporter.h"
#include "garnet/public/lib/fsl/handles/object_info.h"
#include "garnet/public/lib/fxl/macros.h"

namespace scenic {
namespace gfx {

// Allows linking of peer objects in different sessions via event pairs.  Two
// objects are considered peers if they each hold either end of an event pair.
//
// Accepts one endpoint of an eventpair along with an object to export.  The
// object contains callbacks for both successful and unsuccessful resolution of
// the link.  When the other endpoint of the eventpair is registered with the
// ObjectLinker, the provided resolution callback will be fired.  If the other
// endpoint is destroyed before a link is made, this will also cause a
// resolution callback to be fired.  The callback will be fired regardless of
// which peer is registered first.
//
// If the link was successful, the two peer objects can communicate with each
// other over the connection established by the ObjectLinker.
//
// TODO(SCN-769): Allow multiple Imports.
// Attempts to register either eventpair peer multiple times will result in an
// error.
//
// This class is thread-hostile.  It should only be used from the message
// dispatch thread.
class ObjectLinkerBase {
 public:
  ObjectLinkerBase() = default;
  ObjectLinkerBase(ObjectLinkerBase&&) = default;
  ObjectLinkerBase& operator=(ObjectLinkerBase&&) = default;

  // Opaque object handle generated by and consumed by ObjectLinker calls.
  using ObjectId = uint64_t;

  size_t ExportCount() { return export_count_; }
  size_t UnresolvedExportCount() { return unresolved_export_count_; }
  size_t ImportCount() { return import_count_; }
  size_t UnresolvedImportCount() { return unresolved_import_count_; }

 protected:
  // Information for an object registered with the linker.
  struct Entry {
    void* const object{nullptr};  // Opaque pointer to client object
    zx_koid_t peer_koid{ZX_KOID_INVALID};
    fit::function<void(ObjectLinkerBase*, Entry*)> on_link_resolved;
    fit::function<void()> on_peer_destroyed;
    fit::function<void()> on_connection_closed;
  };

  // Temporary information for an object registered with the linker that is used
  // to establish links with other objects.  These entries are destroyed once a
  // successful link is established.
  struct PendingEntry {
    zx::eventpair token;
    std::unique_ptr<async::Wait> peer_waiter;  // Waiter for early peer death
  };

  using KoidToEntryMap = std::unordered_map<zx_koid_t, Entry>;
  using KoidToPendingEntryMap = std::unordered_map<zx_koid_t, PendingEntry>;
  using KoidPair = std::pair<zx_koid_t, zx_koid_t>;

  // Registers the complete |new_pending_entry| + |new_entry| for linking and
  // reports any errors in registration using |error_reporter|.
  // |import| is used to signal whether the operation is taking place on an
  // exported or imported Entry.
  //
  // The return value is a unique 64 bit ID that can be used to identify the
  // registered entry, or 0 if registration failed.
  uint64_t RegisterEntry(PendingEntry new_pending_entry, Entry new_entry,
                         ErrorReporter* error_reporter, bool import);

  // Unregisters the object pointed to by |entry_handle| and removes all traces
  // of it from the linker.  If the object is linked to a peer, the peer will
  // be notified of the object's destruction.
  // |import| is used to signal whether the operation is taking place on an
  // exported or imported Entry.
  void UnregisterEntry(uint64_t entry_handle, bool import);

  // Sets up an async::Wait on |entry| that will fire a callback if the entry's
  // peer token is destroyed before a link has been established.
  // |import| is used to signal whether the operation is taking place on an
  // exported or imported Entry.
  void WaitForPeerDeath(KoidToPendingEntryMap::iterator& pending_entry,
                        bool import);

  // Attempts linking of the objects associated with the koids in |peer_koids|.
  // The link will only succeed if both koids have objects associated with them.
  // Returns whether or not the link was made successfully.
  bool AttemptLinking(KoidPair peer_koids,
                      KoidToPendingEntryMap::iterator& pending_entry_iter,
                      KoidToEntryMap::iterator& entry_iter);

  KoidToEntryMap entries_;
  KoidToPendingEntryMap pending_entries_;
  size_t unresolved_export_count_{0};
  size_t unresolved_import_count_{0};
  size_t export_count_{0};
  size_t import_count_{0};
};

// Helper class which adds type information to ObjectLinkerBase.  |I| represents
// the type of imported object while |E| represents the type of exported object.
template <typename I, typename E>
class ObjectLinker : public ObjectLinkerBase {
 public:
  ObjectLinker() = default;
  ObjectLinker(ObjectLinker&&) = default;
  ObjectLinker& operator=(ObjectLinker&&) = default;

  // Register an |object| so that it can be exported to a paired object in
  // another Session.  The ObjectLinker takes ownership over the provided
  // |token|, which is used to locate the paired object.
  //
  // It is possible to export the same |object| multiple times with different
  // |token|s, allowing for a one-to-many relationship.
  //
  // Once the objects are linked, they can communicate directly within Scenic.
  // This is true as soon as RegisterImport() is called on |object|'s' partner
  /// and both of the calls return.
  //
  // The return value is a unique identifier that can be used to identify this
  // object to the ObjectLinker later.
  ObjectId RegisterExport(E* object, zx::eventpair token,
                          ErrorReporter* error_reporter);

  // Register an |object| so that it can be imported from a paired object in
  // another Session.  The ObjectLinker takes ownership over the provided
  // |token|, which is used to locate the paired object.
  //
  // Once the objects are linked, they can communicate directly within Scenic.
  // This is true as soon as RegisterExport() is called on |object|'s' partner
  /// and both of the calls return.
  //
  // The return value is a unique identifier that can be used to identify this
  // object to the ObjectLinker later.
  ObjectId RegisterImport(I* object, zx::eventpair token,
                          ErrorReporter* error_reporter);

  // Unregisters an exported object using the |export_handle| returned from
  // RegisterExport, and unlinks it from its paired object if they are linked.
  void UnregisterExport(uint64_t export_handle);

  // Unregisters an imported object using the |import_handle| returned from
  // RegisterImport, and unlinks it from its paired object if they are linked.
  void UnregisterImport(uint64_t import_handle);
};

// Template functions must be defined in the header.
template <typename I, typename E>
ObjectLinkerBase::ObjectId ObjectLinker<I, E>::RegisterExport(
    E* object, zx::eventpair token, ErrorReporter* error_reporter) {
  PendingEntry pending_export_entry{
      .token = std::move(token),
  };
  Entry export_entry{
      .object = object,
      .on_link_resolved =
          [object](ObjectLinkerBase* linker, Entry* import_entry) {
            ObjectLinker* typed_linker = static_cast<ObjectLinker*>(linker);
            I* import_obj = static_cast<I*>(import_entry->object);
            object->LinkResolved(typed_linker, import_obj);
          },
      .on_peer_destroyed = [object]() { object->PeerDestroyed(); },
      .on_connection_closed = [object]() { object->ConnectionClosed(); },
  };
  return RegisterEntry(std::move(pending_export_entry), std::move(export_entry),
                       error_reporter, false);
}

// Template functions must be defined in the header.
template <typename I, typename E>
ObjectLinkerBase::ObjectId ObjectLinker<I, E>::RegisterImport(
    I* object, zx::eventpair token, ErrorReporter* error_reporter) {
  PendingEntry pending_import_entry{
      .token = std::move(token),
  };
  Entry import_entry{
      .object = object,
      .on_link_resolved =
          [object](ObjectLinkerBase* linker, Entry* export_entry) {
            E* export_obj = static_cast<E*>(export_entry->object);
            object->LinkResolved(export_obj);
          },
      .on_peer_destroyed = [object]() { object->PeerDestroyed(); },
      .on_connection_closed = [object]() { object->ConnectionClosed(); },
  };
  return RegisterEntry(std::move(pending_import_entry), std::move(import_entry),
                       error_reporter, true);
}

// Template functions must be defined in the header.
template <typename I, typename E>
void ObjectLinker<I, E>::UnregisterExport(ObjectId export_handle) {
  UnregisterEntry(export_handle, false);
}

// Template functions must be defined in the header.
template <typename I, typename E>
void ObjectLinker<I, E>::UnregisterImport(ObjectId import_handle) {
  UnregisterEntry(import_handle, true);
}

}  // namespace gfx
}  // namespace scenic

#endif  // GARNET_LIB_UI_GFX_ENGINE_OBJECT_LINKER_H_
