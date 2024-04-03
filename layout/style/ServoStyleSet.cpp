/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ServoStyleSet.h"

#include "mozilla/DocumentStyleRootIterator.h"
#include "mozilla/ServoRestyleManager.h"
#include "mozilla/dom/ChildIterator.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/ElementInlines.h"
#include "nsCSSAnonBoxes.h"
#include "nsCSSPseudoElements.h"
#include "nsIDocumentInlines.h"
#include "nsPrintfCString.h"
#include "nsStyleContext.h"
#include "nsStyleSet.h"

using namespace mozilla;
using namespace mozilla::dom;

ServoStyleSet::ServoStyleSet()
  : mPresContext(nullptr)
  , mBatching(0)
{
}

void
ServoStyleSet::Init(nsPresContext* aPresContext)
{
}

void
ServoStyleSet::BeginShutdown()
{
}

void
ServoStyleSet::Shutdown()
{
}

bool
ServoStyleSet::GetAuthorStyleDisabled() const
{
  return false;
}

nsresult
ServoStyleSet::SetAuthorStyleDisabled(bool aStyleDisabled)
{
  return NS_OK;
}

void
ServoStyleSet::BeginUpdate()
{
}

nsresult
ServoStyleSet::EndUpdate()
{
  return NS_OK;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ResolveStyleFor(Element* aElement,
                               nsStyleContext* aParentContext,
                               LazyComputeBehavior aMayCompute)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::GetContext(nsIContent* aContent,
                          nsStyleContext* aParentContext,
                          nsIAtom* aPseudoTag,
                          CSSPseudoElementType aPseudoType,
                          LazyComputeBehavior aMayCompute)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::GetContext(already_AddRefed<ServoComputedValues> aComputedValues,
                          nsStyleContext* aParentContext,
                          nsIAtom* aPseudoTag,
                          CSSPseudoElementType aPseudoType)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ResolveStyleFor(Element* aElement,
                               nsStyleContext* aParentContext,
                               LazyComputeBehavior aMayCompute,
                               TreeMatchContext& aTreeMatchContext)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ResolveStyleForText(nsIContent* aTextNode,
                                   nsStyleContext* aParentContext)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ResolveStyleForOtherNonElement(nsStyleContext* aParentContext)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ResolvePseudoElementStyle(Element* aOriginatingElement,
                                         CSSPseudoElementType aType,
                                         nsStyleContext* aParentContext,
                                         Element* aPseudoElement)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ResolveTransientStyle(Element* aElement, CSSPseudoElementType aType)
{
  return nullptr;
}

// aFlags is an nsStyleSet flags bitfield
already_AddRefed<nsStyleContext>
ServoStyleSet::ResolveAnonymousBoxStyle(nsIAtom* aPseudoTag,
                                        nsStyleContext* aParentContext,
                                        uint32_t aFlags)
{
  return nullptr;
}

// manage the set of style sheets in the style set
nsresult
ServoStyleSet::AppendStyleSheet(SheetType aType,
                                ServoStyleSheet* aSheet)
{
  return NS_OK;
}

nsresult
ServoStyleSet::PrependStyleSheet(SheetType aType,
                                 ServoStyleSheet* aSheet)
{
  return NS_OK;
}

nsresult
ServoStyleSet::RemoveStyleSheet(SheetType aType,
                                ServoStyleSheet* aSheet)
{
  return NS_OK;
}

nsresult
ServoStyleSet::ReplaceSheets(SheetType aType,
                             const nsTArray<RefPtr<ServoStyleSheet>>& aNewSheets)
{
  return NS_OK;
}

nsresult
ServoStyleSet::InsertStyleSheetBefore(SheetType aType,
                                      ServoStyleSheet* aNewSheet,
                                      ServoStyleSheet* aReferenceSheet)
{
  return NS_OK;
}

int32_t
ServoStyleSet::SheetCount(SheetType aType) const
{
  return 0;
}

ServoStyleSheet*
ServoStyleSet::StyleSheetAt(SheetType aType,
                            int32_t aIndex) const
{
  return nullptr;
}

nsresult
ServoStyleSet::RemoveDocStyleSheet(ServoStyleSheet* aSheet)
{
  return NS_OK;
}

nsresult
ServoStyleSet::AddDocStyleSheet(ServoStyleSheet* aSheet,
                                nsIDocument* aDocument)
{
  return NS_OK;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ProbePseudoElementStyle(Element* aOriginatingElement,
                                       CSSPseudoElementType aType,
                                       nsStyleContext* aParentContext)
{
  return nullptr;
}

already_AddRefed<nsStyleContext>
ServoStyleSet::ProbePseudoElementStyle(Element* aOriginatingElement,
                                       CSSPseudoElementType aType,
                                       nsStyleContext* aParentContext,
                                       TreeMatchContext& aTreeMatchContext,
                                       Element* aPseudoElement)
{
  return nullptr;
}

nsRestyleHint
ServoStyleSet::HasStateDependentStyle(dom::Element* aElement,
                                      EventStates aStateMask)
{
  return nsRestyleHint(0);
}

nsRestyleHint
ServoStyleSet::HasStateDependentStyle(dom::Element* aElement,
                                      CSSPseudoElementType aPseudoType,
                                     dom::Element* aPseudoElement,
                                     EventStates aStateMask)
{
  return nsRestyleHint(0);
}

void
ServoStyleSet::StyleDocument()
{
}

void
ServoStyleSet::StyleNewSubtree(Element* aRoot)
{
}

void
ServoStyleSet::StyleNewChildren(Element* aParent)
{
}

void
ServoStyleSet::NoteStyleSheetsChanged()
{
}

#ifdef DEBUG
void
ServoStyleSet::AssertTreeIsClean()
{
}
#endif

void
ServoStyleSet::RebuildData()
{
}

ServoComputedValuesStrong
ServoStyleSet::RestyleWithAddedDeclaration(RawServoDeclarationBlock* aDeclarations,
                                           const ServoComputedValues* aPreviousStyle)
{
  MOZ_CRASH("stylo: shouldn't be calling RestyleWithAddedDeclaration in a non-stylo build");
}


already_AddRefed<ServoComputedValues>
ServoStyleSet::ResolveServoStyle(Element* aElement)
{
  return nullptr;
}
