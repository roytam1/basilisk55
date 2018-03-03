package org.mozilla.goanna.tests;

import org.mozilla.goanna.tests.helpers.DeviceHelper;
import org.mozilla.goanna.tests.helpers.GoannaClickHelper;
import org.mozilla.goanna.tests.helpers.GoannaHelper;
import org.mozilla.goanna.tests.helpers.NavigationHelper;
import org.mozilla.goanna.tests.helpers.WaitHelper;

/**
 * This test ensures the back/forward state is correct when switching to loading pages
 * to prevent regressions like Bug 1124190.
 */
public class testStateWhileLoading extends UITest {
    public void testStateWhileLoading() {
        if (!DeviceHelper.isTablet()) {
            // This test case only covers tablets currently.
            return;
        }

        GoannaHelper.blockForReady();

        NavigationHelper.enterAndLoadUrl(mStringHelper.ROBOCOP_LINK_TO_SLOW_LOADING);

        GoannaClickHelper.openCentralizedLinkInNewTab();

        WaitHelper.waitForPageLoad(new Runnable() {
            @Override
            public void run() {
                mTabStrip.switchToTab(1);

                // Assert that the state of the back button is correct
                // after switching to the new (still loading) tab.
                mToolbar.assertBackButtonIsNotEnabled();
            }
        });

        // Assert that the state of the back button is still correct after the page has loaded.
        mToolbar.assertBackButtonIsNotEnabled();
    }
}
