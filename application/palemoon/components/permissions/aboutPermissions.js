/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var Ci = Components.interfaces;
var Cc = Components.classes;
var Cu = Components.utils;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/DownloadUtils.jsm");
Cu.import("resource://gre/modules/AddonManager.jsm");
Cu.import("resource://gre/modules/NetUtil.jsm");
Cu.import("resource://gre/modules/ForgetAboutSite.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "PluralForm",
                                  "resource://gre/modules/PluralForm.jsm");

var gSecMan = Cc["@mozilla.org/scriptsecuritymanager;1"].
              getService(Ci.nsIScriptSecurityManager);

var gFaviconService = Cc["@mozilla.org/browser/favicon-service;1"].
                      getService(Ci.nsIFaviconService);

var gPlacesDatabase = Cc["@mozilla.org/browser/nav-history-service;1"].
                      getService(Ci.nsPIPlacesDatabase).
                      DBConnection.
                      clone(true);

var gSitesStmt = gPlacesDatabase.createAsyncStatement(
                  "SELECT url " +
                  "FROM moz_places " +
                  "WHERE rev_host > '.' " +
                  "AND visit_count > 0 " +
                  "GROUP BY rev_host " +
                  "ORDER BY MAX(frecency) DESC " +
                  "LIMIT :limit");

var gVisitStmt = gPlacesDatabase.createAsyncStatement(
                  "SELECT SUM(visit_count) AS count " +
                  "FROM moz_places " +
                  "WHERE rev_host = :rev_host");

var gFlash = {
  name: "Shockwave Flash",
  betterName: "Adobe Flash",
  type: "application/x-shockwave-flash",
};

// XXX:
// Is there a better way to do this rather than this hacky comparison?
// Copied this from toolkit/components/passwordmgr/crypto-SDR.js
const MASTER_PASSWORD_MESSAGE = "User canceled master password entry";

/**
 * Permission types that should be tested with testExactPermission, as opposed
 * to testPermission. This is based on what consumers use to test these
 * permissions.
 */
const TEST_EXACT_PERM_TYPES = ["desktop-notification", "geo", "pointerLock"];

/**
 * Site object represents a single site, uniquely identified by a principal.
 */
function Site(principal) {
  this.principal = principal;
  this.listitem = null;
}

Site.prototype = {
  /**
   * Gets the favicon to use for the site. The callback only gets called if
   * a favicon is found for either the http URI or the https URI.
   *
   * @param aCallback
   *        A callback function that takes a favicon image URL as a parameter.
   */
  getFavicon: function(aCallback) {
    function invokeCallback(aFaviconURI) {
      try {
        // Use getFaviconLinkForIcon to get image data from the database instead
        // of using the favicon URI to fetch image data over the network.
        aCallback(gFaviconService.getFaviconLinkForIcon(aFaviconURI).spec);
      } catch (e) {
        Cu.reportError("AboutPermissions: " + e);
      }
    }

    // Get the favicon for the origin
    gFaviconService.getFaviconURLForPage(this.principal.URI, function (aURI) {
      if (aURI) {
        invokeCallback(aURI);
      }
    }.bind(this));
  },

  /**
   * Gets the number of history visits for the site.
   *
   * @param aCallback
   *        A function that takes the visit count (a number) as a parameter.
   */
  getVisitCount: function(aCallback) {
    // XXX This won't be a very reliable system, as it will count both http: and https: visits
    // Unfortunately, I don't think that there is a much better way to do it right now.
    let rev_host = this.principal.URI.host.split("").reverse().join("") + ".";
    gVisitStmt.params.rev_host = rev_host;
    gVisitStmt.executeAsync({
      handleResult: function(aResults) {
        let row = aResults.getNextRow();
        let count = row.getResultByName("count") || 0;
        try {
          aCallback(count);
        } catch (e) {
          Cu.reportError("AboutPermissions: " + e);
        }
      },
      handleError: function(aError) {
        Cu.reportError("AboutPermissions: " + aError);
      },
      handleCompletion: function(aReason) {
      }
    });
  },

  /**
   * Gets the permission value stored for a specified permission type.
   *
   * @param aType
   *        The permission type string stored in permission manager.
   *        e.g. "cookie", "geo", "popup", "image"
   * @param aResultObj
   *        An object that stores the permission value set for aType.
   *
   * @return A boolean indicating whether or not a permission is set.
   */
  getPermission: function(aType, aResultObj) {
    // Password saving isn't a nsIPermissionManager permission type, so handle
    // it seperately.
    if (aType == "password") {
      aResultObj.value =  this.loginSavingEnabled
                          ? Ci.nsIPermissionManager.ALLOW_ACTION
                          : Ci.nsIPermissionManager.DENY_ACTION;
      return true;
    }

    let permissionValue;
    if (TEST_EXACT_PERM_TYPES.indexOf(aType) == -1) {
      permissionValue = Services.perms.testPermissionFromPrincipal(this.principal, aType);
    } else {
      permissionValue = Services.perms.testExactPermissionFromPrincipal(this.principal, aType);
    }
    aResultObj.value = permissionValue;

    if (aType.startsWith("plugin")) {
      if (permissionValue == Ci.nsIPermissionManager.PROMPT_ACTION) {
        aResultObj.value = Ci.nsIPermissionManager.UNKNOWN_ACTION;
        return true;
      }
    }

    return permissionValue != Ci.nsIPermissionManager.UNKNOWN_ACTION;
  },

  /**
   * Sets a permission for the site given a permission type and value.
   *
   * @param aType
   *        The permission type string stored in permission manager.
   *        e.g. "cookie", "geo", "popup", "image"
   * @param aPerm
   *        The permission value to set for the permission type. This should
   *        be one of the constants defined in nsIPermissionManager.
   */
  setPermission: function(aType, aPerm) {
    // Password saving isn't a nsIPermissionManager permission type, so handle
    // it seperately.
    if (aType == "password") {
      this.loginSavingEnabled = aPerm == Ci.nsIPermissionManager.ALLOW_ACTION;
      return;
    }

    if (aType.startsWith("plugin")) {
      if (aPerm == Ci.nsIPermissionManager.UNKNOWN_ACTION) {
        aPerm = Ci.nsIPermissionManager.PROMPT_ACTION;
      }
    }

    Services.perms.addFromPrincipal(this.principal, aType, aPerm);
  },

  /**
   * Clears a user-set permission value for the site given a permission type.
   *
   * @param aType
   *        The permission type string stored in permission manager.
   *        e.g. "cookie", "geo", "popup", "image"
   */
  clearPermission: function(aType) {
    Services.perms.removeFromPrincipal(this.principal, aType);
  },

  /**
   * Gets logins stored for the site.
   *
   * @return An array of the logins stored for the site.
   */
  get logins() {
    try {
     let logins = Services.logins.findLogins({},
                  this.principal.originNoSuffix, "", "");
     return logins;
    } catch (e) {
      if (!e.message.includes(MASTER_PASSWORD_MESSAGE)) {
        Cu.reportError("AboutPermissions: " + e);
      }
      return [];
    }
  },

  get loginSavingEnabled() {
    // Only say that login saving is blocked if it is blocked for both
    // http and https.
    try {
      return Services.logins.getLoginSavingEnabled(this.principal.originNoSuffix);
    } catch (e) {
      if (!e.message.includes(MASTER_PASSWORD_MESSAGE)) {
        Cu.reportError("AboutPermissions: " + e);
      }
      return false;
    }
  },

  set loginSavingEnabled(isEnabled) {
    try {
      Services.logins.setLoginSavingEnabled(this.principal.originNoSuffix, isEnabled);
    } catch (e) {
      if (!e.message.includes(MASTER_PASSWORD_MESSAGE)) {
        Cu.reportError("AboutPermissions: " + e);
      }
    }
  },

  /**
   * Gets cookies stored for the site and base domain.
   *
   * @return An array of the cookies set for the site and base domain.
   */
  get cookies() {
    let cookies = [];
    let enumerator = Services.cookies.enumerator;
    while (enumerator.hasMoreElements()) {
      let cookie = enumerator.getNext().QueryInterface(Ci.nsICookie2);
      if (cookie.host.hasRootDomain(
          AboutPermissions.domainFromHost(this.principal.URI.host))) {
        cookies.push(cookie);
      }
    }
    return cookies;
  },

  /**
   * Removes a set of specific cookies from the browser.
   */
  clearCookies: function() {
    this.cookies.forEach(function(aCookie) {
      Services.cookies.remove(aCookie.host, aCookie.name, aCookie.path, false,
                              aCookie.originAttributes);
    });
  },

  /**
   * Removes all data from the browser corresponding to the site.
   */
  forgetSite: function() {
    // XXX This removes data for an entire domain, rather than just
    // an origin. This may produce confusing results, as data will
    // be cleared for the http:// as well as the https:// domain
    // if you try to forget the https:// site.
    ForgetAboutSite.removeDataFromDomain(this.principal.URI.host)
                   .catch(Cu.reportError);
  }
}

/**
 * PermissionDefaults object keeps track of default permissions for sites based
 * on global preferences.
 *
 * Inspired by pageinfo/permissions.js
 */
var PermissionDefaults = {
  UNKNOWN: Ci.nsIPermissionManager.UNKNOWN_ACTION,   // 0
  ALLOW: Ci.nsIPermissionManager.ALLOW_ACTION,       // 1
  DENY: Ci.nsIPermissionManager.DENY_ACTION,         // 2
  SESSION: Ci.nsICookiePermission.ACCESS_SESSION,    // 8

  get password() {
    if (Services.prefs.getBoolPref("signon.rememberSignons")) {
      return this.ALLOW;
    }
    return this.DENY;
  },
  set password(aValue) {
    let value = (aValue != this.DENY);
    Services.prefs.setBoolPref("signon.rememberSignons", value);
  },

  IMAGE_ALLOW: 1,
  IMAGE_DENY: 2,
  IMAGE_ALLOW_FIRST_PARTY_ONLY: 3,

  get image() {
    if (Services.prefs.getIntPref("permissions.default.image")
        == this.IMAGE_DENY) {
      return this.IMAGE_DENY;
    } else if (Services.prefs.getIntPref("permissions.default.image")
        == this.IMAGE_ALLOW_FIRST_PARTY_ONLY) {
      return this.IMAGE_ALLOW_FIRST_PARTY_ONLY;
    }
    return this.IMAGE_ALLOW;
  },
  set image(aValue) {
    let value = this.IMAGE_ALLOW; 
    if (aValue == this.IMAGE_DENY) {
      value = this.IMAGE_DENY;
    } else if (aValue == this.IMAGE_ALLOW_FIRST_PARTY_ONLY) {
      value = this.IMAGE_ALLOW_FIRST_PARTY_ONLY;
    }
    Services.prefs.setIntPref("permissions.default.image", value);
  },

  get popup() {
    if (Services.prefs.getBoolPref("dom.disable_open_during_load")) {
      return this.DENY;
    }
    return this.ALLOW;
  },
  set popup(aValue) {
    let value = (aValue == this.DENY);
    Services.prefs.setBoolPref("dom.disable_open_during_load", value);
  },

  // For use with network.cookie.* prefs.
  COOKIE_ACCEPT: 0,
  COOKIE_DENY: 2,
  COOKIE_NORMAL: 0,
  COOKIE_SESSION: 2,

  get cookie() {
    if (Services.prefs.getIntPref("network.cookie.cookieBehavior")
        == this.COOKIE_DENY) {
      return this.DENY;
    }

    if (Services.prefs.getIntPref("network.cookie.lifetimePolicy")
        == this.COOKIE_SESSION) {
      return this.SESSION;
    }
    return this.ALLOW;
  },
  set cookie(aValue) {
    let value = (aValue == this.DENY) ? this.COOKIE_DENY : this.COOKIE_ACCEPT;
    Services.prefs.setIntPref("network.cookie.cookieBehavior", value);

    let lifetimeValue = aValue == this.SESSION ? this.COOKIE_SESSION :
                                                 this.COOKIE_NORMAL;
    Services.prefs.setIntPref("network.cookie.lifetimePolicy", lifetimeValue);
  },

  get ["desktop-notification"]() {
    if (!Services.prefs.getBoolPref("dom.webnotifications.enabled")) {
      return this.DENY;
    }
    // We always ask for permission to enable notifications for a specific
    // site, so there is no global ALLOW.
    return this.UNKNOWN;
  },
  set ["desktop-notification"](aValue) {
    let value = (aValue != this.DENY);
    Services.prefs.setBoolPref("dom.webnotifications.enabled", value);
  },

  get install() {
    if (Services.prefs.getBoolPref("xpinstall.whitelist.required")) {
      return this.DENY;
    }
    return this.ALLOW;
  },
  set install(aValue) {
    let value = (aValue == this.DENY);
    Services.prefs.setBoolPref("xpinstall.whitelist.required", value);
  },

  get geo() {
    if (!Services.prefs.getBoolPref("geo.enabled")) {
      return this.DENY;
    }
    // We always ask for permission to share location with a specific site,
    // so there is no global ALLOW.
    return this.UNKNOWN;
  },
  set geo(aValue) {
    let value = (aValue != this.DENY);
    Services.prefs.setBoolPref("geo.enabled", value);
  },
}

/**
 * AboutPermissions manages the about:permissions page.
 */
var AboutPermissions = {
 /**
  * Maximum number of sites to return from the places database.
  */  
  PLACES_SITES_LIMIT_MAX: 100,

  /**
   * When adding sites to the dom sites-list, divide workload into intervals.
   */
  LIST_BUILD_DELAY: 100, // delay between intervals

  /**
   * Stores a mapping of origin strings to Site objects.
   */
  _sites: {},

  /**
   * Using a getter for sitesFilter to avoid races with tests.
   */
  get sitesFilter () {
    delete this.sitesFilter;
    return this.sitesFilter = document.getElementById("sites-filter");
  },

  sitesList: null,
  _selectedSite: null,

  /**
   * For testing, track initializations so we can send notifications.
   */
  _initPlacesDone: false,
  _initServicesDone: false,

  /**
   * This reflects the permissions that we expose in the UI. These correspond
   * to permission type strings in the permission manager, PermissionDefaults,
   * and element ids in aboutPermissions.xul.
   *
   * Potential future additions: "sts/use", "sts/subd"
   */
  _supportedPermissions: ["password", "image", "popup", "cookie",
                          "desktop-notification", "install", "geo"],

  /**
   * Permissions that don't have a global "Allow" option.
   */
  _noGlobalAllow: ["desktop-notification", "geo"],

  /**
   * Permissions that don't have a global "Deny" option.
   */
  _noGlobalDeny: [],

  _stringBundleBrowser: Services.strings
      .createBundle("chrome://browser/locale/browser.properties"),

  _stringBundleAboutPermissions: Services.strings.createBundle(
      "chrome://browser/locale/permissions/aboutPermissions.properties"),

  _initPart1: function() {
    this.initPluginList();
    this.cleanupPluginList();

    this.getSitesFromPlaces();

    this.enumerateServicesGenerator = this.getEnumerateServicesGenerator();
    setTimeout(this.enumerateServicesDriver.bind(this), this.LIST_BUILD_DELAY);
  },

  _initPart2: function() {
    this._supportedPermissions.forEach(function(aType) {
      this.updatePermission(aType);
    }, this);
  },

  /**
   * Called on page load.
   */
  init: function() {
    this.sitesList = document.getElementById("sites-list");

    this._initPart1();

    // Attach observers in case data changes while the page is open.
    Services.prefs.addObserver("signon.rememberSignons", this, false);
    Services.prefs.addObserver("permissions.default.image", this, false);
    Services.prefs.addObserver("dom.disable_open_during_load", this, false);
    Services.prefs.addObserver("network.cookie.", this, false);
    Services.prefs.addObserver("dom.webnotifications.enabled", this, false);
    Services.prefs.addObserver("xpinstall.whitelist.required", this, false);
    Services.prefs.addObserver("geo.enabled", this, false);
    Services.prefs.addObserver("plugins.click_to_play", this, false);
    Services.prefs.addObserver("permissions.places-sites-limit", this, false);

    Services.obs.addObserver(this, "perm-changed", false);
    Services.obs.addObserver(this, "passwordmgr-storage-changed", false);
    Services.obs.addObserver(this, "cookie-changed", false);
    Services.obs.addObserver(this, "browser:purge-domain-data", false);
    Services.obs.addObserver(this, "plugin-info-updated", false);
    Services.obs.addObserver(this, "plugin-list-updated", false);
    Services.obs.addObserver(this, "blocklist-updated", false);
    
    this._observersInitialized = true;
    Services.obs.notifyObservers(null, "browser-permissions-preinit", null);

    this._initPart2();

    // Process about:permissions?filter=<string>
    // About URIs don't support query params, so do this manually
    var loc = document.location.href;
    var matches = /[?&]filter\=([^&]+)/i.exec(loc);
    if (matches) {
      this.sitesFilter.value = decodeURIComponent(matches[1]);
    }
  },

  sitesReload: function() {
    Object.getOwnPropertyNames(this._sites).forEach(function(prop) {
      AboutPermissions.deleteFromSitesList(prop);
    });
    this._initPart1();
    this._initPart2();
  },

  // XXX copied this from browser-plugins.js - is there a way to share?
  // Map the plugin's name to a filtered version more suitable for user UI.
  makeNicePluginName: function(aName) {
    if (aName == gFlash.name) {
      return gFlash.betterName;
    }

    // Clean up the plugin name by stripping off any trailing version numbers
    // or "plugin". EG, "Foo Bar Plugin 1.23_02" --> "Foo Bar"
    // Do this by first stripping the numbers, etc. off the end, and then
    // removing "Plugin" (and then trimming to get rid of any whitespace).
    // (Otherwise, something like "Java(TM) Plug-in 1.7.0_07" gets mangled.)
    let newName = aName.replace(
        /[\s\d\.\-\_\(\)]+$/, "").replace(/\bplug-?in\b/i, "").trim();
    return newName;
  },

  initPluginList: function() {
    let pluginHost = Cc["@mozilla.org/plugin/host;1"]
                     .getService(Ci.nsIPluginHost);
    let tags = pluginHost.getPluginTags();

    let permissionMap = new Map();

    let permissionEntries = [];
    let XUL_NS =
        "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul";
    for (let plugin of tags) {
      for (let mimeType of plugin.getMimeTypes()) {
        if ((mimeType == gFlash.type) && (plugin.name != gFlash.name)) {
          continue;
        }
        let permString = pluginHost.getPermissionStringForType(mimeType);
        if (!permissionMap.has(permString)) {
          let permissionEntry = document.createElementNS(XUL_NS, "box");
          permissionEntry.setAttribute("label",
                                       this.makeNicePluginName(plugin.name)
                                       + " " + plugin.version);
          permissionEntry.setAttribute("tooltiptext", plugin.description);
          permissionEntry.setAttribute("vulnerable", "");
          permissionEntry.setAttribute("mimeType", mimeType);
          permissionEntry.setAttribute("permString", permString);
          permissionEntry.setAttribute("class", "pluginPermission");
          permissionEntry.setAttribute("id", permString + "-entry");
          // If the plugin is disabled, it makes no sense to change its
          // click-to-play status, so don't add it.
          if (plugin.disabled) {
            permissionEntry.hidden = true;
          } else {
            permissionEntry.hidden = false;
          }
          permissionEntries.push(permissionEntry);
          this._supportedPermissions.push(permString);
          this._noGlobalDeny.push(permString);
          Object.defineProperty(PermissionDefaults, permString, {
            get: function() {
                   if ((Services.prefs.getBoolPref("plugins.click_to_play") &&
                       plugin.clicktoplay) ||
                       permString.startsWith("plugin-vulnerable:")) {
                     return PermissionDefaults.UNKNOWN;
                   }
                   return PermissionDefaults.ALLOW;
                 },
            set: function(aValue) {
                   this.clicktoplay = (aValue == PermissionDefaults.UNKNOWN);
                 }.bind(plugin),
            configurable: true
          });
          permissionMap.set(permString, "");
        }
      }
    }

    if (permissionEntries.length > 0) {
      permissionEntries.sort(function(entryA, entryB) {
        let labelA = entryA.getAttribute("label");
        let labelB = entryB.getAttribute("label");
        return ((labelA < labelB) ? -1 : (labelA == labelB ? 0 : 1));
      });
    }

    let pluginsBox = document.getElementById("plugins-box");
    while (pluginsBox.hasChildNodes()) {
      pluginsBox.removeChild(pluginsBox.firstChild);
    }
    for (let permissionEntry of permissionEntries) {
      pluginsBox.appendChild(permissionEntry);
    }
  },

  cleanupPluginList: function() {
    let pluginsPrefItem = document.getElementById("plugins-pref-item");
    let pluginsBox = document.getElementById("plugins-box");
    let pluginsBoxEmpty = true;
    let pluginsBoxSibling = pluginsBox.firstChild;
    while (pluginsBoxSibling) {
      if (!pluginsBoxSibling.hidden) {
        pluginsBoxEmpty = false;
        break;
      }
      pluginsBoxSibling = pluginsBoxSibling.nextSibling;
    }
    if (pluginsBoxEmpty) {
      pluginsPrefItem.collapsed = true;
    } else {
      pluginsPrefItem.collapsed = false;
    }
  },

  /**
   * Called on page unload.
   */
  cleanUp: function() {
    if (this._observersInitialized) {
      Services.prefs.removeObserver("signon.rememberSignons", this, false);
      Services.prefs.removeObserver("permissions.default.image", this, false);
      Services.prefs.removeObserver("dom.disable_open_during_load", this, false);
      Services.prefs.removeObserver("network.cookie.", this, false);
      Services.prefs.removeObserver("dom.webnotifications.enabled", this, false);
      Services.prefs.removeObserver("xpinstall.whitelist.required", this, false);
      Services.prefs.removeObserver("geo.enabled", this, false);
      Services.prefs.removeObserver("plugins.click_to_play", this, false);
      Services.prefs.removeObserver("permissions.places-sites-limit", this, false);

      Services.obs.removeObserver(this, "perm-changed");
      Services.obs.removeObserver(this, "passwordmgr-storage-changed");
      Services.obs.removeObserver(this, "cookie-changed");
      Services.obs.removeObserver(this, "browser:purge-domain-data");
      Services.obs.removeObserver(this, "plugin-info-updated");
      Services.obs.removeObserver(this, "plugin-list-updated");
      Services.obs.removeObserver(this, "blocklist-updated");
    }

    gSitesStmt.finalize();
    gVisitStmt.finalize();
    gPlacesDatabase.asyncClose(null);
  },

  observe: function(aSubject, aTopic, aData) {
    switch(aTopic) {
      case "perm-changed":
        // Permissions changes only affect individual sites.
        if (!this._selectedSite) {
          break;
        }
        // aSubject is null when nsIPermisionManager::removeAll() is called.
        if (!aSubject) {
          this._supportedPermissions.forEach(function(aType) {
            this.updatePermission(aType);
          }, this);
          break;
        }
        let permission = aSubject.QueryInterface(Ci.nsIPermission);
        // We can't compare selectedSite.principal and permission.principal here
        // because we need to handle the case where a parent domain was changed
        // in a way that affects the subdomain.
        if (this._supportedPermissions.indexOf(permission.type) != -1) {
          this.updatePermission(permission.type);
        }
        break;
      case "nsPref:changed":
        if (aData == "permissions.places-sites-limit") {
          this.sitesReload();
          return;
        }
        let plugin = false;
        if (aData.startsWith("plugin")) {
          plugin = true;
        }
        if (plugin) {
          this.initPluginList();
        }
        this._supportedPermissions.forEach(function(aType) {
          if (!plugin || (plugin && aType.startsWith("plugin"))) {
            this.updatePermission(aType);
          }
        }, this);
        if (plugin) {
          this.cleanupPluginList();
        }
        break;
      case "passwordmgr-storage-changed":
        this.updatePermission("password");
        if (this._selectedSite) {
          this.updatePasswordsCount();
        }
        break;
      case "cookie-changed":
        if (this._selectedSite) {
          this.updateCookiesCount();
        }
        break;
      case "browser:purge-domain-data":
        this.deleteFromSitesList(aData);
        break;
      case "plugin-info-updated":
      case "plugin-list-updated":
      case "blocklist-updated":
        this.initPluginList();
        this._supportedPermissions.forEach(function(aType) {
          if (aType.startsWith("plugin")) {
            this.updatePermission(aType);
          }
        }, this);
        this.cleanupPluginList();
        break;
    }
  },

  /**
   * Creates Site objects for the top-frecency sites in the places database
   * and stores them in _sites.
   * The number of sites created is controlled by _placesSitesLimit.
   */
  getSitesFromPlaces: function() {
    let _placesSitesLimit = Services.prefs.getIntPref(
        "permissions.places-sites-limit");
    if (_placesSitesLimit <= 0) {
      return;
    }
    if (_placesSitesLimit > this.PLACES_SITES_LIMIT_MAX) {
      _placesSitesLimit = this.PLACES_SITES_LIMIT_MAX;
    }

    gSitesStmt.params.limit = _placesSitesLimit;
    gSitesStmt.executeAsync({
      handleResult: function(aResults) {
        AboutPermissions.startSitesListBatch();
        let row;
        while (row = aResults.getNextRow()) {
          let spec = row.getResultByName("url");
          let uri = NetUtil.newURI(spec);
          let principal = gSecMan.getNoAppCodebasePrincipal(uri);

          AboutPermissions.addPrincipal(principal);
        }
        AboutPermissions.endSitesListBatch();
      },
      handleError: function(aError) {
        Cu.reportError("AboutPermissions: " + aError);
      },
      handleCompletion: function(aReason) {
        // Notify oberservers for testing purposes.
        AboutPermissions._initPlacesDone = true;
        if (AboutPermissions._initServicesDone) {
          Services.obs.notifyObservers(
              null, "browser-permissions-initialized", null);
        }
      }
    });
  },

  /**
   * Drives getEnumerateServicesGenerator to work in intervals.
   */
  enumerateServicesDriver: function() {
    if (this.enumerateServicesGenerator.next()) {
      // Build top sitesList items faster so that the list never seems sparse
      let delay = Math.min(this.sitesList.itemCount * 5, this.LIST_BUILD_DELAY);
      setTimeout(this.enumerateServicesDriver.bind(this), delay);
    } else {
      this.enumerateServicesGenerator.close();
      this._initServicesDone = true;
      if (this._initPlacesDone) {
        Services.obs.notifyObservers(
            null, "browser-permissions-initialized", null);
      }
    }
  },

  /**
   * Finds sites that have non-default permissions and creates Site objects
   * for them if they are not already stored in _sites.
   */
  getEnumerateServicesGenerator: function() {
    let itemCnt = 1;
    let schemeChrome = "chrome";

    try {
      let logins = Services.logins.getAllLogins();
      logins.forEach(function(aLogin) {
        try {
          // aLogin.hostname is a string in origin URL format
          // (e.g. "http://foo.com").
          // newURI will throw for add-ons logins stored in chrome:// URIs
          // i.e.: "chrome://weave" (Sync)
          if (!aLogin.hostname.startsWith(schemeChrome + ":")) {
            let uri = NetUtil.newURI(aLogin.hostname);
            let principal = gSecMan.getNoAppCodebasePrincipal(uri);
            this.addPrincipal(principal);
          }
        } catch (e) {
          Cu.reportError("AboutPermissions: " + e);
        }
        itemCnt++;
      }, this);

      let disabledHosts = Services.logins.getAllDisabledHosts();
      disabledHosts.forEach(function(aHostname) {
        try {
          // aHostname is a string in origin URL format (e.g. "http://foo.com").
          // newURI will throw for add-ons logins stored in chrome:// URIs
          // i.e.: "chrome://weave" (Sync)
          if (!aHostname.startsWith(schemeChrome + ":")) {
            let uri = NetUtil.newURI(aHostname);
            let principal = gSecMan.getNoAppCodebasePrincipal(uri);
            this.addPrincipal(principal);
          }
        } catch (e) {
          Cu.reportError("AboutPermissions: " + e);
        }
        itemCnt++;
      }, this);
    } catch (e) {
      if (!e.message.includes(MASTER_PASSWORD_MESSAGE)) {
        Cu.reportError("AboutPermissions: " + e);
      }
    }

    let enumerator = Services.perms.enumerator;
    while (enumerator.hasMoreElements()) {
      let permission = enumerator.getNext().QueryInterface(Ci.nsIPermission);
      // Only include sites with exceptions set for supported permission types.
      if (this._supportedPermissions.indexOf(permission.type) != -1) {
        this.addPrincipal(permission.principal);
      }
      itemCnt++;
    }

    yield false;
  },

  /**
   * Creates a new Site and adds it to _sites if it's not already there.
   *
   * @param aPrincipal
   *        A principal.
   */
  addPrincipal: function(aPrincipal) {
    if (aPrincipal.origin in this._sites) {
      return;
    }
    let site = new Site(aPrincipal);
    this._sites[aPrincipal.origin] = site;
    this.addToSitesList(site);
  },

  /**
   * Populates sites-list richlistbox with data from Site object.
   *
   * @param aSite
   *        A Site object.
   */
  addToSitesList: function(aSite) {
    let item = document.createElement("richlistitem");
    item.setAttribute("class", "site");
    item.setAttribute("value", aSite.principal.origin);

    aSite.getFavicon(function(aURL) {
      item.setAttribute("favicon", aURL);
    });
    aSite.listitem = item;

    // Make sure to only display relevant items when list is filtered.
    let filterValue = this.sitesFilter.value.toLowerCase();
    item.collapsed = aSite.principal.origin.toLowerCase().indexOf(filterValue) == -1;

    (this._listFragment || this.sitesList).appendChild(item);
  },

  startSitesListBatch: function() {
    if (!this._listFragment)
      this._listFragment = document.createDocumentFragment();
  },

  endSitesListBatch: function() {
    if (this._listFragment) {
      this.sitesList.appendChild(this._listFragment);
      this._listFragment = null;
    }
  },

  /**
   * Hides sites in richlistbox based on search text in sites-filter textbox.
   */
  filterSitesList: function() {
    let siteItems = this.sitesList.children;
    let filterValue = this.sitesFilter.value.toLowerCase();

    if (filterValue == "") {
      for (let i = 0, iLen = siteItems.length; i < iLen; i++) {
        siteItems[i].collapsed = false;
      }
      return;
    }

    for (let i = 0, iLen = siteItems.length; i < iLen; i++) {
      let siteValue = siteItems[i].value.toLowerCase();
      siteItems[i].collapsed = siteValue.indexOf(filterValue) == -1;
    }
  },

  /**
   * Removes all evidence of the selected site. The "forget this site" observer
   * will call deleteFromSitesList to update the UI.
   */
  forgetSite: function() {
    this._selectedSite.forgetSite();
  },

  /**
   * Deletes sites for a host and all of its sub-domains. Removes these sites
   * from _sites and removes their corresponding elements from the DOM.
   *
   * @param aHost
   *        The host string corresponding to the site to delete.
   */
  deleteFromSitesList: function(aHost) {
    for (let origin in this._sites) {
      let site = this._sites[origin];
      if (site.principal.URI.host.hasRootDomain(aHost)) {
        if (site == this._selectedSite) {
          // Replace site-specific interface with "All Sites" interface.
          this.sitesList.selectedItem =
              document.getElementById("all-sites-item");
        }

        this.sitesList.removeChild(site.listitem);
        delete this._sites[site.principal.origin];
      }
    }
  },

  /**
   * Shows interface for managing site-specific permissions.
   */
  onSitesListSelect: function(event) {
    if (event.target.selectedItem.id == "all-sites-item") {
      // Clear the header label value from the previously selected site.
      document.getElementById("site-label").value = "";
      this.manageDefaultPermissions();
      return;
    }

    let origin = event.target.value;
    let site = this._selectedSite = this._sites[origin];
    document.getElementById("site-label").value = origin;
    document.getElementById("header-deck").selectedPanel =
        document.getElementById("site-header");

    this.updateVisitCount();
    this.updatePermissionsBox();
  },

  /**
   * Shows interface for managing default permissions. This corresponds to
   * the "All Sites" list item.
   */
  manageDefaultPermissions: function() {
    this._selectedSite = null;

    document.getElementById("header-deck").selectedPanel =
      document.getElementById("defaults-header");

    this.updatePermissionsBox();
  },

  /**
   * Updates permissions interface based on selected site.
   */
  updatePermissionsBox: function() {
    this._supportedPermissions.forEach(function(aType) {
      this.updatePermission(aType);
    }, this);

    this.updatePasswordsCount();
    this.updateCookiesCount();
  },

  /**
   * Sets menulist for a given permission to the correct state, based on
   * the stored permission.
   *
   * @param aType
   *        The permission type string stored in permission manager.
   *        e.g. "cookie", "geo", "popup", "image"
   */
  updatePermission: function(aType) {
    let allowItem = document.getElementById(
        aType + "-" + PermissionDefaults.ALLOW);
    allowItem.hidden = !this._selectedSite &&
                       this._noGlobalAllow.indexOf(aType) != -1;
    let denyItem = document.getElementById(
        aType + "-" + PermissionDefaults.DENY);
    denyItem.hidden = !this._selectedSite &&
                      this._noGlobalDeny.indexOf(aType) != -1;

    let permissionMenulist = document.getElementById(aType + "-menulist");
    let permissionSetDefault = document.getElementById(aType + "-set-default");
    let permissionValue;
    let permissionDefault;
    let pluginPermissionEntry;
    let elementsPrefSetDefault = document.querySelectorAll(".pref-set-default");
    if (!this._selectedSite) {
      let _visibility = "collapse";
      for (let i = 0, iLen = elementsPrefSetDefault.length; i < iLen; i++) {
        elementsPrefSetDefault[i].style.visibility = _visibility;
      }
      permissionSetDefault.style.visibility = _visibility;
      // If there is no selected site, we are updating the default permissions
      // interface.
      permissionValue = PermissionDefaults[aType];
      permissionDefault = permissionValue;
      if (aType == "image") {
        // (aType + "-3") corresponds to ALLOW_FIRST_PARTY_ONLY,
        // which is reserved for global preferences only.
        document.getElementById(aType + "-3").hidden = false;
      } else if (aType == "cookie") {
        // (aType + "-9") corresponds to ALLOW_FIRST_PARTY_ONLY,
        // which is reserved for site-specific preferences only.
        document.getElementById(aType + "-9").hidden = true;
      } else if (aType.startsWith("plugin")) {
        pluginPermissionEntry = document.getElementById(aType + "-entry");
        pluginPermissionEntry.setAttribute("vulnerable", "");
        let vulnerable = false;
        if (pluginPermissionEntry.isBlocklisted()) {
          permissionMenulist.disabled = true;
          permissionMenulist.setAttribute("tooltiptext",
              AboutPermissions._stringBundleAboutPermissions
              .GetStringFromName("pluginBlocklisted"));
          vulnerable = true;
        } else {
          permissionMenulist.disabled = false;
          permissionMenulist.setAttribute("tooltiptext", "");
        }
        if (Services.prefs.getBoolPref("plugins.click_to_play") || vulnerable) {
          document.getElementById(aType + "-0").disabled = false;
        } else {
          document.getElementById(aType + "-0").disabled = true;
        }
      }
    } else {
      let _visibility = "visible";
      for (let i = 0, iLen = elementsPrefSetDefault.length; i < iLen; i++) {
        elementsPrefSetDefault[i].style.visibility = _visibility;
      }
      permissionSetDefault.style.visibility = _visibility;
      permissionDefault = PermissionDefaults[aType];
      if (aType == "image") {
        document.getElementById(aType + "-3").hidden = true;
      } else if (aType == "cookie") {
        document.getElementById(aType + "-9").hidden = false;
      } else if (aType.startsWith("plugin")) {
        pluginPermissionEntry = document.getElementById(aType + "-entry");
        let permString = pluginPermissionEntry.getAttribute("permString");
        let vulnerable = false;        
        if (permString.startsWith("plugin-vulnerable:")) {
          let nameVulnerable = " \u2014 "
              + AboutPermissions._stringBundleBrowser
                .GetStringFromName("pluginActivateVulnerable.label");
          pluginPermissionEntry.setAttribute("vulnerable", nameVulnerable);
          vulnerable = true;
        }
        if (Services.prefs.getBoolPref("plugins.click_to_play") || vulnerable) {
          document.getElementById(aType + "-0").disabled = false;
        } else {
          document.getElementById(aType + "-0").disabled = true;
        }
        permissionMenulist.disabled = false;
        permissionMenulist.setAttribute("tooltiptext", "");
      }
      let result = {};
      permissionValue = this._selectedSite.getPermission(aType, result) ?
                        result.value : permissionDefault;
    }

    if (aType == "image") {
      if (document.getElementById(aType + "-" + permissionValue).hidden) {
        // ALLOW
        permissionValue = 1;
      }
    }
    if (aType.startsWith("plugin")) {
      if (document.getElementById(aType + "-" + permissionValue).disabled) {
        // ALLOW
        permissionValue = 1;
      }
    }

    if (!aType.startsWith("plugin")) {
      let _elementDefault = document.getElementById(aType + "-default");
      if (!this._selectedSite || (permissionValue == permissionDefault)) {
        _elementDefault.setAttribute("value", "");
      } else {
        _elementDefault.setAttribute("value", "*");
      }
    } else {
      let _elementDefaultVisibility;
      if (!this._selectedSite || (permissionValue == permissionDefault)) {
        _elementDefaultVisibility = false;
      } else {
        _elementDefaultVisibility = true;
      }
      pluginPermissionEntry.setDefaultVisibility(_elementDefaultVisibility);
    }

    permissionMenulist.selectedItem = document.getElementById(
        aType + "-" + permissionValue);
  },

  onPermissionCommand: function(event, _default) {
    let pluginHost = Cc["@mozilla.org/plugin/host;1"] 
                     .getService(Ci.nsIPluginHost);
    let permissionMimeType = event.currentTarget.getAttribute("mimeType");
    let permissionType = event.currentTarget.getAttribute("type");
    let permissionValue = event.target.value;

    if (!this._selectedSite) {
      if (permissionType.startsWith("plugin")) {
        let addonValue = AddonManager.STATE_ASK_TO_ACTIVATE;
        switch(permissionValue) {
          case "1":
            addonValue = false;
            break;
          case "2":
            addonValue = true;
            break;
        }

        AddonManager.getAddonsByTypes(["plugin"], function(addons) {
          for (let addon of addons) {
            for (let type of addon.pluginMimeTypes) {
              if ((type.type == gFlash.type) && (addon.name != gFlash.name)) {
                continue;
              }
              if (type.type.toLowerCase() == permissionMimeType.toLowerCase()) {
                addon.userDisabled = addonValue;
                return;
              }
            }
          }
        });
      } else {
        // If there is no selected site, we are setting the default permission.
        PermissionDefaults[permissionType] = permissionValue;
      }
    } else {
      if (_default) {
        this._selectedSite.clearPermission(permissionType);
      } else {
        this._selectedSite.setPermission(permissionType, permissionValue);
      }
    }
  },

  updateVisitCount: function() {
    this._selectedSite.getVisitCount(function(aCount) {
      let visitForm = AboutPermissions._stringBundleAboutPermissions
                      .GetStringFromName("visitCount");
      let visitLabel = PluralForm.get(aCount, visitForm)
                       .replace("#1", aCount);
      document.getElementById("site-visit-count").value = visitLabel;
    });  
  },

  updatePasswordsCount: function() {
    if (!this._selectedSite) {
      document.getElementById("passwords-count").hidden = true;
      document.getElementById("passwords-manage-all-button").hidden = false;
      return;
    }

    let passwordsCount = this._selectedSite.logins.length;
    let passwordsForm = this._stringBundleAboutPermissions
                        .GetStringFromName("passwordsCount");
    let passwordsLabel = PluralForm.get(passwordsCount, passwordsForm)
                                   .replace("#1", passwordsCount);

    document.getElementById("passwords-label").value = passwordsLabel;
    document.getElementById("passwords-manage-button").disabled =
        (passwordsCount < 1);
    document.getElementById("passwords-manage-all-button").hidden = true;
    document.getElementById("passwords-count").hidden = false;
  },

  /**
   * Opens password manager dialog.
   */
  managePasswords: function() {
    let selectedOrigin = "";
    if (this._selectedSite) {
      selectedOrigin = this._selectedSite.principal.URI.prePath;
    }

    let win = Services.wm.getMostRecentWindow("Toolkit:PasswordManager");
    if (win) {
      win.setFilter(selectedOrigin);
      win.focus();
    } else {
      window.openDialog("chrome://passwordmgr/content/passwordManager.xul",
                        "Toolkit:PasswordManager", "",
                        {filterString : selectedOrigin});
    }
  },

  domainFromHost: function(aHost) {
    let domain = aHost;
    try {
      domain = Services.eTLD.getBaseDomainFromHost(aHost);
    } catch (e) {
      // getBaseDomainFromHost will fail if the host is an IP address
      // or is empty.
    }

    return domain;
  },

  updateCookiesCount: function() {
    if (!this._selectedSite) {
      document.getElementById("cookies-count").hidden = true;
      document.getElementById("cookies-clear-all-button").hidden = false;
      document.getElementById("cookies-manage-all-button").hidden = false;
      return;
    }

    let cookiesCount = this._selectedSite.cookies.length;
    let cookiesForm = this._stringBundleAboutPermissions
                      .GetStringFromName("cookiesCount");
    let cookiesLabel = PluralForm.get(cookiesCount, cookiesForm)
                                 .replace("#1", cookiesCount);

    document.getElementById("cookies-label").value = cookiesLabel;
    document.getElementById("cookies-clear-button").disabled =
        (cookiesCount < 1);
    document.getElementById("cookies-manage-button").disabled =
        (cookiesCount < 1);
    document.getElementById("cookies-clear-all-button").hidden = true;
    document.getElementById("cookies-manage-all-button").hidden = true;
    document.getElementById("cookies-count").hidden = false;
  },

  /**
   * Clears cookies for the selected site and base domain.
   */
  clearCookies: function() {
    if (!this._selectedSite) {
      return;
    }
    let site = this._selectedSite;
    site.clearCookies(site.cookies);
    this.updateCookiesCount();
  },

  /**
   * Opens cookie manager dialog.
   */
  manageCookies: function() {
    // Cookies are stored by-host, and thus we filter the cookie window
    // using only the host of the selected principal's origin
    let selectedHost = "";
    let selectedDomain = "";
    if (this._selectedSite) {
      selectedHost = this._selectedSite.principal.URI.host;
      selectedDomain = this.domainFromHost(selectedHost);
    }

    let win = Services.wm.getMostRecentWindow("Browser:Cookies");
    if (win) {
      win.gCookiesWindow.setFilter(selectedDomain);
      win.focus();
    } else {
      window.openDialog("chrome://browser/content/preferences/cookies.xul",
                        "Browser:Cookies", "", {filterString : selectedDomain});
    }
  },

  /**
   * Focusses the filter box.
   */
  focusFilterBox: function() {
    this.sitesFilter.focus();
  }
}

// See toolkit/forgetaboutsite/ForgetAboutSite.jsm
String.prototype.hasRootDomain = function(aDomain) {
  let index = this.indexOf(aDomain);
  if (index == -1) {
    return false;
  }

  if (this == aDomain) {
    return true;
  }

  let prevChar = this[index - 1];
  return (index == (this.length - aDomain.length)) &&
         (prevChar == "." || prevChar == "/");
}
