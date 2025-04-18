/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * representation of CSS style rules (selectors+declaration), CSS
 * selectors, and DOM objects for style rules, selectors, and
 * declarations
 */

#include "mozilla/css/StyleRule.h"

#include "mozilla/StyleSheetInlines.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/css/GroupRule.h"
#include "mozilla/css/Declaration.h"
#include "mozilla/dom/CSSStyleRuleBinding.h"
#include "nsIDocument.h"
#include "nsIAtom.h"
#include "nsString.h"
#include "nsStyleUtil.h"
#include "nsDOMCSSDeclaration.h"
#include "nsNameSpaceManager.h"
#include "nsXMLNameSpaceMap.h"
#include "nsCSSParser.h"
#include "nsCSSPseudoClasses.h"
#include "nsCSSAnonBoxes.h"
#include "nsTArray.h"
#include "nsContentUtils.h"
#include "nsError.h"
#include "mozAutoDocUpdate.h"

class nsIDOMCSSStyleDeclaration;
class nsIDOMCSSStyleSheet;

using namespace mozilla;

#define NS_IF_CLONE(member_)                                                  \
  PR_BEGIN_MACRO                                                              \
    if (member_) {                                                            \
      result->member_ = member_->Clone();                                     \
      if (!result->member_) {                                                 \
        delete result;                                                        \
        return nullptr;                                                        \
      }                                                                       \
    }                                                                         \
  PR_END_MACRO

#define NS_IF_DELETE(ptr)                                                     \
  PR_BEGIN_MACRO                                                              \
    delete ptr;                                                               \
    ptr = nullptr;                                                             \
  PR_END_MACRO

/* ************************************************************************** */

nsAtomList::nsAtomList(nsIAtom* aAtom)
  : mAtom(aAtom),
    mNext(nullptr)
{
  MOZ_COUNT_CTOR(nsAtomList);
}

nsAtomList::nsAtomList(const nsString& aAtomValue)
  : mAtom(nullptr),
    mNext(nullptr)
{
  MOZ_COUNT_CTOR(nsAtomList);
  mAtom = NS_Atomize(aAtomValue);
}

nsAtomList*
nsAtomList::Clone(bool aDeep) const
{
  nsAtomList *result = new nsAtomList(mAtom);
  if (!result)
    return nullptr;

  if (aDeep)
    NS_CSS_CLONE_LIST_MEMBER(nsAtomList, this, mNext, result, (false));
  return result;
}

size_t
nsAtomList::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  size_t n = 0;
  const nsAtomList* a = this;
  while (a) {
    n += aMallocSizeOf(a);

    // The following members aren't measured:
    // - a->mAtom, because it may be shared

    a = a->mNext;
  }
  return n;
}

nsAtomList::~nsAtomList(void)
{
  MOZ_COUNT_DTOR(nsAtomList);
  NS_CSS_DELETE_LIST_MEMBER(nsAtomList, this, mNext);
}

nsPseudoClassList::nsPseudoClassList(CSSPseudoClassType aType)
  : mType(aType),
    mNext(nullptr)
{
  NS_ASSERTION(!nsCSSPseudoClasses::HasStringArg(aType) &&
               !nsCSSPseudoClasses::HasNthPairArg(aType),
               "unexpected pseudo-class");
  MOZ_COUNT_CTOR(nsPseudoClassList);
  u.mMemory = nullptr;
}

nsPseudoClassList::nsPseudoClassList(CSSPseudoClassType aType,
                                     const char16_t* aString)
  : mType(aType),
    mNext(nullptr)
{
  NS_ASSERTION(nsCSSPseudoClasses::HasStringArg(aType),
               "unexpected pseudo-class");
  NS_ASSERTION(aString, "string expected");
  MOZ_COUNT_CTOR(nsPseudoClassList);
  u.mString = NS_strdup(aString);
}

nsPseudoClassList::nsPseudoClassList(CSSPseudoClassType aType,
                                     const int32_t* aIntPair)
  : mType(aType),
    mNext(nullptr)
{
  NS_ASSERTION(nsCSSPseudoClasses::HasNthPairArg(aType),
               "unexpected pseudo-class");
  NS_ASSERTION(aIntPair, "integer pair expected");
  MOZ_COUNT_CTOR(nsPseudoClassList);
  u.mNumbers =
    static_cast<int32_t*>(nsMemory::Clone(aIntPair, sizeof(int32_t) * 2));
}

// adopts aSelectorList
nsPseudoClassList::nsPseudoClassList(CSSPseudoClassType aType,
                                     nsCSSSelectorList* aSelectorList)
  : mType(aType),
    mNext(nullptr)
{
  NS_ASSERTION(nsCSSPseudoClasses::HasSelectorListArg(aType) ||
               nsCSSPseudoClasses::HasOptionalSelectorListArg(aType),
               "unexpected pseudo-class");
  NS_ASSERTION(aSelectorList, "selector list expected");
  MOZ_COUNT_CTOR(nsPseudoClassList);
  u.mSelectorList = aSelectorList;
}

nsPseudoClassList*
nsPseudoClassList::Clone(bool aDeep) const
{
  nsPseudoClassList *result;
  if (!u.mMemory) {
    result = new nsPseudoClassList(mType);
  } else if (nsCSSPseudoClasses::HasStringArg(mType)) {
    result = new nsPseudoClassList(mType, u.mString);
  } else if (nsCSSPseudoClasses::HasNthPairArg(mType)) {
    result = new nsPseudoClassList(mType, u.mNumbers);
  } else {
    NS_ASSERTION(nsCSSPseudoClasses::HasSelectorListArg(mType),
                 "unexpected pseudo-class");
    // This constructor adopts its selector list argument.
    result = new nsPseudoClassList(mType, u.mSelectorList->Clone());
  }

  if (aDeep)
    NS_CSS_CLONE_LIST_MEMBER(nsPseudoClassList, this, mNext, result,
                             (false));

  return result;
}

size_t
nsPseudoClassList::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  size_t n = 0;
  const nsPseudoClassList* p = this;
  while (p) {
    n += aMallocSizeOf(p);
    if (!p->u.mMemory) {
      // do nothing

    } else if (nsCSSPseudoClasses::HasStringArg(p->mType)) {
      n += aMallocSizeOf(p->u.mString);

    } else if (nsCSSPseudoClasses::HasNthPairArg(p->mType)) {
      n += aMallocSizeOf(p->u.mNumbers);

    } else {
      NS_ASSERTION(nsCSSPseudoClasses::HasSelectorListArg(p->mType),
                   "unexpected pseudo-class");
      n += p->u.mSelectorList->SizeOfIncludingThis(aMallocSizeOf);
    }
    p = p->mNext;
  }
  return n;
}

nsPseudoClassList::~nsPseudoClassList(void)
{
  MOZ_COUNT_DTOR(nsPseudoClassList);
  if (nsCSSPseudoClasses::HasSelectorListArg(mType)) {
    delete u.mSelectorList;
  } else if (u.mMemory) {
    free(u.mMemory);
  }
  NS_CSS_DELETE_LIST_MEMBER(nsPseudoClassList, this, mNext);
}

nsAttrSelector::nsAttrSelector(int32_t aNameSpace, const nsString& aAttr)
  : mValue(),
    mNext(nullptr),
    mLowercaseAttr(nullptr),
    mCasedAttr(nullptr),
    mNameSpace(aNameSpace),
    mFunction(NS_ATTR_FUNC_SET),
    // mValueCaseSensitivity doesn't matter; we have no value.
    mValueCaseSensitivity(ValueCaseSensitivity::CaseSensitive)
{
  MOZ_COUNT_CTOR(nsAttrSelector);

  nsAutoString lowercase;
  nsContentUtils::ASCIIToLower(aAttr, lowercase);
  
  mCasedAttr = NS_Atomize(aAttr);
  mLowercaseAttr = NS_Atomize(lowercase);
}

nsAttrSelector::nsAttrSelector(int32_t aNameSpace, const nsString& aAttr, uint8_t aFunction, 
                               const nsString& aValue,
                               ValueCaseSensitivity aValueCaseSensitivity)
  : mValue(aValue),
    mNext(nullptr),
    mLowercaseAttr(nullptr),
    mCasedAttr(nullptr),
    mNameSpace(aNameSpace),
    mFunction(aFunction),
    mValueCaseSensitivity(aValueCaseSensitivity)
{
  MOZ_COUNT_CTOR(nsAttrSelector);

  nsAutoString lowercase;
  nsContentUtils::ASCIIToLower(aAttr, lowercase);
  
  mCasedAttr = NS_Atomize(aAttr);
  mLowercaseAttr = NS_Atomize(lowercase);
}

nsAttrSelector::nsAttrSelector(int32_t aNameSpace,  nsIAtom* aLowercaseAttr,
                               nsIAtom* aCasedAttr, uint8_t aFunction, 
                               const nsString& aValue,
                               ValueCaseSensitivity aValueCaseSensitivity)
  : mValue(aValue),
    mNext(nullptr),
    mLowercaseAttr(aLowercaseAttr),
    mCasedAttr(aCasedAttr),
    mNameSpace(aNameSpace),
    mFunction(aFunction),
    mValueCaseSensitivity(aValueCaseSensitivity)
{
  MOZ_COUNT_CTOR(nsAttrSelector);
}

nsAttrSelector*
nsAttrSelector::Clone(bool aDeep) const
{
  nsAttrSelector *result =
    new nsAttrSelector(mNameSpace, mLowercaseAttr, mCasedAttr, 
                       mFunction, mValue, mValueCaseSensitivity);

  if (aDeep)
    NS_CSS_CLONE_LIST_MEMBER(nsAttrSelector, this, mNext, result, (false));

  return result;
}

nsAttrSelector::~nsAttrSelector(void)
{
  MOZ_COUNT_DTOR(nsAttrSelector);

  NS_CSS_DELETE_LIST_MEMBER(nsAttrSelector, this, mNext);
}

size_t
nsAttrSelector::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  size_t n = 0;
  const nsAttrSelector* p = this;
  while (p) {
    n += aMallocSizeOf(p);
    n += p->mValue.SizeOfExcludingThisIfUnshared(aMallocSizeOf);
    p = p->mNext;
  }
  return n;
}

// -- nsCSSSelector -------------------------------

nsCSSSelector::nsCSSSelector(void)
  : mLowercaseTag(nullptr),
    mCasedTag(nullptr),
    mIDList(nullptr),
    mClassList(nullptr),
    mPseudoClassList(nullptr),
    mAttrList(nullptr),
    mNegations(nullptr),
    mExplicitUniversal(false),
    mNext(nullptr),
    mNameSpace(kNameSpaceID_Unknown),
    mOperator(0),
    mPseudoType(CSSPseudoElementType::NotPseudo),
    mHybridPseudoType(CSSPseudoElementType::NotPseudo)
{
  MOZ_COUNT_CTOR(nsCSSSelector);
}

nsCSSSelector*
nsCSSSelector::Clone(bool aDeepNext, bool aDeepNegations) const
{
  nsCSSSelector *result = new nsCSSSelector();
  if (!result)
    return nullptr;

  result->mNameSpace = mNameSpace;
  result->mLowercaseTag = mLowercaseTag;
  result->mCasedTag = mCasedTag;
  result->mOperator = mOperator;
  result->mPseudoType = mPseudoType;

  NS_IF_CLONE(mIDList);
  NS_IF_CLONE(mClassList);
  NS_IF_CLONE(mPseudoClassList);
  NS_IF_CLONE(mAttrList);

  // No need to worry about multiple levels of recursion since an
  // mNegations can't have an mNext.
  NS_ASSERTION(!mNegations || !mNegations->mNext,
               "mNegations can't have non-null mNext");
  if (aDeepNegations) {
    NS_CSS_CLONE_LIST_MEMBER(nsCSSSelector, this, mNegations, result,
                             (true, false));
  }

  if (aDeepNext) {
    NS_CSS_CLONE_LIST_MEMBER(nsCSSSelector, this, mNext, result,
                             (false, true));
  }

  return result;
}

nsCSSSelector::~nsCSSSelector(void)  
{
  MOZ_COUNT_DTOR(nsCSSSelector);
  Reset();
  // No need to worry about multiple levels of recursion since an
  // mNegations can't have an mNext.
  NS_CSS_DELETE_LIST_MEMBER(nsCSSSelector, this, mNext);
}

void nsCSSSelector::Reset(void)
{
  mNameSpace = kNameSpaceID_Unknown;
  mLowercaseTag = nullptr;
  mCasedTag = nullptr;
  NS_IF_DELETE(mIDList);
  NS_IF_DELETE(mClassList);
  NS_IF_DELETE(mPseudoClassList);
  NS_IF_DELETE(mAttrList);
  // No need to worry about multiple levels of recursion since an
  // mNegations can't have an mNext.
  NS_ASSERTION(!mNegations || !mNegations->mNext,
               "mNegations can't have non-null mNext");
  NS_CSS_DELETE_LIST_MEMBER(nsCSSSelector, this, mNegations);
  mOperator = char16_t(0);
}

bool nsCSSSelector::HasFeatureSelectors()
{
  return mExplicitUniversal || mLowercaseTag || mCasedTag ||
    mIDList || mClassList || mAttrList;
}

void nsCSSSelector::SetHasExplicitUniversal()
{
  mExplicitUniversal = true;
}

void nsCSSSelector::SetNameSpace(int32_t aNameSpace)
{
  mNameSpace = aNameSpace;
}

void nsCSSSelector::SetTag(const nsString& aTag)
{
  if (aTag.IsEmpty()) {
    mLowercaseTag = mCasedTag =  nullptr;
    return;
  }

  mCasedTag = NS_Atomize(aTag);
 
  nsAutoString lowercase;
  nsContentUtils::ASCIIToLower(aTag, lowercase);
  mLowercaseTag = NS_Atomize(lowercase);
}

void nsCSSSelector::AddID(const nsString& aID)
{
  if (!aID.IsEmpty()) {
    nsAtomList** list = &mIDList;
    while (nullptr != *list) {
      list = &((*list)->mNext);
    }
    *list = new nsAtomList(aID);
  }
}

void nsCSSSelector::AddClass(const nsString& aClass)
{
  if (!aClass.IsEmpty()) {
    nsAtomList** list = &mClassList;
    while (nullptr != *list) {
      list = &((*list)->mNext);
    }
    *list = new nsAtomList(aClass);
  }
}

void nsCSSSelector::AddPseudoClass(CSSPseudoClassType aType)
{
  AddPseudoClassInternal(new nsPseudoClassList(aType));
}

void nsCSSSelector::AddPseudoClass(CSSPseudoClassType aType,
                                   const char16_t* aString)
{
  AddPseudoClassInternal(new nsPseudoClassList(aType, aString));
}

void nsCSSSelector::AddPseudoClass(CSSPseudoClassType aType,
                                   const int32_t* aIntPair)
{
  AddPseudoClassInternal(new nsPseudoClassList(aType, aIntPair));
}

void nsCSSSelector::AddPseudoClass(CSSPseudoClassType aType,
                                   nsCSSSelectorList* aSelectorList)
{
  // Take ownership of nsCSSSelectorList instead of copying.
  AddPseudoClassInternal(new nsPseudoClassList(aType, aSelectorList));
}

void nsCSSSelector::AddPseudoClassInternal(nsPseudoClassList *aPseudoClass)
{
  nsPseudoClassList** list = &mPseudoClassList;
  while (nullptr != *list) {
    list = &((*list)->mNext);
  }
  *list = aPseudoClass;
}

void nsCSSSelector::AddAttribute(int32_t aNameSpace, const nsString& aAttr)
{
  if (!aAttr.IsEmpty()) {
    nsAttrSelector** list = &mAttrList;
    while (nullptr != *list) {
      list = &((*list)->mNext);
    }
    *list = new nsAttrSelector(aNameSpace, aAttr);
  }
}

void nsCSSSelector::AddAttribute(int32_t aNameSpace, const nsString& aAttr, uint8_t aFunc, 
                                 const nsString& aValue,
                                 nsAttrSelector::ValueCaseSensitivity aCaseSensitivity)
{
  if (!aAttr.IsEmpty()) {
    nsAttrSelector** list = &mAttrList;
    while (nullptr != *list) {
      list = &((*list)->mNext);
    }
    *list = new nsAttrSelector(aNameSpace, aAttr, aFunc, aValue, aCaseSensitivity);
  }
}

void nsCSSSelector::SetOperator(char16_t aOperator)
{
  mOperator = aOperator;
}

int32_t nsCSSSelector::CalcWeightWithoutNegations() const
{
  int32_t weight = 0;

#ifdef MOZ_XUL
  MOZ_ASSERT(!(IsPseudoElement() &&
               PseudoType() != CSSPseudoElementType::XULTree &&
               mClassList),
             "If non-XUL-tree pseudo-elements can have class selectors "
             "after them, specificity calculation must be updated");
#else
  MOZ_ASSERT(!(IsPseudoElement() && mClassList),
             "If pseudo-elements can have class selectors "
             "after them, specificity calculation must be updated");
#endif
  MOZ_ASSERT(!(IsPseudoElement() && (mIDList || mAttrList)),
             "If pseudo-elements can have id or attribute selectors "
             "after them, specificity calculation must be updated");

  if (nullptr != mCasedTag) {
    weight += 0x000001;
  }
  nsAtomList* list = mIDList;
  while (nullptr != list) {
    weight += 0x010000;
    list = list->mNext;
  }
  list = mClassList;
#ifdef MOZ_XUL
  // XUL tree pseudo-elements abuse mClassList to store some private
  // data; ignore that.
  if (PseudoType() == CSSPseudoElementType::XULTree) {
    list = nullptr;
  }
#endif
  while (nullptr != list) {
    weight += 0x000100;
    list = list->mNext;
  }
  nsPseudoClassList *plist = mPseudoClassList;
  while (nullptr != plist) {
    int pseudoClassWeight = 0x000100;
    // XXX(franklindm): Check for correctness.
    if (nsCSSPseudoClasses::HasSelectorListArg(plist->mType) &&
        plist->mType != CSSPseudoClassType::mozAny) {
      // The specificity of :host() and :host-context is that of a
      // pseudo-class, plus the specificity of its argument.
      if (plist->mType != CSSPseudoClassType::host &&
          plist->mType != CSSPseudoClassType::hostContext) {
        pseudoClassWeight = 0;
      }
      // The specificity of the :where() pseudo-class is always zero.
      if (plist->mType != CSSPseudoClassType::where) {
        // The specificity of the :is() pseudo-class is replaced by the
        // specificity of its most specific argument.
        nsCSSSelectorList* slist = plist->u.mSelectorList;
        while (slist) {
          int currentWeight = slist->mSelectors ?
                              slist->mWeight :
                              0;
          if (currentWeight > pseudoClassWeight) {
            pseudoClassWeight = currentWeight;
          }
          slist = slist->mNext;
        }
      }
    }
    weight += pseudoClassWeight;
    plist = plist->mNext;
  }
  nsAttrSelector* attr = mAttrList;
  while (nullptr != attr) {
    weight += 0x000100;
    attr = attr->mNext;
  }
  return weight;
}

int32_t nsCSSSelector::CalcWeight() const
{
  // Loop over this selector and all its negations.
  int32_t weight = 0;
  for (const nsCSSSelector *n = this; n; n = n->mNegations) {
    weight += n->CalcWeightWithoutNegations();
  }
  return weight;
}

//
// Builds the textual representation of a selector. Called by DOM 2 CSS 
// StyleRule:selectorText
//
void
nsCSSSelector::ToString(nsAString& aString, CSSStyleSheet* aSheet,
                        bool aAppend) const
{
  if (!aAppend)
   aString.Truncate();

  // selectors are linked from right-to-left, so the next selector in
  // the linked list actually precedes this one in the resulting string
  AutoTArray<const nsCSSSelector*, 8> stack;
  for (const nsCSSSelector *s = this; s; s = s->mNext) {
    stack.AppendElement(s);
  }

  while (!stack.IsEmpty()) {
    uint32_t index = stack.Length() - 1;
    const nsCSSSelector *s = stack.ElementAt(index);
    stack.RemoveElementAt(index);

    s->AppendToStringWithoutCombinators(aString, aSheet, false);

    // Append the combinator, if needed.
    if (!stack.IsEmpty()) {
      const nsCSSSelector *next = stack.ElementAt(index - 1);
      char16_t oper = s->mOperator;
      if (next->IsPseudoElement()) {
        NS_ASSERTION(oper == char16_t(':'),
                     "improperly chained pseudo element");
      } else {
        NS_ASSERTION(oper != char16_t(0),
                     "compound selector without combinator");

        aString.Append(char16_t(' '));
        if (oper != char16_t(' ')) {
          aString.Append(oper);
          aString.Append(char16_t(' '));
        }
      }
    }
  }
}

void
nsCSSSelector::AppendToStringWithoutCombinators(
    nsAString& aString,
    CSSStyleSheet* aSheet,
    bool aUseStandardNamespacePrefixes) const
{
  AppendToStringWithoutCombinatorsOrNegations(aString, aSheet, false,
                                              aUseStandardNamespacePrefixes);

  for (const nsCSSSelector* negation = mNegations; negation;
       negation = negation->mNegations) {
    aString.AppendLiteral(":not(");
    negation->AppendToStringWithoutCombinatorsOrNegations(
        aString, aSheet, true, aUseStandardNamespacePrefixes);
    aString.Append(char16_t(')'));
  }
}

#ifdef DEBUG
nsCString
nsCSSSelector::RestrictedSelectorToString() const
{
  MOZ_ASSERT(IsRestrictedSelector());

  nsString result;
  AppendToStringWithoutCombinators(result, nullptr, true);
  return NS_ConvertUTF16toUTF8(result);
}

static bool
AppendStandardNamespacePrefixToString(nsAString& aString, int32_t aNameSpace)
{
  if (aNameSpace == kNameSpaceID_Unknown) {
    // Wildcard namespace; no prefix to write.
    return false;
  }
  switch (aNameSpace) {
    case kNameSpaceID_None:
      break;
    case kNameSpaceID_XML:
      aString.AppendLiteral("xml");
      break;
    case kNameSpaceID_XHTML:
      aString.AppendLiteral("html");
      break;
    case kNameSpaceID_XLink:
      aString.AppendLiteral("xlink");
      break;
    case kNameSpaceID_XSLT:
      aString.AppendLiteral("xsl");
      break;
    case kNameSpaceID_XBL:
      aString.AppendLiteral("xbl");
      break;
    case kNameSpaceID_MathML:
      aString.AppendLiteral("math");
      break;
    case kNameSpaceID_RDF:
      aString.AppendLiteral("rdf");
      break;
    case kNameSpaceID_XUL:
      aString.AppendLiteral("xul");
      break;
    case kNameSpaceID_SVG:
      aString.AppendLiteral("svg");
      break;
    default:
      aString.AppendLiteral("ns");
      aString.AppendInt(aNameSpace);
      break;
  }
  return true;
}
#endif

void
nsCSSSelector::AppendToStringWithoutCombinatorsOrNegations
                   (nsAString& aString, CSSStyleSheet* aSheet,
                   bool aIsNegated,
                   bool aUseStandardNamespacePrefixes) const
{
  nsAutoString temp;
  bool isPseudoElement = IsPseudoElement();

  // For non-pseudo-element selectors or for lone pseudo-elements, deal with
  // namespace prefixes.
  bool wroteNamespace = false;
  if (!isPseudoElement || !mNext) {
    // append the namespace prefix if needed
    nsXMLNameSpaceMap *sheetNS = aSheet ? aSheet->GetNameSpaceMap() : nullptr;

    // sheetNS is non-null if and only if we had an @namespace rule.  If it's
    // null, that means that the only namespaces we could have are the
    // wildcard namespace (which can be implicit in this case) and the "none"
    // namespace, which then needs to be explicitly specified.
    if (aUseStandardNamespacePrefixes) {
#ifdef DEBUG
      // We have no sheet to look up prefix information from.  This is
      // only for debugging, so use some "standard" prefixes that
      // are recognizable.
      wroteNamespace =
        AppendStandardNamespacePrefixToString(aString, mNameSpace);
      if (wroteNamespace) {
        aString.Append(char16_t('|'));
      }
#endif
    } else if (!sheetNS) {
      NS_ASSERTION(mNameSpace == kNameSpaceID_Unknown ||
                   mNameSpace == kNameSpaceID_None,
                   "How did we get this namespace?");
      if (mNameSpace == kNameSpaceID_None) {
        aString.Append(char16_t('|'));
        wroteNamespace = true;
      }
    } else if (sheetNS->FindNameSpaceID(nullptr) == mNameSpace) {
      // We have the default namespace (possibly including the wildcard
      // namespace).  Do nothing.
      NS_ASSERTION(mNameSpace == kNameSpaceID_Unknown ||
                   CanBeNamespaced(aIsNegated),
                   "How did we end up with this namespace?");
    } else if (mNameSpace == kNameSpaceID_None) {
      NS_ASSERTION(CanBeNamespaced(aIsNegated),
                   "How did we end up with this namespace?");
      aString.Append(char16_t('|'));
      wroteNamespace = true;
    } else if (mNameSpace != kNameSpaceID_Unknown) {
      NS_ASSERTION(CanBeNamespaced(aIsNegated),
                   "How did we end up with this namespace?");
      nsIAtom *prefixAtom = sheetNS->FindPrefix(mNameSpace);
      NS_ASSERTION(prefixAtom, "how'd we get a non-default namespace "
                   "without a prefix?");
      nsStyleUtil::AppendEscapedCSSIdent(nsDependentAtomString(prefixAtom),
                                         aString);
      aString.Append(char16_t('|'));
      wroteNamespace = true;
    } else {
      // A selector for an element in any namespace, while the default
      // namespace is something else.  :not() is special in that the default
      // namespace is not implied for non-type selectors, so if this is a
      // negated non-type selector we don't need to output an explicit wildcard
      // namespace here, since those default to a wildcard namespace.
      if (CanBeNamespaced(aIsNegated)) {
        aString.AppendLiteral("*|");
        wroteNamespace = true;
      }
    }
  }
      
  if (!mLowercaseTag) {
    // Universal selector:  avoid writing the universal selector when we
    // can avoid it, especially since we're required to avoid it for the
    // inside of :not()
    if (wroteNamespace || mExplicitUniversal ||
        (!mIDList && !mClassList && !mPseudoClassList && !mAttrList &&
         aIsNegated)) {
      aString.Append(char16_t('*'));
    }
  } else {
    // Append the tag name
    nsAutoString tag;
    (isPseudoElement ? mLowercaseTag : mCasedTag)->ToString(tag);
    if (isPseudoElement) {
      if (mExplicitUniversal) {
        aString.Append(char16_t('*'));
      }
      // While our atoms use one colon, most pseudo-elements require two
      // colons (those not in CSS level 2) and all pseudo-elements allow
      // two colons. So serialize to the non-deprecated two colon syntax.
      aString.Append(char16_t(':'));
      // This should not be escaped since (a) the pseudo-element string
      // has a ":" that can't be escaped and (b) all pseudo-elements at
      // this point are known, and therefore we know they don't need
      // escaping.
      aString.Append(tag);
    } else {
      nsStyleUtil::AppendEscapedCSSIdent(tag, aString);
    }
  }

  // Append the id, if there is one
  if (mIDList) {
    nsAtomList* list = mIDList;
    while (list != nullptr) {
      list->mAtom->ToString(temp);
      aString.Append(char16_t('#'));
      nsStyleUtil::AppendEscapedCSSIdent(temp, aString);
      list = list->mNext;
    }
  }

  // Append each class in the linked list
  if (mClassList) {
    if (isPseudoElement) {
#ifdef MOZ_XUL
      MOZ_ASSERT(nsCSSAnonBoxes::IsTreePseudoElement(mLowercaseTag),
                 "must be tree pseudo-element");

      aString.Append(char16_t('('));
      for (nsAtomList* list = mClassList; list; list = list->mNext) {
        nsStyleUtil::AppendEscapedCSSIdent(nsDependentAtomString(list->mAtom), aString);
        aString.Append(char16_t(','));
      }
      // replace the final comma with a close-paren
      aString.Replace(aString.Length() - 1, 1, char16_t(')'));
#else
      NS_ERROR("Can't happen");
#endif
    } else {
      nsAtomList* list = mClassList;
      while (list != nullptr) {
        list->mAtom->ToString(temp);
        aString.Append(char16_t('.'));
        nsStyleUtil::AppendEscapedCSSIdent(temp, aString);
        list = list->mNext;
      }
    }
  }

  // Append each attribute selector in the linked list
  if (mAttrList) {
    nsAttrSelector* list = mAttrList;
    while (list != nullptr) {
      aString.Append(char16_t('['));
      // Append the namespace prefix
      if (list->mNameSpace == kNameSpaceID_Unknown) {
        aString.Append(char16_t('*'));
        aString.Append(char16_t('|'));
      } else if (list->mNameSpace != kNameSpaceID_None) {
        if (aUseStandardNamespacePrefixes) {
#ifdef DEBUG
          AppendStandardNamespacePrefixToString(aString, list->mNameSpace);
          aString.Append(char16_t('|'));
#endif
        } else if (aSheet) {
          nsXMLNameSpaceMap *sheetNS = aSheet->GetNameSpaceMap();
          nsIAtom *prefixAtom = sheetNS->FindPrefix(list->mNameSpace);
          // Default namespaces don't apply to attribute selectors, so
          // we must have a useful prefix.
          NS_ASSERTION(prefixAtom,
                       "How did we end up with a namespace if the prefix "
                       "is unknown?");
          nsAutoString prefix;
          prefixAtom->ToString(prefix);
          nsStyleUtil::AppendEscapedCSSIdent(prefix, aString);
          aString.Append(char16_t('|'));
        }
      }
      // Append the attribute name
      list->mCasedAttr->ToString(temp);
      nsStyleUtil::AppendEscapedCSSIdent(temp, aString);

      if (list->mFunction != NS_ATTR_FUNC_SET) {
        // Append the function
        if (list->mFunction == NS_ATTR_FUNC_INCLUDES)
          aString.Append(char16_t('~'));
        else if (list->mFunction == NS_ATTR_FUNC_DASHMATCH)
          aString.Append(char16_t('|'));
        else if (list->mFunction == NS_ATTR_FUNC_BEGINSMATCH)
          aString.Append(char16_t('^'));
        else if (list->mFunction == NS_ATTR_FUNC_ENDSMATCH)
          aString.Append(char16_t('$'));
        else if (list->mFunction == NS_ATTR_FUNC_CONTAINSMATCH)
          aString.Append(char16_t('*'));

        aString.Append(char16_t('='));
      
        // Append the value
        nsStyleUtil::AppendEscapedCSSString(list->mValue, aString);

        if (list->mValueCaseSensitivity ==
              nsAttrSelector::ValueCaseSensitivity::CaseInsensitive) {
          aString.Append(NS_LITERAL_STRING(" i"));
        }
      }

      aString.Append(char16_t(']'));
      
      list = list->mNext;
    }
  }

  // Append each pseudo-class in the linked list
  for (nsPseudoClassList* list = mPseudoClassList; list; list = list->mNext) {
    // Serialize pseudo-elements that were treated as if they were a
    // pseudo-class to the two colon syntax.
    if (nsCSSPseudoClasses::IsHybridPseudoElement(list->mType)) {
      aString.Append(char16_t(':'));
    }
    nsCSSPseudoClasses::PseudoTypeToString(list->mType, temp);
    // This should not be escaped since (a) the pseudo-class string
    // has a ":" that can't be escaped and (b) all pseudo-classes at
    // this point are known, and therefore we know they don't need
    // escaping.
    if (!nsCSSPseudoClasses::IsHiddenFromSerialization(list->mType)) {
      aString.Append(temp);
    }
    if (list->u.mMemory) {
      if (!nsCSSPseudoClasses::IsHiddenFromSerialization(list->mType)) {
        aString.Append(char16_t('('));
      }
      if (nsCSSPseudoClasses::HasStringArg(list->mType)) {
        nsStyleUtil::AppendEscapedCSSIdent(
          nsDependentString(list->u.mString), aString);
      } else if (nsCSSPseudoClasses::HasNthPairArg(list->mType)) {
        int32_t a = list->u.mNumbers[0],
                b = list->u.mNumbers[1];
        temp.Truncate();
        if (a != 0) {
          if (a == -1) {
            temp.Append(char16_t('-'));
          } else if (a != 1) {
            temp.AppendInt(a);
          }
          temp.Append(char16_t('n'));
        }
        if (b != 0 || a == 0) {
          if (b >= 0 && a != 0) // check a != 0 for whether we printed above
            temp.Append(char16_t('+'));
          temp.AppendInt(b);
        }
        aString.Append(temp);
      } else {
        NS_ASSERTION(nsCSSPseudoClasses::HasSelectorListArg(list->mType),
                     "unexpected pseudo-class");
        nsString tmp;
        list->u.mSelectorList->ToString(tmp, aSheet);
        aString.Append(tmp);
      }
      if (!nsCSSPseudoClasses::IsHiddenFromSerialization(list->mType)) {
        aString.Append(char16_t(')'));
      }
    }
  }
}

bool
nsCSSSelector::CanBeNamespaced(bool aIsNegated) const
{
  return !aIsNegated ||
         (!mIDList && !mClassList && !mPseudoClassList && !mAttrList);
}

size_t
nsCSSSelector::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  size_t n = 0;
  const nsCSSSelector* s = this;
  while (s) {
    n += aMallocSizeOf(s);

    #define MEASURE(x)   n += x ? x->SizeOfIncludingThis(aMallocSizeOf) : 0;

    MEASURE(s->mIDList);
    MEASURE(s->mClassList);
    MEASURE(s->mPseudoClassList);
    MEASURE(s->mNegations);
    MEASURE(s->mAttrList);

    // The following members aren't measured:
    // - s->mLowercaseTag, because it's an atom and therefore shared
    // - s->mCasedTag, because it's an atom and therefore shared

    s = s->mNext;
  }
  return n;
}

// -- nsCSSSelectorList -------------------------------

nsCSSSelectorList::nsCSSSelectorList(void)
  : mSelectors(nullptr),
    mWeight(0),
    mNext(nullptr)
{
  MOZ_COUNT_CTOR(nsCSSSelectorList);
}

nsCSSSelectorList::~nsCSSSelectorList()
{
  MOZ_COUNT_DTOR(nsCSSSelectorList);
  delete mSelectors;
  NS_CSS_DELETE_LIST_MEMBER(nsCSSSelectorList, this, mNext);
}

nsCSSSelector*
nsCSSSelectorList::AddSelector(char16_t aOperator)
{
  nsCSSSelector* newSel = new nsCSSSelector();

  if (mSelectors) {
    NS_ASSERTION(aOperator != char16_t(0), "chaining without combinator");
    mSelectors->SetOperator(aOperator);
  } else {
    NS_ASSERTION(aOperator == char16_t(0), "combinator without chaining");
  }

  newSel->mNext = mSelectors;
  mSelectors = newSel;
  return newSel;
}

void
nsCSSSelectorList::RemoveRightmostSelector()
{
  nsCSSSelector* current = mSelectors;
  mSelectors = mSelectors->mNext;
  MOZ_ASSERT(mSelectors,
             "Rightmost selector has been removed, but now "
             "mSelectors is null");
  mSelectors->SetOperator(char16_t(0));

  // Make sure that deleting current won't delete the whole list.
  current->mNext = nullptr;
  delete current;
}

void
nsCSSSelectorList::ToString(nsAString& aResult, CSSStyleSheet* aSheet)
{
  aResult.Truncate();
  nsCSSSelectorList *p = this;
  // Don't append anything if we're an empty selector list.
  if (!mSelectors) {
    return;
  }
  for (;;) {
    p->mSelectors->ToString(aResult, aSheet, true);
    p = p->mNext;
    if (!p)
      break;
    aResult.AppendLiteral(", ");
  }
}

nsCSSSelectorList*
nsCSSSelectorList::Clone(bool aDeep) const
{
  nsCSSSelectorList *result = new nsCSSSelectorList();
  result->mWeight = mWeight;
  NS_IF_CLONE(mSelectors);

  if (aDeep) {
    NS_CSS_CLONE_LIST_MEMBER(nsCSSSelectorList, this, mNext, result,
                             (false));
  }
  return result;
}

size_t
nsCSSSelectorList::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  size_t n = 0;
  const nsCSSSelectorList* s = this;
  while (s) {
    n += aMallocSizeOf(s);
    n += s->mSelectors ? s->mSelectors->SizeOfIncludingThis(aMallocSizeOf) : 0;
    s = s->mNext;
  }
  return n;
}

// --------------------------------------------------------

class DOMCSSDeclarationImpl : public nsDOMCSSDeclaration
{
protected:
  // Needs to be protected so we can use NS_IMPL_ADDREF_USING_AGGREGATOR.
  virtual ~DOMCSSDeclarationImpl(void);

  // But we need to allow UniquePtr to delete us.
  friend class mozilla::DefaultDelete<DOMCSSDeclarationImpl>;

public:
  typedef mozilla::dom::DocGroup DocGroup;

  explicit DOMCSSDeclarationImpl(css::StyleRule *aRule);

  NS_IMETHOD GetParentRule(nsIDOMCSSRule **aParent) override;
  virtual css::Declaration* GetCSSDeclaration(Operation aOperation) override;
  virtual nsresult SetCSSDeclaration(css::Declaration* aDecl) override;
  virtual void GetCSSParsingEnvironment(CSSParsingEnvironment& aCSSParseEnv) override;
  virtual nsIDocument* DocToUpdate() override;

  // Override |AddRef| and |Release| for being owned by StyleRule.  Also, we
  // need to forward QI for cycle collection things to StyleRule.
  NS_DECL_ISUPPORTS_INHERITED

  virtual nsINode *GetParentObject() override
  {
    return mRule ? mRule->GetDocument() : nullptr;
  }

  virtual DocGroup* GetDocGroup() const override
  {
    if (!mRule) {
      return nullptr;
    }

    nsIDocument* document = mRule->GetDocument();
    return document ? document->GetDocGroup() : nullptr;
  }

protected:
  // This reference is not reference-counted. The rule object owns us and we go
  // away when it does.
  css::StyleRule *mRule;
};

DOMCSSDeclarationImpl::DOMCSSDeclarationImpl(css::StyleRule *aRule)
  : mRule(aRule)
{
}

DOMCSSDeclarationImpl::~DOMCSSDeclarationImpl(void)
{
}

NS_IMPL_ADDREF_USING_AGGREGATOR(DOMCSSDeclarationImpl, mRule)
NS_IMPL_RELEASE_USING_AGGREGATOR(DOMCSSDeclarationImpl, mRule)

NS_INTERFACE_MAP_BEGIN(DOMCSSDeclarationImpl)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  // We forward the cycle collection interfaces to mRule, which is
  // never null.
  if (aIID.Equals(NS_GET_IID(nsCycleCollectionISupports)) ||
      aIID.Equals(NS_GET_IID(nsXPCOMCycleCollectionParticipant))) {
    return mRule->QueryInterface(aIID, aInstancePtr);
  }
  else
NS_IMPL_QUERY_TAIL_INHERITING(nsDOMCSSDeclaration)

css::Declaration*
DOMCSSDeclarationImpl::GetCSSDeclaration(Operation aOperation)
{
  if (aOperation != eOperation_Read) {
    RefPtr<CSSStyleSheet> sheet = mRule->GetStyleSheet();
    if (sheet) {
      sheet->WillDirty();
    }
  }
  return mRule->GetDeclaration();
}

void
DOMCSSDeclarationImpl::GetCSSParsingEnvironment(CSSParsingEnvironment& aCSSParseEnv)
{
  GetCSSParsingEnvironmentForRule(mRule, aCSSParseEnv);
}

NS_IMETHODIMP
DOMCSSDeclarationImpl::GetParentRule(nsIDOMCSSRule **aParent)
{
  NS_ENSURE_ARG_POINTER(aParent);

  NS_IF_ADDREF(*aParent = mRule);
  return NS_OK;
}

nsresult
DOMCSSDeclarationImpl::SetCSSDeclaration(css::Declaration* aDecl)
{
  NS_PRECONDITION(mRule,
         "can only be called when |GetCSSDeclaration| returned a declaration");

  nsCOMPtr<nsIDocument> doc;
  RefPtr<CSSStyleSheet> sheet = mRule->GetStyleSheet();
  if (sheet) {
    doc = sheet->GetAssociatedDocument();
  }

  mozAutoDocUpdate updateBatch(doc, UPDATE_STYLE, true);

  mRule->SetDeclaration(aDecl);

  if (sheet) {
    sheet->DidDirty();
  }

  if (doc) {
    doc->StyleRuleChanged(sheet, mRule);
  }
  return NS_OK;
}

nsIDocument*
DOMCSSDeclarationImpl::DocToUpdate()
{
  return nullptr;
}

// -- StyleRule ------------------------------------

namespace mozilla {
namespace css {

uint16_t
StyleRule::Type() const
{
  return nsIDOMCSSRule::STYLE_RULE;
}

NS_IMETHODIMP
StyleRule::GetStyle(nsIDOMCSSStyleDeclaration** aStyle)
{
  NS_ADDREF(*aStyle = Style());
  return NS_OK;
}

nsICSSDeclaration*
StyleRule::Style()
{
  if (!mDOMDeclaration) {
    mDOMDeclaration.reset(new DOMCSSDeclarationImpl(this));
  }
  return mDOMDeclaration.get();
}

NS_IMETHODIMP
StyleRule::GetCSSStyleRule(StyleRule **aResult)
{
  *aResult = this;
  NS_ADDREF(*aResult);
  return NS_OK;
}

StyleRule::StyleRule(nsCSSSelectorList* aSelector,
                     Declaration* aDeclaration,
                     uint32_t aLineNumber,
                     uint32_t aColumnNumber)
  : BindingStyleRule(aLineNumber, aColumnNumber),
    mSelector(aSelector),
    mDeclaration(aDeclaration)
{
  NS_PRECONDITION(aDeclaration, "must have a declaration");

  mDeclaration->SetOwningRule(this);
}

// for |Clone|
StyleRule::StyleRule(const StyleRule& aCopy)
  : BindingStyleRule(aCopy),
    mSelector(aCopy.mSelector ? aCopy.mSelector->Clone() : nullptr),
    mDeclaration(new Declaration(*aCopy.mDeclaration))
{
  mDeclaration->SetOwningRule(this);
  // rest is constructed lazily on existing data
}

StyleRule::~StyleRule()
{
  delete mSelector;
  DropReferences();
}

void
StyleRule::DropReferences()
{
  if (mDeclaration) {
    mDeclaration->SetOwningRule(nullptr);
  }
}

// QueryInterface implementation for StyleRule
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(StyleRule)
  if (aIID.Equals(NS_GET_IID(mozilla::css::StyleRule))) {
    *aInstancePtr = this;
    NS_ADDREF_THIS();
    return NS_OK;
  }
  else
  NS_INTERFACE_MAP_ENTRY(nsICSSStyleRuleDOMWrapper)
  NS_INTERFACE_MAP_ENTRY(nsIDOMCSSStyleRule)
NS_INTERFACE_MAP_END_INHERITING(Rule)

NS_IMPL_ADDREF_INHERITED(StyleRule, Rule)
NS_IMPL_RELEASE_INHERITED(StyleRule, Rule)

NS_IMPL_CYCLE_COLLECTION_CLASS(StyleRule)
NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(StyleRule, Rule)
  // Keep this in sync with IsCCLeaf.
  // Trace the wrapper for our declaration.  This just expands out
  // NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER which we can't use
  // directly because the wrapper is on the declaration, not on us.
  if (tmp->mDOMDeclaration) {
    tmp->mDOMDeclaration->TraceWrapper(aCallbacks, aClosure);
  }
NS_IMPL_CYCLE_COLLECTION_TRACE_END
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(StyleRule, Rule)
  // Unlink the wrapper for our declaraton.  This just expands out
  // NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER which we can't use
  // directly because the wrapper is on the declaration, not on us.
  if (tmp->mDOMDeclaration) {
    tmp->mDOMDeclaration->ReleaseWrapper(static_cast<nsISupports*>(p));
  }
  tmp->DropReferences();
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(StyleRule, Rule)
  // Keep this in sync with IsCCLeaf.
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

bool
StyleRule::IsCCLeaf() const
{
  if (!Rule::IsCCLeaf()) {
    return false;
  }

  return !mDOMDeclaration || !mDOMDeclaration->PreservingWrapper();
}

/* virtual */ int32_t
StyleRule::GetType() const
{
  return Rule::STYLE_RULE;
}

/* virtual */ already_AddRefed<Rule>
StyleRule::Clone() const
{
  RefPtr<Rule> clone = new StyleRule(*this);
  return clone.forget();
}

void
StyleRule::SetDeclaration(Declaration* aDecl)
{
  if (aDecl == mDeclaration) {
    return;
  }
  mDeclaration->SetOwningRule(nullptr);
  mDeclaration = aDecl;
  mDeclaration->SetOwningRule(this);
}

#ifdef DEBUG
/* virtual */ void
StyleRule::List(FILE* out, int32_t aIndent) const
{
  nsAutoCString str;
  // Indent
  for (int32_t index = aIndent; --index >= 0; ) {
    str.AppendLiteral("  ");
  }

  if (mSelector) {
    nsAutoString buffer;
    mSelector->ToString(buffer, GetStyleSheet());
    AppendUTF16toUTF8(buffer, str);
    str.Append(' ');
  }

  if (nullptr != mDeclaration) {
    nsAutoString buffer;
    str.AppendLiteral("{ ");
    mDeclaration->ToString(buffer);
    AppendUTF16toUTF8(buffer, str);
    str.Append('}');
    CSSStyleSheet* sheet = GetStyleSheet();
    if (sheet) {
      nsIURI* uri = sheet->GetSheetURI();
      if (uri) {
        str.Append(" /* ");
        str.Append(uri->GetSpecOrDefault());
        str.Append(':');
        str.AppendInt(mLineNumber);
        str.Append(" */");
      }
    }
  }
  else {
    str.AppendLiteral("{ null declaration }");
  }
  str.Append('\n');
  fprintf_stderr(out, "%s", str.get());
}
#endif

void
StyleRule::GetCssTextImpl(nsAString& aCssText) const
{
  if (mSelector) {
    mSelector->ToString(aCssText, GetStyleSheet());
    aCssText.Append(char16_t(' '));
  }
  aCssText.Append(char16_t('{'));
  aCssText.Append(char16_t(' '));
  if (mDeclaration)
  {
    nsAutoString   tempString;
    mDeclaration->ToString( tempString );
    aCssText.Append( tempString );
  }
  aCssText.Append(char16_t(' '));
  aCssText.Append(char16_t('}'));
}

NS_IMETHODIMP
StyleRule::GetSelectorText(nsAString& aSelectorText)
{
  if (mSelector)
    mSelector->ToString(aSelectorText, GetStyleSheet());
  else
    aSelectorText.Truncate();
  return NS_OK;
}

NS_IMETHODIMP
StyleRule::SetSelectorText(const nsAString& aSelectorText)
{
  CSSStyleSheet* sheet = GetStyleSheet();

  nsIDocument* doc = GetDocument();
  RefPtr<css::Loader> loader;

  if (doc) {
    loader = doc->CSSLoader();
  }

  // NOTE: Passing a null loader means that the parser is always in
  // standards mode and never in quirks mode.
  nsCSSParser parser(loader, sheet);

  // StyleRule lives inside of the Inner, it is unsafe to call WillDirty
  // if sheet does not already have a unique Inner.
  sheet->AssertHasUniqueInner();
  sheet->WillDirty();

  nsCSSSelectorList* selectorList = nullptr;

  nsresult result = parser.ParseSelectorString(
    aSelectorText, sheet->GetSheetURI(), 0, &selectorList);
  if (NS_FAILED(result)) {
    // Ignore parsing errors and continue to use the previous value.
    return NS_OK;
  }

  // Replace selector.
  delete mSelector;
  mSelector = selectorList;

  sheet->DidDirty();

  if (doc) {
    mozAutoDocUpdate updateBatch(doc, UPDATE_STYLE, true);
    doc->StyleRuleChanged(sheet, this);
  }
  return NS_OK;
}

/* virtual */ size_t
StyleRule::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  size_t n = aMallocSizeOf(this);
  n += mSelector ? mSelector->SizeOfIncludingThis(aMallocSizeOf) : 0;
  n += mDeclaration ? mDeclaration->SizeOfIncludingThis(aMallocSizeOf) : 0;

  // Measurement of the following members may be added later if DMD finds it is
  // worthwhile:
  // - mDOMRule;

  return n;
}

} // namespace css
} // namespace mozilla
