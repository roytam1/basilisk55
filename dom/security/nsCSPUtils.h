/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsCSPUtils_h___
#define nsCSPUtils_h___

#include "nsCOMPtr.h"
#include "nsIContentPolicy.h"
#include "nsIContentSecurityPolicy.h"
#include "nsIURI.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsUnicharUtils.h"
#include "mozilla/dom/CSPDictionariesBinding.h"
#include "mozilla/Logging.h"

// ======= Defines and helper functions =======

static const char16_t COLON        = ':';
static const char16_t SEMICOLON    = ';';
static const char16_t SLASH        = '/';
static const char16_t PLUS         = '+';
static const char16_t DASH         = '-';
static const char16_t DOT          = '.';
static const char16_t UNDERLINE    = '_';
static const char16_t TILDE        = '~';
static const char16_t WILDCARD     = '*';
static const char16_t SINGLEQUOTE  = '\'';
static const char16_t NUMBER_SIGN  = '#';
static const char16_t QUESTIONMARK = '?';
static const char16_t PERCENT_SIGN = '%';
static const char16_t EXCLAMATION  = '!';
static const char16_t DOLLAR       = '$';
static const char16_t AMPERSAND    = '&';
static const char16_t OPENBRACE    = '(';
static const char16_t CLOSINGBRACE = ')';
static const char16_t EQUALS       = '=';
static const char16_t ATSYMBOL     = '@';

static bool
isCharacterToken(char16_t aSymbol)
{
  return (aSymbol >= 'a' && aSymbol <= 'z') ||
         (aSymbol >= 'A' && aSymbol <= 'Z');
}

static bool
isNumberToken(char16_t aSymbol)
{
  return (aSymbol >= '0' && aSymbol <= '9');
}

static bool
isValidHexDig(char16_t aHexDig)
{
  return (isNumberToken(aHexDig) ||
          (aHexDig >= 'A' && aHexDig <= 'F') ||
          (aHexDig >= 'a' && aHexDig <= 'f'));
}

static bool
isValidBase64Value(const char16_t* cur, const char16_t* end)
{
  // Using grammar at https://w3c.github.io/webappsec-csp/#grammardef-nonce-source

  // May end with one or two =
  if (end > cur && *(end-1) == EQUALS) end--;
  if (end > cur && *(end-1) == EQUALS) end--;

  // Must have at least one character aside from any =
  if (end == cur) {
    return false;
  }

  // Rest must all be A-Za-z0-9+/-_
  for (; cur < end; ++cur) {
    if (!(isCharacterToken(*cur) || isNumberToken(*cur) ||
          *cur == PLUS || *cur == SLASH ||
          *cur == DASH || *cur == UNDERLINE)) {
      return false;
    }
  }

  return true;
}

namespace mozilla {
namespace dom {
  struct CSP;
} // namespace dom
} // namespace mozilla

/* =============== Logging =================== */

void CSP_LogLocalizedStr(const char16_t* aName,
                         const char16_t** aParams,
                         uint32_t aLength,
                         const nsAString& aSourceName,
                         const nsAString& aSourceLine,
                         uint32_t aLineNumber,
                         uint32_t aColumnNumber,
                         uint32_t aFlags,
                         const char* aCategory,
                         uint64_t aInnerWindowID);

void CSP_GetLocalizedStr(const char16_t* aName,
                         const char16_t** aParams,
                         uint32_t aLength,
                         char16_t** outResult);

void CSP_LogStrMessage(const nsAString& aMsg);

void CSP_LogMessage(const nsAString& aMessage,
                    const nsAString& aSourceName,
                    const nsAString& aSourceLine,
                    uint32_t aLineNumber,
                    uint32_t aColumnNumber,
                    uint32_t aFlags,
                    const char* aCategory,
                    uint64_t aInnerWindowID);


/* =============== Constant and Type Definitions ================== */

#define INLINE_STYLE_VIOLATION_OBSERVER_TOPIC        "violated base restriction: Inline Stylesheets will not apply"
#define INLINE_SCRIPT_VIOLATION_OBSERVER_TOPIC       "violated base restriction: Inline Scripts will not execute"
#define EVAL_VIOLATION_OBSERVER_TOPIC                "violated base restriction: Code will not be created from strings"
#define SCRIPT_NONCE_VIOLATION_OBSERVER_TOPIC        "Inline Script had invalid nonce"
#define STYLE_NONCE_VIOLATION_OBSERVER_TOPIC         "Inline Style had invalid nonce"
#define SCRIPT_HASH_VIOLATION_OBSERVER_TOPIC         "Inline Script had invalid hash"
#define STYLE_HASH_VIOLATION_OBSERVER_TOPIC          "Inline Style had invalid hash"
#define REQUIRE_SRI_SCRIPT_VIOLATION_OBSERVER_TOPIC  "Missing required Subresource Integrity for Script"
#define REQUIRE_SRI_STYLE_VIOLATION_OBSERVER_TOPIC   "Missing required Subresource Integrity for Style"

// these strings map to the CSPDirectives in nsIContentSecurityPolicy
// NOTE: When implementing a new directive, you will need to add it here but also
// add a corresponding entry to the constants in nsIContentSecurityPolicy.idl
// and also create an entry for the new directive in
// nsCSPDirective::toDomCSPStruct() and add it to CSPDictionaries.webidl.
// Order of elements below important! Make sure it matches the order as in
// nsIContentSecurityPolicy.idl
static const char* CSPStrDirectives[] = {
  "-error-",                   // NO_DIRECTIVE
  "default-src",               // DEFAULT_SRC_DIRECTIVE
  "script-src",                // SCRIPT_SRC_DIRECTIVE
  "object-src",                // OBJECT_SRC_DIRECTIVE
  "style-src",                 // STYLE_SRC_DIRECTIVE
  "img-src",                   // IMG_SRC_DIRECTIVE
  "media-src",                 // MEDIA_SRC_DIRECTIVE
  "frame-src",                 // FRAME_SRC_DIRECTIVE
  "font-src",                  // FONT_SRC_DIRECTIVE
  "connect-src",               // CONNECT_SRC_DIRECTIVE
  "report-uri",                // REPORT_URI_DIRECTIVE
  "frame-ancestors",           // FRAME_ANCESTORS_DIRECTIVE
  "reflected-xss",             // REFLECTED_XSS_DIRECTIVE
  "base-uri",                  // BASE_URI_DIRECTIVE
  "form-action",               // FORM_ACTION_DIRECTIVE
  "manifest-src",              // MANIFEST_SRC_DIRECTIVE
  "upgrade-insecure-requests", // UPGRADE_IF_INSECURE_DIRECTIVE
  "child-src",                 // CHILD_SRC_DIRECTIVE
  "block-all-mixed-content",   // BLOCK_ALL_MIXED_CONTENT
  "require-sri-for",           // REQUIRE_SRI_FOR
  "sandbox",                   // SANDBOX_DIRECTIVE
  "worker-src",                // WORKER_SRC_DIRECTIVE
  "script-src-elem",           // SCRIPT_SRC_ELEM_DIRECTIVE
  "script-src-attr",           // SCRIPT_SRC_ATTR_DIRECTIVE
  "style-src-elem",            // STYLE_SRC_ELEM_DIRECTIVE
  "style-src-attr"             // STYLE_SRC_ATTR_DIRECTIVE
};

inline const char* CSP_CSPDirectiveToString(CSPDirective aDir)
{
  return CSPStrDirectives[static_cast<uint32_t>(aDir)];
}

inline CSPDirective CSP_StringToCSPDirective(const nsAString& aDir)
{
  nsString lowerDir = PromiseFlatString(aDir);
  ToLowerCase(lowerDir);

  uint32_t numDirs = (sizeof(CSPStrDirectives) / sizeof(CSPStrDirectives[0]));
  for (uint32_t i = 1; i < numDirs; i++) {
    if (lowerDir.EqualsASCII(CSPStrDirectives[i])) {
      return static_cast<CSPDirective>(i);
    }
  }
  NS_ASSERTION(false, "Can not convert unknown Directive to Integer");
  return nsIContentSecurityPolicy::NO_DIRECTIVE;
}

// Please add any new enum items not only to CSPKeyword, but also add
// a string version for every enum >> using the same index << to
// CSPStrKeywords underneath.
enum CSPKeyword {
  CSP_SELF = 0,
  CSP_UNSAFE_INLINE,
  CSP_UNSAFE_EVAL,
  CSP_NONE,
  CSP_NONCE,
  CSP_REQUIRE_SRI_FOR,
  CSP_STRICT_DYNAMIC,
  // CSP_LAST_KEYWORD_VALUE always needs to be the last element in the enum
  // because we use it to calculate the size for the char* array.
  CSP_LAST_KEYWORD_VALUE,
  // Putting CSP_HASH after the delimitor, because CSP_HASH is not a valid
  // keyword (hash uses e.g. sha256, sha512) but we use CSP_HASH internally
  // to identify allowed hashes in ::allows.
  CSP_HASH
 };

static const char* CSPStrKeywords[] = {
  "'self'",          // CSP_SELF = 0
  "'unsafe-inline'", // CSP_UNSAFE_INLINE
  "'unsafe-eval'",   // CSP_UNSAFE_EVAL
  "'none'",          // CSP_NONE
  "'nonce-",         // CSP_NONCE
  "require-sri-for", // CSP_REQUIRE_SRI_FOR
  "'strict-dynamic'" // CSP_STRICT_DYNAMIC
  // Remember: CSP_HASH is not supposed to be used
};

inline const char* CSP_EnumToKeyword(enum CSPKeyword aKey)
{
  // Make sure all elements in enum CSPKeyword got added to CSPStrKeywords.
  static_assert((sizeof(CSPStrKeywords) / sizeof(CSPStrKeywords[0]) ==
                static_cast<uint32_t>(CSP_LAST_KEYWORD_VALUE)),
                "CSP_LAST_KEYWORD_VALUE does not match length of CSPStrKeywords");

  if (static_cast<uint32_t>(aKey) < static_cast<uint32_t>(CSP_LAST_KEYWORD_VALUE)) {
      return CSPStrKeywords[static_cast<uint32_t>(aKey)];
  }
  return "error: invalid keyword in CSP_EnumToKeyword";
}

inline CSPKeyword CSP_KeywordToEnum(const nsAString& aKey)
{
  nsString lowerKey = PromiseFlatString(aKey);
  ToLowerCase(lowerKey);

  static_assert(CSP_LAST_KEYWORD_VALUE ==
                (sizeof(CSPStrKeywords) / sizeof(CSPStrKeywords[0])),
                 "CSP_LAST_KEYWORD_VALUE does not match length of CSPStrKeywords");

  for (uint32_t i = 0; i < CSP_LAST_KEYWORD_VALUE; i++) {
    if (lowerKey.EqualsASCII(CSPStrKeywords[i])) {
      return static_cast<CSPKeyword>(i);
    }
  }
  NS_ASSERTION(false, "Can not convert unknown Keyword to Enum");
  return CSP_LAST_KEYWORD_VALUE;
}

nsresult CSP_AppendCSPFromHeader(nsIContentSecurityPolicy* aCsp,
                                 const nsAString& aHeaderValue,
                                 bool aReportOnly);

/* =============== Helpers ================== */

class nsCSPHostSrc;

nsCSPHostSrc* CSP_CreateHostSrcFromURI(nsIURI* aSelfURI, bool aIsSelf = false);
bool CSP_IsEmptyDirective(const nsAString& aValue, const nsAString& aDir);
bool CSP_IsValidDirective(const nsAString& aDir);
bool CSP_IsDirective(const nsAString& aValue, CSPDirective aDir);
bool CSP_IsKeyword(const nsAString& aValue, enum CSPKeyword aKey);
bool CSP_IsQuotelessKeyword(const nsAString& aKey);
CSPDirective CSP_ContentTypeToDirective(nsContentPolicyType aType);

class nsCSPSrcVisitor;

void CSP_PercentDecodeStr(const nsAString& aEncStr, nsAString& outDecStr);

/* =============== nsCSPSrc ================== */

class nsCSPBaseSrc {
  public:
    nsCSPBaseSrc();
    virtual ~nsCSPBaseSrc();

    virtual bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                         bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const;
    virtual bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                        bool aParserCreated) const;
    virtual bool visit(nsCSPSrcVisitor* aVisitor) const = 0;
    virtual void toString(nsAString& outStr) const = 0;

    virtual void invalidate() const
      { mInvalidated = true; }

/* TenFourFox issue 602 */
    bool getCameFromSelf() const;
    void setCameFromSelf(bool isSelf);
 private:
    bool mCameFromSelf;

  protected:
    // invalidate srcs if 'script-dynamic' is present or also invalidate
    // unsafe-inline' if nonce- or hash-source specified
    mutable bool mInvalidated;

};

/* =============== nsCSPSchemeSrc ============ */

class nsCSPSchemeSrc : public nsCSPBaseSrc {
  public:
    explicit nsCSPSchemeSrc(const nsAString& aScheme);
    virtual ~nsCSPSchemeSrc();

    bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                 bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const;
    bool visit(nsCSPSrcVisitor* aVisitor) const;
    void toString(nsAString& outStr) const;

    inline void getScheme(nsAString& outStr) const
      { outStr.Assign(mScheme); };

  private:
    nsString mScheme;
};

/* =============== nsCSPHostSrc ============== */

class nsCSPHostSrc : public nsCSPBaseSrc {
  public:
    explicit nsCSPHostSrc(const nsAString& aHost);
    virtual ~nsCSPHostSrc();

    bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                 bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const;
    bool visit(nsCSPSrcVisitor* aVisitor) const;
    void toString(nsAString& outStr) const;

/* TenFourFox issue 602 */
    bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce) const;

    void setScheme(const nsAString& aScheme);
    void setPort(const nsAString& aPort);
    void appendPath(const nsAString &aPath);

    inline void setGeneratedFromSelfKeyword() const
      { mGeneratedFromSelfKeyword = true;}

    inline void setWithinFrameAncestorsDir(bool aValue) const
      { mWithinFrameAncstorsDir = aValue; }

    inline void getScheme(nsAString& outStr) const
      { outStr.Assign(mScheme); };

    inline void getHost(nsAString& outStr) const
      { outStr.Assign(mHost); };

    inline void getPort(nsAString& outStr) const
      { outStr.Assign(mPort); };

    inline void getPath(nsAString& outStr) const
      { outStr.Assign(mPath); };

  private:
    nsString mScheme;
    nsString mHost;
    nsString mPort;
    nsString mPath;
    mutable bool mGeneratedFromSelfKeyword;
    mutable bool mWithinFrameAncstorsDir;
};

/* =============== nsCSPKeywordSrc ============ */

class nsCSPKeywordSrc : public nsCSPBaseSrc {
  public:
    explicit nsCSPKeywordSrc(CSPKeyword aKeyword);
    virtual ~nsCSPKeywordSrc();

    bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                bool aParserCreated) const;
    bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                 bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const;
    bool visit(nsCSPSrcVisitor* aVisitor) const;
    void toString(nsAString& outStr) const;

    inline CSPKeyword getKeyword() const
      { return mKeyword; };

    inline void invalidate() const
    {
      // keywords that need to invalidated
      if (mKeyword == CSP_SELF || mKeyword == CSP_UNSAFE_INLINE) {
        mInvalidated = true;
      }
    }

  private:
    CSPKeyword mKeyword;
};

/* =============== nsCSPNonceSource =========== */

class nsCSPNonceSrc : public nsCSPBaseSrc {
  public:
    explicit nsCSPNonceSrc(const nsAString& aNonce);
    virtual ~nsCSPNonceSrc();

    bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                 bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const;
    bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                bool aParserCreated) const;
    bool visit(nsCSPSrcVisitor* aVisitor) const;
    void toString(nsAString& outStr) const;

    inline void getNonce(nsAString& outStr) const
      { outStr.Assign(mNonce); };

    inline void invalidate() const
    {
      // overwrite nsCSPBaseSRC::invalidate() and explicitily
      // do *not* invalidate, because 'strict-dynamic' should
      // not invalidate nonces.
    }

  private:
    nsString mNonce;
};

/* =============== nsCSPHashSource ============ */

class nsCSPHashSrc : public nsCSPBaseSrc {
  public:
    nsCSPHashSrc(const nsAString& algo, const nsAString& hash);
    virtual ~nsCSPHashSrc();

    bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                bool aParserCreated) const;
    void toString(nsAString& outStr) const;
    bool visit(nsCSPSrcVisitor* aVisitor) const;

    inline void getAlgorithm(nsAString& outStr) const
      { outStr.Assign(mAlgorithm); };

    inline void getHash(nsAString& outStr) const
      { outStr.Assign(mHash); };

    inline void invalidate() const
    {
      // overwrite nsCSPBaseSRC::invalidate() and explicitily
      // do *not* invalidate, because 'strict-dynamic' should
      // not invalidate hashes.
    }

  private:
    nsString mAlgorithm;
    nsString mHash;
};

/* =============== nsCSPReportURI ============ */

class nsCSPReportURI : public nsCSPBaseSrc {
  public:
    explicit nsCSPReportURI(nsIURI* aURI);
    virtual ~nsCSPReportURI();

    bool visit(nsCSPSrcVisitor* aVisitor) const;
    void toString(nsAString& outStr) const;

  private:
    nsCOMPtr<nsIURI> mReportURI;
};

/* =============== nsCSPSandboxFlags ================== */

class nsCSPSandboxFlags : public nsCSPBaseSrc {
  public:
    explicit nsCSPSandboxFlags(const nsAString& aFlags);
    virtual ~nsCSPSandboxFlags();

    bool visit(nsCSPSrcVisitor* aVisitor) const;
    void toString(nsAString& outStr) const;

  private:
    nsString mFlags;
};

/* =============== nsCSPSrcVisitor ================== */

class nsCSPSrcVisitor {
  public:
    virtual bool visitSchemeSrc(const nsCSPSchemeSrc& src) = 0;

    virtual bool visitHostSrc(const nsCSPHostSrc& src) = 0;

    virtual bool visitKeywordSrc(const nsCSPKeywordSrc& src) = 0;

    virtual bool visitNonceSrc(const nsCSPNonceSrc& src) = 0;

    virtual bool visitHashSrc(const nsCSPHashSrc& src) = 0;

  protected:
    explicit nsCSPSrcVisitor() {};
    virtual ~nsCSPSrcVisitor() {};
};

/* =============== nsCSPDirective ============= */

class nsCSPDirective {
  public:
    explicit nsCSPDirective(CSPDirective aDirective);
    virtual ~nsCSPDirective();

    virtual bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                         bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const;
    virtual bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                 bool aParserCreated) const;
    virtual void toString(nsAString& outStr) const;
    void toDomCSPStruct(mozilla::dom::CSP& outCSP) const;

    virtual void addSrcs(const nsTArray<nsCSPBaseSrc*>& aSrcs)
      { mSrcs = aSrcs; }

    inline bool isDefaultDirective() const
     { return mDirective == nsIContentSecurityPolicy::DEFAULT_SRC_DIRECTIVE; }

    virtual bool equals(CSPDirective aDirective) const;

    void getReportURIs(nsTArray<nsString> &outReportURIs) const;

    bool visitSrcs(nsCSPSrcVisitor* aVisitor) const;

    virtual void getDirName(nsAString& outStr) const;

  protected:
    CSPDirective            mDirective;
    nsTArray<nsCSPBaseSrc*> mSrcs;
};

/* =============== nsCSPChildSrcDirective ============= */

/*
 * In CSP 3 child-src is deprecated. For backwards compatibility
 * child-src needs to restrict:
 *   (*) frames, in case frame-src is not expicitly specified
 *   (*) workers, in case worker-src is not expicitly specified
 */
class nsCSPChildSrcDirective : public nsCSPDirective {
  public:
    explicit nsCSPChildSrcDirective(CSPDirective aDirective);
    virtual ~nsCSPChildSrcDirective();

    void setRestrictFrames()
      { mRestrictFrames = true; }

    void setRestrictWorkers()
      { mRestrictWorkers = true; }

    virtual bool equals(CSPDirective aDirective) const;

  private:
    bool mRestrictFrames;
    bool mRestrictWorkers;
};

/* =============== nsCSPScriptSrcDirective ============= */

/*
 * In CSP 3 worker-src restricts workers, for backwards compatibily
 * script-src has to restrict workers as the ultimate fallback if
 * neither worker-src nor child-src is present in a CSP.
 */
class nsCSPScriptSrcDirective : public nsCSPDirective {
  public:
    explicit nsCSPScriptSrcDirective(CSPDirective aDirective);
    virtual ~nsCSPScriptSrcDirective();

    void setRestrictWorkers()    { mRestrictWorkers = true; }
    void setRestrictScriptElem() { mRestrictScriptElem = true; }
    void setRestrictScriptAttr() { mRestrictScriptAttr = true; }

    virtual bool equals(CSPDirective aDirective) const;

  private:
    bool mRestrictWorkers;
    bool mRestrictScriptElem;
    bool mRestrictScriptAttr;
};

/* =============== nsCSPStyleSrcDirective ============= */

/*
 * In CSP 3, style-src is used as a fallback for style-src-elem and
 * style-src-attr in case they aren't defined.
 */
class nsCSPStyleSrcDirective : public nsCSPDirective {
  public:
    explicit nsCSPStyleSrcDirective(CSPDirective aDirective);
    virtual ~nsCSPStyleSrcDirective();

    void setRestrictStyleElem() { mRestrictStyleElem = true; }
    void setRestrictStyleAttr() { mRestrictStyleAttr = true; }

    virtual bool equals(CSPDirective aDirective) const;

  private:
    bool mRestrictStyleElem;
    bool mRestrictStyleAttr;
};

/* =============== nsBlockAllMixedContentDirective === */

class nsBlockAllMixedContentDirective : public nsCSPDirective {
  public:
    explicit nsBlockAllMixedContentDirective(CSPDirective aDirective);
    ~nsBlockAllMixedContentDirective();

    bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                 bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const
      { return false; }

    bool permits(nsIURI* aUri) const
      { return false; }

    bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                bool aParserCreated) const
      { return false; }

    void toString(nsAString& outStr) const;

    void addSrcs(const nsTArray<nsCSPBaseSrc*>& aSrcs)
      {  MOZ_ASSERT(false, "block-all-mixed-content does not hold any srcs"); }

    void getDirName(nsAString& outStr) const override;
};

/* =============== nsUpgradeInsecureDirective === */

/*
 * Upgrading insecure requests includes the following actors:
 * (1) CSP:
 *     The CSP implementation whitelists the http-request
 *     in case the policy is executed in enforcement mode.
 *     The CSP implementation however does not allow http
 *     requests to succeed if executed in report-only mode.
 *     In such a case the CSP implementation reports the
 *     error back to the page.
 *
 * (2) MixedContent:
 *     The evalution of MixedContent whitelists all http
 *     requests with the promise that the http requests
 *     gets upgraded to https before any data is fetched
 *     from the network.
 *
 * (3) CORS:
 *     Does not consider the http request to be of a
 *     different origin in case the scheme is the only
 *     difference in otherwise matching URIs.
 *
 * (4) nsHttpChannel:
 *     Before connecting, the channel gets redirected
 *     to use https.
 *
 * (5) WebSocketChannel:
 *     Similar to the httpChannel, the websocketchannel
 *     gets upgraded from ws to wss.
 */
class nsUpgradeInsecureDirective : public nsCSPDirective {
  public:
    explicit nsUpgradeInsecureDirective(CSPDirective aDirective);
    ~nsUpgradeInsecureDirective();

    bool permits(nsIURI* aUri, const nsAString& aNonce, bool aWasRedirected,
                 bool aReportOnly, bool aUpgradeInsecure, bool aParserCreated) const
      { return false; }

    bool permits(nsIURI* aUri) const
      { return false; }

    bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                bool aParserCreated) const
      { return false; }

    void toString(nsAString& outStr) const;

    void addSrcs(const nsTArray<nsCSPBaseSrc*>& aSrcs)
      {  MOZ_ASSERT(false, "upgrade-insecure-requests does not hold any srcs"); }

    void getDirName(nsAString& outStr) const override;
};

/* ===== nsRequireSRIForDirective ========================= */

class nsRequireSRIForDirective : public nsCSPDirective {
  public:
    explicit nsRequireSRIForDirective(CSPDirective aDirective);
    ~nsRequireSRIForDirective();

    void toString(nsAString& outStr) const;

    void addType(nsContentPolicyType aType)
      { mTypes.AppendElement(aType); }
    bool hasType(nsContentPolicyType aType) const;
    bool restrictsContentType(nsContentPolicyType aType) const;
    bool allows(enum CSPKeyword aKeyword, const nsAString& aHashOrNonce,
                bool aParserCreated) const;
    void getDirName(nsAString& outStr) const override;

  private:
    nsTArray<nsContentPolicyType> mTypes;
};

/* =============== nsCSPPolicy ================== */

class nsCSPPolicy {
  public:
    nsCSPPolicy();
    virtual ~nsCSPPolicy();

    bool permits(CSPDirective aDirective,
                 nsIURI* aUri,
                 const nsAString& aNonce,
                 bool aWasRedirected,
                 bool aSpecific,
                 bool aParserCreated,
                 nsAString& outViolatedDirective) const;
    bool allows(CSPDirective aDirective,
                enum CSPKeyword aKeyword,
                const nsAString& aHashOrNonce,
                bool aParserCreated) const;
    void toString(nsAString& outStr) const;
    void toDomCSPStruct(mozilla::dom::CSP& outCSP) const;

    inline void addDirective(nsCSPDirective* aDir)
      { mDirectives.AppendElement(aDir); }

    inline void addUpgradeInsecDir(nsUpgradeInsecureDirective* aDir)
      {
        mUpgradeInsecDir = aDir;
        addDirective(aDir);
      }

    bool hasDirective(CSPDirective aDir) const;

    inline void setReportOnlyFlag(bool aFlag)
      { mReportOnly = aFlag; }

    inline bool getReportOnlyFlag() const
      { return mReportOnly; }

    void getReportURIs(nsTArray<nsString> &outReportURIs) const;

    void getDirectiveStringForContentType(CSPDirective aDirective,
                                          nsAString& outDirective) const;

    void getDirectiveAsString(CSPDirective aDir, nsAString& outDirective) const;

    uint32_t getSandboxFlags() const;

    bool requireSRIForType(nsContentPolicyType aContentType);

    inline uint32_t getNumDirectives() const
      { return mDirectives.Length(); }

    bool visitDirectiveSrcs(CSPDirective aDir, nsCSPSrcVisitor* aVisitor) const;

  private:
    nsUpgradeInsecureDirective* mUpgradeInsecDir;
    nsTArray<nsCSPDirective*>   mDirectives;
    bool                        mReportOnly;
};

#endif /* nsCSPUtils_h___ */
