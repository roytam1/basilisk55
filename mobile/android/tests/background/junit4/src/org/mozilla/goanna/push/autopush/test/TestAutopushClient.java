/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

package org.mozilla.goanna.push.autopush.test;

import org.junit.Assert;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mozilla.goanna.background.testhelpers.TestRunner;
import org.mozilla.goanna.background.testhelpers.WaitHelper;
import org.mozilla.goanna.push.autopush.AutopushClient;
import org.mozilla.goanna.push.autopush.AutopushClientException;
import org.mozilla.goanna.sync.Utils;

@RunWith(TestRunner.class)
public class TestAutopushClient {
    @Test
    public void testGetSenderID() throws Exception {
        final AutopushClient client = new AutopushClient("https://updates-autopush-dev.stage.mozaws.net/v1/gcm/829133274407",
                Utils.newSynchronousExecutor());
        Assert.assertEquals("829133274407", client.getSenderIDFromServerURI());
    }

    @Test(expected=AutopushClientException.class)
    public void testGetNoSenderID() throws Exception {
        final AutopushClient client = new AutopushClient("https://updates-autopush-dev.stage.mozaws.net/v1/gcm",
                Utils.newSynchronousExecutor());
        client.getSenderIDFromServerURI();
    }
}
