/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ServoRestyleManager.h"

#include "mozilla/DocumentStyleRootIterator.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoRestyleManagerInlines.h"
#include "mozilla/ServoStyleSet.h"
#include "mozilla/dom/ChildIterator.h"
#include "nsContentUtils.h"
#include "nsPrintfCString.h"
#include "nsStyleChangeList.h"

using namespace mozilla::dom;

namespace mozilla {

ServoRestyleManager::ServoRestyleManager(nsPresContext* aPresContext)
  : RestyleManagerBase(aPresContext)
  , mReentrantChanges(nullptr)
{
}

void
ServoRestyleManager::PostRestyleEvent(Element* aElement,
                                      nsRestyleHint aRestyleHint,
                                      nsChangeHint aMinChangeHint)
{
}

void
ServoRestyleManager::PostRestyleEventForLazyConstruction()
{
}

void
ServoRestyleManager::RebuildAllStyleData(nsChangeHint aExtraHint,
                                         nsRestyleHint aRestyleHint)
{
}

void
ServoRestyleManager::PostRebuildAllStyleDataEvent(nsChangeHint aExtraHint,
                                                  nsRestyleHint aRestyleHint)
{
}

/* static */ void
ServoRestyleManager::ClearServoDataFromSubtree(Element* aElement)
{
}

/* static */ void
ServoRestyleManager::ClearDirtyDescendantsFromSubtree(Element* aElement)
{
}

void
ServoRestyleManager::RecreateStyleContexts(Element* aElement,
                                           nsStyleContext* aParentContext,
                                           ServoStyleSet* aStyleSet,
                                           nsStyleChangeList& aChangeListToProcess)
{
}

void
ServoRestyleManager::RecreateStyleContextsForText(nsIContent* aTextNode,
                                                  nsStyleContext* aParentContext,
                                                  ServoStyleSet* aStyleSet)
{
}

/* static */ nsIFrame*
ServoRestyleManager::FrameForPseudoElement(const nsIContent* aContent,
                                           nsIAtom* aPseudoTagOrNull)
{
  return nullptr;
}

void
ServoRestyleManager::ProcessPendingRestyles()
{
}

void
ServoRestyleManager::RestyleForInsertOrChange(nsINode* aContainer,
                                              nsIContent* aChild)
{
}

void
ServoRestyleManager::ContentInserted(nsINode* aContainer, nsIContent* aChild)
{
}

void
ServoRestyleManager::RestyleForAppend(nsIContent* aContainer,
                                      nsIContent* aFirstNewContent)
{
}

void
ServoRestyleManager::ContentAppended(nsIContent* aContainer,
                                     nsIContent* aFirstNewContent)
{
}

void
ServoRestyleManager::ContentRemoved(nsINode* aContainer,
                                    nsIContent* aOldChild,
                                    nsIContent* aFollowingSibling)
{
}

void
ServoRestyleManager::ContentStateChanged(nsIContent* aContent,
                                         EventStates aChangedBits)
{
}

void
ServoRestyleManager::AttributeWillChange(Element* aElement,
                                         int32_t aNameSpaceID,
                                         nsIAtom* aAttribute, int32_t aModType,
                                         const nsAttrValue* aNewValue)
{
}

void
ServoRestyleManager::AttributeChanged(Element* aElement, int32_t aNameSpaceID,
                                      nsIAtom* aAttribute, int32_t aModType,
                                      const nsAttrValue* aOldValue)
{
}

nsresult
ServoRestyleManager::ReparentStyleContext(nsIFrame* aFrame)
{
  return NS_OK;
}

} // namespace mozilla
