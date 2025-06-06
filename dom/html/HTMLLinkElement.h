/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_HTMLLinkElement_h
#define mozilla_dom_HTMLLinkElement_h

#include "mozilla/Attributes.h"
#include "mozilla/dom/Link.h"
#include "ImportManager.h"
#include "nsGenericHTMLElement.h"
#include "nsIDOMHTMLLinkElement.h"
#include "nsStyleLinkElement.h"

namespace mozilla {
class EventChainPostVisitor;
class EventChainPreVisitor;
namespace dom {

class HTMLLinkElement final : public nsGenericHTMLElement,
                              public nsIDOMHTMLLinkElement,
                              public nsStyleLinkElement,
                              public Link
{
public:
  explicit HTMLLinkElement(already_AddRefed<mozilla::dom::NodeInfo>& aNodeInfo);

  // nsISupports
  NS_DECL_ISUPPORTS_INHERITED

  // CC
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(HTMLLinkElement,
                                           nsGenericHTMLElement)

  // nsIDOMHTMLLinkElement
  NS_DECL_NSIDOMHTMLLINKELEMENT

  // DOM memory reporter participant
  NS_DECL_SIZEOF_EXCLUDING_THIS

  void LinkAdded();
  void LinkRemoved();

  void UpdateImport();

  // nsIDOMEventTarget
  virtual nsresult GetEventTargetParent(
                     EventChainPreVisitor& aVisitor) override;
  virtual nsresult PostHandleEvent(
                     EventChainPostVisitor& aVisitor) override;

  // nsINode
  virtual nsresult Clone(mozilla::dom::NodeInfo* aNodeInfo, nsINode** aResult) const override;
  virtual JSObject* WrapNode(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

  // nsIContent
  virtual nsresult BindToTree(nsIDocument* aDocument, nsIContent* aParent,
                              nsIContent* aBindingParent,
                              bool aCompileEventHandlers) override;
  virtual void UnbindFromTree(bool aDeep = true,
                              bool aNullParent = true) override;
  virtual nsresult BeforeSetAttr(int32_t aNameSpaceID, nsIAtom* aName,
                                 const nsAttrValueOrString* aValue,
                                 bool aNotify) override;
  virtual nsresult AfterSetAttr(int32_t aNameSpaceID, nsIAtom* aName,
                                const nsAttrValue* aValue,
                                const nsAttrValue* aOldValue,
                                nsIPrincipal* aSubjectPrincipal,
                                bool aNotify) override;
  virtual bool IsLink(nsIURI** aURI) const override;
  virtual already_AddRefed<nsIURI> GetHrefURI() const override;

  // Element
  virtual bool ParseAttribute(int32_t aNamespaceID,
                              nsIAtom* aAttribute,
                              const nsAString& aValue,
                              nsAttrValue& aResult) override;
  virtual void GetLinkTarget(nsAString& aTarget) override;
  virtual EventStates IntrinsicState() const override;

  void CreateAndDispatchEvent(nsIDocument* aDoc, const nsAString& aEventName);

  virtual void OnDNSPrefetchDeferred() override;
  virtual void OnDNSPrefetchRequested() override;
  virtual bool HasDeferredDNSPrefetchRequest() override;

  // WebIDL
  bool Disabled() const;
  void SetDisabled(bool aDisabled, ErrorResult& aRv);

  void GetHref(nsString& aValue, nsIPrincipal&)
  {
    GetHref(aValue);
  }
  void SetHref(const nsAString& aHref, nsIPrincipal& aTriggeringPrincipal, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::href, aHref, aTriggeringPrincipal, aRv);
  }
  void GetCrossOrigin(nsAString& aResult)
  {
    // Null for both missing and invalid defaults is ok, since we
    // always parse to an enum value, so we don't need an invalid
    // default, and we _want_ the missing default to be null.
    GetEnumAttr(nsGkAtoms::crossorigin, nullptr, aResult);
  }
  void SetCrossOrigin(const nsAString& aCrossOrigin, ErrorResult& aError)
  {
    SetOrRemoveNullableStringAttr(nsGkAtoms::crossorigin, aCrossOrigin, aError);
  }
  // XPCOM GetRel is fine.
  void SetRel(const nsAString& aRel, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::rel, aRel, aRv);
  }
  nsDOMTokenList* RelList();
  void GetNonce(nsAString& aNonce) const
  {
    GetHTMLAttr(nsGkAtoms::nonce, aNonce);
  }
  void SetNonce(const nsAString& aNonce, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::nonce, aNonce, aRv);
  }
  // XPCOM GetMedia is fine.
  void SetMedia(const nsAString& aMedia, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::media, aMedia, aRv);
  }
  // XPCOM GetHreflang is fine.
  void SetHreflang(const nsAString& aHreflang, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::hreflang, aHreflang, aRv);
  }
  void GetAs(nsAString& aResult);
  void SetAs(const nsAString& aAs, ErrorResult& aRv)
  {
    SetAttr(nsGkAtoms::as, aAs, aRv);
  }
  nsDOMTokenList* Sizes()
  {
    return GetTokenList(nsGkAtoms::sizes);
  }
  // XPCOM GetType is fine.
  void SetType(const nsAString& aType, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::type, aType, aRv);
  }
  // XPCOM GetCharset is fine.
  void SetCharset(const nsAString& aCharset, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::charset, aCharset, aRv);
  }
  // XPCOM GetRev is fine.
  void SetRev(const nsAString& aRev, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::rev, aRev, aRv);
  }
  // XPCOM GetTarget is fine.
  void SetTarget(const nsAString& aTarget, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::target, aTarget, aRv);
  }
  void GetIntegrity(nsAString& aIntegrity) const
  {
    GetHTMLAttr(nsGkAtoms::integrity, aIntegrity);
  }
  void SetIntegrity(const nsAString& aIntegrity, ErrorResult& aRv)
  {
    SetHTMLAttr(nsGkAtoms::integrity, aIntegrity, aRv);
  }
  void SetReferrerPolicy(const nsAString& aReferrer, ErrorResult& aError)
  {
    SetHTMLAttr(nsGkAtoms::referrerpolicy, aReferrer, aError);
  }
  void GetReferrerPolicy(nsAString& aReferrer)
  {
    GetEnumAttr(nsGkAtoms::referrerpolicy, EmptyCString().get(), aReferrer);
  }
  mozilla::net::ReferrerPolicy GetLinkReferrerPolicy() override
  {
    return GetReferrerPolicyAsEnum();
  }

  already_AddRefed<nsIDocument> GetImport();
  already_AddRefed<ImportLoader> GetImportLoader()
  {
    return RefPtr<ImportLoader>(mImportLoader).forget();
  }

  virtual CORSMode GetCORSMode() const override;
protected:
  virtual ~HTMLLinkElement();

  // nsStyleLinkElement
  virtual already_AddRefed<nsIURI> GetStyleSheetURL(bool* aIsInline, nsIPrincipal** aTriggeringPrincipal) override;
  virtual void GetStyleSheetInfo(nsAString& aTitle,
                                 nsAString& aType,
                                 nsAString& aMedia,
                                 bool* aIsScoped,
                                 bool* aIsAlternate,
                                 bool* aIsExplicitlyEnabled) override;

  RefPtr<nsDOMTokenList> mRelList;

  // The "explicitly enabled" flag. This flag is set whenever the 'disabled'
  // attribute is explicitly unset, and makes alternate stylesheets not be
  // disabled by default anymore.
  //
  // See https://github.com/whatwg/html/issues/3840#issuecomment-481034206.
  bool mExplicitlyEnabled = false;

private:
  RefPtr<ImportLoader> mImportLoader;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_HTMLLinkElement_h
