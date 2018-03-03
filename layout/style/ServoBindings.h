/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ServoBindings_h
#define mozilla_ServoBindings_h

#include <stdint.h>

#include "mozilla/ServoTypes.h"
#include "mozilla/ServoBindingTypes.h"
#include "mozilla/ServoElementSnapshot.h"
#include "mozilla/css/SheetParsingMode.h"
#include "nsChangeHint.h"
#include "nsCSSPseudoClasses.h"
#include "nsStyleStruct.h"

/*
 * API for Servo to access Goanna data structures. This file must compile as valid
 * C code in order for the binding generator to parse it.
 *
 * Functions beginning with Goanna_ are implemented in Goanna and invoked from Servo.
 * Functions beginning with Servo_ are implemented in Servo and invoked from Goanna.
 */

class nsIAtom;
class nsIPrincipal;
class nsIURI;
struct nsFont;
namespace mozilla {
  class ServoStyleSheet;
  class FontFamilyList;
  enum FontFamilyType : uint32_t;
}
using mozilla::FontFamilyList;
using mozilla::FontFamilyType;
using mozilla::ServoElementSnapshot;
struct nsMediaFeature;
struct nsStyleList;
struct nsStyleImage;
struct nsStyleGradientStop;
class nsStyleGradient;
class nsStyleCoord;
struct nsStyleDisplay;

#define NS_DECL_THREADSAFE_FFI_REFCOUNTING(class_, name_)                     \
  void Goanna_AddRef##name_##ArbitraryThread(class_* aPtr);                    \
  void Goanna_Release##name_##ArbitraryThread(class_* aPtr);
#define NS_IMPL_THREADSAFE_FFI_REFCOUNTING(class_, name_)                     \
  static_assert(class_::HasThreadSafeRefCnt::value,                           \
                "NS_DECL_THREADSAFE_FFI_REFCOUNTING can only be used with "   \
                "classes that have thread-safe refcounting");                 \
  void Goanna_AddRef##name_##ArbitraryThread(class_* aPtr)                     \
  { NS_ADDREF(aPtr); }                                                        \
  void Goanna_Release##name_##ArbitraryThread(class_* aPtr)                    \
  { NS_RELEASE(aPtr); }

#define NS_DECL_HOLDER_FFI_REFCOUNTING(class_, name_)                         \
  typedef nsMainThreadPtrHolder<class_> ThreadSafe##name_##Holder;            \
  void Goanna_AddRef##name_##ArbitraryThread(ThreadSafe##name_##Holder* aPtr); \
  void Goanna_Release##name_##ArbitraryThread(ThreadSafe##name_##Holder* aPtr);
#define NS_IMPL_HOLDER_FFI_REFCOUNTING(class_, name_)                         \
  void Goanna_AddRef##name_##ArbitraryThread(ThreadSafe##name_##Holder* aPtr)  \
  { NS_ADDREF(aPtr); }                                                        \
  void Goanna_Release##name_##ArbitraryThread(ThreadSafe##name_##Holder* aPtr) \
  { NS_RELEASE(aPtr); }                                                       \


#define DEFINE_ARRAY_TYPE_FOR(type_)                                \
  struct nsTArrayBorrowed_##type_ {                                 \
    nsTArray<type_>* mArray;                                        \
    MOZ_IMPLICIT nsTArrayBorrowed_##type_(nsTArray<type_>* aArray)  \
      : mArray(aArray) {}                                           \
  }
DEFINE_ARRAY_TYPE_FOR(uintptr_t);
#undef DEFINE_ARRAY_TYPE_FOR

extern "C" {

// Object refcounting.
NS_DECL_HOLDER_FFI_REFCOUNTING(nsIPrincipal, Principal)
NS_DECL_HOLDER_FFI_REFCOUNTING(nsIURI, URI)

// DOM Traversal.
uint32_t Goanna_ChildrenCount(RawGoannaNodeBorrowed node);
bool Goanna_NodeIsElement(RawGoannaNodeBorrowed node);
bool Goanna_IsInDocument(RawGoannaNodeBorrowed node);
RawGoannaNodeBorrowedOrNull Goanna_GetParentNode(RawGoannaNodeBorrowed node);
RawGoannaNodeBorrowedOrNull Goanna_GetFirstChild(RawGoannaNodeBorrowed node);
RawGoannaNodeBorrowedOrNull Goanna_GetLastChild(RawGoannaNodeBorrowed node);
RawGoannaNodeBorrowedOrNull Goanna_GetPrevSibling(RawGoannaNodeBorrowed node);
RawGoannaNodeBorrowedOrNull Goanna_GetNextSibling(RawGoannaNodeBorrowed node);
RawGoannaElementBorrowedOrNull Goanna_GetParentElement(RawGoannaElementBorrowed element);
RawGoannaElementBorrowedOrNull Goanna_GetFirstChildElement(RawGoannaElementBorrowed element);
RawGoannaElementBorrowedOrNull Goanna_GetLastChildElement(RawGoannaElementBorrowed element);
RawGoannaElementBorrowedOrNull Goanna_GetPrevSiblingElement(RawGoannaElementBorrowed element);
RawGoannaElementBorrowedOrNull Goanna_GetNextSiblingElement(RawGoannaElementBorrowed element);
RawGoannaElementBorrowedOrNull Goanna_GetDocumentElement(RawGoannaDocumentBorrowed document);
void Goanna_LoadStyleSheet(mozilla::css::Loader* loader,
                          mozilla::ServoStyleSheet* parent,
                          RawServoImportRuleBorrowed import_rule,
                          const uint8_t* url_bytes,
                          uint32_t url_length,
                          const uint8_t* media_bytes,
                          uint32_t media_length);

// By default, Servo walks the DOM by traversing the siblings of the DOM-view
// first child. This generally works, but misses anonymous children, which we
// want to traverse during styling. To support these cases, we create an
// optional heap-allocated iterator for nodes that need it. If the creation
// method returns null, Servo falls back to the aforementioned simpler (and
// faster) sibling traversal.
StyleChildrenIteratorOwnedOrNull Goanna_MaybeCreateStyleChildrenIterator(RawGoannaNodeBorrowed node);
void Goanna_DropStyleChildrenIterator(StyleChildrenIteratorOwned it);
RawGoannaNodeBorrowedOrNull Goanna_GetNextStyleChild(StyleChildrenIteratorBorrowedMut it);

// Selector Matching.
uint16_t Goanna_ElementState(RawGoannaElementBorrowed element);
bool Goanna_IsHTMLElementInHTMLDocument(RawGoannaElementBorrowed element);
bool Goanna_IsLink(RawGoannaElementBorrowed element);
bool Goanna_IsTextNode(RawGoannaNodeBorrowed node);
bool Goanna_IsVisitedLink(RawGoannaElementBorrowed element);
bool Goanna_IsUnvisitedLink(RawGoannaElementBorrowed element);
bool Goanna_IsRootElement(RawGoannaElementBorrowed element);
bool Goanna_MatchesElement(mozilla::CSSPseudoClassType type, RawGoannaElementBorrowed element);
nsIAtom* Goanna_LocalName(RawGoannaElementBorrowed element);
nsIAtom* Goanna_Namespace(RawGoannaElementBorrowed element);
nsIAtom* Goanna_GetElementId(RawGoannaElementBorrowed element);

// Attributes.
#define SERVO_DECLARE_ELEMENT_ATTR_MATCHING_FUNCTIONS(prefix_, implementor_)  \
  nsIAtom* prefix_##AtomAttrValue(implementor_ element, nsIAtom* attribute);  \
  bool prefix_##HasAttr(implementor_ element, nsIAtom* ns, nsIAtom* name);    \
  bool prefix_##AttrEquals(implementor_ element, nsIAtom* ns, nsIAtom* name,  \
                           nsIAtom* str, bool ignoreCase);                    \
  bool prefix_##AttrDashEquals(implementor_ element, nsIAtom* ns,             \
                               nsIAtom* name, nsIAtom* str);                  \
  bool prefix_##AttrIncludes(implementor_ element, nsIAtom* ns,               \
                             nsIAtom* name, nsIAtom* str);                    \
  bool prefix_##AttrHasSubstring(implementor_ element, nsIAtom* ns,           \
                                 nsIAtom* name, nsIAtom* str);                \
  bool prefix_##AttrHasPrefix(implementor_ element, nsIAtom* ns,              \
                              nsIAtom* name, nsIAtom* str);                   \
  bool prefix_##AttrHasSuffix(implementor_ element, nsIAtom* ns,              \
                              nsIAtom* name, nsIAtom* str);                   \
  uint32_t prefix_##ClassOrClassList(implementor_ element, nsIAtom** class_,  \
                                     nsIAtom*** classList);

SERVO_DECLARE_ELEMENT_ATTR_MATCHING_FUNCTIONS(Goanna_, RawGoannaElementBorrowed)
SERVO_DECLARE_ELEMENT_ATTR_MATCHING_FUNCTIONS(Goanna_Snapshot,
                                              const ServoElementSnapshot*)

#undef SERVO_DECLARE_ELEMENT_ATTR_MATCHING_FUNCTIONS

// Style attributes.
RawServoDeclarationBlockStrongBorrowedOrNull
Goanna_GetServoDeclarationBlock(RawGoannaElementBorrowed element);

// Atoms.
nsIAtom* Goanna_Atomize(const char* aString, uint32_t aLength);
void Goanna_AddRefAtom(nsIAtom* aAtom);
void Goanna_ReleaseAtom(nsIAtom* aAtom);
const uint16_t* Goanna_GetAtomAsUTF16(nsIAtom* aAtom, uint32_t* aLength);
bool Goanna_AtomEqualsUTF8(nsIAtom* aAtom, const char* aString, uint32_t aLength);
bool Goanna_AtomEqualsUTF8IgnoreCase(nsIAtom* aAtom, const char* aString, uint32_t aLength);

// Font style
void Goanna_FontFamilyList_Clear(FontFamilyList* aList);
void Goanna_FontFamilyList_AppendNamed(FontFamilyList* aList, nsIAtom* aName);
void Goanna_FontFamilyList_AppendGeneric(FontFamilyList* list, FontFamilyType familyType);
void Goanna_CopyFontFamilyFrom(nsFont* dst, const nsFont* src);

// Counter style.
void Goanna_SetListStyleType(nsStyleList* style_struct, uint32_t type);
void Goanna_CopyListStyleTypeFrom(nsStyleList* dst, const nsStyleList* src);

// background-image style.
// TODO: support element() and -moz-image()
void Goanna_SetNullImageValue(nsStyleImage* image);
void Goanna_SetGradientImageValue(nsStyleImage* image, nsStyleGradient* gradient);
void Goanna_SetUrlImageValue(nsStyleImage* image,
                            const uint8_t* url_bytes,
                            uint32_t url_length,
                            ThreadSafeURIHolder* base_uri,
                            ThreadSafeURIHolder* referrer,
                            ThreadSafePrincipalHolder* principal);
void Goanna_CopyImageValueFrom(nsStyleImage* image, const nsStyleImage* other);

nsStyleGradient* Goanna_CreateGradient(uint8_t shape,
                                      uint8_t size,
                                      bool repeating,
                                      bool legacy_syntax,
                                      uint32_t stops);

// list-style-image style.
void Goanna_SetListStyleImageNone(nsStyleList* style_struct);
void Goanna_SetListStyleImage(nsStyleList* style_struct,
                             const uint8_t* string_bytes, uint32_t string_length,
                             ThreadSafeURIHolder* base_uri,
                             ThreadSafeURIHolder* referrer,
                             ThreadSafePrincipalHolder* principal);
void Goanna_CopyListStyleImageFrom(nsStyleList* dest, const nsStyleList* src);

// cursor style.
void Goanna_SetCursorArrayLength(nsStyleUserInterface* ui, size_t len);
void Goanna_SetCursorImage(nsCursorImage* cursor,
                          const uint8_t* string_bytes, uint32_t string_length,
                          ThreadSafeURIHolder* base_uri,
                          ThreadSafeURIHolder* referrer,
                          ThreadSafePrincipalHolder* principal);
void Goanna_CopyCursorArrayFrom(nsStyleUserInterface* dest,
                               const nsStyleUserInterface* src);

// Display style.
void Goanna_SetMozBinding(nsStyleDisplay* style_struct,
                         const uint8_t* string_bytes, uint32_t string_length,
                         ThreadSafeURIHolder* base_uri,
                         ThreadSafeURIHolder* referrer,
                         ThreadSafePrincipalHolder* principal);
void Goanna_CopyMozBindingFrom(nsStyleDisplay* des, const nsStyleDisplay* src);

// Dirtiness tracking.
uint32_t Goanna_GetNodeFlags(RawGoannaNodeBorrowed node);
void Goanna_SetNodeFlags(RawGoannaNodeBorrowed node, uint32_t flags);
void Goanna_UnsetNodeFlags(RawGoannaNodeBorrowed node, uint32_t flags);

// Incremental restyle.
// Also, we might want a ComputedValues to ComputedValues API for animations?
// Not if we do them in Goanna...
nsStyleContext* Goanna_GetStyleContext(RawGoannaNodeBorrowed node,
                                      nsIAtom* aPseudoTagOrNull);
nsChangeHint Goanna_CalcStyleDifference(nsStyleContext* oldstyle,
                                       ServoComputedValuesBorrowed newstyle);

// Element snapshot.
ServoElementSnapshotOwned Goanna_CreateElementSnapshot(RawGoannaElementBorrowed element);
void Goanna_DropElementSnapshot(ServoElementSnapshotOwned snapshot);

// `array` must be an nsTArray
// If changing this signature, please update the
// friend function declaration in nsTArray.h
void Goanna_EnsureTArrayCapacity(void* array, size_t capacity, size_t elem_size);

// Same here, `array` must be an nsTArray<T>, for some T.
//
// Important note: Only valid for POD types, since destructors won't be run
// otherwise. This is ensured with rust traits for the relevant structs.
void Goanna_ClearPODTArray(void* array, size_t elem_size, size_t elem_align);

// Clear the mContents field in nsStyleContent. This is needed to run the
// destructors, otherwise we'd leak the images (though we still don't support
// those), strings, and whatnot.
void Goanna_ClearStyleContents(nsStyleContent* content);
void Goanna_CopyStyleContentsFrom(nsStyleContent* content, const nsStyleContent* other);

void Goanna_EnsureImageLayersLength(nsStyleImageLayers* layers, size_t len,
                                   nsStyleImageLayers::LayerType layer_type);

void Goanna_EnsureStyleAnimationArrayLength(void* array, size_t len);

// Clean up pointer-based coordinates
void Goanna_ResetStyleCoord(nsStyleUnit* unit, nsStyleUnion* value);

// Set an nsStyleCoord to a computed `calc()` value
void Goanna_SetStyleCoordCalcValue(nsStyleUnit* unit, nsStyleUnion* value, nsStyleCoord::CalcValue calc);

void Goanna_CopyClipPathValueFrom(mozilla::StyleClipPath* dst, const mozilla::StyleClipPath* src);

void Goanna_DestroyClipPath(mozilla::StyleClipPath* clip);
mozilla::StyleBasicShape* Goanna_NewBasicShape(mozilla::StyleBasicShapeType type);

void Goanna_ResetFilters(nsStyleEffects* effects, size_t new_len);
void Goanna_CopyFiltersFrom(nsStyleEffects* aSrc, nsStyleEffects* aDest);

void Goanna_FillAllBackgroundLists(nsStyleImageLayers* layers, uint32_t max_len);
void Goanna_FillAllMaskLists(nsStyleImageLayers* layers, uint32_t max_len);
NS_DECL_THREADSAFE_FFI_REFCOUNTING(nsStyleCoord::Calc, Calc);

nsCSSShadowArray* Goanna_NewCSSShadowArray(uint32_t len);
NS_DECL_THREADSAFE_FFI_REFCOUNTING(nsCSSShadowArray, CSSShadowArray);

nsStyleQuoteValues* Goanna_NewStyleQuoteValues(uint32_t len);
NS_DECL_THREADSAFE_FFI_REFCOUNTING(nsStyleQuoteValues, QuoteValues);

nsCSSValueSharedList* Goanna_NewCSSValueSharedList(uint32_t len);
void Goanna_CSSValue_SetAbsoluteLength(nsCSSValueBorrowedMut css_value, nscoord len);
void Goanna_CSSValue_SetNumber(nsCSSValueBorrowedMut css_value, float number);
void Goanna_CSSValue_SetKeyword(nsCSSValueBorrowedMut css_value, nsCSSKeyword keyword);
void Goanna_CSSValue_SetPercentage(nsCSSValueBorrowedMut css_value, float percent);
void Goanna_CSSValue_SetAngle(nsCSSValueBorrowedMut css_value, float radians);
void Goanna_CSSValue_SetCalc(nsCSSValueBorrowedMut css_value, nsStyleCoord::CalcValue calc);
void Goanna_CSSValue_SetFunction(nsCSSValueBorrowedMut css_value, int32_t len);
nsCSSValueBorrowedMut Goanna_CSSValue_GetArrayItem(nsCSSValueBorrowedMut css_value, int32_t index);
void Goanna_CSSValue_Drop(nsCSSValueBorrowedMut css_value);
NS_DECL_THREADSAFE_FFI_REFCOUNTING(nsCSSValueSharedList, CSSValueSharedList);
bool Goanna_PropertyId_IsPrefEnabled(nsCSSPropertyID id);

const nsMediaFeature* Goanna_GetMediaFeatures();

// Style-struct management.
#define STYLE_STRUCT(name, checkdata_cb)                                       \
  void Goanna_Construct_Default_nsStyle##name(                                  \
    nsStyle##name* ptr,                                                        \
    RawGoannaPresContextBorrowed pres_context);                                 \
  void Goanna_CopyConstruct_nsStyle##name(nsStyle##name* ptr,                   \
                                         const nsStyle##name* other);          \
  void Goanna_Destroy_nsStyle##name(nsStyle##name* ptr);
#include "nsStyleStructList.h"
#undef STYLE_STRUCT

void Goanna_Construct_nsStyleVariables(nsStyleVariables* ptr);

#define SERVO_BINDING_FUNC(name_, return_, ...) return_ name_(__VA_ARGS__);
#include "mozilla/ServoBindingList.h"
#undef SERVO_BINDING_FUNC

} // extern "C"

#endif // mozilla_ServoBindings_h
