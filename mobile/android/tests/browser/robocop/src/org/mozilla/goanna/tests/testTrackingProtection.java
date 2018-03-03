/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna.tests;

import static org.mozilla.goanna.tests.helpers.AssertionHelper.fFail;

import org.mozilla.goanna.EventDispatcher;
import org.mozilla.goanna.GoannaApp;
import org.mozilla.goanna.GoannaAppShell;
import org.mozilla.goanna.util.BundleEventListener;
import org.mozilla.goanna.util.EventCallback;
import org.mozilla.goanna.util.GoannaBundle;

public class testTrackingProtection extends JavascriptTest implements BundleEventListener {
    private String mLastTracking;

    public testTrackingProtection() {
        super("testTrackingProtection.js");
    }

    @Override // BundleEventListener
    public void handleMessage(final String event, final GoannaBundle message,
                              final EventCallback callback) {
        if ("Content:SecurityChange".equals(event)) {
            final GoannaBundle identity = message.getBundle("identity");
            final GoannaBundle mode = identity.getBundle("mode");
            mLastTracking = mode.getString("tracking");
            mAsserter.dumpLog("Security change (tracking): " + mLastTracking);

        } else if ("Test:Expected".equals(event)) {
            final String expected = message.getString("expected");
            mAsserter.is(mLastTracking, expected, "Tracking matched expectation");
            mAsserter.dumpLog("Testing (tracking): " + mLastTracking + " = " + expected);
        }
    }

    @Override
    public void setUp() throws Exception {
        super.setUp();

        EventDispatcher.getInstance().registerUiThreadListener(this,
                                                               "Content:SecurityChange",
                                                               "Test:Expected");
    }

    @Override
    public void tearDown() throws Exception {
        super.tearDown();

        EventDispatcher.getInstance().unregisterUiThreadListener(this,
                                                                 "Content:SecurityChange",
                                                                 "Test:Expected");
    }
}
