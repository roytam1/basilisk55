// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Load DownloadUtils module for convertByteUnits
Components.utils.import("resource://gre/modules/DownloadUtils.jsm");
Components.utils.import("resource://gre/modules/ctypes.jsm");
Components.utils.import("resource://gre/modules/Services.jsm");
Components.utils.import("resource://gre/modules/LoadContextInfo.jsm");
Components.utils.import("resource://gre/modules/BrowserUtils.jsm");

var gAdvancedPane = {
  _inited: false,

  /**
   * Brings the appropriate tab to the front and initializes various bits of UI.
   */
  init: function()
  {
    this._inited = true;
    var advancedPrefs = document.getElementById("advancedPrefs");

    var extraArgs = window.arguments[1];
    if (extraArgs && extraArgs["advancedTab"]){
      advancedPrefs.selectedTab = document.getElementById(extraArgs["advancedTab"]);
    } else {
      var preference = document.getElementById("browser.preferences.advanced.selectedTabIndex");
      if (preference.value !== null)
        advancedPrefs.selectedIndex = preference.value;
    }

#ifdef HAVE_SHELL_SERVICE
    this.updateSetDefaultBrowser();
#ifdef XP_WIN
    // In Windows 8 we launch the control panel since it's the only
    // way to get all file type association prefs. So we don't know
    // when the user will select the default.  We refresh here periodically
    // in case the default changes.  On other Windows OS's defaults can also
    // be set while the prefs are open.
    window.setInterval(this.updateSetDefaultBrowser, 1000);
#endif
#endif

#ifdef MOZ_UPDATER
    this.updateReadPrefs();
#endif
    this.updateOfflineAppsPermissions();
    this.updateOfflineApps();

    this.updateActualCacheSize();
    this.updateActualAppCacheSize();

    this.updateHWADisplay();

    // Notify observers that the UI is now ready
    Services.obs.notifyObservers(window, "advanced-pane-loaded", null);
  },

  /**
   * Stores the identity of the current tab in preferences so that the selected
   * tab can be persisted between openings of the preferences window.
   */
  tabSelectionChanged: function()
  {
    if (!this._inited)
      return;
    var advancedPrefs = document.getElementById("advancedPrefs");
    var preference = document.getElementById("browser.preferences.advanced.selectedTabIndex");
    preference.valueFromPreferences = advancedPrefs.selectedIndex;
  },

  // GENERAL TAB

  /*
   * Preferences:
   *
   * accessibility.browsewithcaret
   * - true enables keyboard navigation and selection within web pages using a
   *   visible caret, false uses normal keyboard navigation with no caret
   * accessibility.typeaheadfind
   * - when set to true, typing outside text areas and input boxes will
   *   automatically start searching for what's typed within the current
   *   document; when set to false, no search action happens
   * general.autoScroll
   * - when set to true, clicking the scroll wheel on the mouse activates a
   *   mouse mode where moving the mouse down scrolls the document downward with
   *   speed correlated with the distance of the cursor from the original
   *   position at which the click occurred (and likewise with movement upward);
   *   if false, this behavior is disabled
   * general.smoothScroll
   * - set to true to enable finer page scrolling than line-by-line on page-up,
   *   page-down, and other such page movements
   * layout.spellcheckDefault
   * - an integer:
   *     0  disables spellchecking
   *     1  enables spellchecking, but only for multiline text fields
   *     2  enables spellchecking for all text fields
   */

  /**
   * Stores the original value of the spellchecking preference to enable proper
   * restoration if unchanged (since we're mapping a tristate onto a checkbox).
   */
  _storedSpellCheck: 0,

  /**
   * Returns true if any spellchecking is enabled and false otherwise, caching
   * the current value to enable proper pref restoration if the checkbox is
   * never changed.
   */
  readCheckSpelling: function()
  {
    var pref = document.getElementById("layout.spellcheckDefault");
    this._storedSpellCheck = pref.value;

    return (pref.value != 0);
  },

  /**
   * Returns the value of the spellchecking preference represented by UI,
   * preserving the preference's "hidden" value if the preference is
   * unchanged and represents a value not strictly allowed in UI.
   */
  writeCheckSpelling: function()
  {
    var checkbox = document.getElementById("checkSpelling");
    return checkbox.checked ? (this._storedSpellCheck == 2 ? 2 : 1) : 0;
  },

  /**
   * security.OCSP.enabled is an integer value for legacy reasons.
   * A value of 1 means OCSP is enabled. Any other value means it is disabled.
   */
  readEnableOCSP: function()
  {
    var preference = document.getElementById("security.OCSP.enabled");
    // This is the case if the preference is the default value.
    if (preference.value === undefined) {
      return true;
    }
    return preference.value == 1;
  },

  /**
   * See documentation for readEnableOCSP.
   */
  writeEnableOCSP: function()
  {
    var checkbox = document.getElementById("enableOCSP");
    return checkbox.checked ? 1 : 0;
  },

  /**
   * When the user toggles the layers.acceleration.disabled pref,
   * sync its new value to the gfx.direct2d.disabled pref too.
   */
  updateHardwareAcceleration: function()
  {
#ifdef XP_WIN
    var fromPref = document.getElementById("layers.acceleration.enabled");
    var toPref = document.getElementById("gfx.direct2d.enabled");
    toPref.value = fromPref.value;
#endif
    this.updateHWADisplay();
  },

  updateHWADisplay: function()
  {
#ifdef XP_LINUX
    let HWA = document.getElementById("layers.acceleration.enabled");
    document.getElementById("forceHWAccel").disabled = !HWA.value;
#endif
  },  

  // DATA CHOICES TAB

  /**
   * opening links behind a modal dialog is poor form. Work around flawed text-link handling here.
   */
  openTextLink: function(evt) {
    let where = Services.prefs.getBoolPref("browser.preferences.instantApply") ? "tab" : "window";
    openUILinkIn(evt.target.getAttribute("href"), where);
    evt.preventDefault();
  },

  /**
   * Set up or hide the Learn More links for various data collection options
   */
  _setupLearnMoreLink: function(pref, element) {
    // set up the Learn More link with the correct URL
    let url = Services.prefs.getCharPref(pref);
    let el = document.getElementById(element);

    if (url) {
      el.setAttribute("href", url);
    } else {
      el.setAttribute("hidden", "true");
    }
  },

  // NETWORK TAB

  /*
   * Preferences:
   *
   * browser.cache.disk.capacity
   * - the size of the browser cache in KB
   * - Only used if browser.cache.disk.smart_size.enabled is disabled
   */

  /**
   * Displays a dialog in which proxy settings may be changed.
   */
  showConnections: function()
  {
    document.documentElement.openSubDialog("chrome://browser/content/preferences/connection.xul",
                                           "", null);
  },

  // Retrieves the amount of space currently used by disk cache
  updateActualCacheSize: function()
  {
    var sum = 0;
    function updateUI(consumption) {
      var actualSizeLabel = document.getElementById("actualDiskCacheSize");
      var sizeStrings = DownloadUtils.convertByteUnits(consumption);
      var prefStrBundle = document.getElementById("bundlePreferences");
      var sizeStr = prefStrBundle.getFormattedString("actualDiskCacheSize", sizeStrings);
      actualSizeLabel.value = sizeStr;
    }

    Visitor.prototype = {
      expected: 0,
      sum: 0,
      QueryInterface: function(iid) {
        if (iid.equals(Components.interfaces.nsISupports) ||
            iid.equals(Components.interfaces.nsICacheStorageVisitor)) {
          return this;
        }
        throw Components.results.NS_ERROR_NO_INTERFACE;
      },
      onCacheStorageInfo: function(num, consumption)
      {
        this.sum += consumption;
        if (!--this.expected)
          updateUI(this.sum);
      }
    };
    function Visitor(callbacksExpected) {
      this.expected = callbacksExpected;
    }

    var cacheService =
      Components.classes["@mozilla.org/netwerk/cache-storage-service;1"]
                .getService(Components.interfaces.nsICacheStorageService);
    // non-anonymous
    var storage1 = cacheService.diskCacheStorage(LoadContextInfo.default, false);
    // anonymous
    var storage2 = cacheService.diskCacheStorage(LoadContextInfo.anonymous, false);

    // expect 2 callbacks
    var visitor = new Visitor(2);
    storage1.asyncVisitStorage(visitor, false /* Do not walk entries */);
    storage2.asyncVisitStorage(visitor, false /* Do not walk entries */);
  },

  // Retrieves the amount of space currently used by offline cache
  updateActualAppCacheSize: function()
  {
    var visitor = {
      onCacheStorageInfo: function(aEntryCount, aConsumption, aCapacity, aDiskDirectory)
      {
        var actualSizeLabel = document.getElementById("actualAppCacheSize");
        var sizeStrings = DownloadUtils.convertByteUnits(aConsumption);
        var prefStrBundle = document.getElementById("bundlePreferences");
        var sizeStr = prefStrBundle.getFormattedString("actualAppCacheSize", sizeStrings);
        actualSizeLabel.value = sizeStr;
      }
    };

    var cacheService =
      Components.classes["@mozilla.org/netwerk/cache-storage-service;1"]
                .getService(Components.interfaces.nsICacheStorageService);
    var storage = cacheService.appCacheStorage(LoadContextInfo.default, null);
    try {
      storage.asyncVisitStorage(visitor, false);
    } catch(ex) {
      // Service unavailable: user most likely crippled the cache.
    }
  },

  updateCacheSizeUI: function(smartSizeEnabled)
  {
    document.getElementById("useCacheBefore").disabled = smartSizeEnabled;
    document.getElementById("cacheSize").disabled = smartSizeEnabled;
    document.getElementById("useCacheAfter").disabled = smartSizeEnabled;
  },

  readSmartSizeEnabled: function()
  {
    // The smart_size.enabled preference element is inverted="true", so its
    // value is the opposite of the actual pref value
    var disabled = document.getElementById("browser.cache.disk.smart_size.enabled").value;
    this.updateCacheSizeUI(!disabled);
  },

  /**
   * Converts the cache size from units of KB to units of MB and returns that
   * value.
   */
  readCacheSize: function()
  {
    var preference = document.getElementById("browser.cache.disk.capacity");
    return preference.value / 1024;
  },

  /**
   * Converts the cache size as specified in UI (in MB) to KB and returns that
   * value.
   */
  writeCacheSize: function()
  {
    var cacheSize = document.getElementById("cacheSize");
    var intValue = parseInt(cacheSize.value, 10);
    return isNaN(intValue) ? 0 : intValue * 1024;
  },

  /**
   * Clears the cache.
   */
  clearCache: function()
  {
    var cache = Components.classes["@mozilla.org/netwerk/cache-storage-service;1"]
                                 .getService(Components.interfaces.nsICacheStorageService);
    try {
      cache.clear();
    } catch(ex) {}
    this.updateActualCacheSize();
  },

  /**
   * Clears the application cache.
   */
  clearOfflineAppCache: function()
  {
    Components.utils.import("resource:///modules/offlineAppCache.jsm");
    OfflineAppCacheHelper.clear();

    this.updateActualAppCacheSize();
    this.updateOfflineApps();
  },

  updateOfflineAppsPermissions: function()
  {
    var permPref = document.getElementById("offline-apps.permissions");
    var allowPref = document.getElementById("offline-apps.allow_by_default");
    var notifyPref = document.getElementById("browser.offline-apps.notify");
    switch (permPref.value) {
      case 0: allowPref.value = false;
              notifyPref.value = false;
              break;
      case 1: allowPref.value = false;
              notifyPref.value = true;
              break;
      case 2: allowPref.value = true;
              notifyPref.value = true;
              break;
      default: console.error("Preference error: Invalid value ",permPref.value," for offline app permissions - resetting to default.");
               permPref.value = 2;
               allowPref.value = true;
               notifyPref.value = true;
    }
    // Set state of "Exceptions" button accordingly.
    var button = document.getElementById("offlineNotifyExceptions");
    button.disabled = !allowPref.value && !notifyPref.value;
  },
      
  showOfflineExceptions: function()
  {
    var bundlePreferences = document.getElementById("bundlePreferences");
    var params = { blockVisible     : false,
                   sessionVisible   : false,
                   allowVisible     : false,
                   prefilledHost    : "",
                   permissionType   : "offline-app",
                   manageCapability : Components.interfaces.nsIPermissionManager.DENY_ACTION,
                   windowTitle      : bundlePreferences.getString("offlinepermissionstitle"),
                   introText        : bundlePreferences.getString("offlinepermissionstext") };
    document.documentElement.openWindow("Browser:Permissions",
                                        "chrome://browser/content/preferences/permissions.xul",
                                        "", params);
  },

  // XXX: duplicated in browser.js
  _getOfflineAppUsage: function(perm, groups)
  {
    var cacheService = Components.classes["@mozilla.org/network/application-cache-service;1"].
                       getService(Components.interfaces.nsIApplicationCacheService);
    if (!groups) {
      try {
        groups = cacheService.getGroups();
      } catch(ex) {
        // Cache disabled.
        return 0;
      }
    }

    var ios = Components.classes["@mozilla.org/network/io-service;1"].
              getService(Components.interfaces.nsIIOService);

    var usage = 0;
    for (var i = 0; i < groups.length; i++) {
      var uri = ios.newURI(groups[i], null, null);
      if (perm.matchesURI(uri, true)) {
        var cache = cacheService.getActiveCache(groups[i]);
        usage += cache.usage;
      }
    }

    return usage;
  },

  /**
   * Updates the list of offline applications
   */
  updateOfflineApps: function()
  {
    var pm = Components.classes["@mozilla.org/permissionmanager;1"]
                       .getService(Components.interfaces.nsIPermissionManager);

    var list = document.getElementById("offlineAppsList");
    while (list.firstChild) {
      list.removeChild(list.firstChild);
    }

    var cacheService = Components.classes["@mozilla.org/network/application-cache-service;1"].
                       getService(Components.interfaces.nsIApplicationCacheService);
    
    try {
      var groups = cacheService.getGroups();

      var bundle = document.getElementById("bundlePreferences");

      var enumerator = pm.enumerator;
      while (enumerator.hasMoreElements()) {
        var perm = enumerator.getNext().QueryInterface(Components.interfaces.nsIPermission);
        if (perm.type == "offline-app" &&
            perm.capability != Components.interfaces.nsIPermissionManager.DEFAULT_ACTION &&
            perm.capability != Components.interfaces.nsIPermissionManager.DENY_ACTION) {
          var row = document.createElement("listitem");
          row.id = "";
          row.className = "offlineapp";
          row.setAttribute("origin", perm.principal.origin);
          var converted = DownloadUtils.
                          convertByteUnits(this._getOfflineAppUsage(perm, groups));
          row.setAttribute("usage",
                           bundle.getFormattedString("offlineAppUsage",
                                                     converted));
          list.appendChild(row);
        }
      }
    } catch(ex) {
        // Cache service unavailable/errored, off-line app cache is disabled or 0
        // Do nothing, just leave the box blank.
    }
  },

  offlineAppSelected: function()
  {
    var removeButton = document.getElementById("offlineAppsListRemove");
    var list = document.getElementById("offlineAppsList");
    if (list.selectedItem) {
      removeButton.setAttribute("disabled", "false");
    } else {
      removeButton.setAttribute("disabled", "true");
    }
  },

  removeOfflineApp: function()
  {
    var list = document.getElementById("offlineAppsList");
    var item = list.selectedItem;
    var origin = item.getAttribute("origin");
    var principal = Services.scriptSecurityManager.createCodebasePrincipalFromOrigin(origin);

    var prompts = Components.classes["@mozilla.org/embedcomp/prompt-service;1"]
                            .getService(Components.interfaces.nsIPromptService);
    var flags = prompts.BUTTON_TITLE_IS_STRING * prompts.BUTTON_POS_0 +
                prompts.BUTTON_TITLE_CANCEL * prompts.BUTTON_POS_1;

    var bundle = document.getElementById("bundlePreferences");
    var title = bundle.getString("offlineAppRemoveTitle");
    var prompt = bundle.getFormattedString("offlineAppRemovePrompt", [principal.URI.prePath]);
    var confirm = bundle.getString("offlineAppRemoveConfirm");
    var result = prompts.confirmEx(window, title, prompt, flags, confirm,
                                   null, null, null, {});
    if (result != 0)
      return;

    // get the permission
    var pm = Components.classes["@mozilla.org/permissionmanager;1"]
                       .getService(Components.interfaces.nsIPermissionManager);
    var perm = pm.getPermissionObject(principal, "offline-app", true);
    if (perm) {
      // clear offline cache entries
      try {
        var cacheService = Components.classes["@mozilla.org/network/application-cache-service;1"].
                           getService(Components.interfaces.nsIApplicationCacheService);
        var groups = cacheService.getGroups();
        for (var i = 0; i < groups.length; i++) {
          var uri = Services.io.newURI(groups[i], null, null);
          if (perm.matchesURI(uri, true)) {
            var cache = cacheService.getActiveCache(groups[i]);
            cache.discard();
          }
        }
      } catch (e) {}

      pm.removePermission(perm);
    }
    list.removeChild(item);
    gAdvancedPane.offlineAppSelected();
    this.updateActualAppCacheSize();
  },

  // UPDATE TAB

  /*
   * Preferences:
   *
   * app.update.enabled
   * - true if updates to the application are enabled, false otherwise
   * extensions.update.enabled
   * - true if updates to extensions and themes are enabled, false otherwise
   * browser.search.update
   * - true if updates to search engines are enabled, false otherwise
   * app.update.auto
   * - true if updates should be automatically downloaded and installed,
   *   possibly with a warning if incompatible extensions are installed (see
   *   app.update.mode); false if the user should be asked what he wants to do
   *   when an update is available
   * app.update.mode
   * - an integer:
   *     0    do not warn if an update will disable extensions or themes
   *     1    warn if an update will disable extensions or themes
   *     2    warn if an update will disable extensions or themes *or* if the
   *          update is a major update
   */

#ifdef MOZ_UPDATER
  /**
   * Selects the item of the radiogroup, and sets the warnIncompatible checkbox
   * based on the pref values and locked states.
   *
   * UI state matrix for update preference conditions
   *
   * UI Components:                              Preferences
   * Radiogroup                                  i   = app.update.enabled
   * Warn before disabling extensions checkbox   ii  = app.update.auto
   *                                             iii = app.update.mode
   *
   * Disabled states:
   * Element           pref  value  locked  disabled
   * radiogroup        i     t/f    f       false
   *                   i     t/f    *t*     *true*
   *                   ii    t/f    f       false
   *                   ii    t/f    *t*     *true*
   *                   iii   0/1/2  t/f     false
   * warnIncompatible  i     t      f       false
   *                   i     t      *t*     *true*
   *                   i     *f*    t/f     *true*
   *                   ii    t      f       false
   *                   ii    t      *t*     *true*
   *                   ii    *f*    t/f     *true*
   *                   iii   0/1/2  f       false
   *                   iii   0/1/2  *t*     *true*
   */
  updateReadPrefs: function()
  {
    var enabledPref = document.getElementById("app.update.enabled");
    var autoPref = document.getElementById("app.update.auto");
    var radiogroup = document.getElementById("updateRadioGroup");

    if (!enabledPref.value)   // Don't care for autoPref.value in this case.
      radiogroup.value="manual";    // 3. Never check for updates.
    else if (autoPref.value)  // enabledPref.value && autoPref.value
      radiogroup.value="auto";      // 1. Automatically install updates for Desktop only
    else                      // enabledPref.value && !autoPref.value
      radiogroup.value="checkOnly"; // 2. Check, but let me choose

    var canCheck = Components.classes["@mozilla.org/updates/update-service;1"].
                     getService(Components.interfaces.nsIApplicationUpdateService).
                     canCheckForUpdates;
    // canCheck is false if the enabledPref is false and locked,
    // or the binary platform or OS version is not known.
    // A locked pref is sufficient to disable the radiogroup.
    radiogroup.disabled = !canCheck || enabledPref.locked || autoPref.locked;

    var modePref = document.getElementById("app.update.mode");
    var warnIncompatible = document.getElementById("warnIncompatible");
    // the warnIncompatible checkbox value is set by readAddonWarn
    warnIncompatible.disabled = radiogroup.disabled || modePref.locked ||
                                !enabledPref.value || !autoPref.value;
  },

  /**
   * Sets the pref values based on the selected item of the radiogroup,
   * and sets the disabled state of the warnIncompatible checkbox accordingly.
   */
  updateWritePrefs: function()
  {
    var enabledPref = document.getElementById("app.update.enabled");
    var autoPref = document.getElementById("app.update.auto");
    var radiogroup = document.getElementById("updateRadioGroup");
    switch (radiogroup.value) {
      case "auto":      // 1. Automatically install updates for Desktop only
        enabledPref.value = true;
        autoPref.value = true;
        break;
      case "checkOnly": // 2. Check, but let me choose
        enabledPref.value = true;
        autoPref.value = false;
        break;
      case "manual":    // 3. Never check for updates.
        enabledPref.value = false;
        autoPref.value = false;
    }

    var warnIncompatible = document.getElementById("warnIncompatible");
    var modePref = document.getElementById("app.update.mode");
    warnIncompatible.disabled = enabledPref.locked || !enabledPref.value ||
                                autoPref.locked || !autoPref.value ||
                                modePref.locked;

  },

  /**
   * Stores the value of the app.update.mode preference, which is a tristate
   * integer preference.  We store the value here so that we can properly
   * restore the preference value if the UI reflecting the preference value
   * is in a state which can represent either of two integer values (as
   * opposed to only one possible value in the other UI state).
   */
  _modePreference: -1,

  /**
   * Reads the app.update.mode preference and converts its value into a
   * true/false value for use in determining whether the "Warn me if this will
   * disable extensions or themes" checkbox is checked.  We also save the value
   * of the preference so that the preference value can be properly restored if
   * the user's preferences cannot adequately be expressed by a single checkbox.
   *
   * app.update.mode          Checkbox State    Meaning
   * 0                        Unchecked         Do not warn
   * 1                        Checked           Warn if there are incompatibilities
   * 2                        Checked           Warn if there are incompatibilities,
   *                                            or the update is major.
   */
  readAddonWarn: function()
  {
    var preference = document.getElementById("app.update.mode");
    var warn = preference.value != 0;
    gAdvancedPane._modePreference = warn ? preference.value : 1;
    return warn;
  },

  /**
   * Converts the state of the "Warn me if this will disable extensions or
   * themes" checkbox into the integer preference which represents it,
   * returning that value.
   */
  writeAddonWarn: function()
  {
    var warnIncompatible = document.getElementById("warnIncompatible");
    return !warnIncompatible.checked ? 0 : gAdvancedPane._modePreference;
  },

  /**
   * Displays the history of installed updates.
   */
  showUpdates: function()
  {
    var prompter = Components.classes["@mozilla.org/updates/update-prompt;1"]
                             .createInstance(Components.interfaces.nsIUpdatePrompt);
    prompter.showUpdateHistory(window);
  },
#endif

  // CERTIFICATES TAB

  /*
   * Preferences:
   *
   * security.default_personal_cert
   * - a string:
   *     "Select Automatically"   select a certificate automatically when a site
   *                              requests one
   *     "Ask Every Time"         present a dialog to the user so he can select
   *                              the certificate to use on a site which
   *                              requests one
   */

  /**
   * Displays the user's certificates and associated options.
   */
  showCertificates: function()
  {
    document.documentElement.openWindow("mozilla:certmanager",
                                        "chrome://pippki/content/certManager.xul",
                                        "", null);
  },

  /**
   * Displays a dialog from which the user can manage his security devices.
   */
  showSecurityDevices: function()
  {
    document.documentElement.openWindow("mozilla:devicemanager",
                                        "chrome://pippki/content/device_manager.xul",
                                        "", null);
  }
#ifdef HAVE_SHELL_SERVICE
  ,

  // SYSTEM DEFAULTS

  /*
   * Preferences:
   *
   * browser.shell.checkDefault
   * - true if a default-browser check (and prompt to make it so if necessary)
   *   occurs at startup, false otherwise
   */

  /**
   * Show button for setting browser as default browser or information that
   * browser is already the default browser.
   */
  updateSetDefaultBrowser: function()
  {
    let shellSvc = getShellService();
    let setDefaultPane = document.getElementById("setDefaultPane");
    if (!shellSvc) {
      setDefaultPane.hidden = true;
      document.getElementById("alwaysCheckDefault").disabled = true;
      return;
    }
    let selectedIndex =
      shellSvc.isDefaultBrowser(false, true) ? 1 : 0;
    setDefaultPane.selectedIndex = selectedIndex;
  },

  /**
   * Set browser as the operating system default browser.
   */
  setDefaultBrowser: function()
  {
    let shellSvc = getShellService();
    if (!shellSvc)
      return;
    try {
      let claimAllTypes = true;
#ifdef XP_WIN
      // In Windows 8+, the UI for selecting default protocol is much
      // nicer than the UI for setting file type associations. So we
      // only show the protocol association screen on Windows 8+.
      // Windows 8 is version 6.2.
      let version = Services.sysinfo.getProperty("version");
      claimAllTypes = (parseFloat(version) < 6.2);
#endif
      shellSvc.setDefaultBrowser(claimAllTypes, false);
    } catch (ex) {
      Components.utils.reportError(ex);
      return;
    }
    let selectedIndex =
      shellSvc.isDefaultBrowser(false, true) ? 1 : 0;
    document.getElementById("setDefaultPane").selectedIndex = selectedIndex;
  }
#endif
};
