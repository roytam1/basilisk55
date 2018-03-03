/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

package org.mozilla.goanna.icons.preparation;

import org.junit.Assert;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mozilla.goanna.background.testhelpers.TestRunner;
import org.mozilla.goanna.icons.IconDescriptor;
import org.mozilla.goanna.icons.IconRequest;
import org.mozilla.goanna.icons.Icons;
import org.robolectric.RuntimeEnvironment;

@RunWith(TestRunner.class)
public class TestFilterMimeTypes {
    private static final String TEST_PAGE_URL = "http://www.mozilla.org";
    private static final String TEST_ICON_URL = "https://example.org/favicon.ico";
    private static final String TEST_ICON_URL_2 = "https://mozilla.org/favicon.ico";

    @Test
    public void testUrlsWithoutMimeTypesAreNotFiltered() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createGenericIcon(TEST_ICON_URL))
                .build();

        Assert.assertEquals(1, request.getIconCount());

        final Preparer preparer = new FilterMimeTypes();
        preparer.prepare(request);

        Assert.assertEquals(1, request.getIconCount());
    }

    @Test
    public void testUnknownMimeTypesAreFiltered() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createFavicon(TEST_ICON_URL, 256, "image/zaphod"))
                .icon(IconDescriptor.createFavicon(TEST_ICON_URL_2, 128, "audio/mpeg"))
                .build();

        Assert.assertEquals(2, request.getIconCount());

        final Preparer preparer = new FilterMimeTypes();
        preparer.prepare(request);

        Assert.assertEquals(0, request.getIconCount());
    }

    @Test
    public void testKnownMimeTypesAreNotFiltered() {
        final IconRequest request = Icons.with(RuntimeEnvironment.application)
                .pageUrl(TEST_PAGE_URL)
                .icon(IconDescriptor.createFavicon(TEST_ICON_URL, 256, "image/x-icon"))
                .icon(IconDescriptor.createFavicon(TEST_ICON_URL_2, 128, "image/png"))
                .build();

        Assert.assertEquals(2, request.getIconCount());

        final Preparer preparer = new FilterMimeTypes();
        preparer.prepare(request);

        Assert.assertEquals(2, request.getIconCount());
    }
}
