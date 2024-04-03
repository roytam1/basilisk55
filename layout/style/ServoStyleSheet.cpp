/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ServoStyleSheet.h"

#include "mozilla/css/Rule.h"
#include "mozilla/StyleBackendType.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoCSSRuleList.h"
#include "mozilla/dom/CSSRuleList.h"

#include "mozAutoDocUpdate.h"

using namespace mozilla::dom;

namespace mozilla {

ServoStyleSheet::ServoStyleSheet(css::SheetParsingMode aParsingMode,
                                 CORSMode aCORSMode,
                                 net::ReferrerPolicy aReferrerPolicy,
                                 const dom::SRIMetadata& aIntegrity)
  : StyleSheet(StyleBackendType::Servo, aParsingMode)
  , mSheetInfo(aCORSMode, aReferrerPolicy, aIntegrity)
{
}

ServoStyleSheet::~ServoStyleSheet()
{
}

// QueryInterface implementation for ServoStyleSheet
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(ServoStyleSheet)
NS_INTERFACE_MAP_END_INHERITING(StyleSheet)

NS_IMPL_ADDREF_INHERITED(ServoStyleSheet, StyleSheet)
NS_IMPL_RELEASE_INHERITED(ServoStyleSheet, StyleSheet)

NS_IMPL_CYCLE_COLLECTION_CLASS(ServoStyleSheet)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(ServoStyleSheet)
  tmp->DropRuleList();
NS_IMPL_CYCLE_COLLECTION_UNLINK_END_INHERITED(StyleSheet)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ServoStyleSheet, StyleSheet)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mRuleList)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

bool
ServoStyleSheet::HasRules() const
{
  return false;
}

void
ServoStyleSheet::SetAssociatedDocument(nsIDocument* aDocument,
                                       DocumentAssociationMode aAssociationMode)
{
}

ServoStyleSheet*
ServoStyleSheet::GetParentSheet() const
{
  return nullptr;
}

void
ServoStyleSheet::AppendStyleSheet(ServoStyleSheet* aSheet)
{
}

nsresult
ServoStyleSheet::ParseSheet(css::Loader* aLoader,
                            const nsAString& aInput,
                            nsIURI* aSheetURI,
                            nsIURI* aBaseURI,
                            nsIPrincipal* aSheetPrincipal,
                            uint32_t aLineNumber)
{
  return NS_OK;
}

void
ServoStyleSheet::LoadFailed()
{
}

void
ServoStyleSheet::DropSheet()
{
}

void
ServoStyleSheet::DropRuleList()
{
}

size_t
ServoStyleSheet::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const
{
  return 0;
}

#ifdef DEBUG
void
ServoStyleSheet::List(FILE* aOut, int32_t aIndex) const
{
}
#endif

css::Rule*
ServoStyleSheet::GetDOMOwnerRule() const
{
  return nullptr;
}

CSSRuleList*
ServoStyleSheet::GetCssRulesInternal(ErrorResult& aRv)
{
  return nullptr;
}

uint32_t
ServoStyleSheet::InsertRuleInternal(const nsAString& aRule,
                                    uint32_t aIndex, ErrorResult& aRv)
{
  return 0;
}

void
ServoStyleSheet::DeleteRuleInternal(uint32_t aIndex, ErrorResult& aRv)
{
}

} // namespace mozilla
