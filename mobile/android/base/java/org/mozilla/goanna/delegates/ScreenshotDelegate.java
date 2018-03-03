/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna.delegates;

import android.app.Activity;
import android.os.Bundle;
import android.support.design.widget.Snackbar;
import android.util.Log;

import org.mozilla.goanna.AppConstants;
import org.mozilla.goanna.BrowserApp;
import org.mozilla.goanna.GoannaProfile;
import org.mozilla.goanna.R;
import org.mozilla.goanna.ScreenshotObserver;
import org.mozilla.goanna.SnackbarBuilder;
import org.mozilla.goanna.Tab;
import org.mozilla.goanna.Tabs;
import org.mozilla.goanna.Telemetry;
import org.mozilla.goanna.TelemetryContract;
import org.mozilla.goanna.db.BrowserDB;

import java.lang.ref.WeakReference;

/**
 * Delegate for observing screenshots being taken.
 */
public class ScreenshotDelegate extends BrowserAppDelegateWithReference implements ScreenshotObserver.OnScreenshotListener {
    private static final String LOGTAG = "GoannaScreenshotDelegate";

    private final ScreenshotObserver mScreenshotObserver = new ScreenshotObserver();

    @Override
    public void onCreate(BrowserApp browserApp, Bundle savedInstanceState) {
        super.onCreate(browserApp, savedInstanceState);

        mScreenshotObserver.setListener(browserApp, this);
    }

    @Override
    public void onScreenshotTaken(String screenshotPath, String title) {
        // Treat screenshots as a sharing method.
        Telemetry.sendUIEvent(TelemetryContract.Event.SHARE, TelemetryContract.Method.BUTTON, "screenshot");

        if (!AppConstants.SCREENSHOTS_IN_BOOKMARKS_ENABLED) {
            return;
        }

        final Tab selectedTab = Tabs.getInstance().getSelectedTab();
        if (selectedTab == null) {
            Log.w(LOGTAG, "Selected tab is null: could not page info to store screenshot.");
            return;
        }

        final Activity activity = getBrowserApp();
        if (activity == null) {
            return;
        }

        BrowserDB.from(activity).getUrlAnnotations().insertScreenshot(
                activity.getContentResolver(), selectedTab.getURL(), screenshotPath);

        SnackbarBuilder.builder(activity)
                .message(R.string.screenshot_added_to_bookmarks)
                .duration(Snackbar.LENGTH_SHORT)
                .buildAndShow();
    }

    @Override
    public void onResume(BrowserApp browserApp) {
        mScreenshotObserver.start();
    }

    @Override
    public void onPause(BrowserApp browserApp) {
        mScreenshotObserver.stop();
    }
}
