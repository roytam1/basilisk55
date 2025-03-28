/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_LAYERS_COMPOSITABLETRANSACTIONPARENT_H
#define MOZILLA_LAYERS_COMPOSITABLETRANSACTIONPARENT_H

#include <vector>                       // for vector
#include "mozilla/Attributes.h"         // for override
#include "mozilla/layers/ISurfaceAllocator.h"  // for ISurfaceAllocator
#include "mozilla/layers/LayersMessages.h"  // for EditReply, etc
#include "CompositableHost.h"

namespace mozilla {
namespace layers {

typedef std::vector<mozilla::layers::EditReply> EditReplyVector;

// Since PCompositble has two potential manager protocols, we can't just call
// the Manager() method usually generated when there's one manager protocol,
// so both manager protocols implement this and we keep a reference to them
// through this interface.
class CompositableParentManager : public HostIPCAllocator
{
public:
  CompositableParentManager() {}

  void DestroyActor(const OpDestroy& aOp);

  void UpdateFwdTransactionId(uint64_t aTransactionId)
  {
    MOZ_ASSERT(mFwdTransactionId < aTransactionId);
    mFwdTransactionId = aTransactionId;
  }

  uint64_t GetFwdTransactionId() { return mFwdTransactionId; }

  RefPtr<CompositableHost> AddCompositable(
    const CompositableHandle& aHandle,
    const TextureInfo& aInfo);
  RefPtr<CompositableHost> FindCompositable(const CompositableHandle& aHandle);

protected:
  /**
   * Handle the IPDL messages that affect PCompositable actors.
   */
  bool ReceiveCompositableUpdate(const CompositableOperation& aEdit,
                                 EditReplyVector& replyv);

  void ReleaseCompositable(const CompositableHandle& aHandle);

  uint64_t mFwdTransactionId = 0;

  /**
   * Mapping form IDs to CompositableHosts.
   */
  std::map<uint64_t, RefPtr<CompositableHost>> mCompositables;
};

} // namespace layers
} // namespace mozilla

#endif
