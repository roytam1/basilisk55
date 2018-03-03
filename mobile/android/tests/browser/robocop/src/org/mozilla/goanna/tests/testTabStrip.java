package org.mozilla.goanna.tests;

import org.mozilla.goanna.Actions;
import org.mozilla.goanna.R;
import org.mozilla.goanna.tests.helpers.DeviceHelper;
import org.mozilla.goanna.tests.helpers.GoannaClickHelper;
import org.mozilla.goanna.tests.helpers.GoannaHelper;
import org.mozilla.goanna.tests.helpers.NavigationHelper;
import org.mozilla.goanna.tests.helpers.WaitHelper;

import static org.mozilla.goanna.tests.helpers.AssertionHelper.fAssertEquals;

public class testTabStrip extends UITest {
    public void testTabStrip() {
        if (!DeviceHelper.isTablet()) {
            return;
        }

        GoannaHelper.blockForReady();

        testOpenPrivateTabInNormalMode();
        // This test depends on ROBOCOP_BIG_LINK_URL being loaded in tab 0 from the first test.
        testNewNormalTabScroll();
    }

    /**
     * Make sure that a private tab opened while the tab strip is in normal mode does not get added
     * to the tab strip (bug 1339066).
    */
    private void testOpenPrivateTabInNormalMode() {
        final String normalModeUrl = mStringHelper.ROBOCOP_BIG_LINK_URL;
        NavigationHelper.enterAndLoadUrl(normalModeUrl);

        final Actions.EventExpecter titleExpecter = mActions.expectGlobalEvent(Actions.EventType.UI, "Content:DOMTitleChanged");
        GoannaClickHelper.openCentralizedLinkInNewPrivateTab();

        titleExpecter.blockForEvent();
        titleExpecter.unregisterListener();
        // In the passing version of this test the UI shouldn't change at all in response to the
        // new private tab, but to prevent a false positive when the private tab does get added to
        // the tab strip in error, sleep here to make sure the UI has time to update before we
        // check it.
        mSolo.sleep(250);

        // Now make sure there's still only one tab in the tab strip, and that it's still the normal
        // mode tab.

        mTabStrip.assertTabCount(1);
        mToolbar.assertTitle(normalModeUrl);
    }

    /**
     * Test that we do *not* scroll to a new normal tab opened from a link in a page (bug 1340929).
     * Assumes ROBOCOP_BIG_LINK_URL is loaded in tab 0.
     */
    private void testNewNormalTabScroll() {
        mTabStrip.fillStripWithTabs();
        mTabStrip.switchToTab(0);

        final int tabZeroId = mTabStrip.getTabViewAtVisualIndex(0).getTabId();

        final int tabCountBeforeNewTab = mTabStrip.getTabCount();
        GoannaClickHelper.openCentralizedLinkInNewTab();
        mTabStrip.waitForNewTab(tabCountBeforeNewTab);

        // If we scrolled to the new tab then the first tab visible on screen will no longer be the
        // tabs list tab 0.
        fAssertEquals("Current first tab is tabs list first tab", tabZeroId, mTabStrip.getTabViewAtVisualIndex(0).getTabId());
    }
 }
