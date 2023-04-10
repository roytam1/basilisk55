/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

this.EXPORTED_SYMBOLS = [
  "DownloadsCommon",
];

/**
 * Handles the Downloads panel shared methods and data access.
 *
 * This file includes the following constructors and global objects:
 *
 * DownloadsCommon
 * This object is exposed directly to the consumers of this JavaScript module,
 * and provides shared methods for all the instances of the user interface.
 *
 * DownloadsData
 * Retrieves the list of past and completed downloads from the underlying
 * Downloads API data, and provides asynchronous notifications allowing
 * to build a consistent view of the available data.
 *
 * DownloadsIndicatorData
 * This object registers itself with DownloadsData as a view, and transforms the
 * notifications it receives into overall status data, that is then broadcast to
 * the registered download status indicators.
 */

////////////////////////////////////////////////////////////////////////////////
//// Globals

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;
const Cr = Components.results;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "NetUtil",
                                  "resource://gre/modules/NetUtil.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "PluralForm",
                                  "resource://gre/modules/PluralForm.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "Downloads",
                                  "resource://gre/modules/Downloads.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "DownloadUIHelper",
                                  "resource://gre/modules/DownloadUIHelper.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "DownloadUtils",
                                  "resource://gre/modules/DownloadUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "FileUtils",
                                  "resource://gre/modules/FileUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "OS",
                                  "resource://gre/modules/osfile.jsm")
XPCOMUtils.defineLazyModuleGetter(this, "PlacesUtils",
                                  "resource://gre/modules/PlacesUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "PrivateBrowsingUtils",
                                  "resource://gre/modules/PrivateBrowsingUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "RecentWindow",
                                  "resource:///modules/RecentWindow.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "PlacesUtils",
                                  "resource://gre/modules/PlacesUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "Promise",
                                  "resource://gre/modules/Promise.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "DownloadsLogger",
                                  "resource:///modules/DownloadsLogger.jsm");

const nsIDM = Ci.nsIDownloadManager;

const kDownloadsStringBundleUrl =
  "chrome://browser/locale/downloads/downloads.properties";

const kPrefConfirmOpenExe = "browser.download.confirmOpenExecutable";

const kDownloadsStringsRequiringFormatting = {
  sizeWithUnits: true,
  shortTimeLeftSeconds: true,
  shortTimeLeftMinutes: true,
  shortTimeLeftHours: true,
  shortTimeLeftDays: true,
  statusSeparator: true,
  statusSeparatorBeforeNumber: true,
  fileExecutableSecurityWarning: true
};

const kDownloadsStringsRequiringPluralForm = {
  otherDownloads2: true
};

const kPartialDownloadSuffix = ".part";

const kPrefBranch = Services.prefs.getBranch("browser.download.");

var PrefObserver = {
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIObserver,
                                         Ci.nsISupportsWeakReference]),
  getPref: function(name) {
    try {
      switch (typeof this.prefs[name]) {
        case "boolean":
          return kPrefBranch.getBoolPref(name);
      }
    } catch (ex) { }
    return this.prefs[name];
  },
  observe: function(aSubject, aTopic, aData) {
    if (this.prefs.hasOwnProperty(aData)) {
      return this[aData] = this.getPref(aData);
    }
  },
  register: function(prefs) {
    this.prefs = prefs;
    kPrefBranch.addObserver("", this, true);
    for (let key in prefs) {
      let name = key;
      XPCOMUtils.defineLazyGetter(this, name, function() {
        return PrefObserver.getPref(name);
      });
    }
  },
};

PrefObserver.register({
  // prefName: defaultValue
  debug: false,
  animateNotifications: true
});


////////////////////////////////////////////////////////////////////////////////
//// DownloadsCommon

/**
 * This object is exposed directly to the consumers of this JavaScript module,
 * and provides shared methods for all the instances of the user interface.
 */
this.DownloadsCommon = {
  log: function(...aMessageArgs) {
    delete this.log;
    this.log = function(...aMessageArgs) {
      if (!PrefObserver.debug) {
        return;
      }
      DownloadsLogger.log.apply(DownloadsLogger, aMessageArgs);
    }
    this.log.apply(this, aMessageArgs);
  },

  error: function(...aMessageArgs) {
    delete this.error;
    this.error = function(...aMessageArgs) {
      if (!PrefObserver.debug) {
        return;
      }
      DownloadsLogger.reportError.apply(DownloadsLogger, aMessageArgs);
    }
    this.error.apply(this, aMessageArgs);
  },
  /**
   * Returns an object whose keys are the string names from the downloads string
   * bundle, and whose values are either the translated strings or functions
   * returning formatted strings.
   */
  get strings()
  {
    let strings = {};
    let sb = Services.strings.createBundle(kDownloadsStringBundleUrl);
    let enumerator = sb.getSimpleEnumeration();
    while (enumerator.hasMoreElements()) {
      let string = enumerator.getNext().QueryInterface(Ci.nsIPropertyElement);
      let stringName = string.key;
      if (stringName in kDownloadsStringsRequiringFormatting) {
        strings[stringName] = function() {
          // Convert "arguments" to a real array before calling into XPCOM.
          return sb.formatStringFromName(stringName,
                                         Array.slice(arguments, 0),
                                         arguments.length);
        };
      } else if (stringName in kDownloadsStringsRequiringPluralForm) {
        strings[stringName] = function(aCount) {
          // Convert "arguments" to a real array before calling into XPCOM.
          let formattedString = sb.formatStringFromName(stringName,
                                         Array.slice(arguments, 0),
                                         arguments.length);
          return PluralForm.get(aCount, formattedString);
        };
      } else {
        strings[stringName] = string.value;
      }
    }
    delete this.strings;
    return this.strings = strings;
  },

  /**
   * Generates a very short string representing the given time left.
   *
   * @param aSeconds
   *        Value to be formatted.  It represents the number of seconds, it must
   *        be positive but does not need to be an integer.
   *
   * @return Formatted string, for example "30s" or "2h".  The returned value is
   *         maximum three characters long, at least in English.
   */
  formatTimeLeft: function(aSeconds)
  {
    // Decide what text to show for the time
    let seconds = Math.round(aSeconds);
    if (!seconds) {
      return "";
    } else if (seconds <= 30) {
      return DownloadsCommon.strings["shortTimeLeftSeconds"](seconds);
    }
    let minutes = Math.round(aSeconds / 60);
    if (minutes < 60) {
      return DownloadsCommon.strings["shortTimeLeftMinutes"](minutes);
    }
    let hours = Math.round(minutes / 60);
    if (hours < 48) { // two days
      return DownloadsCommon.strings["shortTimeLeftHours"](hours);
    }
    let days = Math.round(hours / 24);
    return DownloadsCommon.strings["shortTimeLeftDays"](Math.min(days, 99));
  },

  /**
   * Indicates whether we should show the full Download Manager window interface
   * instead of the simplified panel interface.  The behavior of downloads
   * across browsing session is consistent with the selected interface.
   */
  get useToolkitUI()
  {
    /* Toolkit UI is currently incompatible.
     * FIXME: Either fix the toolkitUI (make DBConnection work) or remove
     * the unused code altogether
     */
    //try {
    //  return Services.prefs.getBoolPref("browser.download.useToolkitUI");
    //} catch (ex) { }
    return false;
  },

  /**
   * Indicates whether we should show visual notification on the indicator
   * when a download event is triggered.
   */
  get animateNotifications()
  {
    return PrefObserver.animateNotifications;
  },

  /**
   * Get access to one of the DownloadsData or PrivateDownloadsData objects,
   * depending on the privacy status of the window in question.
   *
   * @param aWindow
   *        The browser window which owns the download button.
   */
  getData: function(aWindow) {
    if (PrivateBrowsingUtils.isWindowPrivate(aWindow)) {
      return PrivateDownloadsData;
    } else {
      return DownloadsData;
    }
  },

  /**
   * Initializes the data link for both the private and non-private downloads
   * data objects.
   *
   * @param aDownloadManagerService
   *        Reference to the service implementing nsIDownloadManager.  We need
   *        this because getService isn't available for us when this method is
   *        called, and we must ensure to register our listeners before the
   *        getService call for the Download Manager returns.
   */
  initializeAllDataLinks: function(aDownloadManagerService) {
    DownloadsData.initializeDataLink(aDownloadManagerService);
    PrivateDownloadsData.initializeDataLink(aDownloadManagerService);
  },

  /**
   * Terminates the data link for both the private and non-private downloads
   * data objects.
   */
  terminateAllDataLinks: function() {
    DownloadsData.terminateDataLink();
    PrivateDownloadsData.terminateDataLink();
  },

  /**
   * Reloads the specified kind of downloads from the non-private store.
   * This method must only be called when Private Browsing Mode is disabled.
   *
   * @param aActiveOnly
   *        True to load only active downloads from the database.
   */
  ensureAllPersistentDataLoaded:
  function(aActiveOnly) {
    DownloadsData.ensurePersistentDataLoaded(aActiveOnly);
  },

  /**
   * Get access to one of the DownloadsIndicatorData or
   * PrivateDownloadsIndicatorData objects, depending on the privacy status of
   * the window in question.
   */
  getIndicatorData: function(aWindow) {
    if (PrivateBrowsingUtils.isWindowPrivate(aWindow)) {
      return PrivateDownloadsIndicatorData;
    } else {
      return DownloadsIndicatorData;
    }
  },

  /**
   * Returns a reference to the DownloadsSummaryData singleton - creating one
   * in the process if one hasn't been instantiated yet.
   *
   * @param aWindow
   *        The browser window which owns the download button.
   * @param aNumToExclude
   *        The number of items on the top of the downloads list to exclude
   *        from the summary.
   */
  getSummary: function(aWindow, aNumToExclude)
  {
    if (PrivateBrowsingUtils.isWindowPrivate(aWindow)) {
      if (this._privateSummary) {
        return this._privateSummary;
      }
      return this._privateSummary = new DownloadsSummaryData(true, aNumToExclude);
    } else {
      if (this._summary) {
        return this._summary;
      }
      return this._summary = new DownloadsSummaryData(false, aNumToExclude);
    }
  },
  _summary: null,
  _privateSummary: null,

  /**
   * Returns the legacy state integer value for the provided Download object.
   */
  stateOfDownload(download) {
    // Collapse state using the correct priority.
    if (!download.stopped) {
      return nsIDM.DOWNLOAD_DOWNLOADING;
    }
    if (download.succeeded) {
      return nsIDM.DOWNLOAD_FINISHED;
    }
    if (download.error) {
      if (download.error.becauseBlockedByParentalControls) {
        return nsIDM.DOWNLOAD_BLOCKED_PARENTAL;
      }
      if (download.error.becauseBlockedByReputationCheck) {
        return nsIDM.DOWNLOAD_DIRTY;
      }
      return nsIDM.DOWNLOAD_FAILED;
    }
    if (download.canceled) {
      if (download.hasPartialData) {
        return nsIDM.DOWNLOAD_PAUSED;
      }
      return nsIDM.DOWNLOAD_CANCELED;
    }
    return nsIDM.DOWNLOAD_NOTSTARTED;
  },

  /**
   * Helper function required because the Downloads Panel and the Downloads View
   * don't share the controller yet.
   */
  removeAndFinalizeDownload(download) {
    Downloads.getList(Downloads.ALL)
             .then(list => list.remove(download))
             .then(() => download.finalize(true))
             .catch(Cu.reportError);
  },

  /**
   * Given an iterable collection of Download objects, generates and returns
   * statistics about that collection.
   *
   * @param downloads An iterable collection of Download objects.
   *
   * @return Object whose properties are the generated statistics. Currently,
   *         we return the following properties:
   *
   *         numActive       : The total number of downloads.
   *         numPaused       : The total number of paused downloads.
   *         numDownloading  : The total number of downloads being downloaded.
   *         totalSize       : The total size of all downloads once completed.
   *         totalTransferred: The total amount of transferred data for these
   *                           downloads.
   *         slowestSpeed    : The slowest download rate.
   *         rawTimeLeft     : The estimated time left for the downloads to
   *                           complete.
   *         percentComplete : The percentage of bytes successfully downloaded.
   */
  summarizeDownloads(downloads) {
    let summary = {
      numActive: 0,
      numPaused: 0,
      numDownloading: 0,
      totalSize: 0,
      totalTransferred: 0,
      // slowestSpeed is Infinity so that we can use Math.min to
      // find the slowest speed. We'll set this to 0 afterwards if
      // it's still at Infinity by the time we're done iterating all
      // download.
      slowestSpeed: Infinity,
      rawTimeLeft: -1,
      percentComplete: -1
    }

    for (let download of downloads) {
      summary.numActive++;

      if (!download.stopped) {
        summary.numDownloading++;
        if (download.hasProgress && download.speed > 0) {
          let sizeLeft = download.totalBytes - download.currentBytes;
          summary.rawTimeLeft = Math.max(summary.rawTimeLeft,
                                         sizeLeft / download.speed);
          summary.slowestSpeed = Math.min(summary.slowestSpeed,
                                          download.speed);
        }
      } else if (download.canceled && download.hasPartialData) {
        summary.numPaused++;
      }
      // Only add to total values if we actually know the download size.
      if (download.succeeded) {
        summary.totalSize += download.target.size;
        summary.totalTransferred += download.target.size;
      } else if (download.hasProgress) {
        summary.totalSize += download.totalBytes;
        summary.totalTransferred += download.currentBytes;
      }
    }

    if (summary.totalSize != 0) {
      summary.percentComplete = (summary.totalTransferred /
                                 summary.totalSize) * 100;
    }

    if (summary.slowestSpeed == Infinity) {
      summary.slowestSpeed = 0;
    }

    return summary;
  },

  /**
   * If necessary, smooths the estimated number of seconds remaining for one
   * or more downloads to complete.
   *
   * @param aSeconds
   *        Current raw estimate on number of seconds left for one or more
   *        downloads. This is a floating point value to help get sub-second
   *        accuracy for current and future estimates.
   */
  smoothSeconds: function(aSeconds, aLastSeconds)
  {
    // We apply an algorithm similar to the DownloadUtils.getTimeLeft function,
    // though tailored to a single time estimation for all downloads.  We never
    // apply something if the new value is less than half the previous value.
    let shouldApplySmoothing = aLastSeconds >= 0 &&
                               aSeconds > aLastSeconds / 2;
    if (shouldApplySmoothing) {
      // Apply hysteresis to favor downward over upward swings.  Trust only 30%
      // of the new value if lower, and 10% if higher (exponential smoothing).
      let diff = aSeconds - aLastSeconds;
      aSeconds = aLastSeconds + (diff < 0 ? .3 : .1) * diff;
      
      // If the new time is similar, reuse something close to the last time
      // left, but subtract a little to provide forward progress.
      diff = aSeconds - aLastSeconds;
      let diffPercent = diff / aLastSeconds * 100;
      if (Math.abs(diff) < 5 || Math.abs(diffPercent) < 5) {
        aSeconds = aLastSeconds - (diff < 0 ? .4 : .2);
      }
    }

    // In the last few seconds of downloading, we are always subtracting and
    // never adding to the time left.  Ensure that we never fall below one
    // second left until all downloads are actually finished.
    return aLastSeconds = Math.max(aSeconds, 1);
  },

  /**
   * Opens a downloaded file.
   *
   * @param aFile
   *        the downloaded file to be opened.
   * @param aMimeInfo
   *        the mime type info object.  May be null.
   * @param aOwnerWindow
   *        the window with which this action is associated.
   */
  openDownloadedFile: function(aFile, aMimeInfo, aOwnerWindow) {
    if (!(aFile instanceof Ci.nsIFile))
      throw new Error("aFile must be a nsIFile object");
    if (aMimeInfo && !(aMimeInfo instanceof Ci.nsIMIMEInfo))
      throw new Error("Invalid value passed for aMimeInfo");
    if (!(aOwnerWindow instanceof Ci.nsIDOMWindow))
      throw new Error("aOwnerWindow must be a dom-window object");

#ifdef XP_WIN
    // On Windows, the system will provide a native confirmation prompt
    // for .exe files. Exclude this from our prompt, but prompt on other
    // executable types.
    let isWindowsExe = aFile.leafName.toLowerCase().endsWith(".exe");
#else
    let isWindowsExe = false;
#endif

    // Confirm opening executable files if required.
    if (aFile.isExecutable() && !isWindowsExe) {
      let showAlert = true;
      try {
        showAlert = Services.prefs.getBoolPref(kPrefConfirmOpenExe);
      } catch (ex) {
        // If the preference does not exist, continue with the prompt.
      }

      if (showAlert) {
        let name = aFile.leafName;
        let message =
          DownloadsCommon.strings.fileExecutableSecurityWarning(name, name);
        let title =
          DownloadsCommon.strings.fileExecutableSecurityWarningTitle;

        let open = Services.prompt.confirm(aOwnerWindow, title, message);
        if (!open) {
          return;
        }
      }
    }

    // Actually open the file.
    try {
      if (aMimeInfo && aMimeInfo.preferredAction == aMimeInfo.useHelperApp) {
        aMimeInfo.launchWithFile(aFile);
        return;
      }
    }
    catch(ex) { }

    // If either we don't have the mime info, or the preferred action failed,
    // attempt to launch the file directly.
    try {
      aFile.launch();
    }
    catch(ex) {
      // If launch fails, try sending it through the system's external "file:"
      // URL handler.
      Cc["@mozilla.org/uriloader/external-protocol-service;1"]
        .getService(Ci.nsIExternalProtocolService)
        .loadUrl(NetUtil.newURI(aFile));
    }
  },

  /**
   * Show a downloaded file in the system file manager.
   *
   * @param aFile
   *        a downloaded file.
   */
  showDownloadedFile: function(aFile) {
    if (!(aFile instanceof Ci.nsIFile))
      throw new Error("aFile must be a nsIFile object");
    try {
      // Show the directory containing the file and select the file.
      aFile.reveal();
    } catch (ex) {
      // If reveal fails for some reason (e.g., it's not implemented on unix
      // or the file doesn't exist), try using the parent if we have it.
      let parent = aFile.parent;
      if (parent) {
        try {
          // Open the parent directory to show where the file should be.
          parent.launch();
        } catch (ex) {
          // If launch also fails (probably because it's not implemented), let
          // the OS handler try to open the parent.
          Cc["@mozilla.org/uriloader/external-protocol-service;1"]
            .getService(Ci.nsIExternalProtocolService)
            .loadUrl(NetUtil.newURI(parent));
        }
      }
    }
  }
};

/**
 * Returns true if we are executing on Windows Vista or a later version.
 */
XPCOMUtils.defineLazyGetter(DownloadsCommon, "isWinVistaOrHigher", function() {
  let os = Cc["@mozilla.org/xre/app-info;1"].getService(Ci.nsIXULRuntime).OS;
  if (os != "WINNT") {
    return false;
  }
  let sysInfo = Cc["@mozilla.org/system-info;1"].getService(Ci.nsIPropertyBag2);
  return parseFloat(sysInfo.getProperty("version")) >= 6;
});

/**
 * Returns true to indicate that we should hook the panel to the JavaScript API
 * for downloads instead of the nsIDownloadManager back-end.
 * This is kept for compatibility/leftovers and should be removed later.
 */
XPCOMUtils.defineLazyGetter(DownloadsCommon, "useJSTransfer", function() {
  return true;
});

////////////////////////////////////////////////////////////////////////////////
//// DownloadsData

/**
 * Retrieves the list of past and completed downloads from the underlying
 * Download Manager data, and provides asynchronous notifications allowing to
 * build a consistent view of the available data.
 *
 * This object responds to real-time changes in the underlying Download Manager
 * data.  For example, the deletion of one or more downloads is notified through
 * the nsIObserver interface, while any state or progress change is notified
 * through the nsIDownloadProgressListener interface.
 *
 * Note that using this object does not automatically start the Download Manager
 * service.  Consumers will see an empty list of downloads until the service is
 * actually started.  This is useful to display a neutral progress indicator in
 * the main browser window until the autostart timeout elapses.
 *
 * Note that DownloadsData and PrivateDownloadsData are two equivalent singleton
 * objects, one accessing non-private downloads, and the other accessing private
 * ones.
 */
function DownloadsDataCtor(aPrivate) {
  this._isPrivate = aPrivate;

  // Contains all the available Download objects and their integer state.
  this.oldDownloadStates = new Map();

  // Array of view objects that should be notified when the available download
  // data changes.
  this._views = [];
}

DownloadsDataCtor.prototype = {
  /**
   * Starts receiving events for current downloads.
   */
  initializeDataLink() {
    if (!this._dataLinkInitialized) {
      let promiseList = Downloads.getList(this._isPrivate ? Downloads.PRIVATE
                                                          : Downloads.PUBLIC);
      promiseList.then(list => list.addView(this)).then(null, Cu.reportError);
      this._dataLinkInitialized = true;
    }
  },
  _dataLinkInitialized: false,

  /**
   * Stops receiving events for current downloads and cancels any pending read.
   */
  terminateDataLink: function()
  {
    Cu.reportError("terminateDataLink not applicable with JS Transfers");
    return;
  },

  /**
   * Iterator for all the available Download objects. This is empty until the
   * data has been loaded using the JavaScript API for downloads.
   */
  get downloads() this.oldDownloadStates.keys(),

  /**
   * True if there are finished downloads that can be removed from the list.
   */
  get canRemoveFinished()
  {
    for (let download of this.downloads) {
      // Stopped, paused, and failed downloads with partial data are removed.
      if (download.stopped && !(download.canceled && download.hasPartialData)) {
        return true;
      }
    }
    return false;
  },

  /**
   * Asks the back-end to remove finished downloads from the list.
   */
  removeFinished: function()
  {
    let promiseList = Downloads.getList(this._isPrivate ? Downloads.PRIVATE
                                                        : Downloads.PUBLIC);
    promiseList.then(list => list.removeFinished())
               .then(null, Cu.reportError);
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Integration with the asynchronous Downloads back-end

  onDownloadAdded(download) {
    // Download objects do not store the end time of downloads, as the Downloads
    // API does not need to persist this information for all platforms. Once a
    // download terminates on a Desktop browser, it becomes a history download,
    // for which the end time is stored differently, as a Places annotation.
    download.endTime = Date.now();

    this.oldDownloadStates.set(download,
                               DownloadsCommon.stateOfDownload(download));

    for (let view of this._views) {
      view.onDownloadAdded(download, true);
    }
  },

  onDownloadChanged(download) {
    let oldState = this.oldDownloadStates.get(download);
    let newState = DownloadsCommon.stateOfDownload(download);
    this.oldDownloadStates.set(download, newState);

    if (oldState != newState) {
      if (download.succeeded ||
          (download.canceled && !download.hasPartialData) ||
          download.error) {
        // Store the end time that may be displayed by the views.
        download.endTime = Date.now();

        // This state transition code should actually be located in a Downloads
        // API module (bug 941009).  Moreover, the fact that state is stored as
        // annotations should be ideally hidden behind methods of
        // nsIDownloadHistory (bug 830415).
        if (!this._isPrivate) {
          try {
            let downloadMetaData = {
              state: DownloadsCommon.stateOfDownload(download),
              endTime: download.endTime,
            };
            if (download.succeeded) {
              downloadMetaData.fileSize = download.target.size;
            }
  
            PlacesUtils.annotations.setPageAnnotation(
                          NetUtil.newURI(download.source.url),
                          "downloads/metaData",
                          JSON.stringify(downloadMetaData), 0,
                          PlacesUtils.annotations.EXPIRE_WITH_HISTORY);
          } catch (ex) {
            Cu.reportError(ex);
          }
        }
      }

      for (let view of this._views) {
        try {
          view.onDownloadStateChanged(download);
        } catch (ex) {
          Cu.reportError(ex);
        }
      }

      if (download.succeeded ||
          (download.error && download.error.becauseBlocked)) {
        this._notifyDownloadEvent("finish");
      }
    }

    if (!download.newDownloadNotified) {
      download.newDownloadNotified = true;
      this._notifyDownloadEvent("start");
    }

    for (let view of this._views) {
      view.onDownloadChanged(download);
    }
  },

  onDownloadRemoved(download) {
    this.oldDownloadStates.delete(download);

    for (let view of this._views) {
      view.onDownloadRemoved(download);
    }
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Registration of views

  /**
   * Adds an object to be notified when the available download data changes.
   * The specified object is initialized with the currently available downloads.
   *
   * @param aView
   *        DownloadsView object to be added.  This reference must be passed to
   *        removeView before termination.
   */
  addView: function(aView)
  {
    this._views.push(aView);
    this._updateView(aView);
  },

  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        DownloadsView object to be removed.
   */
  removeView: function(aView)
  {
    let index = this._views.indexOf(aView);
    if (index != -1) {
      this._views.splice(index, 1);
    }
  },

  /**
   * Ensures that the currently loaded data is added to the specified view.
   *
   * @param aView
   *        DownloadsView object to be initialized.
   */
  _updateView: function(aView)
  {
    // Indicate to the view that a batch loading operation is in progress.
    aView.onDataLoadStarting();

    // Sort backwards by start time, ensuring that the most recent
    // downloads are added first regardless of their state.
    // Tycho:
    //let loadedItemsArray = [dataItem
    //                        for each (dataItem in this.dataItems)
    //                        if (dataItem)];
    let downloadsArray = [...this.downloads];
    downloadsArray.sort((a, b) => b.startTime - a.startTime);
    downloadsArray.forEach(download => aView.onDownloadAdded(download, false));

    // Notify the view that all data is available unless loading is in progress.
    if (!this._pendingStatement) {
      aView.onDataLoadCompleted();
    }
  },

  //////////////////////////////////////////////////////////////////////////////
  //// In-memory downloads data store

  /**
   * Clears the loaded data.
   */
  clear: function()
  {
    this._terminateDataAccess();
    this.dataItems = {};
  },

  /**
   * Returns the data item associated with the provided source object.  The
   * source can be a download object that we received from the Download Manager
   * because of a real-time notification, or a row from the downloads database,
   * during the asynchronous data load.
   *
   * In case we receive download status notifications while we are still
   * populating the list of downloads from the database, we want the real-time
   * status to take precedence over the state that is read from the database,
   * which might be older.  This is achieved by creating the download item if
   * it's not already in the list, but never updating the returned object using
   * the data from the database, if the object already exists.
   *
   * @param aSource
   *        Object containing the data with which the item should be initialized
   *        if it doesn't already exist in the list.  This should implement
   *        either nsIDownload or mozIStorageRow.  If the item exists, this
   *        argument is only used to retrieve the download identifier.
   * @param aMayReuseGUID
   *        If false, indicates that the download should not be added if a
   *        download with the same identifier was removed in the meantime.  This
   *        ensures that, while loading the list asynchronously, downloads that
   *        have been removed in the meantime do no reappear inadvertently.
   *
   * @return New or existing data item, or null if the item was deleted from the
   *         list of available downloads.
   */
  _getOrAddDataItem: function(aSource, aMayReuseGUID)
  {
    let downloadGuid = (aSource instanceof Ci.nsIDownload)
                       ? aSource.guid
                       : aSource.getResultByName("guid");
    if (downloadGuid in this.dataItems) {
      let existingItem = this.dataItems[downloadGuid];
      if (existingItem || !aMayReuseGUID) {
        // Returns null if the download was removed and we can't reuse the item.
        return existingItem;
      }
    }
    DownloadsCommon.log("Creating a new DownloadsDataItem with downloadGuid =",
                        downloadGuid);
    let dataItem = new DownloadsDataItem(aSource);
    this.dataItems[downloadGuid] = dataItem;

    // Create the view items before returning.
    let addToStartOfList = aSource instanceof Ci.nsIDownload;
    this._views.forEach(
      function(view) view.onDataItemAdded(dataItem, addToStartOfList)
    );
    return dataItem;
  },

  /**
   * Removes the data item with the specified identifier.
   *
   * This method can be called at most once per download identifier.
   */
  _removeDataItem: function(aDownloadId)
  {
    if (aDownloadId in this.dataItems) {
      let dataItem = this.dataItems[aDownloadId];
      this.dataItems[aDownloadId] = null;
      this._views.forEach(
        function(view) view.onDataItemRemoved(dataItem)
      );
    }
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Persistent data loading

  /**
   * Represents an executing statement, allowing its cancellation.
   */
  _pendingStatement: null,

  /**
   * Indicates which kind of items from the persistent downloads database have
   * been fully loaded in memory and are available to the views.  This can
   * assume the value of one of the kLoad constants.
   */
  _loadState: 0,

  /** No downloads have been fully loaded yet. */
  get kLoadNone() 0,
  /** All the active downloads in the database are loaded in memory. */
  get kLoadActive() 1,
  /** All the downloads in the database are loaded in memory. */
  get kLoadAll() 2,

  /**
   * Reloads the specified kind of downloads from the persistent database.  This
   * method must only be called when Private Browsing Mode is disabled.
   *
   * @param aActiveOnly
   *        True to load only active downloads from the database.
   */
  ensurePersistentDataLoaded:
  function(aActiveOnly)
  {
    if (this == PrivateDownloadsData) {
      Cu.reportError("ensurePersistentDataLoaded should not be called on PrivateDownloadsData");
      return;
    }

    if (this._pendingStatement) {
      // We are already in the process of reloading all downloads.
      return;
    }

    if (aActiveOnly) {
      if (this._loadState == this.kLoadNone) {
        DownloadsCommon.log("Loading only active downloads from the persistence database");
        // Indicate to the views that a batch loading operation is in progress.
        this._views.forEach(
          function(view) view.onDataLoadStarting()
        );

        // Reload the list using the Download Manager service.  The list is
        // returned in no particular order.
        let downloads = Services.downloads.activeDownloads;
        while (downloads.hasMoreElements()) {
          let download = downloads.getNext().QueryInterface(Ci.nsIDownload);
          this._getOrAddDataItem(download, true);
        }
        this._loadState = this.kLoadActive;

        // Indicate to the views that the batch loading operation is complete.
        this._views.forEach(
          function(view) view.onDataLoadCompleted()
        );
        DownloadsCommon.log("Active downloads done loading.");
      }
    } else {
      if (this._loadState != this.kLoadAll) {
        // Load only the relevant columns from the downloads database.  The
        // columns are read in the _initFromDataRow method of DownloadsDataItem.
        // Order by descending download identifier so that the most recent
        // downloads are notified first to the listening views.
        DownloadsCommon.log("Loading all downloads from the persistence database.");
        let dbConnection = Services.downloads.DBConnection;
        let statement = dbConnection.createAsyncStatement(
          "SELECT guid, target, name, source, referrer, state, "
        +        "startTime, endTime, currBytes, maxBytes "
        + "FROM moz_downloads "
        + "ORDER BY startTime DESC"
        );
        try {
          this._pendingStatement = statement.executeAsync(this);
        } finally {
          statement.finalize();
        }
      }
    }
  },

  /**
   * Cancels any pending data access and ensures views are notified.
   */
  _terminateDataAccess: function()
  {
    if (this._pendingStatement) {
      this._pendingStatement.cancel();
      this._pendingStatement = null;
    }

    // Close all the views on the current data.  Create a copy of the array
    // because some views might unregister while processing this event.
    Array.slice(this._views, 0).forEach(
      function(view) view.onDataInvalidated()
    );
  },

  //////////////////////////////////////////////////////////////////////////////
  //// mozIStorageStatementCallback

  handleResult: function(aResultSet)
  {
    for (let row = aResultSet.getNextRow();
         row;
         row = aResultSet.getNextRow()) {
      // Add the download to the list and initialize it with the data we read,
      // unless we already received a notification providing more reliable
      // information for this download.
      this._getOrAddDataItem(row, false);
    }
  },

  handleError: function(aError)
  {
    DownloadsCommon.error("Database statement execution error (",
                          aError.result, "): ", aError.message);
  },

  handleCompletion: function(aReason)
  {
    DownloadsCommon.log("Loading all downloads from database completed with reason:",
                        aReason);
    this._pendingStatement = null;

    // To ensure that we don't inadvertently delete more downloads from the
    // database than needed on shutdown, we should update the load state only if
    // the operation completed successfully.
    if (aReason == Ci.mozIStorageStatementCallback.REASON_FINISHED) {
      this._loadState = this.kLoadAll;
    }

    // Indicate to the views that the batch loading operation is complete, even
    // if the lookup failed or was canceled.  The only possible glitch happens
    // in case the database backend changes while loading data, when the views
    // would open and immediately close.  This case is rare enough not to need a
    // special treatment.
    this._views.forEach(
      function(view) view.onDataLoadCompleted()
    );
  },

  //////////////////////////////////////////////////////////////////////////////
  //// nsIObserver

  observe: function(aSubject, aTopic, aData)
  {
    switch (aTopic) {
      case "download-manager-remove-download-guid":
        // If a single download was removed, remove the corresponding data item.
        if (aSubject) {
            let downloadGuid = aSubject.QueryInterface(Ci.nsISupportsCString).data;
            DownloadsCommon.log("A single download with id",
                                downloadGuid, "was removed.");
          this._removeDataItem(downloadGuid);
          break;
        }

        // Multiple downloads have been removed.  Iterate over known downloads
        // and remove those that don't exist anymore.
        DownloadsCommon.log("Multiple downloads were removed.");
        for each (let dataItem in this.dataItems) {
          if (dataItem) {
            // Bug 449811 - We have to bind to the dataItem because Javascript
            // doesn't do fresh let-bindings per loop iteration.
            let dataItemBinding = dataItem;
            Services.downloads.getDownloadByGUID(dataItemBinding.downloadGuid,
                                                 function(aStatus, aResult) {
              if (aStatus == Components.results.NS_ERROR_NOT_AVAILABLE) {
                DownloadsCommon.log("Removing download with id",
                                    dataItemBinding.downloadGuid);
                this._removeDataItem(dataItemBinding.downloadGuid);
              }
            }.bind(this));
          }
        }
        break;
    }
  },

  //////////////////////////////////////////////////////////////////////////////
  //// nsIDownloadProgressListener

  onDownloadStateChange: function(aOldState, aDownload)
  {
    if (aDownload.isPrivate != this._isPrivate) {
      // Ignore the downloads with a privacy status other than what we are
      // tracking.
      return;
    }

    // When a new download is added, it may have the same identifier of a
    // download that we previously deleted during this session, and we also
    // want to provide a visible indication that the download started.
    let isNew = aOldState == nsIDM.DOWNLOAD_NOTSTARTED ||
                aOldState == nsIDM.DOWNLOAD_QUEUED;

    let dataItem = this._getOrAddDataItem(aDownload, isNew);
    if (!dataItem) {
      return;
    }

    let wasInProgress = dataItem.inProgress;

    DownloadsCommon.log("A download changed its state to:", aDownload.state);
    dataItem.state = aDownload.state;
    dataItem.referrer = aDownload.referrer && aDownload.referrer.spec;
    dataItem.resumable = aDownload.resumable;
    dataItem.startTime = Math.round(aDownload.startTime / 1000);
    dataItem.currBytes = aDownload.amountTransferred;
    dataItem.maxBytes = aDownload.size;

    if (wasInProgress && !dataItem.inProgress) {
      dataItem.endTime = Date.now();
    }

    // When a download is retried, we create a different download object from
    // the database with the same ID as before. This means that the nsIDownload
    // that the dataItem holds might now need updating.
    //
    // We only overwrite this in the event that _download exists, because if it
    // doesn't, that means that no caller ever tried to get the nsIDownload,
    // which means it was never retrieved and doesn't need to be overwritten.
    if (dataItem._download) {
      dataItem._download = aDownload;
    }

    for (let view of this._views) {
      try {
        view.getViewItem(dataItem).onStateChange(aOldState);
      } catch (ex) {
        Cu.reportError(ex);
      }
    }

    if (isNew && !dataItem.newDownloadNotified) {
      dataItem.newDownloadNotified = true;
      this._notifyDownloadEvent("start");
    }

    // This is a final state of which we are only notified once.
    if (dataItem.done) {
      this._notifyDownloadEvent("finish");
    }

    // TODO Bug 830415: this isn't the right place to set these annotation.
    // It should be set it in places' nsIDownloadHistory implementation.
    if (!this._isPrivate && !dataItem.inProgress) {
      let downloadMetaData = { state: dataItem.state,
                               endTime: dataItem.endTime };
      if (dataItem.done)
        downloadMetaData.fileSize = dataItem.maxBytes;

      try {
        PlacesUtils.annotations.setPageAnnotation(
          NetUtil.newURI(dataItem.uri), "downloads/metaData", JSON.stringify(downloadMetaData), 0,
          PlacesUtils.annotations.EXPIRE_WITH_HISTORY);
      }
      catch(ex) {
        Cu.reportError(ex);
      }
    }
  },

  onProgressChange: function(aWebProgress, aRequest,
                                                  aCurSelfProgress,
                                                  aMaxSelfProgress,
                                                  aCurTotalProgress,
                                                  aMaxTotalProgress, aDownload)
  {
    if (aDownload.isPrivate != this._isPrivate) {
      // Ignore the downloads with a privacy status other than what we are
      // tracking.
      return;
    }

    let dataItem = this._getOrAddDataItem(aDownload, false);
    if (!dataItem) {
      return;
    }

    dataItem.currBytes = aDownload.amountTransferred;
    dataItem.maxBytes = aDownload.size;
    dataItem.speed = aDownload.speed;
    dataItem.percentComplete = aDownload.percentComplete;

    this._views.forEach(
      function(view) view.getViewItem(dataItem).onProgressChange()
    );
  },

  onStateChange: function() { },

  onSecurityChange: function() { },

  //////////////////////////////////////////////////////////////////////////////
  //// Notifications sent to the most recent browser window only

  /**
   * Set to true after the first download causes the downloads panel to be
   * displayed.
   */
  get panelHasShownBefore() {
    try {
      return Services.prefs.getBoolPref("browser.download.panel.shown");
    } catch (ex) { }
    return false;
  },

  set panelHasShownBefore(aValue) {
    Services.prefs.setBoolPref("browser.download.panel.shown", aValue);
    return aValue;
  },

  /**
   * Displays a new or finished download notification in the most recent browser
   * window, if one is currently available with the required privacy type.
   *
   * @param aType
   *        Set to "start" for new downloads, "finish" for completed downloads.
   */
  _notifyDownloadEvent: function(aType)
  {
    DownloadsCommon.log("Attempting to notify that a new download has started or finished.");
    if (DownloadsCommon.useToolkitUI) {
      DownloadsCommon.log("Cancelling notification - we're using the toolkit downloads manager.");
      return;
    }

    // Show the panel in the most recent browser window, if present.
    let browserWin = RecentWindow.getMostRecentBrowserWindow({ private: this._isPrivate });
    if (!browserWin) {
      return;
    }

    if (this.panelHasShownBefore) {
      // For new downloads after the first one, don't show the panel
      // automatically, but provide a visible notification in the topmost
      // browser window, if the status indicator is already visible.
      DownloadsCommon.log("Showing new download notification.");
      browserWin.DownloadsIndicatorView.showEventNotification(aType);
      return;
    }
    this.panelHasShownBefore = true;
    browserWin.DownloadsPanel.showPanel();
  }
};

XPCOMUtils.defineLazyGetter(this, "PrivateDownloadsData", function() {
  return new DownloadsDataCtor(true);
});

XPCOMUtils.defineLazyGetter(this, "DownloadsData", function() {
  return new DownloadsDataCtor(false);
});

////////////////////////////////////////////////////////////////////////////////
//// DownloadsViewPrototype

/**
 * A prototype for an object that registers itself with DownloadsData as soon
 * as a view is registered with it.
 */
const DownloadsViewPrototype = {
  //////////////////////////////////////////////////////////////////////////////
  //// Registration of views

  /**
   * Array of view objects that should be notified when the available status
   * data changes.
   *
   * SUBCLASSES MUST OVERRIDE THIS PROPERTY.
   */
  _views: null,

  /**
   * Determines whether this view object is over the private or non-private
   * downloads.
   *
   * SUBCLASSES MUST OVERRIDE THIS PROPERTY.
   */
  _isPrivate: false,

  /**
   * Adds an object to be notified when the available status data changes.
   * The specified object is initialized with the currently available status.
   *
   * @param aView
   *        View object to be added.  This reference must be
   *        passed to removeView before termination.
   */
  addView: function(aView)
  {
    // Start receiving events when the first of our views is registered.
    if (this._views.length == 0) {
      if (this._isPrivate) {
        PrivateDownloadsData.addView(this);
      } else {
        DownloadsData.addView(this);
      }
    }

    this._views.push(aView);
    this.refreshView(aView);
  },

  /**
   * Updates the properties of an object previously added using addView.
   *
   * @param aView
   *        View object to be updated.
   */
  refreshView: function(aView)
  {
    // Update immediately even if we are still loading data asynchronously.
    // Subclasses must provide these two functions!
    this._refreshProperties();
    this._updateView(aView);
  },

  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        View object to be removed.
   */
  removeView: function(aView)
  {
    let index = this._views.indexOf(aView);
    if (index != -1) {
      this._views.splice(index, 1);
    }

    // Stop receiving events when the last of our views is unregistered.
    if (this._views.length == 0) {
      if (this._isPrivate) {
        PrivateDownloadsData.removeView(this);
      } else {
        DownloadsData.removeView(this);
      }
    }
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Callback functions from DownloadsData

  /**
   * Indicates whether we are still loading downloads data asynchronously.
   */
  _loading: false,

  /**
   * Called before multiple downloads are about to be loaded.
   */
  onDataLoadStarting: function()
  {
    this._loading = true;
  },

  /**
   * Called after data loading finished.
   */
  onDataLoadCompleted: function()
  {
    this._loading = false;
  },

  /**
   * Called when the downloads database becomes unavailable (for example, we
   * entered Private Browsing Mode and the database backend changed).
   * References to existing data should be discarded.
   *
   * @note Subclasses should override this.
   */
  onDataInvalidated: function()
  {
    throw Components.results.NS_ERROR_NOT_IMPLEMENTED;
  },

  /**
   * Called when a new download data item is available, either during the
   * asynchronous data load or when a new download is started.
   *
   * @param download
   *        Download object that was just added.
   * @param newest
   *        When true, indicates that this item is the most recent and should be
   *        added in the topmost position.  This happens when a new download is
   *        started.  When false, indicates that the item is the least recent
   *        with regard to the items that have been already added. The latter
   *        generally happens during the asynchronous data load.
   *
   * @note Subclasses should override this.
   */
  onDownloadAdded(download, newest) {
    throw Components.results.NS_ERROR_NOT_IMPLEMENTED;
  },

  /**
   * Called when the overall state of a Download has changed. In particular,
   * this is called only once when the download succeeds or is blocked
   * permanently, and is never called if only the current progress changed.
   *
   * The onDownloadChanged notification will always be sent afterwards.
   *
   * @note Subclasses should override this.
   */
  onDownloadStateChanged(download) {
    throw Components.results.NS_ERROR_NOT_IMPLEMENTED;
  },

  /**
   * Called every time any state property of a Download may have changed,
   * including progress properties.
   *
   * Note that progress notification changes are throttled at the Downloads.jsm
   * API level, and there is no throttling mechanism in the front-end.
   *
   * @note Subclasses should override this.
   */
  onDownloadChanged(download) {
    throw Components.results.NS_ERROR_NOT_IMPLEMENTED;
  },

  /**
   * Called when a data item is removed, ensures that the widget associated with
   * the view item is removed from the user interface.
   *
   * @param download
   *        Download object that is being removed.
   *
   * @note Subclasses should override this.
   */
  onDownloadRemoved(download) {
    throw Components.results.NS_ERROR_NOT_IMPLEMENTED;
  },

  /**
   * Private function used to refresh the internal properties being sent to
   * each registered view.
   *
   * @note Subclasses should override this.
   */
  _refreshProperties: function()
  {
    throw Components.results.NS_ERROR_NOT_IMPLEMENTED;
  },

  /**
   * Private function used to refresh an individual view.
   *
   * @note Subclasses should override this.
   */
  _updateView: function()
  {
    throw Components.results.NS_ERROR_NOT_IMPLEMENTED;
  }
};

////////////////////////////////////////////////////////////////////////////////
//// DownloadsIndicatorData

/**
 * This object registers itself with DownloadsData as a view, and transforms the
 * notifications it receives into overall status data, that is then broadcast to
 * the registered download status indicators.
 *
 * Note that using this object does not automatically start the Download Manager
 * service.  Consumers will see an empty list of downloads until the service is
 * actually started.  This is useful to display a neutral progress indicator in
 * the main browser window until the autostart timeout elapses.
 */
function DownloadsIndicatorDataCtor(aPrivate) {
  this._isPrivate = aPrivate;
  this._views = [];
}
DownloadsIndicatorDataCtor.prototype = {
  __proto__: DownloadsViewPrototype,

  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        DownloadsIndicatorView object to be removed.
   */
  removeView: function(aView)
  {
    DownloadsViewPrototype.removeView.call(this, aView);

    if (this._views.length == 0) {
      this._itemCount = 0;
    }
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Callback functions from DownloadsData

  onDataLoadCompleted: function()
  {
    DownloadsViewPrototype.onDataLoadCompleted.call(this);
    this._updateViews();
  },

  /**
   * Called when the downloads database becomes unavailable (for example, we
   * entered Private Browsing Mode and the database backend changed).
   * References to existing data should be discarded.
   */
  onDataInvalidated: function()
  {
    this._itemCount = 0;
  },

  onDownloadAdded(download, newest) {
    this._itemCount++;
    this._updateViews();
  },

  onDownloadStateChanged(download) {
    if (download.succeeded || download.error) {
      this.attention = true;
    }

    // Since the state of a download changed, reset the estimated time left.
    this._lastRawTimeLeft = -1;
    this._lastTimeLeft = -1;
  },

  onDownloadChanged(download) {
    this._updateViews();
  },

  onDownloadRemoved(download) {
    this._itemCount--;
    this._updateViews();
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Propagation of properties to our views

  // The following properties are updated by _refreshProperties and are then
  // propagated to the views.  See _refreshProperties for details.
  _hasDownloads: false,
  _counter: "",
  _percentComplete: -1,
  _paused: false,

  /**
   * Indicates whether the download indicators should be highlighted.
   */
  set attention(aValue)
  {
    this._attention = aValue;
    this._updateViews();
    return aValue;
  },
  _attention: false,

  /**
   * Indicates whether the user is interacting with downloads, thus the
   * attention indication should not be shown even if requested.
   */
  set attentionSuppressed(aValue)
  {
    this._attentionSuppressed = aValue;
    this._attention = false;
    this._updateViews();
    return aValue;
  },
  _attentionSuppressed: false,

  /**
   * Computes aggregate values and propagates the changes to our views.
   */
  _updateViews: function()
  {
    // Do not update the status indicators during batch loads of download items.
    if (this._loading) {
      return;
    }

    this._refreshProperties();
    this._views.forEach(this._updateView, this);
  },

  /**
   * Updates the specified view with the current aggregate values.
   *
   * @param aView
   *        DownloadsIndicatorView object to be updated.
   */
  _updateView: function(aView)
  {
    aView.hasDownloads = this._hasDownloads;
    aView.counter = this._counter;
    aView.percentComplete = this._percentComplete;
    aView.paused = this._paused;
    aView.attention = this._attention && !this._attentionSuppressed;
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Property updating based on current download status

  /**
   * Number of download items that are available to be displayed.
   */
  _itemCount: 0,

  /**
   * Floating point value indicating the last number of seconds estimated until
   * the longest download will finish.  We need to store this value so that we
   * don't continuously apply smoothing if the actual download state has not
   * changed.  This is set to -1 if the previous value is unknown.
   */
  _lastRawTimeLeft: -1,

  /**
   * Last number of seconds estimated until all in-progress downloads with a
   * known size and speed will finish.  This value is stored to allow smoothing
   * in case of small variations.  This is set to -1 if the previous value is
   * unknown.
   */
  _lastTimeLeft: -1,

  /**
   * A generator function for the Download objects this summary is currently
   * interested in. This generator is passed off to summarizeDownloads in order
   * to generate statistics about the downloads we care about - in this case,
   * it's all active downloads.
   */
  * _activeDownloads() {
    let downloads = this._isPrivate ? PrivateDownloadsData.downloads
                                    : DownloadsData.downloads;
    for (let download of downloads) {
      if (!download.stopped || (download.canceled && download.hasPartialData)) {
        yield download;
      }
    }
  },

  /**
   * Computes aggregate values based on the current state of downloads.
   */
  _refreshProperties: function()
  {
    let summary =
      DownloadsCommon.summarizeDownloads(this._activeDownloads());

    // Determine if the indicator should be shown or get attention.
    this._hasDownloads = (this._itemCount > 0);

    // If all downloads are paused, show the progress indicator as paused.
    this._paused = summary.numActive > 0 &&
                   summary.numActive == summary.numPaused;

    this._percentComplete = summary.percentComplete;

    // Display the estimated time left, if present.
    if (summary.rawTimeLeft == -1) {
      // There are no downloads with a known time left.
      this._lastRawTimeLeft = -1;
      this._lastTimeLeft = -1;
      this._counter = "";
    } else {
      // Compute the new time left only if state actually changed.
      if (this._lastRawTimeLeft != summary.rawTimeLeft) {
        this._lastRawTimeLeft = summary.rawTimeLeft;
        this._lastTimeLeft = DownloadsCommon.smoothSeconds(summary.rawTimeLeft,
                                                           this._lastTimeLeft);
      }
      this._counter = DownloadsCommon.formatTimeLeft(this._lastTimeLeft);
    }
  }
};

XPCOMUtils.defineLazyGetter(this, "PrivateDownloadsIndicatorData", function() {
  return new DownloadsIndicatorDataCtor(true);
});

XPCOMUtils.defineLazyGetter(this, "DownloadsIndicatorData", function() {
  return new DownloadsIndicatorDataCtor(false);
});

////////////////////////////////////////////////////////////////////////////////
//// DownloadsSummaryData

/**
 * DownloadsSummaryData is a view for DownloadsData that produces a summary
 * of all downloads after a certain exclusion point aNumToExclude. For example,
 * if there were 5 downloads in progress, and a DownloadsSummaryData was
 * constructed with aNumToExclude equal to 3, then that DownloadsSummaryData
 * would produce a summary of the last 2 downloads.
 *
 * @param aIsPrivate
 *        True if the browser window which owns the download button is a private
 *        window.
 * @param aNumToExclude
 *        The number of items to exclude from the summary, starting from the
 *        top of the list.
 */
function DownloadsSummaryData(aIsPrivate, aNumToExclude) {
  this._numToExclude = aNumToExclude;
  // Since we can have multiple instances of DownloadsSummaryData, we
  // override these values from the prototype so that each instance can be
  // completely separated from one another.
  this._loading = false;

  this._downloads = [];

  // Floating point value indicating the last number of seconds estimated until
  // the longest download will finish.  We need to store this value so that we
  // don't continuously apply smoothing if the actual download state has not
  // changed.  This is set to -1 if the previous value is unknown.
  this._lastRawTimeLeft = -1;

  // Last number of seconds estimated until all in-progress downloads with a
  // known size and speed will finish.  This value is stored to allow smoothing
  // in case of small variations.  This is set to -1 if the previous value is
  // unknown.
  this._lastTimeLeft = -1;

  // The following properties are updated by _refreshProperties and are then
  // propagated to the views.
  this._showingProgress = false;
  this._details = "";
  this._description = "";
  this._numActive = 0;
  this._percentComplete = -1;

  this._isPrivate = aIsPrivate;
  this._views = [];
}

DownloadsSummaryData.prototype = {
  __proto__: DownloadsViewPrototype,

  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        DownloadsSummary view to be removed.
   */
  removeView: function(aView)
  {
    DownloadsViewPrototype.removeView.call(this, aView);

    if (this._views.length == 0) {
      // Clear out our collection of Download objects. If we ever have
      // another view registered with us, this will get re-populated.
      this._downloads = [];
    }
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Callback functions from DownloadsData - see the documentation in
  //// DownloadsViewPrototype for more information on what these functions
  //// are used for.

  onDataLoadCompleted: function()
  {
    DownloadsViewPrototype.onDataLoadCompleted.call(this);
    this._updateViews();
  },

  onDataInvalidated: function()
  {
    this._dataItems = [];
  },

  onDownloadAdded(download, newest) {
    if (newest) {
      this._downloads.unshift(download);
    } else {
      this._downloads.push(download);
    }

    this._updateViews();
  },

  onDownloadStateChanged() {
    // Since the state of a download changed, reset the estimated time left.
    this._lastRawTimeLeft = -1;
    this._lastTimeLeft = -1;
  },

  onDownloadChanged() {
    this._updateViews();
  },

  onDownloadRemoved(download) {
    let itemIndex = this._downloads.indexOf(download);
    this._downloads.splice(itemIndex, 1);
    this._updateViews();
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Propagation of properties to our views

  /**
   * Computes aggregate values and propagates the changes to our views.
   */
  _updateViews: function()
  {
    // Do not update the status indicators during batch loads of download items.
    if (this._loading) {
      return;
    }

    this._refreshProperties();
    this._views.forEach(this._updateView, this);
  },

  /**
   * Updates the specified view with the current aggregate values.
   *
   * @param aView
   *        DownloadsIndicatorView object to be updated.
   */
  _updateView: function(aView)
  {
    aView.showingProgress = this._showingProgress;
    aView.percentComplete = this._percentComplete;
    aView.description = this._description;
    aView.details = this._details;
  },

  //////////////////////////////////////////////////////////////////////////////
  //// Property updating based on current download status

  /**
   * A generator function for the Download objects this summary is currently
   * interested in. This generator is passed off to summarizeDownloads in order
   * to generate statistics about the downloads we care about - in this case,
   * it's the downloads in this._downloads after the first few to exclude,
   * which was set when constructing this DownloadsSummaryData instance.
   */
  * _downloadsForSummary() {
    if (this._downloads.length > 0) {
      for (let i = this._numToExclude; i < this._downloads.length; ++i) {
        yield this._downloads[i];
      }
    }
  },

  /**
   * Computes aggregate values based on the current state of downloads.
   */
  _refreshProperties: function()
  {
    // Pre-load summary with default values.
    let summary =
      DownloadsCommon.summarizeDownloads(this._downloadsForSummary());

    this._description = DownloadsCommon.strings
                                       .otherDownloads2(summary.numActive);
    this._percentComplete = summary.percentComplete;

    // If all downloads are paused, show the progress indicator as paused.
    this._showingProgress = summary.numDownloading > 0 ||
                            summary.numPaused > 0;

    // Display the estimated time left, if present.
    if (summary.rawTimeLeft == -1) {
      // There are no downloads with a known time left.
      this._lastRawTimeLeft = -1;
      this._lastTimeLeft = -1;
      this._details = "";
    } else {
      // Compute the new time left only if state actually changed.
      if (this._lastRawTimeLeft != summary.rawTimeLeft) {
        this._lastRawTimeLeft = summary.rawTimeLeft;
        this._lastTimeLeft = DownloadsCommon.smoothSeconds(summary.rawTimeLeft,
                                                           this._lastTimeLeft);
      }
      [this._details] = DownloadUtils.getDownloadStatusNoRate(
        summary.totalTransferred, summary.totalSize, summary.slowestSpeed,
        this._lastTimeLeft);
    }
  }
}
