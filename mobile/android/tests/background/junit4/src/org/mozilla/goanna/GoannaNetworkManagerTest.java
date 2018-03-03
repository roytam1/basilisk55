/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

package org.mozilla.goanna;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.mozilla.goanna.background.testhelpers.TestRunner;
import org.mozilla.goanna.GoannaNetworkManager.ManagerState;
import org.mozilla.goanna.GoannaNetworkManager.ManagerEvent;

import static org.junit.Assert.*;

@RunWith(TestRunner.class)
public class GoannaNetworkManagerTest {
    /**
     * Tests the transition matrix.
     */
    @Test
    public void testGetNextState() {
        ManagerState testingState;

        testingState = ManagerState.OffNoListeners;
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.disableNotifications));
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.stop));
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.receivedUpdate));
        assertEquals(ManagerState.OnNoListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.start));
        assertEquals(ManagerState.OffWithListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.enableNotifications));

        testingState = ManagerState.OnNoListeners;
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.start));
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.disableNotifications));
        assertEquals(ManagerState.OnWithListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.enableNotifications));
        assertEquals(ManagerState.OffNoListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.stop));
        assertEquals(ManagerState.OnNoListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.receivedUpdate));

        testingState = ManagerState.OnWithListeners;
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.start));
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.enableNotifications));
        assertEquals(ManagerState.OffWithListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.stop));
        assertEquals(ManagerState.OnNoListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.disableNotifications));
        assertEquals(ManagerState.OnWithListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.receivedUpdate));

        testingState = ManagerState.OffWithListeners;
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.stop));
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.enableNotifications));
        assertNull(GoannaNetworkManager.getNextState(testingState, ManagerEvent.receivedUpdate));
        assertEquals(ManagerState.OnWithListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.start));
        assertEquals(ManagerState.OffNoListeners, GoannaNetworkManager.getNextState(testingState, ManagerEvent.disableNotifications));
    }
}