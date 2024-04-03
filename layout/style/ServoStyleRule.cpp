/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* representation of CSSStyleRule for stylo */

#include "mozilla/ServoStyleRule.h"

#include "mozilla/DeclarationBlockInlines.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoDeclarationBlock.h"
#include "mozilla/dom/CSSStyleRuleBinding.h"

#include "mozAutoDocUpdate.h"

namespace mozilla {

// -- ServoStyleRuleDeclaration ---------------------------------------

ServoStyleRuleDeclaration::ServoStyleRuleDeclaration(
  already_AddRefed<RawServoDeclarationBlock> aDecls)
  : mDecls(new ServoDeclarationBlock(Move(aDecls)))
{
}

ServoStyleRuleDeclaration::~ServoStyleRuleDeclaration()
{
}

// QueryInterface implementation for ServoStyleRuleDeclaration
NS_INTERFACE_MAP_BEGIN(ServoStyleRuleDeclaration)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  // We forward the cycle collection interfaces to Rule(), which is
  // never null (in fact, we're part of that object!)
  if (aIID.Equals(NS_GET_IID(nsCycleCollectionISupports)) ||
      aIID.Equals(NS_GET_IID(nsXPCOMCycleCollectionParticipant))) {
    return Rule()->QueryInterface(aIID, aInstancePtr);
  }
  else
NS_IMPL_QUERY_TAIL_INHERITING(nsDOMCSSDeclaration)

NS_IMPL_ADDREF_USING_AGGREGATOR(ServoStyleRuleDeclaration, Rule())
NS_IMPL_RELEASE_USING_AGGREGATOR(ServoStyleRuleDeclaration, Rule())

/* nsDOMCSSDeclaration implementation */

NS_IMETHODIMP
ServoStyleRuleDeclaration::GetParentRule(nsIDOMCSSRule** aParent)
{
  return NS_OK;
}

nsINode*
ServoStyleRuleDeclaration::GetParentObject()
{
  return nullptr;
}

DocGroup*
ServoStyleRuleDeclaration::GetDocGroup() const
{
  return nullptr;
}

DeclarationBlock*
ServoStyleRuleDeclaration::GetCSSDeclaration(Operation aOperation)
{
  return nullptr;
}

nsresult
ServoStyleRuleDeclaration::SetCSSDeclaration(DeclarationBlock* aDecl)
{
  return NS_OK;
}

nsIDocument*
ServoStyleRuleDeclaration::DocToUpdate()
{
  return nullptr;
}

void
ServoStyleRuleDeclaration::GetCSSParsingEnvironment(
  CSSParsingEnvironment& aCSSParseEnv)
{
}

// -- ServoStyleRule --------------------------------------------------

ServoStyleRule::ServoStyleRule(already_AddRefed<RawServoStyleRule> aRawRule)
  : BindingStyleRule(0, 0)
  , mRawRule(aRawRule)
  , mDecls(Servo_StyleRule_GetStyle(mRawRule).Consume())
{
}

// QueryInterface implementation for ServoStyleRule
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(ServoStyleRule)
  NS_INTERFACE_MAP_ENTRY(nsIDOMCSSStyleRule)
NS_INTERFACE_MAP_END_INHERITING(css::Rule)

NS_IMPL_ADDREF_INHERITED(ServoStyleRule, css::Rule)
NS_IMPL_RELEASE_INHERITED(ServoStyleRule, css::Rule)

NS_IMPL_CYCLE_COLLECTION_CLASS(ServoStyleRule)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(ServoStyleRule, css::Rule)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(ServoStyleRule, css::Rule)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ServoStyleRule, css::Rule)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

bool
ServoStyleRule::IsCCLeaf() const
{
  return false;
}

already_AddRefed<css::Rule>
ServoStyleRule::Clone() const
{
  return nullptr;
}

size_t
ServoStyleRule::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const
{
  return 0;
}

#ifdef DEBUG
void
ServoStyleRule::List(FILE* out, int32_t aIndent) const
{
}
#endif

/* CSSRule implementation */

uint16_t
ServoStyleRule::Type() const
{
  return 0;
}

void
ServoStyleRule::GetCssTextImpl(nsAString& aCssText) const
{
}

nsICSSDeclaration*
ServoStyleRule::Style()
{
  return nullptr;
}

/* CSSStyleRule implementation */

NS_IMETHODIMP
ServoStyleRule::GetSelectorText(nsAString& aSelectorText)
{
  return NS_OK;
}

NS_IMETHODIMP
ServoStyleRule::SetSelectorText(const nsAString& aSelectorText)
{
  return NS_OK;
}

NS_IMETHODIMP
ServoStyleRule::GetStyle(nsIDOMCSSStyleDeclaration** aStyle)
{
  return NS_OK;
}

} // namespace mozilla
