/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* atom list for CSS pseudo-classes */

#include "mozilla/ArrayUtils.h"

#include "nsCSSPseudoClasses.h"
#include "nsCSSPseudoElements.h"
#include "nsStaticAtom.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/Element.h"
#include "nsString.h"
#include "nsAttrValueInlines.h"
#include "nsIMozBrowserFrame.h"

using namespace mozilla;

// define storage for all atoms
#define CSS_PSEUDO_CLASS(_name, _value, _flags, _pref) \
  static nsIAtom* sPseudoClass_##_name;
#include "nsCSSPseudoClassList.h"
#undef CSS_PSEUDO_CLASS

#define CSS_PSEUDO_CLASS(name_, value_, flags_, pref_) \
  NS_STATIC_ATOM_BUFFER(name_##_pseudo_class_buffer, value_)
#include "nsCSSPseudoClassList.h"
#undef CSS_PSEUDO_CLASS

#define CSS_PSEUDO_CLASS(name_, value_, flags_, pref_) \
  static_assert(!((flags_) & CSS_PSEUDO_CLASS_ENABLED_IN_CHROME) || \
                ((flags_) & CSS_PSEUDO_CLASS_ENABLED_IN_UA_SHEETS), \
                "Pseudo-class '" #name_ "' is enabled in chrome, so it " \
                "should also be enabled in UA sheets");
#include "nsCSSPseudoClassList.h"
#undef CSS_PSEUDO_CLASS

// Array of nsStaticAtom for each of the pseudo-classes.
static const nsStaticAtom CSSPseudoClasses_info[] = {
#define CSS_PSEUDO_CLASS(name_, value_, flags_, pref_) \
  NS_STATIC_ATOM(name_##_pseudo_class_buffer, &sPseudoClass_##name_),
#include "nsCSSPseudoClassList.h"
#undef CSS_PSEUDO_CLASS
};

// Flags data for each of the pseudo-classes, which must be separate
// from the previous array since there's no place for it in
// nsStaticAtom.
/* static */ const uint32_t
nsCSSPseudoClasses::kPseudoClassFlags[] = {
#define CSS_PSEUDO_CLASS(name_, value_, flags_, pref_) \
  flags_,
#include "nsCSSPseudoClassList.h"
#undef CSS_PSEUDO_CLASS
};

/* static */ bool
nsCSSPseudoClasses::sPseudoClassEnabled[] = {
// If the pseudo class has any "ENABLED_IN" flag set, it is disabled by
// default. Note that, if a pseudo class has pref, whatever its default
// value is, it'll later be changed in nsCSSPseudoClasses::AddRefAtoms()
// If the pseudo class has "ENABLED_IN" flags but doesn't have a pref,
// it is an internal pseudo class which is disabled elsewhere.
#define IS_ENABLED_BY_DEFAULT(flags_) \
  (!((flags_) & CSS_PSEUDO_CLASS_ENABLED_MASK))
#define CSS_PSEUDO_CLASS(name_, value_, flags_, pref_) \
  IS_ENABLED_BY_DEFAULT(flags_),
#include "nsCSSPseudoClassList.h"
#undef CSS_PSEUDO_CLASS
#undef IS_ENABLED_BY_DEFAULT
};

// Arrays of the states that are relevant for various pseudoclasses.

/* static */ const mozilla::EventStates
nsCSSPseudoClasses::sPseudoClassStateDependences[] = {
#define CSS_PSEUDO_CLASS(_name, _value, _flags, _pref) EventStates(),
#define CSS_STATE_DEPENDENT_PSEUDO_CLASS(                                      \
  _name, _value, _flags, _pref, _states)                                       \
  _states,
#include "nsCSSPseudoClassList.h"
#undef CSS_STATE_DEPENDENT_PSEUDO_CLASS
#undef CSS_PSEUDO_CLASS
    // Add more entries for our fake values to make sure we can't
    // index out of bounds into this array no matter what.
    EventStates(),
    EventStates()
  };

/* static */ const mozilla::EventStates
 nsCSSPseudoClasses::sPseudoClassStates[] = {
#define CSS_PSEUDO_CLASS(_name, _value, _flags, _pref) EventStates(),
#define CSS_STATE_PSEUDO_CLASS(_name, _value, _flags, _pref, _states) _states,
#include "nsCSSPseudoClassList.h"
#undef CSS_STATE_PSEUDO_CLASS
#undef CSS_PSEUDO_CLASS
  // Add more entries for our fake values to make sure we can't
  // index out of bounds into this array no matter what.
  EventStates(),
  EventStates()
};
static_assert(MOZ_ARRAY_LENGTH(nsCSSPseudoClasses::sPseudoClassStates) ==
                static_cast<size_t>(CSSPseudoClassType::MAX),
              "CSSPseudoClassType::MAX is no longer equal to the length of "
              "nsCSSPseudoClasses::sPseudoClassStates");

void nsCSSPseudoClasses::AddRefAtoms()
{
  NS_RegisterStaticAtoms(CSSPseudoClasses_info);

#define CSS_PSEUDO_CLASS(name_, value_, flags_, pref_)                        \
  if (pref_[0]) {                                                             \
    auto idx = static_cast<CSSPseudoElementTypeBase>(Type::name_);            \
    Preferences::AddBoolVarCache(&sPseudoClassEnabled[idx], pref_);           \
  }
#include "nsCSSPseudoClassList.h"
#undef CSS_PSEUDO_CLASS
}

bool
nsCSSPseudoClasses::HasStringArg(Type aType)
{
  return aType == Type::lang ||
         aType == Type::mozEmptyExceptChildrenWithLocalname ||
         aType == Type::mozSystemMetric ||
         aType == Type::mozLocaleDir ||
         aType == Type::dir;
}

bool
nsCSSPseudoClasses::HasNthPairArg(Type aType)
{
  return aType == Type::nthChild ||
         aType == Type::nthLastChild ||
         aType == Type::nthOfType ||
         aType == Type::nthLastOfType;
}

bool
nsCSSPseudoClasses::HasSingleSelectorArg(Type aType)
{
  return aType == Type::host ||
         aType == Type::hostContext ||
         aType == Type::slotted;
}

bool
nsCSSPseudoClasses::HasForgivingSelectorListArg(Type aType)
{
  return aType == Type::is ||
         aType == Type::matches ||
         aType == Type::any ||
         aType == Type::where;
}

bool
nsCSSPseudoClasses::HasSelectorListArg(Type aType)
{
  return HasForgivingSelectorListArg(aType) ||
         aType == Type::negation ||
         aType == Type::mozAny ||
         aType == Type::mozAnyPrivate ||
         aType == Type::host ||
         aType == Type::hostContext ||
         aType == Type::slotted;
}

bool
nsCSSPseudoClasses::HasOptionalSelectorListArg(Type aType)
{
  return aType == Type::host;
}

bool
nsCSSPseudoClasses::IsHiddenFromSerialization(Type aType)
{
  return aType == Type::mozAnyPrivate;
}

void
nsCSSPseudoClasses::PseudoTypeToString(Type aType, nsAString& aString)
{
  MOZ_ASSERT(aType < Type::Count, "Unexpected type");
  auto idx = static_cast<CSSPseudoClassTypeBase>(aType);
  (*CSSPseudoClasses_info[idx].mAtom)->ToString(aString);
}

/* static */ CSSPseudoClassType
nsCSSPseudoClasses::GetPseudoType(nsIAtom* aAtom, EnabledState aEnabledState)
{
  for (uint32_t i = 0; i < ArrayLength(CSSPseudoClasses_info); ++i) {
    if (*CSSPseudoClasses_info[i].mAtom == aAtom) {
      Type type = Type(i);
      return IsEnabled(type, aEnabledState) ? type : Type::NotPseudo;
    }
  }
  return Type::NotPseudo;
}

/* static */ bool
nsCSSPseudoClasses::IsUserActionPseudoClass(Type aType)
{
  // See http://dev.w3.org/csswg/selectors4/#useraction-pseudos
  return aType == Type::hover ||
         aType == Type::active ||
         aType == Type::focus;
}

/* static */ bool
nsCSSPseudoClasses::IsHybridPseudoElement(Type aType)
{
  return aType == Type::slotted;
}

/* static */ Maybe<bool>
nsCSSPseudoClasses::MatchesElement(Type aType, const dom::Element* aElement)
{
  switch (aType) {
    case CSSPseudoClassType::mozNativeAnonymous:
      return Some(aElement->IsInNativeAnonymousSubtree());
    case CSSPseudoClassType::mozTableBorderNonzero: {
      if (!aElement->IsHTMLElement(nsGkAtoms::table)) {
        return Some(false);
      }
      const nsAttrValue *val = aElement->GetParsedAttr(nsGkAtoms::border);
      return Some(val && (val->Type() != nsAttrValue::eInteger ||
                          val->GetIntegerValue() != 0));
    }
    case CSSPseudoClassType::mozBrowserFrame: {
      nsCOMPtr<nsIMozBrowserFrame> browserFrame =
        do_QueryInterface(const_cast<Element*>(aElement));
      return Some(browserFrame && browserFrame->GetReallyIsBrowser());
    }
    default:
      return Nothing();
  }
}
