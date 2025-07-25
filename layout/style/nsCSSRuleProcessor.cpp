/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * style rule processor for CSS style sheets, responsible for selector
 * matching and cascading
 */

#include "nsAutoPtr.h"
#include "nsCSSRuleProcessor.h"
#include "nsRuleProcessorData.h"
#include <algorithm>
#include "nsIAtom.h"
#include "PLDHashTable.h"
#include "nsICSSPseudoComparator.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/css/StyleRule.h"
#include "mozilla/css/GroupRule.h"
#include "nsIDocument.h"
#include "nsPresContext.h"
#include "nsGkAtoms.h"
#include "nsUnicharUtils.h"
#include "nsError.h"
#include "nsRuleWalker.h"
#include "nsCSSPseudoClasses.h"
#include "nsCSSPseudoElements.h"
#include "nsIContent.h"
#include "nsCOMPtr.h"
#include "nsHashKeys.h"
#include "nsStyleUtil.h"
#include "nsQuickSort.h"
#include "nsAttrValue.h"
#include "nsAttrValueInlines.h"
#include "nsAttrName.h"
#include "nsTArray.h"
#include "nsContentUtils.h"
#include "nsMediaList.h"
#include "nsCSSRules.h"
#include "nsStyleSet.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLSlotElement.h"
#include "mozilla/dom/ShadowRoot.h"
#include "nsNthIndexCache.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/EventStates.h"
#include "mozilla/Preferences.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Likely.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/TypedEnumBits.h"
#include "RuleProcessorCache.h"
#include "nsIDOMMutationEvent.h"
#include "RuleCascadeData.h"
#include "nsCSSRuleUtils.h"

using namespace mozilla;
using namespace mozilla::dom;

// -------------------------------
// CSS Style rule processor implementation
//

nsCSSRuleProcessor::nsCSSRuleProcessor(const sheet_array_type& aSheets,
                                       SheetType aSheetType,
                                       Element* aScopeElement,
                                       nsCSSRuleProcessor*
                                         aPreviousCSSRuleProcessor,
                                       bool aIsShared)
  : nsCSSRuleProcessor(sheet_array_type(aSheets), aSheetType, aScopeElement,
                       aPreviousCSSRuleProcessor, aIsShared)
{
}

nsCSSRuleProcessor::nsCSSRuleProcessor(sheet_array_type&& aSheets,
                                       SheetType aSheetType,
                                       Element* aScopeElement,
                                       nsCSSRuleProcessor*
                                         aPreviousCSSRuleProcessor,
                                       bool aIsShared)
  : mSheets(aSheets)
  , mRuleCascades(nullptr)
  , mPreviousCacheKey(aPreviousCSSRuleProcessor
                       ? aPreviousCSSRuleProcessor->CloneMQCacheKey()
                       : UniquePtr<nsMediaQueryResultCacheKey>())
  , mLastPresContext(nullptr)
  , mScopeElement(aScopeElement)
  , mStyleSetRefCnt(0)
  , mSheetType(aSheetType)
  , mIsShared(aIsShared)
  , mMustGatherDocumentRules(aIsShared)
  , mInRuleProcessorCache(false)
#ifdef DEBUG
  , mDocumentRulesAndCacheKeyValid(false)
#endif
{
  NS_ASSERTION(!!mScopeElement == (aSheetType == SheetType::ScopedDoc),
               "aScopeElement must be specified iff aSheetType is "
               "eScopedDocSheet");
  for (sheet_array_type::size_type i = mSheets.Length(); i-- != 0; ) {
    mSheets[i]->AddRuleProcessor(this);
  }
}

nsCSSRuleProcessor::~nsCSSRuleProcessor()
{
  if (mInRuleProcessorCache) {
    RuleProcessorCache::RemoveRuleProcessor(this);
  }
  MOZ_ASSERT(!mExpirationState.IsTracked());
  MOZ_ASSERT(mStyleSetRefCnt == 0);
  ClearSheets();
  ClearRuleCascades();
}

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsCSSRuleProcessor)
  NS_INTERFACE_MAP_ENTRY(nsIStyleRuleProcessor)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(nsCSSRuleProcessor)
NS_IMPL_CYCLE_COLLECTING_RELEASE(nsCSSRuleProcessor)

NS_IMPL_CYCLE_COLLECTION_CLASS(nsCSSRuleProcessor)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(nsCSSRuleProcessor)
  tmp->ClearSheets();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mScopeElement)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(nsCSSRuleProcessor)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSheets)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mScopeElement)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

void
nsCSSRuleProcessor::ClearSheets()
{
  for (sheet_array_type::size_type i = mSheets.Length(); i-- != 0; ) {
    mSheets[i]->DropRuleProcessor(this);
  }
  mSheets.Clear();
}

/* virtual */ void
nsCSSRuleProcessor::RulesMatching(ElementRuleProcessorData *aData)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aData->mPresContext);

  if (!resolvedCascade) {
    return;
  }
  for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
    cascade->RulesMatching(aData);
  }
}

/* virtual */ void
nsCSSRuleProcessor::RulesMatching(PseudoElementRuleProcessorData* aData)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aData->mPresContext);

  if (!resolvedCascade) {
    return;
  }
  for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
    cascade->RulesMatching(aData);
  }
}

/* virtual */ void
nsCSSRuleProcessor::RulesMatching(AnonBoxRuleProcessorData* aData)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aData->mPresContext);

  if (!resolvedCascade) {
    return;
  }
  for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
    cascade->RulesMatching(aData);
  }
}

#ifdef MOZ_XUL
/* virtual */ void
nsCSSRuleProcessor::RulesMatching(XULTreeRuleProcessorData* aData)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aData->mPresContext);

  if (!resolvedCascade) {
    return;
  }
  for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
    cascade->RulesMatching(aData);
  }
}
#endif

nsRestyleHint
nsCSSRuleProcessor::HasStateDependentStyle(ElementDependentRuleProcessorData* aData,
                                           Element* aStatefulElement,
                                           CSSPseudoElementType aPseudoType,
                                           EventStates aStateMask)
{
  MOZ_ASSERT(!aData->mTreeMatchContext.mForScopedStyle,
             "mCurrentStyleScope will need to be saved and restored after the "
             "SelectorMatchesTree call");

  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aData->mPresContext);

  nsRestyleHint hint = nsRestyleHint(0);
  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      cascade->HasStateDependentStyle(
        aData, aStatefulElement, aPseudoType, aStateMask, hint);
    }
  }
  return hint;
}

nsRestyleHint
nsCSSRuleProcessor::HasStateDependentStyle(StateRuleProcessorData* aData)
{
  return HasStateDependentStyle(aData,
                                aData->mElement,
                                CSSPseudoElementType::NotPseudo,
                                aData->mStateMask);
}

nsRestyleHint
nsCSSRuleProcessor::HasStateDependentStyle(PseudoElementStateRuleProcessorData* aData)
{
  return HasStateDependentStyle(aData,
                                aData->mPseudoElement,
                                aData->mPseudoType,
                                aData->mStateMask);
}

bool
nsCSSRuleProcessor::HasDocumentStateDependentStyle(StateRuleProcessorData* aData)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aData->mPresContext);

  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      if (cascade->mSelectorDocumentStates.HasAtLeastOneOfStates(
            aData->mStateMask)) {
        return true;
      }
    }
  }

  return false;
}

nsRestyleHint
nsCSSRuleProcessor::HasAttributeDependentStyle(
    AttributeRuleProcessorData* aData,
    RestyleHintData& aRestyleHintDataResult)
{
  //  We could try making use of aData->mModType, but :not rules make it a bit
  //  of a pain to do so...  So just ignore it for now.

  AttributeEnumData data(aData, aRestyleHintDataResult);

  // Don't do our special handling of certain attributes if the attr
  // hasn't changed yet.
  if (aData->mAttrHasChanged) {
    // check for the lwtheme and lwthemetextcolor attribute on root XUL elements
    if ((aData->mAttribute == nsGkAtoms::lwtheme ||
         aData->mAttribute == nsGkAtoms::lwthemetextcolor) &&
        aData->mElement->GetNameSpaceID() == kNameSpaceID_XUL &&
        aData->mElement == aData->mElement->OwnerDoc()->GetRootElement())
      {
        data.change = nsRestyleHint(data.change | eRestyle_Subtree);
      }

    // We don't know the namespace of the attribute, and xml:lang applies to
    // all elements.  If the lang attribute changes, we need to restyle our
    // whole subtree, since the :lang selector on our descendants can examine
    // our lang attribute.
    if (aData->mAttribute == nsGkAtoms::lang) {
      data.change = nsRestyleHint(data.change | eRestyle_Subtree);
    }
  }

  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aData->mPresContext);

  // Since we get both before and after notifications for attributes, we
  // don't have to ignore aData->mAttribute while matching.  Just check
  // whether we have selectors relevant to aData->mAttribute that we
  // match.  If this is the before change notification, that will catch
  // rules we might stop matching; if the after change notification, the
  // ones we might have started matching.
  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      cascade->HasAttributeDependentStyle(
        aData, &data, aRestyleHintDataResult);
    }
  }

  return data.change;
}

/* virtual */ bool
nsCSSRuleProcessor::MediumFeaturesChanged(nsPresContext* aPresContext)
{
  // We don't want to do anything if there aren't any sets of rules
  // cached yet, since we should not build the rule cascade too early
  // (e.g., before we know whether the quirk style sheet should be
  // enabled).  And if there's nothing cached, it doesn't matter if
  // anything changed.  But in the cases where it does matter, we've
  // cached a previous cache key to test against, instead of our current
  // rule cascades.  See bug 448281 and bug 1089417.
  MOZ_ASSERT(!(mRuleCascades && mPreviousCacheKey));
  ResolvedRuleCascades* old = mRuleCascades;
  if (old) {  
    RefreshRuleCascade(aPresContext);
    return (old != mRuleCascades);
  }

  if (mPreviousCacheKey) {
    // RefreshRuleCascade will get rid of mPreviousCacheKey anyway to
    // maintain the invariant that we can't have both an mRuleCascades
    // and an mPreviousCacheKey.  But we need to hold it a little
    // longer.
    UniquePtr<nsMediaQueryResultCacheKey> previousCacheKey(
      Move(mPreviousCacheKey));
    RefreshRuleCascade(aPresContext);

    // This test is a bit pessimistic since the cache key's operator==
    // just does list comparison rather than set comparison, but it
    // should catch all the cases we care about (i.e., where the cascade
    // order hasn't changed).  Other cases will do a restyle anyway, so
    // we shouldn't need to worry about posting a second.
    return !mRuleCascades || // all sheets gone, but we had sheets before
           mRuleCascades->mUnlayered->mCacheKey != *previousCacheKey;
  }

  return false;
}

UniquePtr<nsMediaQueryResultCacheKey>
nsCSSRuleProcessor::CloneMQCacheKey()
{
  MOZ_ASSERT(!(mRuleCascades && mPreviousCacheKey));

  ResolvedRuleCascades* c = mRuleCascades;
  if (!c || !c->mUnlayered) {
    // We might have an mPreviousCacheKey.  It already comes from a call
    // to CloneMQCacheKey, so don't bother checking
    // HasFeatureConditions().
    if (mPreviousCacheKey) {
      NS_ASSERTION(mPreviousCacheKey->HasFeatureConditions(),
                   "we shouldn't have a previous cache key unless it has "
                   "feature conditions");
      return MakeUnique<nsMediaQueryResultCacheKey>(*mPreviousCacheKey);
    }

    return UniquePtr<nsMediaQueryResultCacheKey>();
  }

  if (!c->mUnlayered->mCacheKey.HasFeatureConditions()) {
    return UniquePtr<nsMediaQueryResultCacheKey>();
  }

  return MakeUnique<nsMediaQueryResultCacheKey>(c->mUnlayered->mCacheKey);
}

/* virtual */ size_t
nsCSSRuleProcessor::SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const
{
  size_t n = 0;
  n += mSheets.ShallowSizeOfExcludingThis(aMallocSizeOf);
  for (ResolvedRuleCascades* cascade = mRuleCascades; cascade;
       cascade = cascade->mNext) {
    n += cascade->SizeOfIncludingThis(aMallocSizeOf);
  }

  return n;
}

/* virtual */ size_t
nsCSSRuleProcessor::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const
{
  return aMallocSizeOf(this) + SizeOfExcludingThis(aMallocSizeOf);
}

// Append all the currently-active font face rules to aArray.  Return
// true for success and false for failure.
bool
nsCSSRuleProcessor::AppendFontFaceRules(
                              nsPresContext *aPresContext,
                              nsTArray<nsFontFaceRuleContainer>& aArray)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aPresContext);

  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      if (!aArray.AppendElements(cascade->mFontFaceRules)) {
        return false;
      }
    }
  }
  
  return true;
}

nsCSSKeyframesRule*
nsCSSRuleProcessor::KeyframesRuleForName(nsPresContext* aPresContext,
                                         const nsString& aName)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aPresContext);

  nsCSSKeyframesRule* rule = nullptr;
  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      if (nsCSSKeyframesRule* newRule =
            cascade->mKeyframesRuleTable.Get(aName)) {
        rule = newRule;
      }
    }
  }

  return rule;
}

nsCSSCounterStyleRule*
nsCSSRuleProcessor::CounterStyleRuleForName(nsPresContext* aPresContext,
                                            const nsAString& aName)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aPresContext);

  nsCSSCounterStyleRule* rule = nullptr;
  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      if (nsCSSCounterStyleRule* newRule =
            cascade->mCounterStyleRuleTable.Get(aName)) {
        rule = newRule;
      }
    }
  }

  return rule;
}

// Append all the currently-active page rules to aArray.  Return
// true for success and false for failure.
bool
nsCSSRuleProcessor::AppendPageRules(
                              nsPresContext* aPresContext,
                              nsTArray<nsCSSPageRule*>& aArray)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aPresContext);

  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      if (!aArray.AppendElements(cascade->mPageRules)) {
        return false;
      }
    }
  }
  
  return true;
}

bool
nsCSSRuleProcessor::AppendFontFeatureValuesRules(
                              nsPresContext *aPresContext,
                              nsTArray<nsCSSFontFeatureValuesRule*>& aArray)
{
  ResolvedRuleCascades* resolvedCascade = GetRuleCascade(aPresContext);

  if (resolvedCascade) {
    for (RuleCascadeData* cascade : resolvedCascade->mOrderedData) {
      if (!aArray.AppendElements(cascade->mFontFeatureValuesRules)) {
        return false;
      }
    }
  }

  return true;
}

nsresult
nsCSSRuleProcessor::ClearRuleCascades()
{
  if (!mPreviousCacheKey) {
    mPreviousCacheKey = CloneMQCacheKey();
  }

  // No need to remove the rule processor from the RuleProcessorCache here,
  // since CSSStyleSheet::ClearRuleCascades will have called
  // RuleProcessorCache::RemoveSheet() passing itself, which will catch
  // this rule processor (and any others for different @-moz-document
  // cache key results).
  MOZ_ASSERT(!RuleProcessorCache::HasRuleProcessor(this));

#ifdef DEBUG
  // For shared rule processors, if we've already gathered document
  // rules, then they will now be out of date.  We don't actually need
  // them to be up-to-date (see the comment in RefreshRuleCascade), so
  // record their invalidity so we can assert if we try to use them.
  if (!mMustGatherDocumentRules) {
    mDocumentRulesAndCacheKeyValid = false;
  }
#endif

  // We rely on our caller (perhaps indirectly) to do something that
  // will rebuild style data and the user font set (either
  // nsIPresShell::RestyleForCSSRuleChanges or
  // nsPresContext::RebuildAllStyleData).
  ResolvedRuleCascades* data = mRuleCascades;
  mRuleCascades = nullptr;
  while (data) {
    ResolvedRuleCascades* next = data->mNext;
    delete data;
    data = next;
  }
  return NS_OK;
}


/**
 * Recursively traverses rules in order to:
 *  (1) add any @-moz-document rules into data->mDocumentRules.
 *  (2) record any @-moz-document rules whose conditions evaluate to true
 *      on data->mDocumentCacheKey.
 *
 * See also CascadeRuleEnumFunc below, which calls us via
 * EnumerateRulesForwards.  If modifying this function you may need to
 * update CascadeRuleEnumFunc too.
 */
static bool
GatherDocRuleEnumFunc(css::Rule* aRule, void* aData)
{
  CascadeEnumData* layer = (CascadeEnumData*)aData;
  int32_t type = aRule->GetType();

  MOZ_ASSERT(layer->mMustGatherDocumentRules,
             "should only call GatherDocRuleEnumFunc if "
             "mMustGatherDocumentRules is true");

  if (css::Rule::MEDIA_RULE == type || css::Rule::SUPPORTS_RULE == type ||
      css::Rule::LAYER_BLOCK_RULE == type) {
    css::GroupRule* groupRule = static_cast<css::GroupRule*>(aRule);
    if (!groupRule->EnumerateRulesForwards(GatherDocRuleEnumFunc, aData)) {
      return false;
    }
  } else if (css::Rule::DOCUMENT_RULE == type) {
    css::DocumentRule* docRule = static_cast<css::DocumentRule*>(aRule);
    if (!layer->mDocumentRules.AppendElement(docRule)) {
      return false;
    }
    if (docRule->UseForPresentation(layer->mPresContext)) {
      if (!layer->mDocumentCacheKey.AddMatchingRule(docRule)) {
        return false;
      }
    }
    if (!docRule->EnumerateRulesForwards(GatherDocRuleEnumFunc, aData)) {
      return false;
    }
  }
  return true;
}

/*
 * This enumerates style rules in a sheet (and recursively into any
 * grouping rules) in order to:
 *  (1) add any style rules, in order, into data->mRulesByWeight (for
 *      the primary CSS cascade), where they are separated by weight
 *      but kept in order per-weight, and
 *  (2) add any @font-face rules, in order, into data->mFontFaceRules.
 *  (3) add any @keyframes rules, in order, into data->mKeyframesRules.
 *  (4) add any @font-feature-value rules, in order,
 *      into data->mFontFeatureValuesRules.
 *  (5) add any @page rules, in order, into data->mPageRules.
 *  (6) add any @counter-style rules, in order, into data->mCounterStyleRules.
 *  (7) add any @-moz-document rules into data->mDocumentRules.
 *  (8) record any @-moz-document rules whose conditions evaluate to true
 *      on data->mDocumentCacheKey.
 *
 * See also GatherDocRuleEnumFunc above, which we call to traverse into
 * @-moz-document rules even if their (or an ancestor's) condition
 * fails.  This means we might look at the result of some @-moz-document
 * rules that don't actually affect whether a RuleProcessorCache lookup
 * is a hit or a miss.  The presence of @-moz-document rules inside
 * @media etc. rules should be rare, and looking at all of them in the
 * sheets lets us avoid the complication of having different document
 * cache key results for different media.
 *
 * If modifying this function you may need to update
 * GatherDocRuleEnumFunc too.
 */
static bool
CascadeRuleEnumFunc(css::Rule* aRule, void* aData)
{
  CascadeEnumData* layer = (CascadeEnumData*)aData;
  int32_t type = aRule->GetType();

  if (css::Rule::STYLE_RULE == type) {
    css::StyleRule* styleRule = static_cast<css::StyleRule*>(aRule);
    if (!layer->mStyleRules.AppendElement(styleRule)) {
      return false;
    }
  } else if (css::Rule::MEDIA_RULE == type ||
             css::Rule::SUPPORTS_RULE == type) {
    css::GroupRule* groupRule = static_cast<css::GroupRule*>(aRule);
    const bool use = groupRule->UseForPresentation(layer->mPresContext,
                                                   layer->mData->mCacheKey);
    if (use || layer->mMustGatherDocumentRules) {
      if (!groupRule->EnumerateRulesForwards(
            use ? CascadeRuleEnumFunc : GatherDocRuleEnumFunc, aData)) {
        return false;
      }
    }
  } else if (css::Rule::LAYER_STATEMENT_RULE == type) {
    CSSLayerStatementRule* layerRule =
      static_cast<CSSLayerStatementRule*>(aRule);
    nsTArray<nsTArray<nsString>> pathList;
    layerRule->GetPathList(pathList);
    for (const nsTArray<nsString>& path : pathList) {
      layer->CreateNamedChildLayer(path);
    }
  } else if (css::Rule::LAYER_BLOCK_RULE == type) {
    CSSLayerBlockRule* layerRule = static_cast<CSSLayerBlockRule*>(aRule);
    nsTArray<nsString> path;
    layerRule->GetPath(path);
    nsString name;
    layerRule->GetName(name);
    CascadeEnumData* targetLayer = name.IsEmpty()
                                     ? layer->CreateAnonymousChildLayer()
                                     : layer->CreateNamedChildLayer(path);
    const bool use = layerRule->UseForPresentation(layer->mPresContext,
                                                   layer->mData->mCacheKey);
    if (use || layer->mMustGatherDocumentRules) {
      if (!layerRule->EnumerateRulesForwards(
            use ? CascadeRuleEnumFunc : GatherDocRuleEnumFunc, targetLayer)) {
        return false;
      }
    }
  } else if (css::Rule::DOCUMENT_RULE == type) {
    css::DocumentRule* docRule = static_cast<css::DocumentRule*>(aRule);
    if (layer->mMustGatherDocumentRules) {
      if (!layer->mDocumentRules.AppendElement(docRule)) {
        return false;
      }
    }
    const bool use = docRule->UseForPresentation(layer->mPresContext);
    if (use && layer->mMustGatherDocumentRules) {
      if (!layer->mDocumentCacheKey.AddMatchingRule(docRule)) {
        return false;
      }
    }
    if (use || layer->mMustGatherDocumentRules) {
      if (!docRule->EnumerateRulesForwards(
            use ? CascadeRuleEnumFunc : GatherDocRuleEnumFunc, aData)) {
        return false;
      }
    }
  } else if (css::Rule::FONT_FACE_RULE == type) {
    nsCSSFontFaceRule* fontFaceRule = static_cast<nsCSSFontFaceRule*>(aRule);
    nsFontFaceRuleContainer* ptr = layer->mData->mFontFaceRules.AppendElement();
    if (!ptr)
      return false;
    ptr->mRule = fontFaceRule;
    ptr->mSheetType = layer->mSheetType;
  } else if (css::Rule::KEYFRAMES_RULE == type) {
    nsCSSKeyframesRule* keyframesRule = static_cast<nsCSSKeyframesRule*>(aRule);
    if (!layer->mData->mKeyframesRules.AppendElement(keyframesRule)) {
      return false;
    }
  } else if (css::Rule::FONT_FEATURE_VALUES_RULE == type) {
    nsCSSFontFeatureValuesRule* fontFeatureValuesRule =
      static_cast<nsCSSFontFeatureValuesRule*>(aRule);
    if (!layer->mData->mFontFeatureValuesRules.AppendElement(
          fontFeatureValuesRule)) {
      return false;
    }
  } else if (css::Rule::PAGE_RULE == type) {
    nsCSSPageRule* pageRule = static_cast<nsCSSPageRule*>(aRule);
    if (!layer->mData->mPageRules.AppendElement(pageRule)) {
      return false;
    }
  } else if (css::Rule::COUNTER_STYLE_RULE == type) {
    nsCSSCounterStyleRule* counterStyleRule =
      static_cast<nsCSSCounterStyleRule*>(aRule);
    if (!layer->mData->mCounterStyleRules.AppendElement(counterStyleRule)) {
      return false;
    }
  }
  return true;
}

/* static */ bool
nsCSSRuleProcessor::CascadeSheet(CSSStyleSheet* aSheet, CascadeEnumData* aLayer)
{
  if (aSheet->IsApplicable() &&
      aSheet->UseForPresentation(aLayer->mPresContext,
                                 aLayer->mData->mCacheKey) &&
      aSheet->mInner) {
    CSSStyleSheet* child = aSheet->mInner->mFirstChild;
    while (child) {
      CascadeSheet(child, aLayer);
      child = child->mNext;
    }

    if (!aSheet->mInner->mOrderedRules.EnumerateForwards(CascadeRuleEnumFunc,
                                                         aLayer))
      return false;
  }
  return true;
}

ResolvedRuleCascades*
nsCSSRuleProcessor::GetRuleCascade(nsPresContext* aPresContext)
{
  // FIXME:  Make this infallible!

  // If anything changes about the presentation context, we will be
  // notified.  Otherwise, our cache is valid if mLastPresContext
  // matches aPresContext.  (The only rule processors used for multiple
  // pres contexts are for XBL.  These rule processors are probably less
  // likely to have @media rules, and thus the cache is pretty likely to
  // hit instantly even when we're switching between pres contexts.)

  if (!mRuleCascades || aPresContext != mLastPresContext) {
    RefreshRuleCascade(aPresContext);
  }
  mLastPresContext = aPresContext;

  return mRuleCascades;
}

void
nsCSSRuleProcessor::RefreshRuleCascade(nsPresContext* aPresContext)
{
  // Having RuleCascadeData objects be per-medium (over all variation
  // caused by media queries, handled through mCacheKey) works for now
  // since nsCSSRuleProcessor objects are per-document.  (For a given
  // set of stylesheets they can vary based on medium (@media) or
  // document (@-moz-document).)
  for (ResolvedRuleCascades** cascadep = &mRuleCascades, *cascade;
        (cascade = *cascadep);
        cascadep = &cascade->mNext) {
    if (cascade->mUnlayered->mCacheKey.Matches(aPresContext)) {
      // Ensure that the current one is always mRuleCascades.
      *cascadep = cascade->mNext;
      cascade->mNext = mRuleCascades;
      mRuleCascades = cascade;

      return;
    }
  }

  // We're going to make a new rule cascade; this means that we should
  // now stop using the previous cache key that we're holding on to from
  // the last time we had rule cascades.
  mPreviousCacheKey = nullptr;

  if (mSheets.Length() != 0) {
    nsAutoPtr<ResolvedRuleCascades> resolvedCascade =
      new ResolvedRuleCascades();
    CascadeEnumData unlayered(aPresContext,
                              resolvedCascade,
                              mDocumentRules,
                              mDocumentCacheKey,
                              mSheetType,
                              mMustGatherDocumentRules);
    if (unlayered.mData) {
      for (uint32_t i = 0; i < mSheets.Length(); ++i) {
        if (!CascadeSheet(mSheets.ElementAt(i), &unlayered)) {
          return; /* out of memory */
        }
      }

      unlayered.Flatten();
      resolvedCascade->mUnlayered = unlayered.mData;

      // mMustGatherDocumentRules controls whether we build mDocumentRules
      // and mDocumentCacheKey so that they can be used as keys by the
      // RuleProcessorCache, as obtained by TakeDocumentRulesAndCacheKey
      // later.  We set it to false just below so that we only do this
      // the first time we build a RuleProcessorCache for a shared rule
      // processor.
      //
      // An up-to-date mDocumentCacheKey is only needed if we
      // are still in the RuleProcessorCache (as we store a copy of the
      // cache key in the RuleProcessorCache), and an up-to-date
      // mDocumentRules is only needed at the time TakeDocumentRulesAndCacheKey
      // is called, which is immediately after the rule processor is created
      // (by nsStyleSet).
      //
      // Note that when nsCSSRuleProcessor::ClearRuleCascades is called,
      // by CSSStyleSheet::ClearRuleCascades, we will have called
      // RuleProcessorCache::RemoveSheet, which will remove the rule
      // processor from the cache.  (This is because the list of document
      // rules now may not match the one used as they key in the
      // RuleProcessorCache.)
      //
      // Thus, as we'll no longer be in the RuleProcessorCache, and we won't
      // have TakeDocumentRulesAndCacheKey called on us, we don't need to ensure
      // mDocumentCacheKey and mDocumentRules are up-to-date after the
      // first time GetRuleCascade is called.
      if (mMustGatherDocumentRules) {
        mDocumentRules.Sort();
        mDocumentCacheKey.Finalize();
        mMustGatherDocumentRules = false;
#ifdef DEBUG
        mDocumentRulesAndCacheKeyValid = true;
#endif
      }

      // Ensure that the current one is always mRuleCascades.
      resolvedCascade->mNext = mRuleCascades;
      mRuleCascades = resolvedCascade.forget();
    }
  }
  return;
}

void
nsCSSRuleProcessor::TakeDocumentRulesAndCacheKey(
    nsPresContext* aPresContext,
    nsTArray<css::DocumentRule*>& aDocumentRules,
    nsDocumentRuleResultCacheKey& aCacheKey)
{
  MOZ_ASSERT(mIsShared);

  GetRuleCascade(aPresContext);
  MOZ_ASSERT(mDocumentRulesAndCacheKeyValid);

  aDocumentRules.Clear();
  aDocumentRules.SwapElements(mDocumentRules);
  aCacheKey.Swap(mDocumentCacheKey);

#ifdef DEBUG
  mDocumentRulesAndCacheKeyValid = false;
#endif
}

void
nsCSSRuleProcessor::AddStyleSetRef()
{
  MOZ_ASSERT(mIsShared);
  if (++mStyleSetRefCnt == 1) {
    RuleProcessorCache::StopTracking(this);
  }
}

void
nsCSSRuleProcessor::ReleaseStyleSetRef()
{
  MOZ_ASSERT(mIsShared);
  MOZ_ASSERT(mStyleSetRefCnt > 0);
  if (--mStyleSetRefCnt == 0 && mInRuleProcessorCache) {
    RuleProcessorCache::StartTracking(this);
  }
}
