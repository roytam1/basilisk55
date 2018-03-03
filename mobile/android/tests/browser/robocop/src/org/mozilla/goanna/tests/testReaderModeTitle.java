package org.mozilla.goanna.tests;

import org.mozilla.goanna.tests.helpers.GoannaHelper;
import org.mozilla.goanna.tests.helpers.NavigationHelper;

/**
 * This tests ensures that the toolbar in reader mode displays the original page url.
 */
public class testReaderModeTitle extends UITest {
    public void testReaderModeTitle() {
        GoannaHelper.blockForReady();

        NavigationHelper.enterAndLoadUrl(mStringHelper.ROBOCOP_READER_MODE_BASIC_ARTICLE);

        mToolbar.pressReaderModeButton();

        mToolbar.assertTitle(mStringHelper.ROBOCOP_READER_MODE_BASIC_ARTICLE);
    }
}
