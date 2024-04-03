/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* representation of CSSRuleList for stylo */

#include "mozilla/ServoCSSRuleList.h"

#include "mozilla/ServoBindings.h"
#include "mozilla/ServoStyleRule.h"

namespace mozilla {

ServoCSSRuleList::ServoCSSRuleList(ServoStyleSheet* aStyleSheet,
                                   already_AddRefed<ServoCssRules> aRawRules)
  : mStyleSheet(aStyleSheet)
  , mRawRules(aRawRules)
{
}

// QueryInterface implementation for ServoCSSRuleList
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(ServoCSSRuleList)
NS_INTERFACE_MAP_END_INHERITING(dom::CSSRuleList)

NS_IMPL_ADDREF_INHERITED(ServoCSSRuleList, dom::CSSRuleList)
NS_IMPL_RELEASE_INHERITED(ServoCSSRuleList, dom::CSSRuleList)

NS_IMPL_CYCLE_COLLECTION_CLASS(ServoCSSRuleList)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(ServoCSSRuleList)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END_INHERITED(dom::CSSRuleList)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ServoCSSRuleList,
                                                  dom::CSSRuleList)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

css::Rule*
ServoCSSRuleList::GetRule(uint32_t aIndex)
{
  return nullptr;
}

css::Rule*
ServoCSSRuleList::IndexedGetter(uint32_t aIndex, bool& aFound)
{
  return nullptr;
}

template<typename Func>
void
ServoCSSRuleList::EnumerateInstantiatedRules(Func aCallback)
{
}

void
ServoCSSRuleList::DropReference()
{
}

nsresult
ServoCSSRuleList::InsertRule(const nsAString& aRule, uint32_t aIndex)
{
  return NS_OK;
}

nsresult
ServoCSSRuleList::DeleteRule(uint32_t aIndex)
{
  return NS_OK;
}

ServoCSSRuleList::~ServoCSSRuleList()
{
}

} // namespace mozilla
